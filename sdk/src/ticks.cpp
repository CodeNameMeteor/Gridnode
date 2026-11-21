// Server and client ticks: GridNode wraps the two worlds' postUpdate vtable slots (pointer swaps,
// see db/annotations/world.yaml), calls the game's function, then runs the realm's dispatcher.

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <string>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

using UpdateFn = void (*)(void* world, void* updateInfo);
// The game's functions (or an earlier hook). Written by HookTable::install before the slot is
// swapped, so a detour never runs without them.
// The game's postUpdate under each hook. The hook table rewrites these while a detour may be
// reading them (a hook above ours removed: hook_table.cpp, one aligned 8-byte store), so they are
// atomic; the table is given their address as a plain uint64_t*, which is the same object.
std::atomic<uint64_t> g_serverOrig{0};
std::atomic<uint64_t> g_clientOrig{0};
static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t) && std::atomic<uint64_t>::is_always_lock_free,
              "the hook table writes these as plain 8-byte values");
std::atomic<DWORD> g_serverThread{0};
std::atomic<DWORD> g_clientThread{0};

// Whether the two realms' ticks overlap (block X, 2026-11-15: in the game's job mode updateGameFrame can run the
// client's frame while the server's runs as a job). Measured over GridNode's own part of each tick: how many
// client ticks began while a server tick of ours was still running (and the reverse), and how often the server
// tick moved to another thread. Read by the console's "threads".
std::atomic<int> g_inServer{0};
std::atomic<int> g_inClient{0};
std::atomic<uint64_t> g_serverTicks{0}, g_clientTicks{0}, g_overlaps{0}, g_serverMoves{0};

struct InTick {
    std::atomic<int>& mine;
    InTick(std::atomic<int>& m, const std::atomic<int>& other) : mine(m) {
        mine.fetch_add(1, std::memory_order_acq_rel);
        if (other.load(std::memory_order_acquire) > 0) g_overlaps.fetch_add(1, std::memory_order_relaxed);
    }
    ~InTick() { mine.fetch_sub(1, std::memory_order_acq_rel); }
};

float dtOf(void* updateInfo) {
    float dt = 0;
    mem::read(reinterpret_cast<uintptr_t>(updateInfo) + 0x10, dt);
    return dt;
}

// Runs one of GridNode's tick steps; an exception never leaves into the engine's frames.
template <class F> void guarded(const char* what, F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 20) GN_LOG("tick: %s threw (%s); ignored", what, e.what());
    } catch (...) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 20) GN_LOG("tick: %s threw; ignored", what);
    }
}

void serverPostUpdate(void* world, void* updateInfo) {
    reinterpret_cast<UpdateFn>(g_serverOrig.load(std::memory_order_acquire))(world, updateInfo);
    if (unloaded()) return; // kept in place by a hook over it: the game's update only
    InTick in(g_inServer, g_inClient);
    g_serverTicks.fetch_add(1, std::memory_order_relaxed);
    if (g_serverThread.exchange(GetCurrentThreadId(), std::memory_order_relaxed) != GetCurrentThreadId()) g_serverMoves.fetch_add(1, std::memory_order_relaxed);
    guarded("time of day", [] { timeOfDayTick(); }); // before anything of ours reads the clock
    guarded("visual environment", [] { visualEnvTick(); });
    guarded("server messages", [] { messagesServerTick(); });
    guarded("layouts (server)", [] { layouts::serverTick(); });
    guarded("kill volumes", [] { killVolumesServerTick(); });
    guarded("level ready", [] { levelReadyServerTick(); });
    guarded("server dispatch", [&] { server().dispatch(dtOf(updateInfo)); });
}

void clientPostUpdate(void* world, void* updateInfo) {
    reinterpret_cast<UpdateFn>(g_clientOrig.load(std::memory_order_acquire))(world, updateInfo);
    if (unloaded()) return;
    InTick in(g_inClient, g_inServer);
    g_clientTicks.fetch_add(1, std::memory_order_relaxed);
    g_clientThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    // The time of day is held on the server tick only: the clock is the server's entity (2026-11-20; it was
    // written from both ticks, which in the game's job mode can run at once).
    guarded("visual environment", [] { visualEnvTick(); });
    guarded("world", [] { worldClientTick(); });
    guarded("client messages", [] { messagesClientTick(); });
    guarded("faith", [] { faithClientTick(); });
    guarded("audio mix", [&] { audioMixTick(dtOf(updateInfo)); });
    guarded("movement events", [] { movementEventsClientTick(); });
    guarded("layouts (client)", [] { layouts::clientTick(); });
    guarded("ant types", [] { antTick(); });
    guarded("effects", [] { effectsClientTick(); });
    guarded("rules", [] { rulesClientTick(); });
    guarded("actions", [] { actionsClientTick(); });
    guarded("client dispatch", [&] { client().dispatch(dtOf(updateInfo)); });
}

bool install(const engine::hook::Point& p, UpdateFn detour, uint64_t* original) {
    std::string why;
    const auto r = hooks().install("gridnode", mem::rva(p.slot), mem::rva(p.original), reinterpret_cast<uint64_t>(detour),
                                   original, &why);
    if (r != hook::Result::Ok) {
        GN_LOG("ticks: cannot hook %s: %s", p.name, why.c_str());
        return false;
    }
    return true;
}

} // namespace

TickStatus installTicks() {
    auto report = [](const char* realm) {
        return [realm](const std::string& what) { GN_LOG("%s tick: %s", realm, what.c_str()); };
    };
    server().setErrorSink(report("server"));
    client().setErrorSink(report("client"));
    server().setTaskFilter(taskLive);
    client().setTaskFilter(taskLive);
    TickStatus s;
    s.server = install(engine::hook::ServerGameWorld_postUpdate, &serverPostUpdate, reinterpret_cast<uint64_t*>(&g_serverOrig));
    s.client = install(engine::hook::ClientGameWorld_postUpdate, &clientPostUpdate, reinterpret_cast<uint64_t*>(&g_clientOrig));
    return s;
}

std::string threadsCommand() {
    char b[400];
    const uint64_t moves = g_serverMoves.load();
    snprintf(b, sizeof(b),
             "threads: %llu server ticks, %llu client ticks; %llu began while the other realm's tick (GridNode's part) was running; "
             "the server tick changed thread %llu times (more than once: it runs as a job, so the realms can overlap)",
             static_cast<unsigned long long>(g_serverTicks.load()), static_cast<unsigned long long>(g_clientTicks.load()),
             static_cast<unsigned long long>(g_overlaps.load()), static_cast<unsigned long long>(moves));
    return b;
}

bool isServerThread() { return g_serverThread.load(std::memory_order_relaxed) == GetCurrentThreadId(); }
bool isClientThread() { return g_clientThread.load(std::memory_order_relaxed) == GetCurrentThreadId(); }

} // namespace gn::host
