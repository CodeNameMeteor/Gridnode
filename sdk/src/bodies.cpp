// Collision boxes mods make (gn_api box_*), and what a ray hit's Havok body says (its layer).
//
// A box is a static Havok body in the client's physics world (the world Faith moves in) and the
// server's (AI, the server's ray casts), one in each. Each is made,
// moved and destroyed only right after the game's own physics flush (PhysicsWorld_flush), on the
// thread that runs it: the only place the game frees its body records. How, from the cinematic
// tools' research (Research_Ground_Level_2026-11-01 sections 4, 9, 10, 13), proven there in game:
//   - hits on a body are turned into the game's objects through the body's user data (+0x88), whose
//     low 16 bits index the world's records with no null check (0x1435177b0). So a box carries the
//     user data, filter, material and quality of a live static body (its "donor").
//   - a donor's record can die in a flush (its part of the level unloads); right after each flush
//     the boxes' donor is checked and, if gone, every box takes another or is destroyed.
//   - Havok writes past a full body buffer: nothing is made with fewer than 4,096 places free.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/reflect/registry.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;
namespace c = e::constant;

constexpr uint32_t kNoBody = 0x7fffffffu;
constexpr uint32_t kBodySize = 0x90;      // hknpBody
constexpr uint32_t kMinFree = 4096;       // places left in the body buffer before nothing more is made

struct Box {
    uint32_t handle = 0;
    std::string mod;
    uintptr_t owner = 0; // an address in the code that made it (its mod)
    alignas(16) gn_transform at{};
    float half[3] = {};
    uint32_t body[2] = {kNoBody, kNoBody}; // per realm (0 client, 1 server), in g_world[realm]'s hknpWorld
    bool moved[2] = {};      // a new place to put it at
    bool removed = false;    // to destroy
    bool failed[2] = {};     // could not be made (logged)
    // The standalone (native) kind, client world only: a game physics object made by the game's own factory
    // (box_backend 1) instead of a Havok body lending a record. Then body[0] stays kNoBody.
    uint32_t nid = kNoBody;
    uintptr_t parent = 0;        // the barrier it was made from: the game keeps its pointer, so the box goes when the barrier does
    gn_transform* ntf = nullptr; // where the game reads its transform from: kept (the game holds the pointer), never freed
};

std::mutex g_m;
std::vector<Box> g_boxes; // (g_m)
uint32_t g_next = 1;      // (g_m)
std::atomic<bool> g_native{false}; // box_backend: boxes made from now on in the client world are native
uintptr_t g_world[2] = {}; // (g_m) the client and server physics worlds (PhysicsWorld, 0xBB0) the bodies are in
struct Donor {
    uint64_t userData = 0;
    uint32_t filter = 0;
    uint16_t material = 0;
    uint8_t quality = 0;
    uint32_t body = kNoBody; // the donor's own Havok body
    uint32_t record = 0;
    bool ok = false;
} g_donor[2]; // (g_m) per realm

// Gravity a mod set, per realm: written into each of the realm's Havok worlds (the main one and the
// client's effect world: debris and ragdolls may be there) after the game's flush, again into each new
// world, and the game's value put back when the mod lets go.
struct Gravity {
    bool want = false;
    float g[3] = {};
    uintptr_t owner = 0; // an address in the mod's code
    uint32_t changes = 0;
} g_gravity[2]; // (g_m)

struct WorldGravity {
    uintptr_t world = 0;  // the world `game` was read from
    float game[3] = {};   // its gravity as the game made it
    bool applied = false; // ours is in it
    uint32_t seen = 0;    // the change last applied
    ULONGLONG census = 0; // when its bodies were last counted
} g_worldGravity[2][2]; // (g_m) [realm][0 main, 1 effect world]

const char* worldName(int r, int kind) { return kind ? (r ? "server's other" : "client's effect") : (r ? "server" : "client"); }

// How many of a Havok world's bodies can move (gravity moves only those), from its body array.
void censusLocked(uintptr_t hk, int r, int kind) {
    const uintptr_t bodies = mem::readOr<uintptr_t>(hk + c::HavokWorld_Bodies);
    if (!bodies) return;
    constexpr uint32_t kChunk = 256;
    std::vector<uint8_t> buf(kChunk * kBodySize);
    int live = 0, dynamic = 0, active = 0, keyframed = 0, unowned = 0, shown = 0;
    for (uint32_t first = 0; first < 0x10000; first += kChunk) {
        if (!mem::read(bodies + uint64_t(first) * kBodySize, buf.data(), buf.size())) break; // past the array
        for (uint32_t k = 0; k < kChunk; ++k) {
            const uint8_t* b = buf.data() + size_t(k) * kBodySize;
            uint32_t id, flags, filter, motion;
            uint64_t user;
            memcpy(&id, b + 0x60, 4), memcpy(&flags, b + 0x58, 4), memcpy(&filter, b + 0x5c, 4), memcpy(&motion, b + 0x68, 4), memcpy(&user, b + 0x88, 8);
            if (id != first + k || !flags) continue; // a free entry
            ++live;
            if (!motion) continue;
            if (!(flags & 2)) {
                ++keyframed;
                continue;
            }
            ++dynamic;
            if (flags & 8) ++active; // hknp's IS_ACTIVE (simulated this step), if the flags are hknp's usual ones
            if (!user) ++unowned;
            if (shown++ < 8) {
                float at[3], v[4] = {};
                memcpy(at, b + 0x30, 12);
                if (const uintptr_t motions = mem::readOr<uintptr_t>(hk + c::HavokWorld_Motions)) mem::read(motions + uint64_t(motion) * 0x80 + 0x40, v, 16);
                GN_LOG("gravity:   moving body %u (flags %x, layer %u, user data %llx) at %.1f %.1f %.1f, velocity %.2f %.2f %.2f", id, flags,
                       filter & 31u, static_cast<unsigned long long>(user), at[0], at[1], at[2], v[0], v[1], v[2]);
            }
        }
    }
    GN_LOG("gravity: the %s world has %d bodies: %d dynamic (gravity and pushes move these; %d with flag 8, %d with no user data), %d keyframed (the game moves these)",
           worldName(r, kind), live, dynamic, active, unowned, keyframed);
}

void gravityLocked(uintptr_t world, int r, int kind) {
    Gravity& gr = g_gravity[r];
    WorldGravity& w = g_worldGravity[r][kind];
    if (!gr.want && !(w.applied && w.world == world)) return;
    const uintptr_t hk = mem::readOr<uintptr_t>(world + c::PhysicsWorld_HavokWorld);
    if (!hk) return;
    if (w.world != world) {
        w = WorldGravity{};
        w.world = world;
        if (!mem::read(hk + c::HavokWorld_Gravity, w.game, sizeof(w.game))) return;
    }
    float now[3] = {};
    mem::read(hk + c::HavokWorld_Gravity, now, sizeof(now));
    if (gr.want) {
        if (memcmp(now, gr.g, sizeof(now)) != 0) mem::write(hk + c::HavokWorld_Gravity, gr.g, sizeof(gr.g));
        const bool first = !w.applied;
        w.applied = true;
        if (w.seen == gr.changes) return;
        // A count after each change (at most every 2 s), so one after killing someone sees ragdolls.
        if (first) GN_LOG("gravity: the %s world's is now %.2f %.2f %.2f (the game's %.2f %.2f %.2f)", worldName(r, kind), gr.g[0], gr.g[1], gr.g[2],
                          w.game[0], w.game[1], w.game[2]);
        if (GetTickCount64() - w.census < 2000) return;
        w.seen = gr.changes, w.census = GetTickCount64();
        if (!first) GN_LOG("gravity: the %s world's is now %.2f %.2f %.2f", worldName(r, kind), gr.g[0], gr.g[1], gr.g[2]);
        guardHook("gravity census", [&] { censusLocked(hk, r, kind); });
    } else {
        mem::write(hk + c::HavokWorld_Gravity, w.game, sizeof(w.game));
        GN_LOG("gravity: the %s world's is the game's again", worldName(r, kind));
        w.applied = false;
    }
}

bool gravityAppliedLocked() {
    for (auto& realm : g_worldGravity)
        for (const WorldGravity& w : realm)
            if (w.applied) return true;
    return false;
}

using FlushFn = void (*)(uintptr_t world);
FlushFn g_flush = nullptr;
std::atomic<bool> g_off{false}; // boxes switched off (not used yet: a fault reaches the crash handler)
std::atomic<int> g_flushes{0};
std::atomic<uint64_t> g_serverFlushes{0}; // flushes of the server's world (every one, boxes or not)
std::atomic<uint64_t> g_staleFlush{0};    // the server flush count a destroy is waiting for (0: none)
std::atomic<int64_t> g_staleSince{0};     // when it was noted (ms, steady clock)
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

uintptr_t havokWorld(uintptr_t world) { return mem::readOr<uintptr_t>(world + c::PhysicsWorld_HavokWorld); }
uintptr_t havokBody(uintptr_t hk, uint32_t id) {
    const uintptr_t bodies = mem::readOr<uintptr_t>(hk + c::HavokWorld_Bodies);
    return bodies ? bodies + uint64_t(id) * kBodySize : 0;
}

// 0: the client's main world, 1: the server's, -1: another (the client's effect world).
int realmOf(uintptr_t world) {
    const uintptr_t realm = mem::readOr<uintptr_t>(world + 8);
    const int32_t r = realm ? mem::readOr<int32_t>(realm + 0x10, -1) : -1;
    if (r == 0) return mem::readOr<uint8_t>(world + c::PhysicsWorld_EffectFlag, 1) == 0 ? 0 : -1;
    return r == 1 ? 1 : -1;
}

// The world's realm (0 client, 1 server) whatever kind it is; -1 if unknown.
int anyRealmOf(uintptr_t world) {
    const uintptr_t realm = mem::readOr<uintptr_t>(world + 8);
    const int32_t r = realm ? mem::readOr<int32_t>(realm + 0x10, -1) : -1;
    return r == 0 || r == 1 ? r : -1;
}

// A live static body record (RigidBody) of this world whose Havok body points back at it.
bool donorAt(uintptr_t world, uint32_t index, Donor& out) {
    const uintptr_t records = mem::readOr<uintptr_t>(world + c::PhysicsWorld_Records);
    const uintptr_t rec = records ? mem::readOr<uintptr_t>(records + uint64_t(index) * 8) : 0;
    if (rec < 0x10000 || mem::readOr<uintptr_t>(rec) != mem::rva(c::RigidBody_vtable)) return false; // free entries hold list links
    if (mem::readOr<uintptr_t>(rec + c::RigidBody_World) != world) return false;
    const uint32_t body = mem::readOr<uint32_t>(rec + c::RigidBody_HavokBody, kNoBody);
    const uintptr_t hk = havokWorld(world);
    const uintptr_t b = hk && body < 0x10000 ? havokBody(hk, body) : 0;
    if (!b) return false;
    const uint64_t user = mem::readOr<uint64_t>(b + 0x88);
    if ((user & 0xffff) != index || mem::readOr<uint32_t>(b + 0x68, 1) != 0) return false; // its record, and static (motion 0)
    const uint32_t filter = mem::readOr<uint32_t>(b + 0x5c);
    if ((filter & 0x1f) != 1) return false; // layer Static
    out.userData = user;
    out.filter = filter;
    out.material = mem::readOr<uint16_t>(b + 0x6c);
    out.quality = mem::readOr<uint8_t>(b + 0x6e);
    out.body = body;
    out.record = index;
    out.ok = true;
    return true;
}

bool donorAlive(uintptr_t world, const Donor& d) {
    Donor now;
    return d.ok && donorAt(world, d.record, now) && now.body == d.body && now.userData == d.userData;
}

// Any live static body of the world, from the records (a few thousand reads, after a flush).
bool findDonor(uintptr_t world, Donor& out) {
    const uint32_t count = std::min<uint32_t>(mem::readOr<uint32_t>(world + c::PhysicsWorld_RecordCount), 65536);
    for (uint32_t i = 0; i < count; ++i)
        if (donorAt(world, i, out)) return true;
    out = Donor{};
    return false;
}

uint32_t freePlaces(uintptr_t world, uintptr_t hk) {
    // The body manager at hknpWorld +0x10: its count at +0x58 (incremented by the id allocator). The
    // room is the world's record count (the description's body count: 65535 with GridNode's fix).
    const uint32_t used = mem::readOr<uint32_t>(hk + 0x68, 0xffffffffu);
    const uint32_t room = mem::readOr<uint32_t>(world + c::PhysicsWorld_RecordCount);
    return used >= room ? 0 : room - used;
}

// The shapes, one per size, kept for the game's life (a few hundred bytes each; Havok frees a shape
// with its last body otherwise, and boxes come and go).
struct Shape {
    float half[3];
    uintptr_t shape;
};
std::vector<Shape> g_shapes; // (flush thread)

uintptr_t boxShape(const float* half) {
    for (const Shape& s : g_shapes)
        if (std::fabs(s.half[0] - half[0]) + std::fabs(s.half[1] - half[1]) + std::fabs(s.half[2] - half[2]) < 1e-4f) return s.shape;
    alignas(16) uint8_t cfg[0x80] = {};
    alignas(16) float h[4] = {half[0], half[1], half[2], 0.f};
    using CfgFn = void* (*)(void*);
    using BoxFn = uintptr_t (*)(const float*, float, void*);
    void* config = reinterpret_cast<CfgFn>(mem::rva(e::function::hknpConvexShape_BuildConfig_ctor))(cfg);
    const uintptr_t shape = reinterpret_cast<BoxFn>(mem::rva(e::function::hknpConvexShape_createFromHalfExtents))(h, 0.01f, config);
    if (!shape) return 0;
    mem::write(shape + 0x18, "\0\0\0\0\0\0\0\0", 8); // its user data (material index): 0, as the cinematic tools' boxes
    // The game's own factory holds one reference and lets it go after making the body; this keeps it.
    g_shapes.push_back({{half[0], half[1], half[2]}, shape});
    return shape;
}

// The game's math: a LinearTransform's rotation as a quaternion, and a Havok transform from it.
void toQuat(const gn_transform& t, float* quat) {
    alignas(16) gn_transform rows = t;
    rows.right[3] = rows.up[3] = rows.forward[3] = 0.f, rows.trans[3] = 1.f;
    reinterpret_cast<void (*)(float*, const gn_transform*)>(mem::rva(e::function::Quat_fromLinearTransform))(quat, &rows);
}

uint32_t makeBody(uintptr_t world, const Donor& donor, const Box& b) {
    const uintptr_t hk = havokWorld(world);
    if (!hk || !donor.ok || freePlaces(world, hk) < kMinFree) return kNoBody;
    const uintptr_t shape = boxShape(b.half);
    if (!shape) return kNoBody;
    alignas(16) uint8_t cinfo[0x60] = {};
    reinterpret_cast<void (*)(void*)>(mem::rva(e::function::hknpBodyCinfo_ctor))(cinfo);
    alignas(16) float quat[4] = {0, 0, 0, 1};
    toQuat(b.at, quat);
    memcpy(cinfo + 0x00, &shape, 8);
    const uint32_t motion = 0; // static
    memcpy(cinfo + 0x0c, &motion, 4);
    cinfo[0x10] = donor.quality;
    memcpy(cinfo + 0x12, &donor.material, 2);
    memcpy(cinfo + 0x14, &donor.filter, 4);
    const float pos[4] = {b.at.trans[0], b.at.trans[1], b.at.trans[2], 0.f};
    memcpy(cinfo + 0x30, pos, 16);
    memcpy(cinfo + 0x40, quat, 16);
    uint32_t id = kNoBody;
    // Made without adding it (flags bit 0), so its user data is the donor's before anything can hit it.
    reinterpret_cast<void (*)(uintptr_t, uint32_t*, void*, uint8_t)>(mem::rva(e::function::hknpWorld_createBody))(hk, &id, cinfo, 1);
    const uintptr_t body = id < 0x10000 ? havokBody(hk, id) : 0;
    if (!body) return kNoBody;
    mem::write(body + 0x88, &donor.userData, 8);
    reinterpret_cast<void (*)(uintptr_t, const uint32_t*, int32_t, uint32_t)>(mem::rva(e::function::hknpWorld_addBodies))(hk, &id, 1, 0);
    return id;
}

void placeBody(uintptr_t world, uint32_t id, const gn_transform& at) {
    const uintptr_t hk = havokWorld(world);
    if (!hk) return;
    alignas(16) float quat[4] = {0, 0, 0, 1};
    toQuat(at, quat);
    alignas(16) float t[16] = {};
    reinterpret_cast<void (*)(float*, const float*)>(mem::rva(e::function::hkTransform_fromQuat))(t, quat);
    t[12] = at.trans[0], t[13] = at.trans[1], t[14] = at.trans[2], t[15] = 1.f;
    reinterpret_cast<void (*)(uintptr_t, uint32_t, const float*, int32_t)>(mem::rva(e::function::hknpWorld_setBodyTransform))(hk, id, t, 1);
}

void destroyBodies(uintptr_t world, const std::vector<uint32_t>& ids) {
    const uintptr_t hk = havokWorld(world);
    if (!hk || ids.empty()) return;
    reinterpret_cast<void (*)(uintptr_t, const uint32_t*, int32_t, int32_t)>(mem::rva(e::function::hknpWorld_destroyBodies))(
        hk, ids.data(), static_cast<int32_t>(ids.size()), 1);
}


// ---- standalone boxes (box_backend 1) ----------------------------------------------------------------
// The calls a barrier (PamOBBCollisionEntity) makes to build its body (PamOBBCollision_enableClient),
// with a box's size in the description. Needs a live client barrier for the setup the factory reads
// (its bus, its data's first RigidBodyData); nothing of it is kept by the new object but the parent
// pointer (the barrier's +0x30), so a box can outlive it only if the game does not read that again.
struct NativeCtx {
    uintptr_t gw = 0, ent = 0, bus = 0, rigid = 0, owner = 0, factory = 0;
    char why[200] = {}; // when nothing is usable: where it stopped (logged, a few times)
};

uintptr_t g_nativeEnt = 0; // (flush thread) the barrier last used
ULONGLONG g_worldSince[2] = {}; // (g_m) when each realm's physics world was last replaced

bool nativeContext(NativeCtx& c) {
    static const reflect::Type* type = registry().find("PamOBBCollisionEntity");
    auto fail = [&](const char* what) {
        snprintf(c.why, sizeof(c.why), "%s", what);
        return false;
    };
    if (!type) return fail("the type PamOBBCollisionEntity is not known");
    c.gw = mem::readOr<uintptr_t>(mem::rva(e::global::GameWorlds)); // [0]: the client's
    if (!c.gw) return fail("no client game world");
    const uintptr_t mgr = mem::readOr<uintptr_t>(c.gw + 0x50), factories = mem::readOr<uintptr_t>(c.gw + 0x58);
    if (!mgr || !factories) return fail("the game world has no physics manager or factory list");
    int server = 0, noBus = 0, noData = 0, noRigid = 0, noOwner = 0;
    auto usableEnt = [&](uintptr_t ent) {
        if ((mem::readOr<uint32_t>(ent + 0x18) & 1) != 0) return ++server, false; // the server's
        c.ent = ent;
        c.bus = mem::readOr<uintptr_t>(ent + 0x20);
        const uintptr_t data = mem::readOr<uintptr_t>(ent + 0x28);
        // The body setups array: +0x70 in the data's reflection (PhysicsBodies); the client barrier's enable reads +0x78.
        uintptr_t arr = 0;
        for (const uintptr_t off : {0x70, 0x78}) {
            const uintptr_t cand = data ? mem::readOr<uintptr_t>(data + off) : 0;
            if (cand > 0x10000 && mem::readOr<int32_t>(cand - 4, 0) > 0 && mem::readOr<uintptr_t>(cand) > 0x10000) {
                arr = cand;
                break;
            }
        }
        if (!c.bus) return ++noBus, false;
        if (!arr) {
            static int dumped = 0;
            if (data && dumped++ < 2) {
                uint64_t q[8] = {};
                mem::read(data + 0x60, q, sizeof(q));
                GN_LOG("bodies: barrier data %llx +0x60..: %llx %llx %llx %llx %llx %llx %llx %llx", static_cast<unsigned long long>(data), static_cast<unsigned long long>(q[0]),
                       static_cast<unsigned long long>(q[1]), static_cast<unsigned long long>(q[2]), static_cast<unsigned long long>(q[3]), static_cast<unsigned long long>(q[4]),
                       static_cast<unsigned long long>(q[5]), static_cast<unsigned long long>(q[6]), static_cast<unsigned long long>(q[7]));
            }
            return ++noData, false;
        }
        c.rigid = mem::readOr<uintptr_t>(arr);
        if (!c.rigid) return ++noRigid, false;
        const uintptr_t table = mem::readOr<uintptr_t>(mgr + 0xf0);
        c.owner = table ? mem::readOr<uintptr_t>(table + uint64_t(mem::readOr<uint8_t>(c.rigid + 0x2a)) * 8) : 0;
        return c.owner != 0 || (++noOwner, false);
    };
    if (g_nativeEnt && inspector::alive(g_nativeEnt, type) && usableEnt(g_nativeEnt)) {
    } else {
        g_nativeEnt = 0;
        size_t tracked = 0;
        for (const auto& [ent, t] : inspector::trackedOf(type)) {
            ++tracked;
            if (inspector::alive(ent, t) && usableEnt(ent)) {
                g_nativeEnt = ent;
                break;
            }
        }
        if (!g_nativeEnt) {
            snprintf(c.why, sizeof(c.why), "no usable client barrier: %zu tracked, %d server's, %d without a bus, %d without data, %d without a body setup, %d without a physics owner",
                     tracked, server, noBus, noData, noRigid, noOwner);
            return false;
        }
    }
    using FindFn = uintptr_t (*)(uintptr_t, const void*);
    c.factory = reinterpret_cast<FindFn>(mem::rva(e::function::PhysicsFactory_find))(factories, reinterpret_cast<const void*>(mem::rva(e::global::TI_PhysicsEntityFactory)));
    if (!c.factory) return fail("the game world has no PhysicsEntityFactory");
    return true;
}

uintptr_t nativeOwner(uintptr_t gw, uint32_t id) {
    const uintptr_t mgr = gw ? mem::readOr<uintptr_t>(gw + 0x50) : 0;
    const uintptr_t table = mgr ? mem::readOr<uintptr_t>(mgr + 0xf0) : 0;
    return table ? mem::readOr<uintptr_t>(table + uint64_t((id >> 16) & 0xff) * 8) : 0;
}

void nativePlace(Box& b, uintptr_t owner, uint32_t id) {
    if (!b.ntf) {
        b.ntf = static_cast<gn_transform*>(_aligned_malloc(sizeof(gn_transform), 16)); // leaked on purpose (see ntf)
        if (!b.ntf) return;
    }
    *b.ntf = b.at;
    b.ntf->right[3] = b.ntf->up[3] = b.ntf->forward[3] = 0.f, b.ntf->trans[3] = 1.f;
    using XformFn = void (*)(uintptr_t, uint32_t*, const gn_transform*);
    uint32_t a = id, n = id;
    reinterpret_cast<XformFn>(mem::rva(e::function::RigidBody_transformSource))(owner, &a, b.ntf);
    reinterpret_cast<void (*)(uintptr_t, uint32_t*, const gn_transform*, bool)>(mem::rva(e::function::RigidBody_setTransformNative))(owner, &n, b.ntf, true);
}

bool nativeMake(const NativeCtx& c, Box& b) {
    alignas(16) uint8_t desc[0x40] = {}; // a box's description: vtable[0]() is 1; kind 2 (static), layer, half extents
    const uintptr_t vt = mem::rva(c::PhysicsBoxDesc_vtable);
    memcpy(desc, &vt, 8);
    const uint32_t kind = 2, layer = 1;
    memcpy(desc + 0x14, &kind, 4);
    memcpy(desc + 0x1c, &layer, 4);
    memcpy(desc + 0x20, b.half, 12);
    alignas(16) uint8_t info[0x100] = {};
    reinterpret_cast<void* (*)(void*, uintptr_t, uint32_t, uintptr_t, uintptr_t)>(mem::rva(e::function::PhysicsCreationInfo_ctor))(info, c.bus, 0, c.owner, c.ent + 0x30);
    uint32_t id = kNoBody;
    const uint32_t* made = reinterpret_cast<const uint32_t* (*)(uintptr_t, uint32_t*, void*, uintptr_t, void*)>(mem::rva(e::function::PhysicsEntityFactory_create))(
        c.factory, &id, info, c.rigid, desc);
    if (made) id = *made;
    if (id == kNoBody || id == 0xffffffffu) return false;
    nativePlace(b, c.owner, id);
    if (!b.ntf) return false;
    uint32_t t1 = id, t2 = id;
    reinterpret_cast<void (*)(uintptr_t, uint32_t*)>(mem::rva(e::function::RigidBody_enable))(c.owner, &t1);
    reinterpret_cast<void (*)(uintptr_t, uint32_t*)>(mem::rva(e::function::RigidBody_registerSpatial))(c.owner, &t2);
    b.nid = id;
    b.parent = c.ent;
    return true;
}

// What PamOBBCollision_disable and _deinit do.
void nativeDestroy(uintptr_t gw, uint32_t id) {
    const uintptr_t owner = nativeOwner(gw, id);
    if (!owner) return;
    uint32_t a = id, d = id, k = id;
    reinterpret_cast<void (*)(uintptr_t, uint32_t*)>(mem::rva(e::function::RigidBody_unregisterSpatial))(owner, &a);
    reinterpret_cast<void (*)(uintptr_t, uint32_t*)>(mem::rva(e::function::RigidBody_disable))(owner, &d);
    const uint32_t args[2] = {0, 1}; // {realm, 1}
    reinterpret_cast<void (*)(uintptr_t, uint32_t*, const uint32_t*)>(mem::rva(e::function::RigidBody_destroy))(owner, &k, args);
}

// After the game's flush of one realm's world, on its thread (g_m held). `nat`: a barrier's setup for
// native boxes (client world, box_backend 1), else null.
void afterFlushLocked(uintptr_t world, int r, const NativeCtx* nat) {
    Donor& donor = g_donor[r];
    if (world != g_world[r]) {
        // A new world (a level load): the bodies died with the old one.
        for (Box& b : g_boxes) b.body[r] = kNoBody, b.failed[r] = false, b.nid = r == 0 ? kNoBody : b.nid, b.parent = r == 0 ? 0 : b.parent;
        g_world[r] = world;
        g_worldSince[r] = GetTickCount64();
        if (r == 0) g_nativeEnt = 0;
        donor = Donor{};
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 8) GN_LOG("bodies: the %s physics world is %llx", r ? "server" : "client", static_cast<unsigned long long>(world));
    }
    if (g_boxes.empty()) return;
    // The donor: its record may have died in this flush.
    static ULONGLONG lastSearch[2] = {};
    if (!donorAlive(world, donor)) {
        const Donor was = donor;
        // A search reads up to 65,536 records: at most twice a second while none is found.
        if (was.ok || GetTickCount64() - lastSearch[r] >= 500) {
            lastSearch[r] = GetTickCount64();
            findDonor(world, donor);
            if (donor.ok) {
                int rebound = 0;
                const uintptr_t hk = havokWorld(world);
                for (const Box& b : g_boxes)
                    if (b.body[r] != kNoBody) {
                        mem::write(havokBody(hk, b.body[r]) + 0x88, &donor.userData, 8);
                        ++rebound;
                    }
                if (was.ok && rebound)
                    GN_LOG("bodies: %d %s box(es) now carry static body %u's record (%u's is gone)", rebound, r ? "server" : "client", donor.body,
                           was.body);
            } else {
                std::vector<uint32_t> ids;
                for (Box& b : g_boxes)
                    if (b.body[r] != kNoBody) ids.push_back(b.body[r]), b.body[r] = kNoBody;
                destroyBodies(world, ids);
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 5)
                    GN_LOG("bodies: no static body to lend its record in the %s world; %d box(es) waiting", r ? "server" : "client",
                           static_cast<int>(g_boxes.size()));
            }
        } else {
            donor = Donor{};
        }
    }
    std::vector<uint32_t> gone;
    const uintptr_t gw = r == 0 ? mem::readOr<uintptr_t>(mem::rva(e::global::GameWorlds)) : 0;
    int made = 0;
    for (auto it = g_boxes.begin(); it != g_boxes.end();) {
        Box& b = *it;
        if (b.removed) {
            if (b.body[r] != kNoBody) gone.push_back(b.body[r]), b.body[r] = kNoBody;
            if (r == 0 && b.nid != kNoBody) {
                guardHook("native box destroy", [&] { nativeDestroy(gw, b.nid); });
                b.nid = kNoBody;
            }
            // Gone from both worlds (or never made in the other): forgotten.
            if (b.body[0] == kNoBody && b.body[1] == kNoBody && b.nid == kNoBody) {
                it = g_boxes.erase(it);
                continue;
            }
        } else if (b.body[r] == kNoBody && b.nid == kNoBody && !b.failed[r] && made < 32 && r == 0 && g_native.load()) {
            // A new world: the level is still loading (a box made then crashed the game when Faith touched it). Wait 3 s.
            if (nat && GetTickCount64() - g_worldSince[0] >= 3000) { // else it waits for a barrier to copy the setup from
                bool ok = false;
                guardHook("native box make", [&] { ok = nativeMake(*nat, b); });
                b.moved[r] = false;
                if (ok) {
                    GN_LOG("[%s] box %u: made as a game physics object (id %x)", b.mod.c_str(), b.handle, b.nid);
                } else {
                    b.failed[r] = true;
                    GN_LOG("[%s] box %u: the game's factory did not make it", b.mod.c_str(), b.handle);
                }
                ++made;
            }
        } else if (b.body[r] == kNoBody && b.nid == kNoBody && !b.failed[r] && donor.ok && made < 32) {
            b.body[r] = makeBody(world, donor, b);
            b.moved[r] = false;
            if (b.body[r] == kNoBody) {
                b.failed[r] = true;
                GN_LOG("[%s] box %u: Havok did not make it in the %s world (body buffer nearly full?)", b.mod.c_str(), b.handle,
                       r ? "server" : "client");
            }
            ++made;
        } else if (b.nid != kNoBody && b.moved[r] && r == 0) {
            guardHook("native box move", [&] { nativePlace(b, nativeOwner(gw, b.nid), b.nid); });
            b.moved[r] = false;
        } else if (b.body[r] != kNoBody && b.moved[r]) {
            placeBody(world, b.body[r], b.at);
            b.moved[r] = false;
        }
        ++it;
    }
    destroyBodies(world, gone);
}

void flushHook(uintptr_t world) {
    g_flush(world);
    const int r = realmOf(world);
    if (r == 1) g_serverFlushes.fetch_add(1);
    if (r < 0) {
        if (const int any = anyRealmOf(world); any >= 0) {
            std::lock_guard<std::mutex> lock(g_m);
            gravityLocked(world, any, 1);
        }
        return;
    }
    if (g_off.load()) return;
    g_flushes.fetch_add(1);
    // The setup for native boxes is found before g_m: it takes the entity lists' lock.
    NativeCtx nat;
    bool haveNat = false;
    if (r == 0 && g_native.load()) {
        guardHook("native setup", [&] { haveNat = nativeContext(nat); });
        static ULONGLONG lastSaid = 0;
        static int said = 0;
        if (!haveNat && nat.why[0] && said < 12 && GetTickCount64() - lastSaid > 5000) {
            lastSaid = GetTickCount64(), ++said;
            GN_LOG("bodies: standalone boxes cannot be made yet: %s", nat.why);
        }
    }
    std::lock_guard<std::mutex> lock(g_m);
    gravityLocked(world, r, 0);
    if (g_boxes.empty() && world == g_world[r]) return;
    guardHook("bodies", [&] { afterFlushLocked(world, r, haveNat ? &nat : nullptr); });
}

// A client barrier is being destroyed (still whole): native boxes made from it go now. The game keeps the
// barrier's pointer in each box (its surface classification reads it when Faith touches the box), so one
// outliving it crashed the game. They are made again from another barrier at the next flush.
void barrierDestroyed(void* entity, void*) {
    const uintptr_t ent = reinterpret_cast<uintptr_t>(entity);
    std::lock_guard<std::mutex> lock(g_m);
    for (Box& b : g_boxes) {
        if (b.nid == kNoBody || b.parent != ent) continue;
        // Only while the world and the object are still there (a level ending takes the world's objects itself).
        const uintptr_t gw = mem::readOr<uintptr_t>(mem::rva(e::global::GameWorlds));
        const uintptr_t owner = nativeOwner(gw, b.nid);
        const uintptr_t table = owner ? mem::readOr<uintptr_t>(owner + c::PhysicsWorld_Records) : 0;
        const uintptr_t rec = table ? mem::readOr<uintptr_t>(table + uint64_t(b.nid & 0xffff) * 8) : 0;
        if (owner == g_world[0] && rec > 0x10000)
            guardHook("native box destroy", [&] { nativeDestroy(gw, b.nid); });
        b.nid = kNoBody, b.parent = 0, b.moved[0] = false;
    }
}

bool g_started = false;
bool g_followOk = true; // the barrier-destroyed listener is in (else standalone boxes are refused)
bool g_ready = false; // the flush is hooked

bool usable(const gn_transform& t) {
    const float* rows[4] = {t.right, t.up, t.forward, t.trans};
    for (const float* r : rows)
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(r[k]) || std::fabs(r[k]) > 1e7f) return false;
    return true;
}
} // namespace

// An entity of the server world was destroyed. The game releases a physics record only in the next
// flush (PhysicsWorld_flush: its slot at +0x130 stays until then), but its physics component is freed
// at once, so a query in between reads the freed component (a crash in a ray cast on 2026-11-06, in
// the same server tick as the destroy). Queries wait for the next flush, or a second at most.
void physicsStaleNote() {
    g_staleSince.store(nowMs());
    g_staleFlush.store(g_serverFlushes.load() + 1);
}

bool physicsQueriesSafe() {
    const uint64_t want = g_staleFlush.load();
    if (!want) return true;
    if (g_serverFlushes.load() >= want || nowMs() - g_staleSince.load() > 1000) {
        g_staleFlush.store(0);
        return true;
    }
    return false;
}

void bodiesStart() {
    if (g_started) return;
    g_started = true;
    for (const char* f : {"PhysicsWorld_flush", "hknpWorld_createBody", "hknpWorld_addBodies", "hknpWorld_destroyBodies",
                          "hknpWorld_setBodyTransform", "hknpConvexShape_createFromHalfExtents", "hknpConvexShape_BuildConfig_ctor",
                          "hknpBodyCinfo_ctor", "Quat_fromLinearTransform", "hkTransform_fromQuat"})
        if (!build::codeMatches(f)) {
            GN_LOG("bodies: %s does not look as expected; collision boxes are off", f);
            return;
        }
    void* original = nullptr;
    if (hookFunction("GridNode", e::function::PhysicsWorld_flush, reinterpret_cast<void*>(&flushHook), &original) != GN_OK) {
        GN_LOG("bodies: could not hook the physics flush; collision boxes are off");
        return;
    }
    g_flush = reinterpret_cast<FlushFn>(original);
    g_ready = true;
    if (onDestroyed("GridNode", "PamOBBCollisionEntity", &barrierDestroyed, nullptr) != GN_OK) GN_LOG("bodies: standalone boxes cannot follow their barrier's destruction; they are off"), g_followOk = false;
}

int boxBackend(int native) {
    if (!g_ready || g_off.load() || (native && !g_followOk)) return GN_ERR_UNAVAILABLE;
    if (g_native.exchange(native != 0) != (native != 0)) GN_LOG("bodies: new client-world boxes are made %s", native ? "by the game's physics factory (standalone, experimental)" : "as Havok bodies lending a record");
    return GN_OK;
}

int boxAdd(const char* mod, const gn_transform* at, const float* half, uint32_t* out, const void* caller) {
    if (!at || !half || !out || !usable(*at)) return GN_ERR_ARGUMENT;
    for (int k = 0; k < 3; ++k)
        if (!(half[k] >= 0.01f && half[k] <= 1000.f)) return GN_ERR_ARGUMENT;
    if (!g_ready || g_off.load()) return GN_ERR_UNAVAILABLE;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_boxes.size() >= 4096) return GN_ERR_FULL;
    Box b;
    b.handle = g_next++;
    b.mod = mod ? mod : "?";
    b.owner = reinterpret_cast<uintptr_t>(caller);
    b.at = *at;
    memcpy(b.half, half, sizeof(b.half));
    g_boxes.push_back(b);
    *out = b.handle;
    return GN_OK;
}

int boxMove(uint32_t box, const gn_transform* at) {
    if (!at || !usable(*at)) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_m);
    for (Box& b : g_boxes)
        if (b.handle == box && !b.removed) {
            b.at = *at;
            b.moved[0] = b.moved[1] = true;
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

int boxRemove(uint32_t box) {
    std::lock_guard<std::mutex> lock(g_m);
    for (Box& b : g_boxes)
        if (b.handle == box && !b.removed) {
            b.removed = true;
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

int boxState(uint32_t box) {
    std::lock_guard<std::mutex> lock(g_m);
    for (const Box& b : g_boxes)
        if (b.handle == box && !b.removed) return b.failed[0] ? -GN_ERR_FULL : b.body[0] != kNoBody || b.nid != kNoBody ? 1 : 0; // what Faith collides with
    return -GN_ERR_NOT_FOUND;
}

// A mod unloaded: its boxes go at the next flush.
void forgetBoxes(const CodeRange& r) {
    std::lock_guard<std::mutex> lock(g_m);
    for (Box& b : g_boxes)
        if (r.has(reinterpret_cast<void*>(b.owner))) b.removed = true;
}

// GridNode unloading: every box out of the world before the flush hook goes (a box left behind would
// keep a record index nobody re-checks). Waits for flushes up to `ms`; false if some are still in.
bool bodiesRelease(unsigned long ms) {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_gravity[0].want = g_gravity[1].want = false; // the game's gravity back at the next update
        if (g_boxes.empty() && !gravityAppliedLocked()) return true;
        for (Box& b : g_boxes) b.removed = true;
    }
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < ms) {
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (g_boxes.empty() && !gravityAppliedLocked()) return true;
        }
        Sleep(20);
    }
    std::lock_guard<std::mutex> lock(g_m);
    const bool inWorld = std::any_of(g_boxes.begin(), g_boxes.end(), [](const Box& b) { return b.body[0] != kNoBody || b.body[1] != kNoBody || b.nid != kNoBody; });
    if (!inWorld) g_boxes.clear();
    return !inWorld;
}

int physicsGravity(int realm, float* out) {
    if (!out || (realm != 0 && realm != 1)) return GN_ERR_ARGUMENT;
    uintptr_t world;
    {
        std::lock_guard<std::mutex> lock(g_m);
        world = g_world[realm];
    }
    const uintptr_t hk = world ? havokWorld(world) : 0;
    return hk && mem::read(hk + c::HavokWorld_Gravity, out, 12) ? GN_OK : GN_ERR_UNAVAILABLE;
}

int physicsSetGravity(int realm, const float* g, const void* caller) {
    if (realm != 0 && realm != 1) return GN_ERR_ARGUMENT;
    if (g)
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(g[k]) || std::fabs(g[k]) > 1000.f) return GN_ERR_ARGUMENT;
    if (!g_ready) return GN_ERR_UNAVAILABLE; // applied from the flush hook
    std::lock_guard<std::mutex> lock(g_m);
    Gravity& gr = g_gravity[realm];
    if (g && gr.want && memcmp(gr.g, g, sizeof(gr.g)) == 0) return GN_OK;
    gr.want = g != nullptr;
    ++gr.changes;
    if (g) memcpy(gr.g, g, sizeof(gr.g)), gr.owner = reinterpret_cast<uintptr_t>(caller);
    return GN_OK;
}

void forgetGravity(const CodeRange& r) {
    std::lock_guard<std::mutex> lock(g_m);
    for (Gravity& gr : g_gravity)
        if (gr.want && r.has(reinterpret_cast<void*>(gr.owner))) gr.want = false;
}

// What a game physics object (a RigidBody record, as a ray hit gives it) has on its Havok body: the
// filter info (its collision layer in the low 5 bits) and the material. False for other objects (a
// static model group's body).
bool bodyFilter(uintptr_t object, uint32_t& filter, uint32_t& material) {
    if (!object || mem::readOr<uintptr_t>(object) != mem::rva(c::RigidBody_vtable)) return false;
    const uintptr_t world = mem::readOr<uintptr_t>(object + c::RigidBody_World);
    const uint32_t id = mem::readOr<uint32_t>(object + c::RigidBody_HavokBody, kNoBody);
    const uintptr_t hk = world ? havokWorld(world) : 0;
    const uintptr_t b = hk && id < 0x10000 ? havokBody(hk, id) : 0;
    if (!b) return false;
    filter = mem::readOr<uint32_t>(b + 0x5c);
    material = mem::readOr<uint16_t>(b + 0x6c);
    return true;
}

} // namespace gn::host
