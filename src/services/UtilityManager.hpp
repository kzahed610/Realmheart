#pragma once

#include "core/Command.hpp"
#include "services/WallpaperService.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace realmheart::services {

class IUtilityExecutor {
public:
    virtual ~IUtilityExecutor() = default;
    virtual bool run_background(const std::vector<std::string>& argv) = 0;
    virtual realmheart::core::CommandResult run_capture(const std::vector<std::string>& argv) = 0;
};

class SystemUtilityExecutor : public IUtilityExecutor {
public:
    bool run_background(const std::vector<std::string>& argv) override {
        return realmheart::core::run_background(argv);
    }
    realmheart::core::CommandResult run_capture(const std::vector<std::string>& argv) override {
        return realmheart::core::run_capture(argv);
    }
};

class UtilityManager {
public:
    explicit UtilityManager(
        std::unique_ptr<IUtilityExecutor> executor = std::make_unique<SystemUtilityExecutor>(),
        std::filesystem::path recorder_pid_path = {},
        std::filesystem::path proc_root = "/proc"
    );
    ~UtilityManager() = default;

    bool take_screenshot_full(const std::string& path);
    bool take_screenshot_area(const std::string& path);
    bool take_screenshot_area_to_clipboard();
    bool extract_text_from_area();

    bool set_wallpaper(const std::string& path);
    bool choose_wallpaper();
    bool generate_colors(const std::string& path);
    std::string load_wallpaper_path();

    bool start_recording(const std::string& path);
    bool stop_recording();

    bool copy_to_clipboard(const std::string& text);
    std::string paste_from_clipboard();

    bool launch_wofi();
    services::WallpaperService* get_wallpaper_service() { return wallpaper_service_.get(); }

private:
    std::unique_ptr<IUtilityExecutor> executor_;
    std::filesystem::path recorder_pid_path_;
    std::filesystem::path proc_root_;
    std::unique_ptr<realmheart::services::WallpaperService> wallpaper_service_ = std::make_unique<realmheart::services::WallpaperService>();

    bool recorder_identity_matches(int pid, const std::string& expected_start_time) const;
};

} // namespace realmheart::services
