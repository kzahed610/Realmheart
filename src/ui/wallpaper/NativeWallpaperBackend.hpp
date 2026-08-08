#pragma once

#include "ui/wallpaper/WallpaperBackend.hpp"

#include <gio/gio.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace realmheart::ui::wallpaper {

class NativeWallpaperBackend final : public WallpaperBackend {
public:
    NativeWallpaperBackend() = default;
    ~NativeWallpaperBackend() override;

    [[nodiscard]] WallpaperBackendType type() const noexcept override {
        return WallpaperBackendType::Native;
    }

    [[nodiscard]] bool initialize(std::string* error_message = nullptr) override;
    [[nodiscard]] bool set_wallpaper(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    ) override;
    [[nodiscard]] bool prepare_wallpaper(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    ) override;
    [[nodiscard]] bool commit_prepared_wallpaper(
        std::string* error_message = nullptr
    ) override;
    void discard_prepared_wallpaper() noexcept override;

private:
    [[nodiscard]] bool initialize_locked(std::string* error_message = nullptr);
    [[nodiscard]] bool set_wallpaper_locked(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    );
    [[nodiscard]] bool prepare_wallpaper_locked(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    );
    [[nodiscard]] bool commit_prepared_wallpaper_locked(
        std::string* error_message = nullptr
    );
    [[nodiscard]] std::string find_renderer_executable() const;
    [[nodiscard]] bool send_line(
        const std::string& line,
        std::string* error_message = nullptr
    );
    [[nodiscard]] bool read_response(
        const char* expected_success,
        std::string* error_message = nullptr
    );
    void stop() noexcept;
    void stop_locked() noexcept;

    std::mutex operation_mutex_;
    GSubprocess* process_ = nullptr;
    GOutputStream* command_stream_ = nullptr;
    GDataInputStream* response_stream_ = nullptr;
    bool initialized_ = false;
};

} // namespace realmheart::ui::wallpaper
