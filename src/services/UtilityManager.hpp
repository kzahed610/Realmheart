#pragma once

#include "core/Command.hpp"
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <filesystem>

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
    explicit UtilityManager(std::unique_ptr<IUtilityExecutor> executor = std::make_unique<SystemUtilityExecutor>());

    // Screenshots
    bool take_screenshot_full(const std::string& path);
    bool take_screenshot_area(const std::string& path);
    bool extract_text_from_area();
    
    // Wallpaper & Theme
    bool set_wallpaper(const std::string& path);
    bool generate_colors(const std::string& path);
    
    // Screen Recording
    bool start_recording(const std::string& path);
    bool stop_recording();

    // Clipboard
    bool copy_to_clipboard(const std::string& text);
    std::string paste_from_clipboard();

    // Launcher
    bool launch_wofi();

private:
    std::unique_ptr<IUtilityExecutor> executor_;
};

} // namespace realmheart::services
