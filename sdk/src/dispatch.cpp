#include "gridnode/dispatch.hpp"

#include <algorithm>
#include <exception>

namespace gn::thread {

void Dispatcher::addListener(std::string owner, Tick fn) {
    std::lock_guard<std::mutex> lock(m_);
    added_.push_back({std::move(owner), std::move(fn)});
}

void Dispatcher::post(Task fn) {
    std::lock_guard<std::mutex> lock(m_);
    tasks_.push_back(std::move(fn));
}

void Dispatcher::removeListeners(std::function<bool(const Tick&)> which) {
    std::lock_guard<std::mutex> lock(m_);
    added_.erase(std::remove_if(added_.begin(), added_.end(), [&](const Listener& l) { return which(l.fn); }), added_.end());
    removals_.push_back(std::move(which));
    settle_ = true;
}

size_t Dispatcher::removeTasks(const std::function<bool(const Task&)>& which) {
    std::lock_guard<std::mutex> lock(m_);
    const size_t before = tasks_.size();
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [&](const Task& t) { return which(t); }), tasks_.end());
    settle_ = true; // a batch swapped out before this may still hold some: wait for the next tick
    return before - tasks_.size();
}

bool Dispatcher::removing() const {
    std::lock_guard<std::mutex> lock(m_);
    return settle_ || settling_;
}

size_t Dispatcher::listenerCount() const {
    std::lock_guard<std::mutex> lock(m_);
    return listeners_.size() + added_.size();
}

void Dispatcher::dispatch(float dt) {
    ticks_.fetch_add(1, std::memory_order_relaxed);
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(m_);
        tasks.swap(tasks_);
        // listeners_ is only touched on this thread; new ones are handed over here.
        for (auto& l : added_) listeners_.push_back(std::move(l));
        added_.clear();
        for (const auto& which : removals_)
            listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [&](const Listener& l) { return which(l.fn); }),
                             listeners_.end());
        removals_.clear();
        settling_ = settle_;
        settle_ = false;
    }
    for (auto& t : tasks) {
        if (live_ && !live_(t)) {
            if (errors_) errors_("a task of a mod being unloaded was dropped");
            continue;
        }
        try {
            t();
        } catch (const std::exception& e) {
            if (errors_) errors_(std::string("task threw: ") + e.what());
        } catch (...) {
            if (errors_) errors_("task threw an unknown exception");
        }
    }
    bool removed = false;
    for (auto& l : listeners_) {
        if (l.dead) continue;
        try {
            l.fn(dt);
        } catch (const std::exception& e) {
            l.dead = removed = true;
            if (errors_) errors_(l.owner + ": tick listener threw (" + e.what() + "); it is removed");
        } catch (...) {
            l.dead = removed = true;
            if (errors_) errors_(l.owner + ": tick listener threw; it is removed");
        }
    }
    std::lock_guard<std::mutex> lock(m_);
    if (removed) // under the lock: listenerCount() reads the size from other threads
        listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [](const Listener& l) { return l.dead; }),
                         listeners_.end());
    settling_ = false; // everything this tick took has returned
}

} // namespace gn::thread
