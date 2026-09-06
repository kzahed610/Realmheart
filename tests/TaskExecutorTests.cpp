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
    executor.wait_for_idle();
    require(runs == 10, "coalescing must discard the obsolete queued task");
}

void test_owner_invalidation_cancels_queued_callback() {
    realmheart::core::TaskExecutor executor(1);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    std::atomic<bool> owner_alive = true;
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
    require(executor.post(
                [&] { ran = true; }, {}, [&] { return !owner_alive.load(); }
            ),
            "owner-bound callback should be accepted before invalidation");

    owner_alive = false;

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_one();
    executor.wait_for_idle();
    require(!ran.load(), "owner-invalidated callback must not execute");
}

void test_wait_for_idle_waits_for_running_task() {
    realmheart::core::TaskExecutor executor(1);
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    bool wait_started = false;
    std::atomic<bool> wait_returned = false;

    require(executor.post([&] {
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        condition.notify_one();
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return release; });
    }), "running task should be accepted");
    wait_until_started(mutex, condition, started);

    std::thread wait_thread([&] {
        {
            std::lock_guard lock(mutex);
            wait_started = true;
        }
        condition.notify_one();
        executor.wait_for_idle();
        wait_returned = true;
    });
    wait_until_started(mutex, condition, wait_started);

    require(!wait_returned.load(),
            "wait_for_idle must wait while a task is still running");

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_one();
    wait_thread.join();

    require(wait_returned.load(), "wait_for_idle should complete after release");
}

void test_shutdown_rejects_work_after_quiescence() {
    realmheart::core::TaskExecutor executor(1);
    std::atomic<bool> ran = false;
    require(executor.post([&] { ran = true; }), "task should be accepted");
    executor.wait_for_idle();
    require(ran.load(), "wait_for_idle must observe completed work");

    executor.shutdown();
    require(!executor.post([] {}), "shutdown must reject new work");
}

} // namespace

int main() {
    test_queue_is_bounded();
    test_coalescing_keeps_only_latest_queued_task();
    test_owner_invalidation_cancels_queued_callback();
    test_wait_for_idle_waits_for_running_task();
    test_shutdown_rejects_work_after_quiescence();
    std::cout << "TaskExecutor tests passed\n";
    return 0;
}
