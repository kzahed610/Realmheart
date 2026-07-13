#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace realmheart::core {

// Small shared worker pool for short blocking desktop-service operations.
// Keeping this centralized avoids one permanent thread per GTK widget while
// ensuring subprocess/D-Bus work never blocks GTK's main loop.
class TaskExecutor {
public:
    explicit TaskExecutor(std::size_t worker_count = 2);
    ~TaskExecutor();

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    bool post(std::function<void()> task);
    void shutdown();

private:
    void worker_loop();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

TaskExecutor& shared_task_executor();

} // namespace realmheart::core
