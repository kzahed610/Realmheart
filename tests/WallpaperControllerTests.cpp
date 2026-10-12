#include "core/TaskExecutor.hpp"
#include "ui/wallpaper/WallpaperController.hpp"

#include <gtest/gtest.h>
#include <glib.h>

#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace realmheart::ui::wallpaper;

class FakeWallpaperBackend final : public WallpaperBackend {
public:
    explicit FakeWallpaperBackend(WallpaperBackendType backend_type)
        : backend_type(backend_type) {}

    WallpaperBackendType type() const noexcept override { return backend_type; }

    bool initialize(std::string* error_message) override {
        if (error_message != nullptr) error_message->clear();
        return initialize_success;
    }

    bool set_wallpaper(
        const WallpaperSource& source,
        std::string* error_message
    ) override {
        if (error_message != nullptr) error_message->clear();
        if (source.is_owned() && source.bytes() != nullptr) {
            observed_source_bytes = *source.bytes();
        }
        if (block_first_set && source.path().filename() == "first.png") {
            std::unique_lock lock(mutex);
            first_set_started = true;
            condition.notify_all();
            condition.wait(lock, [this] { return release_first_set; });
        }
        if (!set_success && error_message != nullptr) {
            *error_message = "fake set failure";
        }
        return set_success;
    }

    bool prepare_wallpaper(
        const WallpaperSource&,
        std::string* error_message
    ) override {
        if (error_message != nullptr) error_message->clear();
        prepared = prepare_success;
        if (!prepare_success && error_message != nullptr) {
            *error_message = "fake prepare failure";
        }
        return prepare_success;
    }

    bool prepare_wallpaper_for_output(
        const WallpaperSource&,
        const WallpaperOutputTarget& target,
        std::string* error_message
    ) override {
        if (error_message != nullptr) error_message->clear();
        prepared = prepare_success && target.valid();
        if (!prepared && error_message != nullptr) {
            *error_message = "fake output prepare failure";
        }
        return prepared;
    }

    bool commit_prepared_wallpaper(std::string* error_message) override {
        if (error_message != nullptr) error_message->clear();
        if (!prepared) {
            if (error_message != nullptr) *error_message = "nothing prepared";
            return false;
        }
        prepared = false;
        return commit_success;
    }

    void discard_prepared_wallpaper() noexcept override { prepared = false; }

    // Mirrors the real backends: only the native renderer decodes video.
    bool supports_live_wallpapers() const noexcept override {
        return backend_type == WallpaperBackendType::Native;
    }

    WallpaperBackendType backend_type;
    bool initialize_success = true;
    bool set_success = true;
    bool prepare_success = true;
    bool commit_success = true;
    bool prepared = false;
    bool block_first_set = false;
    bool first_set_started = false;
    bool release_first_set = false;
    std::string observed_source_bytes;
    std::mutex mutex;
    std::condition_variable condition;
};

void drain_main_context() {
    while (g_main_context_pending(nullptr)) {
        g_main_context_iteration(nullptr, false);
    }
}

template <typename Predicate>
bool wait_for_callback(Predicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        realmheart::core::shared_task_executor().wait_for_idle();
        drain_main_context();
        if (predicate()) return true;
        g_usleep(1000);
    }
    return predicate();
}

TEST(WallpaperControllerTest, PreparedOutputTargetClearsOnCommitAndDiscard) {
    auto backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Native);
    WallpaperController controller(
        nullptr,
        WallpaperBackendType::Native,
        [backend](GtkApplication*, WallpaperBackendType) { return backend; }
    );
    ASSERT_TRUE(controller.initialize());

    bool prepare_done = false;
    bool prepare_success = false;
    controller.prepare_wallpaper_for_output_async(
        "/wallpapers/output.png",
        WallpaperOutputTarget{1, "DP-1"},
        [&prepare_done, &prepare_success](bool success, std::string) {
            prepare_done = true;
            prepare_success = success;
        }
    );
    ASSERT_TRUE(wait_for_callback([&prepare_done] { return prepare_done; }));
    EXPECT_TRUE(prepare_success);
    EXPECT_TRUE(controller.has_prepared_wallpaper());
    EXPECT_TRUE(controller.has_prepared_output_target());
    EXPECT_TRUE(backend->prepared);

    bool commit_done = false;
    bool commit_success = false;
    controller.commit_prepared_wallpaper_async(
        [&commit_done, &commit_success](bool success, std::string) {
            commit_done = true;
            commit_success = success;
        }
    );
    ASSERT_TRUE(wait_for_callback([&commit_done] { return commit_done; }));
    EXPECT_TRUE(commit_success);
    EXPECT_FALSE(controller.has_prepared_wallpaper());
    EXPECT_FALSE(controller.has_prepared_output_target());
    EXPECT_FALSE(backend->prepared);

    controller.prepare_wallpaper_for_output_async(
        "/wallpapers/output.png",
        WallpaperOutputTarget{1, "DP-1"},
        [](bool, std::string) {}
    );
    ASSERT_TRUE(wait_for_callback([&controller] {
        return controller.has_prepared_wallpaper();
    }));
    controller.discard_prepared_wallpaper();
    EXPECT_FALSE(controller.has_prepared_wallpaper());
    EXPECT_FALSE(controller.has_prepared_output_target());
}

TEST(WallpaperControllerTest, RapidReplacementSuppressesStaleCallback) {
    auto backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Native);
    backend->block_first_set = true;
    WallpaperController controller(
        nullptr,
        WallpaperBackendType::Native,
        [backend](GtkApplication*, WallpaperBackendType) { return backend; }
    );
    ASSERT_TRUE(controller.initialize());

    int first_callbacks = 0;
    int second_callbacks = 0;
    controller.set_wallpaper_async(
        "/wallpapers/first.png",
        [&first_callbacks](bool, std::string) { ++first_callbacks; }
    );
    {
        std::unique_lock lock(backend->mutex);
        ASSERT_TRUE(backend->condition.wait_for(
            lock,
            std::chrono::seconds(2),
            [&backend] { return backend->first_set_started; }
        ));
    }
    controller.set_wallpaper_async(
        "/wallpapers/second.png",
        [&second_callbacks](bool success, std::string) {
            if (success) ++second_callbacks;
        }
    );
    {
        std::lock_guard lock(backend->mutex);
        backend->release_first_set = true;
    }
    backend->condition.notify_all();

    ASSERT_TRUE(wait_for_callback([&second_callbacks] {
        return second_callbacks == 1;
    }));
    EXPECT_EQ(first_callbacks, 0);
    EXPECT_EQ(second_callbacks, 1);
}

TEST(WallpaperControllerTest, OwnedSourceSurvivesAsyncWorkerLifetime) {
    auto backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Native);
    backend->block_first_set = true;
    WallpaperController controller(
        nullptr,
        WallpaperBackendType::Native,
        [backend](GtkApplication*, WallpaperBackendType) { return backend; }
    );
    ASSERT_TRUE(controller.initialize());

    int callbacks = 0;
    controller.set_wallpaper_async(
        WallpaperSource::owned_bytes("/project/first.png", "immutable-pixels"),
        [&callbacks](bool success, std::string) {
            if (success) ++callbacks;
        }
    );
    {
        std::unique_lock lock(backend->mutex);
        ASSERT_TRUE(backend->condition.wait_for(
            lock,
            std::chrono::seconds(2),
            [&backend] { return backend->first_set_started; }
        ));
    }
    {
        std::lock_guard lock(backend->mutex);
        backend->release_first_set = true;
    }
    backend->condition.notify_all();

    ASSERT_TRUE(wait_for_callback([&callbacks] { return callbacks == 1; }));
    EXPECT_EQ(backend->observed_source_bytes, "immutable-pixels");
}

TEST(WallpaperControllerTest, PrepareFailureClearsControllerTransactionState) {
    auto backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Native);
    WallpaperController controller(
        nullptr,
        WallpaperBackendType::Native,
        [backend](GtkApplication*, WallpaperBackendType) { return backend; }
    );
    ASSERT_TRUE(controller.initialize());
    backend->prepare_success = false;

    bool callback_called = false;
    bool callback_success = true;
    controller.prepare_wallpaper_async(
        WallpaperSource(std::filesystem::path("/wallpapers/broken.png")),
        [&callback_called, &callback_success](bool success, std::string) {
            callback_called = true;
            callback_success = success;
        }
    );

    ASSERT_TRUE(wait_for_callback([&callback_called] { return callback_called; }));
    EXPECT_FALSE(callback_success);
    EXPECT_FALSE(controller.has_prepared_wallpaper());
    EXPECT_FALSE(controller.has_prepared_output_target());
}

TEST(WallpaperControllerTest, NativeFailureFallsBackToGtkWithoutDroppingOwnedSource) {
    auto native_backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Native);
    native_backend->set_success = false;
    auto gtk_backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Gtk);
    WallpaperController controller(
        nullptr,
        WallpaperBackendType::Native,
        [native_backend, gtk_backend](GtkApplication*, WallpaperBackendType type) {
            return type == WallpaperBackendType::Native
                ? native_backend
                : gtk_backend;
        }
    );
    ASSERT_TRUE(controller.initialize());

    std::string error;
    const auto source = WallpaperSource::owned_bytes(
        "/project/fallback.png", "fallback-pixels"
    );
    ASSERT_TRUE(controller.set_wallpaper(source, &error)) << error;
    EXPECT_EQ(controller.active_backend(), WallpaperBackendType::Gtk);
    EXPECT_EQ(gtk_backend->observed_source_bytes, "fallback-pixels");

    // BUG-2: the fallback must be explainable after the fact, not just logged
    // from an async path the user never sees.
    const auto status = controller.backend_status();
    EXPECT_EQ(status.requested, WallpaperBackendType::Native);
    EXPECT_EQ(status.active, WallpaperBackendType::Gtk);
    EXPECT_TRUE(status.fell_back);
    EXPECT_FALSE(status.reason.empty());
    EXPECT_FALSE(status.live_wallpapers_playable);
}

// The callback is how the shell turns a silent fallback into something the user
// notices while it happens, rather than when their live wallpaper stops moving.
TEST(WallpaperControllerTest, AFallbackReportsItsStatusThroughTheCallback) {
    auto native_backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Native);
    native_backend->set_success = false;
    auto gtk_backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Gtk);
    WallpaperController controller(
        nullptr,
        WallpaperBackendType::Native,
        [native_backend, gtk_backend](GtkApplication*, WallpaperBackendType type) {
            return type == WallpaperBackendType::Native
                ? native_backend
                : gtk_backend;
        }
    );

    std::vector<WallpaperController::BackendStatus> reported;
    controller.set_backend_status_callback(
        [&reported](const WallpaperController::BackendStatus& status) {
            reported.push_back(status);
        }
    );
    ASSERT_TRUE(controller.initialize());

    std::string error;
    const auto source = WallpaperSource::owned_bytes(
        "/project/fallback.png", "fallback-pixels"
    );
    ASSERT_TRUE(controller.set_wallpaper(source, &error)) << error;

    ASSERT_EQ(reported.size(), 1U);
    EXPECT_TRUE(reported.front().fell_back);
    EXPECT_EQ(reported.front().active, WallpaperBackendType::Gtk);
    EXPECT_FALSE(reported.front().live_wallpapers_playable);
    EXPECT_FALSE(reported.front().reason.empty());
}

// A status query with no fallback at all must not invent one.
TEST(WallpaperControllerTest, AHealthyBackendReportsNoFallback) {
    auto backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Native);
    WallpaperController controller(
        nullptr,
        WallpaperBackendType::Native,
        [backend](GtkApplication*, WallpaperBackendType) { return backend; }
    );
    ASSERT_TRUE(controller.initialize());

    const auto status = controller.backend_status();
    EXPECT_EQ(status.requested, WallpaperBackendType::Native);
    EXPECT_EQ(status.active, WallpaperBackendType::Native);
    EXPECT_FALSE(status.fell_back);
    EXPECT_TRUE(status.reason.empty());
    EXPECT_TRUE(status.live_wallpapers_playable);
}

// Back on the native renderer, the fallback reason is gone: a stale reason
// would keep blaming a backend that is now working.
TEST(WallpaperControllerTest, ReturningToTheNativeBackendClearsTheFallbackReason) {
    auto native_backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Native);
    native_backend->set_success = false;
    auto gtk_backend = std::make_shared<FakeWallpaperBackend>(WallpaperBackendType::Gtk);
    WallpaperController controller(
        nullptr,
        WallpaperBackendType::Native,
        [native_backend, gtk_backend](GtkApplication*, WallpaperBackendType type) {
            return type == WallpaperBackendType::Native
                ? native_backend
                : gtk_backend;
        }
    );
    ASSERT_TRUE(controller.initialize());
    std::string error;
    const auto source = WallpaperSource::owned_bytes(
        "/project/fallback.png", "fallback-pixels"
    );
    ASSERT_TRUE(controller.set_wallpaper(source, &error)) << error;
    EXPECT_TRUE(controller.backend_status().fell_back);

    native_backend->set_success = true;
    gtk_backend->set_success = true;
    ASSERT_TRUE(controller.switch_backend(WallpaperBackendType::Native, &error)) << error;

    const auto status = controller.backend_status();
    EXPECT_EQ(status.active, WallpaperBackendType::Native);
    EXPECT_FALSE(status.fell_back);
    EXPECT_TRUE(status.reason.empty());
    EXPECT_TRUE(status.live_wallpapers_playable);
}

} // namespace
