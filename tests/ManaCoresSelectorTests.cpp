// tests/ManaCoresSelectorTests.cpp
#include <gtest/gtest.h>
#include <array>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>
#define private public
#include "mana_core/ManaCoresSelector.hpp"
#undef private
#include "mana_core/ThumbnailCache.hpp"
#include "core/TaskExecutor.hpp"
#include <gdk/gdk.h>

namespace {

GdkPixbuf* make_sized_test_pixbuf(guint8 value, int width, int height) {
    auto* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
    if (pixbuf != nullptr) {
        const guint32 packed =
            (static_cast<guint32>(value) << 24) |
            (static_cast<guint32>(value) << 16) |
            (static_cast<guint32>(value) << 8) | 0xffU;
        gdk_pixbuf_fill(pixbuf, packed);
    }
    return pixbuf;
}

GdkPixbuf* make_test_pixbuf(guint8 value) {
    return make_sized_test_pixbuf(value, 4, 4);
}

void seed_preview_batch(realmheart::mana_core::ManaCoresSelector& selector) {
    selector.all_wallpaper_paths_ = {
        std::filesystem::path("core-a.png"),
        std::filesystem::path("core-b.png"),
        std::filesystem::path("core-c.png"),
        std::filesystem::path("core-d.png")
    };
    selector.wallpaper_decode_ready_.assign(4, true);
    selector.current_wallpaper_index_ = 0;

    auto* core = make_test_pixbuf(0x11);
    std::array<GdkPixbuf*, 3> slices = {
        make_test_pixbuf(0x21), make_test_pixbuf(0x31), make_test_pixbuf(0x41)
    };
    selector.set_current_wallpaper(core);
    selector.set_next_wallpapers(slices);
    g_object_unref(core);
    for (auto* slice : slices) g_object_unref(slice);
}

std::array<GdkPixbuf*, 4> make_target_batch() {
    return {
        make_test_pixbuf(0x12), make_test_pixbuf(0x22),
        make_test_pixbuf(0x32), make_test_pixbuf(0x42)
    };
}

} // namespace

TEST(ManaCoresSelector, Constructs) {
    realmheart::mana_core::ManaCoresSelector sel;
    EXPECT_FALSE(sel.is_visible());
}

TEST(ManaCoresSelector, DismissCallbackInvokedOnDismiss) {
    realmheart::mana_core::ManaCoresSelector sel;
    bool dismissed = false;
    sel.set_dismiss_callback([&dismissed]() {
        dismissed = true;
    });
    sel.dismiss();
    EXPECT_TRUE(dismissed);
    EXPECT_FALSE(sel.is_visible());
}

TEST(ManaCoresSelector, ApplyCallbackWiring) {
    realmheart::mana_core::ManaCoresSelector sel;
    std::string applied_path;
    sel.set_apply_callback([&applied_path](const std::string& path) {
        applied_path = path;
    });
    seed_preview_batch(sel);
    sel.visible_ = true;
    sel.state_ = realmheart::mana_core::ManaCoresSelector::State::Idle;
    sel.force_apply("core-b.png");
    ASSERT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Applying);
    sel.apply_start_micros_ = g_get_monotonic_time() - 600'000;
    realmheart::mana_core::ManaCoresSelector::tick_callback(nullptr, nullptr, &sel);
    EXPECT_EQ(applied_path, "core-b.png");
}

TEST(ManaCoresSelector, HandleKeyWhenHiddenReturnsFalse) {
    realmheart::mana_core::ManaCoresSelector sel;
    EXPECT_FALSE(sel.handle_key(GDK_KEY_Escape));
    EXPECT_FALSE(sel.handle_key(GDK_KEY_Return));
    EXPECT_FALSE(sel.handle_key(GDK_KEY_Left));
    EXPECT_FALSE(sel.handle_key(GDK_KEY_Right));
}

TEST(ManaCoresSelector, NavigationRequestRetainsVisibleBatchUntilDecodeReady) {
    realmheart::mana_core::ManaCoresSelector sel;
    seed_preview_batch(sel);
    auto* old_core = sel.current_core_pixbuf_;
    const auto old_slices = sel.slice_pixbufs_;

    sel.cycle_wallpaper(1);

    EXPECT_EQ(sel.current_core_pixbuf_, old_core);
    EXPECT_EQ(sel.slice_pixbufs_, old_slices);
    EXPECT_FALSE(sel.nav_transitioning_);
    EXPECT_EQ(sel.nav_transition_start_micros_, 0U);
}

TEST(ManaCoresSelector, MatchingCompleteBatchPromotesAtomicallyAndStartsOneTransition) {
    realmheart::mana_core::ManaCoresSelector sel;
    seed_preview_batch(sel);
    auto target = make_target_batch();
    const auto expected = target;
    sel.cycle_wallpaper(1);
    const auto generation = sel.async_state_->generation.load();

    sel.apply_preview_load(generation, 1, target);

    EXPECT_EQ(sel.current_core_pixbuf_, expected[0]);
    EXPECT_EQ(sel.slice_pixbufs_[0], expected[1]);
    EXPECT_EQ(sel.slice_pixbufs_[1], expected[2]);
    EXPECT_EQ(sel.slice_pixbufs_[2], expected[3]);
    EXPECT_TRUE(sel.nav_transitioning_);
    EXPECT_NE(sel.nav_transition_start_micros_, 0U);
    EXPECT_EQ(sel.nav_direction_, 1);
    EXPECT_EQ(target, (std::array<GdkPixbuf*, 4>{nullptr, nullptr, nullptr, nullptr}));
}

TEST(ManaCoresSelector, RapidNavigationPublishesOnlyLatestGeneration) {
    realmheart::mana_core::ManaCoresSelector sel;
    seed_preview_batch(sel);
    auto* old_core = sel.current_core_pixbuf_;
    const auto old_slices = sel.slice_pixbufs_;

    sel.cycle_wallpaper(1);
    const auto stale_generation = sel.async_state_->generation.load();
    sel.cycle_wallpaper(-1);
    const auto latest_generation = sel.async_state_->generation.load();
    EXPECT_EQ(sel.current_core_pixbuf_, old_core);
    EXPECT_EQ(sel.slice_pixbufs_, old_slices);

    auto stale = make_target_batch();
    sel.apply_preview_load(stale_generation, 1, stale);
    EXPECT_EQ(sel.current_core_pixbuf_, old_core);
    EXPECT_FALSE(sel.nav_transitioning_);
    for (auto* pixbuf : stale) g_object_unref(pixbuf);

    auto latest = make_target_batch();
    const auto expected = latest;
    sel.apply_preview_load(latest_generation, 0, latest);
    EXPECT_EQ(sel.current_core_pixbuf_, expected[0]);
    EXPECT_EQ(sel.slice_pixbufs_[0], expected[1]);
    EXPECT_EQ(sel.slice_pixbufs_[1], expected[2]);
    EXPECT_EQ(sel.slice_pixbufs_[2], expected[3]);
    EXPECT_EQ(sel.nav_direction_, -1);
    EXPECT_TRUE(sel.nav_transitioning_);
}

TEST(ManaCoresSelector, StaleOrFailedBatchRetainsVisibleBatchWithoutTransition) {
    realmheart::mana_core::ManaCoresSelector sel;
    seed_preview_batch(sel);
    auto* old_core = sel.current_core_pixbuf_;
    const auto old_slices = sel.slice_pixbufs_;
    sel.cycle_wallpaper(1);
    const auto generation = sel.async_state_->generation.load();

    auto stale = make_target_batch();
    sel.apply_preview_load(generation - 1, 1, stale);
    EXPECT_EQ(sel.current_core_pixbuf_, old_core);
    EXPECT_EQ(sel.slice_pixbufs_, old_slices);
    EXPECT_FALSE(sel.nav_transitioning_);
    for (auto* pixbuf : stale) g_object_unref(pixbuf);

    auto failed = make_target_batch();
    g_object_unref(failed[0]);
    failed[0] = nullptr;
    sel.apply_preview_load(generation, 1, failed);
    EXPECT_EQ(sel.current_core_pixbuf_, old_core);
    EXPECT_EQ(sel.slice_pixbufs_, old_slices);
    EXPECT_FALSE(sel.nav_transitioning_);
    if (failed[0] != nullptr && failed[0] != sel.current_core_pixbuf_) {
        g_object_unref(failed[0]);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (failed[i + 1] != nullptr && failed[i + 1] != sel.slice_pixbufs_[i]) {
            g_object_unref(failed[i + 1]);
        }
    }
}

TEST(ManaCoresSelector, InitialPublicationDoesNotAnimate) {
    realmheart::mana_core::ManaCoresSelector sel;
    sel.all_wallpaper_paths_ = {std::filesystem::path("core-a.png")};
    sel.wallpaper_decode_ready_.assign(1, false);
    sel.state_ = realmheart::mana_core::ManaCoresSelector::State::Assembling;
    sel.visible_ = true;
    sel.initial_preview_ready_ = false;
    sel.animation_start_micros_ = 0;
    EXPECT_EQ(
        realmheart::mana_core::ManaCoresSelector::tick_callback(nullptr, nullptr, &sel),
        G_SOURCE_CONTINUE
    );
    EXPECT_FALSE(sel.initial_preview_ready_);
    EXPECT_EQ(sel.animation_start_micros_, 0U);
    EXPECT_EQ(sel.current_wallpaper_alpha_, 0.0);

    auto initial = make_target_batch();
    const auto expected = initial;

    sel.apply_preview_load(sel.async_state_->generation.load(), 0, initial);

    EXPECT_TRUE(sel.initial_preview_ready_);
    EXPECT_EQ(sel.current_core_pixbuf_, expected[0]);
    EXPECT_FALSE(sel.nav_transitioning_);
    EXPECT_EQ(sel.nav_transition_start_micros_, 0U);
    EXPECT_EQ(initial, (std::array<GdkPixbuf*, 4>{nullptr, nullptr, nullptr, nullptr}));
}

TEST(ManaCoresSelector, FailedInitialPayloadReleasesRevealGate) {
    realmheart::mana_core::ManaCoresSelector sel;
    sel.all_wallpaper_paths_ = {std::filesystem::path("missing-core.png")};
    sel.wallpaper_decode_ready_.assign(1, false);
    sel.state_ = realmheart::mana_core::ManaCoresSelector::State::Assembling;
    sel.visible_ = true;
    sel.initial_preview_ready_ = false;

    std::array<GdkPixbuf*, 4> failed = {nullptr, nullptr, nullptr, nullptr};
    sel.apply_preview_load(sel.async_state_->generation.load(), 0, failed);

    EXPECT_TRUE(sel.initial_preview_ready_);
    EXPECT_EQ(sel.current_core_pixbuf_, nullptr);
    EXPECT_FALSE(sel.nav_transitioning_);
    EXPECT_EQ(sel.nav_transition_start_micros_, 0U);
}

TEST(ManaCoresSelector, AdjacentPrewarmDoesNotAlterVisibleBatch) {
    realmheart::mana_core::ManaCoresSelector sel;
    seed_preview_batch(sel);
    const auto old_core = sel.current_core_pixbuf_;
    const auto old_slices = sel.slice_pixbufs_;

    sel.schedule_adjacent_prewarm();
    realmheart::core::shared_task_executor().wait_for_idle();

    EXPECT_EQ(sel.current_core_pixbuf_, old_core);
    EXPECT_EQ(sel.slice_pixbufs_, old_slices);
    EXPECT_FALSE(sel.nav_transitioning_);
}

TEST(ThumbnailCache, RejectsUnsafeTargetDimensionsBeforeDecode) {
    const auto missing = std::filesystem::temp_directory_path() /
        "realmheart-thumbnail-cache-missing.png";
    std::string error;
    EXPECT_EQ(
        realmheart::mana_core::ThumbnailCache::load_or_create(missing, 5000, &error),
        nullptr
    );
    EXPECT_FALSE(error.empty());
}

TEST(ThumbnailCache, ReusesDecodedPreviewAndInvalidatesChangedSource) {
    const auto source = std::filesystem::temp_directory_path() /
        ("realmheart-thumbnail-cache-memory-" +
         std::to_string(static_cast<long long>(::getpid())) + ".png");
    auto save_source = [](const std::filesystem::path& path, GdkPixbuf* pixbuf) {
        GError* save_error = nullptr;
        const gboolean saved = gdk_pixbuf_save(
            pixbuf,
            path.c_str(),
            "png",
            &save_error,
            nullptr
        );
        if (save_error != nullptr) g_error_free(save_error);
        return saved == TRUE;
    };

    auto* initial_source = make_sized_test_pixbuf(0x51, 8, 8);
    ASSERT_NE(initial_source, nullptr);
    ASSERT_TRUE(save_source(source, initial_source));
    g_object_unref(initial_source);

    auto* first = realmheart::mana_core::ThumbnailCache::load_or_create(source, 64);
    ASSERT_NE(first, nullptr);
    auto* second = realmheart::mana_core::ThumbnailCache::load_or_create(source, 64);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first, second);

    auto* changed_source = make_sized_test_pixbuf(0x61, 16, 8);
    ASSERT_NE(changed_source, nullptr);
    ASSERT_TRUE(save_source(source, changed_source));
    g_object_unref(changed_source);

    auto* changed = realmheart::mana_core::ThumbnailCache::load_or_create(source, 64);
    ASSERT_NE(changed, nullptr);
    EXPECT_NE(changed, first);
    EXPECT_EQ(gdk_pixbuf_get_width(changed), 16);
    EXPECT_EQ(gdk_pixbuf_get_height(changed), 8);

    g_object_unref(first);
    g_object_unref(second);
    g_object_unref(changed);
    std::error_code remove_error;
    std::filesystem::remove(source, remove_error);
}

TEST(ThumbnailCache, UsesDecodedCacheBeforeSourceProbe) {
    const auto source = std::filesystem::temp_directory_path() /
        ("realmheart-thumbnail-cache-probe-order-" +
         std::to_string(static_cast<long long>(::getpid())) + ".png");
    auto save_source = [](const std::filesystem::path& path, GdkPixbuf* pixbuf) {
        GError* save_error = nullptr;
        const gboolean saved = gdk_pixbuf_save(
            pixbuf,
            path.c_str(),
            "png",
            &save_error,
            nullptr
        );
        if (save_error != nullptr) g_error_free(save_error);
        return saved == TRUE;
    };

    auto* initial_source = make_sized_test_pixbuf(0x65, 8, 8);
    ASSERT_NE(initial_source, nullptr);
    ASSERT_TRUE(save_source(source, initial_source));
    g_object_unref(initial_source);

    auto* first = realmheart::mana_core::ThumbnailCache::load_or_create(source, 64);
    ASSERT_NE(first, nullptr);

    std::error_code metadata_error;
    const auto original_permissions = std::filesystem::status(source, metadata_error).permissions();
    ASSERT_FALSE(metadata_error);
    std::filesystem::permissions(
        source,
        std::filesystem::perms::none,
        std::filesystem::perm_options::replace,
        metadata_error
    );
    ASSERT_FALSE(metadata_error);

    auto* second = realmheart::mana_core::ThumbnailCache::load_or_create(source, 64);
    const bool cache_hit = second != nullptr;

    std::filesystem::permissions(
        source,
        original_permissions,
        std::filesystem::perm_options::replace,
        metadata_error
    );
    ASSERT_FALSE(metadata_error);

    ASSERT_TRUE(cache_hit);
    EXPECT_EQ(second, first);
    EXPECT_EQ(gdk_pixbuf_get_width(second), 8);
    EXPECT_EQ(gdk_pixbuf_get_height(second), 8);

    g_object_unref(first);
    g_object_unref(second);
    std::error_code remove_error;
    std::filesystem::remove(source, remove_error);
}

TEST(ThumbnailCache, PreservesDecodedPreviewsAcrossTargetDimensions) {
    const auto source = std::filesystem::temp_directory_path() /
        ("realmheart-thumbnail-cache-memory-dimensions-" +
         std::to_string(static_cast<long long>(::getpid())) + "-" +
         std::to_string(static_cast<long long>(g_get_real_time())) + ".png");
    auto save_source = [](const std::filesystem::path& path, GdkPixbuf* pixbuf) {
        GError* save_error = nullptr;
        const gboolean saved = gdk_pixbuf_save(
            pixbuf,
            path.c_str(),
            "png",
            &save_error,
            nullptr
        );
        if (save_error != nullptr) g_error_free(save_error);
        return saved == TRUE;
    };

    auto* source_pixbuf = make_sized_test_pixbuf(0x59, 160, 80);
    ASSERT_NE(source_pixbuf, nullptr);
    ASSERT_TRUE(save_source(source, source_pixbuf));
    g_object_unref(source_pixbuf);

    auto* first = realmheart::mana_core::ThumbnailCache::load_or_create(source, 80);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(gdk_pixbuf_get_width(first), 80);
    EXPECT_EQ(gdk_pixbuf_get_height(first), 40);

    auto* second = realmheart::mana_core::ThumbnailCache::load_or_create(source, 40);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(gdk_pixbuf_get_width(second), 40);
    EXPECT_EQ(gdk_pixbuf_get_height(second), 20);

    auto* first_again = realmheart::mana_core::ThumbnailCache::load_or_create(source, 80);
    ASSERT_NE(first_again, nullptr);
    EXPECT_EQ(first_again, first);

    g_object_unref(first);
    g_object_unref(second);
    g_object_unref(first_again);
    std::error_code remove_error;
    std::filesystem::remove(source, remove_error);
}

TEST(ThumbnailCache, DoesNotRetainPreviewOverDecodedPixelBudget) {
    const auto source = std::filesystem::temp_directory_path() /
        ("realmheart-thumbnail-cache-memory-oversized-" +
         std::to_string(static_cast<long long>(::getpid())) + ".png");
    auto* source_pixbuf = make_sized_test_pixbuf(0x71, 2048, 2048);
    ASSERT_NE(source_pixbuf, nullptr);
    GError* save_error = nullptr;
    ASSERT_TRUE(gdk_pixbuf_save(source_pixbuf, source.c_str(), "png", &save_error, nullptr));
    if (save_error != nullptr) g_error_free(save_error);
    g_object_unref(source_pixbuf);

    auto* first = realmheart::mana_core::ThumbnailCache::load_or_create(source, 0);
    ASSERT_NE(first, nullptr);
    auto* second = realmheart::mana_core::ThumbnailCache::load_or_create(source, 0);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(gdk_pixbuf_get_width(first), 2048);
    EXPECT_EQ(gdk_pixbuf_get_height(first), 2048);

    g_object_unref(first);
    g_object_unref(second);
    std::error_code remove_error;
    std::filesystem::remove(source, remove_error);
}