#include "core/DisplayTier.hpp"
#include "ui/workspace/WorkspaceOverviewAssets.hpp"

#include "nlohmann_json/json.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifndef REALMHEART_TEST_ASSET_ROOT
#define REALMHEART_TEST_ASSET_ROOT "assets"
#endif

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

struct TierCase {
    realmheart::core::DisplayTier tier;
    std::string_view directory;
};

constexpr std::array<TierCase, 3> kTiers{{
    {realmheart::core::DisplayTier::P1080, "1080p"},
    {realmheart::core::DisplayTier::P1440, "1440p"},
    {realmheart::core::DisplayTier::P4K, "4k"},
}};

constexpr std::array<std::string_view, 4> kBackgrounds{{
    "fire-the-hearth.png",
    "water-etistin-bay.png",
    "wind-elshire-forest.png",
    "earth-vildorial.png",
}};

constexpr std::array<std::string_view, 4> kCharacters{{
    "bairon-wykes.png",
    "varay-aurae.png",
    "aya-grephin.png",
    "mica-earthborn.png",
}};

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool same_bytes(const fs::path& left, const fs::path& right) {
    std::ifstream left_stream(left, std::ios::binary);
    std::ifstream right_stream(right, std::ios::binary);
    if (!left_stream || !right_stream) return false;

    constexpr std::size_t kChunkSize = 64 * 1024;
    std::array<char, kChunkSize> left_buffer{};
    std::array<char, kChunkSize> right_buffer{};
    while (left_stream && right_stream) {
        left_stream.read(left_buffer.data(), left_buffer.size());
        right_stream.read(right_buffer.data(), right_buffer.size());
        if (left_stream.gcount() != right_stream.gcount()) return false;
        if (!std::equal(
                left_buffer.begin(),
                left_buffer.begin() + left_stream.gcount(),
                right_buffer.begin()
            )) {
            return false;
        }
    }
    return left_stream.eof() && right_stream.eof();
}

Json read_json(const fs::path& path) {
    std::ifstream stream(path);
    require(static_cast<bool>(stream), "unable to open " + path.string());
    return Json::parse(stream);
}

void test_workspace_overview_assets_exist_for_every_tier(const fs::path& root) {
    for (const auto& tier : kTiers) {
        for (const auto filename : kBackgrounds) {
            const auto relative = realmheart::ui::workspace::workspace_overview_asset_path(
                "backgrounds", filename, tier.tier
            );
            require(
                fs::is_regular_file(root / relative),
                "missing workspace background: " + (root / relative).string()
            );
        }
        for (const auto filename : kCharacters) {
            const auto relative = realmheart::ui::workspace::workspace_overview_asset_path(
                "characters", filename, tier.tier
            );
            require(
                fs::is_regular_file(root / relative),
                "missing workspace character: " + (root / relative).string()
            );
        }
    }
}

void test_tessia_manifests_and_family_membership(const fs::path& root) {
    const auto tessia_root = root / "characters" / "tessia";
    const auto canonical_manifest = read_json(tessia_root / "1x" / "manifest.json");
    const auto canonical_assets = canonical_manifest.at("assets");

    for (const auto& tier : kTiers) {
        const auto tier_root = tessia_root / tier.directory;
        const auto manifest_path = tier_root / "manifest.json";
        const auto manifest = read_json(manifest_path);
        require(
            manifest.at("displayTier").get<std::string>() == tier.directory,
            "Tessia manifest tier metadata mismatch: " + manifest_path.string()
        );
        require(
            manifest.at("assets").size() == canonical_assets.size(),
            "Tessia asset family count mismatch: " + manifest_path.string()
        );
        for (const auto& [asset_id, asset] : manifest.at("assets").items()) {
            (void)asset_id;
            const auto file = asset.at("file").get<std::string>();
            require(
                fs::is_regular_file(tier_root / file),
                "missing Tessia asset: " + (tier_root / file).string()
            );
        }
    }
}

void test_1080p_package_is_byte_preserving(const fs::path& root) {
    const auto tessia_root = root / "characters" / "tessia";
    const auto canonical_assets = read_json(
        tessia_root / "1x" / "manifest.json"
    ).at("assets");
    for (const auto& [asset_id, asset] : canonical_assets.items()) {
        (void)asset_id;
        const auto file = asset.at("file").get<std::string>();
        require(
            same_bytes(
                tessia_root / "1x" / file,
                tessia_root / "1080p" / file
            ),
            "1080p Tessia asset is not byte-preserving: " + file
        );
    }
}

} // namespace

int main() {
    const fs::path root = REALMHEART_TEST_ASSET_ROOT;
    require(fs::is_directory(root), "asset root does not exist: " + root.string());
    test_workspace_overview_assets_exist_for_every_tier(root);
    test_tessia_manifests_and_family_membership(root);
    test_1080p_package_is_byte_preserving(root);
    std::cout << "Resolution asset package tests passed\n";
    return 0;
}
