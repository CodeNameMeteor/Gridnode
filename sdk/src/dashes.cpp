// Custom dashes (experimental, not in the API yet): the game's own dash (Gameplay/Social/PF_RunnersRoute) with a
// free-roam marker, a start box, an end circle and guide arrows the player places. Worked out with the claim lab
// (networked-dash) and tested there in game on 2026-11-13; docs/research/custom-dashes.md has the details.
//
// A dash is built in GridNode's memory as a copy of one the city places (mainlogic links each dash's parts:
// the dash prefab, the start box it links as StartGeometry, an area box (0x32107415), the end arrow whose
// EndVolume is the finish, a start arrow and medium arrows it turns on with Activated, and the challenge its
// ChallengeSelector picks). Our blueprint holds the copies and those connections; a reference to it with
// StreamRealm 2 makes a ServerBlueprintEntity, placed under a bus the client knows (Faith's root: its owner
// has a ghost) and streamed in, so the client builds its half from the network like a prefab the level placed.
// Two things make the network work: every object of ours is in the data registry (DataRegistry_register,
// under a bundle id of ours), and every copy carries its 16-byte GUID in front of it (the shared content
// manager finds the running challenge by it: without it the end screen could not be left).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

#include <windows.h>

// unverified: entityCreator_spawnIntoBus (also used by the layout editor; game-tested with custom dashes).

namespace gn::host {
namespace {

namespace e = gn::engine;

constexpr const char* kWorld = "levels/sp/sp_maincity/mainlogic/mainlogic"; // the city's dashes are linked here
constexpr const char* kDashAsset = "gameplay/social/pf_runnersroute";
constexpr uint32_t kStreamIn = 0xd27bc17e, kStreamOut = 0x21f3f677; // ServerBlueprintEntity events
constexpr uint16_t kFreeSlot = 0x7d3, kFreeId = 0x7d2;               // DataRegistry_SlotOfId / IdOfSlot
constexpr uint32_t kContentIdBase = 0xf1d00000u;                     // our dashes' ContentIds (the game's are name hashes)
// A destroyed dash's data stays registered this long: its client half is taken down from the network.
constexpr auto kUnregisterDelay = std::chrono::seconds(5);

// Frostbite's field and event name hash (djb2-xor from 5381).
constexpr uint32_t fieldHash(const char* s) {
    uint32_t h = 5381;
    while (*s) h = (h * 33u) ^ static_cast<uint8_t>(*s++);
    return h;
}
const uint32_t kActivated = fieldHash("Activated"), kStartGeometry = fieldHash("StartGeometry"), kStopGeometry = fieldHash("StopGeometry"),
               kChallengeSelector = fieldHash("ChallengeSelector");
constexpr uint32_t kAreaGeometry = 0x32107415; // a box beside the marker (name unknown)

// ---- small helpers -------------------------------------------------------------------------------
bool plausible(uintptr_t p) {
    uint64_t v = 0;
    return p > 0x10000 && (p & 7) == 0 && p < 0x7ff000000000ull && mem::read(p, v);
}
const reflect::Type* typeOf(uintptr_t p) { return p ? registry().typeOf(mem::ProcessMemory(), p) : nullptr; }
std::string typeName(uintptr_t p) {
    const reflect::Type* t = typeOf(p);
    return t ? t->name : std::string();
}
std::string lowerText(uintptr_t p) { // a CString, lower case
    char b[160] = {};
    if (!plausible(p & ~7ull) || !mem::read(p, b, sizeof(b) - 1)) return {};
    for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return b;
}
// An asset's blueprint name (Asset.Name, +0x10), lower case.
std::string blueprintName(uintptr_t ref) { return lowerText(mem::readOr<uintptr_t>(mem::readOr<uintptr_t>(ref + 0x18) + 0x10)); }

bool isBus(uintptr_t bus) {
    if (!plausible(bus)) return false;
    const uintptr_t owner = mem::readOr<uintptr_t>(bus);
    const int32_t refs = mem::readOr<int32_t>(bus + 0x30);
    return plausible(owner) && mem::inImage(mem::readOr<uintptr_t>(owner)) && refs > 0 && refs < (1 << 24);
}
// The top of Faith's bus chain on the server; usable when its owner has a ghost (+0x40): the client knows
// that bus by the ghost and its id (bus-network-ids; game-tested 2026-11-13).
uintptr_t parentBus() {
    const uintptr_t faith = reinterpret_cast<uintptr_t>(localServerCharacter());
    uintptr_t bus = faith ? mem::readOr<uintptr_t>(faith + 0x28) : 0;
    if (!isBus(bus)) return 0;
    for (int d = 0; d < 64; ++d) {
        const uintptr_t parent = mem::readOr<uintptr_t>(bus + 0x08);
        if (!isBus(parent) || parent == bus) break;
        bus = parent;
    }
    return mem::readOr<uintptr_t>(mem::readOr<uintptr_t>(bus) + 0x40) ? bus : 0;
}

// Rigid transforms (rows right, up, forward, then the position): the child in the parent's frame, and back.
gn_transform relative(const gn_transform& c, const gn_transform& p) {
    gn_transform r{};
    const float* pr[3] = {p.right, p.up, p.forward};
    const float* cr[4] = {c.right, c.up, c.forward, c.trans};
    float* rr[4] = {r.right, r.up, r.forward, r.trans};
    for (int i = 0; i < 4; ++i) {
        float v[3] = {cr[i][0], cr[i][1], cr[i][2]};
        if (i == 3)
            for (int k = 0; k < 3; ++k) v[k] -= p.trans[k];
        for (int j = 0; j < 3; ++j) rr[i][j] = v[0] * pr[j][0] + v[1] * pr[j][1] + v[2] * pr[j][2];
    }
    r.trans[3] = 1.f;
    return r;
}
gn_transform compose(const gn_transform& rel, const gn_transform& p) {
    gn_transform w{};
    const float* pr[3] = {p.right, p.up, p.forward};
    const float* rr[4] = {rel.right, rel.up, rel.forward, rel.trans};
    float* wr[4] = {w.right, w.up, w.forward, w.trans};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k) wr[i][k] = rr[i][0] * pr[0][k] + rr[i][1] * pr[1][k] + rr[i][2] * pr[2][k] + (i == 3 ? p.trans[k] : 0.f);
    w.trans[3] = 1.f;
    return w;
}
// t turned and placed as given, its rows as long as the template's (the city's arrows are scaled).
gn_transform keepScale(gn_transform t, const gn_transform& tpl) {
    float* rows[3] = {t.right, t.up, t.forward};
    const float* tr[3] = {tpl.right, tpl.up, tpl.forward};
    for (int i = 0; i < 3; ++i) {
        const float want = std::sqrt(tr[i][0] * tr[i][0] + tr[i][1] * tr[i][1] + tr[i][2] * tr[i][2]);
        const float have = std::sqrt(rows[i][0] * rows[i][0] + rows[i][1] * rows[i][1] + rows[i][2] * rows[i][2]);
        if (want > 1e-4f && have > 1e-4f)
            for (int k = 0; k < 3; ++k) rows[i][k] *= want / have;
    }
    return t;
}
gn_transform clean(gn_transform t) {
    t.right[3] = t.up[3] = t.forward[3] = 0.f, t.trans[3] = 1.f;
    return t;
}

// ---- memory that is never freed --------------------------------------------------------------------
// The game reads our data while the dash lives and the registry keeps pointers to it; a level end does not
// say when the last client half is gone. A dash is a few kilobytes.
// A data object with flag 0x100 at +0x0a has its GUID in the 16 bytes before it: a copy gets one too, the
// source's or a new one (newGuid).
uint8_t* copyData(uintptr_t src, size_t size, bool newGuid = false) {
    uint8_t* block = static_cast<uint8_t*>(_aligned_malloc(0x10 + size, 16));
    if (!block) return nullptr;
    std::memset(block, 0, 0x10 + size);
    uint8_t* p = block + 0x10;
    if (!mem::read(src, p, size)) return nullptr;
    if (mem::readOr<uint16_t>(src + 0x0a) & 0x100) {
        mem::read(src - 0x10, block, 0x10);
        if (newGuid) {
            static std::atomic<uint32_t> counter{0};
            const uint32_t g[4] = {0x47524944u /* "GRID" */, 0x4e4f4445u /* "NODE" */, GetCurrentProcessId(), ++counter};
            std::memcpy(block, g, sizeof(g));
        }
    }
    *reinterpret_cast<uint16_t*>(p + 8) = 1; // our reference: never released to 0
    return p;
}
void* createData(const char* typeName) {
    const reflect::Type* t = registry().find(typeName);
    if (!t || !t->createFn || !t->size) return nullptr;
    void* mem = _aligned_malloc(t->size, 16);
    if (!mem) return nullptr;
    std::memset(mem, 0, t->size);
    void* obj = reinterpret_cast<void* (*)(void*, void*, bool)>(t->createFn)(mem, nullptr, false);
    if (obj) *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(obj) + 0x08) = 1;
    return obj;
}
// An engine array: the count at -4 (data-arrays c1).
uint8_t* engineArray(uint32_t count, size_t stride) {
    uint8_t* block = static_cast<uint8_t*>(_aligned_malloc(8 + count * stride + 8, 16));
    if (!block) return nullptr;
    std::memset(block, 0, 8 + count * stride + 8);
    *reinterpret_cast<uint32_t*>(block + 4) = count;
    return block + 8;
}
std::mutex g_textM;
std::deque<std::string> g_texts; // (g_textM) strings handed to the game (never freed: the game keeps the pointers)
const char* keptText(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_textM);
    g_texts.push_back(s);
    return g_texts.back().c_str();
}

// ---- the level's own dash, to copy from ------------------------------------------------------------
// Blueprint (DataBusData / EntityBusData / PrefabBlueprint, dump): +0x18 PropertyConnections (0x20 each, Flags
// at +0x18), +0x20 LinkConnections (0x18), +0x38 EventConnections (0x20, TargetType at +0x18), +0x40 Objects.
constexpr uint32_t kConnOff[3] = {0x18, 0x20, 0x38}, kConnStride[3] = {0x20, 0x18, 0x20};
enum Role { RDash, RArea, RStart, REnd, RStartArrow, RMedium, RSelector, RCount };

struct TConn { // a connection of the template dash, by role
    int kind;
    Role src, dst;
    uint32_t sf, tf, extra;
};
struct Templates {
    uint64_t level = 0;
    uintptr_t dashBp = 0;
    uintptr_t obj[RCount] = {};
    std::vector<TConn> conns; // the medium arrow's once: made for every guide arrow
    std::string missing;      // why there is none
    std::map<std::string, uintptr_t> arrowObj; // an arrow kind (kArrowBlueprint) -> one of the city's dashes' arrows of it
    std::map<std::string, uintptr_t> arrowBp;  // an arrow kind no city dash uses -> its blueprint (in the city's bundle)
};

// Guide arrow kinds (DashArrowSpec::kind): the blueprint name's end; turn right is turn left mirrored (the city's
// bundle has no turn-right or large arrow: belikewater has, not loaded in free roam).
const std::vector<std::string> kArrowNames = {"medium", "small", "turn left", "turn right"};
const char* const kArrowBlueprint[] = {"dasharrow_medium", "dasharrow_small", "dasharrow_turnleft", "dasharrow_turnleft"};
std::atomic<int32_t> g_tplStars[3] = {{0}, {0}, {0}};
std::mutex g_tplM;
Templates g_tpl; // (g_tplM)
std::atomic<uint64_t> g_level{1};
// Which axis of an arrow's transform its model points along (0 +forward, 1 +right, 2 -forward, 3 -right; -1 not
// known yet): the axis of the city's arrows that best points from each arrow toward its dash's end circle (they
// are turned along the route). Not forward: the editor's facing was 90 degrees off (user, 2026-11-13).
std::atomic<int> g_arrowAxis{-1};

void learnArrowAxis(const std::vector<uintptr_t>& arrows, uintptr_t end) {
    if (arrows.empty() || !end) return;
    gn_transform e{};
    mem::read(end + 0x20, &e, sizeof(e)); // ObjectReferenceObjectData.BlueprintTransform
    float score[4] = {0.f, 0.f, 0.f, 0.f};
    for (uintptr_t a : arrows) {
        gn_transform t{};
        if (!mem::read(a + 0x20, &t, sizeof(t))) continue;
        float d[2] = {e.trans[0] - t.trans[0], e.trans[2] - t.trans[2]};
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1]);
        if (dl < 1.f) continue;
        const float* rows[2] = {t.forward, t.right};
        for (int i = 0; i < 2; ++i) {
            const float l = std::sqrt(rows[i][0] * rows[i][0] + rows[i][2] * rows[i][2]);
            if (l < 1e-4f) continue;
            const float dot = (rows[i][0] * d[0] + rows[i][2] * d[1]) / (l * dl);
            score[i] += dot, score[i + 2] -= dot;
        }
    }
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (score[i] > score[best]) best = i;
    static const char* names[4] = {"+forward", "+right", "-forward", "-right"};
    if (g_arrowAxis.exchange(best) != best)
        GN_LOG("dashes: arrows point along their %s axis (from %u of the city's; scores %.1f %.1f %.1f %.1f)", names[best], static_cast<unsigned>(arrows.size()),
               score[0], score[1], score[2], score[3]);
}

Templates findTemplates() { // server thread
    Templates t;
    t.level = g_level.load();
    void* w = assetFind(kWorld, nullptr);
    void* dbp = assetFind(kDashAsset, nullptr);
    if (w) assetRelease(w); // the level holds both
    if (dbp) assetRelease(dbp);
    if (!w || !dbp) {
        t.missing = "no dash to copy from here (dashes are made from the city's own; load the city in free roam)";
        return t;
    }
    t.dashBp = reinterpret_cast<uintptr_t>(dbp);
    const uintptr_t world = reinterpret_cast<uintptr_t>(w);
    struct C {
        int kind;
        uintptr_t src, dst;
        uint32_t sf, tf, extra;
    };
    std::vector<C> all;
    for (int k = 0; k < 3; ++k) {
        const uintptr_t a = mem::readOr<uintptr_t>(world + kConnOff[k]);
        const uint32_t n = a ? mem::readOr<uint32_t>(a - 4) : 0;
        for (uint32_t i = 0; i < n && i < 200000; ++i) {
            const uintptr_t r = a + uintptr_t(i) * kConnStride[k];
            all.push_back({k, mem::readOr<uintptr_t>(r), mem::readOr<uintptr_t>(r + 8), mem::readOr<uint32_t>(r + 0x10), mem::readOr<uint32_t>(r + 0x14),
                           k == 1 ? 0u : mem::readOr<uint32_t>(r + 0x18)});
        }
    }
    // Every arrow kind the city's dashes turn on (Activated), for its object (its scale) to copy; and how many start
    // boxes (StartGeometry links) a dash has at most.
    std::map<uintptr_t, int> starts;
    for (const C& c : all) {
        if (!c.src || !c.dst || mem::readOr<uintptr_t>(c.src + 0x18) != t.dashBp) continue;
        if (c.kind == 1 && c.sf == kStartGeometry) ++starts[c.src];
        if (c.kind != 0 || c.sf != kActivated) continue;
        const std::string n = blueprintName(c.dst);
        for (const char* k : kArrowBlueprint)
            if (n.size() >= std::strlen(k) && n.compare(n.size() - std::strlen(k), std::string::npos, k) == 0 && !t.arrowObj.count(k)) t.arrowObj[k] = c.dst;
    }
    int most = 0;
    for (const auto& [ref, n] : starts) most = std::max(most, n);
    uint32_t worldGroup = 0;
    if (void* w2 = assetFind(kWorld, &worldGroup)) assetRelease(w2);
    for (const char* k : kArrowBlueprint) {
        if (t.arrowObj.count(k) || t.arrowBp.count(k)) continue;
        uint32_t g = 0;
        void* bp = assetFind((std::string("objects/props/runnerobject/dasharrows/pf_") + k).c_str(), &g);
        if (!bp) continue;
        assetRelease(bp); // the level holds it (only taken from the city's own group)
        if (g == worldGroup) t.arrowBp[k] = reinterpret_cast<uintptr_t>(bp);
    }
    {
        std::string kinds;
        for (const auto& [k, o] : t.arrowObj) kinds += std::string(" ") + k;
        for (const auto& [k, o] : t.arrowBp) kinds += std::string(" ") + k + "(blueprint)";
        GN_LOG("dashes: arrow kinds in the city's data:%s; a dash has at most %d start box(es)", kinds.empty() ? " none" : kinds.c_str(), most);
    }
    // The first dash with all its parts.
    std::vector<uintptr_t> tried;
    for (const C& c0 : all) {
        const uintptr_t ref = c0.src;
        if (!ref || mem::readOr<uintptr_t>(ref + 0x18) != t.dashBp || typeName(ref) != "SpatialPrefabReferenceObjectData") continue;
        if (std::find(tried.begin(), tried.end(), ref) != tried.end()) continue;
        tried.push_back(ref);
        Templates d = t;
        d.obj[RDash] = ref;
        std::map<uintptr_t, Role> role{{ref, RDash}};
        for (const C& c : all) {
            if (c.src != ref || !c.dst) continue;
            if (c.kind == 1 && c.sf == kStartGeometry) role[c.dst] = RStart;
            else if (c.kind == 1 && c.sf == kAreaGeometry) role[c.dst] = RArea;
            else if (c.kind == 1 && c.sf == kStopGeometry) role[c.dst] = REnd;
            else if (c.kind == 1 && c.sf == kChallengeSelector) role[c.dst] = RSelector;
            else if (c.kind == 0 && c.sf == kActivated) {
                const std::string n = blueprintName(c.dst);
                if (n.find("dasharrow_start") != std::string::npos) role[c.dst] = RStartArrow;
                else if (n.find("dasharrow_medium") != std::string::npos) role[c.dst] = RMedium;
                else if (n.find("dasharrow_end") != std::string::npos) role[c.dst] = REnd;
            }
        }
        for (const auto& [o, r] : role)
            if (!d.obj[r]) d.obj[r] = o; // the medium arrow template: the first of them
        // The medium arrow used as the template: the first; connections to the others are left out.
        for (const C& c : all) {
            const auto s = role.find(c.src), t2 = role.find(c.dst);
            if (s == role.end() || t2 == role.end()) continue;
            if ((s->second == RMedium && c.src != d.obj[RMedium]) || (t2->second == RMedium && c.dst != d.obj[RMedium])) continue;
            d.conns.push_back({c.kind, s->second, t2->second, c.sf, c.tf, c.extra});
        }
        const bool selectorOk = typeName(d.obj[RSelector]) == "PamNamedChallengeSelectorEntityData" && plausible(mem::readOr<uintptr_t>(d.obj[RSelector] + 0x18));
        std::vector<uintptr_t> mediums;
        for (const auto& [o, r] : role)
            if (r == RMedium) mediums.push_back(o);
        learnArrowAxis(mediums, d.obj[REnd]);
        if (d.obj[RStart] && d.obj[REnd] && d.obj[RStartArrow] && d.obj[RMedium] && d.obj[RArea] && selectorOk) {
            const uintptr_t tch = mem::readOr<uintptr_t>(d.obj[RSelector] + 0x18);
            for (int k = 0; k < 3; ++k) g_tplStars[k] = mem::readOr<int32_t>(tch + 0x18 + 4 * k);
            GN_LOG("dashes: the template's stars at %.2f / %.2f / %.2f s, cancel at %.2f s (InternalThreshold1..3Star, Cancel)", mem::readOr<int32_t>(tch + 0x18) / 100.0,
                   mem::readOr<int32_t>(tch + 0x1c) / 100.0, mem::readOr<int32_t>(tch + 0x20) / 100.0, mem::readOr<int32_t>(tch + 0x24) / 100.0);
            GN_LOG("dashes: copying the city's dash at (%.0f, %.0f, %.0f): %u connections", mem::readOr<float>(ref + 0x50), mem::readOr<float>(ref + 0x54),
                   mem::readOr<float>(ref + 0x58), static_cast<unsigned>(d.conns.size()));
            return d;
        }
    }
    t.missing = "none of the city's dashes has all its parts loaded (see the log)";
    GN_LOG("dashes: %u dash references in %s, none with a start, end, start arrow, medium arrow, area box and challenge", static_cast<unsigned>(tried.size()),
           kWorld);
    return t;
}

const Templates* templates() { // server thread; (g_tplM) held by the caller
    if (g_tpl.level != g_level.load() || !g_tpl.missing.empty()) g_tpl = findTemplates(); // again while there is none (a level loads in parts)
    return &g_tpl;
}

// ---- the dashes ---------------------------------------------------------------------------------
struct Dash {
    uint32_t handle = 0;
    uint64_t level = 0;
    DashSpec spec;
    uintptr_t entity = 0, bus = 0; // our ServerBlueprintEntity and the bus it is under (server thread)
    uintptr_t challenge = 0;       // our challenge copy: the stats hook knows our finishes by it
    uint16_t regId = 0;            // our bundle id in the data registry
    enum State { Queued, Live, Failed } state = Queued;
    std::string status = "being made";
    bool finished = false;
    int64_t lastTime = 0; // centiseconds
    uintptr_t challengeEnt = 0; // its PamServerNamedChallengeEntity (+0x80 = our challenge, +0x78 state), found once
    std::chrono::steady_clock::time_point lookedAt{};
    int64_t best = -1;          // GridNode's best (the layout's, dashSetBest), centiseconds; -1 none
    uintptr_t marker = 0;       // our map marker (client), hidden when the dash goes
};
struct PendingId {
    uint16_t id;
    uint64_t level;
    std::chrono::steady_clock::time_point at;
};

std::mutex g_m;
std::vector<Dash> g_dashes;       // (g_m)
std::vector<PendingId> g_unreg;   // (g_m) ids to take out of the registry later
std::vector<uint16_t> g_usedIds;  // (g_m) ids registered and not yet taken out
std::vector<uintptr_t> g_liveEnts; // (g_m) our ServerBlueprintEntities not yet destroyed (entityGone takes them out)
uint32_t g_next = 1;              // (g_m)
std::once_flag g_once;

Dash* dashOf(uint32_t h) {
    for (Dash& d : g_dashes)
        if (d.handle == h) return &d;
    return nullptr;
}

// (g_m) A free bundle id: its slot unused both ways and not one of ours waiting. The game's bundles take low ids
// (3..46 seen), so the search goes down from 1903.
uint16_t freeId() {
    const uintptr_t slots = mem::rva(e::global::DataRegistry_SlotOfId), back = mem::rva(e::global::DataRegistry_IdOfSlot);
    for (uint32_t i = 1900; i > 100; --i) {
        const uint16_t id = static_cast<uint16_t>(i + 3);
        if (std::find(g_usedIds.begin(), g_usedIds.end(), id) != g_usedIds.end()) continue;
        if (mem::readOr<uint16_t>(slots + 2 * i) == kFreeSlot && mem::readOr<uint16_t>(back + 2 * i) == kFreeId) return id;
    }
    return 0;
}

void unregister(uint16_t id) {
    reinterpret_cast<void (*)(uint16_t)>(mem::rva(e::function::DataRegistry_unregister))(id);
    std::lock_guard<std::mutex> lock(g_m);
    g_usedIds.erase(std::remove(g_usedIds.begin(), g_usedIds.end(), id), g_usedIds.end());
}

// ---- the stats hook ---------------------------------------------------------------------------------
// NamedChallengeStats_onFinished writes new bests into DashState's maps (+0x38 + player * 0x30, keyed by the
// challenge's stat names) with a pending list of changes (+0x218 flag, +0x220.., saved later), and queues the
// online upload for player index 1 (its active byte at *(dashState +0x10) + 1). For a custom dash it runs as
// usual (the end screen needs it: with stat keys of our own, restart and exit did nothing, 2026-11-13) with
// index 1 off, so nothing is sent; then every best it changed and the pending list are put back, as the MEC
// trainer's anti-cheat does (game-tested). GridNode keeps the time.
using StatsFn = void (*)(void* dashState, void* challengeData, void* times, uint32_t runId);
StatsFn g_statsOrig = nullptr;
std::atomic<bool> g_statsHooked{false};

struct BestValue {
    uintptr_t at; // a map node's value (+0x10)
    int64_t value;
    bool main;    // the main stat's (the time)
};
// The values in DashState's maps for the challenge's stats (main +0x58, total stars +0x70, extras +0x88), per player.
std::vector<BestValue> bestsOf(uintptr_t state, uintptr_t data) {
    std::vector<BestValue> out;
    std::vector<const char*> keys;
    const char* mainKey = reinterpret_cast<const char*>(mem::readOr<uintptr_t>(data + 0x58));
    for (const uint32_t off : {0x58u, 0x70u})
        if (const char* k = reinterpret_cast<const char*>(mem::readOr<uintptr_t>(data + off))) keys.push_back(k);
    if (const uintptr_t extras = mem::readOr<uintptr_t>(data + 0x88)) {
        const uint32_t n = mem::readOr<uint32_t>(extras - 4);
        for (uint32_t i = 0; i < n && i < 16; ++i)
            if (const char* k = reinterpret_cast<const char*>(mem::readOr<uintptr_t>(extras + i * 0x18ull))) keys.push_back(k);
    }
    const uintptr_t maps = mem::readOr<uintptr_t>(state + 0x38);
    if (!maps) return out;
    using FindFn = void** (*)(uintptr_t map, void** out, const char** key);
    const auto find = reinterpret_cast<FindFn>(mem::rva(e::function::DashStats_find));
    for (int player = 0; player < 2; ++player) {
        const uintptr_t map = maps + player * 0x30ull;
        const uintptr_t buckets = mem::readOr<uintptr_t>(map + 8);
        const uint32_t count = mem::readOr<uint32_t>(map + 0x10);
        if (!buckets || !count || count > 1000000) continue;
        const uintptr_t end = mem::readOr<uintptr_t>(buckets + uintptr_t(count) * 8);
        for (const char* key : keys) {
            void* it[2] = {};
            find(map, it, &key);
            const uintptr_t node = reinterpret_cast<uintptr_t>(it[0]);
            if (!node || node == end) continue;
            BestValue b{node + 0x10, 0, key == mainKey};
            if (mem::read(b.at, b.value)) out.push_back(b);
        }
    }
    return out;
}

// No best yet: a time no run beats the other way (the game's "none" for a player with no best, 99999.99 s).
constexpr int64_t kNoBest = 9999999;

void statsHook(void* state, void* data, void* times, uint32_t run) {
    uint32_t mine = 0;
    int64_t ours = -1; // GridNode's best for the dash
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const Dash& d : g_dashes)
            if (d.challenge && d.challenge == reinterpret_cast<uintptr_t>(data)) {
                mine = d.handle;
                ours = d.best;
                if (d.lastTime > 0 && (ours <= 0 || d.lastTime < ours)) ours = d.lastTime;
            }
    }
    if (!mine || !state || unloaded()) return g_statsOrig(state, data, times, run);
    int64_t t = -1;
    const uintptr_t begin = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(times));
    if (begin) t = mem::readOr<int64_t>(begin, -1);
    const uintptr_t st = reinterpret_cast<uintptr_t>(state);
    const std::vector<BestValue> before = bestsOf(st, reinterpret_cast<uintptr_t>(data));
    uint8_t pending[0x60] = {};
    const bool havePending = mem::read(st + 0x218, pending, sizeof(pending));
    const uintptr_t active = mem::readOr<uintptr_t>(st + 0x10);
    uint8_t was = 0;
    const bool flag = active && mem::read(active + 1, was);
    if (flag && was) mem::write(active + 1, uint8_t{0});
    // The run is compared with GridNode's best, not the template's: the finished message (the end screen's record,
    // the difference, new record or not) is made in the call from the best it finds (2026-11-13: a custom dash was
    // compared with the template's 13.96 s). The template's bests are written back after.
    const int64_t swapIn = ours > 0 ? ours : kNoBest;
    for (const BestValue& bv : before)
        if (bv.main) mem::write(bv.at, swapIn);
    g_statsOrig(state, data, times, run);
    if (flag && was) mem::write(active + 1, was);
    bool changed = false;
    for (const BestValue& bv : before) {
        int64_t now = 0;
        if (mem::read(bv.at, now) && now != (bv.main ? swapIn : bv.value)) changed = true;
        mem::write(bv.at, bv.value);
    }
    if (changed && havePending) mem::write(st + 0x218, pending, sizeof(pending));
    std::lock_guard<std::mutex> lock(g_m);
    if (Dash* d = dashOf(mine)) {
        if (t > 0) d->finished = true, d->lastTime = t;
        GN_LOG("dashes: dash %u finished in %.2f s, compared with GridNode's best %s (kept by GridNode; nothing saved to the game's stats or sent online%s)", mine,
               t / 100.0, ours > 0 ? (std::to_string(ours / 100) + "." + std::to_string(ours % 100 / 10) + std::to_string(ours % 10) + " s").c_str() : "(none)",
               changed ? "; the best it wrote was put back" : "");
    }
}

// The best the client shows (the dash's own screens, its marker's stars): the server sends DashState's best for the
// challenge's stat key, which for a custom dash is the template's (dash-end-screen-best c1, c2). The client stores it
// with PamClientNamedChallengeEntity_storeResult (entity +0x58 = the challenge): for ours, GridNode's best is given
// instead (none: no best).
using StoreFn = void (*)(void* entity, int32_t player, int64_t best, char hasBest);
StoreFn g_storeOrig = nullptr;

void storeHook(void* entity, int32_t player, int64_t best, char hasBest) {
    if (entity && !unloaded()) {
        const uintptr_t ch = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(entity) + 0x58);
        std::lock_guard<std::mutex> lock(g_m);
        for (const Dash& d : g_dashes)
            if (ch && d.challenge == ch) {
                int64_t b = d.best;
                if (d.lastTime > 0 && (b <= 0 || d.lastTime < b)) b = d.lastTime;
                static int said = 0;
                if (said++ < 20)
                    GN_LOG("dashes: dash %u: the client shows best %.2f s for player %d (the game sent %.2f s%s)", d.handle, b > 0 ? b / 100.0 : 0.0, player,
                           best / 100.0, hasBest ? "" : ", none");
                best = b > 0 ? b : 0, hasBest = b > 0 ? 1 : 0;
                break;
            }
    }
    g_storeOrig(entity, player, best, hasBest);
}

void hookStats() {
    static std::atomic<bool> storeTried{false};
    if (!storeTried.exchange(true) &&
        hookFunction("GridNode", e::function::PamClientNamedChallengeEntity_storeResult, reinterpret_cast<void*>(&storeHook), reinterpret_cast<void**>(&g_storeOrig)) != GN_OK)
        GN_LOG("dashes: could not hook PamClientNamedChallengeEntity_storeResult: custom dashes show the template dash's best");
    if (g_statsHooked.exchange(true)) return;
    if (hookFunction("GridNode", e::function::NamedChallengeStats_onFinished, reinterpret_cast<void*>(&statsHook), reinterpret_cast<void**>(&g_statsOrig)) != GN_OK) {
        g_statsHooked = false;
        GN_LOG("dashes: could not hook NamedChallengeStats_onFinished: custom dashes are not made (their times could reach the game's stats)");
    }
}

// ---- building one (server thread) ------------------------------------------------------------------
int32_t transformOffset(uintptr_t o) { // BlueprintTransform (references) or Transform (shapes)
    const reflect::Type* t = typeOf(o);
    reflect::FieldInfo fi;
    if (t && (registry().findField(t, "BlueprintTransform", fi) || registry().findField(t, "Transform", fi))) return static_cast<int32_t>(fi.field->offset);
    return -1;
}
gn_transform transformOf(uintptr_t o) {
    gn_transform t{};
    const int32_t off = transformOffset(o);
    if (off >= 0) mem::read(o + off, &t, sizeof(t));
    return clean(t);
}

struct Built {
    std::vector<uintptr_t> objects; // ours, for the registry
    uintptr_t outer = 0, challenge = 0;
    std::string error;
};

Built build(const Templates& tp, const DashSpec& spec, uint32_t handle) {
    Built b;
    auto fail = [&](const char* why) {
        b.error = why;
        return b;
    };
    auto copyOf = [&](uintptr_t src, bool newGuid = false) -> uint8_t* {
        const reflect::Type* t = typeOf(src);
        if (!t || t->size < 0x10 || t->size > 0x400) return nullptr;
        uint8_t* c = copyData(src, t->size, newGuid);
        if (c) b.objects.push_back(reinterpret_cast<uintptr_t>(c));
        return c;
    };
    auto place = [&](uint8_t* c, uintptr_t src, const gn_transform& t) {
        const int32_t off = transformOffset(src);
        if (off < 0) return;
        const gn_transform v = clean(t);
        std::memcpy(c + off, &v, sizeof(v));
    };

    // The challenge: the template's, a new GUID, nothing online (see statsHook for the stats).
    const uintptr_t tch = mem::readOr<uintptr_t>(tp.obj[RSelector] + 0x18);
    uint8_t* ch = copyOf(tch, true);
    uint8_t* type = ch ? copyOf(mem::readOr<uintptr_t>(tch + 0x28)) : nullptr;
    uint8_t* sel = static_cast<uint8_t*>(createData("PamNamedChallengeSelectorEntityData"));
    if (!ch || !type || !sel) return fail("could not copy the challenge");
    b.objects.push_back(reinterpret_cast<uintptr_t>(sel));
    type[0x9a] = 0, type[0x9d] = 0, type[0x9f] = 0; // RequiresOnline, HasDuration (the Started message, the replay), SendRefreshLeaderboardMessage
    // A ContentId (+0x98) of its own: the client's shared content manager keys map markers by it (findMarker, also
    // from the client challenge entity's constructor), so with the template's the real dash's marker was found and
    // ours could not be added (dash-map-register c5, c6). markOnClient moves it on if one is taken. Shown on the
    // map (HideInMap +0x9c off).
    static std::atomic<uint32_t> contentIds{0};
    *reinterpret_cast<uint32_t*>(ch + 0x98) = kContentIdBase + (++contentIds & 0xfffff) * 16;
    ch[0x9c] = 0;
    *reinterpret_cast<uint8_t**>(ch + 0x28) = type;
    *reinterpret_cast<uint8_t**>(sel + 0x18) = ch;
    // StatsData keeps the template's stat keys: the end screen needs them (statsHook puts the game's bests back).
    // The name: the localizer translates only ids starting with "ID_" and gives any other text back as it is
    // (dash-name c6, c7), so the typed name is the Sid (+0x10). The free-roam marker reads it there.
    if (!spec.name.empty()) {
        std::string n = spec.name.substr(0, 60);
        if (n.compare(0, 3, "ID_") == 0) n.insert(0, " "); // never taken for an id
        *reinterpret_cast<const char**>(ch + 0x10) = keptText(n);
    }
    // Stars (InternalThreshold1Star..3Star, ints in centiseconds; lower is better: NamedChallenge_starsForValue).
    if (spec.stars[0] > 0 && spec.stars[1] > 0 && spec.stars[2] > 0) {
        int32_t st[3] = {spec.stars[0], spec.stars[1], spec.stars[2]};
        std::sort(st, st + 3, std::greater<int32_t>()); // 1 star the slowest
        std::memcpy(ch + 0x18, st, sizeof(st));
        GN_LOG("dashes: dash %u: stars at %.2f / %.2f / %.2f s", handle, st[0] / 100.0, st[1] / 100.0, st[2] / 100.0);
    }
    // InternalThresholdCancel (+0x24): what it does is not read yet (a run longer than it cancelled?). A custom dash
    // may be longer than the template, so it is never below 3 minutes or twice the 1-star time.
    if (int32_t& cancel = *reinterpret_cast<int32_t*>(ch + 0x24); cancel > 0)
        cancel = std::max({cancel, 18000, *reinterpret_cast<int32_t*>(ch + 0x18) * 2});
    b.challenge = reinterpret_cast<uintptr_t>(ch);

    // The parts.
    const gn_transform tDash = transformOf(tp.obj[RDash]), tStart = transformOf(tp.obj[RStart]);
    std::vector<std::pair<Role, uint8_t*>> parts;
    uint8_t* dash = copyOf(tp.obj[RDash]);
    uint8_t* area = copyOf(tp.obj[RArea]);
    uint8_t* start = copyOf(tp.obj[RStart]);
    uint8_t* startArrow = copyOf(tp.obj[RStartArrow]);
    uint8_t* end = copyOf(tp.obj[REnd]);
    if (!dash || !area || !start || !startArrow || !end) return fail("could not copy the dash's parts");
    *reinterpret_cast<uint32_t*>(dash + 0x10) |= 0x60000000u; // links from it are followed in both realms (blueprint-wiring c4)
    place(dash, tp.obj[RDash], spec.marker);
    place(area, tp.obj[RArea], compose(relative(transformOf(tp.obj[RArea]), tDash), spec.marker)); // stays with the marker
    place(start, tp.obj[RStart], spec.start);
    for (int k = 0; k < 3; ++k) reinterpret_cast<float*>(start + 0x60)[k] = std::clamp(spec.startHalf[k], 0.1f, 100.f); // OBBData.HalfExtents
    std::vector<uint8_t*> starts{start};
    for (const DashStartSpec& more : spec.moreStarts) {
        uint8_t* c = copyOf(tp.obj[RStart]);
        if (!c) return fail("could not copy a start box");
        place(c, tp.obj[RStart], more.at);
        for (int k = 0; k < 3; ++k) reinterpret_cast<float*>(c + 0x60)[k] = std::clamp(more.half[k], 0.1f, 100.f);
        starts.push_back(c);
    }
    const gn_transform arrowAt = spec.haveStartLine ? keepScale(spec.startLine, transformOf(tp.obj[RStartArrow]))
                                                    : compose(relative(transformOf(tp.obj[RStartArrow]), tStart), spec.start);
    place(startArrow, tp.obj[RStartArrow], arrowAt);
    GN_LOG("dashes: dash %u: start arrow at (%.1f, %.1f, %.1f), start box at (%.1f, %.1f, %.1f)", handle, arrowAt.trans[0], arrowAt.trans[1], arrowAt.trans[2],
           spec.start.trans[0], spec.start.trans[1], spec.start.trans[2]);
    place(end, tp.obj[REnd], keepScale(spec.end, transformOf(tp.obj[REnd])));
    parts = {{RDash, dash}, {RArea, area}, {RStart, start}, {RStartArrow, startArrow}, {REnd, end}};
    std::vector<uint8_t*> arrows;
    for (const DashArrowSpec& as : spec.arrows) {
        const int kind = std::clamp(as.kind, 0, static_cast<int>(kArrowNames.size()) - 1);
        const std::string bpName = kArrowBlueprint[kind];
        uintptr_t src = tp.obj[RMedium], bp = 0;
        if (const auto o = tp.arrowObj.find(bpName); o != tp.arrowObj.end()) src = o->second;
        else if (const auto f = tp.arrowBp.find(bpName); f != tp.arrowBp.end() && typeName(f->second) == typeName(mem::readOr<uintptr_t>(src + 0x18))) bp = f->second;
        else if (kind) GN_LOG("dashes: dash %u: no %s arrow in the city's data: a medium one instead", handle, kArrowNames[kind].c_str());
        uint8_t* a = copyOf(src);
        if (!a) return fail("could not copy an arrow");
        if (bp) *reinterpret_cast<uintptr_t*>(a + 0x18) = bp; // ObjectReferenceObjectData.Blueprint
        gn_transform t = keepScale(as.at, transformOf(src));
        const float sc = std::clamp(as.scale, 0.1f, 20.f);
        float* rows[3] = {t.right, t.up, t.forward};
        for (float* r : rows)
            for (int k = 0; k < 3; ++k) r[k] *= sc;
        if (kind == 3) { // turn right: turn left mirrored across the way it points (the row beside it, flat, turned round)
            const int axis = g_arrowAxis.load();
            float* side = axis == 1 || axis == 3 ? t.forward : t.right;
            for (int k = 0; k < 3; ++k) side[k] = -side[k];
        }
        place(a, src, t);
        arrows.push_back(a);
    }

    // Our blueprint: the parts and the template's connections between them (the medium arrow's for each arrow).
    uint8_t* bp = static_cast<uint8_t*>(createData("SpatialPrefabBlueprint"));
    if (!bp) return fail("could not make a blueprint");
    b.objects.push_back(reinterpret_cast<uintptr_t>(bp));
    auto ptrOf = [&](Role r) -> uintptr_t {
        if (r == RSelector) return reinterpret_cast<uintptr_t>(sel);
        for (const auto& [pr, p] : parts)
            if (pr == r) return reinterpret_cast<uintptr_t>(p);
        return 0;
    };
    struct Out {
        int kind;
        uintptr_t s, d;
        uint32_t sf, tf, extra;
    };
    std::vector<Out> out;
    // The template's connections, for every arrow (the medium's) and every start box (the start's).
    auto all = [&](Role r) {
        std::vector<uintptr_t> v;
        if (r == RMedium)
            for (uint8_t* a : arrows) v.push_back(reinterpret_cast<uintptr_t>(a));
        else if (r == RStart)
            for (uint8_t* st : starts) v.push_back(reinterpret_cast<uintptr_t>(st));
        else if (const uintptr_t p = ptrOf(r)) v.push_back(p);
        return v;
    };
    for (const TConn& c : tp.conns)
        for (const uintptr_t s : all(c.src))
            for (const uintptr_t d : all(c.dst)) out.push_back({c.kind, s, d, c.sf, c.tf, c.extra});
    uint32_t nk[3] = {0, 0, 0};
    for (const Out& o : out) ++nk[o.kind];
    uint8_t* arr[3];
    for (int k = 0; k < 3; ++k)
        if (!(arr[k] = engineArray(nk[k], kConnStride[k]))) return fail("out of memory");
    uint32_t ik[3] = {0, 0, 0};
    for (const Out& o : out) {
        uint8_t* r = arr[o.kind] + uintptr_t(ik[o.kind]++) * kConnStride[o.kind];
        std::memcpy(r, &o.s, 8);
        std::memcpy(r + 8, &o.d, 8);
        std::memcpy(r + 0x10, &o.sf, 4);
        std::memcpy(r + 0x14, &o.tf, 4);
        if (o.kind != 1) std::memcpy(r + 0x18, &o.extra, 4);
    }
    const uint32_t nObj = static_cast<uint32_t>(parts.size() + arrows.size() + starts.size() - 1);
    uint8_t* objs = engineArray(nObj, 8);
    if (!objs) return fail("out of memory");
    uint32_t io = 0;
    for (const auto& [r, p] : parts) reinterpret_cast<uint8_t**>(objs)[io++] = p;
    for (uint8_t* a : arrows) reinterpret_cast<uint8_t**>(objs)[io++] = a;
    for (size_t i = 1; i < starts.size(); ++i) reinterpret_cast<uint8_t**>(objs)[io++] = starts[i];
    for (int k = 0; k < 3; ++k) *reinterpret_cast<uint8_t**>(bp + kConnOff[k]) = arr[k];
    *reinterpret_cast<uint16_t*>(bp + 0x30) = mem::readOr<uint16_t>(tp.dashBp + 0x30); // Flags as the dash's
    *reinterpret_cast<uint8_t**>(bp + 0x40) = objs;

    // The reference a level would hold: StreamRealm 2 makes a ServerBlueprintEntity (SpatialPrefabReference_create).
    uint8_t* outer = copyOf(tp.obj[RDash]);
    if (!outer) return fail("could not copy the dash reference");
    const gn_transform origin = clean(gn_transform{{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}, {0.f, 0.f, 0.f, 1.f}});
    *reinterpret_cast<uintptr_t*>(outer + 0x18) = reinterpret_cast<uintptr_t>(bp);
    std::memcpy(outer + 0x20, &origin, sizeof(origin));
    outer[0x83] = 0;                                           // Excluded
    *reinterpret_cast<uint32_t*>(outer + 0x10) |= 0x60000000u; // both realms
    *reinterpret_cast<int32_t*>(outer + 0x68) = 2;             // StreamRealm
    b.outer = reinterpret_cast<uintptr_t>(outer);
    return b;
}

void destroyOnServer(uintptr_t ent, uintptr_t bus, uint16_t id, uint64_t level);
void hideMarker(uintptr_t marker, uintptr_t challenge, uint64_t level);

// A dash between the server's build and its placing: the map marker is added on the client first, so the client
// challenge entity (made from the network after the placing) finds it by its ContentId.
struct Prep {
    uint32_t handle = 0;
    uint64_t level = 0;
    DashSpec spec;
    Built built;
    uint16_t id = 0;
    uintptr_t bus = 0;
    uintptr_t marker = 0;
};
void placeOnServer(std::shared_ptr<Prep> p);

// Client thread: our challenge's map marker, as PamClientSharedContentManagerEntity's vf15 adds the level's (a layout
// entry: the marker's transform, then the challenge's GUID at +0x40; the constructor copies it). Not in the manager's
// lists keyed by stat name (+0x280: ours has the template's keys) or by ContentId (+0x2b0): the readers of those
// skip a challenge they do not find (dash-map-register c10).
void markOnClient(std::shared_ptr<Prep> p) {
    if (p->level != g_level.load() || unloading()) return server().post([p] { placeOnServer(p); });
    const uintptr_t mgr = mem::readOr<uintptr_t>(mem::rva(e::global::PamClientSharedContentManager));
    uint8_t* ch = reinterpret_cast<uint8_t*>(p->built.challenge);
    const uintptr_t type = mem::readOr<uintptr_t>(p->built.challenge + 0x28);
    using FindFn = uintptr_t (*)(uintptr_t, uintptr_t);
    const auto find = reinterpret_cast<FindFn>(mem::rva(e::function::PamClientSharedContentManager_findMarker));
    if (!mgr) {
        GN_LOG("dashes: dash %u: no shared content manager on the client: no map icon", p->handle);
    } else if (!mem::readOr<uintptr_t>(type + 0x68)) {
        GN_LOG("dashes: dash %u: the template's challenge type has no map icon data (+0x68): no map icon", p->handle);
    } else {
        int tries = 0;
        while (find(mgr, p->built.challenge) && tries++ < 64) *reinterpret_cast<uint32_t*>(ch + 0x98) += 1; // taken
        alignas(16) uint8_t entry[0xc0] = {};
        const gn_transform at = clean(p->spec.marker);
        std::memcpy(entry, &at, sizeof(at));
        std::memcpy(entry + 0x40, ch - 0x10, 0x10); // the GUID copyData gave it
        using AddFn = void (*)(uintptr_t, void*, uintptr_t);
        reinterpret_cast<AddFn>(mem::rva(e::function::PamClientSharedContentManager_addChallengeMarker))(mgr, entry, p->built.challenge);
        p->marker = find(mgr, p->built.challenge);
        GN_LOG("dashes: dash %u: map marker %s (ContentId %08x; allowed %u, shown %u, unlock data %s)", p->handle, p->marker ? "added" : "NOT added",
               mem::readOr<uint32_t>(p->built.challenge + 0x98), p->marker ? mem::readOr<uint8_t>(p->marker + 0x225) : 0u,
               p->marker ? mem::readOr<uint8_t>(p->marker + 0x121) : 0u, mem::readOr<uintptr_t>(p->built.challenge + 0x40) ? "set (shown once unlocked)" : "none");
    }
    server().post([p] { placeOnServer(p); });
}

// Client thread: a marker of ours hidden (no removal function is known): +0x225 (allowed) off, then its refresh.
void hideMarker(uintptr_t marker, uintptr_t challenge, uint64_t level) {
    if (!marker || level != g_level.load()) return; // the level's manager and its markers are gone with it
    const uintptr_t mgr = mem::readOr<uintptr_t>(mem::rva(e::global::PamClientSharedContentManager));
    using FindFn = uintptr_t (*)(uintptr_t, uintptr_t);
    if (!mgr || reinterpret_cast<FindFn>(mem::rva(e::function::PamClientSharedContentManager_findMarker))(mgr, challenge) != marker) return;
    mem::write(marker + 0x225, uint8_t{0});
    reinterpret_cast<void (*)(uintptr_t)>(mem::rva(e::function::PamNamedChallengeMapMarker_refresh))(marker);
}

void spawnOnServer(uint32_t h) {
    auto p = std::make_shared<Prep>();
    p->handle = h;
    {
        std::lock_guard<std::mutex> lock(g_m);
        Dash* d = dashOf(h);
        if (!d || d->state != Dash::Queued) return;
        p->spec = d->spec, p->level = d->level;
    }
    auto failWith = [&](const std::string& why) {
        std::lock_guard<std::mutex> lock(g_m);
        if (Dash* d = dashOf(h)) d->state = Dash::Failed, d->status = why;
        GN_LOG("dashes: dash %u not made: %s", h, why.c_str());
    };
    if (p->level != g_level.load()) return;
    hookStats();
    if (!g_statsHooked) return failWith("the stats hook is not in (see the log)");
    Templates tp;
    {
        std::lock_guard<std::mutex> lock(g_tplM);
        tp = *templates();
    }
    if (!tp.missing.empty()) return failWith(tp.missing);
    p->bus = parentBus();
    if (!p->bus) return failWith("no bus the client knows (Faith not in the world yet?)");
    p->built = build(tp, p->spec, h);
    if (!p->built.error.empty()) return failWith(p->built.error);

    {
        std::lock_guard<std::mutex> lock(g_m);
        p->id = freeId();
        if (p->id) g_usedIds.push_back(p->id);
    }
    if (!p->id) return failWith("no free bundle id in the data registry");
    uint8_t* partition = static_cast<uint8_t*>(_aligned_malloc(0x40, 16));
    uint8_t* inst = engineArray(static_cast<uint32_t>(p->built.objects.size()), 8);
    if (!partition || !inst) return failWith("out of memory");
    std::memset(partition, 0, 0x40);
    for (size_t i = 0; i < p->built.objects.size(); ++i) reinterpret_cast<uintptr_t*>(inst)[i] = p->built.objects[i];
    *reinterpret_cast<uint8_t**>(partition + 0x18) = inst; // all DataRegistry_register reads
    reinterpret_cast<void (*)(void*, uint16_t)>(mem::rva(e::function::DataRegistry_register))(partition, p->id);
    client().post([p] { markOnClient(p); });
}

// Server thread, after markOnClient: the ServerBlueprintEntity under the bus, streamed in.
void placeOnServer(std::shared_ptr<Prep> p) {
    const uint32_t h = p->handle;
    const uint64_t level = p->level;
    const uint16_t id = p->id;
    uintptr_t ent = 0;
    if (level == g_level.load() && isBus(p->bus)) {
        alignas(16) uint8_t ci[0x200] = {};
        const gn_transform origin = clean(gn_transform{{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}, {0.f, 0.f, 0.f, 1.f}});
        reinterpret_cast<void (*)(uint8_t*, uintptr_t, uintptr_t, const gn_transform*)>(mem::rva(e::function::ServerEntityCreationInfo_ctor))(ci, p->built.outer, p->bus, &origin);
        ent = reinterpret_cast<uintptr_t (*)(uint8_t*, uintptr_t)>(mem::rva(e::function::unverified::entityCreator_spawnIntoBus))(ci, 0);
        if (ent) {
            alignas(16) uint8_t init[0x40] = {};
            reinterpret_cast<void (*)(uint8_t*, uint64_t, uint64_t)>(mem::rva(e::function::ServerEntityInitInfo_ctor))(init, 0, 0);
            reinterpret_cast<void (*)(uintptr_t, uint8_t*)>(mem::rva(e::function::Entity_init))(ent, init);
            inspector::noteCreated(ent); // its creator is not hooked, and its address may be marked dead (entity-liveness c3, c7)
            char ev[16];
            std::snprintf(ev, sizeof(ev), "0x%08x", kStreamIn);
            eventSend("GridNode", reinterpret_cast<void*>(ent), ev); // its contents at its next tick; the client's from the network
        }
    }
    std::lock_guard<std::mutex> lock(g_m);
    Dash* d = dashOf(h);
    if (!d || level != g_level.load()) { // destroyed while made (or the level went)
        if (ent) {
            g_liveEnts.push_back(ent);
            const uintptr_t bus = p->bus;
            server().post([ent, bus, level] { destroyOnServer(ent, bus, 0, level); });
        }
        if (p->marker) {
            const uintptr_t m = p->marker, c = p->built.challenge;
            client().post([m, c, level] { hideMarker(m, c, level); });
        }
        if (level == g_level.load()) g_unreg.push_back({id, level, std::chrono::steady_clock::now() + kUnregisterDelay});
        return;
    }
    d->regId = id;
    d->challenge = p->built.challenge;
    d->marker = p->marker;
    if (!ent) {
        d->state = Dash::Failed, d->status = "the game made no entity (spawning is off?)";
        g_unreg.push_back({id, level, std::chrono::steady_clock::now() + kUnregisterDelay});
        d->regId = 0;
        if (p->marker) {
            const uintptr_t m = p->marker, c = p->built.challenge;
            client().post([m, c, level] { hideMarker(m, c, level); });
            d->marker = 0;
        }
        return;
    }
    d->entity = ent, d->bus = p->bus;
    d->state = Dash::Live, d->status = p->marker ? "placed" : "placed (no map icon: see the log)";
    g_liveEnts.push_back(ent);
    GN_LOG("dashes: dash %u made: %u objects (bundle id %u), %u guide arrows, start box %.1f x %.1f x %.1f m", h, static_cast<unsigned>(p->built.objects.size()), id,
           static_cast<unsigned>(p->spec.arrows.size()), p->spec.startHalf[0] * 2.f, p->spec.startHalf[1] * 2.f, p->spec.startHalf[2] * 2.f);
}

// Server thread: streamed out and destroyed; its data leaves the registry a little later.
void destroyOnServer(uintptr_t ent, uintptr_t bus, uint16_t id, uint64_t level) {
    if (level != g_level.load()) return;
    bool live = false;
    {
        std::lock_guard<std::mutex> lock(g_m);
        live = ent && std::find(g_liveEnts.begin(), g_liveEnts.end(), ent) != g_liveEnts.end();
    }
    if (live) {
        if (!(mem::readOr<uint32_t>(ent + 0x18) & 0x400)) { // not already going
            char ev[16];
            std::snprintf(ev, sizeof(ev), "0x%08x", kStreamOut);
            eventSend("GridNode", reinterpret_cast<void*>(ent), ev);
            if (isBus(bus)) reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(mem::rva(e::function::Entity_requestDestroy))(ent, bus);
        }
    }
    if (id) {
        std::lock_guard<std::mutex> lock(g_m);
        g_unreg.push_back({id, level, std::chrono::steady_clock::now() + kUnregisterDelay});
    }
}

void pollOnServer() {
    std::vector<uint16_t> due;
    {
        std::lock_guard<std::mutex> lock(g_m);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = g_unreg.begin(); it != g_unreg.end();)
            if (now >= it->at) due.push_back(it->id), it = g_unreg.erase(it);
            else ++it;
    }
    for (uint16_t id : due) unregister(id);
}

void levelChanged(const char*, int loaded, void*) {
    if (loaded) return;
    g_level.fetch_add(1);
    std::vector<uint16_t> ids;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_dashes.empty()) GN_LOG("dashes: the level ended: %u custom dashes went with it", static_cast<unsigned>(g_dashes.size()));
        g_dashes.clear();
        g_unreg.clear();
        g_liveEnts.clear();
        ids = g_usedIds;
    }
    // The level's entities are gone with it: every id of ours leaves the registry now (one left at game exit
    // crashed the registry's destructor, 2026-11-13).
    for (uint16_t id : ids) unregister(id);
}

// The game takes a dash away on a checkpoint or level reload: its ServerBlueprintEntity's destructor (GridNode
// wraps every entity class's) says so. inspector::alive could not: it took our entity for dead at once, and the
// dash was made again every 3 s (2026-11-13).
void entityGone(void* entity, void*) {
    std::lock_guard<std::mutex> lock(g_m);
    g_liveEnts.erase(std::remove(g_liveEnts.begin(), g_liveEnts.end(), reinterpret_cast<uintptr_t>(entity)), g_liveEnts.end());
    for (Dash& d : g_dashes)
        if (d.state == Dash::Live && d.entity == reinterpret_cast<uintptr_t>(entity)) {
            d.state = Dash::Failed, d.status = "taken away by the game (a checkpoint or reload)";
            d.entity = 0;
            if (d.marker) {
                const uintptr_t m = d.marker, c = d.challenge;
                const uint64_t level = d.level;
                client().post([m, c, level] { hideMarker(m, c, level); });
                d.marker = 0;
            }
            if (d.regId) g_unreg.push_back({d.regId, d.level, std::chrono::steady_clock::now() + kUnregisterDelay}), d.regId = 0;
        }
}

void start() {
    std::call_once(g_once, [] {
        onDestroyed("GridNode", "ServerBlueprintEntity", &entityGone, nullptr);
        onLevel("GridNode", &levelChanged, nullptr);
        server().addListener("GridNode dashes", [](float) { pollOnServer(); });
    });
}

bool finite(const gn_transform& t) {
    for (int k = 0; k < 4; ++k)
        if (!std::isfinite(t.right[k]) || !std::isfinite(t.up[k]) || !std::isfinite(t.forward[k]) || !std::isfinite(t.trans[k])) return false;
    return std::fabs(t.trans[0]) < 1e6f && std::fabs(t.trans[1]) < 1e6f && std::fabs(t.trans[2]) < 1e6f;
}

} // namespace

int dashArrowAxis() { return g_arrowAxis.load(); }
const std::vector<std::string>& dashArrowKinds() { return kArrowNames; }
bool dashTemplateStars(int32_t out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = g_tplStars[k].load();
    return out[0] || out[1] || out[2];
}

int dashSpawn(const DashSpec& spec, uint32_t* handle) {
    if (!handle || !finite(spec.marker) || !finite(spec.start) || !finite(spec.end) || spec.arrows.size() > 64 || spec.moreStarts.size() > 8 ||
        (spec.haveStartLine && !finite(spec.startLine)))
        return GN_ERR_ARGUMENT;
    for (const DashArrowSpec& a : spec.arrows)
        if (!finite(a.at) || !std::isfinite(a.scale)) return GN_ERR_ARGUMENT;
    for (const DashStartSpec& m : spec.moreStarts)
        if (!finite(m.at)) return GN_ERR_ARGUMENT;
    start();
    std::lock_guard<std::mutex> lock(g_m);
    if (g_dashes.size() >= 32) return GN_ERR_FULL;
    Dash d;
    d.handle = g_next++;
    d.level = g_level.load();
    d.spec = spec;
    *handle = d.handle;
    const uint32_t h = d.handle;
    g_dashes.push_back(std::move(d));
    server().post([h] { spawnOnServer(h); });
    return GN_OK;
}

int dashState(uint32_t handle, uint32_t* live, std::string* status) {
    std::lock_guard<std::mutex> lock(g_m);
    Dash* d = dashOf(handle);
    if (!d) return GN_ERR_NOT_FOUND;

    if (status) *status = d->status;
    if (live) *live = d->state == Dash::Live ? 1u : 0u;
    return d->state == Dash::Failed ? GN_ERR_UNAVAILABLE : GN_OK;
}

void dashSetBest(uint32_t handle, int64_t centiseconds) {
    std::lock_guard<std::mutex> lock(g_m);
    if (Dash* d = dashOf(handle)) d->best = centiseconds;
}

bool dashFinished(uint32_t handle, int64_t* centiseconds) {
    std::lock_guard<std::mutex> lock(g_m);
    Dash* d = dashOf(handle);
    if (!d || !d->finished) return false;
    d->finished = false;
    if (centiseconds) *centiseconds = d->lastTime;
    return true;
}

// A run is on (the player chose to play, is running, or is at the end screen): the challenge entity's state
// (+0x78) is not 0 (dash-runtime c1: 0 idle, 1 activated, 2 running, 3 finishing, 4 finished). Made again then,
// the end screen would belong to a dash that is gone.
bool dashBusy(uint32_t handle) {
    uintptr_t ent = 0, ch = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        Dash* d = dashOf(handle);
        if (!d || d->state != Dash::Live) return false;
        ent = d->challengeEnt, ch = d->challenge;
        const auto now = std::chrono::steady_clock::now();
        if (!ent && now - d->lookedAt < std::chrono::seconds(1)) return false;
        if (!ent) d->lookedAt = now;
    }
    if (!ent) { // the entity our blueprint made for the challenge, among those GridNode saw created
        static const reflect::Type* chType = registry().find("PamServerNamedChallengeEntity");
        for (const auto& [p, t] : inspector::tracked())
            if (t == chType && mem::readOr<uintptr_t>(p + 0x80) == ch) ent = p;
        if (!ent) return false;
        std::lock_guard<std::mutex> lock(g_m);
        if (Dash* d = dashOf(handle)) d->challengeEnt = ent;
    }
    const reflect::Type* t = typeOf(ent);
    if (!t || !inspector::alive(ent, t) || mem::readOr<uintptr_t>(ent + 0x80) != ch) {
        std::lock_guard<std::mutex> lock(g_m);
        if (Dash* d = dashOf(handle)) d->challengeEnt = 0;
        return false;
    }
    return mem::readOr<int32_t>(ent + 0x78) != 0;
}

int dashDestroy(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto it = g_dashes.begin(); it != g_dashes.end(); ++it)
        if (it->handle == handle) {
            const uintptr_t ent = it->entity, bus = it->bus, marker = it->marker, ch = it->challenge;
            const uint16_t id = it->regId;
            const uint64_t level = it->level;
            g_dashes.erase(it);
            server().post([ent, bus, id, level] { destroyOnServer(ent, bus, id, level); });
            if (marker) client().post([marker, ch, level] { hideMarker(marker, ch, level); });
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

void dashesRelease() {
    std::vector<uint32_t> all;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const Dash& d : g_dashes) all.push_back(d.handle);
    }
    for (uint32_t h : all) dashDestroy(h);
    if (!all.empty()) GN_LOG("dashes: unload: %u custom dashes taken away", static_cast<unsigned>(all.size()));
}

void dashesReleaseIds() {
    std::vector<uint16_t> ids;
    {
        std::lock_guard<std::mutex> lock(g_m);
        ids = g_usedIds;
        g_unreg.clear();
    }
    for (uint16_t id : ids) unregister(id);
    if (!ids.empty()) GN_LOG("dashes: unload: %u bundle ids taken out of the data registry", static_cast<unsigned>(ids.size()));
}

} // namespace gn::host
