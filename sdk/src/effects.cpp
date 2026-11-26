// Effects (particles, sparks, smoke: an EffectBlueprint asset) spawned, moved and stopped through the client's
// effect manager, as the game's own code does (EffectManager_spawn / _move / _stop / _isDone, db/annotations/
// render.yaml; read in Ghidra 2026-11-14 from a modder's notes). The manager is a command queue: these only queue,
// its update does the work. API 41, untested in game.

#include <windows.h>

#include <cctype>
#include <map>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
namespace k = gn::engine::constant;

struct Effect {
    uint32_t handle;
    uintptr_t owner; // the caller's return address (its mod)
    int group;       // the resource group holding its blueprint (-1: not found), watched each client tick
};
std::mutex g_m;
std::vector<Effect> g_effects; // (g_m) spawned and not stopped by us; done ones are dropped as they are seen
constexpr size_t kMaxEffects = 1024;

uintptr_t manager() { return mem::readOr<uintptr_t>(mem::rva(e::global::EffectManager)); }

bool finiteTransform(const gn_transform& t) {
    const float* f = &t.right[0];
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(f[i])) return false;
    return true;
}

// The level an effect belongs to, as the game passes it: EntityBus_getLevel(bus) = *(*bus + 0x38), with the bus an
// entity's +0x28 (ClientStaticModelGroup's effect call 0x143185490 passes *(group + 0x28); a spawner's +0x28 is its bus
// too). Faith's client character is the entity used. (The first try took the bus interface at +0x20 minus 8, whose
// first word is a vtable: "not spawned" in game, 2026-11-14.)
uintptr_t level() {
    const uintptr_t faith = reinterpret_cast<uintptr_t>(localClientCharacter());
    const uintptr_t bus = faith ? mem::readOr<uintptr_t>(faith + 0x28) : 0;
    const uintptr_t inner = bus ? mem::readOr<uintptr_t>(bus) : 0;
    if (!inner || mem::inImage(inner)) return 0; // a vtable there: not a bus
    return mem::readOr<uintptr_t>(inner + 0x38);
}

void dropDone(uintptr_t mgr) { // g_m held
    using DoneFn = bool (*)(uintptr_t, uint32_t);
    const auto done = reinterpret_cast<DoneFn>(mem::rva(e::function::EffectManager_isDone));
    for (size_t i = g_effects.size(); i-- > 0;)
        if (done(mgr, g_effects[i].handle)) g_effects.erase(g_effects.begin() + static_cast<std::ptrdiff_t>(i));
}

} // namespace

int effectSpawn(const void* caller, const void* blueprint, const gn_transform* at, uint32_t* handle) {
    static const reflect::Type* bp = registry().find("EffectBlueprint");
    if (!blueprint || !at || !handle || !finiteTransform(*at)) return GN_ERR_ARGUMENT;
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(blueprint));
    if (!t || !bp || !registry().isKindOf(t, bp)) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    const uintptr_t mgr = manager();
    const uintptr_t lvl = level();
    // Why it is refused, once each (game test 2026-11-14: "not spawned (5)" with nothing to tell the cases apart).
    static std::atomic<int> said{0};
    auto why = [&](const char* text) {
        if (said.fetch_add(1) < 8)
            GN_LOG("effect: not spawned: %s (manager %llx, level %llx, level +0x30 %02x, +0x9c %02x)", text, static_cast<unsigned long long>(mgr),
                   static_cast<unsigned long long>(lvl), lvl ? mem::readOr<uint8_t>(lvl + 0x30) : 0, lvl ? mem::readOr<uint8_t>(lvl + 0x9c) : 0);
        return GN_ERR_UNAVAILABLE;
    };
    if (!mgr) return why("no effect manager");
    if (!lvl) return why("no level from Faith's bus");
    if (mem::readOr<uint8_t>(lvl + 0x30, 1) & 1) return why("the level's +0x30 bit 0 is set (the manager's own refusal)");
    alignas(16) gn_transform tr = *at;
    tr.right[3] = tr.up[3] = tr.forward[3] = 0.f, tr.trans[3] = 1.f;
    alignas(16) const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    using SpawnFn = uint32_t (*)(uintptr_t, const void*, const gn_transform*, uintptr_t, uint32_t, const float*, const float*, uint8_t);
    std::lock_guard<std::mutex> lock(g_m);
    if (g_effects.size() >= kMaxEffects) dropDone(mgr);
    if (g_effects.size() >= kMaxEffects) return GN_ERR_FULL;
    const uint32_t h = reinterpret_cast<SpawnFn>(mem::rva(e::function::EffectManager_spawn))(mgr, blueprint, &tr, lvl, 0, zero, zero, 0);
    if (!h) return why("EffectManager_spawn returned 0 (with level +0x9c set it asks a distance or count check first)");
    // A looping effect outlives the district whose bundle holds its blueprint: when that group goes, the blueprint is
    // freed under the running effect (game test 2026-11-14: a crash in the effect update after a teleport away). The
    // group is found once here and watched each client tick.
    g_effects.push_back({h, reinterpret_cast<uintptr_t>(caller), assetGroupOf(reinterpret_cast<uintptr_t>(blueprint))});
    *handle = h;
    return GN_OK;
}

void effectsClientTick() {
    std::vector<uint32_t> stop;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_effects.empty()) return;
        const uintptr_t table = mem::rva(e::global::ResourceGroups);
        for (size_t i = g_effects.size(); i-- > 0;) {
            const int grp = g_effects[i].group;
            if (grp >= 0 && mem::readOr<uintptr_t>(table + uint64_t(grp) * 8)) continue;
            if (grp < 0) continue; // not found at spawn: kept (a blueprint outside the groups GridNode can see)
            stop.push_back(g_effects[i].handle);
            g_effects.erase(g_effects.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    const uintptr_t mgr = manager();
    for (uint32_t h : stop) {
        if (mgr) reinterpret_cast<void (*)(uintptr_t, uint32_t)>(mem::rva(e::function::EffectManager_stop))(mgr, h);
        GN_LOG("effect %u stopped: the bundle holding its blueprint went", h);
    }
}

int effectMove(uint32_t handle, const gn_transform* at) {
    if (!handle || !at || !finiteTransform(*at)) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    const uintptr_t mgr = manager();
    if (!mgr) return GN_ERR_UNAVAILABLE;
    alignas(16) gn_transform tr = *at;
    tr.right[3] = tr.up[3] = tr.forward[3] = 0.f, tr.trans[3] = 1.f;
    using MoveFn = void (*)(uintptr_t, uint32_t, const gn_transform*);
    reinterpret_cast<MoveFn>(mem::rva(e::function::EffectManager_move))(mgr, handle, &tr);
    return GN_OK;
}

int effectStop(uint32_t handle) {
    if (!handle) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    const uintptr_t mgr = manager();
    if (!mgr) return GN_ERR_UNAVAILABLE;
    reinterpret_cast<void (*)(uintptr_t, uint32_t)>(mem::rva(e::function::EffectManager_stop))(mgr, handle);
    std::lock_guard<std::mutex> lock(g_m);
    for (size_t i = g_effects.size(); i-- > 0;)
        if (g_effects[i].handle == handle) g_effects.erase(g_effects.begin() + static_cast<std::ptrdiff_t>(i));
    return GN_OK;
}

int effectAlive(uint32_t handle) {
    const uintptr_t mgr = manager();
    if (!handle || !mgr) return 0;
    return reinterpret_cast<bool (*)(uintptr_t, uint32_t)>(mem::rva(e::function::EffectManager_isDone))(mgr, handle) ? 0 : 1;
}

// A mod's effects stopped when it unloads (all of them at GridNode's unload): queued on the client thread.
void forgetEffects(const CodeRange& r, bool all) {
    std::vector<uint32_t> stop;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (size_t i = g_effects.size(); i-- > 0;)
            if (all || r.has(reinterpret_cast<void*>(g_effects[i].owner))) {
                stop.push_back(g_effects[i].handle);
                g_effects.erase(g_effects.begin() + static_cast<std::ptrdiff_t>(i));
            }
    }
    if (stop.empty()) return;
    client().post([stop] {
        const uintptr_t mgr = manager();
        if (!mgr) return;
        for (uint32_t h : stop) reinterpret_cast<void (*)(uintptr_t, uint32_t)>(mem::rva(e::function::EffectManager_stop))(mgr, h);
    });
}

namespace {
std::string assetNameOf(uintptr_t obj) {
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), obj);
    reflect::FieldInfo fi;
    if (!t || !registry().findField(t, "Name", fi) || !fi.type || fi.type->name != "CString") return {};
    std::string out;
    const uintptr_t p = mem::readOr<uintptr_t>(obj + fi.field->offset);
    return p && mem::readCString(p, out, 260) ? out : std::string();
}

// "effect list [TEXT]": the loaded EffectBlueprints. Nothing points from an effect entity or its data to its
// blueprint (the blueprint's Object points to the EffectEntityData; game test 2026-11-14: 0 found that way) and the
// data registry does not hold them, so the game's private memory is scanned once for the blueprint's vtable (a
// few seconds), as the inspector's memory pass does; each hit is checked with the type registry.
std::string effectList(std::string filter) {
    static const reflect::Type* bpType = registry().find("EffectBlueprint");
    if (!bpType || bpType->vtables.empty()) return "effect list: EffectBlueprint's vtable is not known";
    for (char& ch : filter) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    const ULONGLONG start = GetTickCount64();
    std::vector<uint64_t> vts(bpType->vtables.begin(), bpType->vtables.end());
    std::map<uintptr_t, std::string> found;
    std::vector<uint64_t> buf(1u << 17);
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = 0x10000; a < 0x7ff000000000ull && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));
         a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD)) continue;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = begin + mbi.RegionSize;
        for (uintptr_t p = begin; p < end; p += buf.size() * 8) {
            const size_t n = static_cast<size_t>(std::min<uintptr_t>(buf.size() * 8, end - p));
            if (!mem::read(p, buf.data(), n)) continue;
            for (size_t k = 0; k < n / 8; ++k)
                for (uint64_t vt : vts)
                    if (buf[k] == vt) {
                        const uintptr_t obj = p + k * 8;
                        const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), obj);
                        if (t && registry().isKindOf(t, bpType) && found.size() < 20000) found.emplace(obj, assetNameOf(obj));
                    }
        }
        if (GetTickCount64() - start > 15000) break;
    }
    std::string out;
    size_t shown = 0, matching = 0;
    for (const auto& [bp, name] : found) {
        std::string low = name;
        for (char& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        if (!filter.empty() && low.find(filter) == std::string::npos) continue;
        ++matching;
        if (shown >= 30) continue;
        ++shown;
        char b[320];
        snprintf(b, sizeof(b), "\n  0x%llx %s", static_cast<unsigned long long>(bp), name.c_str());
        out += b;
    }
    char head[200];
    snprintf(head, sizeof(head), "effect list: %llu effect blueprints in memory (%llu ms)%s%llu match%s", static_cast<unsigned long long>(found.size()),
             static_cast<unsigned long long>(GetTickCount64() - start), filter.empty() ? "; " : ", with the text: ", static_cast<unsigned long long>(matching),
             shown < matching ? " (the first 30):" : ":");
    return head + out;
}
} // namespace

// Console: "effect NAME" (an EffectBlueprint's asset path) spawns it 3 m ahead of Faith; "effect stop" stops the
// console's. Runs on the client thread (the console waits for it).
std::string effectCommand(const std::string& rest) {
    const size_t a = rest.find_first_not_of(' ');
    const std::string arg = a == std::string::npos ? std::string() : rest.substr(a, rest.find_last_not_of(' ') - a + 1);
    if (arg.empty()) return "effect list [TEXT] (the level's effects) | effect 0xADDRESS | effect ASSET_PATH; effect stop";
    if (!_strnicmp(arg.c_str(), "list", 4)) return effectList(arg.size() > 5 ? arg.substr(5) : std::string());
    if (!_stricmp(arg.c_str(), "stop")) {
        forgetEffects(CodeRange{reinterpret_cast<uintptr_t>(&effectCommand), reinterpret_cast<uintptr_t>(&effectCommand) + 1}, false);
        return "effect: the console's effects stopped";
    }
    // An address (from "data EffectBlueprint") or an asset path.
    void* asset = nullptr;
    bool found = false;
    if (arg.size() > 2 && arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
        asset = reinterpret_cast<void*>(static_cast<uintptr_t>(strtoull(arg.c_str(), nullptr, 16)));
    } else {
        asset = assetFind(arg.c_str(), nullptr);
        found = asset != nullptr;
        if (!asset) return "effect: no loaded asset " + arg + " (effect list [TEXT] lists the level's effects)";
    }
    const std::string out = runOnRealmAndWait(false, [asset, arg]() -> std::string {
        alignas(16) gn_transform at{};
        void* faith = localClientCharacter();
        if (!faith || !entityTransform(faith, at)) return "effect: no Faith on the client";
        for (int i = 0; i < 3; ++i) at.trans[i] += at.forward[i] * 3.f + at.up[i] * 1.f;
        uint32_t h = 0;
        const int r = effectSpawn(reinterpret_cast<const void*>(&effectCommand), asset, &at, &h);
        char b[160];
        snprintf(b, sizeof(b), r == GN_OK ? "effect: spawned (handle %u) 3 m ahead" : "effect: not spawned (%d%s)", r == GN_OK ? h : static_cast<unsigned>(r),
                 r == GN_ERR_ARGUMENT ? ": not an EffectBlueprint" : "");
        GN_LOG("%s: %s", b, arg.c_str());
        return b;
    });
    if (found) assetRelease(asset);
    return out;
}

} // namespace gn::host
