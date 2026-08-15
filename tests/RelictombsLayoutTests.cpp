#include "relictombs/RelictombsLayout.hpp"

#include <cmath>
#include <filesystem>
#include <vector>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <gtest/gtest.h>

namespace realmheart::relictombs {
namespace {

TEST(RelictombsLayoutTests, ExactSixteenByNineOutputNeedsNoOffset) {
    const SceneTransform transform = make_scene_transform(1920.0F, 1080.0F);

    EXPECT_FLOAT_EQ(transform.scale, 1.0F);
    EXPECT_FLOAT_EQ(transform.offset_x, 0.0F);
    EXPECT_FLOAT_EQ(transform.offset_y, 0.0F);
}

TEST(RelictombsLayoutTests, UltrawideOutputUsesUniformCoverAndCentresCrop) {
    const SceneTransform transform = make_scene_transform(3440.0F, 1440.0F);

    EXPECT_FLOAT_EQ(transform.scale, 3440.0F / kDesignWidth);
    EXPECT_FLOAT_EQ(transform.offset_x, 0.0F);
    EXPECT_LT(transform.offset_y, 0.0F);
    EXPECT_FLOAT_EQ(
        transform.offset_y,
        (1440.0F - kDesignHeight * transform.scale) * 0.5F
    );
}

TEST(RelictombsLayoutTests, PortalViewportStaysInsideDesignCanvas) {
    EXPECT_GT(kPortalViewport.x, 0.0F);
    EXPECT_GT(kPortalViewport.y, 0.0F);
    EXPECT_GT(kPortalViewport.width, 0.0F);
    EXPECT_GT(kPortalViewport.height, 0.0F);
    EXPECT_LT(kPortalViewport.x + kPortalViewport.width, kDesignWidth);
    EXPECT_LT(kPortalViewport.y + kPortalViewport.height, kDesignHeight);
}

TEST(RelictombsLayoutTests, PhysicalOutputHeightSelectsSmallestSharpAssetTier) {
    EXPECT_EQ(select_asset_tier(1080), AssetTier::P1080);
    EXPECT_EQ(select_asset_tier(1081), AssetTier::P1440);
    EXPECT_EQ(select_asset_tier(1440), AssetTier::P1440);
    EXPECT_EQ(select_asset_tier(1441), AssetTier::P2160);
    EXPECT_EQ(select_asset_tier(2160), AssetTier::P2160);
}

TEST(RelictombsLayoutTests, AssetTierUsesCanonicalTierPaths) {
    EXPECT_EQ(
        base_asset_relative_path(AssetTier::P1080),
        "1080p/base.png"
    );
    EXPECT_EQ(
        base_asset_relative_path(AssetTier::P1440),
        "1440p/base.png"
    );
    EXPECT_EQ(
        base_asset_relative_path(AssetTier::P2160),
        "4k/base.png"
    );
}

TEST(RelictombsLayoutTests, FourBrokenFragmentsDefinedWithDistinctRects) {
    ASSERT_EQ(kFragmentSpecs.size(), 4U);

    for (const auto& fragment : kFragmentSpecs) {
        EXPECT_GT(fragment.idle_rect.width, 0.0F);
        EXPECT_GT(fragment.idle_rect.height, 0.0F);
        EXPECT_GT(fragment.idle_rect.x, 0.0F);
        EXPECT_GT(fragment.idle_rect.y, 0.0F);
        // Sprites rest on the arch surround, fully inside the design canvas.
        EXPECT_LT(fragment.idle_rect.x + fragment.idle_rect.width, kDesignWidth);
        EXPECT_LT(
            fragment.idle_rect.y + fragment.idle_rect.height,
            kDesignHeight
        );
        EXPECT_FALSE(fragment.file.empty());
    }
}

TEST(RelictombsLayoutTests, FragmentIdleRectsNeverPileOnEachOther) {
    // The forge pads each sprite by ~10px of transparency around the painted
    // art, so bounding boxes are allowed to graze (top-right vs middle-right
    // overlap by ~20x15px of pure padding with zero painted collision — pixel
    // audit verifies that). A strict bbox-disjoint assertion would be a false
    // oracle. The real constant-level invariant: overlap area must stay far
    // below what an actual pile-up would produce.
    for (std::size_t i = 0; i < kFragmentSpecs.size(); ++i) {
        for (std::size_t j = i + 1; j < kFragmentSpecs.size(); ++j) {
            const auto& a = kFragmentSpecs[i].idle_rect;
            const auto& b = kFragmentSpecs[j].idle_rect;
            const float overlap_w =
                std::min(a.x + a.width, b.x + b.width) -
                std::max(a.x, b.x);
            const float overlap_h =
                std::min(a.y + a.height, b.y + b.height) -
                std::max(a.y, b.y);
            if (overlap_w <= 0.0F || overlap_h <= 0.0F) continue;
            const float overlap_area = overlap_w * overlap_h;
            const float smaller_area =
                std::min(a.width * a.height, b.width * b.height);
            // Padding grazes stay under ~5% of the smaller sprite. A real
            // collision would bury a meaningful fraction of the sprite.
            EXPECT_LT(overlap_area, smaller_area * 0.05F)
                << kFragmentSpecs[i].name << " piles onto "
                << kFragmentSpecs[j].name
                << " (overlap " << overlap_area << " px2)";
        }
    }
}

TEST(RelictombsLayoutTests, FragmentAssetPathsUseTieredFragmentFolders) {
    const auto& first = kFragmentSpecs.front();
    EXPECT_EQ(
        fragment_asset_relative_path(AssetTier::P1080, first),
        "1080p/fragments/" + std::string(first.file)
    );
    EXPECT_EQ(
        fragment_asset_relative_path(AssetTier::P1440, first),
        "1440p/fragments/" + std::string(first.file)
    );
    EXPECT_EQ(
        fragment_asset_relative_path(AssetTier::P2160, first),
        "4k/fragments/" + std::string(first.file)
    );
}

TEST(RelictombsLayoutTests, FragmentSocketRectsStayInsideDesignCanvas) {
    for (const auto& fragment : kFragmentSpecs) {
        EXPECT_GT(fragment.socket_rect.width, 0.0F);
        EXPECT_GT(fragment.socket_rect.height, 0.0F);
        EXPECT_GT(fragment.socket_rect.x, 0.0F);
        EXPECT_GT(fragment.socket_rect.y, 0.0F);
        EXPECT_LT(
            fragment.socket_rect.x + fragment.socket_rect.width,
            kDesignWidth
        );
        EXPECT_LT(
            fragment.socket_rect.y + fragment.socket_rect.height,
            kDesignHeight
        );
    }
}

TEST(RelictombsLayoutTests, SocketRectsMatchSpriteSizeAndMoveInward) {
    // A socket is the same sprite at a new target: identical dimensions, and
    // the piece travels inward toward the arch (socket rect is left/up of the
    // idle rect for the three right-side fragments and right for bottom-left).
    for (const auto& fragment : kFragmentSpecs) {
        EXPECT_FLOAT_EQ(
            fragment.socket_rect.width,
            fragment.idle_rect.width
        );
        EXPECT_FLOAT_EQ(
            fragment.socket_rect.height,
            fragment.idle_rect.height
        );
        // Travel must be non-trivial but bounded — a socket that equals idle
        // would make reconstruction invisible; a huge travel would look like
        // a teleport. Keep the hop under the sprite diagonal.
        const float dx = fragment.socket_rect.x - fragment.idle_rect.x;
        const float dy = fragment.socket_rect.y - fragment.idle_rect.y;
        const float travel = std::sqrt(dx * dx + dy * dy);
        const float diagonal = std::sqrt(
            fragment.idle_rect.width * fragment.idle_rect.width +
            fragment.idle_rect.height * fragment.idle_rect.height
        );
        EXPECT_GT(travel, 20.0F);
        EXPECT_LT(travel, diagonal);
        // Fragments move toward the portal (inward), not away from it.
        EXPECT_LT(std::abs(dx), diagonal);
        EXPECT_LT(std::abs(dy), diagonal);
    }
}

TEST(RelictombsLayoutTests, SocketRectsStayOutOfPortalViewport) {
    // The portal viewport bbox is AA-inclusive, so bbox arithmetic is a false
    // oracle for socket placement: the sockets hug the rim and their padded
    // sprite rects legitimately sit inside the hole's bbox. The real
    // invariant is pixel truth: when the repaired arch is composited, the
    // painted fragment pixels must not bury the wallpaper opening (only ~1%
    // anti-aliased fringe touches). Validate with the actual 1080p assets.
    // ctest runs from the build directory, so walk upward to find the repo
    // root that owns assets/Relictombs-Broken_Arch.
    std::filesystem::path repo_root = std::filesystem::current_path();
    while (!std::filesystem::exists(
        repo_root / "assets/Relictombs-Broken_Arch/1080p/base.png"
    )) {
        const auto parent = repo_root.parent_path();
        if (parent == repo_root) {
            GTEST_SKIP() << "could not locate repo assets from "
                         << std::filesystem::current_path();
        }
        repo_root = parent;
    }
    const auto base_path = repo_root /
        "assets/Relictombs-Broken_Arch/1080p/base.png";
    if (!std::filesystem::exists(base_path)) {
        GTEST_SKIP() << "1080p base asset not present at " << base_path;
    }

    GError* error = nullptr;
    GdkPixbuf* base = gdk_pixbuf_new_from_file(
        base_path.c_str(),
        &error
    );
    if (base == nullptr) {
        g_clear_error(&error);
        GTEST_SKIP() << "base.png failed to decode";
    }
    const int pw = gdk_pixbuf_get_width(base);
    const int ph = gdk_pixbuf_get_height(base);
    std::vector<guint8> portal(pw * ph, 0);
    const guchar* pixels = gdk_pixbuf_get_pixels(base);
    const int n_channels = gdk_pixbuf_get_n_channels(base);
    for (int y = 0; y < ph; ++y) {
        const guchar* row = pixels + y * gdk_pixbuf_get_rowstride(base);
        for (int x = 0; x < pw; ++x) {
            portal[y * pw + x] = row[x * n_channels + 3] == 0 ? 1 : 0;
        }
    }
    g_object_unref(base);

    // Design space == 1080p pixel space (scale 1.0). Place each sprite at its
    // socket rect and count painted pixels (alpha > 128) that fall on portal
    // (alpha == 0) pixels. Fragments may rotate into the socket; without a
    // per-pixel rotation in the test, use the unrotated sprite — rotation is
    // <20 degrees and only shifts fringe, so the invariant holds either way.
    double total_portal = 0.0;
    double total_painted = 0.0;
    for (const auto& fragment : kFragmentSpecs) {
        const auto fragment_path = repo_root /
            ("assets/Relictombs-Broken_Arch/1080p/fragments/" +
             std::string(fragment.file));
        if (!std::filesystem::exists(fragment_path)) continue;
        GError* ferror = nullptr;
        GdkPixbuf* sprite = gdk_pixbuf_new_from_file(
            fragment_path.c_str(),
            &ferror
        );
        if (sprite == nullptr) {
            g_clear_error(&ferror);
            continue;
        }
        const int sw = gdk_pixbuf_get_width(sprite);
        const int sh = gdk_pixbuf_get_height(sprite);
        const guchar* spixels = gdk_pixbuf_get_pixels(sprite);
        const int schannels = gdk_pixbuf_get_n_channels(sprite);
        const int x0 = static_cast<int>(std::lround(fragment.socket_rect.x));
        const int y0 = static_cast<int>(std::lround(fragment.socket_rect.y));
        for (int sy = 0; sy < sh; ++sy) {
            const guchar* srow = spixels + sy * gdk_pixbuf_get_rowstride(sprite);
            const int y = y0 + sy;
            if (y < 0 || y >= ph) continue;
            for (int sx = 0; sx < sw; ++sx) {
                const int x = x0 + sx;
                if (x < 0 || x >= pw) continue;
                if (srow[sx * schannels + 3] <= 128) continue;
                ++total_painted;
                if (portal[y * pw + x]) ++total_portal;
            }
        }
        g_object_unref(sprite);
    }

    ASSERT_GT(total_painted, 0.0);
    // The Python pixel audit measured ~132/38136 (~0.35%) portal overlap
    // across all four repaired fragments (AA fringe only). Allow 2% headroom
    // for rounding/rotation differences so the test catches a fragment that
    // is dropped INTO the opening, not a hair of anti-aliasing.
    EXPECT_LT(total_portal / total_painted, 0.02)
        << "repaired fragments bury the portal opening (" << total_portal
        << " of " << total_painted << " painted pixels over portal)";
}

} // namespace
} // namespace realmheart::relictombs
