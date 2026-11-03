// World API: the local characters, transforms, the game's teleport, components, and level events.
// Offsets and functions are from db/annotations (gridnode/engine.hpp); everything read here is
// read fault tolerantly, and game functions are only called on the thread of their realm.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;

std::atomic<void*> g_clientCharacter{nullptr}; // as of the last client tick

const reflect::Type* typeNamed(const char* name) {
    static std::mutex m;
    static std::vector<std::pair<std::string, const reflect::Type*>> cache;
    std::lock_guard<std::mutex> lock(m);
    for (const auto& c : cache)
        if (c.first == name) return c.second;
    const reflect::Type* t = registry().find(name);
    cache.emplace_back(name, t);
    return t;
}

bool kindOf(const void* object, const char* className) {
    const reflect::Type* base = typeNamed(className);
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uint64_t>(object));
    return t && base && registry().isKindOf(t, base);
}

bool isGameCode(uintptr_t fn) {
    if (!mem::inImage(fn)) return false;
    for (const mem::Section& s : mem::imageSections())
        if (s.region.code && fn >= s.region.begin && fn < s.region.end) return true;
    return false;
}

bool finite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1.0e6f) return false;
    return true;
}

// ServerCharacterEntity_teleport reads its argument with aligned loads and starts with
// "mov rdi, rcx; lea rbx, [rcx+410h]" 24 bytes in (checked once, as MECMP does).
bool teleportFunctionOk() {
    static const int ok = [] {
        static const uint8_t kExpect[] = {0x48, 0x89, 0xCF, 0x48, 0x8D, 0x99, 0x10, 0x04, 0x00, 0x00};
        uint8_t got[sizeof(kExpect)] = {};
        const bool same = mem::read(mem::rva(e::function::ServerCharacterEntity_teleport) + 24, got, sizeof(got)) &&
                          memcmp(got, kExpect, sizeof(got)) == 0;
        if (!same) GN_LOG("world: ServerCharacterEntity_teleport does not look as expected; teleport is off");
        return same ? 1 : 0;
    }();
    return ok == 1;
}

// ---- levels ----------------------------------------------------------------------------------
struct LevelListener {
    std::string mod;
    gn_level_fn fn;
    void* user;
};
std::mutex g_levelM;
std::vector<LevelListener> g_levelListeners; // guarded by g_levelM
std::string g_level;                         // the level the listeners were last told about (g_levelM)

// Server characters seen being created (a fallback for localServerCharacter when the player
// manager cannot be read). Few: Faith, AI, ghosts.
std::mutex g_charM;
std::vector<uintptr_t> g_serverChars; // guarded by g_charM

void onServerCharacter(void* entity, void*, void*) {
    static std::atomic<int> logged{0}; // the first few, to check the capture works
    if (logged.fetch_add(1) < 8) {
        const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(entity));
        GN_LOG("world: server character created %p (%s), player %llx", entity, t ? t->name.c_str() : "?",
               static_cast<unsigned long long>(
                   mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(entity) + e::cls::ServerCharacterEntity::off::player)));
    }
    std::lock_guard<std::mutex> lock(g_charM);
    if (g_serverChars.size() >= 4096) g_serverChars.erase(g_serverChars.begin(), g_serverChars.begin() + 1024);
    g_serverChars.push_back(reinterpret_cast<uintptr_t>(entity));
}

// Live server characters seen being created (dead ones are dropped).
std::vector<uintptr_t> createdCharacters() {
    std::lock_guard<std::mutex> lock(g_charM);
    for (size_t i = g_serverChars.size(); i-- > 0;)
        if (!kindOf(reinterpret_cast<void*>(g_serverChars[i]), "ServerCharacterEntity"))
            g_serverChars.erase(g_serverChars.begin() + static_cast<std::ptrdiff_t>(i));
    return g_serverChars;
}

// The newest created server character owned by a player (AI and ghosts have none).
void* fromCreatedCharacters() {
    const std::vector<uintptr_t> chars = createdCharacters();
    for (size_t i = chars.size(); i-- > 0;)
        if (mem::readOr<uintptr_t>(chars[i] + e::cls::ServerCharacterEntity::off::player)) return reinterpret_cast<void*>(chars[i]);
    return nullptr;
}

void callLevel(const std::vector<LevelListener>& ls, const std::string& level, bool loaded) {
    for (const LevelListener& l : ls) {
        try {
            l.fn(level.c_str(), loaded ? 1 : 0, l.user);
        } catch (...) {
            GN_LOG("level: a mod's level listener threw; ignored");
        }
    }
}

} // namespace

// Client tick (ticks.cpp): remember Faith for the other threads.
void worldClientTick() {
    void* c = reinterpret_cast<void* (*)()>(mem::rva(e::function::LocalClientCharacter))();
    void* before = g_clientCharacter.exchange(c, std::memory_order_relaxed);
    if (c == before) return;
    // Faith changed (a level started, a respawn): say so, and whether her transform reads.
    if (!c) {
        GN_LOG("world: client character gone");
        return;
    }
    const uintptr_t ts = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(c) + e::cls::ComponentEntity::off::transformSpace);
    uint8_t sel[2] = {};
    if (ts) mem::read(ts + 9, sel, 2);
    gn_transform t{};
    const bool ok = entityTransform(c, t);
    const reflect::Type* type = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(c));
    GN_LOG("world: client character %p (%s), transform space %llx (selectors %u %u): %s %.2f %.2f %.2f", c,
           type ? type->name.c_str() : "?", static_cast<unsigned long long>(ts), sel[0], sel[1],
           ok ? "position" : "UNREADABLE, last read", t.trans[0], t.trans[1], t.trans[2]);
}

void* localClientCharacter() {
    if (isClientThread()) return reinterpret_cast<void* (*)()>(mem::rva(e::function::LocalClientCharacter))();
    return g_clientCharacter.load(std::memory_order_relaxed);
}

void worldStart() { onCreated("gridnode", "ServerCharacterEntity", &onServerCharacter, nullptr); }

namespace {
// ServerGameContext -> server player manager (+0x68) -> players (+0x268/+0x270). The first human
// player whose character (+0x1478) is a ServerCharacterEntity that points back at the player.
void* fromPlayerManager() {
    using namespace e::st;
    const uintptr_t mm = mem::readOr<uintptr_t>(mem::rva(e::global::ServerGameContext));
    const uintptr_t pm = mm ? mem::readOr<uintptr_t>(mm + ServerGameContext::off::serverPlayerManager) : 0;
    if (!pm) return nullptr;
    uintptr_t range[2] = {};
    if (!mem::read(pm + ServerPlayerManager::off::playersBegin, range, sizeof(range))) return nullptr;
    if (!range[0] || range[1] < range[0] || (range[1] - range[0]) / 8 > 64) return nullptr;
    uintptr_t players[64];
    const size_t n = (range[1] - range[0]) / 8;
    if (!n || !mem::read(range[0], players, n * 8)) return nullptr;
    for (size_t i = 0; i < n; ++i) {
        const uintptr_t p = players[i];
        if (!p) continue;
        const uintptr_t c = mem::readOr<uintptr_t>(p + ServerPlayer::off::character);
        if (!c || mem::readOr<uintptr_t>(c + e::cls::ServerCharacterEntity::off::player) != p) continue;
        if (kindOf(reinterpret_cast<void*>(c), "ServerCharacterEntity")) return reinterpret_cast<void*>(c);
    }
    return nullptr;
}

// The created server character standing where Faith is on the client (single player: the
// server and client characters share a position; AI and ghosts are elsewhere).
void* fromClientPosition() {
    void* cc = g_clientCharacter.load(std::memory_order_relaxed);
    gn_transform ct{};
    if (!cc || !entityTransform(cc, ct)) return nullptr;
    void* best = nullptr;
    float bestD = 2.0f * 2.0f; // within 2 m
    for (uintptr_t c : createdCharacters()) {
        gn_transform st{};
        if (!entityTransform(reinterpret_cast<void*>(c), st)) continue;
        const float dx = st.trans[0] - ct.trans[0], dy = st.trans[1] - ct.trans[1], dz = st.trans[2] - ct.trans[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) {
            bestD = d;
            best = reinterpret_cast<void*>(c);
        }
    }
    return best;
}

// Once per failure streak: what each way of finding the character saw (for the log).
void diagnoseServerCharacter() {
    using namespace e::st;
    const uintptr_t mm = mem::readOr<uintptr_t>(mem::rva(e::global::ServerGameContext));
    const uintptr_t pm = mm ? mem::readOr<uintptr_t>(mm + ServerGameContext::off::serverPlayerManager) : 0;
    uintptr_t range[2] = {};
    if (pm) mem::read(pm + ServerPlayerManager::off::playersBegin, range, sizeof(range));
    GN_LOG("world: no local server character. server game context %llx, player manager %llx, players %llx..%llx (%lld)",
           static_cast<unsigned long long>(mm), static_cast<unsigned long long>(pm), static_cast<unsigned long long>(range[0]),
           static_cast<unsigned long long>(range[1]), static_cast<long long>(range[1] >= range[0] ? (range[1] - range[0]) / 8 : -1));
    if (range[0] && range[1] > range[0] && (range[1] - range[0]) / 8 <= 16) {
        for (uintptr_t a = range[0]; a < range[1]; a += 8) {
            const uintptr_t p = mem::readOr<uintptr_t>(a);
            const uintptr_t c = p ? mem::readOr<uintptr_t>(p + ServerPlayer::off::character) : 0;
            const reflect::Type* pt = p ? registry().typeOf(mem::ProcessMemory(), p) : nullptr;
            const reflect::Type* ct = c ? registry().typeOf(mem::ProcessMemory(), c) : nullptr;
            GN_LOG("world:   player %llx (%s) isHuman %u character %llx (%s) back %llx", static_cast<unsigned long long>(p),
                   pt ? pt->name.c_str() : "?", p ? mem::readOr<uint8_t>(p + ServerPlayer::off::isHuman) : 0,
                   static_cast<unsigned long long>(c), ct ? ct->name.c_str() : "?",
                   static_cast<unsigned long long>(c ? mem::readOr<uintptr_t>(c + e::cls::ServerCharacterEntity::off::player) : 0));
        }
    }
    const std::vector<uintptr_t> chars = createdCharacters();
    GN_LOG("world:   %llu server characters seen being created", static_cast<unsigned long long>(chars.size()));
    size_t shown = 0;
    for (size_t i = chars.size(); i-- > 0 && shown < 12; ++shown) {
        const uintptr_t c = chars[i];
        const reflect::Type* ct = registry().typeOf(mem::ProcessMemory(), c);
        gn_transform t{};
        const bool pos = entityTransform(reinterpret_cast<void*>(c), t);
        GN_LOG("world:   %llx %s player %llx pos %s%.1f %.1f %.1f", static_cast<unsigned long long>(c), ct ? ct->name.c_str() : "?",
               static_cast<unsigned long long>(mem::readOr<uintptr_t>(c + e::cls::ServerCharacterEntity::off::player)),
               pos ? "" : "(unreadable) ", t.trans[0], t.trans[1], t.trans[2]);
    }
    gn_transform ct{};
    void* cc = g_clientCharacter.load(std::memory_order_relaxed);
    const bool cpos = cc && entityTransform(cc, ct);
    GN_LOG("world:   client character %p pos %s%.1f %.1f %.1f", cc, cpos ? "" : "(unreadable) ", ct.trans[0], ct.trans[1], ct.trans[2]);
}

} // namespace

void* localServerCharacter() {
    static std::atomic<int> reported{0}; // which way found it last (logged when it changes)
    static std::atomic<DWORD> diagnosedAt{0};
    struct Way {
        void* (*fn)();
        const char* what;
    };
    static const Way kWays[] = {{&fromPlayerManager, "the player manager"},
                                {&fromCreatedCharacters, "created characters with a player"},
                                {&fromClientPosition, "the created character at the client character's position"}};
    for (int i = 0; i < 3; ++i) {
        if (void* c = kWays[i].fn()) {
            if (reported.exchange(i + 1) != i + 1) GN_LOG("world: local server character found through %s", kWays[i].what);
            return c;
        }
    }
    reported = 0;
    // Diagnose at most every 30 s, and only while the client has a character (in a level).
    if (g_clientCharacter.load(std::memory_order_relaxed) && GetTickCount() - diagnosedAt.load() > 30000) {
        diagnosedAt = GetTickCount();
        diagnoseServerCharacter();
    }
    return nullptr;
}



bool entityTransform(const void* entity, gn_transform& out) {
    if (!entity) return false;
    const uintptr_t ts = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(entity) + e::cls::ComponentEntity::off::transformSpace);
    uint8_t sel[2] = {};
    // Two indices into the space's transform slots (seen in game: 19 and 10 for Faith).
    if (!ts || !mem::read(ts + 9, sel, 2)) return false;
    float m[16];
    // The rows' fourth floats are padding (not always initialised): only x, y, z are checked.
    if (!mem::read(ts + 0x10 + (sel[0] + 2u * sel[1]) * 0x20, m, sizeof(m)) || !finite(m, 3) || !finite(m + 4, 3) ||
        !finite(m + 8, 3) || !finite(m + 12, 3))
        return false;
    memcpy(out.right, m, 16);
    memcpy(out.up, m + 4, 16);
    memcpy(out.forward, m + 8, 16);
    memcpy(out.trans, m + 12, 16);
    return true;
}

int teleport(void* character, const gn_transform* to) {
    if (!character || !to || !finite(to->right, 3) || !finite(to->up, 3) || !finite(to->forward, 3) || !finite(to->trans, 3))
        return GN_ERR_ARGUMENT;
    if (!isServerThread()) return GN_ERR_THREAD;
    if (!teleportFunctionOk()) return GN_ERR_UNAVAILABLE;
    if (!kindOf(character, "ServerCharacterEntity")) return GN_ERR_ARGUMENT;
    alignas(16) gn_transform t = *to;
    t.right[3] = t.up[3] = t.forward[3] = 0.f;
    t.trans[3] = 1.f;
    reinterpret_cast<void (*)(void*, const void*)>(mem::rva(e::function::ServerCharacterEntity_teleport))(character, &t);
    return GN_OK;
}

void* getComponent(void* character, const gn_type* type) {
    if (!character || !type) return nullptr;
    if (!kindOf(character, "ClientCharacterEntity") && !kindOf(character, "ServerCharacterEntity")) return nullptr;
    uintptr_t vt = 0, fn = 0;
    if (!mem::read(reinterpret_cast<uintptr_t>(character), vt) || !mem::read(vt + e::cls::PamClientCharacterEntity::vf::getComponent, fn) ||
        !isGameCode(fn))
        return nullptr;
    const auto* t = reinterpret_cast<const reflect::Type*>(type);
    return reinterpret_cast<void* (*)(void*, const void*)>(fn)(character, reinterpret_cast<const void*>(t->ti));
}

// ---- ray casts (the call the game's ray cast entities make; MECMP's checked use) -----------------
namespace {
struct alignas(16) RayQuery {
    float from[4] = {0, 0, 0, 0};
    float to[4] = {0, 0, 0, 0};
    float radius = 0.f;
    uint8_t flags[3] = {};
    uint8_t pad[9] = {};
};
static_assert(sizeof(RayQuery) == e::st::RaycastQuery::size, "RaycastQuery layout");
struct alignas(16) RayResult {
    float position[4] = {0, 0, 0, 0};
    float normal[4] = {0, 0, 0, 0};
    uint32_t id = 0;
    float fraction = 0.f;
    float distance = 0.f;
    uint8_t hit = 0;
    uint8_t pad[0x13] = {};
};
static_assert(sizeof(RayResult) == e::st::RaycastResult::size, "RaycastResult layout");

// The server world's physics, as the game's ray cast update finds it; every object on the way is
// checked, so a world that is going away gives "unavailable" instead of a fault.
uintptr_t serverPhysics(uintptr_t world) {
    namespace c = e::constant;
    const uintptr_t manager = mem::readOr<uintptr_t>(world + c::World_PhysicsManager);
    const uintptr_t holder = manager ? mem::readOr<uintptr_t>(manager + c::PhysicsManager_Holder) : 0;
    const uintptr_t inner = holder ? mem::readOr<uintptr_t>(holder) : 0;
    const uintptr_t phys = inner ? mem::readOr<uintptr_t>(inner + c::PhysicsHolder_Physics) : 0;
    if (!phys || !mem::inImage(mem::readOr<uintptr_t>(phys))) return 0;
    const uintptr_t havok = mem::readOr<uintptr_t>(phys + c::Physics_HavokWorld);
    const uintptr_t filterOwner = havok ? mem::readOr<uintptr_t>(havok + c::HavokWorld_FilterOwner) : 0;
    if (!filterOwner || !mem::readOr<uintptr_t>(filterOwner + c::FilterOwner_Filter)) return 0;
    return phys;
}
} // namespace

int raycast(const float from[3], const float to[3], float radius, gn_ray_hit* out) {
    if (!from || !to || !finite(from, 3) || !finite(to, 3) || !std::isfinite(radius) || radius < 0.f || radius > 100.f)
        return GN_ERR_ARGUMENT;
    if (!isServerThread()) return GN_ERR_THREAD;
    static const bool ok = [] {
        const bool b = build::codeMatches("RaycastQuery_execute");
        if (!b) GN_LOG("world: RaycastQuery_execute does not look as expected; ray casts are off");
        return b;
    }();
    if (!ok) return GN_ERR_UNAVAILABLE;
    const uintptr_t world = mem::readOr<uintptr_t>(mem::rva(e::global::ServerGameWorld));
    const uintptr_t phys = world ? serverPhysics(world) : 0;
    if (!phys) return GN_ERR_UNAVAILABLE;
    RayQuery q;
    RayResult r;
    memcpy(q.from, from, 12);
    memcpy(q.to, to, 12);
    q.radius = radius;
    using Fn = void (*)(const RayQuery*, void* world, void* physics, RayResult*);
    reinterpret_cast<Fn>(mem::rva(e::function::RaycastQuery_execute))(&q, reinterpret_cast<void*>(world), reinterpret_cast<void*>(phys), &r);
    if (r.hit != 1 || !finite(r.position, 3)) return GN_ERR_NOT_FOUND;
    if (out) {
        memcpy(out->position, r.position, 12);
        memcpy(out->normal, r.normal, 12);
        out->distance = r.distance;
        out->fraction = r.fraction;
    }
    return GN_OK;
}

// ---- ray casts that say what they hit (picking) ------------------------------------------------------
namespace {
struct alignas(16) PhysRayInput {
    uint32_t wants = 2;    // the part (shape key: which instance of a compound); the body is always filled in
    uint32_t pad0 = 0;
    uint32_t filter = 9;   // a plain ray, as RaycastQuery_execute casts it
    uint32_t pad1 = 0;
    uint32_t* ignoreBegin = nullptr;
    uint32_t* ignoreEnd = nullptr;
    uint32_t* ignoreCapacityEnd = nullptr;
    uint64_t pad2 = 0;
    uint32_t ignoreInline[12] = {};
    float from[4] = {0, 0, 0, 0};
    float to[4] = {0, 0, 0, 0};
    uint8_t sweep = 0;
    uint8_t pad3[0x4f] = {};
};
static_assert(sizeof(PhysRayInput) == e::st::PhysicsRayInput::size, "PhysicsRayInput layout");
static_assert(offsetof(PhysRayInput, ignoreInline) == e::st::PhysicsRayInput::off::ignoreInline, "PhysicsRayInput layout");
static_assert(offsetof(PhysRayInput, from) == e::st::PhysicsRayInput::off::from, "PhysicsRayInput layout");
static_assert(offsetof(PhysRayInput, sweep) == e::st::PhysicsRayInput::off::sweep, "PhysicsRayInput layout");
struct alignas(16) PhysRayHit {
    uint32_t body = 0xffffffffu;
    uint32_t pad0 = 0;
    uint32_t material = 0xffffffffu;
    uint32_t part = 0xffffffffu;
    uint32_t bone = 0xffffffffu;
    uint32_t pad1 = 0;
    float fraction = 1.f;
    uint32_t pad2 = 0;
    float normal[4] = {0, 0, 0, 0};
    float position[4] = {0, 0, 0, 0};
    uint8_t valid = 0;
    uint8_t pad3[0xf] = {};
};
static_assert(sizeof(PhysRayHit) == e::st::PhysicsRayHit::size, "PhysicsRayHit layout");
static_assert(offsetof(PhysRayHit, position) == e::st::PhysicsRayHit::off::position, "PhysicsRayHit layout");
static_assert(offsetof(PhysRayHit, fraction) == e::st::PhysicsRayHit::off::fraction, "PhysicsRayHit layout");
} // namespace

int raycastObject(const float from[3], const float to[3], gn_ray_hit* out, uintptr_t* object, uint32_t* body, uint32_t* part) {
    if (!from || !to || !finite(from, 3) || !finite(to, 3)) return GN_ERR_ARGUMENT;
    if (!isServerThread()) return GN_ERR_THREAD;
    static const bool ok = [] {
        const bool b = build::codeMatches("Physics_castRay");
        if (!b) GN_LOG("world: Physics_castRay does not look as expected; picking objects by ray is off");
        return b;
    }();
    if (!ok) return GN_ERR_UNAVAILABLE;
    const uintptr_t world = mem::readOr<uintptr_t>(mem::rva(e::global::ServerGameWorld));
    const uintptr_t phys = world ? serverPhysics(world) : 0;
    if (!phys) return GN_ERR_UNAVAILABLE;
    PhysRayInput in;
    in.ignoreBegin = in.ignoreEnd = in.ignoreInline;
    in.ignoreCapacityEnd = in.ignoreInline + 12;
    memcpy(in.from, from, 12);
    memcpy(in.to, to, 12);
    PhysRayHit h;
    using Fn = bool (*)(void*, const PhysRayInput*, PhysRayHit*);
    if (!reinterpret_cast<Fn>(mem::rva(e::function::Physics_castRay))(reinterpret_cast<void*>(phys), &in, &h) || !finite(h.position, 3))
        return GN_ERR_NOT_FOUND;
    if (out) {
        memcpy(out->position, h.position, 12);
        memcpy(out->normal, h.normal, 12);
        out->fraction = h.fraction;
        const float d[3] = {h.position[0] - from[0], h.position[1] - from[1], h.position[2] - from[2]};
        out->distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    if (body) *body = h.body;
    if (part) *part = h.part;
    if (object) {
        *object = 0;
        namespace c = e::constant;
        const uintptr_t owner = mem::readOr<uintptr_t>(phys + c::Physics_Owner);
        const uintptr_t table = owner ? mem::readOr<uintptr_t>(owner + c::PhysicsOwner_Objects) : 0;
        if (table && h.body != 0xffffffffu) *object = mem::readOr<uintptr_t>(table + (h.body & 0xffffu) * 8);
    }
    return GN_OK;
}

// ---- collision bodies ----------------------------------------------------------------------------------
// A physics entity's component lists its bodies by id; an id leads to its RigidBody through the
// physics manager's owner table (bits 16..23) and that owner's object table (low 16 bits).
std::vector<std::pair<uint32_t, uintptr_t>> physicsBodies(uintptr_t entity) {
    namespace c = e::constant;
    std::vector<std::pair<uint32_t, uintptr_t>> out;
    static const reflect::Type* rigid = registry().find("RigidBody");
    const uintptr_t comp = mem::readOr<uintptr_t>(entity + c::PhysicsEntity_Physics);
    if (!comp || !rigid) return out;
    const uint16_t count = mem::readOr<uint16_t>(comp + c::PhysicsComponent_BodyCount);
    const uintptr_t ids = mem::readOr<uintptr_t>(comp + c::PhysicsComponent_BodyIds);
    const uintptr_t manager = mem::readOr<uintptr_t>(comp + c::PhysicsComponent_Manager);
    const uintptr_t owners = manager ? mem::readOr<uintptr_t>(manager + c::PhysicsManager_Owners) : 0;
    if (!ids || !owners || count > 64) return out;
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t id = mem::readOr<uint32_t>(ids + i * 4u, 0xffffffffu);
        if (id == 0xffffffffu) continue;
        const uintptr_t owner = mem::readOr<uintptr_t>(owners + ((id >> 16) & 0xff) * 8);
        const uintptr_t table = owner ? mem::readOr<uintptr_t>(owner + c::PhysicsOwner_Objects) : 0;
        const uintptr_t body = table ? mem::readOr<uintptr_t>(table + (id & 0xffff) * 8) : 0;
        const reflect::Type* bt = body ? registry().typeOf(mem::ProcessMemory(), body) : nullptr;
        out.push_back({id, bt && registry().isKindOf(bt, rigid) ? body : 0});
    }
    return out;
}

std::pair<uintptr_t, const reflect::Type*> entityOfBody(uint32_t body) {
    if (body == 0xffffffffu) return {0, nullptr};
    static const reflect::Type* physicsEntity = registry().find("ServerPhysicsEntity");
    if (!physicsEntity) return {0, nullptr};
    for (const auto& [p, t] : inspector::trackedOf(physicsEntity)) {
        if (!inspector::alive(p, t)) continue;
        for (const auto& [id, rb] : physicsBodies(p))
            if (id == body) return {p, t};
    }
    return {0, nullptr};
}

namespace {
// A live physics entity (either realm), or null.
const reflect::Type* physicsEntityType(void* entity) {
    static const reflect::Type* server = registry().find("ServerPhysicsEntity");
    static const reflect::Type* client = registry().find("ClientPhysicsEntity");
    const uintptr_t p = reinterpret_cast<uintptr_t>(entity);
    const reflect::Type* t = p ? registry().typeOf(mem::ProcessMemory(), p) : nullptr;
    if (!t || !((server && registry().isKindOf(t, server)) || (client && registry().isKindOf(t, client)))) return nullptr;
    return inspector::alive(p, t) ? t : nullptr;
}

// The body's RigidBody, on the entity's realm thread; an error code when there is none to use.
int bodyOf(void* entity, uint32_t id, uintptr_t& out) {
    const reflect::Type* t = physicsEntityType(entity);
    if (!t) return GN_ERR_ARGUMENT;
    const bool client = t->name.rfind("Client", 0) == 0 || t->name.rfind("PamClient", 0) == 0;
    if (client ? !isClientThread() : !isServerThread()) return GN_ERR_THREAD;
    for (const auto& [i, rb] : physicsBodies(reinterpret_cast<uintptr_t>(entity)))
        if (i == id) {
            if (!rb) return GN_ERR_UNAVAILABLE;
            out = rb;
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

template <class Fn>
Fn bodyVirtual(uintptr_t body, uint32_t slot) {
    const uintptr_t vt = mem::readOr<uintptr_t>(body);
    const uintptr_t fn = vt ? mem::readOr<uintptr_t>(vt + slot) : 0;
    return mem::inImage(fn) ? reinterpret_cast<Fn>(fn) : nullptr;
}
} // namespace

int apiRaycastObject(const float from[3], const float to[3], gn_ray_hit_ex* out) {
    if (!out || out->size < sizeof(gn_ray_hit_ex)) return GN_ERR_ARGUMENT;
    gn_ray_hit hit{};
    uintptr_t object = 0;
    uint32_t body = 0xffffffffu, part = 0xffffffffu;
    const int r = raycastObject(from, to, &hit, &object, &body, &part);
    if (r != GN_OK) return r;
    out->hit = hit;
    out->body = body;
    out->part = part;
    out->reserved = 0;
    out->physics_object = reinterpret_cast<void*>(object);
    out->entity = reinterpret_cast<void*>(entityOfBody(body).first);
    return GN_OK;
}

int apiEntityBodies(void* entity, uint32_t* ids, int max) {
    if (max < 0 || (max > 0 && !ids) || !physicsEntityType(entity)) return -GN_ERR_ARGUMENT;
    const auto bodies = physicsBodies(reinterpret_cast<uintptr_t>(entity));
    for (int i = 0; i < max && i < static_cast<int>(bodies.size()); ++i) ids[i] = bodies[size_t(i)].first;
    return static_cast<int>(bodies.size());
}

int apiBodyTransform(void* entity, uint32_t id, gn_transform* out) {
    if (!out) return GN_ERR_ARGUMENT;
    uintptr_t body = 0;
    if (const int r = bodyOf(entity, id, body); r != GN_OK) return r;
    const auto get = bodyVirtual<void* (*)(uintptr_t, gn_transform*)>(body, e::constant::RigidBody_vf_getTransform);
    if (!get) return GN_ERR_UNAVAILABLE;
    alignas(16) gn_transform t{};
    get(body, &t);
    if (!finite(t.right, 3) || !finite(t.up, 3) || !finite(t.forward, 3) || !finite(t.trans, 3)) return GN_ERR_UNAVAILABLE;
    *out = t;
    return GN_OK;
}

int apiBodySetTransform(void* entity, uint32_t id, const gn_transform* to) {
    if (!to || !finite(to->right, 3) || !finite(to->up, 3) || !finite(to->forward, 3) || !finite(to->trans, 3)) return GN_ERR_ARGUMENT;
    alignas(16) gn_transform t = *to;
    for (float* row : {t.right, t.up, t.forward}) { // bodies carry no scale
        const float n = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        if (!(n > 1e-4f)) return GN_ERR_ARGUMENT;
        for (int k = 0; k < 3; ++k) row[k] /= n;
        row[3] = 0.f;
    }
    t.trans[3] = 1.f;
    uintptr_t body = 0;
    if (const int r = bodyOf(entity, id, body); r != GN_OK) return r;
    const auto set = bodyVirtual<void (*)(uintptr_t, const gn_transform*, bool)>(body, e::constant::RigidBody_vf_setTransform);
    if (!set) return GN_ERR_UNAVAILABLE;
    set(body, &t, true);
    return GN_OK;
}

void* apiEntityOtherHalf(void* entity) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(entity);
    const reflect::Type* t = p ? registry().typeOf(mem::ProcessMemory(), p) : nullptr;
    if (!t || !inspector::alive(p, t)) return nullptr;
    // ServerX <-> ClientX, PamServerX <-> PamClientX.
    std::string other = t->name;
    const size_t pre = other.rfind("Pam", 0) == 0 ? 3 : 0;
    if (other.compare(pre, 6, "Server") == 0) other.replace(pre, 6, "Client");
    else if (other.compare(pre, 6, "Client") == 0) other.replace(pre, 6, "Server");
    else return nullptr;
    const reflect::Type* ot = registry().find(other);
    gn_transform at{};
    if (!ot || !entityTransform(entity, at)) return nullptr;
    // By class and place, as the layout editor pairs them: the two halves do not keep their data at
    // the same offset (a static model's halves did not match by +0x28, 2026-11-03).
    for (const auto& [q, qt] : inspector::trackedOf(ot)) {
        if (qt != ot || !inspector::alive(q, qt)) continue;
        gn_transform o{};
        if (!entityTransform(reinterpret_cast<void*>(q), o)) continue;
        const float d = std::fabs(o.trans[0] - at.trans[0]) + std::fabs(o.trans[1] - at.trans[1]) + std::fabs(o.trans[2] - at.trans[2]);
        if (d < 0.05f) return reinterpret_cast<void*>(q);
    }
    return nullptr;
}

// The local player's camera (the view the game renders), or false before there is one.
bool cameraTransform(gn_transform& out) {
    namespace cv = e::st::ClientCameraViewManagers::off;
    const uintptr_t managers = mem::readOr<uintptr_t>(mem::rva(e::global::ClientCameraViewManagers));
    const uintptr_t begin = managers ? mem::readOr<uintptr_t>(managers + cv::managersBegin) : 0;
    const uintptr_t end = managers ? mem::readOr<uintptr_t>(managers + cv::managersEnd) : 0;
    const uintptr_t first = begin && end > begin ? mem::readOr<uintptr_t>(begin) : 0;
    float m[16];
    if (!first || !mem::read(first + e::st::ClientCameraViewManager::off::transform, m, sizeof(m)) || !finite(m, 3) || !finite(m + 4, 3) ||
        !finite(m + 8, 3) || !finite(m + 12, 3))
        return false;
    memcpy(out.right, m, 16);
    memcpy(out.up, m + 4, 16);
    memcpy(out.forward, m + 8, 16);
    memcpy(out.trans, m + 12, 16);
    return true;
}

std::string levelName() {
    const uintptr_t world = mem::readOr<uintptr_t>(mem::rva(e::global::ServerGameWorld));
    const uintptr_t level = world ? mem::readOr<uintptr_t>(world + e::st::ServerGameWorld::off::level) : 0;
    const uintptr_t data = level ? mem::readOr<uintptr_t>(level + e::st::ServerLevel::off::data) : 0;
    const uintptr_t name = data ? mem::readOr<uintptr_t>(data + 0x10) : 0;
    std::string s;
    if (name) mem::readCString(name, s, 256);
    return s;
}

int onLevel(const std::string& mod, gn_level_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    std::string current;
    {
        std::lock_guard<std::mutex> lock(g_levelM);
        g_levelListeners.push_back({mod, fn, user});
        current = g_level;
    }
    if (!current.empty()) fn(current.c_str(), 1, user);
    return GN_OK;
}

// GridNode's thread, about every 0.1 s.
void pollLevel() {
    const std::string now = levelName();
    std::string before;
    std::vector<LevelListener> ls;
    {
        std::lock_guard<std::mutex> lock(g_levelM);
        if (now == g_level) return;
        before = g_level;
        g_level = now;
        ls = g_levelListeners;
    }
    if (!before.empty()) {
        GN_LOG("level: ended %s", before.c_str());
        callLevel(ls, before, false);
    }
    if (!now.empty()) {
        GN_LOG("level: started %s", now.c_str());
        layouts::onLevel(now); // its layout, before its objects are created
        recorder::onLevel(now);
        settingsList();    // logs how many settings objects the engine holds
        consoleCommands(); // and how many commands (when that changed)
        callLevel(ls, now, true);
    }
}

} // namespace gn::host

// ---- entities in place (M7) ------------------------------------------------------------------------
namespace gn::host {

int entitiesNear(const float pos[3], float radius, gn_entity_fn fn, void* user) {
    if (!pos || !fn || !(radius > 0.f)) return 0;
    int n = 0;
    const float r2 = radius * radius;
    static const reflect::Type* spatial = registry().find("SpatialEntity");
    for (const auto& [p, t] : inspector::tracked()) {
        gn_transform at{};
        // Only objects with a place (most entities are logic): their type first, then memory reads.
        if (!t || !spatial || !registry().isKindOf(t, spatial)) continue;
        if (!inspector::alive(p, t) || !entityTransform(reinterpret_cast<void*>(p), at)) continue;
        const float dx = at.trans[0] - pos[0], dy = at.trans[1] - pos[1], dz = at.trans[2] - pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) continue;
        fn(reinterpret_cast<void*>(p), reinterpret_cast<const gn_type*>(t), &at, std::sqrt(d2), user);
        ++n;
    }
    return n;
}

int entitiesOfClass(const char* className, gn_entity_each_fn fn, void* user) {
    if (!className || !fn) return -GN_ERR_ARGUMENT;
    const reflect::Type* cls = registry().find(className);
    if (!cls) return -GN_ERR_NOT_FOUND;
    int n = 0;
    for (const auto& [p, t] : inspector::trackedOf(cls)) {
        if (!inspector::alive(p, t)) continue;
        fn(reinterpret_cast<void*>(p), reinterpret_cast<const gn_type*>(t), user);
        ++n;
    }
    return n;
}

int entitySetTransform(void* entity, const gn_transform* to) {
    if (!entity || !to || !finite(to->right, 3) || !finite(to->up, 3) || !finite(to->forward, 3) || !finite(to->trans, 3))
        return GN_ERR_ARGUMENT;
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(entity));
    const reflect::Type* spatial = registry().find("SpatialEntity");
    if (!t || !spatial || !registry().isKindOf(t, spatial)) return GN_ERR_ARGUMENT;
    if (!inspector::alive(reinterpret_cast<uintptr_t>(entity), t)) return GN_ERR_ARGUMENT; // found destroyed
    // The realm from the class name (Client... / Server... / Pam{Client,Server}...).
    const std::string& n = t->name;
    const bool client = n.rfind("Client", 0) == 0 || n.rfind("PamClient", 0) == 0;
    const bool server = n.rfind("Server", 0) == 0 || n.rfind("PamServer", 0) == 0;
    if ((client && !isClientThread()) || (server && !isServerThread()) || (!client && !server && !isClientThread() && !isServerThread()))
        return GN_ERR_THREAD;
    alignas(16) gn_transform m = *to;
    m.right[3] = m.up[3] = m.forward[3] = 0.f;
    m.trans[3] = 1.f;
    using Fn = void (*)(void*, const void*, uint8_t);
    reinterpret_cast<Fn>(mem::rva(e::function::SpatialEntity_setTransform))(entity, &m, 1);
    return GN_OK;
}

void forgetLevel(const CodeRange& r) {
    std::lock_guard<std::mutex> lock(g_levelM);
    g_levelListeners.erase(std::remove_if(g_levelListeners.begin(), g_levelListeners.end(),
                                          [&](const LevelListener& l) { return r.has(reinterpret_cast<void*>(l.fn)); }),
                           g_levelListeners.end());
}

} // namespace gn::host
