#include "services/ThemeService.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace realmheart::services {
namespace {

constexpr std::string_view kCacheHeader = "realmheart-theme-cache-v1";
constexpr std::size_t kMaxCacheBytes = 64 * 1024;
constexpr std::size_t kMaxCacheLineBytes = 320;
constexpr std::size_t kMaxCachePhysicalLines = 65; // header + 64 entries
constexpr std::size_t kMaxPaletteEntries = 64;
constexpr std::array<std::string_view, 4> kRequiredRoles{
    "primary", "background", "surface", "text"
};

std::filesystem::path default_cache_path() {
    if (const char* configured = std::getenv("REALMHEART_THEME_CACHE");
        configured != nullptr && *configured != '\0') {
        return configured;
    }
    if (const char* state_home = std::getenv("XDG_STATE_HOME");
        state_home != nullptr && *state_home != '\0') {
        return std::filesystem::path(state_home) / "realmheart/theme-palette.tsv";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) /
               ".local/state/realmheart/theme-palette.tsv";
    }
    return std::filesystem::temp_directory_path() /
           ("realmheart-" + std::to_string(static_cast<unsigned long>(::geteuid()))) /
           "theme-palette.tsv";
}

bool is_cache_token(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        return character == '\t' || character == '\n' || character == '\r' ||
               std::iscntrl(character) != 0;
    });
}

Palette default_palette() {
    Palette palette;
    palette.colors = {
        {"primary", "#cba6f7"},
        {"accent", "#cba6f7"},
        {"secondary", "#89b4fa"},
        {"tertiary", "#f5c2e7"},
        {"background", "#11111b"},
        {"surface", "#1e1e2e"},
        {"surface_variant", "#313244"},
        {"text", "#cdd6f4"},
        {"text_muted", "#a6adc8"},
        {"outline", "#45475a"},
        {"error", "#f38ba8"},
        {"red", "#f38ba8"},
        {"blue", "#89b4fa"}
    };
    return palette;
}

} // namespace

bool is_valid_palette_color(std::string_view value) {
    if (value.empty() || value.front() != '#') return false;
    const std::size_t digits = value.size() - 1;
    if (digits != 3 && digits != 4 && digits != 6 && digits != 8) return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

bool palette_is_valid(const Palette& palette) {
    if (palette.colors.size() > kMaxPaletteEntries) return false;
    for (const auto& [key, value] : palette.colors) {
        if (!is_cache_token(key) || !is_valid_palette_color(value)) return false;
    }
    return std::all_of(kRequiredRoles.begin(), kRequiredRoles.end(), [&](std::string_view role) {
        return palette.colors.contains(std::string(role));
    });
}

namespace {

enum class BoundedLineResult {
    Line,
    EndOfFile,
    Invalid
};

BoundedLineResult read_bounded_line(
    std::ifstream& input,
    std::string& line,
    std::size_t& total_bytes,
    std::size_t& physical_lines
) {
    line.clear();
    bool read_any = false;
    char character = '\0';
    while (input.get(character)) {
        read_any = true;
        if (++total_bytes > kMaxCacheBytes) return BoundedLineResult::Invalid;
        if (character == '\n') {
            if (++physical_lines > kMaxCachePhysicalLines) return BoundedLineResult::Invalid;
            return BoundedLineResult::Line;
        }
        if (line.size() >= kMaxCacheLineBytes) return BoundedLineResult::Invalid;
        line.push_back(character);
    }
    if (input.bad()) return BoundedLineResult::Invalid;
    if (!read_any) return BoundedLineResult::EndOfFile;
    if (++physical_lines > kMaxCachePhysicalLines) return BoundedLineResult::Invalid;
    return BoundedLineResult::Line;
}

std::optional<Palette> load_cached_palette(const std::filesystem::path& path) {
    try {
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size > kMaxCacheBytes) return std::nullopt;

    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;

    std::string line;
    line.reserve(kMaxCacheLineBytes);
    std::size_t total_bytes = 0;
    std::size_t physical_lines = 0;
    if (read_bounded_line(input, line, total_bytes, physical_lines) != BoundedLineResult::Line) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kCacheHeader) return std::nullopt;

    Palette palette;
    std::size_t entry_count = 0;
    while (true) {
        const auto result = read_bounded_line(input, line, total_bytes, physical_lines);
        if (result == BoundedLineResult::EndOfFile) break;
        if (result == BoundedLineResult::Invalid) return std::nullopt;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (++entry_count > kMaxPaletteEntries) return std::nullopt;

        const auto separator = line.find('\t');
        if (separator == std::string::npos || line.find('\t', separator + 1) != std::string::npos) {
            return std::nullopt;
        }

        std::string key = line.substr(0, separator);
        std::string value = line.substr(separator + 1);
        if (!is_cache_token(key) || !is_valid_palette_color(value) ||
            palette.colors.contains(key)) {
            return std::nullopt;
        }
        palette.colors.emplace(std::move(key), std::move(value));
    }

    if (!input.eof() || !palette_is_valid(palette)) return std::nullopt;
    return palette;
    } catch (const std::exception&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

bool write_all(int file_descriptor, std::string_view content) {
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t result = ::write(
            file_descriptor,
            content.data() + written,
            content.size() - written
        );
        if (result > 0) {
            written += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool persist_cached_palette(const std::filesystem::path& path, const Palette& palette) {
    if (!palette_is_valid(palette)) return false;

    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(palette.colors.size());
    for (const auto& [key, value] : palette.colors) {
        if (!is_cache_token(key) || !is_valid_palette_color(value)) return false;
        entries.emplace_back(key, value);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    std::error_code error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }

    std::string temporary_template = path.string() + ".tmp-XXXXXX";
    std::vector<char> temporary_buffer(temporary_template.begin(), temporary_template.end());
    temporary_buffer.push_back('\0');
    const int temporary_fd = ::mkstemp(temporary_buffer.data());
    if (temporary_fd < 0) return false;

    std::string contents;
    contents.reserve(64 + entries.size() * 32);
    contents += kCacheHeader;
    contents.push_back('\n');
    for (const auto& [key, value] : entries) {
        contents += key;
        contents.push_back('\t');
        contents += value;
        contents.push_back('\n');
    }

    bool succeeded = ::fchmod(temporary_fd, S_IRUSR | S_IWUSR) == 0;
    succeeded = succeeded && write_all(temporary_fd, contents);
    succeeded = succeeded && (::fsync(temporary_fd) == 0);
    if (::close(temporary_fd) != 0) succeeded = false;

    const auto temporary = std::filesystem::path(temporary_buffer.data());
    if (!succeeded) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return false;
    }

    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return false;
    }

    const auto parent = path.parent_path().empty()
        ? std::filesystem::path(".")
        : path.parent_path();
    const int directory_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) return false;
    const bool directory_synced = ::fsync(directory_fd) == 0;
    const bool directory_closed = ::close(directory_fd) == 0;
    return directory_synced && directory_closed;
}

} // namespace

ThemeService::Subscription::~Subscription() {
    reset();
}

ThemeService::Subscription::Subscription(Subscription&& other) noexcept
    : registry_(std::move(other.registry_)), id_(std::exchange(other.id_, 0)) {}

ThemeService::Subscription& ThemeService::Subscription::operator=(Subscription&& other) noexcept {
    if (this == &other) return *this;
    reset();
    registry_ = std::move(other.registry_);
    id_ = std::exchange(other.id_, 0);
    return *this;
}

void ThemeService::Subscription::reset() {
    if (id_ == 0) return;
    if (const auto registry = registry_.lock()) {
        std::lock_guard lock(registry->mutex);
        registry->callbacks.erase(id_);
    }
    registry_.reset();
    id_ = 0;
}

ThemeService::ThemeService(std::filesystem::path cache_path)
    : palette_(default_palette()),
      cache_path_(cache_path.empty() ? default_cache_path() : std::move(cache_path)),
      persistence_worker_(&ThemeService::persistence_loop, this) {

    if (auto cached = load_cached_palette(cache_path_)) {
        palette_ = std::move(*cached);
        std::cout << "[ThemeService] Restored cached palette. Primary="
                  << palette_.get("primary")
                  << " background=" << palette_.get("background") << '\n';
    } else {
        std::error_code error;
        if (std::filesystem::exists(cache_path_, error) && !error) {
            std::cerr << "[ThemeService] Ignoring invalid palette cache: "
                      << cache_path_ << '\n';
        }
    }
}

ThemeService::~ThemeService() {
    {
        std::lock_guard lock(persistence_mutex_);
        stopping_ = true;
    }
    persistence_cv_.notify_one();
    if (persistence_worker_.joinable()) persistence_worker_.join();
}

Palette ThemeService::get_palette() const {
    std::lock_guard lock(palette_mutex_);
    return palette_;
}

bool ThemeService::update_palette(Palette new_palette) {
    if (!palette_is_valid(new_palette)) {
        std::cerr << "[ThemeService] Rejecting invalid palette update\n";
        return false;
    }

    std::vector<ThemeChangedCallback> callbacks;
    Palette snapshot;

    {
        std::lock_guard lock(palette_mutex_);
        palette_ = std::move(new_palette);
        snapshot = palette_;
    }

    enqueue_persistence(snapshot);

    {
        std::lock_guard lock(subscribers_->mutex);
        callbacks.reserve(subscribers_->callbacks.size());
        for (const auto& [_, callback] : subscribers_->callbacks) {
            callbacks.push_back(callback);
        }
    }

    std::cout << "[ThemeService] Palette updated. Primary="
              << snapshot.get("primary")
              << " background=" << snapshot.get("background") << '\n';

    for (auto& callback : callbacks) {
        if (callback) callback(snapshot);
    }
    return true;
}

ThemeService::PersistenceStatus ThemeService::persistence_status() const {
    std::lock_guard lock(persistence_mutex_);
    return persistence_status_;
}

bool ThemeService::retry_persistence() {
    const Palette snapshot = get_palette();
    if (!palette_is_valid(snapshot)) return false;
    {
        std::lock_guard lock(persistence_mutex_);
        if (stopping_) return false;
        pending_persistence_ = PendingPersistence{
            snapshot,
            ++next_persistence_generation_
        };
        persistence_status_ = PersistenceStatus::Pending;
    }
    persistence_cv_.notify_one();
    return true;
}

void ThemeService::wait_for_persistence() {
    std::unique_lock lock(persistence_mutex_);
    persistence_idle_cv_.wait(lock, [this] {
        return !pending_persistence_ && !persistence_in_flight_;
    });
}

void ThemeService::ensure_safe_palette() {
    const Palette current = get_palette();
    if (!palette_is_valid(current)) {
        static_cast<void>(update_palette(default_palette()));
        return;
    }
    if (!current.colors.contains("error")) {
        Palette repaired = current;
        repaired.colors["error"] = "#f38ba8";
        repaired.colors["red"] = "#f38ba8";
        static_cast<void>(update_palette(std::move(repaired)));
    }
}

void ThemeService::enqueue_persistence(Palette palette) {
    {
        std::lock_guard lock(persistence_mutex_);
        if (stopping_) return;
        pending_persistence_ = PendingPersistence{
            std::move(palette),
            ++next_persistence_generation_
        };
        persistence_status_ = PersistenceStatus::Pending;
    }
    persistence_cv_.notify_one();
}

void ThemeService::persistence_loop() {
    while (true) {
        PendingPersistence request;
        {
            std::unique_lock lock(persistence_mutex_);
            persistence_cv_.wait(lock, [this] {
                return stopping_ || pending_persistence_.has_value();
            });
            if (stopping_ && !pending_persistence_) return;
            request = std::move(*pending_persistence_);
            pending_persistence_.reset();
            persistence_in_flight_ = true;
        }

        bool committed = false;
        try {
            committed = persist_cached_palette(cache_path_, request.palette);
        } catch (...) {
            committed = false;
        }

        {
            std::lock_guard lock(persistence_mutex_);
            persistence_in_flight_ = false;
            if (pending_persistence_) {
                persistence_status_ = PersistenceStatus::Pending;
            } else {
                persistence_status_ = committed
                    ? PersistenceStatus::Committed
                    : PersistenceStatus::Failed;
                if (!committed) {
                    std::cerr << "[ThemeService] Unable to persist palette cache: "
                              << cache_path_ << '\n';
                }
            }
            persistence_idle_cv_.notify_all();
        }
    }
}

ThemeService::Subscription ThemeService::subscribe(ThemeChangedCallback callback) {
    if (!callback) return {};

    std::lock_guard lock(subscribers_->mutex);
    const std::size_t id = subscribers_->next_id++;
    subscribers_->callbacks.emplace(id, std::move(callback));
    return Subscription{subscribers_, id};
}

} // namespace realmheart::services
