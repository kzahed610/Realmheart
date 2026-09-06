#include "core/TaskExecutor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void wait_until_started(std::mutex& mutex, std::condition_variable& condition, bool& started) {
    std::unique_lock lock(mutex);
    require(condition.wait_for(lock, 1s, [&] { return started; }), "worker did not start blocker");
}

void test_queue_is_bounded() {
    realmheart::core::TaskExecutor executor(1);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;

    require(executor.post([&] {
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        condition.notify_one();
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return release; });
    }), "blocker should be accepted");
    wait_until_started(mutex, condition, started);

    std::size_t accepted = 0;
    for (std::size_t index = 0; index < realmheart::core::TaskExecutor::kMaxQueuedTasks; ++index) {
        accepted += executor.post([] {}) ? 1 : 0;
    }
    require(accepted == realmheart::core::TaskExecutor::kMaxQueuedTasks,
            "executor must accept up to its queue capacity");
    require(!executor.post([] {}), "executor must reject work beyond its queue capacity");

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_one();
    executor.shutdown();
}

void test_coalescing_keeps_only_latest_queued_task() {
    realmheart::core::TaskExecutor executor(1);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    std::atomic<int> runs = 0;

    require(executor.post([&] {
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        condition.notify_one();
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return release; });
    }), "blocker should be accepted");
    wait_until_started(mutex, condition, started);

    require(executor.post([&] { ++runs; }, "replaceable"), "first coalesced task should be accepted");
    require(executor.post([&] { runs += 10; }, "replaceable"), "coalesced task should be replaced");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_one();
    for (int attempt = 0; attempt < 100 && runs.load() != 10; ++attempt) {
        std::this_thread::sleep_for(1ms);
    }
    executor.shutdown();
    require(runs == 10, "coalescing must discard the obsolete queued task");
}

void test_cancelled_queued_task_is_not_run() {
    realmheart::core::TaskExecutor executor(1);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    std::atomic<bool> cancelled = true;
    std::atomic<bool> ran = false;

    require(executor.post([&] {
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        condition.notify_one();
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return release; });
    }), "blocker should be accepted");
    wait_until_started(mutex, condition, started);
    require(executor.post([&] { ran = true; }, {}, [&] { return cancelled.load(); }),
            "cancelled task should be accepted before execution");

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_one();
    executor.shutdown();
    require(!ran.load(), "cancelled queued task must not execute");
}

} // namespace

int main() {
    test_queue_is_bounded();
    test_coalescing_keeps_only_latest_queued_task();
    test_cancelled_queued_task_is_not_run();
    std::cout << "TaskExecutor tests passed\n";
    return 0;
}
