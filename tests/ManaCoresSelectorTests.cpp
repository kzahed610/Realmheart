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

TEST(ManaCoresSelector, BeginApplyKeepsPreparedFullResolutionFrame) {
    realmheart::mana_core::ManaCoresSelector sel;
    seed_preview_batch(sel);
    auto* prepared = make_sized_test_pixbuf(0x55, 64, 32);
    ASSERT_NE(prepared, nullptr);
    sel.apply_fullscreen_pixbuf_ = prepared;

    sel.begin_apply();

    ASSERT_NE(sel.apply_fullscreen_pixbuf_, nullptr);
    EXPECT_EQ(gdk_pixbuf_get_width(sel.apply_fullscreen_pixbuf_), 64);
    EXPECT_EQ(gdk_pixbuf_get_height(sel.apply_fullscreen_pixbuf_), 32);
}

TEST(ManaCoresSelector, BeginApplyRefusesThumbnailWhenNoPreparedFrameExists) {
    realmheart::mana_core::ManaCoresSelector sel;
    seed_preview_batch(sel);
    sel.state_ = realmheart::mana_core::ManaCoresSelector::State::Idle;

    sel.begin_apply();

    EXPECT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Idle);
    EXPECT_EQ(sel.apply_fullscreen_pixbuf_, nullptr);
}

TEST(ManaCoresSelector, ApplyPreviewTargetDimensionMatchesDeviceScaleAndCoverCrop) {
    using Selector = realmheart::mana_core::ManaCoresSelector;
    EXPECT_EQ(Selector::apply_preview_target_dimension(3840, 2160, 2, 3840, 2160), 7680);
    EXPECT_EQ(Selector::apply_preview_target_dimension(1920, 1080, 1, 6000, 2500), 2592);
    EXPECT_EQ(Selector::apply_preview_target_dimension(1920, 1080, 1, 2500, 6000), 4608);
    EXPECT_EQ(Selector::apply_preview_target_dimension(16384, 2160, 2, 3840, 2160), 16384);
    EXPECT_EQ(Selector::apply_preview_target_dimension(3840, 2160, 0, 3840, 2160), 0);
    EXPECT_EQ(Selector::apply_preview_target_dimension(3840, 2160, 1, 0, 2160), 0);
}

TEST(ManaCoresSelector, RequestApplyWaitsForAsyncPreparationBeforeStartingAnimation) {
    realmheart::mana_core::ManaCoresSelector sel;
    seed_preview_batch(sel);
    const auto source_path = std::filesystem::temp_directory_path() /
        ("realmheart-mana-apply-" + std::to_string(getpid()) + "-" +
         std::to_string(g_get_monotonic_time()) + ".png");
    GError* save_error = nullptr;
    GdkPixbuf* source = make_sized_test_pixbuf(0x55, 96, 64);
    ASSERT_NE(source, nullptr);
    ASSERT_TRUE(gdk_pixbuf_save(source, source_path.c_str(), "png", &save_error, nullptr));
    g_clear_error(&save_error);
    g_object_unref(source);
    sel.all_wallpaper_paths_[0] = source_path;
    sel.layout_ = realmheart::mana_core::ManaCoresLayout::for_height(1080, 1920);
    sel.visible_ = true;
    sel.state_ = realmheart::mana_core::ManaCoresSelector::State::Idle;
    realmheart::mana_core::ApplyCompletion finish_preparation;
    sel.set_apply_prepare_callback([
        &finish_preparation
    ](const std::string&, realmheart::mana_core::ApplyCompletion completion) {
        finish_preparation = std::move(completion);
    });

    sel.request_apply();

    EXPECT_EQ(sel.apply_start_micros_, 0U);
    EXPECT_FALSE(sel.apply_callback_fired_);

    realmheart::core::shared_task_executor().wait_for_idle();
    while (g_main_context_iteration(nullptr, FALSE)) {}

    ASSERT_TRUE(static_cast<bool>(finish_preparation));
    EXPECT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::PreparingApply);
    EXPECT_TRUE(sel.apply_preview_ready_);
    EXPECT_FALSE(sel.apply_backend_ready_);
    EXPECT_EQ(sel.apply_start_micros_, 0U);

    finish_preparation(true, {});
    while (g_main_context_iteration(nullptr, FALSE)) {}
    ASSERT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Applying);
    ASSERT_NE(sel.apply_fullscreen_pixbuf_, nullptr);
    EXPECT_EQ(gdk_pixbuf_get_width(sel.apply_fullscreen_pixbuf_), 96);
    EXPECT_EQ(gdk_pixbuf_get_height(sel.apply_fullscreen_pixbuf_), 64);
    sel.dismiss();
    std::error_code remove_error;
    std::filesystem::remove(source_path, remove_error);
}

TEST(ManaCoresSelector, ApplyCallbackWaitsForPresentedOpaqueCoverFrame) {
    realmheart::mana_core::ManaCoresSelector sel;
    std::string applied_path;
    sel.set_apply_callback([&applied_path](
        const std::string& path,
        realmheart::mana_core::ApplyCompletion completion
    ) {
        applied_path = path;
        completion(true, {});
    });
    seed_preview_batch(sel);
    sel.layout_ = realmheart::mana_core::ManaCoresLayout::for_height(1080, 1920);
    sel.apply_output_geometry_ = sel.current_apply_output_geometry();
    sel.visible_ = true;
    sel.state_ = realmheart::mana_core::ManaCoresSelector::State::Idle;
    sel.current_wallpaper_index_ = 1;
    sel.apply_fullscreen_pixbuf_ = make_sized_test_pixbuf(0x55, 64, 32);
    sel.apply_fullscreen_opaque_ = true;
    sel.applying_wallpaper_path_ = "core-b.png";
    sel.begin_apply();
    ASSERT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Applying);
    constexpr guint64 animation_start = 1'000'000;
    sel.apply_start_micros_ = animation_start;
    sel.update_animations(animation_start + 600'000);

    EXPECT_TRUE(applied_path.empty());
    EXPECT_GT(sel.apply_mask_radius_, 0.0);

    sel.update_animations(animation_start + 650'000);
    EXPECT_TRUE(sel.apply_animation_finished_);
    EXPECT_DOUBLE_EQ(sel.apply_mask_radius_, 0.0);
    EXPECT_TRUE(applied_path.empty());

    sel.apply_cover_frame_counter_ = 42;
    sel.handle_apply_cover_presentation_feedback(41, true, 12'345);
    EXPECT_TRUE(applied_path.empty());
    sel.handle_apply_cover_presentation_feedback(42, false, 0);
    EXPECT_TRUE(applied_path.empty());
    sel.handle_apply_cover_presentation_feedback(42, true, 12'345);

    EXPECT_EQ(applied_path, "core-b.png");
    while (g_main_context_iteration(nullptr, FALSE)) {}
}

TEST(ManaCoresSelector, MissingPresentationTimestampAbortsApplyBeforeCommit) {
    realmheart::mana_core::ManaCoresSelector sel;
    bool aborted = false;
    std::string applied_path;
    sel.set_apply_abort_callback([&aborted] { aborted = true; });
    sel.set_apply_callback([&applied_path](
        const std::string& path,
        realmheart::mana_core::ApplyCompletion
    ) {
        applied_path = path;
    });
    seed_preview_batch(sel);
    sel.layout_ = realmheart::mana_core::ManaCoresLayout::for_height(1080, 1920);
    sel.apply_output_geometry_ = sel.current_apply_output_geometry();
    sel.visible_ = true;
    sel.state_ = realmheart::mana_core::ManaCoresSelector::State::Idle;
    sel.apply_fullscreen_pixbuf_ = make_sized_test_pixbuf(0x55, 64, 32);
    sel.apply_fullscreen_opaque_ = true;
    sel.begin_apply();
    sel.apply_start_micros_ = 2'000'000;
    sel.update_animations(2'650'000);
    sel.apply_cover_frame_counter_ = 9;

    sel.handle_apply_cover_presentation_feedback(9, true, 0);

    EXPECT_TRUE(aborted);
    EXPECT_TRUE(applied_path.empty());
    EXPECT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Idle);
}

TEST(ManaCoresSelector, OutputGeometryChangeAbortsBeforeCoverFrameIsRecorded) {
    realmheart::mana_core::ManaCoresSelector sel;
    bool aborted = false;
    sel.set_apply_abort_callback([&aborted] { aborted = true; });
    seed_preview_batch(sel);
    sel.layout_ = realmheart::mana_core::ManaCoresLayout::for_height(1080, 1920);
    sel.apply_output_geometry_ = sel.current_apply_output_geometry();
    sel.visible_ = true;
    sel.state_ = realmheart::mana_core::ManaCoresSelector::State::Idle;
    sel.apply_fullscreen_pixbuf_ = make_sized_test_pixbuf(0x55, 64, 32);
    sel.apply_fullscreen_opaque_ = true;
    sel.begin_apply();
    sel.apply_start_micros_ = 4'000'000;
    sel.update_animations(4'650'000);
    ASSERT_EQ(sel.apply_cover_frame_counter_, 0);

    sel.layout_.canvas_width += 1.0;
    sel.maybe_commit_after_cover_presented(nullptr);

    EXPECT_TRUE(aborted);
    EXPECT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Idle);
}

TEST(ManaCoresSelector, ApplyAnimationCrossfadesCommittedWallpaperBeforeDismissing) {
    realmheart::mana_core::ManaCoresSelector sel;
    seed_preview_batch(sel);
    sel.layout_ = realmheart::mana_core::ManaCoresLayout::for_height(1080, 1920);
    sel.apply_output_geometry_ = sel.current_apply_output_geometry();
    sel.visible_ = true;
    sel.state_ = realmheart::mana_core::ManaCoresSelector::State::Idle;
    sel.apply_fullscreen_pixbuf_ = make_sized_test_pixbuf(0x55, 64, 32);
    sel.apply_fullscreen_opaque_ = true;
    realmheart::mana_core::ApplyCompletion finish_commit;
    sel.set_apply_callback([
        &finish_commit
    ](const std::string&, realmheart::mana_core::ApplyCompletion completion) {
        finish_commit = std::move(completion);
    });

    sel.begin_apply();
    constexpr guint64 animation_start = 3'000'000;
    sel.apply_start_micros_ = animation_start;
    sel.update_animations(animation_start + 650'000);
    EXPECT_FALSE(static_cast<bool>(finish_commit));
    sel.apply_cover_frame_counter_ = 11;
    sel.handle_apply_cover_presentation_feedback(11, true, 12'345);

    ASSERT_TRUE(static_cast<bool>(finish_commit));
    EXPECT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Applying);
    EXPECT_TRUE(sel.visible_);
    EXPECT_NE(sel.apply_fullscreen_pixbuf_, nullptr);

    finish_commit(true, {});
    while (g_main_context_iteration(nullptr, FALSE)) {}
    EXPECT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Applying);
    EXPECT_TRUE(sel.visible_);
    EXPECT_NE(sel.apply_fullscreen_pixbuf_, nullptr);
    ASSERT_NE(sel.apply_handoff_start_micros_, 0U);
    EXPECT_DOUBLE_EQ(sel.apply_fullscreen_alpha_, 1.0);

    const guint64 handoff_start = sel.apply_handoff_start_micros_;
    sel.update_animations(
        handoff_start + realmheart::mana_core::ManaCoresSelector::kApplyHandoffDurationMicros / 2
    );
    EXPECT_TRUE(sel.visible_);
    EXPECT_NEAR(sel.apply_fullscreen_alpha_, 0.5, 0.01);

    sel.update_animations(
        handoff_start + realmheart::mana_core::ManaCoresSelector::kApplyHandoffDurationMicros
    );
    EXPECT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Applying);
    EXPECT_TRUE(sel.visible_);
    EXPECT_DOUBLE_EQ(sel.apply_fullscreen_alpha_, 0.0);

    sel.update_animations(
        handoff_start + realmheart::mana_core::ManaCoresSelector::kApplyHandoffDurationMicros +
            16'000
    );
    EXPECT_EQ(sel.state_, realmheart::mana_core::ManaCoresSelector::State::Hidden);
    EXPECT_FALSE(sel.visible_);
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
        realmheart::mana_core::ThumbnailCache::load_or_create(
            missing,
            realmheart::mana_core::ThumbnailCache::max_preview_dimension() + 1,
            &error
        ),
        nullptr
    );
    EXPECT_EQ(error, "thumbnail target dimension is out of bounds");
}

TEST(ThumbnailCache, SupportsDeviceResolutionPreviewsAbove4096Pixels) {
    const auto source = std::filesystem::temp_directory_path() /
        ("realmheart-thumbnail-cache-device-resolution-" +
         std::to_string(static_cast<long long>(::getpid())) + "-" +
         std::to_string(static_cast<long long>(g_get_real_time())) + ".png");
    GdkPixbuf* source_pixbuf = make_sized_test_pixbuf(0x71, 5000, 2);
    ASSERT_NE(source_pixbuf, nullptr);
    GError* save_error = nullptr;
    ASSERT_TRUE(gdk_pixbuf_save(
        source_pixbuf,
        source.c_str(),
        "png",
        &save_error,
        nullptr
    ));
    if (save_error != nullptr) g_error_free(save_error);
    g_object_unref(source_pixbuf);

    std::string error;
    GdkPixbuf* preview = realmheart::mana_core::ThumbnailCache::load_or_create(
        source,
        5000,
        &error
    );
    ASSERT_NE(preview, nullptr) << error;
    EXPECT_EQ(gdk_pixbuf_get_width(preview), 5000);
    EXPECT_EQ(gdk_pixbuf_get_height(preview), 2);

    g_object_unref(preview);
    std::error_code remove_error;
    std::filesystem::remove(source, remove_error);
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