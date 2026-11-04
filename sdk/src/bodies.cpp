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
#include <cmath>
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
};

std::mutex g_m;
std::vector<Box> g_boxes; // (g_m)
uint32_t g_next = 1;      // (g_m)
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

// After the game's flush of one realm's world, on its thread (g_m held).
void afterFlushLocked(uintptr_t world, int r) {
    Donor& donor = g_donor[r];
    if (world != g_world[r]) {
        // A new world (a level load): the bodies died with the old one.
        for (Box& b : g_boxes) b.body[r] = kNoBody, b.failed[r] = false;
        g_world[r] = world;
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
    int made = 0;
    for (auto it = g_boxes.begin(); it != g_boxes.end();) {
        Box& b = *it;
        if (b.removed) {
            if (b.body[r] != kNoBody) gone.push_back(b.body[r]), b.body[r] = kNoBody;
            // Gone from both worlds (or never made in the other): forgotten.
            if (b.body[0] == kNoBody && b.body[1] == kNoBody) {
                it = g_boxes.erase(it);
                continue;
            }
        } else if (b.body[r] == kNoBody && !b.failed[r] && donor.ok && made < 32) {
            b.body[r] = makeBody(world, donor, b);
            b.moved[r] = false;
            if (b.body[r] == kNoBody) {
                b.failed[r] = true;
                GN_LOG("[%s] box %u: Havok did not make it in the %s world (body buffer nearly full?)", b.mod.c_str(), b.handle,
                       r ? "server" : "client");
            }
            ++made;
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
    if (r < 0) {
        if (const int any = anyRealmOf(world); any >= 0) {
            std::lock_guard<std::mutex> lock(g_m);
            gravityLocked(world, any, 1);
        }
        return;
    }
    if (g_off.load()) return;
    g_flushes.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_m);
    gravityLocked(world, r, 0);
    if (g_boxes.empty() && world == g_world[r]) return;
    guardHook("bodies", [&] { afterFlushLocked(world, r); });
}

bool g_started = false;
bool g_ready = false; // the flush is hooked

bool usable(const gn_transform& t) {
    const float* rows[4] = {t.right, t.up, t.forward, t.trans};
    for (const float* r : rows)
        for (int k = 0; k < 3; ++k)
            if (!std::isfinite(r[k]) || std::fabs(r[k]) > 1e7f) return false;
    return true;
}
} // namespace

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
        if (b.handle == box && !b.removed) return b.failed[0] ? -GN_ERR_FULL : b.body[0] != kNoBody ? 1 : 0; // what Faith collides with
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
    const bool inWorld = std::any_of(g_boxes.begin(), g_boxes.end(), [](const Box& b) { return b.body[0] != kNoBody || b.body[1] != kNoBody; });
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
