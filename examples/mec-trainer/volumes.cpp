// Trigger volumes: the level's area triggers drawn in the world, sorted into categories by what
// their events reach (a cutscene's timeline, a spawn-control prefab, music...). Research and layouts:
// docs/research/trigger-volumes.md.
//
//   - shapes: the trigger's own geometry cache, the one the game tests Faith against each tick
//     (PamServerAreaTriggerEntity +0x1b8, ServerAreaTriggerEntity +0x1a8)
//   - categories: the bus's event connections from the trigger's data, followed two steps through
//     logic in the same bus; prefab references name their blueprint (Gameplay/Logic/PF_SafeSpawnControl)
//   - barriers (OBBCollisionEntity boxes) are drawn with them, in a category of their own
//
// Threads: the client tick reads the game (a few new triggers per tick, nearest first) and publishes
// the lines to draw; the render thread draws them and owns the settings.
#include "volumes.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gridnode/ui.hpp"

namespace volumes {
namespace {

// ---- categories ----------------------------------------------------------------------------------
enum Cat { kChallenge, kTeleport, kKill, kInteract, kCutscene, kCheckpoint, kSpawnLock, kBarrier, kMission, kCityAlert, kAi, kSound, kOther, kCats };
struct CatInfo {
    const char* key;
    const char* label;
    ImU32 colour;
    bool on; // default
};
const CatInfo kCatInfo[kCats] = {
    {"challenge", "Dashes and challenges", IM_COL32(255, 150, 30, 240), true},
    {"teleport", "Teleports (fall catchers, skips)", IM_COL32(0, 255, 230, 240), true},
    {"kill", "Kill volumes", IM_COL32(255, 0, 60, 240), true},
    {"interact", "Interaction points (mission starts, interact prompts)", IM_COL32(255, 255, 120, 230), true},
    {"cutscene", "Cutscenes and scripted scenes", IM_COL32(255, 80, 200, 230), true},
    {"checkpoint", "Checkpoints and spawns", IM_COL32(80, 255, 120, 230), true},
    {"spawn_lock", "Safe spawn blockers (no safe spawn set inside)", IM_COL32(210, 255, 140, 200), false},
    {"barrier", "Movement blockers (barriers)", IM_COL32(0, 140, 255, 220), false},
    {"mission", "Mission logic and markers", IM_COL32(255, 200, 60, 230), false},
    {"city_alert", "City alert and safe zones", IM_COL32(80, 200, 255, 230), false},
    {"ai", "AI spawns", IM_COL32(255, 90, 60, 230), false},
    {"sound", "Music and voice", IM_COL32(170, 140, 255, 200), false},
    {"other", "Other", IM_COL32(200, 200, 200, 160), false},
};
// Which category a target says, by its type and blueprint name (lower case). Sound first: mission
// music prefabs are named "MissionMusic_...". Dashes are named challenges in the game's types
// (PamNamedChallenge*, shared content); their trigger names are a guess until seen in game.
int categoryOf(const std::string& s) {
    auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
    if (has("challenge") || has("dash") || has("sharedcontent") || has("timetrial") || has("finishline") || has("/race")) return kChallenge;
    if (has("teleportentitydata")) return kTeleport;
    // A trigger that switches a kill volume (the kill volumes themselves are drawn from their own cache).
    if (has("killvolume")) return kKill;
    // The game's interact zones: a marker's prompt and what pressing it starts (a side mission at a
    // silver marker, 2026-11-03: PF_S_Ui_GenericMarkerInteractLogic inside PF_MissionInteraction).
    if (has("markerinteract") || has("pf_s_interact") || has("missioninteraction") || has("interactlogic")) return kInteract; // Old Friends' fall catcher under the roofs (2026-11-02)
    if (has("me_sound") || has("soundentity") || has("voiceover") || has("mixerentity") || has("/vo/") || has("music")) return kSound;
    if (has("timeline") || has("cinematic") || has("sequence") || has("cutscene") || has("animations/") || has("anim_")) return kCutscene;
    if (has("safespawnlocker")) return kSpawnLock; // PamSafeSpawnLocker: Faith sets no safe spawn inside (user, 2026-11-03)
    if (has("checkpoint") || has("safespawn") || has("respawn") || has("spawncontrol") || has("savegame")) return kCheckpoint;
    if (has("cityalert")) return kCityAlert;
    if (has("aigroup") || has("/ai/") || has("aispawn")) return kAi;
    if (has("mission")) return kMission;
    return -1;
}

// ---- settings (render thread) ----------------------------------------------------------------------
std::atomic<bool> g_on{false};
std::atomic<bool> g_catOn[kCats];
std::atomic<float> g_radius{60.f};
std::atomic<float> g_maxSize{150.f}; // larger ones (zones) are left out
std::atomic<bool> g_pick{false};     // the one in the middle of the screen lights up (off unless asked: user, 2026-11-02)
std::atomic<int> g_labels{6};
std::mutex g_searchM;
std::string g_search; // (g_searchM) lower case; when set, it picks the triggers instead of the categories
std::atomic<void*> g_highlight{nullptr};
std::atomic<bool> g_logWanted{false};

// ---- reading the game (client thread) --------------------------------------------------------------
template <class T> T rd(const void* p, size_t off) {
    T v{};
    gn::read(static_cast<const char*>(p) + off, v);
    return v;
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string cstr(const void* p) {
    char b[256] = {};
    if (p) gn::api().read(p, b, sizeof(b) - 1);
    size_t n = 0;
    while (n < sizeof(b) - 1 && b[n] >= 32) ++n;
    return std::string(b, n);
}

struct Line {
    gn::Vec3 a, b;
};
struct M { // LinearTransform: rows x, y, z, translation
    float r[4][4];
};
gn::Vec3 xform(const M& m, gn::Vec3 p, float w = 1.f) {
    return {p.x * m.r[0][0] + p.y * m.r[1][0] + p.z * m.r[2][0] + w * m.r[3][0], p.x * m.r[0][1] + p.y * m.r[1][1] + p.z * m.r[2][1] + w * m.r[3][1],
            p.x * m.r[0][2] + p.y * m.r[1][2] + p.z * m.r[2][2] + w * m.r[3][2]};
}
M identity() {
    M m{};
    m.r[0][0] = m.r[1][1] = m.r[2][2] = m.r[3][3] = 1.f;
    return m;
}
// The cache keeps world -> local transforms; drawing needs local -> world.
bool inverse(const M& m, M& o) {
    const float a = m.r[0][0], b = m.r[0][1], c = m.r[0][2], d = m.r[1][0], e = m.r[1][1], f = m.r[1][2], g = m.r[2][0], h = m.r[2][1], i = m.r[2][2];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return false;
    const float k = 1.f / det;
    o = identity();
    o.r[0][0] = (e * i - f * h) * k, o.r[0][1] = (c * h - b * i) * k, o.r[0][2] = (b * f - c * e) * k;
    o.r[1][0] = (f * g - d * i) * k, o.r[1][1] = (a * i - c * g) * k, o.r[1][2] = (c * d - a * f) * k;
    o.r[2][0] = (d * h - e * g) * k, o.r[2][1] = (b * g - a * h) * k, o.r[2][2] = (a * e - b * d) * k;
    for (int col = 0; col < 3; ++col) o.r[3][col] = -(m.r[3][0] * o.r[0][col] + m.r[3][1] * o.r[1][col] + m.r[3][2] * o.r[2][col]);
    return true;
}
M combine(const M& a, const M& b) { // a, then b
    M o = identity();
    for (int r = 0; r < 4; ++r) {
        const gn::Vec3 q = xform(b, {a.r[r][0], a.r[r][1], a.r[r][2]}, r == 3 ? 1.f : 0.f);
        o.r[r][0] = q.x, o.r[r][1] = q.y, o.r[r][2] = q.z;
    }
    return o;
}
M readM(const void* p) {
    M m = identity();
    gn::read(p, m);
    return m;
}

void addBox(std::vector<Line>& out, const M& world, gn::Vec3 c, gn::Vec3 h) {
    gn::Vec3 v[8];
    for (int i = 0; i < 8; ++i) v[i] = xform(world, {c.x + (i & 1 ? h.x : -h.x), c.y + (i & 2 ? h.y : -h.y), c.z + (i & 4 ? h.z : -h.z)});
    static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& k : kEdges) out.push_back({v[k[0]], v[k[1]]});
}

// One BaseShapeData (layouts from the reflected types; the test is FUN_143427220) into lines.
void addShape(std::vector<Line>& out, const M& world, void* s) {
    const gn::Type t = s ? gn::Type::of(s) : gn::Type();
    if (!t) return;
    const char* n = t.name();
    if (!std::strcmp(n, "SphereData")) { // Position +0x20, Radius +0x18
        const gn::Vec3 c = rd<gn::Vec3>(s, 0x20);
        const float r = rd<float>(s, 0x18);
        if (!(r > 0.f) || r > 1000.f) return;
        for (int ring = 0; ring < 3; ++ring)
            for (int i = 0; i < 24; ++i) {
                auto at = [&](int k) {
                    const float a = k * 6.2831853f / 24, x = std::cos(a) * r, y = std::sin(a) * r;
                    return ring == 0 ? gn::Vec3{c.x + x, c.y, c.z + y} : ring == 1 ? gn::Vec3{c.x + x, c.y + y, c.z} : gn::Vec3{c.x, c.y + x, c.z + y};
                };
                out.push_back({xform(world, at(i)), xform(world, at(i + 1))});
            }
    } else if (!std::strcmp(n, "AABBData")) { // Position +0x20, HalfExtents +0x30
        addBox(out, world, rd<gn::Vec3>(s, 0x20), rd<gn::Vec3>(s, 0x30));
    } else if (!std::strcmp(n, "OBBData")) { // Transform +0x20, HalfExtents +0x60
        addBox(out, combine(readM(static_cast<char*>(s) + 0x20), world), {0, 0, 0}, rd<gn::Vec3>(s, 0x60));
    } else if (std::strstr(n, "VectorShapeData")) {
        // An outline in x / z (Points +0x18, Vec3 0x10 each) from its lowest point up Height (+0x28,
        // VolumeVectorShapeData); no height is unbounded in y, drawn 3 m tall.
        char* pts = rd<char*>(s, 0x18);
        const uint32_t np = pts ? std::min<uint32_t>(rd<uint32_t>(pts, -4), 256) : 0;
        if (np < 2) return;
        std::vector<gn::Vec3> p(np);
        float base = 1e30f;
        for (uint32_t i = 0; i < np; ++i) p[i] = rd<gn::Vec3>(pts, i * 0x10), base = std::min(base, p[i].y);
        const float h = !std::strcmp(n, "VolumeVectorShapeData") ? rd<float>(s, 0x28) : 0.f;
        const float top = base + (h > 0.f ? h : 3.f);
        for (uint32_t i = 0; i < np; ++i) {
            const gn::Vec3 a = p[i], b = p[(i + 1) % np];
            out.push_back({xform(world, {a.x, base, a.z}), xform(world, {b.x, base, b.z})});
            out.push_back({xform(world, {a.x, top, a.z}), xform(world, {b.x, top, b.z})});
            out.push_back({xform(world, {a.x, base, a.z}), xform(world, {a.x, top, a.z})});
        }
    }
}

// The trigger's shapes in world space. Its geometry cache (FUN_14330d320): +0x08 shapes
// (BaseShapeData*[], count at -4), +0x10 world -> local transforms (0x40 each, count at -4), +0x20
// kind: 1 in world space, 2 one transform (none: the bus's), 3 one per shape, 4 looked up each test.
// A kill volume builds the same cache, at +0x38 (PamServerKillVolumeEntity vf15 0x143971260, with
// FUN_14330bac0 from its data's Geometry links, like the triggers' vf15).
size_t cacheOffset(const char* cls) {
    if (!std::strcmp(cls, "PamServerKillVolumeEntity")) return 0x38;
    return std::strncmp(cls, "Pam", 3) == 0 ? 0x1b8 : 0x1a8;
}

std::vector<Line> shapesOf(void* e, const char* cls) {
    std::vector<Line> out;
    const char* g = static_cast<char*>(e) + cacheOffset(cls);
    const int kind = rd<int>(g, 0x20);
    if (kind < 1 || kind > 3) return out;
    char* shapes = rd<char*>(g, 0x08);
    char* xf = rd<char*>(g, 0x10);
    const uint32_t ns = shapes ? std::min<uint32_t>(rd<uint32_t>(shapes, -4), 32) : 0;
    const uint32_t nx = xf ? std::min<uint32_t>(rd<uint32_t>(xf, -4), 32) : 0;
    for (uint32_t i = 0; i < ns; ++i) {
        M w = identity();
        if (kind == 2 && nx == 0) {
            // The bus's transform (FUN_14314df80: its owner at the interface's +0x10). Not seen yet.
            void* owner = rd<void*>(rd<void*>(e, 0x20), 0x10);
            gn::Transform t;
            if (!owner || !gn::transformOf(owner, t)) return {};
            std::memcpy(&w, &t, sizeof(w) < sizeof(t) ? sizeof(w) : sizeof(t));
        } else if (kind == 2 || kind == 3) {
            const uint32_t k = kind == 2 ? 0 : i;
            if (k >= nx || !inverse(readM(xf + k * 0x40), w)) continue;
        }
        addShape(out, w, rd<void*>(shapes, i * 8));
    }
    return out;
}

// The bus's event connections (EntityBus_fireEvent @0x14314e4a0). The entity's +0x20 is its bus's
// interface (bus + 8). bus +0x68 -> +0x120 (+0x148 when bus +0x3c bit 28) {begin, end} of pointers to
// records {+0 source data, +8 target, +0x10 event, +0x14 target event, +0x18 kind}.
struct Conn {
    void* src;
    void* dst;
    uint32_t ev, tev;
};
// Per bus, as read: where its link array was (a bus freed with its part of the level and another made
// at the same address has a different array: read again; review, 2026-11-03), its links by source,
// and by event when needed. Read on the client tick, one read per link: a bus over the cap is left
// unread (its triggers stay "Other") and the cache stops growing at the total (a level's root bus
// could take ~96 MB on its own).
struct BusLinks {
    char* begin = nullptr;
    char* end = nullptr;
    std::vector<Conn> bySrc;
    std::vector<Conn> byEvent;
    bool byEventMade = false;
};
std::unordered_map<void*, BusLinks> g_conns;
size_t g_connsTotal = 0; // links cached, all buses (each list counted)
constexpr size_t kConnsPerBus = 400000, kConnsTotal = 1000000; // 24 bytes each: ~10 MB, ~24 MB

BusLinks& linksOf(void* bus) {
    void* x = rd<void*>(bus, 0x68);
    const size_t at = (rd<uint32_t>(bus, 0x3c) & 0x10000000) ? 0x148 : 0x120;
    char* b = x ? rd<char*>(x, at) : nullptr;
    char* e = x ? rd<char*>(x, at + 8) : nullptr;
    BusLinks& l = g_conns[bus];
    if (l.begin == b && l.end == e) return l;
    g_connsTotal -= std::min(g_connsTotal, l.bySrc.size() + l.byEvent.size());
    l = BusLinks{};
    l.begin = b, l.end = e;
    if (!b || e <= b) return l;
    const size_t n = size_t(e - b) / 8;
    if (n > kConnsPerBus || g_connsTotal + n > kConnsTotal) {
        static int said = 0;
        if (said++ < 5) gn::log("volumes: bus %p has %llu links: not read (cap %llu per bus, %llu in all; %llu cached)", bus, (unsigned long long)n,
                                (unsigned long long)kConnsPerBus, (unsigned long long)kConnsTotal, (unsigned long long)g_connsTotal);
        return l;
    }
    std::vector<void*> recs(n);
    if (gn::api().read(b, recs.data(), recs.size() * 8) != GN_OK) return l;
    l.bySrc.reserve(recs.size());
    for (void* r : recs) {
        struct {
            void* src;
            void* dst;
            uint32_t ev, tev;
        } rec{};
        if (r && gn::api().read(r, &rec, sizeof(rec)) == GN_OK) l.bySrc.push_back({rec.src, rec.dst, rec.ev, rec.tev});
    }
    std::sort(l.bySrc.begin(), l.bySrc.end(), [](const Conn& a, const Conn& b) { return a.src < b.src; });
    g_connsTotal += l.bySrc.size();
    return l;
}

const std::vector<Conn>& connectionsOf(void* bus) { return linksOf(bus).bySrc; }

// The bus's connections that send `event`, at most `max`: by an index on the event, not a pass over
// every link (a bus can hold hundreds of thousands).
std::vector<const Conn*> sendersOf(void* bus, uint32_t event, size_t max) {
    // As already read (the caller holds its links by source): not checked again here.
    auto found = g_conns.find(bus);
    BusLinks& l = found != g_conns.end() ? found->second : linksOf(bus);
    if (!l.byEventMade) {
        l.byEventMade = true;
        if (g_connsTotal + l.bySrc.size() <= kConnsTotal) {
            l.byEvent = l.bySrc;
            std::sort(l.byEvent.begin(), l.byEvent.end(), [](const Conn& a, const Conn& b) { return a.ev < b.ev; });
            g_connsTotal += l.byEvent.size();
        }
    }
    std::vector<const Conn*> out;
    auto lo = std::lower_bound(l.byEvent.begin(), l.byEvent.end(), event, [](const Conn& c, uint32_t k) { return c.ev < k; });
    for (; lo != l.byEvent.end() && lo->ev == event && out.size() < max; ++lo) out.push_back(&*lo);
    return out;
}

// target -> its type and "type[blueprint]"; named again when the type at that address changes (data
// freed with a part of the level and the address reused).
std::unordered_map<void*, std::pair<const void*, std::string>> g_names;
struct BarEnt {
    void* e;
    void* data;
};
std::vector<BarEnt> g_barEnts; // the barriers found by the last full pass (client tick)
const std::string& targetName(void* p) {
    const gn::Type t = p ? gn::Type::of(p) : gn::Type();
    auto it = g_names.find(p);
    if (it != g_names.end() && it->second.first == t.get()) return it->second.second;
    auto& entry = g_names[p];
    entry.first = t.get();
    std::string& n = entry.second;
    n.clear();
    if (!t) return n;
    n = t.name();
    gn_field f;
    if (t.field("Blueprint", f)) {
        void* bp = rd<void*>(p, f.offset);
        const gn::Type bt = bp ? gn::Type::of(bp) : gn::Type();
        gn_field nf;
        if (bt && bt.field("Name", nf)) n += "[" + cstr(rd<void*>(bp, nf.offset)) + "]";
    }
    return n;
}

struct Volume {
    std::vector<Line> lines;
    gn::Vec3 centre, lo, hi; // the box around its shapes
    float reach = 0.f;       // from the centre to its farthest point
    bool classified = false;
    uint32_t cats = 0;
    int primary = kOther;
    std::string why;  // the target that decided it, short
    std::string hay;  // every target, lower case, for the search
    bool teleports = false;
    gn::Vec3 teleportTo; // a teleport it fires: TeleportEntityData NewPositionTransform (+0x20), as authored
    bool barrier = false; // an OBB collision entity, not a trigger
};

std::string eventText(uint32_t id);

// A target that is a placed blueprint (a prefab or sublevel reference) whose peer in this bus is a
// FullEntityBusBridge: the bridge's onEvent (vtable +0x20, 0x14314a5d0) fires the event in the bridged
// bus (bridge +8) from that bus's interface descriptor (bus +0x68 -> +0xb8 -> +0x28). The peer is
// found as EntityBus_deliverEvent does: the bus data's sorted targets (+0xd0, +0xf8 when bus +0x3c
// bit 28) give the index into the peer table (bus +0x40). Key checkpoints go this way (2026-11-02).
bool bridgeInto(void* iface, void* target, void*& childIface, void*& source) {
    char* bus = static_cast<char*>(iface) - 8;
    char* x = rd<char*>(bus, 0x68);
    if (!x || !target) return false;
    const size_t at = (rd<uint32_t>(bus, 0x3c) & 0x10000000) ? 0xf8 : 0xd0;
    char* b = rd<char*>(x, at);
    char* e = rd<char*>(x, at + 8);
    if (!b || e <= b || e - b > 8 * 4000000) return false;
    size_t lo = 0, hi = size_t(e - b) / 8; // lower_bound over sorted pointers
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (rd<uintptr_t>(b, mid * 8) < reinterpret_cast<uintptr_t>(target)) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= size_t(e - b) / 8 || rd<void*>(b, lo * 8) != target) return false;
    void* peer = rd<void*>(rd<char*>(bus, 0x40), lo * 8);
    const gn::Type pt = peer ? gn::Type::of(peer) : gn::Type();
    if (!pt || !std::strstr(pt.name(), "FullEntityBusBridge")) return false;
    char* child = rd<char*>(peer, 8);
    char* cx = child ? rd<char*>(child, 0x68) : nullptr;
    char* desc = cx ? rd<char*>(cx, 0xb8) : nullptr;
    source = desc ? rd<void*>(desc, 0x28) : nullptr;
    childIface = child ? child + 8 : nullptr;
    return childIface && source;
}

// What a blueprint's interface event comes out of in the parent bus (EntityBus_deliverEvent
// 0x14314eac0: fireEvent(parent, *(bus +0x60) & ~3, event)); `iface` is the bus's interface (bus + 8).
void* outerSource(void* iface) { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(rd<void*>(iface, 0x58)) & ~uintptr_t(3)); }

// The bus that event lands in: the parent (interface +8), passed on upwards while that bus is a
// see-through one (its interface +0x34 bit 27; 0x14314e980), unless it has a proxy (+0x50: not
// followed). A sublevel's events go this way (its parent bus has no links of its own).
void* outerBus(void* iface) {
    char* p = rd<char*>(iface, 0x8);
    for (int i = 0; p && i < 8 && ((rd<uint32_t>(p, 0x34) >> 27) & 1); ++i) {
        if (rd<void*>(p, 0x50)) return nullptr;
        p = rd<char*>(p, 0x8);
    }
    return p;
}

// Follows the trigger's events. A link to an InterfaceDescriptorData leaves the blueprint the trigger
// is in (a prefab, a Dash's logic): it goes on in the parent bus (the bus interface's +8), from the
// source EntityBus_deliverEvent gives it (bus +0x60, low bits masked: the interface's +0x58), with the
// interface's event (Dash starts, 2026-11-01; the source offset from EntityBus_deliverEvent, 2026-11-02).
void classify(void* e, Volume& v) {
    v.classified = true;
    struct Step {
        void* bus; // as entities keep it: the bus's interface (bus + 8)
        void* from;
        int depth;
        uint32_t event; // 0: any
    };
    std::vector<Step> todo{{rd<void*>(e, 0x20), rd<void*>(e, 0x28), 0, 0}};
    std::unordered_set<void*> seen{todo[0].from};
    int best = kCats;
    while (!todo.empty()) {
        const Step st = todo.back();
        todo.pop_back();
        if (!st.bus) continue;
        const std::vector<Conn>& all = connectionsOf(static_cast<char*>(st.bus) - 8);
        std::vector<const Conn*> hits;
        auto lo = std::lower_bound(all.begin(), all.end(), st.from, [](const Conn& c, void* k) { return c.src < k; });
        for (auto it = lo; it != all.end() && it->src == st.from; ++it)
            if (!st.event || it->ev == st.event) hits.push_back(&*it);
        if (hits.empty() && st.event) {
            // The placing reference was not the source: any reference sending that interface event
            // (the first 64 senders looked at, 4 kept).
            for (const Conn* c : sendersOf(static_cast<char*>(st.bus) - 8, st.event, 64))
                if (targetName(c->src).find("ReferenceObjectData") != std::string::npos && hits.size() < 4) hits.push_back(c);
        }
        for (const Conn* c : hits) {
            if (!c->dst || !seen.insert(c->dst).second) continue;
            const std::string& name = targetName(c->dst);
            if (name.rfind("PamAreaTriggerData", 0) == 0) continue; // enabling another trigger says nothing
            if (name.rfind("InterfaceDescriptorData", 0) == 0) {
                if (st.depth < 4) todo.push_back({outerBus(st.bus), outerSource(st.bus), st.depth + 1, c->tev});
                seen.erase(c->dst); // the same interface, another event
                continue;
            }
            if (void *childIface = nullptr, *src = nullptr; st.depth < 4 && bridgeInto(st.bus, c->dst, childIface, src))
                todo.push_back({childIface, src, st.depth + 1, c->tev}); // into the placed blueprint
            const std::string low = lower(name);
            v.hay += low + "\n";
            if (!v.teleports && low.find("teleportentitydata") != std::string::npos) {
                // Where it sends her, unless a link overrides the transform at run time.
                v.teleports = true;
                v.teleportTo = rd<gn::Vec3>(c->dst, 0x20 + 0x30);
            }
            const int cat = categoryOf(low);
            if (cat >= 0) {
                v.cats |= 1u << cat;
                if (cat < best) {
                    best = cat;
                    const size_t open = name.find('['), slash = name.rfind('/');
                    v.why = open == std::string::npos ? name : name.substr(slash != std::string::npos && slash > open ? slash + 1 : open + 1);
                    if (!v.why.empty() && v.why.back() == ']') v.why.pop_back();
                    if (cat == kKill) // which input: Enable / Disable say it is only switched by this trigger
                        v.why += "." + eventText(c->tev) + (rd<uint8_t>(c->dst, 0x1e) ? "" : " (not the player)");
                }
            }
            if (st.depth < 2) todo.push_back({st.bus, c->dst, st.depth + 1, 0});
        }
    }
    if (!v.cats) v.cats = 1u << kOther;
    v.primary = best < kCats ? best : kOther;
    if (v.primary == kInteract) { // which mission it starts: silver markers are side missions, gold the story's
        if (v.hay.find("silvermissionmarker") != std::string::npos) v.why += " (side mission)";
        else if (v.hay.find("goldmissionmarker") != std::string::npos) v.why += " (main mission)";
    }
}

std::unordered_map<void*, Volume> g_all; // client thread
std::string eventText(uint32_t id) {
    if (!id) return "-";
    std::string n = gn::eventName(id);
    if (!n.empty()) return n;
    char b[16];
    snprintf(b, sizeof(b), "%08x", id);
    return b;
}

// One trigger in full, for working out what it does (the tab's Log button): its data, shapes and
// every link its events follow, four steps deep, out of blueprints through their interfaces.
void describe(void* e) {
    const gn::Type t = gn::Type::of(e);
    if (auto it = g_all.find(e); it != g_all.end() && it->second.barrier) {
        const Volume& v = it->second;
        void* data = rd<void*>(e, 0x28);
        const gn::Type dt = data ? gn::Type::of(data) : gn::Type();
        const gn::Vec3 h = data ? rd<gn::Vec3>(data, 0x60) : gn::Vec3{};
        gn::log("volumes: barrier %p (%s), data %s %p: centre (%.1f, %.1f, %.1f), half extents (%.2f, %.2f, %.2f), body %08x%s%s", e,
                t ? t.name() : "?", dt ? dt.name() : "?", data, v.centre.x, v.centre.y, v.centre.z, h.x, h.y, h.z, rd<uint32_t>(e, 0x90),
                v.why.empty() ? "" : ": ", v.why.c_str());
        return;
    }
    void* data = rd<void*>(e, 0x28);
    const gn::Type dt = data ? gn::Type::of(data) : gn::Type();
    if (t && !std::strcmp(t.name(), "PamServerKillVolumeEntity"))
        gn::log("volumes: kill volume %p, data %p: DamageType %d, Enabled %d (now %d), KillEnemies %d, KillPlayer %d", e, data,
                data ? rd<int32_t>(data, 0x18) : -1, data ? rd<uint8_t>(data, 0x1c) : 0, rd<uint8_t>(e, 0x62), data ? rd<uint8_t>(data, 0x1d) : 0,
                data ? rd<uint8_t>(data, 0x1e) : 0);
    else
        gn::log("volumes: trigger %p (%s), data %s %p: Delay %.2f, RunOnce %d, Enabled %d", e, t ? t.name() : "?", dt ? dt.name() : "?", data,
                data ? rd<float>(data, 0x60) : 0.f, data ? rd<uint8_t>(data, 0x64) : 0, data ? rd<uint8_t>(data, 0x65) : 0);
    if (auto it = g_all.find(e); it != g_all.end() && !it->second.lines.empty()) {
        const Volume& v = it->second;
        gn::log("  shapes: %d lines, centre (%.1f, %.1f, %.1f), %.1f m across", int(v.lines.size()), v.centre.x, v.centre.y, v.centre.z, v.reach * 2.f);
    }
    int lines = 0;
    struct Step {
        void* bus;
        void* from;
        uint32_t event; // 0: any
        int depth;
    };
    std::vector<Step> todo{{rd<void*>(e, 0x20), data, 0, 0}};
    std::unordered_set<void*> seen{data};
    while (!todo.empty() && lines < 120) {
        const Step st = todo.back();
        todo.pop_back();
        if (!st.bus) continue;
        const std::vector<Conn>& all = connectionsOf(static_cast<char*>(st.bus) - 8);
        std::vector<const Conn*> hits;
        auto lo = std::lower_bound(all.begin(), all.end(), st.from, [](const Conn& c, void* k) { return c.src < k; });
        for (auto it = lo; it != all.end() && it->src == st.from; ++it)
            if (!st.event || it->ev == st.event) hits.push_back(&*it);
        if (st.event) {
            // Just left a blueprint: say where it went, and if the placing reference sends nothing on,
            // any reference in that bus that sends the interface's event (as the sorting does).
            const std::string pad(size_t(st.depth) * 2 + 4, ' ');
            gn::log("%s(in bus %p, placed by %s %p: %d links in it, %d from it with %s)", pad.c_str(), st.bus, targetName(st.from).c_str(), st.from,
                    int(all.size()), int(hits.size()), eventText(st.event).c_str());
            if (hits.empty())
                for (const Conn* c : sendersOf(static_cast<char*>(st.bus) - 8, st.event, 6)) {
                    gn::log("%s(sent by %s %p)", pad.c_str(), targetName(c->src).c_str(), c->src);
                    hits.push_back(c);
                }
        }
        for (const Conn* c : hits) {
            const std::string& name = targetName(c->dst);
            const std::string pad(size_t(st.depth) * 2 + 4, ' ');
            if (name.rfind("InterfaceDescriptorData", 0) == 0) {
                gn::log("%s%s -> out of the blueprint as %s", pad.c_str(), eventText(c->ev).c_str(), eventText(c->tev).c_str());
                ++lines;
                if (st.depth < 4) todo.push_back({outerBus(st.bus), outerSource(st.bus), c->tev, st.depth + 1});
                continue;
            }
            if (void *childIface = nullptr, *src = nullptr; st.depth < 4 && bridgeInto(st.bus, c->dst, childIface, src)) {
                gn::log("%s%s -> into %s as %s", pad.c_str(), eventText(c->ev).c_str(), name.c_str(), eventText(c->tev).c_str());
                ++lines;
                todo.push_back({childIface, src, c->tev, st.depth + 1});
                continue;
            }

            gn::log("%s%s -> %s.%s", pad.c_str(), eventText(c->ev).c_str(), name.c_str(), eventText(c->tev).c_str());
            ++lines;
            if (name.find("TeleportEntityData") != std::string::npos) {
                const gn::Vec3 to = rd<gn::Vec3>(c->dst, 0x20 + 0x30);
                gn::log("%s  (to %.1f, %.1f, %.1f; CheckBelowGround %d)", pad.c_str(), to.x, to.y, to.z, rd<uint8_t>(c->dst, 0x18));
            }
            if (st.depth < 4 && c->dst && seen.insert(c->dst).second) todo.push_back({st.bus, c->dst, 0, st.depth + 1});
        }
    }
    if (!lines) gn::log("  (no links found from its data)");
}

std::atomic<void*> g_describeWanted{nullptr};
std::atomic<void*> g_aimed{nullptr}; // the one in the middle of the screen (render thread)

std::atomic<unsigned> g_generation{0};    // bumped on a level change
unsigned g_seenGeneration = 0;

// ---- what is drawn (client tick publishes, render thread draws) -----------------------------------
struct Shown {
    void* e;
    std::vector<Line> lines;
    gn::Vec3 centre, lo, hi;
    bool teleports;
    gn::Vec3 teleportTo;
    ImU32 colour;
    int primary;
    float dist;
    std::string why;
};
std::mutex g_shownM;
std::vector<Shown> g_shown; // (g_shownM) nearest first
std::atomic<int> g_total{0}, g_withShapes{0}, g_pending{0};

bool wanted(const Volume& v, const std::string& search) {
    if (!search.empty()) return v.hay.find(search) != std::string::npos || lower(v.why).find(search) != std::string::npos;
    for (int c = 0; c < kCats; ++c)
        if ((v.cats & (1u << c)) && g_catOn[c]) return true;
    return false;
}

} // namespace

void logAimed() {
    if (!g_pick || !g_on) return gn::log("volumes: picking is off (Volumes tab, or its key)");
    if (void* a = g_aimed.load()) g_describeWanted = a;
    else gn::log("volumes: no trigger in the middle of the screen");
}

void toggleShow() {
    const bool on = !g_on;
    g_on = on;
    gn::config::setBool("volumes.on", on);
    gn::log("volumes: %s", on ? "shown" : "hidden");
}

void togglePick() {
    g_pick = !g_pick;
    if (!g_pick) g_aimed = nullptr;
    gn::log("volumes: picking %s", g_pick ? "on" : "off");
}

void loadSettings() {
    g_on = gn::config::getBool("volumes.on", false);
    for (int c = 0; c < kCats; ++c) g_catOn[c] = gn::config::getBool((std::string("volumes.") + kCatInfo[c].key).c_str(), kCatInfo[c].on);
    g_radius = gn::config::getFloat("volumes.radius", 60.f);
    g_labels = int(gn::config::getFloat("volumes.labels", 6.f));
    g_maxSize = gn::config::getFloat("volumes.max_size", 150.f);
}

void levelChanged() { ++g_generation; }

bool anyShown() { return g_on.load(); }

void unload() {
    g_on = false; // reads nothing more; GridNode drops the trainer's callbacks
}

void clientTick(float dt) {
    static float t = 0.f;
    if (const unsigned gen = g_generation.load(); gen != g_seenGeneration) {
        g_seenGeneration = gen;
        g_all.clear(), g_conns.clear(), g_names.clear(), g_barEnts.clear();
        g_connsTotal = 0;
    }
    if (!g_on) {
        if (!g_all.empty()) {
            g_all.clear(), g_conns.clear(), g_names.clear(), g_barEnts.clear();
            g_connsTotal = 0;
            std::lock_guard<std::mutex> lock(g_shownM);
            g_shown.clear();
        }
        return;
    }
    if ((t += dt) < 0.25f) return;
    t = 0.f;
    void* faith = gn::localClientCharacter();
    gn::Vec3 at{};
    if (!faith || !gn::positionOf(faith, at)) return;

    // What exists now: new triggers get their shapes, gone ones are dropped. A full pass over the
    // tracked entities once a second, not every 0.25 s (four classes, ~135,000 entities each time;
    // review, 2026-11-03); between passes the known ones are kept.
    static int tick = 0;
    const bool scan = tick++ % 4 == 0 || g_all.empty();
    std::unordered_set<void*> alive;
    if (!scan)
        for (const auto& [e, v] : g_all)
            if (!v.barrier) alive.insert(e);
    if (scan)
    for (const char* cls : {"PamServerAreaTriggerEntity", "ServerAreaTriggerEntity", "PamServerKillVolumeEntity"}) {
        gn::entitiesOfClass(cls, [&](void* e, gn::Type type) {
            if (!alive.insert(e).second || g_all.count(e)) return;
            Volume v;
            v.lines = shapesOf(e, type.name());
            if (!std::strcmp(type.name(), "PamServerKillVolumeEntity")) {
                // Data (PamKillVolumeEntityData): DamageType +0x18, KillEnemies +0x1d, KillPlayer +0x1e; the
                // entity's +0x62 is its Enabled as read at start (vf15).
                void* d = rd<void*>(e, 0x28);
                v.classified = true, v.cats = 1u << kKill, v.primary = kKill;
                const bool player = d && rd<uint8_t>(d, 0x1e), enemies = d && rd<uint8_t>(d, 0x1d);
                v.why = player ? (enemies ? "Faith and enemies" : "Faith") : enemies ? "enemies only" : "nobody (flags off)";
                if (!rd<uint8_t>(e, 0x62)) v.why += ", disabled at start";
                v.hay = "killvolume pamkillvolumeentitydata " + lower(v.why);
            }
            if (!v.lines.empty()) {
                gn::Vec3 lo = v.lines[0].a, hi = lo;
                for (const Line& l : v.lines)
                    for (const gn::Vec3& p : {l.a, l.b})
                        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}, hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                v.centre = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
                v.lo = lo, v.hi = hi;
                const float dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
                v.reach = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            g_all.emplace(e, std::move(v));
        });
    }
    // Barriers, as the layout editor finds them (layouts.cpp): OBBCollisionEntity, both realms (one
    // drawn: the server half where both have the same data at the same place). Its world transform at
    // +0x50 (set when it is enabled), enabled +0x95, server +0x18 bit 0; the box is the data's
    // HalfExtents (+0x60). PamOBBCollisionEntityData +0x90..0x93: can't stand, climb, vault, wall run.
    // Read again each time: the layout editor moves them.
    struct Bar {
        void* e;
        void* data;
        M t;
        bool server;
    };
    std::vector<Bar> bars;
    if (scan) {
        g_barEnts.clear();
        gn::entitiesOfClass("OBBCollisionEntity", [&](void* e, gn::Type) {
            void* data = rd<void*>(e, 0x28);
            const gn::Type dt = data ? gn::Type::of(data) : gn::Type();
            if (!dt || !std::strstr(dt.name(), "OBBCollisionEntityData")) return; // another layout
            g_barEnts.push_back({e, data});
        });
    }
    static const gn::Type obb = gn::Type::find("OBBCollisionEntity");
    for (const BarEnt& b : g_barEnts) {
        // Between passes: still a barrier with the same data (one that streamed out is not).
        if (!scan && (!gn::Type::of(b.e).isKindOf(obb) || rd<void*>(b.e, 0x28) != b.data)) continue;
        bars.push_back({b.e, b.data, readM(static_cast<char*>(b.e) + 0x50), (rd<uint32_t>(b.e, 0x18) & 1) != 0});
    }
    std::unordered_multimap<void*, const Bar*> serverHalves; // by data
    for (const Bar& b : bars)
        if (b.server) serverHalves.emplace(b.data, &b);
    for (const Bar& b : bars) {
        if (!b.server) {
            bool twin = false;
            for (auto [i, end] = serverHalves.equal_range(b.data); i != end && !twin; ++i)
                twin = std::fabs(i->second->t.r[3][0] - b.t.r[3][0]) + std::fabs(i->second->t.r[3][1] - b.t.r[3][1]) +
                           std::fabs(i->second->t.r[3][2] - b.t.r[3][2]) < 0.01f;
            if (twin) continue;
        }
        const gn::Vec3 h = rd<gn::Vec3>(b.data, 0x60);
        if (!std::isfinite(b.t.r[3][0]) || !std::isfinite(h.x) || !std::isfinite(h.y) || !std::isfinite(h.z)) continue;
        if (std::fabs(b.t.r[0][0]) + std::fabs(b.t.r[0][1]) + std::fabs(b.t.r[0][2]) < 0.01f) continue; // never enabled: no transform yet
        alive.insert(b.e);
        Volume v;
        v.barrier = true, v.classified = true, v.cats = 1u << kBarrier, v.primary = kBarrier;
        addBox(v.lines, b.t, {0, 0, 0}, h);
        gn::Vec3 lo = v.lines[0].a, hi = lo;
        for (const Line& l : v.lines)
            for (const gn::Vec3& p : {l.a, l.b})
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}, hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        v.lo = lo, v.hi = hi, v.centre = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        const float dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
        v.reach = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        const gn::Type dt = gn::Type::of(b.data);
        if (std::strstr(dt.name(), "Pam")) {
            uint8_t cant[4] = {};
            gn::api().read(static_cast<char*>(b.data) + 0x90, cant, sizeof(cant));
            static const char* kWhat[4] = {"stand", "climb", "vault", "wall run"};
            for (int i = 0; i < 4; ++i)
                if (cant[i]) v.why += (v.why.empty() ? "no " : ", ") + std::string(kWhat[i]);
        }
        if (!rd<uint8_t>(b.e, 0x95)) v.why += v.why.empty() ? "disabled" : " (disabled)";
        v.hay = "barrier obbcollision " + v.why;
        g_all[b.e] = std::move(v);
    }
    for (auto it = g_all.begin(); it != g_all.end();) it = alive.count(it->first) ? std::next(it) : g_all.erase(it);

    // Near ones, nearest first; a few new ones classified per tick (a bus can hold thousands of links).
    const float radius = g_radius;
    const float maxSize = g_maxSize;
    std::string search;
    {
        std::lock_guard<std::mutex> lock(g_searchM);
        search = g_search;
    }
    std::vector<std::pair<float, void*>> near;
    int withShapes = 0;
    for (auto& [e, v] : g_all) {
        if (v.lines.empty()) continue;
        ++withShapes;
        if (v.reach * 2.f > maxSize) continue; // a zone around half the city hides what is in it
        // From her to the nearest point of its box (0 inside).
        const float dx = std::max({v.lo.x - at.x, 0.f, at.x - v.hi.x}), dy = std::max({v.lo.y - at.y, 0.f, at.y - v.hi.y}),
                    dz = std::max({v.lo.z - at.z, 0.f, at.z - v.hi.z});
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d <= radius) near.push_back({d, e});
    }
    std::sort(near.begin(), near.end());
    int budget = 24, pending = 0;
    for (auto& [d, e] : near) {
        Volume& v = g_all[e];
        if (v.classified) continue;
        if (budget-- > 0) classify(e, v);
        else ++pending;
    }
    if (void* d = g_describeWanted.exchange(nullptr)) describe(d);
    if (g_logWanted.exchange(false)) {
        // For sorting out a category: every target the nearest triggers reach, and their buses' blueprints.
        gn::log("volumes: the nearest triggers at (%.1f, %.1f, %.1f), shown or not", at.x, at.y, at.z);
        // Research (Dashes): every trigger that reaches a challenge, near or not, with or without shapes.
        int challenges = 0;
        for (auto& [e, v] : g_all) {
            if (!v.classified) classify(e, v);
            if (!(v.cats & (1u << kChallenge)) || ++challenges > 40) continue;
            const gn::Type t = gn::Type::of(e);
            const char* g = static_cast<char*>(e) + cacheOffset(t.name());
            char* shapes = rd<char*>(g, 0x08);
            float d = -1.f;
            if (!v.lines.empty()) {
                const float dx = v.centre.x - at.x, dy = v.centre.y - at.y, dz = v.centre.z - at.z;
                d = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            gn::log("  challenge trigger %p: kind %d, %u shapes, %d lines, %.1f m, centre (%.1f, %.1f, %.1f): %s", e, rd<int>(g, 0x20),
                    shapes ? rd<uint32_t>(shapes, -4) : 0u, int(v.lines.size()), d, v.centre.x, v.centre.y, v.centre.z, v.why.c_str());
            size_t from = 0;
            while (from < v.hay.size()) {
                const size_t to = v.hay.find('\n', from);
                gn::log("      -> %s", v.hay.substr(from, to - from).c_str());
                if (to == std::string::npos) break;
                from = to + 1;
            }
        }
        gn::log("  %d triggers reach a challenge (of %d)", challenges, int(g_all.size()));
        int n = 0;
        for (auto& [d, e] : near) {
            if (n++ >= 8) break;
            Volume& v = g_all[e];
            if (!v.classified) classify(e, v);
            std::string up;
            void* b = static_cast<char*>(rd<void*>(e, 0x20)) - 8;
            for (int i = 0; i < 6 && b; ++i) {
                void* ref = rd<void*>(b, 0x38);
                const std::string rn = ref ? targetName(ref) : "";
                if (!rn.empty()) up += (up.empty() ? "" : " < ") + rn;
                void* parent = rd<void*>(b, 0x8);
                b = parent ? static_cast<char*>(parent) - 8 : nullptr;
            }
            gn::log("  %.1f m %p %s%s%s | in: %s", d, e, kCatInfo[v.primary].label, v.why.empty() ? "" : ": ", v.why.c_str(), up.c_str());
            size_t from = 0;
            while (from < v.hay.size()) {
                const size_t to = v.hay.find('\n', from);
                gn::log("      -> %s", v.hay.substr(from, to - from).c_str());
                if (to == std::string::npos) break;
                from = to + 1;
            }
        }
    }
    std::vector<Shown> shown;
    size_t lines = 0;
    for (auto& [d, e] : near) {
        const Volume& v = g_all[e];
        if (!v.classified || !wanted(v, search)) continue;
        if ((lines += v.lines.size()) > 40000) break; // a busy place stays drawable
        shown.push_back({e, v.lines, v.centre, v.lo, v.hi, v.teleports, v.teleportTo, kCatInfo[v.primary].colour, v.primary, d, v.why});
    }
    g_total = int(g_all.size()), g_withShapes = withShapes, g_pending = pending;
    std::lock_guard<std::mutex> lock(g_shownM);
    g_shown = std::move(shown);
}

// Where the view's centre line first enters the box, or -1 (slab test).
float rayBox(const gn::ui::View& v, const gn::Vec3& lo, const gn::Vec3& hi) {
    float tmin = 0.f, tmax = 1e9f;
    const float o[3] = {v.position[0], v.position[1], v.position[2]}, b0[3] = {lo.x, lo.y, lo.z}, b1[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(v.forward[k]) < 1e-6f) {
            if (o[k] < b0[k] || o[k] > b1[k]) return -1.f;
            continue;
        }
        float t0 = (b0[k] - o[k]) / v.forward[k], t1 = (b1[k] - o[k]) / v.forward[k];
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0), tmax = std::min(tmax, t1);
        if (tmin > tmax) return -1.f;
    }
    return tmin;
}

void draw() {
    if (!g_on) return;
    gn::ui::View view;
    if (!gn::ui::view(view)) return;
    const int labels = g_labels;
    void* hl = g_highlight.exchange(nullptr);
    std::lock_guard<std::mutex> lock(g_shownM);
    // The one in the middle of the screen: the nearest box the view's centre line enters (from inside
    // a box, the smallest one around the camera).
    void* aimed = nullptr;
    float best = 1e9f, bestSize = 1e9f;
    if (g_pick)
        for (const Shown& s : g_shown) {
            const float t = rayBox(view, s.lo, s.hi);
            if (t < 0.f) continue;
            const float size = (s.hi.x - s.lo.x) + (s.hi.y - s.lo.y) + (s.hi.z - s.lo.z);
            if (t < best - 0.01f || (t <= best + 0.01f && size < bestSize)) best = t, bestSize = size, aimed = s.e;
        }
    g_aimed = aimed;
    if (aimed) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
        ImGui::GetForegroundDrawList()->AddCircle(c, 5.f, IM_COL32(255, 255, 255, 200));
    }
    int n = 0;
    for (const Shown& s : g_shown) {
        const bool lit = s.e == hl || s.e == aimed;
        const ImU32 c = lit ? IM_COL32(255, 255, 255, 255) : s.colour;
        for (const Line& l : s.lines) gn::ui::line3d(view, l.a, l.b, c, lit ? 3.f : 1.5f);
        if (s.teleports) { // where it sends her
            gn::ui::line3d(view, s.centre, s.teleportTo, IM_COL32(0, 255, 230, lit ? 255 : 120), 1.f);
            gn::ui::marker(view, s.teleportTo, IM_COL32(0, 255, 230, 255), 5.f);
            if (lit) gn::ui::text3d(view, s.teleportTo, "teleports here", IM_COL32(0, 255, 230, 255));
        }
        if (n++ < labels || lit) {
            char label[200];
            snprintf(label, sizeof(label), "%s%s%s  %.0f m", kCatInfo[s.primary].label, s.why.empty() ? "" : ": ", s.why.c_str(), s.dist);
            gn::ui::text3d(view, s.centre, label, c);
        }
    }
}

void tab() {
    bool on = g_on;
    if (ImGui::Checkbox("Show trigger volumes", &on)) g_on = on, gn::config::setBool("volumes.on", on);
    ImGui::TextDisabled("The level's area triggers, as the game tests them. Sorted by what they set off.");
    ImGui::SeparatorText("Show");
    for (int c = 0; c < kCats; ++c) {
        bool v = g_catOn[c];
        ImGui::PushStyleColor(ImGuiCol_CheckMark, kCatInfo[c].colour);
        ImGui::PushStyleColor(ImGuiCol_Text, kCatInfo[c].colour);
        if (ImGui::Checkbox(kCatInfo[c].label, &v)) g_catOn[c] = v, gn::config::setBool((std::string("volumes.") + kCatInfo[c].key).c_str(), v);
        ImGui::PopStyleColor(2);
    }
    static char buf[96] = "";
    if (ImGui::InputTextWithHint("Search", "e.g. flytrap, checkpoint (all categories)", buf, sizeof(buf))) {
        std::lock_guard<std::mutex> lock(g_searchM);
        g_search = lower(buf);
    }
    float r = g_radius;
    if (ImGui::SliderFloat("Distance (m)", &r, 5.f, 300.f, "%.0f")) g_radius = r, gn::config::setFloat("volumes.radius", r);
    float ms = g_maxSize;
    if (ImGui::SliderFloat("Largest shown (m)", &ms, 5.f, 2000.f, "%.0f", ImGuiSliderFlags_Logarithmic))
        g_maxSize = ms, gn::config::setFloat("volumes.max_size", ms);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zones bigger than this (music, city alert, streaming) are left out, so the small triggers in them show.");
    int l = g_labels;
    if (ImGui::SliderInt("Labels on the nearest", &l, 0, 30)) g_labels = l, gn::config::setFloat("volumes.labels", float(l));
    if (!g_on) return;
    bool pick = g_pick;
    if (ImGui::Checkbox("Pick: the one in the middle of the screen lights up", &pick)) togglePick();
    if (g_pick) {
        if (ImGui::Button("Log the one in the middle of the screen")) logAimed();
        ImGui::SameLine();
        ImGui::TextDisabled("(white; the free camera helps aim)");
    }
    if (ImGui::Button("Log the nearest triggers")) g_logWanted = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(what each reaches, to gridnode.log)");
    ImGui::TextDisabled("%d triggers in the level, %d with shapes%s", g_total.load(), g_withShapes.load(),
                        g_pending ? " (still sorting the near ones)" : "");
    {
        // Where GridNode says the camera is (the volumes are drawn from it), next to Faith: a stale or
        // missing view is the first thing to rule out when they do not show.
        gn::ui::View v;
        gn::Vec3 at{};
        void* faith = gn::localClientCharacter();
        const bool haveFaith = faith && gn::positionOf(faith, at);
        if (gn::ui::view(v))
            ImGui::TextDisabled("view from (%.1f, %.1f, %.1f), %.0f x %.0f; Faith at (%.1f, %.1f, %.1f)", v.position[0], v.position[1], v.position[2],
                                v.width, v.height, haveFaith ? at.x : 0.f, haveFaith ? at.y : 0.f, haveFaith ? at.z : 0.f);
        else
            ImGui::TextDisabled("no view (GridNode has no camera now): nothing can be drawn");
    }
    ImGui::SeparatorText("Nearest shown (hover to light one up; Log: everything it does, to gridnode.log)");
    std::lock_guard<std::mutex> lock(g_shownM);
    int n = 0;
    for (const Shown& s : g_shown) {
        if (n++ >= 20) break;
        ImGui::PushStyleColor(ImGuiCol_Text, s.colour);
        ImGui::PushID(s.e);
        if (ImGui::SmallButton("Log")) g_describeWanted = s.e;
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::Text("%5.0f m  %s%s%s", s.dist, kCatInfo[s.primary].label, s.why.empty() ? "" : ": ", s.why.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) g_highlight = s.e;
    }
    if (g_shown.empty()) ImGui::TextDisabled("none within %.0f m", g_radius.load());
}

} // namespace volumes
