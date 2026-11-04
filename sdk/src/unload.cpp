// Unloading GridNode completely, without restarting the game: the mods are unloaded (each puts back
// what it changed in gn_mod_unload), GridNode's own changes are put back (the free camera, held
// keys, god mode, layout copies, message listeners), every hook comes out of the hook table, the
// overlay closes and the game's Present and window procedure go back. Then gridnode.dll is freed
// (dllmain.cpp). Anything that cannot be put back (another tool hooked over GridNode, a mod without
// gn_mod_unload) keeps it loaded but inert: nothing of it runs any more.
// Before the free, every other thread is checked for being in GridNode's code or about to return
// into it (quiet.cpp); if one still is after a few seconds, GridNode stays in memory, inert.
//
// Not put back: props and barriers moved by a saved layout (until the level reloads), and settings
// changed through the console.

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "gridnode/log.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

std::atomic<bool> g_requested{false};
std::atomic<bool> g_unloading{false};
std::atomic<bool> g_unloaded{false};
std::atomic<int> g_busy{0}; // our short-lived threads still running

// Waits until each ticking realm ran `n` more ticks (a realm that is not ticking is not waited for
// past the timeout: its hooks are idle).
void waitTicks(int n, DWORD ms) {
    const uint64_t s0 = server().ticks(), c0 = client().ticks();
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < ms) {
        if (server().ticks() >= s0 + n && client().ticks() >= c0 + n) return;
        Sleep(20);
    }
    GN_LOG("unload: %s not ticking; going on", server().ticks() < s0 + n ? (client().ticks() < c0 + n ? "the server and client are" : "the server is")
                                                                        : "the client is");
}

} // namespace

BusyThread::BusyThread() { g_busy.fetch_add(1); }
BusyThread::~BusyThread() { g_busy.fetch_sub(1); }

void unloadRequest() { g_requested = true; }
bool unloadRequested() { return g_requested.load(); }
bool unloading() { return g_unloading.load(); }
bool unloaded() { return g_unloaded.load(std::memory_order_relaxed); }

bool unloadEverything() {
    g_unloading = true;
    GN_LOG("unload: unloading GridNode and every mod");
    bool canFree = true;

    // 1. The mods: each one's gn_mod_unload puts back what it changed (god mode, settings...).
    if (!unloadAllMods()) canFree = false;

    // 2. GridNode's own changes, on the threads they belong to.
    recorder::release();
    tracer::release();
    layouts::release();
    faithRelease();
    messagesRelease();
    forgetSounds(CodeRange{}); // every one, released on the client thread in the ticks below
    waitTicks(3, 3000);
    // Collision boxes leave the world at its next physics update, while the flush hook is still in.
    if (!bodiesRelease(2000)) GN_LOG("unload: collision boxes still in the physics world (no physics update ran); they stay");
    if (!messagesReleased()) {
        GN_LOG("unload: a message manager was not ticking; its listener registration stays, so GridNode stays in memory");
        canFree = false;
    }

    // 3. Every code patch, then every hook, all owners (entity creators and destructors, events,
    // ticks, the recorder's).
    if (const size_t n = removeAllPatches()) GN_LOG("unload: %llu code patches undone", static_cast<unsigned long long>(n));
    std::vector<std::string> refused;
    const size_t out = hooks().remove([](const hook::Entry&) { return true; }, &refused);
    g_unloaded = true;
    GN_LOG("unload: %llu hooks taken out", static_cast<unsigned long long>(out));
    for (const std::string& r : refused) GN_LOG("unload: kept: %s", r.c_str());
    if (!refused.empty()) canFree = false;

    // 4. The overlay and the window's input.
    if (!ui::release()) canFree = false;
    crash::uninstall();

    // 5. Threads still in our code: the game's (in a hook or the overlay when it went) and ours.
    const ULONGLONG start = GetTickCount64();
    while (g_busy.load() > 0 && GetTickCount64() - start < 10000) Sleep(20);
    if (g_busy.load() > 0) {
        GN_LOG("unload: %d of GridNode's threads still running (an export?); GridNode stays loaded", g_busy.load());
        canFree = false;
    }
    // A game thread may still be in one of GridNode's hooks (a tick wrapping a level load runs for
    // seconds), or returning into one: gridnode.dll is freed only once none is.
    if (canFree) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(reinterpret_cast<void*>(&unloadEverything)), &self);
        if (!self || !waitCodeQuiet(self, "unload: gridnode.dll", 5000)) canFree = false;
    }
    configFlush(true);
    GN_LOG(canFree ? "unload: done; gridnode.dll is being freed" : "unload: done; GridNode stays in memory, inactive (see above)");
    return canFree;
}

} // namespace gn::host
