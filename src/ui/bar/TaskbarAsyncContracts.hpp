#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realmheart::ui::bar {

// A single owner may have many service notifications pending at once, but
// only one GTK idle callback is needed to refresh the latest state.
class RefreshGate {
public:
    RefreshGate() = default;
    RefreshGate(const RefreshGate&) = delete;
    RefreshGate& operator=(const RefreshGate&) = delete;

    bool claim() {
        return !queued_.exchange(true, std::memory_order_acq_rel);
    }

    void release() {
        queued_.store(false, std::memory_order_release);
    }

    bool queued() const {
        return queued_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> queued_{false};
};

// Registry for work keyed by an external identity such as a media-art URL.
// The Request type must expose `using Subscriber = ...` and a
// `std::vector<Subscriber> subscribers` member. Completion and discard both
// remove the entry, so a worker that produced no GTK source cannot orphan it.
template <typename Request>
class PendingRequestRegistry {
public:
    using RequestPtr = std::shared_ptr<Request>;
    using Subscriber = typename Request::Subscriber;

    template <typename IsSameSubscriber>
    std::pair<RequestPtr, bool> subscribe(
        const std::string& key,
        Subscriber subscriber,
        IsSameSubscriber&& is_same_subscriber
    ) {
        std::lock_guard lock(mutex_);
        auto& entry = requests_[key];
        const bool created = !entry;
        if (created) entry = std::make_shared<Request>();

        auto& subscribers = entry->subscribers;
        subscribers.erase(
            std::remove_if(
                subscribers.begin(),
                subscribers.end(),
                [&](const Subscriber& existing) {
                    return is_same_subscriber(existing, subscriber);
                }
            ),
            subscribers.end()
        );
        subscribers.push_back(std::move(subscriber));
        return {entry, created};
    }

    std::vector<Subscriber> complete(
        const std::string& key,
        const RequestPtr& expected
    ) {
        std::lock_guard lock(mutex_);
        const auto iterator = requests_.find(key);
        if (iterator == requests_.end() || iterator->second != expected) return {};
        auto subscribers = std::move(iterator->second->subscribers);
        requests_.erase(iterator);
        return subscribers;
    }

    template <typename Predicate>
    bool has_subscriber(
        const std::string& key,
        const RequestPtr& expected,
        Predicate&& predicate
    ) const {
        std::lock_guard lock(mutex_);
        const auto iterator = requests_.find(key);
        if (iterator == requests_.end() || iterator->second != expected) return false;
        return std::any_of(
            iterator->second->subscribers.begin(),
            iterator->second->subscribers.end(),
            std::forward<Predicate>(predicate)
        );
    }

    bool discard(const std::string& key, const RequestPtr& expected) {
        std::lock_guard lock(mutex_);
        const auto iterator = requests_.find(key);
        if (iterator == requests_.end() || iterator->second != expected) return false;
        requests_.erase(iterator);
        return true;
    }

    template <typename Predicate>
    bool discard_if(
        const std::string& key,
        const RequestPtr& expected,
        Predicate&& predicate
    ) {
        std::lock_guard lock(mutex_);
        const auto iterator = requests_.find(key);
        if (iterator == requests_.end() || iterator->second != expected) return false;
        if (!std::invoke(std::forward<Predicate>(predicate), iterator->second->subscribers)) {
            return false;
        }
        requests_.erase(iterator);
        return true;
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return requests_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, RequestPtr> requests_;
};

} // namespace realmheart::ui::bar
