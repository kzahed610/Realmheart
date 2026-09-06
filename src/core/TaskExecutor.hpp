#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace realmheart::core {

// Small shared worker pool for short blocking desktop-service operations.
// Keeping this centralized avoids one permanent thread per GTK widget while
// ensuring subprocess/D-Bus work never blocks GTK's main loop.
class TaskExecutor {
public:
    static constexpr std::size_t kMaxQueuedTasks = 64;

    explicit TaskExecutor(std::size_t worker_count = 2);
    ~TaskExecutor();

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    bool post(
        std::function<void()> task,
        std::string coalesce_key = {},
        std::function<bool()> cancelled = {}
    );
    void shutdown();

private:
    struct QueuedTask {
        std::function<void()> task;
        std::string coalesce_key;
        std::function<bool()> cancelled;
    };

    void worker_loop();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueuedTask> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

TaskExecutor& shared_task_executor();

} // namespace realmheart::core
