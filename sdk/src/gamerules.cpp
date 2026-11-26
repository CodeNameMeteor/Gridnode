// Two of the game's rules a mod can change (API 41): fall damage, and kill volumes one by one.
// - Fall damage (below): Faith's fall damage passes only from drops of at least a held distance.
// - Kill volumes: one switched off leaves the kill volume manager's list, as when the level turns it off.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <algorithm>
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;
namespace k = gn::engine::constant;

// ---- fall damage ------------------------------------------------------------------------------------
// The damage a landing does comes from Faith's animation graph: an ANT DamageProcessorAsset (update 0x142f26cc0)
// builds a damage record from authored values and the client health component's ClientHealth_applyAntDamage
// (0x1439196b0) turns it into a PamClientDamageInfo, type Fall when the record's damage type value (+0x28, its +0x1c)
// is 1, and sends it on to the server. No C++ compares a fall height (Gemini block J, checked in Ghidra 2026-11-14);
// RayCastCollisionProcessor_update, the first guess, never runs for Faith (game test 2026-11-14). So GridNode
// measures the drop itself (Faith's highest point since she last stood still on the ground, each client tick) and
// lets the fall damage through only from drops of at least the held distance.
using ApplyAntDamageFn = void (*)(void* healthComponent, const void* damage);
ApplyAntDamageFn g_fallOrig = nullptr; // written by the hook table
std::atomic<bool> g_fallHooked{false};
std::atomic<float> g_fall{-1.f}; // the held distance, < 0: the game's (fall damage passes as it comes)
std::atomic<uintptr_t> g_fallOwner{0};
std::atomic<uintptr_t> g_faithHealth{0}; // Faith's client health component (client tick)
std::atomic<float> g_top{0.f}, g_y{0.f}; // her highest point in the current fall, and where she is now
std::atomic<bool> g_topValid{false};
std::atomic<uint32_t> g_fallSeenN{0}, g_fallSpared{0};
int g_groundTicks = 0; // client thread only

constexpr uint64_t kDamage_TypeValue = 0x28; // the record's damage type value (an enum asset; its +0x1c the value)
constexpr uint64_t kTypeValue_Value = 0x1c;
constexpr int32_t kDamageType_Fall = 1;      // -> PamDamageType_Fall (0x68)

void applyAntDamageHook(void* component, const void* damage) {
    bool spare = false;
    if (!unloaded() && component && damage)
        guardHook("fall damage", [&] {
            const uintptr_t tv = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(damage) + kDamage_TypeValue);
            if (!tv || mem::readOr<int32_t>(tv + kTypeValue_Value, -1) != kDamageType_Fall) return;
            const bool faith = reinterpret_cast<uintptr_t>(component) == g_faithHealth.load(std::memory_order_relaxed);
            const float amount = mem::readOr<float>(reinterpret_cast<uintptr_t>(damage), 0.f);
            const float drop = g_topValid.load() ? g_top.load() - g_y.load() : -1.f;
            const float hold = g_fall.load();
            spare = faith && hold >= 0.f && drop >= 0.f && drop < hold; // no measurement: the game decides
            if (spare) g_fallSpared.fetch_add(1, std::memory_order_relaxed);
            if (g_fallSeenN.fetch_add(1, std::memory_order_relaxed) < 200)
                GN_LOG("rules: fall damage %.1f to %s health component %llx; drop %.1f m%s", amount, faith ? "Faith's" : "another",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(component)), drop,
                       spare ? " (spared: under the held distance)" : (hold >= 0.f && faith ? " (passed: at or over the held distance)" : ""));
        });
    if (!spare) g_fallOrig(component, damage);
}

// A pointer swap on PamClientHumanHealthComponent's slot (the player's component; the AI's calls the function
// directly), not a code patch: the function detour of api41m worked in game (2026-11-14) and showed only Faith's
// component taking fall damage through this path.
bool installFallHook() {
    if (g_fallHooked.load()) return true;
    std::string why;
    const auto r = hooks().install("gridnode", mem::rva(e::hook::ClientHumanHealth_applyAntDamageSlot.slot), mem::rva(e::hook::ClientHumanHealth_applyAntDamageSlot.original),
                                   reinterpret_cast<uint64_t>(&applyAntDamageHook), reinterpret_cast<uint64_t*>(&g_fallOrig), &why);
    if (r != hook::Result::Ok) {
        GN_LOG("rules: the fall damage slot could not be hooked (%s)", why.c_str());
        return false;
    }
    g_fallHooked = true;
    return true;
}

void restoreFalls() {
    if (g_fall.exchange(-1.f) >= 0.f) GN_LOG("rules: fall damage as the game's (%u fall damages spared)", g_fallSpared.load());
}

// ---- kill volumes ------------------------------------------------------------------------------------
// The game's own way (Gemini block Q, read in Ghidra 2026-11-14): the kill volume manager tests only the volumes in
// its list, and a volume leaves it when its Enabled property turns off (EntityUpdateList_remove) and comes back when
// it turns on (EntityUpdateList_add). GridNode queues the same remove for a volume switched off, and the add when it
// is switched back on and the game still has it enabled (+0x62). No code is patched (until api41u: a detour on
// KillVolume_testCharacter, shared with the trainer's). The manager has no lock: all of it on the server thread.
struct Off {
    uintptr_t volume; // 0: all of them
    uintptr_t owner;
};
std::mutex g_killM;
std::vector<Off> g_off;                 // (g_killM) what is asked
std::atomic<int> g_offCount{0};         // g_off.size(), read without the lock
std::vector<uintptr_t> g_removed;       // server thread: volumes GridNode took out of the manager's list
std::atomic<uint32_t> g_removals{0};

uintptr_t killManager() { return mem::readOr<uintptr_t>(mem::rva(e::global::KillVolumeManager)); }

bool killVolumeAlive(uintptr_t v) {
    static const reflect::Type* kv = registry().find("PamServerKillVolumeEntity");
    const reflect::Type* t = v ? registry().typeOf(mem::ProcessMemory(), v) : nullptr;
    return t && kv && registry().isKindOf(t, kv) && inspector::alive(v, t);
}

// Server thread, each tick while anything is off or was taken out.
} // namespace

void killVolumesServerTick() {
    if (g_offCount.load(std::memory_order_relaxed) == 0 && g_removed.empty()) return;
    const uintptr_t mgr = killManager();
    if (!mgr) return;
    std::vector<Off> off;
    {
        std::lock_guard<std::mutex> lock(g_killM);
        off = g_off;
    }
    const auto wanted = [&](uintptr_t v) {
        for (const Off& o : off)
            if (!o.volume || o.volume == v) return true;
        return false;
    };
    const auto remove = reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(mem::rva(e::function::EntityUpdateList_remove));
    const auto add = reinterpret_cast<void (*)(uintptr_t, uintptr_t, uint16_t)>(mem::rva(e::function::EntityUpdateList_add));
    // Out: every listed (or queued-in) volume asked off. The remove only queues when it is in or coming in.
    const uintptr_t b0 = mem::readOr<uintptr_t>(mgr + 0x28), b1 = mem::readOr<uintptr_t>(mgr + 0x30);
    const uintptr_t q0 = mem::readOr<uintptr_t>(mgr + 0x48), q1 = mem::readOr<uintptr_t>(mgr + 0x50);
    std::vector<uintptr_t> candidates;
    for (uintptr_t p = b0; b1 > b0 && p < b1 && candidates.size() < 4096; p += 0x10) candidates.push_back(mem::readOr<uintptr_t>(p));
    for (uintptr_t p = q0; q1 > q0 && p < q1 && candidates.size() < 8192; p += 0x10)
        if (mem::readOr<int32_t>(p + 0xc) == 1) candidates.push_back(mem::readOr<uintptr_t>(p));
    for (uintptr_t v : candidates) {
        if (!v || !wanted(v)) continue;
        remove(mgr, v);
        if (std::find(g_removed.begin(), g_removed.end(), v) == g_removed.end()) {
            g_removed.push_back(v);
            g_removals.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Back in: ones no longer asked off, if they still live and the game still has them enabled.
    for (size_t i = g_removed.size(); i-- > 0;) {
        const uintptr_t v = g_removed[i];
        if (wanted(v)) continue;
        if (killVolumeAlive(v) && mem::readOr<uint8_t>(v + k::KillVolume_EnabledByGame)) add(mgr, v, 1);
        g_removed.erase(g_removed.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

namespace {

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
    GN_LOG("rules: Faith's fall damage passes only from drops of %.1f m or more", metres);
    return GN_OK;
}

// Client thread: Faith's health component and her highest point in the current fall. The top is reset once she has
// been on the ground for 30 ticks (half a second): the damage comes at the landing, a few ticks after touching down.
void rulesClientTick() {
    if (!g_fallHooked.load(std::memory_order_relaxed)) return;
    void* faith = localClientCharacter();
    gn_transform tr{};
    gn_movement mv{};
    if (!faith || !entityTransform(faith, tr) || movementGet(&mv) != GN_OK) {
        g_topValid = false;
        g_faithHealth = 0;
        return;
    }
    static const reflect::Type* hc = registry().find("PamClientHumanHealthComponent");
    g_faithHealth = hc ? reinterpret_cast<uintptr_t>(getComponent(faith, reinterpret_cast<const gn_type*>(hc))) : 0;
    const float y = tr.trans[1];
    g_y = y;
    const bool inAir = mv.on_ground != 0; // 1 while in the air (gn_movement's historical name)
    g_groundTicks = inAir ? 0 : g_groundTicks + 1;
    if (!g_topValid.load() || g_groundTicks > 30) {
        g_top = y;
        g_topValid = true;
    } else if (y > g_top.load()) {
        g_top = y;
    }
}

std::string fallLog() {
    if (!installFallHook()) return "fall: the fall damage path could not be hooked (see gridnode.log)";
    return "fall: logging every fall damage to gridnode.log (" + std::to_string(g_fallSeenN.load()) + " so far)";
}

int killVolumeSet(const void* caller, void* volume, int enabled) {
    if (volume && !killVolumeAlive(reinterpret_cast<uintptr_t>(volume))) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_killM);
    const uintptr_t v = reinterpret_cast<uintptr_t>(volume);
    for (size_t i = g_off.size(); i-- > 0;)
        if (g_off[i].volume == v) g_off.erase(g_off.begin() + static_cast<std::ptrdiff_t>(i));
    if (!enabled) {
        if (g_off.size() >= 4096) return GN_ERR_FULL;
        g_off.push_back({v, reinterpret_cast<uintptr_t>(caller)});
    }
    g_offCount = static_cast<int>(g_off.size());
    GN_LOG("rules: kill volume %llx %s (%d off; %u taken out of the game's list so far)", static_cast<unsigned long long>(v), enabled ? "on" : "off",
           g_offCount.load(), g_removals.load());
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
        snprintf(b, sizeof(b), "fall: %s; %u fall damages seen, %u spared (fall METRES | fall off | fall log)",
                 f < 0.f ? "the game's fall damage" : (std::to_string(f) + " m held").c_str(), g_fallSeenN.load(), g_fallSpared.load());
        return b;
    }
    if (!_stricmp(arg.c_str(), "log")) return fallLog();
    const float m = !_stricmp(arg.c_str(), "off") ? -1.f : strtof(arg.c_str(), nullptr);
    const int r = fallThresholdSet(reinterpret_cast<const void*>(&fallCommand), m);
    return r == GN_OK ? (m < 0.f ? "fall: the game's fall damage" : "fall: Faith's fall damage only from drops of " + std::to_string(m) + " m or more") : "fall: error " + std::to_string(r);
}

std::string killVolumeCommand(const std::string& rest) {
    char word[16] = {};
    unsigned long long addr = 0;
    const int n = sscanf(rest.c_str(), " %15s %llx", word, &addr);
    if (n < 1 || (_stricmp(word, "off") && _stricmp(word, "on")))
        return "killvol off [ADDRESS] | killvol on [ADDRESS] (no address: every kill volume); " + std::to_string(g_removals.load()) + " taken out of the game's list so far";
    const int r = killVolumeSet(reinterpret_cast<const void*>(&killVolumeCommand), reinterpret_cast<void*>(static_cast<uintptr_t>(n > 1 ? addr : 0)), !_stricmp(word, "on"));
    if (r != GN_OK) return "killvol: error " + std::to_string(r) + (r == GN_ERR_ARGUMENT ? " (not a PamServerKillVolumeEntity)" : "");
    return std::string("killvol: ") + (n > 1 ? "that kill volume" : "every kill volume") + (_stricmp(word, "on") ? " off" : " back on (as far as GridNode goes)");
}

} // namespace gn::host
