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

bool TaskExecutor::post(
    std::function<void()> task,
    std::string coalesce_key,
    std::function<bool()> cancelled
) {
    if (!task) return false;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return false;
        if (!coalesce_key.empty()) {
            for (auto& queued : tasks_) {
                if (queued.coalesce_key == coalesce_key) {
                    queued.task = std::move(task);
                    queued.cancelled = std::move(cancelled);
                    return true;
                }
            }
        }
        if (tasks_.size() >= kMaxQueuedTasks) return false;
        tasks_.push_back({
            std::move(task),
            std::move(coalesce_key),
            std::move(cancelled)
        });
    }
    cv_.notify_one();
    return true;
}

void TaskExecutor::wait_for_idle() {
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [this] {
        return tasks_.empty() && active_tasks_ == 0;
    });
}

void TaskExecutor::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        tasks_.clear();
        if (active_tasks_ == 0) idle_cv_.notify_all();
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();

}

void TaskExecutor::worker_loop() {
    while (true) {
        std::function<void()> task;
        std::function<bool()> cancelled;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_) return;
            auto queued = std::move(tasks_.front());
            tasks_.pop_front();
            ++active_tasks_;
            task = std::move(queued.task);
            cancelled = std::move(queued.cancelled);
        }

        try {
            if (!cancelled || !cancelled()) task();
        } catch (const std::exception&) {
            // Individual jobs own their error reporting. One failed callback
            // must not terminate the shared worker pool.
        } catch (...) {
        }

        {
            std::lock_guard lock(mutex_);
            --active_tasks_;
            if (tasks_.empty() && active_tasks_ == 0) idle_cv_.notify_all();
        }
    }
}

TaskExecutor& shared_task_executor() {
    static TaskExecutor executor(2);
    return executor;
}

} // namespace realmheart::core
