// Contact and activation events of one physics body (API 41), through Havok's own per-body signals, as the
// game's physics entities take them (PhysicsManager_detachBodySignals 0x1434f9e70 detaches the game's five
// handlers the same way): hknpWorld_getEventSignal(world, type, havok body index) and hkSignal_attach /
// hkSignal_detach (db/annotations/physics.yaml; read in Ghidra 2026-11-14, from Gemini's answer B).
// Havok fires most of these inline on its worker threads during the physics step: the handler only copies the
// event; mods hear it on the body's realm thread. Untested in game.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/mod_call.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;

// RigidBody: +0x30 its physics owner (whose +0xe0 is the Havok world), +0x58 the Havok body index (FUN_1434f13f0).
constexpr uint64_t kBodyOwner = 0x30, kBodyIndex = 0x58, kOwnerWorld = 0xe0;
constexpr uint32_t kTypes[] = {GN_CONTACT_IMPULSE, GN_CONTACT_ACTIVATION}; // Havok event types 5 and 10
constexpr size_t kMaxWatches = 1024, kMaxPending = 4096;

struct Obj {
    uint32_t handle; // what the signal node hands back (its object)
};

struct Watch {
    std::string mod;
    uintptr_t entity = 0, owner = 0, world = 0;
    int32_t index = -1;
    uint32_t types = 0; // bits 1 << event type
    gn_contact_fn fn = nullptr;
    void* user = nullptr;
    Obj* obj = nullptr;
    bool server = false;
    bool live = true;     // calls go to the mod
    bool attached = true; // in Havok's lists (cleared on the realm thread by the detach)
};

std::recursive_mutex g_m;
std::unordered_map<uint32_t, Watch> g_watches; // by handle (g_m)
std::deque<Obj> g_objs;                         // stable addresses, freed with gridnode.dll (g_m)
uint32_t g_next = 1;
bool g_destroyHooked = false;

struct Pending {
    uint32_t handle;
    gn_contact c;
};
std::mutex g_pm;
std::vector<Pending> g_pending; // (g_pm) from the worker threads
std::atomic<bool> g_drainServer{false}, g_drainClient{false};
std::atomic<uint32_t> g_dropped{0};

using GetSignalFn = uint64_t* (*)(uintptr_t world, uint16_t type, int32_t index);
using AttachFn = void (*)(uint64_t* signal, void* object, void* fn);
using DetachFn = void* (*)(uint64_t* signal, uint8_t* removed, void* object, void* fn);

void deliver(bool server);

// Havok's thread (a worker during the step, or the realm's): event +0x04 its type (u16), +0x08 / +0x0c the two
// Havok body indices (activation: +0x08 the body, +0x0c a byte, activated); contact impulse +0x90 (f32).
void contactEvent(Obj* obj, void* /*context*/, const uint8_t* ev) {
    guardHook("contact event", [&] {
        if (!obj || !ev || unloaded()) return;
        gn_contact c{};
        c.type = *reinterpret_cast<const uint16_t*>(ev + 4);
        c.body_a = *reinterpret_cast<const uint32_t*>(ev + 8);
        if (c.type == GN_CONTACT_ACTIVATION) {
            c.body_b = 0xffffffffu;
            c.activated = ev[0x0c] != 0;
        } else {
            c.body_b = *reinterpret_cast<const uint32_t*>(ev + 0x0c);
            c.impulse = *reinterpret_cast<const float*>(ev + 0x90);
        }
        bool server = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_m);
            const auto it = g_watches.find(obj->handle);
            if (it == g_watches.end() || !it->second.live || !(it->second.types & (1u << c.type))) return;
            server = it->second.server;
        }
        {
            std::lock_guard<std::mutex> lock(g_pm);
            if (g_pending.size() >= kMaxPending) {
                g_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            g_pending.push_back({obj->handle, c});
        }
        std::atomic<bool>& flag = server ? g_drainServer : g_drainClient;
        if (!flag.exchange(true)) (server ? gn::host::server() : client()).post([server] { deliver(server); });
    });
}

// The realm thread: the events for this realm's watches to their mods.
void deliver(bool server) {
    (server ? g_drainServer : g_drainClient) = false;
    std::vector<Pending> mine;
    {
        std::lock_guard<std::mutex> lock(g_pm);
        std::vector<Pending> rest;
        std::lock_guard<std::recursive_mutex> l(g_m);
        for (const Pending& p : g_pending) {
            const auto it = g_watches.find(p.handle);
            if (it == g_watches.end()) continue;
            (it->second.server == server ? mine : rest).push_back(p);
        }
        g_pending.swap(rest);
    }
    for (const Pending& p : mine) {
        Watch w;
        {
            std::lock_guard<std::recursive_mutex> lock(g_m);
            const auto it = g_watches.find(p.handle);
            if (it == g_watches.end() || !it->second.live) continue;
            w = it->second;
        }
        try {
            modcall::Scope scope(w.fn);
            w.fn(&p.c, w.user);
        } catch (...) {
            GN_LOG("contacts: %s's contact callback threw; ignored", w.mod.c_str());
        }
    }
}

// (realm thread) Out of Havok's lists, if the body's world is still the one it was attached in: a new world (a level
// load) freed the old one's signals with it.
void detach(Watch& w) {
    if (!w.attached) return;
    w.attached = false;
    if (mem::readOr<uintptr_t>(w.owner + kOwnerWorld) != w.world) return;
    const auto get = reinterpret_cast<GetSignalFn>(mem::rva(e::function::hknpWorld_getEventSignal));
    const auto off = reinterpret_cast<DetachFn>(mem::rva(e::function::hkSignal_detach));
    for (uint32_t type : kTypes) {
        if (!(w.types & (1u << type))) continue;
        uint8_t removed = 0;
        off(get(w.world, static_cast<uint16_t>(type), w.index), &removed, w.obj, reinterpret_cast<void*>(&contactEvent));
    }
}

// A watched entity is about to be destroyed (its realm thread): out now, while its body is still in the world.
void watchedDestroyed(void* entity, void*) {
    std::lock_guard<std::recursive_mutex> lock(g_m);
    for (auto it = g_watches.begin(); it != g_watches.end();) {
        if (it->second.entity != reinterpret_cast<uintptr_t>(entity)) {
            ++it;
            continue;
        }
        detach(it->second);
        it = g_watches.erase(it);
    }
}

template <class Pick> void detachLater(Pick which) {
    std::lock_guard<std::recursive_mutex> lock(g_m);
    for (auto& [h, w] : g_watches) {
        if (!which(w)) continue;
        w.live = false;
        const uint32_t handle = h;
        auto task = [handle] {
            std::lock_guard<std::recursive_mutex> l(g_m);
            const auto it = g_watches.find(handle);
            if (it == g_watches.end()) return;
            detach(it->second);
            g_watches.erase(it);
        };
        (w.server ? server() : client()).post(task);
    }
}

} // namespace

int contactWatchRigid(const std::string& mod, uintptr_t rigid, uintptr_t ent, bool isServer, uint32_t types, gn_contact_fn fn, void* user, uint32_t* handle);

int contactWatch(const std::string& mod, void* entity, uint32_t body, uint32_t types, gn_contact_fn fn, void* user, uint32_t* handle) {
    if (!fn || !handle || !types || (types & ~((1u << GN_CONTACT_IMPULSE) | (1u << GN_CONTACT_ACTIVATION)))) return GN_ERR_ARGUMENT;
    if (unloading()) return GN_ERR_UNAVAILABLE;
    static const reflect::Type* serverType = registry().find("ServerPhysicsEntity");
    static const reflect::Type* clientType = registry().find("ClientPhysicsEntity");
    const uintptr_t ent = reinterpret_cast<uintptr_t>(entity);
    const reflect::Type* t = ent ? registry().typeOf(mem::ProcessMemory(), ent) : nullptr;
    const bool isServer = t && serverType && registry().isKindOf(t, serverType);
    const bool isClient = t && clientType && registry().isKindOf(t, clientType);
    if ((!isServer && !isClient) || !inspector::alive(ent, t)) return GN_ERR_ARGUMENT;
    if (isServer ? !isServerThread() : !isClientThread()) return GN_ERR_THREAD;
    uintptr_t rigid = 0;
    for (const auto& [id, rb] : physicsBodies(ent))
        if (id == body) rigid = rb;
    if (!rigid) return GN_ERR_NOT_FOUND;
    return contactWatchRigid(mod, rigid, ent, isServer, types, fn, user, handle);
}

// A RigidBody's events (entity 0: not followed to its destruction; GridNode's own shapes, which it removes itself).
int contactWatchRigid(const std::string& mod, uintptr_t rigid, uintptr_t ent, bool isServer, uint32_t types, gn_contact_fn fn, void* user, uint32_t* handle) {
    const uintptr_t owner = mem::readOr<uintptr_t>(rigid + kBodyOwner);
    const uintptr_t world = owner ? mem::readOr<uintptr_t>(owner + kOwnerWorld) : 0;
    const int32_t index = mem::readOr<int32_t>(rigid + kBodyIndex, -1);
    if (!world || index < 0 || index > 0xfffff) return GN_ERR_UNAVAILABLE;
    if (!g_destroyHooked) {
        if (onDestroyed("GridNode", "Entity", &watchedDestroyed, nullptr) != GN_OK) return GN_ERR_UNAVAILABLE;
        g_destroyHooked = true;
    }
    std::lock_guard<std::recursive_mutex> lock(g_m);
    if (g_watches.size() >= kMaxWatches) return GN_ERR_FULL;
    Watch w;
    w.mod = mod, w.entity = ent, w.owner = owner, w.world = world, w.index = index, w.types = types, w.fn = fn, w.user = user, w.server = isServer;
    const uint32_t h = g_next++;
    g_objs.push_back(Obj{h});
    w.obj = &g_objs.back();
    const auto get = reinterpret_cast<GetSignalFn>(mem::rva(e::function::hknpWorld_getEventSignal));
    const auto on = reinterpret_cast<AttachFn>(mem::rva(e::function::hkSignal_attach));
    for (uint32_t type : kTypes)
        if (types & (1u << type)) on(get(world, static_cast<uint16_t>(type), index), w.obj, reinterpret_cast<void*>(&contactEvent));
    g_watches.emplace(h, std::move(w));
    *handle = h;
    return GN_OK;
}

int contactUnwatch(uint32_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_m);
    if (!g_watches.count(handle)) return GN_ERR_NOT_FOUND;
    detachLater([handle](const Watch& w) { return w.obj && w.obj->handle == handle; });
    return GN_OK;
}

void forgetContacts(const CodeRange& r) {
    detachLater([&r](const Watch& w) { return r.has(reinterpret_cast<const void*>(w.fn)); });
}

void contactsRelease() {
    detachLater([](const Watch&) { return true; });
}

bool contactsReleased() {
    std::lock_guard<std::recursive_mutex> lock(g_m);
    for (const auto& [h, w] : g_watches)
        if (w.attached) return false;
    return true;
}

// Console: "contact ADDRESS [BODY]": the entity's first (or that) body's contact and activation events, logged.
std::string contactCommand(const std::string& rest) {
    unsigned shapeHandle = 0;
    if (sscanf(rest.c_str(), " shape %u", &shapeHandle) == 1) // a console shape (shape box ... dynamic 50): client world
        return runOnRealmAndWait(false, [shapeHandle]() -> std::string {
            const uintptr_t rigid = shapeRigidBody(shapeHandle);
            if (!rigid) return "contact: no such shape made by the game's factory (shape box ... first)";
            const auto log = [](const gn_contact* c, void*) {
                if (c->type == GN_CONTACT_ACTIVATION) GN_LOG("contact: body %x %s", c->body_a, c->activated ? "activated" : "deactivated");
                else GN_LOG("contact: bodies %x and %x, impulse %.2f", c->body_a, c->body_b, c->impulse);
            };
            uint32_t h = 0;
            const int r = contactWatchRigid("GridNode", rigid, 0, false, (1u << GN_CONTACT_IMPULSE) | (1u << GN_CONTACT_ACTIVATION), log, nullptr, &h);
            return r == GN_OK ? "contact: watching shape " + std::to_string(shapeHandle) + " (watch " + std::to_string(h) + "); events are logged"
                              : "contact: not watched (" + std::to_string(r) + ")";
        });
    unsigned long long addr = 0, body = 0xffffffffull;
    const int n = sscanf(rest.c_str(), " %llx %llx", &addr, &body);
    if (n < 1) return "contact shape N | contact ADDRESS [BODY]: log a physics entity's body's contact impulses and activations (entity_bodies lists its bodies); " +
                      std::to_string(g_dropped.load()) + " events dropped so far";
    void* entity = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), static_cast<uintptr_t>(addr));
    const bool serverRealm = t && t->name.rfind("Client", 0) != 0 && t->name.rfind("PamClient", 0) != 0;
    return runOnRealmAndWait(serverRealm, [entity, body, n]() -> std::string {
        uint32_t id = static_cast<uint32_t>(body);
        if (n < 2) {
            const auto bodies = physicsBodies(reinterpret_cast<uintptr_t>(entity));
            if (bodies.empty()) return "contact: no bodies (not a physics entity?)";
            id = bodies.front().first;
        }
        uint32_t h = 0;
        const auto log = [](const gn_contact* c, void*) {
            if (c->type == GN_CONTACT_ACTIVATION) GN_LOG("contact: body %x %s", c->body_a, c->activated ? "activated" : "deactivated");
            else GN_LOG("contact: bodies %x and %x, impulse %.2f", c->body_a, c->body_b, c->impulse);
        };
        const int r = contactWatch("GridNode", entity, id, (1u << GN_CONTACT_IMPULSE) | (1u << GN_CONTACT_ACTIVATION), log, nullptr, &h);
        char b[128];
        if (r == GN_OK) snprintf(b, sizeof(b), "contact: watching body %x (watch %u); events are logged", id, h);
        else snprintf(b, sizeof(b), "contact: not watched (%d)", r);
        return b;
    });
}

} // namespace gn::host
