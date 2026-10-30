// Server and client ticks: GridNode wraps the two worlds' postUpdate vtable slots (pointer swaps,
// see db/annotations/world.yaml), calls the game's function, then runs the realm's dispatcher.

#include <windows.h>

#include <atomic>
#include <exception>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

using UpdateFn = void (*)(void* world, void* updateInfo);
// The game's functions (or an earlier hook). Written by HookTable::install before the slot is
// swapped, so a detour never runs without them.
uint64_t g_serverOrig = 0;
uint64_t g_clientOrig = 0;
std::atomic<DWORD> g_serverThread{0};
std::atomic<DWORD> g_clientThread{0};

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
    reinterpret_cast<UpdateFn>(g_serverOrig)(world, updateInfo);
    g_serverThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    guarded("server messages", [] { messagesServerTick(); });
    guarded("layouts (server)", [] { layouts::serverTick(); });
    guarded("server dispatch", [&] { server().dispatch(dtOf(updateInfo)); });
}

void clientPostUpdate(void* world, void* updateInfo) {
    reinterpret_cast<UpdateFn>(g_clientOrig)(world, updateInfo);
    g_clientThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    guarded("world", [] { worldClientTick(); });
    guarded("client messages", [] { messagesClientTick(); });
    guarded("faith", [] { faithClientTick(); });
    guarded("movement events", [] { movementEventsClientTick(); });
    guarded("layouts (client)", [] { layouts::clientTick(); });
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
    TickStatus s;
    s.server = install(engine::hook::ServerGameWorld_postUpdate, &serverPostUpdate, &g_serverOrig);
    s.client = install(engine::hook::ClientGameWorld_postUpdate, &clientPostUpdate, &g_clientOrig);
    return s;
}

bool isServerThread() { return g_serverThread.load(std::memory_order_relaxed) == GetCurrentThreadId(); }
bool isClientThread() { return g_clientThread.load(std::memory_order_relaxed) == GetCurrentThreadId(); }

} // namespace gn::host
