#include "core/TaskExecutor.hpp"

#include <algorithm>
#include <exception>

namespace realmheart::core {

TaskExecutor::TaskExecutor(std::size_t worker_count) {
    worker_count = std::max<std::size_t>(1, worker_count);
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers_.emplace_back(&TaskExecutor::worker_loop, this);
    }
}

TaskExecutor::~TaskExecutor() {
    shutdown();
}

bool TaskExecutor::post(std::function<void()> task) {
    if (!task) return false;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return false;
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void TaskExecutor::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();

    std::queue<std::function<void()>> empty;
    {
        std::lock_guard lock(mutex_);
        tasks_.swap(empty);
    }
}

void TaskExecutor::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            task();
        } catch (const std::exception&) {
            // Individual jobs own their error reporting. One failed callback
            // must not terminate the shared worker pool.
        } catch (...) {
        }
    }
}

TaskExecutor& shared_task_executor() {
    static TaskExecutor executor(2);
    return executor;
}

} // namespace realmheart::core
