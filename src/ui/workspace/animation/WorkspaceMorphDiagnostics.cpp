#include "ui/workspace/animation/WorkspaceMorphDiagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

namespace realmheart::ui::workspace::animation {
namespace {

[[nodiscard]] bool env_truthy(const char* value) noexcept {
    if (value == nullptr || *value == '\0') return false;
    const std::string_view text(value);
    return text != "0" && text != "false" && text != "FALSE" &&
        text != "off" && text != "OFF";
}

} // namespace

WorkspaceMorphDiagnostics::WorkspaceMorphDiagnostics(bool enabled) noexcept
    : enabled_(enabled) {}

void WorkspaceMorphDiagnostics::set_enabled(bool enabled) noexcept {
    enabled_ = enabled;
    if (!enabled_) {
        active_ = false;
        report_ = {};
    }
}

void WorkspaceMorphDiagnostics::begin(
    bool opening,
    double now_seconds,
    long rss_kib
) noexcept {
    if (!enabled_) return;
    active_ = true;
    opening_ = opening;
    start_seconds_ = std::isfinite(now_seconds) ? now_seconds : 0.0;
    report_ = {};
    report_.rss_start_kib = rss_kib;
}

void WorkspaceMorphDiagnostics::set_direction(bool opening) noexcept {
    if (!enabled_ || !active_ || opening_ == opening) return;
    opening_ = opening;
    ++report_.reversal_count;
}

void WorkspaceMorphDiagnostics::record_frame(double elapsed_seconds) noexcept {
    if (!enabled_ || !active_ || !std::isfinite(elapsed_seconds) ||
        elapsed_seconds < 0.0) {
        return;
    }
    ++report_.frame_count;
    report_.worst_frame_ms = std::max(
        report_.worst_frame_ms,
        elapsed_seconds * 1000.0
    );
}

void WorkspaceMorphDiagnostics::note_shader_started(
    std::size_t transient_bytes
) noexcept {
    if (!enabled_ || !active_) return;
    report_.shader_started = true;
    note_transient_bytes(transient_bytes);
}

void WorkspaceMorphDiagnostics::note_shader_ready(
    std::size_t transient_bytes
) noexcept {
    if (!enabled_ || !active_) return;
    report_.shader_ready = true;
    note_transient_bytes(transient_bytes);
}

void WorkspaceMorphDiagnostics::note_shader_failed() noexcept {
    if (!enabled_ || !active_) return;
    report_.shader_failed = true;
}

void WorkspaceMorphDiagnostics::note_transient_bytes(
    std::size_t transient_bytes
) noexcept {
    if (!enabled_ || !active_) return;
    report_.peak_transient_bytes = std::max(
        report_.peak_transient_bytes,
        transient_bytes
    );
}

bool WorkspaceMorphDiagnostics::enabled() const noexcept {
    return enabled_;
}

bool WorkspaceMorphDiagnostics::active() const noexcept {
    return enabled_ && active_;
}

WorkspaceMorphDiagnosticReport WorkspaceMorphDiagnostics::finish(
    bool ended_visible,
    double now_seconds,
    long rss_kib,
    std::size_t retained_transient_bytes
) noexcept {
    if (!enabled_ || !active_) return {};

    report_.ended_visible = ended_visible;
    const double safe_now = std::isfinite(now_seconds)
        ? now_seconds
        : start_seconds_;
    report_.elapsed_ms = std::max(0.0, safe_now - start_seconds_) * 1000.0;
    report_.rss_end_kib = rss_kib;
    report_.retained_transient_bytes = retained_transient_bytes;

    const auto finished = report_;
    active_ = false;
    report_ = {};
    return finished;
}

bool workspace_morph_diagnostics_enabled() noexcept {
    return env_truthy(std::getenv("REALMHEART_WORKSPACE_MORPH_DIAGNOSTICS"));
}

long parse_workspace_morph_rss_kib(std::string_view status) noexcept {
    constexpr std::string_view key = "VmRSS:";
    const std::size_t key_position = status.find(key);
    if (key_position == std::string_view::npos) return -1;

    std::size_t position = key_position + key.size();
    while (position < status.size() &&
           (status[position] == ' ' || status[position] == '\t')) {
        ++position;
    }
    if (position >= status.size() || status[position] < '0' ||
        status[position] > '9') {
        return -1;
    }

    long value = 0;
    while (position < status.size() && status[position] >= '0' &&
           status[position] <= '9') {
        const int digit = status[position] - '0';
        if (value > (std::numeric_limits<long>::max() - digit) / 10L) return -1;
        value = value * 10L + digit;
        ++position;
    }
    return value;
}

long workspace_morph_process_rss_kib() noexcept {
    std::ifstream status("/proc/self/status");
    if (!status) return -1;
    std::ostringstream contents;
    contents << status.rdbuf();
    return parse_workspace_morph_rss_kib(contents.str());
}

std::string format_workspace_morph_diagnostics(
    const WorkspaceMorphDiagnosticReport& report
) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2)
           << "endpoint=" << (report.ended_visible ? "visible" : "hidden")
           << " elapsed_ms=" << report.elapsed_ms
           << " frames=" << report.frame_count
           << " worst_frame_ms=" << report.worst_frame_ms
           << " reversals=" << report.reversal_count
           << " shader=";
    if (report.shader_failed) {
        output << "fallback";
    } else if (report.shader_ready) {
        output << "ready";
    } else if (report.shader_started) {
        output << "started";
    } else {
        output << "unused";
    }
    output << " peak_transition_kib="
           << report.peak_transient_bytes / 1024U
           << " retained_transition_bytes="
           << report.retained_transient_bytes;
    if (report.rss_start_kib >= 0 && report.rss_end_kib >= 0) {
        output << " rss_delta_kib="
               << (report.rss_end_kib - report.rss_start_kib);
    }
    return output.str();
}

} // namespace realmheart::ui::workspace::animation
