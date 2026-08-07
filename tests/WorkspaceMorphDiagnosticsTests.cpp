#include "ui/workspace/animation/WorkspaceMorphDiagnostics.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using realmheart::ui::workspace::animation::WorkspaceMorphDiagnostics;
using realmheart::ui::workspace::animation::format_workspace_morph_diagnostics;
using realmheart::ui::workspace::animation::parse_workspace_morph_rss_kib;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void require_near(double actual, double expected, std::string_view message) {
    const double difference = actual > expected
        ? actual - expected
        : expected - actual;
    if (difference > 0.0001) throw std::runtime_error(std::string(message));
}

void test_disabled_diagnostics_are_zero_cost_state() {
    WorkspaceMorphDiagnostics diagnostics(false);
    diagnostics.begin(true, 1.0, 1000);
    diagnostics.record_frame(0.016);
    require(!diagnostics.active(),
            "disabled diagnostics must not create an active session");
}

void test_reversal_frame_and_memory_accounting() {
    WorkspaceMorphDiagnostics diagnostics(true);
    diagnostics.begin(true, 10.0, 1000);
    diagnostics.record_frame(0.016);
    diagnostics.record_frame(0.041);
    diagnostics.note_shader_started(4096);
    diagnostics.note_shader_ready(8192);
    diagnostics.set_direction(false);
    diagnostics.set_direction(true);

    const auto report = diagnostics.finish(false, 10.44, 1012, 0);
    require(!diagnostics.active(), "finish must close the diagnostics session");
    require(report.frame_count == 2, "every timed morph frame must be counted");
    require(report.reversal_count == 2,
            "direction changes must be counted without resetting the session");
    require(report.shader_started && report.shader_ready && !report.shader_failed,
            "shader lifecycle state must survive until the endpoint report");
    require(report.peak_transient_bytes == 8192,
            "peak transition memory must retain the largest observed value");
    require(report.retained_transient_bytes == 0,
            "endpoint cleanup must be reportable as zero retained bytes");
    require(report.rss_end_kib - report.rss_start_kib == 12,
            "RSS delta must remain available for repeated-cycle profiling");
    require_near(report.elapsed_ms, 440.0,
                 "elapsed transition time must use the supplied monotonic clock");
    require_near(report.worst_frame_ms, 41.0,
                 "the worst frame interval must be retained");
}

void test_fallback_and_rss_parsing() {
    WorkspaceMorphDiagnostics diagnostics(true);
    diagnostics.begin(false, 2.0, -1);
    diagnostics.note_shader_failed();
    const auto report = diagnostics.finish(true, 2.34, -1, 0);
    const std::string formatted = format_workspace_morph_diagnostics(report);
    require(formatted.find("shader=fallback") != std::string::npos,
            "formatted diagnostics must make geometry fallback explicit");
    require(parse_workspace_morph_rss_kib(
                "Name:\trealmheart\nVmRSS:\t   28764 kB\n") == 28764,
            "VmRSS parsing must accept normal procfs whitespace");
    require(parse_workspace_morph_rss_kib("Name:\trealmheart\n") == -1,
            "missing VmRSS must be represented as unavailable");
}

} // namespace

int main() {
    try {
        test_disabled_diagnostics_are_zero_cost_state();
        test_reversal_frame_and_memory_accounting();
        test_fallback_and_rss_parsing();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceMorphDiagnosticsTests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace morph diagnostics tests passed\n";
    return 0;
}
