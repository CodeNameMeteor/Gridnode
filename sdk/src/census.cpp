// The creation census (console "census"): counts, from entities.cpp's two ways of seeing creations (the creation
// function entityCreator_spawnEntityWithContext, 0x143167120, and the pool of creator slots), what each one
// missed: entities the slots did not pass on (the creation hook does since API 41) and entities made outside
// the creation function (only the slots see them). If the second stays at 0, the pool can go. Off unless asked.

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

std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_calls{0}, g_made{0}, g_missed{0}, g_unknown{0}, g_poolOnly{0};
std::mutex g_m;
struct Count {
    uint64_t made = 0, missed = 0, poolOnly = 0;
};
std::map<std::string, Count> g_byClass; // (g_m)

} // namespace

// From the creation hook (entities.cpp), after the game's call: poolSaw = a creator slot passed it on.
void censusSpawn(void* entity, const reflect::Type* t, bool poolSaw) {
    if (!g_on.load(std::memory_order_relaxed) || !entity) return;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_made.fetch_add(1, std::memory_order_relaxed);
    if (!t) {
        g_unknown.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!poolSaw) g_missed.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_m);
    Count& c = g_byClass[t->name];
    ++c.made;
    if (!poolSaw) ++c.missed;
}

// From a creator slot outside the creation function: an entity only the pool sees.
void censusPoolOnly(const reflect::Type* t) {
    if (!g_on.load(std::memory_order_relaxed) || !t) return;
    g_poolOnly.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_m);
    ++g_byClass[t->name].poolOnly;
}

namespace {

std::string report() {
    std::vector<std::pair<std::string, Count>> v;
    {
        std::lock_guard<std::mutex> lock(g_m);
        v.assign(g_byClass.begin(), g_byClass.end());
    }
    char b[320];
    snprintf(b, sizeof(b),
             "census: %llu made by the creation function, %llu of them missed by the creator slots (passed on by the creation hook since API 41), "
             "%llu made outside it (only the creator slots saw them), %llu of unknown type",
             static_cast<unsigned long long>(g_made.load()), static_cast<unsigned long long>(g_missed.load()),
             static_cast<unsigned long long>(g_poolOnly.load()), static_cast<unsigned long long>(g_unknown.load()));
    std::string out = b;
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& c) {
        const uint64_t x = a.second.missed + a.second.poolOnly, y = c.second.missed + c.second.poolOnly;
        return x != y ? x > y : a.second.made > c.second.made;
    });
    int shown = 0;
    for (const auto& [name, c] : v) {
        if (shown++ >= 15) break;
        snprintf(b, sizeof(b), "\n  %s: %llu made, %llu missed by the slots, %llu outside the creation function", name.c_str(),
                 static_cast<unsigned long long>(c.made), static_cast<unsigned long long>(c.missed), static_cast<unsigned long long>(c.poolOnly));
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
        if (!spawnHookIn()) return "census: the creation function is not hooked (see gridnode.log)";
        g_on = true;
        GN_LOG("census: counting entity creations (entityCreator_spawnEntityWithContext)");
        return "census: on. Load a level or stream districts, then type census";
    }
    if (!_strnicmp(arg.c_str(), "dispatch", 8)) {
        const bool on = arg.find("off") == std::string::npos;
        spawnDispatch(on);
        GN_LOG("census: the creation hook %s the entities the creator slots missed", on ? "passes on" : "no longer passes on");
        return std::string("census: entities the creator slots missed are ") + (on ? "passed on to on_created listeners" : "no longer passed on (as before API 41)") +
               " (" + std::to_string(spawnCaught()) + " passed on so far)";
    }
    if (!_strnicmp(arg.c_str(), "reset", 5)) {
        g_calls = g_made = g_missed = g_unknown = g_poolOnly = 0;
        std::lock_guard<std::mutex> lock(g_m);
        g_byClass.clear();
        return "census: counts reset";
    }
    if (!g_on.load()) return "census on: count what the game's creation function makes against what GridNode's creator slots see, both ways";
    const std::string r = report();
    GN_LOG("%s", r.c_str());
    return r;
}

} // namespace gn::host
