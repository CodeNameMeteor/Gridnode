#pragma once
// Per-realm tick dispatch: listeners called every tick, and one-shot tasks queued from any
// thread. Portable and lock-light: the tick thread swaps the task queue out under a mutex and
// runs it without holding the lock.

#include <cstdint>
#include <functional>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace gn::thread {

class Dispatcher {
public:
    using Tick = std::function<void(float dt)>;
    using Task = std::function<void()>;
    using ErrorSink = std::function<void(const std::string& what)>;

    void addListener(std::string owner, Tick fn);
    void post(Task fn);
    // Takes out the listeners `which` picks (a mod being unloaded) at the start of the next tick:
    // the listeners belong to the tick thread. removing() is true until that tick has run.
    void removeListeners(std::function<bool(const Tick&)> which);
    bool removing() const;
    // Called on the realm's thread once per tick: queued tasks first, then listeners.
    // A listener that throws is reported and removed; a task that throws is reported.
    void dispatch(float dt);

    void setErrorSink(ErrorSink sink) { errors_ = std::move(sink); }
    size_t listenerCount() const;
    uint64_t ticks() const { return ticks_.load(std::memory_order_relaxed); }

private:
    struct Listener {
        std::string owner;
        Tick fn;
        bool dead = false;
    };
    mutable std::mutex m_;
    std::vector<Listener> listeners_;
    std::vector<Listener> added_;
    std::vector<Task> tasks_;
    std::vector<std::function<bool(const Tick&)>> removals_;
    ErrorSink errors_;
    std::atomic<uint64_t> ticks_{0}; // written on the tick thread, read by the UI
};

} // namespace gn::thread
