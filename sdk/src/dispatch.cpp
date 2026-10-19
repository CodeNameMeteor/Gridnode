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

size_t Dispatcher::listenerCount() const {
    std::lock_guard<std::mutex> lock(m_);
    return listeners_.size() + added_.size();
}

void Dispatcher::dispatch(float dt) {
    ++ticks_;
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(m_);
        tasks.swap(tasks_);
        // listeners_ is only touched on this thread; new ones are handed over here.
        for (auto& l : added_) listeners_.push_back(std::move(l));
        added_.clear();
    }
    for (auto& t : tasks) {
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
    if (removed)
        listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [](const Listener& l) { return l.dead; }),
                         listeners_.end());
}

} // namespace gn::thread
