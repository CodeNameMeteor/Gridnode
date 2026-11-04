// Health, the map waypoint, the last checkpoint and the active mission (API 22). The paths are the
// user's Cheat Engine table's (db/annotations/gamestate.yaml); every object on the way is checked by
// its reflected type, and the first read of each logs what it found.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>
#include <cmath>
#include <cstdint>
#include <string>

#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;
namespace k = gn::engine::constant;

uintptr_t ptr(uintptr_t addr) { return addr > 0x10000 ? mem::readOr<uintptr_t>(addr) : 0; }
const reflect::Type* typeAt(uintptr_t p) { return p > 0x10000 ? registry().typeOf(mem::ProcessMemory(), p) : nullptr; }
const char* nameAt(uintptr_t p) {
    const reflect::Type* t = typeAt(p);
    return t ? t->name.c_str() : "?";
}
bool kindOf(uintptr_t p, const char* base) {
    const reflect::Type* t = typeAt(p);
    const reflect::Type* b = registry().find(base);
    return t && b && registry().isKindOf(t, b);
}

bool readVec(uintptr_t at, float out[3]) {
    float v[3];
    if (!at || !mem::read(at, v, sizeof(v))) return false;
    for (float f : v)
        if (!std::isfinite(f) || std::fabs(f) > 1e6f) return false;
    out[0] = v[0], out[1] = v[1], out[2] = v[2];
    return true;
}

// A server character's health component (getComponent by the base class: Faith's is a
// PamServerHumanHealthComponent, a KSEC's a PamServerAIHealthComponent).
uintptr_t healthComponent(void* character) {
    if (!kindOf(reinterpret_cast<uintptr_t>(character), "ServerCharacterEntity")) return 0;
    for (const char* name : {"PamServerHealthComponent", "ServerCharacterHealthComponent"}) {
        const reflect::Type* t = registry().find(name);
        if (!t) continue;
        const uintptr_t hc = reinterpret_cast<uintptr_t>(getComponent(character, reinterpret_cast<const gn_type*>(t)));
        if (hc && kindOf(hc, name)) return hc;
    }
    return 0;
}
} // namespace

int characterHealth(void* character, float* health, float* maxHealth) {
    if (!health && !maxHealth) return GN_ERR_ARGUMENT;
    if (!isServerThread()) return GN_ERR_THREAD;
    if (!kindOf(reinterpret_cast<uintptr_t>(character), "ServerCharacterEntity")) return GN_ERR_ARGUMENT;
    const uintptr_t hc = healthComponent(character);
    float v[2];
    if (!hc || !mem::read(hc + k::HealthComponent_Health, v, sizeof(v)) || !std::isfinite(v[0]) || !std::isfinite(v[1])) return GN_ERR_UNAVAILABLE;
    if (health) *health = v[0];
    if (maxHealth) *maxHealth = v[1];
    return GN_OK;
}

int characterSetHealth(void* character, float health) {
    if (!std::isfinite(health) || health < 0.f || health > 1e6f) return GN_ERR_ARGUMENT;
    if (!isServerThread()) return GN_ERR_THREAD;
    if (!kindOf(reinterpret_cast<uintptr_t>(character), "ServerCharacterEntity")) return GN_ERR_ARGUMENT;
    const uintptr_t hc = healthComponent(character);
    if (!hc) return GN_ERR_UNAVAILABLE;
    return mem::write(hc + k::HealthComponent_Health, &health, sizeof(health)) ? GN_OK : GN_ERR_UNAVAILABLE;
}

int waypoint(float* out) {
    if (!out) return GN_ERR_ARGUMENT;
    // Realm 0's updater (the client's); its first entity is the map.
    const uintptr_t updater = ptr(mem::rva(e::global::UIMapEntityUpdater));
    const uintptr_t map = ptr(updater + k::EntityUpdater_Entities);
    const uintptr_t holder = typeAt(map) && nameAt(map) == std::string("PamClientUIMapEntity") ? ptr(map + k::UIMap_Waypoint) : 0;
    static std::atomic<int> said{0};
    if (said.fetch_add(1) == 0)
        GN_LOG("waypoint: updater %llx, map %llx (%s), +0x128 -> %llx (%s)", static_cast<unsigned long long>(updater),
               static_cast<unsigned long long>(map), nameAt(map), static_cast<unsigned long long>(holder), nameAt(holder));
    float v[3];
    if (!holder || !readVec(holder + k::Waypoint_Position, v)) return GN_ERR_UNAVAILABLE;
    if (v[0] == 0.f && v[1] == 0.f && v[2] == 0.f) return GN_ERR_NOT_FOUND; // no marker set (assumed: not checked)
    out[0] = v[0], out[1] = v[1], out[2] = v[2];
    return GN_OK;
}

int checkpoint(float* out) {
    if (!out) return GN_ERR_ARGUMENT;
    // The table's path from the spawn updater's first entity; what the objects on the way are is
    // not known yet, so the first read logs their types.
    const uintptr_t updater = ptr(mem::rva(e::global::SpawnEntityUpdater));
    const uintptr_t spawn = ptr(updater + k::EntityUpdater_Entities);
    const bool isSpawn = kindOf(spawn, "ServerSpawnEntity");
    const uintptr_t a = isSpawn ? ptr(spawn + 0x28) : 0, b = ptr(a + 0x38), c = ptr(b + 0x158);
    static std::atomic<int> said{0};
    if (said.fetch_add(1) == 0)
        GN_LOG("checkpoint: spawn %llx (%s), +0x28 %llx (%s), +0x38 %llx (%s), +0x158 %llx (%s)", static_cast<unsigned long long>(spawn),
               nameAt(spawn), static_cast<unsigned long long>(a), nameAt(a), static_cast<unsigned long long>(b), nameAt(b),
               static_cast<unsigned long long>(c), nameAt(c));
    return c && readVec(c + 0x170, out) ? GN_OK : GN_ERR_UNAVAILABLE;
}

namespace {
// ---- progression -------------------------------------------------------------------------------------
// The flags come from PamProgressionData by its reflected fields (names, not offsets); their values
// from the progression manager's hash map, looked up as the game's 0x1439da880 does.

uint32_t fieldOffset(const char* type, const char* field) {
    const reflect::Type* t = registry().find(type);
    reflect::FieldInfo fi;
    return t && registry().findField(t, field, fi) && fi.field ? fi.field->offset : 0xffffffffu;
}

// A Frostbite array field: the elements' pointer, its count at -4.
std::vector<uintptr_t> arrayOf(uintptr_t obj, uint32_t off) {
    std::vector<uintptr_t> out;
    const uintptr_t a = off == 0xffffffffu ? 0 : ptr(obj + off);
    const uint32_t n = a ? mem::readOr<uint32_t>(a - 4) : 0;
    for (uint32_t i = 0; i < n && i < 100000; ++i) out.push_back(mem::readOr<uintptr_t>(a + uint64_t(i) * 8));
    return out;
}

std::string cstr(uintptr_t obj, uint32_t off) {
    const uintptr_t p = off == 0xffffffffu ? 0 : ptr(obj + off);
    return p ? mem::ProcessMemory().readCString(p, 256) : std::string();
}

struct Flag {
    uintptr_t flag = 0;
    std::string name, group;
    uint32_t hash = 0, mission = 0;
    int32_t max = 0;
    bool clamp = false, online = false;
};
struct Flags {
    uintptr_t data = 0;
    ULONGLONG built = 0;
    std::vector<Flag> all;
    uintptr_t activeMission = 0;                            // SystemGeneratedFlags.Flags[0]
    std::vector<std::pair<uint32_t, std::string>> missions; // MissionIndex -> name
};
std::mutex g_flagsM;
Flags g_flags; // (g_flagsM)

uintptr_t progressionData() {
    const uintptr_t settings = settingsFind("PamProgressionSettings");
    const uint32_t off = fieldOffset("PamProgressionSettings", "PlayerProgressionData");
    const uintptr_t data = settings && off != 0xffffffffu ? ptr(settings + off) : 0;
    return kindOf(data, "PamProgressionData") ? data : 0;
}

// All flags, rebuilt when the data changes (a level load can bring another) or every 10 s.
void buildLocked() {
    const uintptr_t data = progressionData();
    if (data == g_flags.data && GetTickCount64() - g_flags.built < 10000) return;
    Flags f;
    f.data = data, f.built = GetTickCount64();
    if (!data) {
        g_flags = std::move(f);
        return;
    }
    static const uint32_t fName = fieldOffset("PamProgressionFlag", "Name"), fHash = fieldOffset("PamProgressionFlag", "NameHash"),
                          fMission = fieldOffset("PamProgressionFlag", "MissionIndex"), fMax = fieldOffset("PamProgressionFlag", "MaxValue"),
                          fClamp = fieldOffset("PamProgressionFlag", "Clamp"), fSync = fieldOffset("PamProgressionFlag", "SyncToOnline"),
                          gName = fieldOffset("PamProgressionFlagGroup", "Name"), gFlags = fieldOffset("PamProgressionFlagGroup", "Flags"),
                          gSyncCount = fieldOffset("PamProgressionFlagGroup", "SyncNumberOfFlagsSetToOnline"),
                          gSyncSum = fieldOffset("PamProgressionFlagGroup", "SyncSumOfFlagValuesToOnline"),
                          gSyncAll = fieldOffset("PamProgressionFlagGroup", "ForceSyncAllFlagsToOnline"),
                          mIndex = fieldOffset("PamProgressionMission", "MissionIndex"), mDone = fieldOffset("PamProgressionMission", "CompletedFlag"),
                          mAvail = fieldOffset("PamProgressionMission", "AvailableFlag"), dGroups = fieldOffset("PamProgressionData", "FlagGroups"),
                          dMissions = fieldOffset("PamProgressionData", "Missions");
    auto u8at = [](uintptr_t o, uint32_t off) { return off == 0xffffffffu ? uint8_t(0) : mem::readOr<uint8_t>(o + off); };
    std::vector<uintptr_t> seen;
    auto add = [&](uintptr_t flag, const std::string& group, bool groupOnline) {
        if (!kindOf(flag, "PamProgressionFlag") || std::find(seen.begin(), seen.end(), flag) != seen.end()) return;
        seen.push_back(flag);
        Flag x;
        x.flag = flag, x.name = cstr(flag, fName), x.group = group;
        x.hash = fHash == 0xffffffffu ? 0 : mem::readOr<uint32_t>(flag + fHash);
        x.mission = fMission == 0xffffffffu ? 0 : mem::readOr<uint32_t>(flag + fMission);
        x.max = fMax == 0xffffffffu ? 0 : mem::readOr<int32_t>(flag + fMax);
        x.clamp = u8at(flag, fClamp) != 0;
        x.online = groupOnline || u8at(flag, fSync) != 0;
        f.all.push_back(std::move(x));
    };
    auto addGroup = [&](uintptr_t g) {
        if (!kindOf(g, "PamProgressionFlagGroup")) return;
        const std::string name = cstr(g, gName);
        const bool online = u8at(g, gSyncCount) || u8at(g, gSyncSum) || u8at(g, gSyncAll);
        for (uintptr_t fl : arrayOf(g, gFlags)) add(fl, name, online);
        if (kindOf(g, "PamProgressionMission")) {
            if (mDone != 0xffffffffu) add(ptr(g + mDone), name, online);
            if (mAvail != 0xffffffffu) add(ptr(g + mAvail), name, online);
            if (mIndex != 0xffffffffu) f.missions.push_back({mem::readOr<uint32_t>(g + mIndex), name});
        }
    };
    for (uintptr_t g : arrayOf(data, dGroups)) addGroup(g);
    for (uintptr_t m : arrayOf(data, dMissions)) addGroup(m);
    for (const char* sys : {"SystemGeneratedFlags", "SystemGeneratedMiscCompletionFlags", "SystemGeneratedBronzeCompletionFlags",
                            "SystemGeneratedSilverCompletionFlags", "SystemGeneratedGoldCompletionFlags"}) {
        const uint32_t off = fieldOffset("PamProgressionData", sys);
        if (off == 0xffffffffu) continue;
        const uintptr_t g = ptr(data + off);
        addGroup(g);
        if (std::string(sys) == "SystemGeneratedFlags") {
            const auto first = arrayOf(g, gFlags); // the active mission (ProgressionManager_setFlag's test)
            if (!first.empty() && kindOf(first[0], "PamProgressionFlag")) f.activeMission = first[0];
        }
    }
    const bool changed = data != g_flags.data;
    g_flags = std::move(f);
    if (changed)
        GN_LOG("progression: %d flags in %llx (PamProgressionData), %d missions, active mission flag %s", static_cast<int>(g_flags.all.size()),
               static_cast<unsigned long long>(data), static_cast<int>(g_flags.missions.size()),
               g_flags.activeMission ? cstr(g_flags.activeMission, fName).c_str() : "none");
}

// The game's value for a flag (by its NameHash), as 0x1439da880 looks it up.
bool valueOf(uint32_t hash, int32_t& out) {
    const uintptr_t m = ptr(mem::rva(e::global::ProgressionManager));
    const uintptr_t buckets = ptr(m + k::Progression_Buckets);
    const uint32_t count = mem::readOr<uint32_t>(m + k::Progression_BucketCount);
    if (!buckets || count == 0 || count > 0x100000) return false;
    const uintptr_t end = mem::readOr<uintptr_t>(buckets + uint64_t(count) * 8);
    int n = 0;
    for (uintptr_t entry = mem::readOr<uintptr_t>(buckets + uint64_t(hash % count) * 8); entry && entry != end && n < 256;
         entry = mem::readOr<uintptr_t>(entry + k::Progression_EntryNext), ++n)
        if (mem::readOr<uint32_t>(entry) == hash) return mem::read(entry + k::Progression_EntryValue, &out, sizeof(out));
    return false;
}

void fill(const Flag& f, gn_progress_flag& out) {
    out = gn_progress_flag{};
    out.flag = reinterpret_cast<void*>(f.flag);
    out.name = f.name.c_str();
    out.group = f.group.c_str();
    out.name_hash = f.hash;
    out.mission_index = f.mission;
    out.max_value = f.max;
    out.clamp = f.clamp;
    out.online = f.online;
    int32_t v = 0;
    out.has_value = valueOf(f.hash, v);
    out.value = out.has_value ? v : 0;
}
} // namespace

int progressFlags(gn_progress_flag_fn fn, void* user) {
    std::lock_guard<std::mutex> lock(g_flagsM);
    buildLocked();
    if (!g_flags.data) return -GN_ERR_UNAVAILABLE;
    if (fn)
        for (const Flag& f : g_flags.all) {
            gn_progress_flag out;
            fill(f, out);
            fn(&out, user);
        }
    return static_cast<int>(g_flags.all.size());
}

int progressFind(const char* name, gn_progress_flag* out) {
    if (!name || !out) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_flagsM);
    buildLocked();
    if (!g_flags.data) return GN_ERR_UNAVAILABLE;
    for (const Flag& f : g_flags.all)
        if (f.name == name) {
            fill(f, *out);
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

int progressSet(void* flag, int32_t value, int32_t* now) {
    if (!isServerThread()) return GN_ERR_THREAD;
    if (!build::codeMatches("ProgressionManager_setFlag")) return GN_ERR_UNAVAILABLE;
    Flag f;
    {
        std::lock_guard<std::mutex> lock(g_flagsM);
        buildLocked();
        const auto it = std::find_if(g_flags.all.begin(), g_flags.all.end(), [&](const Flag& x) { return x.flag == reinterpret_cast<uintptr_t>(flag); });
        if (it == g_flags.all.end()) return GN_ERR_ARGUMENT; // not one of the game's flags
        f = *it;
    }
    // Flags the data marks as synced to online stats are set like any other (user, 2026-11-04: many
    // of them never reach an online service). The log says which ones were.
    if (f.online) GN_LOG("progression: %s is marked as synced to online stats; set anyway", f.name.c_str());
    const uintptr_t m = ptr(mem::rva(e::global::ProgressionManager));
    if (!m) return GN_ERR_UNAVAILABLE;
    using SetFn = int32_t (*)(uintptr_t, uintptr_t, int32_t, bool, bool, bool);
    int32_t was = 0;
    valueOf(f.hash, was);
    // add false, notify missions (as the game's own calls from the progression entities do), and
    // not only while its mission runs.
    const int32_t kept = reinterpret_cast<SetFn>(mem::rva(e::function::ProgressionManager_setFlag))(m, f.flag, value, false, true, false);
    GN_LOG("progression: %s (%s) %d -> %d (asked %d)", f.name.c_str(), f.group.c_str(), was, kept, value);
    if (now) *now = kept;
    return GN_OK;
}

// The console's "progress": no arguments or a filter lists flags (name, group, value, max); a name
// and a number sets that flag (on the server thread; the result goes to the log too).
std::string progressCommand(const std::string& args) {
    std::string a = args, b;
    if (const size_t sp = a.find(' '); sp != std::string::npos) b = a.substr(a.find_first_not_of(' ', sp) == std::string::npos ? a.size() : a.find_first_not_of(' ', sp)), a.resize(sp);
    char* end = nullptr;
    const long value = b.empty() ? 0 : strtol(b.c_str(), &end, 10);
    if (!b.empty() && end && *end == 0) {
        gn_progress_flag f;
        if (progressFind(a.c_str(), &f) != GN_OK) return "No progression flag named \"" + a + "\" (progress " + a + " lists the ones matching)";
        void* flag = f.flag;
        const std::string name = a;
        server().post([flag, name, value] {
            int32_t now = 0;
            if (progressSet(flag, static_cast<int32_t>(value), &now) != GN_OK) GN_LOG("progress %s %ld: refused", name.c_str(), value);
        });
        return "Setting " + a + " to " + b + " (was " + std::to_string(f.value) + "); the result goes to the log";
    }
    std::string out;
    int n = 0, shown = 0;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string filter = lower(a);
    struct Ctx {
        std::string* out;
        const std::string* filter;
        int *n, *shown;
        std::string (*lower)(std::string);
    } ctx{&out, &filter, &n, &shown, +lower};
    const int total = progressFlags(
        [](const gn_progress_flag* f, void* u) {
            auto& c = *static_cast<Ctx*>(u);
            if (!c.filter->empty() && c.lower(f->name).find(*c.filter) == std::string::npos && c.lower(f->group).find(*c.filter) == std::string::npos)
                return;
            ++*c.n;
            if (++*c.shown > 200) return;
            char line[320];
            snprintf(line, sizeof(line), "%s  [%s]  %d%s%s\n", f->name, f->group, f->value, f->clamp ? (" / " + std::to_string(f->max_value)).c_str() : "",
                     f->online ? "  (online stat)" : "");
            *c.out += line;
        },
        &ctx);
    if (total < 0) return "Progression is not available (no level loaded?)";
    out += std::to_string(n) + " of " + std::to_string(total) + " flags" + (shown > 200 ? " (the first 200 shown)" : "") +
           ". progress NAME VALUE sets one.";
    return out;
}

int mission(int32_t* index, char* name, size_t size) {
    if (!index) return GN_ERR_ARGUMENT;
    uintptr_t active;
    uint32_t hash;
    std::vector<std::pair<uint32_t, std::string>> missions;
    {
        std::lock_guard<std::mutex> lock(g_flagsM);
        buildLocked();
        active = g_flags.activeMission;
        if (!active) return GN_ERR_UNAVAILABLE;
        const auto it = std::find_if(g_flags.all.begin(), g_flags.all.end(), [&](const Flag& x) { return x.flag == active; });
        if (it == g_flags.all.end()) return GN_ERR_UNAVAILABLE;
        hash = it->hash;
        missions = g_flags.missions;
    }
    int32_t v = 0;
    valueOf(hash, v); // no entry: no mission
    *index = v;
    if (name && size) {
        name[0] = 0;
        for (const auto& [i, n] : missions)
            if (v && i == uint32_t(v)) {
                snprintf(name, size, "%s", n.c_str());
                break;
            }
    }
    return GN_OK;
}

} // namespace gn::host
