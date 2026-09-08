#include "ui/bar/TaskbarAsyncContracts.hpp"
#include "ui/bar/VerticalBar.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"
#include "core/TaskExecutor.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct FakeRequest {
    using Subscriber = int;
    std::vector<Subscriber> subscribers;
};

struct CancellationRequest {
    using Subscriber = std::shared_ptr<std::atomic<bool>>;
    std::vector<Subscriber> subscribers;
};

struct FakeGeometryBar {
    int geometry_refreshes = 0;

    void refresh_geometry() {
        ++geometry_refreshes;
    }
};

void test_refresh_gate_coalesces_bursts() {
    realmheart::ui::bar::RefreshGate gate;
    require(gate.claim(), "first event must claim the refresh gate");
    require(!gate.claim(), "a burst must not claim a second idle refresh");
    require(gate.queued(), "claimed gate must report queued");
    gate.release();
    require(!gate.queued(), "completed idle refresh must release the gate");
    require(gate.claim(), "a later event must schedule after release");
}

void test_pending_registry_deduplicates_and_cleans_up() {
    realmheart::ui::bar::PendingRequestRegistry<FakeRequest> registry;
    const auto same_subscriber = [](int existing, int incoming) {
        return existing == incoming;
    };

    const auto [first, first_created] = registry.subscribe("art://same", 7, same_subscriber);
    require(first_created, "first art identity must create work");
    const auto [second, second_created] = registry.subscribe("art://same", 7, same_subscriber);
    require(!second_created, "duplicate art identity must reuse work");
    require(first == second, "duplicate art identity must share its request object");
    require(registry.size() == 1, "duplicate subscribers must retain one registry entry");

    const auto subscribers = registry.complete("art://same", first);
    require(subscribers.size() == 1, "same widget must not be retained twice");
    require(subscribers.front() == 7, "completed subscriber identity must be preserved");
    require(registry.size() == 0, "completion must remove the pending identity");

    const auto [cancelled, cancelled_created] = registry.subscribe(
        "art://cancelled", 11, same_subscriber
    );
    require(cancelled_created, "cancelled identity must create work");
    require(registry.discard("art://cancelled", cancelled), "discard must remove cancelled work");
    require(registry.size() == 0, "discard must not orphan cancelled work");

    const auto [retry, retry_created] = registry.subscribe(
        "art://cancelled", 12, same_subscriber
    );
    require(retry_created, "a discarded identity must be retryable");
    require(registry.discard("art://cancelled", retry), "retry cleanup must remove work");
}

void test_pending_registry_cancellation_is_atomic() {
    realmheart::ui::bar::PendingRequestRegistry<CancellationRequest> registry;
    const auto never_same = [](const auto&, const auto&) { return false; };
    const auto live = std::make_shared<std::atomic<bool>>(false);
    const auto [request, created] = registry.subscribe("art://queued", live, never_same);
    require(created, "queued cancellation fixture must create work");

    require(
        !registry.discard_if(
            "art://queued",
            request,
            [](const std::vector<CancellationRequest::Subscriber>&) { return false; }
        ),
        "live queued work must not be discarded by a failed cancellation check"
    );
    require(registry.size() == 1, "failed cancellation must retain queued work");
    require(
        registry.discard_if(
            "art://queued",
            request,
            [](const std::vector<CancellationRequest::Subscriber>& subscribers) {
                return subscribers.empty();
            }
        ) == false,
        "a subscriber must prevent cancellation cleanup"
    );

    realmheart::core::TaskExecutor executor(1);
    std::atomic<bool> task_ran{false};
    require(
        executor.post(
            [&task_ran] { task_ran.store(true); },
            {},
            [&registry, request] {
                return registry.discard_if(
                    "art://queued",
                    request,
                    [](const std::vector<CancellationRequest::Subscriber>& subscribers) {
                        return std::none_of(
                            subscribers.begin(),
                            subscribers.end(),
                            [](const CancellationRequest::Subscriber& subscriber) {
                                return subscriber->load();
                            }
                        );
                    }
                );
            }
        ),
        "executor must accept cancellable queued work"
    );
    executor.wait_for_idle();
    require(!task_ran.load(), "cancelled queued work must skip its task body");
    require(registry.size() == 0, "skipped task cleanup must remove the registry entry");

    const auto [retry, retry_created] = registry.subscribe(
        "art://queued", std::make_shared<std::atomic<bool>>(true), never_same
    );
    require(retry_created, "skipped work must be retryable");
    require(
        registry.discard("art://queued", retry),
        "retry cleanup must remove queued work"
    );
}

void test_pending_registry_handles_independent_bars() {
    realmheart::ui::bar::PendingRequestRegistry<FakeRequest> registry;
    const auto never_same = [](int, int) { return false; };
    const auto [first, first_created] = registry.subscribe("art://shared", 1, never_same);
    const auto [second, second_created] = registry.subscribe("art://shared", 2, never_same);
    require(first_created && !second_created, "bars must share one pending identity");
    require(first == second, "bars must receive the same pending request");
    const auto subscribers = registry.complete("art://shared", first);
    require(subscribers.size() == 2, "all live bars must be fanned out on completion");
    require(registry.size() == 0, "shared completion must remove the identity");
}

void test_all_monitor_bars_refresh_geometry() {
    FakeGeometryBar primary;
    std::vector<std::unique_ptr<FakeGeometryBar>> secondary;
    secondary.push_back(std::make_unique<FakeGeometryBar>());
    secondary.push_back(nullptr);
    secondary.push_back(std::make_unique<FakeGeometryBar>());

    realmheart::ui::bar::refresh_monitor_bars(&primary, secondary);

    require(primary.geometry_refreshes == 1,
            "primary bar must refresh geometry once");
    require(secondary[0]->geometry_refreshes == 1,
            "first secondary bar must refresh geometry once");
    require(secondary[1] == nullptr,
            "null secondary bars must remain safely ignored");
    require(secondary[2]->geometry_refreshes == 1,
            "last secondary bar must refresh geometry once");

    realmheart::ui::bar::refresh_monitor_bars(
        static_cast<FakeGeometryBar*>(nullptr),
        secondary
    );
    require(secondary[0]->geometry_refreshes == 2,
            "secondary bars must remain refreshable without a primary bar");
    require(secondary[2]->geometry_refreshes == 2,
            "all surviving secondary bars must refresh on every notification");
}

void test_svg_fallback_contract_is_discoverable() {
    using realmheart::ui::bar::widgets::ThemedSvgFallback;
    require(ThemedSvgFallback::required(false, true), "missing source must require fallback");
    require(ThemedSvgFallback::required(true, false), "invalid SVG must require fallback");
    require(!ThemedSvgFallback::required(true, true), "valid SVG must not require fallback");
    require(std::string(ThemedSvgFallback::text()) == "?", "fallback must have visible text");
    require(
        std::string(ThemedSvgFallback::tooltip()) == "Icon unavailable",
        "fallback must expose a diagnostic tooltip"
    );
    require(
        std::string(ThemedSvgFallback::css_class()).find("fallback") != std::string::npos,
        "fallback must expose its CSS class"
    );
    require(
        std::string(ThemedSvgFallback::unavailable_css_class()).find("unavailable") != std::string::npos,
        "unavailable icon state must expose its CSS class"
    );
}

} // namespace

int main() {
    test_refresh_gate_coalesces_bursts();
    test_pending_registry_deduplicates_and_cleans_up();
    test_pending_registry_cancellation_is_atomic();
    test_pending_registry_handles_independent_bars();
    test_all_monitor_bars_refresh_geometry();
    test_svg_fallback_contract_is_discoverable();
    std::cout << "Taskbar async/icon contract tests PASSED\n";
    return 0;
}
