#include "ui/powermenu/PowerMenuManifest.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using realmheart::ui::powermenu::PowerMenuManifest;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path() /
            ("realmheart-power-menu-manifest-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << contents;
    assert(output.good());
}

std::string valid_manifest(std::string_view second_file = "1x/arthur/hair.png") {
    return std::string{R"JSON({
  "version": 1,
  "assets": [
    {
      "name": "static-base",
      "category": "base",
      "files": {"1x": "1x/base/static-base.png"},
      "canvas": {"1x": [1200, 675]},
      "placement": {"1x": {"x": 0, "y": 0, "width": 1200, "height": 675}}
    },
    {
      "name": "arthur-hair-back",
      "category": "arthur",
      "files": {"1x": ")JSON"} + std::string(second_file) + R"JSON("},
      "canvas": {"1x": [1200, 675]},
      "placement": {"1x": {"x": 312, "y": -3, "width": 190, "height": 252}}
    }
  ]
})JSON";
}

void loads_valid_manifest_and_preserves_negative_placement() {
    TemporaryDirectory temporary;
    write_file(temporary.path() / "1x/base/static-base.png", "base");
    write_file(temporary.path() / "1x/arthur/hair.png", "hair");
    write_file(temporary.path() / "manifest.json", valid_manifest());

    std::string error;
    const auto manifest = PowerMenuManifest::load(
        temporary.path() / "manifest.json",
        &error
    );

    assert(manifest.has_value());
    assert(error.empty());
    assert(manifest->logical_canvas.width == 1200);
    assert(manifest->logical_canvas.height == 675);

    const auto* hair = manifest->find_asset("arthur-hair-back");
    assert(hair != nullptr);
    assert(hair->placement_1x.x == 312);
    assert(hair->placement_1x.y == -3);
    assert(hair->placement_1x.width == 190);
    assert(hair->placement_1x.height == 252);
}

void rejects_asset_path_escape() {
    TemporaryDirectory temporary;
    const auto outside = temporary.path().parent_path() / "realmheart-outside.png";
    write_file(temporary.path() / "1x/base/static-base.png", "base");
    write_file(outside, "outside");
    write_file(
        temporary.path() / "manifest.json",
        valid_manifest("../realmheart-outside.png")
    );

    std::string error;
    const auto manifest = PowerMenuManifest::load(
        temporary.path() / "manifest.json",
        &error
    );

    assert(!manifest.has_value());
    assert(!error.empty());

    std::error_code remove_error;
    std::filesystem::remove(outside, remove_error);
}

void rejects_duplicate_asset_names() {
    TemporaryDirectory temporary;
    write_file(temporary.path() / "1x/base/static-base.png", "base");
    write_file(temporary.path() / "1x/arthur/hair.png", "hair");

    std::string manifest_text = valid_manifest();
    const std::string original = "\"name\": \"arthur-hair-back\"";
    const auto position = manifest_text.find(original);
    assert(position != std::string::npos);
    manifest_text.replace(
        position,
        original.size(),
        "\"name\": \"static-base\""
    );
    write_file(temporary.path() / "manifest.json", manifest_text);

    std::string error;
    const auto manifest = PowerMenuManifest::load(
        temporary.path() / "manifest.json",
        &error
    );

    assert(!manifest.has_value());
    assert(!error.empty());
}

} // namespace

int main() {
    loads_valid_manifest_and_preserves_negative_placement();
    rejects_asset_path_escape();
    rejects_duplicate_asset_names();
    return 0;
}
