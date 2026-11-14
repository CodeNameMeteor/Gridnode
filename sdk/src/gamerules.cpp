// Two of the game's rules a mod can change (API 41): the fall that kills, and kill volumes one by one.
// - The fall check is ANT's RayCastCollisionProcessorAsset: HasFallenPastThreshold = -FallThreshold (+0x40, 10 m
//   by default) > the distance fallen (RayCastCollisionProcessor_update, read in Ghidra 2026-11-03 and -14). A
//   hold is written into each asset as its update runs (a detour on the update), and each asset's own value is
//   put back when the hold goes. The assets are shared: every character using one falls by it.
// - A kill volume tests each character in it (KillVolume_testCharacter, returns "inside" in al); a volume
//   switched off answers "not inside". The trainer's own "kill volumes off" hooks the same function (both stack).

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;
namespace k = gn::engine::constant;

// ---- the fall threshold ----------------------------------------------------------------------------
using FallUpdateFn = void (*)(void* asset, void* updateInfo);
FallUpdateFn g_fallOrig = nullptr; // written by the hook table
std::atomic<bool> g_fallHooked{false};
std::atomic<float> g_fall{-1.f};    // the held threshold, < 0: none
std::atomic<uintptr_t> g_fallOwner{0};
std::mutex g_fallM;
std::unordered_map<uintptr_t, float> g_fallSeen; // asset -> its own value (g_fallM)

// ANT's update thread(s): the held value into the asset before the game reads it.
void fallUpdateHook(void* asset, void* info) {
    if (!unloaded() && g_fall.load(std::memory_order_relaxed) >= 0.f)
        guardHook("fall threshold", [&] {
            const uintptr_t a = reinterpret_cast<uintptr_t>(asset);
            float* at = reinterpret_cast<float*>(a + k::RayCastCollisionProcessorAsset_FallThreshold);
            if (*at == g_fall.load(std::memory_order_relaxed)) return; // the usual case: nothing to take
            std::lock_guard<std::mutex> lock(g_fallM);
            const float hold = g_fall.load(); // again under the lock: a release may have put the value back
            if (hold < 0.f) return;
            if (g_fallSeen.size() < 4096 && g_fallSeen.emplace(a, *at).second)
                GN_LOG("rules: fall threshold %.1f m written into the fall check asset %llx (it had %.1f)", hold, static_cast<unsigned long long>(a), *at);
            *at = hold;
        });
    g_fallOrig(asset, info);
}

// A function detour, not the vtable slot: with the slot swapped the hold was never written (game test 2026-11-14:
// no "written into" line), so the game does not reach the update through that slot.
bool installFallHook() {
    if (g_fallHooked.load()) return true;
    const int r = hookFunction("GridNode", e::function::RayCastCollisionProcessor_update, reinterpret_cast<void*>(&fallUpdateHook),
                               reinterpret_cast<void**>(&g_fallOrig));
    if (r != GN_OK) {
        GN_LOG("rules: the fall check could not be hooked (%d)", r);
        return false;
    }
    g_fallHooked = true;
    return true;
}

// Each asset's own value back, if it is still that asset.
void restoreFalls() {
    std::lock_guard<std::mutex> lock(g_fallM);
    g_fall = -1.f;
    const uintptr_t vt = mem::rva(k::RayCastCollisionProcessorAsset_vtable);
    size_t n = 0;
    for (const auto& [a, v] : g_fallSeen)
        if (mem::readOr<uintptr_t>(a) == vt && mem::write(a + k::RayCastCollisionProcessorAsset_FallThreshold, v)) ++n;
    if (!g_fallSeen.empty()) GN_LOG("rules: fall threshold back to the game's on %llu asset(s)", static_cast<unsigned long long>(n));
    g_fallSeen.clear();
}

// ---- kill volumes ------------------------------------------------------------------------------------
using KillTestFn = bool (*)(void* volume, void* character);
KillTestFn g_killOrig = nullptr;
std::atomic<bool> g_killHooked{false};
struct Off {
    uintptr_t volume; // 0: all of them
    uintptr_t owner;
};
std::mutex g_killM;
std::vector<Off> g_off;               // (g_killM)
std::atomic<int> g_offCount{0};       // g_off.size(), read without the lock
std::atomic<uint32_t> g_spared{0};

bool killTestHook(void* volume, void* character) {
    if (g_offCount.load(std::memory_order_relaxed) > 0 && !unloaded()) {
        bool off = false;
        {
            std::lock_guard<std::mutex> lock(g_killM);
            for (const Off& o : g_off)
                if (!o.volume || o.volume == reinterpret_cast<uintptr_t>(volume)) off = true;
        }
        if (off) {
            g_spared.fetch_add(1, std::memory_order_relaxed);
            return false; // not inside
        }
    }
    return g_killOrig(volume, character);
}

bool installKillHook() {
    if (g_killHooked.load()) return true;
    // The hook table keeps writing the original into g_killOrig (another mod's hook below ours can come and go: the
    // trainer hooks this function too), so it gets the variable itself, never a local copy (the crash of 2026-11-14).
    const int r = hookFunction("GridNode", e::function::KillVolume_testCharacter, reinterpret_cast<void*>(&killTestHook), reinterpret_cast<void**>(&g_killOrig));
    if (r != GN_OK) {
        GN_LOG("rules: the kill volume test could not be hooked (%d)", r);
        return false;
    }
    g_killHooked = true;
    return true;
}

} // namespace

int fallThresholdSet(const void* caller, float metres) {
    if (!std::isfinite(metres) || metres > 100000.f) return GN_ERR_ARGUMENT;
    if (metres < 0.f) {
        restoreFalls();
        return GN_OK;
    }
    if (!installFallHook()) return GN_ERR_UNAVAILABLE;
    g_fallOwner = reinterpret_cast<uintptr_t>(caller);
    g_fall = metres;
    GN_LOG("rules: fall threshold held at %.1f m (the game's default is 10)", metres);
    return GN_OK;
}

int killVolumeSet(const void* caller, void* volume, int enabled) {
    if (volume) {
        static const reflect::Type* kv = registry().find("PamServerKillVolumeEntity");
        const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(volume));
        if (!t || !kv || !registry().isKindOf(t, kv)) return GN_ERR_ARGUMENT;
    }
    if (!installKillHook()) return GN_ERR_UNAVAILABLE;
    std::lock_guard<std::mutex> lock(g_killM);
    const uintptr_t v = reinterpret_cast<uintptr_t>(volume);
    for (size_t i = g_off.size(); i-- > 0;)
        if (g_off[i].volume == v) g_off.erase(g_off.begin() + static_cast<std::ptrdiff_t>(i));
    if (!enabled) {
        if (g_off.size() >= 4096) return GN_ERR_FULL;
        g_off.push_back({v, reinterpret_cast<uintptr_t>(caller)});
    }
    g_offCount = static_cast<int>(g_off.size());
    GN_LOG("rules: kill volume %llx %s (%d off; %u tests answered 'not inside' so far)", static_cast<unsigned long long>(v), enabled ? "on" : "off",
           g_offCount.load(), g_spared.load());
    return GN_OK;
}

void forgetRules(const CodeRange& r) {
    if (g_fall.load() >= 0.f && r.has(reinterpret_cast<void*>(g_fallOwner.load()))) restoreFalls();
    std::lock_guard<std::mutex> lock(g_killM);
    for (size_t i = g_off.size(); i-- > 0;)
        if (r.has(reinterpret_cast<void*>(g_off[i].owner))) g_off.erase(g_off.begin() + static_cast<std::ptrdiff_t>(i));
    g_offCount = static_cast<int>(g_off.size());
}

void rulesRelease() {
    restoreFalls();
    std::lock_guard<std::mutex> lock(g_killM);
    g_off.clear();
    g_offCount = 0;
}

// Console: "fall [METRES | off]", "killvol off [ADDRESS] | on [ADDRESS]".
std::string fallCommand(const std::string& rest) {
    const size_t a = rest.find_first_not_of(' ');
    const std::string arg = a == std::string::npos ? std::string() : rest.substr(a);
    if (arg.empty()) {
        const float f = g_fall.load();
        char b[96];
        snprintf(b, sizeof(b), "fall: %s (fall METRES | fall off)", f < 0.f ? "the game's threshold" : (std::to_string(f) + " m held").c_str());
        return b;
    }
    const float m = !_stricmp(arg.c_str(), "off") ? -1.f : strtof(arg.c_str(), nullptr);
    const int r = fallThresholdSet(reinterpret_cast<const void*>(&fallCommand), m);
    return r == GN_OK ? (m < 0.f ? "fall: the game's threshold" : "fall: held at " + std::to_string(m) + " m") : "fall: error " + std::to_string(r);
}

std::string killVolumeCommand(const std::string& rest) {
    char word[16] = {};
    unsigned long long addr = 0;
    const int n = sscanf(rest.c_str(), " %15s %llx", word, &addr);
    if (n < 1 || (_stricmp(word, "off") && _stricmp(word, "on")))
        return "killvol off [ADDRESS] | killvol on [ADDRESS] (no address: every kill volume); " + std::to_string(g_spared.load()) + " tests answered 'not inside' so far";
    const int r = killVolumeSet(reinterpret_cast<const void*>(&killVolumeCommand), reinterpret_cast<void*>(static_cast<uintptr_t>(n > 1 ? addr : 0)), !_stricmp(word, "on"));
    if (r != GN_OK) return "killvol: error " + std::to_string(r) + (r == GN_ERR_ARGUMENT ? " (not a PamServerKillVolumeEntity)" : "");
    return std::string("killvol: ") + (n > 1 ? "that kill volume" : "every kill volume") + (_stricmp(word, "on") ? " off" : " back on (as far as GridNode goes)");
}

} // namespace gn::host
