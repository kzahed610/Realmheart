#pragma once

#include "ui/wallpaper/WallpaperBackend.hpp"

#include <gio/gio.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace realmheart::ui::wallpaper {

class NativeWallpaperBackend final
    : public WallpaperBackend,
      public std::enable_shared_from_this<NativeWallpaperBackend> {
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
    [[nodiscard]] bool prepare_wallpaper_for_output(
        const std::filesystem::path& path,
        const WallpaperOutputTarget& target,
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
    [[nodiscard]] bool prepare_wallpaper_for_output_locked(
        const std::filesystem::path& path,
        const WallpaperOutputTarget& target,
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
    void handle_process_exit(GSubprocess* source) noexcept;
    [[nodiscard]] bool replay_last_committed_locked(
        std::string* error_message = nullptr
    );
    void stop() noexcept;
    void stop_locked() noexcept;

    enum class ReplayKind {
        None,
        Global,
        Output,
    };

    std::mutex operation_mutex_;
    GSubprocess* process_ = nullptr;
    GOutputStream* command_stream_ = nullptr;
    GDataInputStream* response_stream_ = nullptr;
    bool initialized_ = false;
    bool shutting_down_ = false;
    bool recovering_ = false;
    unsigned recovery_attempts_ = 0;

    ReplayKind last_replay_kind_ = ReplayKind::None;
    std::filesystem::path last_committed_path_;
    std::string last_committed_output_connector_;
    ReplayKind prepared_replay_kind_ = ReplayKind::None;
    std::filesystem::path prepared_path_;
    std::string prepared_output_connector_;
};

} // namespace realmheart::ui::wallpaper
