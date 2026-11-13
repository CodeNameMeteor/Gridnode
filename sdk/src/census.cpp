// The creation census (console "census"): an opt-in detour on the game's one creation function for
// entities made from data (entityCreator_spawnEntityWithContext, 0x143167120: blueprints, buses, the client's
// spawns), counting what it makes and how many of those GridNode's creator hooks (entities.cpp: a pool of
// 2,048 creator slots, refreshed every second) did not see. Its numbers decide whether that function can
// replace the pool (handover plan, audit item 2). Off unless asked; it only counts.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;

using SpawnFn = void* (*)(void* ci, void* ctx);
SpawnFn g_original = nullptr;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_calls{0}, g_made{0}, g_skipped{0}, g_missed{0}, g_unknown{0};
std::mutex g_m;
struct Count {
    uint64_t made = 0, missed = 0;
};
std::map<std::string, Count> g_byClass; // (g_m)

void* spawnHook(void* ci, void* ctx) {
    void* entity = g_original(ci, ctx);
    guardHook("creation census", [&] {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        if (!entity) {
            g_skipped.fetch_add(1, std::memory_order_relaxed); // no creator, or the data excluded itself
            return;
        }
        g_made.fetch_add(1, std::memory_order_relaxed);
        const reflect::Type* t = registry().typeOfLive(entity); // just made by the game
        if (!t) {
            g_unknown.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // GridNode's creator hooks ran inside the call (the creator's vtable +8 is theirs): what they saw is
        // tracked by now.
        const bool seen = inspector::alive(reinterpret_cast<uintptr_t>(entity), t);
        if (!seen) g_missed.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_m);
        Count& c = g_byClass[t->name];
        ++c.made;
        if (!seen) ++c.missed;
    });
    return entity;
}

std::string report() {
    std::vector<std::pair<std::string, Count>> v;
    {
        std::lock_guard<std::mutex> lock(g_m);
        v.assign(g_byClass.begin(), g_byClass.end());
    }
    char b[256];
    snprintf(b, sizeof(b), "census: %llu calls, %llu entities made (%llu returned none), %llu not seen by GridNode's creator hooks, %llu of unknown type",
             static_cast<unsigned long long>(g_calls.load()), static_cast<unsigned long long>(g_made.load()), static_cast<unsigned long long>(g_skipped.load()),
             static_cast<unsigned long long>(g_missed.load()), static_cast<unsigned long long>(g_unknown.load()));
    std::string out = b;
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& c) { return a.second.missed != c.second.missed ? a.second.missed > c.second.missed : a.second.made > c.second.made; });
    int shown = 0;
    for (const auto& [name, c] : v) {
        if (shown++ >= 15) break;
        snprintf(b, sizeof(b), "\n  %s: %llu made, %llu not seen", name.c_str(), static_cast<unsigned long long>(c.made), static_cast<unsigned long long>(c.missed));
        out += b;
    }
    return out;
}

} // namespace

// "census on": the detour in (it stays until GridNode unloads); "census": the counts, also logged;
// "census reset": counts back to 0.
std::string censusCommand(const std::string& rest) {
    const size_t a = rest.find_first_not_of(' ');
    const std::string arg = a == std::string::npos ? std::string() : rest.substr(a);
    if (!_strnicmp(arg.c_str(), "on", 2)) {
        if (g_on.load()) return "census: already on";
        void* original = nullptr;
        const int r = hookFunction("GridNode", e::function::entityCreator_spawnEntityWithContext, reinterpret_cast<void*>(&spawnHook), &original);
        if (r != GN_OK) return "census: the creation function could not be hooked (" + std::to_string(r) + ")";
        g_original = reinterpret_cast<SpawnFn>(original);
        g_on = true;
        GN_LOG("census: counting entity creations (entityCreator_spawnEntityWithContext)");
        return "census: on. Load a level or stream districts, then type census";
    }
    if (!_strnicmp(arg.c_str(), "reset", 5)) {
        g_calls = g_made = g_skipped = g_missed = g_unknown = 0;
        std::lock_guard<std::mutex> lock(g_m);
        g_byClass.clear();
        return "census: counts reset";
    }
    if (!g_on.load()) return "census on: count what the game's creation function makes against what GridNode's creator hooks see";
    const std::string r = report();
    GN_LOG("%s", r.c_str());
    return r;
}

} // namespace gn::host
