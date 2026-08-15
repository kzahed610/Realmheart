#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace realmheart::ui::workspace::animation {

struct WorkspaceMorphDiagnosticReport {
    bool ended_visible = false;
    bool shader_started = false;
    bool shader_ready = false;
    bool shader_failed = false;
    unsigned int frame_count = 0;
    unsigned int reversal_count = 0;
    double elapsed_ms = 0.0;
    double worst_frame_ms = 0.0;
    long rss_start_kib = -1;
    long rss_end_kib = -1;
    std::size_t peak_transient_bytes = 0;
    std::size_t retained_transient_bytes = 0;
};

class WorkspaceMorphDiagnostics {
public:
    explicit WorkspaceMorphDiagnostics(bool enabled = false) noexcept;

    void set_enabled(bool enabled) noexcept;
    void begin(bool opening, double now_seconds, long rss_kib) noexcept;
    void set_direction(bool opening) noexcept;
    void record_frame(double elapsed_seconds) noexcept;
    void note_shader_started(std::size_t transient_bytes) noexcept;
    void note_shader_ready(std::size_t transient_bytes) noexcept;
    void note_shader_failed() noexcept;
    void note_transient_bytes(std::size_t transient_bytes) noexcept;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] WorkspaceMorphDiagnosticReport finish(
        bool ended_visible,
        double now_seconds,
        long rss_kib,
        std::size_t retained_transient_bytes
    ) noexcept;

private:
    bool enabled_ = false;
    bool active_ = false;
    bool opening_ = true;
    double start_seconds_ = 0.0;
    WorkspaceMorphDiagnosticReport report_{};
};

[[nodiscard]] bool workspace_morph_diagnostics_enabled() noexcept;
[[nodiscard]] long parse_workspace_morph_rss_kib(std::string_view status) noexcept;
[[nodiscard]] long workspace_morph_process_rss_kib() noexcept;
[[nodiscard]] std::string format_workspace_morph_diagnostics(
    const WorkspaceMorphDiagnosticReport& report
);

} // namespace realmheart::ui::workspace::animation
