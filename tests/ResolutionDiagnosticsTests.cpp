#include "ui/ResolutionDiagnostics.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "fixture\n";
}

void test_report_contains_geometry_and_resolved_tier_paths() {
    const auto root = fs::temp_directory_path() / "realmheart-resolution-diagnostics-test";
    std::error_code error;
    fs::remove_all(root, error);

    for (const std::string tier : {"1080p", "1440p", "4k"}) {
        touch(root / "workspace-overview" / "backgrounds" / tier / "fire-the-hearth.png");
        touch(root / "workspace-overview" / "characters" / tier / "bairon-wykes.png");
        touch(root / "characters" / "tessia" / tier / "manifest.json");
    }

    const auto previous = std::getenv("REALMHEART_ASSET_DIR");
    const std::string previous_value = previous != nullptr ? previous : "";
    const bool had_previous = previous != nullptr;
    ::setenv("REALMHEART_ASSET_DIR", root.c_str(), 1);

    const std::string report = realmheart::ui::resolution_compatibility_report();

    if (had_previous) ::setenv("REALMHEART_ASSET_DIR", previous_value.c_str(), 1);
    else ::unsetenv("REALMHEART_ASSET_DIR");
    fs::remove_all(root, error);

    require(report.find("3440x1440 scale=1 aspect=ultrawide layout=1440p assets=1440p") != std::string::npos,
            "report must document ultrawide density behavior");
    require(report.find("5120x1440 scale=1 aspect=super-ultrawide layout=1440p assets=1440p") != std::string::npos,
            "report must document super-ultrawide density behavior");
    require(report.find("1920x1080 scale=2 aspect=standard layout=1080p assets=4k") != std::string::npos,
            "report must document mixed-DPI layout/asset tier separation");
    require(report.find("2560x1440 scale=1.5 aspect=standard layout=1440p assets=4k") != std::string::npos,
            "report must document fractional mixed-DPI tier separation");
    require(report.find("1080p (1920x1080)") != std::string::npos,
            "report must include the 1080p contract");
    require(report.find("taskbar: rail=56 cap=20 visual=76") != std::string::npos,
            "report must preserve the released 1080p taskbar geometry");
    require(report.find("sidebar: frame=486 character-host=240 content=368 surface=726") != std::string::npos,
            "report must preserve the released 1080p sidebar composition");
    require(report.find("launcher: centre=648x200 search=360 node=88x74") != std::string::npos,
            "report must preserve the released 1080p launcher geometry");
    require(report.find("1440p (2560x1440)") != std::string::npos,
            "report must include the 1440p contract");
    require(report.find("launcher: centre=864x267 search=480 node=117x99") != std::string::npos,
            "report must expose the scaled 1440p launcher geometry");
    require(report.find("4k (3840x2160)") != std::string::npos,
            "report must include the 4K contract");
    require(report.find("launcher: centre=1296x400 search=720 node=176x148") != std::string::npos,
            "report must expose the scaled 4K launcher geometry");
    require(report.find("workspace-overview/backgrounds/1440p/fire-the-hearth.png -> ") != std::string::npos,
            "report must expose selected workspace asset provenance");
    require(report.find("characters/tessia/4k/manifest.json -> ") != std::string::npos,
            "report must expose selected Tessia package provenance");
    require(report.find("[missing]") == std::string::npos,
            "fixture assets must resolve through REALMHEART_ASSET_DIR");
}

} // namespace

int main() {
    test_report_contains_geometry_and_resolved_tier_paths();
    std::cout << "Resolution diagnostics tests passed\n";
    return 0;
}
