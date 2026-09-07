#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace realmheart::services {

// A wallpaper source is either an intentionally external path (the persisted
// compatibility contract) or immutable bytes retained for the full lifetime
// of an asynchronous operation. Project assets must use the owned variant.
class WallpaperSource final {
public:
    WallpaperSource() = default;
    WallpaperSource(const WallpaperSource&) = default;
    WallpaperSource& operator=(const WallpaperSource&) = default;
    WallpaperSource(WallpaperSource&&) noexcept = default;
    WallpaperSource& operator=(WallpaperSource&&) noexcept = default;
    ~WallpaperSource() = default;

    // Deliberately implicit so legacy path-based transaction tests/callers keep
    // their external-path semantics while new project callers use owned_bytes().
    WallpaperSource(std::filesystem::path external_path)
        : path_(std::move(external_path)), kind_(Kind::ExternalPath) {}

    [[nodiscard]] static WallpaperSource owned_bytes(
        std::filesystem::path display_path,
        std::string bytes
    ) {
        WallpaperSource source;
        source.path_ = std::move(display_path);
        source.bytes_ = std::make_shared<const std::string>(std::move(bytes));
        source.kind_ = Kind::OwnedBytes;
        return source;
    }

    [[nodiscard]] bool empty() const noexcept {
        return path_.empty() && bytes_ == nullptr;
    }

    [[nodiscard]] bool is_owned() const noexcept {
        return kind_ == Kind::OwnedBytes && bytes_ != nullptr;
    }

    [[nodiscard]] bool is_external() const noexcept {
        return kind_ == Kind::ExternalPath && !path_.empty();
    }

    // This is metadata only for owned sources. Consumers must use bytes() when
    // is_owned() is true; it must never be reopened from this path.
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::optional<std::filesystem::path> external_path() const {
        if (!is_external()) return std::nullopt;
        return path_;
    }

    [[nodiscard]] const std::string* bytes() const noexcept {
        return is_owned() ? bytes_.get() : nullptr;
    }

    [[nodiscard]] std::shared_ptr<const std::string> owned_bytes_handle() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::string string() const {
        return path_.string();
    }

private:
    enum class Kind {
        Invalid,
        ExternalPath,
        OwnedBytes,
    };

    std::filesystem::path path_;
    std::shared_ptr<const std::string> bytes_;
    Kind kind_ = Kind::Invalid;
};

} // namespace realmheart::services
