#include "screenshot/SmartRegionLoader.hpp"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace realmheart::screenshot {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kFrameMagic = 0x52485346u; // RHSF
constexpr std::uint32_t kFrameVersion = 1u;
constexpr int kWorkerFrameFd = 3;
constexpr auto kContentWorkerTimeout = std::chrono::seconds(4);
constexpr auto kWorkerWatchdogPoll = std::chrono::milliseconds(25);

bool cancellation_requested(const std::atomic_bool* cancel_requested) {
    return cancel_requested != nullptr &&
        cancel_requested->load(std::memory_order_acquire);
}

struct SharedFrameHeader {
    std::uint32_t magic = kFrameMagic;
    std::uint32_t version = kFrameVersion;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t stride = 0;
    std::uint32_t reserved = 0;
    std::uint64_t byte_size = 0;
};

struct ContentPixelRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct ContentWorkerResult {
    std::vector<ContentPixelRect> regions;
    std::string error;
};

std::optional<std::string> region_worker_command() {
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !self.empty()) {
        const auto sibling = self.parent_path() / "realmheart-screenshot-regions";
        if (::access(sibling.c_str(), X_OK) == 0) {
            return sibling.string();
        }
    }

    gchar* path = g_find_program_in_path("realmheart-screenshot-regions");
    if (path == nullptr) return std::nullopt;
    std::string resolved{path};
    g_free(path);
    return resolved;
}

bool write_all(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, bytes + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

int create_frame_memfd(const FrozenFrame& frame, std::string& error) {
    const int fd = static_cast<int>(::syscall(
        SYS_memfd_create,
        "realmheart-screenshot-frame",
        MFD_CLOEXEC
    ));
    if (fd < 0) {
        error = "memfd_create failed";
        return -1;
    }

    SharedFrameHeader header;
    header.width = frame.width;
    header.height = frame.height;
    header.stride = frame.stride;
    header.byte_size = static_cast<std::uint64_t>(frame.rgba.size());

    if (!write_all(fd, &header, sizeof(header)) ||
        !write_all(fd, frame.rgba.data(), frame.rgba.size())) {
        ::close(fd);
        error = "failed writing frozen frame to anonymous memory fd";
        return -1;
    }

    if (::lseek(fd, 0, SEEK_SET) < 0) {
        ::close(fd);
        error = "failed rewinding frozen frame memory fd";
        return -1;
    }
    return fd;
}

bool parse_double(std::string_view text, double& value) {
    try {
        std::size_t consumed = 0;
        value = std::stod(std::string{text}, &consumed);
        return consumed == text.size() && std::isfinite(value);
    } catch (...) {
        return false;
    }
}

ContentWorkerResult parse_worker_output(std::string_view output) {
    ContentWorkerResult result;
    std::size_t line_start = 0;
    while (line_start < output.size()) {
        std::size_t line_end = output.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = output.size();
        std::string_view line = output.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;

        std::array<std::string_view, 4> fields{};
        std::size_t field_index = 0;
        std::size_t start = 0;
        while (field_index < fields.size()) {
            const std::size_t tab = line.find('\t', start);
            if (tab == std::string_view::npos) {
                fields[field_index++] = line.substr(start);
                break;
            }
            fields[field_index++] = line.substr(start, tab - start);
            start = tab + 1;
        }
        if (field_index != fields.size()) continue;

        ContentPixelRect rect;
        if (!parse_double(fields[0], rect.x) ||
            !parse_double(fields[1], rect.y) ||
            !parse_double(fields[2], rect.width) ||
            !parse_double(fields[3], rect.height) ||
            rect.width <= 0.0 || rect.height <= 0.0) {
            continue;
        }
        result.regions.push_back(rect);
    }
    return result;
}

ContentWorkerResult run_content_worker(
    const FrozenFrame& frame,
    const std::atomic_bool* cancel_requested
) {
    ContentWorkerResult result;
    if (cancellation_requested(cancel_requested)) {
        result.error = "region worker cancelled";
        return result;
    }
    if (frame.width <= 0 || frame.height <= 0 || frame.stride <= 0 || frame.rgba.empty()) {
        result.error = "frozen frame is empty";
        return result;
    }

    const auto worker = region_worker_command();
    if (!worker.has_value()) {
        result.error = "content detector unavailable: realmheart-screenshot-regions not found";
        return result;
    }

    std::string memfd_error;
    int frame_fd = create_frame_memfd(frame, memfd_error);
    if (frame_fd < 0) {
        result.error = std::move(memfd_error);
        return result;
    }

    GSubprocessLauncher* launcher = g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(
            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_PIPE
        )
    );
    if (launcher == nullptr) {
        ::close(frame_fd);
        result.error = "unable to create region worker launcher";
        return result;
    }

    // Launcher takes ownership of frame_fd and remaps it to a stable descriptor
    // in the child. The frame never touches disk and never needs PNG encoding.
    g_subprocess_launcher_take_fd(launcher, frame_fd, kWorkerFrameFd);

    const std::string fd_arg = std::to_string(kWorkerFrameFd);
    GError* spawn_error = nullptr;
    GSubprocess* process = g_subprocess_launcher_spawn(
        launcher,
        &spawn_error,
        worker->c_str(),
        "--frame-fd",
        fd_arg.c_str(),
        nullptr
    );
    g_object_unref(launcher);

    if (process == nullptr) {
        result.error = spawn_error != nullptr
            ? std::string{"unable to start region worker: "} + spawn_error->message
            : "unable to start region worker";
        if (spawn_error != nullptr) g_error_free(spawn_error);
        return result;
    }

    gchar* stdout_text = nullptr;
    gchar* stderr_text = nullptr;
    GError* communicate_error = nullptr;
    GCancellable* cancellable = g_cancellable_new();
    std::atomic_bool watchdog_timeout{false};
    std::atomic_bool watchdog_external_cancel{false};

    std::jthread watchdog;
    try {
        watchdog = std::jthread([&](std::stop_token stop_token) {
            const auto deadline = Clock::now() + kContentWorkerTimeout;
            while (!stop_token.stop_requested()) {
                if (cancellation_requested(cancel_requested)) {
                    watchdog_external_cancel.store(true, std::memory_order_release);
                    g_cancellable_cancel(cancellable);
                    return;
                }
                if (Clock::now() >= deadline) {
                    watchdog_timeout.store(true, std::memory_order_release);
                    g_cancellable_cancel(cancellable);
                    return;
                }
                std::this_thread::sleep_for(kWorkerWatchdogPoll);
            }
        });
    } catch (const std::exception& exception) {
        g_subprocess_force_exit(process);
        g_subprocess_wait(process, nullptr, nullptr);
        g_object_unref(cancellable);
        g_object_unref(process);
        result.error = std::string{"unable to start region-worker watchdog: "} + exception.what();
        return result;
    }

    const gboolean communicated = g_subprocess_communicate_utf8(
        process,
        nullptr,
        cancellable,
        &stdout_text,
        &stderr_text,
        &communicate_error
    );
    watchdog.request_stop();
    watchdog.join();
    g_object_unref(cancellable);

    if (!communicated || !g_subprocess_get_successful(process)) {
        if (watchdog_external_cancel.load(std::memory_order_acquire)) {
            result.error = "region worker cancelled";
        } else if (watchdog_timeout.load(std::memory_order_acquire)) {
            result.error = "region worker timed out after 4 seconds";
        } else if (communicate_error != nullptr) {
            result.error = std::string{"region worker failed: "} + communicate_error->message;
        } else if (stderr_text != nullptr && *stderr_text != '\0') {
            result.error = std::string{"region worker failed: "} + stderr_text;
        } else {
            result.error = "region worker failed";
        }
        if (!communicated) {
            g_subprocess_force_exit(process);
            g_subprocess_wait(process, nullptr, nullptr);
        }
    } else if (stdout_text != nullptr) {
        result = parse_worker_output(stdout_text);
    }

    if (communicate_error != nullptr) g_error_free(communicate_error);
    g_free(stdout_text);
    g_free(stderr_text);
    g_object_unref(process);
    return result;
}

double intersection_area(const SelectionRect& a, const SelectionRect& b) {
    const double left = std::max(a.x, b.x);
    const double top = std::max(a.y, b.y);
    const double right = std::min(a.x + a.width, b.x + b.width);
    const double bottom = std::min(a.y + a.height, b.y + b.height);
    return std::max(0.0, right - left) * std::max(0.0, bottom - top);
}

bool reject_as_window_duplicate(
    const SemanticRegionSnapshot& snapshot,
    const SelectionRect& candidate
) {
    const double candidate_area = candidate.width * candidate.height;
    if (candidate_area <= std::numeric_limits<double>::epsilon()) return true;

    for (const auto& region : snapshot.regions) {
        if (region.source != SemanticRegionSource::Window) continue;
        const double inter = intersection_area(candidate, region.rect);
        if (inter <= 0.0) continue;

        const double window_area = region.rect.width * region.rect.height;
        if (window_area <= std::numeric_limits<double>::epsilon()) continue;

        const double candidate_inside_window = inter / candidate_area;
        const double window_covered_by_candidate = inter / window_area;
        if (candidate_inside_window >= 0.96 && window_covered_by_candidate >= 0.82) {
            return true;
        }
    }
    return false;
}

int source_rank(SemanticRegionSource source) {
    switch (source) {
        case SemanticRegionSource::Content:
            return 0;
        case SemanticRegionSource::Layer:
            return 1;
        case SemanticRegionSource::Window:
        default:
            return 2;
    }
}

bool clamp_region_to_monitor(
    SemanticRegion& region,
    double monitor_width,
    double monitor_height
) {
    if (!std::isfinite(monitor_width) || !std::isfinite(monitor_height) ||
        monitor_width <= 0.0 || monitor_height <= 0.0 ||
        !std::isfinite(region.rect.x) || !std::isfinite(region.rect.y) ||
        !std::isfinite(region.rect.width) || !std::isfinite(region.rect.height) ||
        region.rect.width <= 0.0 || region.rect.height <= 0.0) {
        return false;
    }

    const double left = std::clamp(region.rect.x, 0.0, monitor_width);
    const double top = std::clamp(region.rect.y, 0.0, monitor_height);
    const double right = std::clamp(
        region.rect.x + region.rect.width,
        0.0,
        monitor_width
    );
    const double bottom = std::clamp(
        region.rect.y + region.rect.height,
        0.0,
        monitor_height
    );
    if (right - left <= 1.0 || bottom - top <= 1.0) return false;

    region.rect = SelectionRect{
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };
    return true;
}

void sanitize_semantic_regions(SemanticRegionSnapshot& snapshot) {
    snapshot.regions.erase(
        std::remove_if(
            snapshot.regions.begin(),
            snapshot.regions.end(),
            [&](SemanticRegion& region) {
                return !clamp_region_to_monitor(
                    region,
                    snapshot.monitor_width,
                    snapshot.monitor_height
                );
            }
        ),
        snapshot.regions.end()
    );
    snapshot.available = snapshot.available && !snapshot.regions.empty();
}

void sort_regions(std::vector<SemanticRegion>& regions) {
    std::stable_sort(
        regions.begin(),
        regions.end(),
        [](const SemanticRegion& left, const SemanticRegion& right) {
            const int left_source = source_rank(left.source);
            const int right_source = source_rank(right.source);
            if (left_source != right_source) return left_source < right_source;
            if (left.priority != right.priority) return left.priority < right.priority;
            if (left.source == SemanticRegionSource::Window &&
                right.source == SemanticRegionSource::Window &&
                left.focus_history_id != right.focus_history_id) {
                return left.focus_history_id < right.focus_history_id;
            }
            const double left_area = left.rect.width * left.rect.height;
            const double right_area = right.rect.width * right.rect.height;
            if (std::abs(left_area - right_area) > 0.5) return left_area < right_area;
            return left.label < right.label;
        }
    );
}

void append_content_regions(
    SemanticRegionSnapshot& snapshot,
    const MonitorTarget& monitor,
    const FrozenFrame& frame,
    const std::vector<ContentPixelRect>& detected
) {
    if (snapshot.monitor_width <= 0.0 || snapshot.monitor_height <= 0.0) {
        snapshot.monitor_width = monitor.logical_width > 0.0
            ? monitor.logical_width
            : static_cast<double>(frame.width);
        snapshot.monitor_height = monitor.logical_height > 0.0
            ? monitor.logical_height
            : static_cast<double>(frame.height);
    }
    if (frame.width <= 0 || frame.height <= 0) return;

    const double scale_x = snapshot.monitor_width / static_cast<double>(frame.width);
    const double scale_y = snapshot.monitor_height / static_cast<double>(frame.height);

    bool appended_any = false;
    for (const auto& rect_px : detected) {
        SemanticRegion region{
            .rect = SelectionRect{
                .x = rect_px.x * scale_x,
                .y = rect_px.y * scale_y,
                .width = rect_px.width * scale_x,
                .height = rect_px.height * scale_y,
            },
            .source = SemanticRegionSource::Content,
            .label = "Region",
            .priority = 2,
            .focus_history_id = -1,
        };
        if (!clamp_region_to_monitor(
                region,
                snapshot.monitor_width,
                snapshot.monitor_height
            )) {
            continue;
        }
        if (reject_as_window_duplicate(snapshot, region.rect)) continue;
        snapshot.regions.push_back(std::move(region));
        appended_any = true;
    }

    if (appended_any) snapshot.available = true;
    sort_regions(snapshot.regions);
}

} // namespace

SmartRegionLoadResult SmartRegionLoader::load(
    const MonitorTarget& monitor,
    const FrozenFrame& frame,
    const std::atomic_bool* cancel_requested
) {
    SmartRegionLoadResult result;

    auto semantic_future = std::async(std::launch::async, [&monitor]() {
        const auto started = Clock::now();
        auto snapshot = SemanticRegionDetector::read(monitor);
        const double elapsed = std::chrono::duration<double, std::milli>(
            Clock::now() - started
        ).count();
        return std::pair<SemanticRegionSnapshot, double>{std::move(snapshot), elapsed};
    });

    auto content_future = std::async(std::launch::async, [&frame, cancel_requested]() {
        const auto started = Clock::now();
        auto content = run_content_worker(frame, cancel_requested);
        const double elapsed = std::chrono::duration<double, std::milli>(
            Clock::now() - started
        ).count();
        return std::pair<ContentWorkerResult, double>{std::move(content), elapsed};
    });

    auto [snapshot, semantic_ms] = semantic_future.get();
    auto [content, content_ms] = content_future.get();

    result.semantic_ms = semantic_ms;
    result.content_ms = content_ms;
    result.semantic_error = snapshot.error;
    result.content_error = content.error;

    if (snapshot.monitor_width <= 0.0 || snapshot.monitor_height <= 0.0) {
        snapshot.monitor_width = monitor.logical_width;
        snapshot.monitor_height = monitor.logical_height;
    }

    sanitize_semantic_regions(snapshot);
    append_content_regions(snapshot, monitor, frame, content.regions);
    result.snapshot = std::move(snapshot);
    return result;
}

} // namespace realmheart::screenshot
