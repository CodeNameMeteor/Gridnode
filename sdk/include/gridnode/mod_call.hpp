#pragma once
// The mod code GridNode is calling on this thread. API functions find their mod from the return
// address, which a tail call breaks: compiled at -O2, `void on_key(void*) { gn::cameraRelease(); }`
// becomes a jump, the return address is GridNode's own, and what the call owned (camera, holds,
// patches, boxes, sounds) had no owner and was not released when the mod unloaded (review,
// 2026-11-11). Every place GridNode calls into a mod sets a Scope with the function it calls (an
// address in that mod's code); an API call whose return address is in gridnode.dll is credited to it.
// Portable (no Windows) so the dispatcher can use it and the host tests can check it.

namespace gn::modcall {

inline thread_local const void* t_current = nullptr;

class Scope {
public:
    template <class F>
    explicit Scope(F* fn) : prev_(t_current) {
        t_current = reinterpret_cast<const void*>(fn);
    }
    ~Scope() { t_current = prev_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const void* prev_;
};

inline const void* current() { return t_current; }

// The address to identify the caller by: `ret`, unless it is in GridNode's own code (inOwn) and a
// mod's callback is running on this thread.
template <class InOwn>
const void* resolve(const void* ret, InOwn inOwn) {
    return t_current && inOwn(ret) ? t_current : ret;
}

} // namespace gn::modcall
