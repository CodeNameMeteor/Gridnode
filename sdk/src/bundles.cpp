// Loading the game's bundles at runtime and spawning prefabs from them (gn_api 38, experimental).
// Worked out with the claim lab (.workflow/runs/bundle-load, prefab-spawn) and tested there in game on
// 2026-11-12; docs/research/zone-streaming.md has the details.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
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

struct Bundle {
    uint32_t handle = 0;
    std::string name;
    uintptr_t owner = 0;  // the mod's code (a return address in it), or 0 for GridNode
    uint64_t level = 0;   // g_level when made: a level ending frees it in the game
    uint64_t game[2] = {0, 0};  // the game's handle: group pointer, compartment (server thread)
    enum State { Queued, Loading, Ready, Failed } state = Queued;
};

struct Prefab {
    uint32_t handle = 0;
    uint32_t bundle = 0;  // 0: not tied to one of ours
    uintptr_t owner = 0;
    uint64_t level = 0;
    std::vector<uintptr_t> server, client;  // the entities it made, in each realm
    enum State { Queued, Live, Failed, Destroyed } state = Queued;
};

// A released bundle's game handle, given back only after a delay: its prefabs' entities are destroyed by
// their owner later in the frame, and must be gone before the bundle's data is (a prefab's entities read
// their data every frame). A bundle_load of the same name meanwhile takes the handle back instead of a
// new request: a load just after a release found the group half unloaded and spawned from it, and the
// game crashed in an entity transform update (2026-11-12).
struct PendingRelease {
    std::string name;
    uint64_t game[2] = {0, 0};
    uint64_t level = 0;
    std::chrono::steady_clock::time_point at;
};
constexpr auto kReleaseDelay = std::chrono::milliseconds(1500);

std::mutex g_m;
std::vector<PendingRelease> g_pending; // (g_m)
std::vector<Bundle> g_bundles;  // (g_m)
std::vector<Prefab> g_prefabs;  // (g_m)
uint32_t g_next = 1;            // (g_m) handles for both
std::atomic<uint64_t> g_level{1};
// The game keeps the name pointer it was asked with: names are never freed (one per bundle_load).
std::deque<std::string> g_names;  // (g_m)
std::once_flag g_once;

Bundle* bundleOf(uint32_t h) {
    for (Bundle& b : g_bundles)
        if (b.handle == h) return &b;
    return nullptr;
}
Prefab* prefabOf(uint32_t h) {
    for (Prefab& p : g_prefabs)
        if (p.handle == h) return &p;
    return nullptr;
}

bool sameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

bool plausible(uintptr_t p) {
    uint64_t v = 0;
    return p > 0x10000 && (p & 7) == 0 && p < 0x7ff000000000ull && mem::read(p, v);
}

// A bus: +0x00 owner (owner +0x38: the level), +0x08 parent bus, +0x28 first child, +0x30 refcount,
// +0x38 data (FUN_14313ffe0). A bus is one when its owner's vtable is in the game.
bool isBus(uintptr_t bus) {
    if (!plausible(bus)) return false;
    const uintptr_t owner = mem::readOr<uintptr_t>(bus);
    return plausible(owner) && mem::inImage(mem::readOr<uintptr_t>(owner));
}

// The level's root bus, from Faith's: a spatial entity keeps its bus at +0x28 (+0x20 holds other data
// there: 0x737 on the server Faith, which crashed the spawn), then up the parents (+0x08). The root bus
// is where the claim lab's spawns survived teleports.
uintptr_t rootBus(void* faith, const char* realm) {
    uintptr_t bus = faith ? mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(faith) + 0x28) : 0;
    if (!isBus(bus)) {
        GN_LOG("bundles: %s: Faith %p has no bus at +0x28 (%llx)", realm, faith, static_cast<unsigned long long>(bus));
        return 0;
    }
    // The chain is logged: which bus is the top decides what the prefab lives under (a teleport that
    // streamed part of the city out crashed a prefab spawned under Faith's top bus, 2026-11-12).
    std::string chain;
    for (int depth = 0; depth < 64; ++depth) {
        const uintptr_t data = mem::readOr<uintptr_t>(bus + 0x38);
        const reflect::Type* t = data ? registry().typeOf(mem::ProcessMemory(), data) : nullptr;
        char b[160];
        std::snprintf(b, sizeof(b), " %llx(%s, refs %d)", static_cast<unsigned long long>(bus), t ? t->name.c_str() : "?",
                      mem::readOr<int32_t>(bus + 0x30));
        chain += b;
        const uintptr_t parent = mem::readOr<uintptr_t>(bus + 0x08);
        if (!isBus(parent) || parent == bus) {
            GN_LOG("bundles: %s bus chain from Faith:%s; top's parent %llx", realm, chain.c_str(), static_cast<unsigned long long>(parent));
            break;
        }
        bus = parent;
    }
    return bus;
}

// The list a blueprint spawn fills (creation info +0xe8 points at it): +0x08 = first chunk; a chunk has
// flags at +0 (bit 0: the last one), entries from +0x10; a full chunk ends at +0x1f0 with the next chunk
// at +0x08; the last one holds its count (u32) at +0x08 (serverSpawned_initAll). Its chunks are the
// game's (freed with EngineAllocator_free and the list as allocator by the game's own spawner); GridNode
// leaves them, 0x1f0 bytes a spawn.
struct EntityList {
    uint64_t allocator = 0, first = 0, end = 0;
    uint32_t count = 0, pad = 0;
};

std::vector<uintptr_t> entitiesOf(const EntityList& list) {
    std::vector<uintptr_t> out;
    uintptr_t chunk = list.first;
    for (int guard = 0; chunk && guard < 4096; ++guard) {
        const bool last = (mem::readOr<uint8_t>(chunk) & 1) != 0;
        const uintptr_t end = last ? chunk + (static_cast<uintptr_t>(mem::readOr<uint32_t>(chunk + 8)) + 2) * 8 : chunk + 0x1f0;
        for (uintptr_t p = chunk + 0x10; p < end; p += 8)
            if (const uintptr_t ent = mem::readOr<uintptr_t>(p)) out.push_back(ent);
        if (last) break;
        chunk = mem::readOr<uintptr_t>(chunk + 8);
    }
    return out;
}

// --- the game's side, each on its realm's thread ----------------------------------------------------

void destroyEntities(const std::vector<uintptr_t>& list, uint64_t level);

using RequestFn = void* (*)(void* manager, uint64_t* handle, void* request);
using RequestInitFn = void* (*)(void* request, const char* name, uint16_t compartment, int64_t a4);
using ReleaseFn = int32_t (*)(uint64_t* handle, int32_t a2);
using CiCtorFn = void (*)(void* ci, void* data, void* bus, const gn_transform* t);
using ServerSpawnFn = void* (*)(void* result, void* ci, void* blueprint, void* ref, uint64_t bus);
using ClientSpawnFn = void* (*)(void* result, void* ci, void* blueprint, uint64_t ref, uint64_t bus);

// Server thread. As a district streaming in asks (0x143363c00): BundleRequest_init with parent
// compartment 0, then +0x2a = 0xfe, +0x24 = 0, +0x28 = 1. The request is 0x50 bytes: +0x2c the heap
// info (left at its default), +0x40 an empty string (nothing to free).
void requestOnServer(uint32_t h) {
    const char* name = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_m);
        Bundle* b = bundleOf(h);
        if (!b || b->state != Bundle::Queued) return;
        for (auto it = g_pending.begin(); it != g_pending.end(); ++it)
            if (it->level == b->level && sameName(it->name, b->name)) {
                b->game[0] = it->game[0], b->game[1] = it->game[1];
                b->state = Bundle::Loading; // pollOnServer sees it ready at once
                g_pending.erase(it);
                GN_LOG("bundles: load %s: taken back from a release not yet given to the game", b->name.c_str());
                return;
            }
        g_names.push_back(b->name);
        name = g_names.back().c_str();
    }
    alignas(16) uint8_t req[0x60] = {};
    reinterpret_cast<RequestInitFn>(mem::rva(e::function::BundleRequest_init))(req, name, 0, 0);
    req[0x2a] = 0xfe;
    std::memset(req + 0x24, 0, 4);
    req[0x28] = 1;
    uint64_t game[2] = {0, 0};
    reinterpret_cast<RequestFn>(mem::rva(e::function::SubLevelManager_requestBundle))(reinterpret_cast<void*>(mem::rva(e::global::ServerSubLevelManager)),
                                                                                    game, req);
    std::lock_guard<std::mutex> lock(g_m);
    Bundle* b = bundleOf(h);
    if (!b) { // released while asked: give it straight back
        if (game[0]) reinterpret_cast<ReleaseFn>(mem::rva(e::function::BundleHandle_release))(game, 0);
        return;
    }
    b->game[0] = game[0], b->game[1] = game[1];
    b->state = game[0] ? Bundle::Loading : Bundle::Failed;
    GN_LOG("bundles: load %s: %s", b->name.c_str(), game[0] ? "asked" : "refused by the game");
}

// Server thread, each tick: a group is ready at state 3 (or 2 with bit 0 of +0x6c clear); >= 4 is
// not usable (BundleHandle_isReady).
void releaseOnServer(uint64_t g0, uint64_t g1, uint64_t level);

void pollOnServer() {
    std::vector<PendingRelease> due;
    {
        std::lock_guard<std::mutex> lock(g_m);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = g_pending.begin(); it != g_pending.end();)
            if (now >= it->at) due.push_back(*it), it = g_pending.erase(it);
            else ++it;
    }
    for (const PendingRelease& r : due) releaseOnServer(r.game[0], r.game[1], r.level);
    std::lock_guard<std::mutex> lock(g_m);
    for (Bundle& b : g_bundles) {
        if (b.state != Bundle::Loading) continue;
        const uintptr_t group = static_cast<uintptr_t>(b.game[0]);
        const int32_t state = mem::readOr<int32_t>(group + 0x08, -1);
        const bool ready = state == 3 || (state == 2 && (mem::readOr<uint8_t>(group + 0x6c) & 1) == 0);
        if (ready) {
            b.state = Bundle::Ready;
            GN_LOG("bundles: %s ready", b.name.c_str());
        } else if (state < 0 || state >= 4) {
            b.state = Bundle::Failed;
            GN_LOG("bundles: %s failed (group state %d)", b.name.c_str(), state);
        }
    }
}

// Server thread. The last reference unloads the bundle (the game held one more while loading, and
// drops it about 2 s after it is ready).
void releaseOnServer(uint64_t g0, uint64_t g1, uint64_t level) {
    if (!g0 || level != g_level.load()) return; // a level since: the game freed the group with it
    uint64_t game[2] = {g0, g1};
    reinterpret_cast<ReleaseFn>(mem::rva(e::function::BundleHandle_release))(game, 0);
}

// The blueprint of an asset in any loaded resource group (asset_find's search), without a reference:
// the bundle it came from keeps it while the prefab lives.
void* blueprintOf(const char* asset) {
    void* found = assetFind(asset, nullptr);
    if (!found) return nullptr;
    assetRelease(found);
    return found;
}

// One realm's half (as the engine's own blueprint spawner, 0x1438fce60 / 0x1438fcbe0): creation info
// with an entity list at +0xe8, kind 2 at +0x28 and the game context's value at +0x108; spawn; release
// the bus reference it returns; Entity_init every listed entity. A null reference object works.
std::vector<uintptr_t> spawnHalf(bool server, void* blueprint, uintptr_t bus, const gn_transform& at) {
    alignas(16) uint8_t ci[0x200] = {};
    alignas(16) gn_transform t = at;
    t.right[3] = t.up[3] = t.forward[3] = 0.f, t.trans[3] = 1.f;
    reinterpret_cast<CiCtorFn>(mem::rva(server ? e::function::ServerEntityCreationInfo_ctor : e::function::ClientEntityCreationInfo_ctor))(
        ci, nullptr, reinterpret_cast<void*>(bus), &t);
    const uintptr_t ctx = mem::readOr<uintptr_t>(mem::rva(server ? e::global::ServerGameContext : e::global::ClientGameContext));
    const uint64_t v = ctx ? mem::readOr<uint64_t>(ctx + (server ? 0x80 : 0x88)) : 0;
    std::memcpy(ci + 0x108, &v, 8);
    const int32_t kind = 2;
    std::memcpy(ci + 0x28, &kind, 4);
    EntityList list;
    const EntityList* lp = &list;
    std::memcpy(ci + 0xe8, &lp, 8);
    uint64_t result[3] = {0, 0, 0};
    if (server) {
        reinterpret_cast<ServerSpawnFn>(mem::rva(e::function::Blueprint_instantiate))(result, ci, blueprint, nullptr, 0);
        if (result[1]) reinterpret_cast<void (*)(uint64_t)>(mem::rva(e::function::EntityBus_release))(result[1]);
        reinterpret_cast<void (*)(EntityList*, uint64_t)>(mem::rva(e::function::serverSpawned_initAll))(&list, 0);
    } else {
        reinterpret_cast<ClientSpawnFn>(mem::rva(e::function::ClientEntity_spawnBlueprint))(result, ci, blueprint, 0, 0);
        if (result[2]) reinterpret_cast<void (*)(uint64_t)>(mem::rva(e::function::EntityBus_release))(result[2]);
        reinterpret_cast<void (*)(EntityList*)>(mem::rva(e::function::clientSpawned_initAll))(&list);
    }
    return entitiesOf(list);
}

void spawnClientHalf(uint32_t h, void* blueprint, gn_transform at, uint64_t level) {
    if (level != g_level.load()) return;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!prefabOf(h)) return; // destroyed before its client half: none is made
    }
    const uintptr_t bus = rootBus(localClientCharacter(), "client");
    std::vector<uintptr_t> made;
    if (bus) made = spawnHalf(false, blueprint, bus, at);
    std::lock_guard<std::mutex> lock(g_m);
    Prefab* p = prefabOf(h);
    if (!p) { // destroyed while this half was made
        client().post([made, level] { destroyEntities(made, level); });
        return;
    }
    p->client = made;
    p->state = bus ? Prefab::Live : Prefab::Failed;
    GN_LOG("bundles: prefab %u: client %s, %u entities", h, bus ? "spawned" : "skipped (no bus from Faith on the client)", static_cast<unsigned>(made.size()));
}

void spawnOnServer(uint32_t h, std::string asset, gn_transform at) {
    uint64_t level = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        Prefab* p = prefabOf(h);
        if (!p || p->state != Prefab::Queued) return;
        level = p->level;
    }
    if (level != g_level.load()) return;
    void* blueprint = blueprintOf(asset.c_str());
    const uintptr_t bus = blueprint ? rootBus(localServerCharacter(), "server") : 0;
    std::vector<uintptr_t> made;
    if (bus) made = spawnHalf(true, blueprint, bus, at);
    {
        std::lock_guard<std::mutex> lock(g_m);
        Prefab* p = prefabOf(h);
        if (!p) { // destroyed while this half was made
            server().post([made, level] { destroyEntities(made, level); });
            return;
        }
        p->server = made;
        if (!bus) {
            p->state = Prefab::Failed;
            GN_LOG("bundles: prefab %u: %s not spawned: %s", h, asset.c_str(), blueprint ? "no bus from Faith on the server" : "no loaded bundle has it");
            return;
        }
    }
    GN_LOG("bundles: prefab %u: %s, server %u entities", h, asset.c_str(), static_cast<unsigned>(made.size()));
    client().post([h, blueprint, at, level] { spawnClientHalf(h, blueprint, at, level); });
}

// The entity's own realm thread. Entity_requestDestroy(entity, ownerRef) reads [ownerRef] = the owner and
// jumps to its vtable +0x10 with (owner, entity); the owner sets entity +0x18 bit 10 and queues it. The
// entity's bus holds the owner at +0x00, so ownerRef = the bus: at +0x20 on logic entities, +0x28 on
// spatial ones (reading the other crashed in the lab).
void destroyEntities(const std::vector<uintptr_t>& list, uint64_t level) {
    if (level != g_level.load()) return;
    for (const uintptr_t ent : list) {
        const reflect::Type* type = registry().typeOf(mem::ProcessMemory(), ent);
        if (!type || !inspector::alive(ent, type)) continue; // gone already (GridNode's destructor hook saw it)
        if (mem::readOr<uint32_t>(ent + 0x18) & 0x400) continue;     // already going
        for (const uint32_t off : {0x20u, 0x28u}) {
            const uintptr_t bus = mem::readOr<uintptr_t>(ent + off);
            if (!isBus(bus)) continue;
            const uintptr_t vt = mem::readOr<uintptr_t>(mem::readOr<uintptr_t>(bus));
            if (!mem::inImage(mem::readOr<uintptr_t>(vt + 0x10))) continue;
            reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(mem::rva(e::function::Entity_requestDestroy))(ent, bus);
            break;
        }
    }
}

// (g_m) Takes a prefab out and queues its entities' destruction.
void destroyLocked(std::vector<Prefab>::iterator it) {
    if (!it->server.empty() || !it->client.empty()) {
        const uint64_t level = it->level;
        std::vector<uintptr_t> s = it->server, c = it->client;
        server().post([s, level] { destroyEntities(s, level); });
        client().post([c, level] { destroyEntities(c, level); });
    }
    g_prefabs.erase(it);
}

// (g_m) Takes a bundle out: its prefabs first, then the game's handle.
void releaseLocked(std::vector<Bundle>::iterator it) {
    for (auto p = g_prefabs.begin(); p != g_prefabs.end();)
        if (p->bundle == it->handle) destroyLocked(p), p = g_prefabs.begin();
        else ++p;
    if (it->game[0]) {
        PendingRelease r;
        r.name = it->name;
        r.game[0] = it->game[0], r.game[1] = it->game[1];
        r.level = it->level;
        r.at = std::chrono::steady_clock::now() + kReleaseDelay;
        g_pending.push_back(r);
    }
    g_bundles.erase(it);
}

void levelChanged(const char* level, int loaded, void*) {
    if (loaded) return;
    g_level.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_m);
    if (!g_bundles.empty() || !g_prefabs.empty())
        GN_LOG("bundles: %s ended: %u bundles and %u prefabs went with it (handles dropped, not released)", level ? level : "the level",
               static_cast<unsigned>(g_bundles.size()), static_cast<unsigned>(g_prefabs.size()));
    g_bundles.clear();
    g_prefabs.clear();
    g_pending.clear(); // the game unloaded them with the level
}

void start() {
    std::call_once(g_once, [] {
        onLevel("GridNode", &levelChanged, nullptr);
        server().addListener("GridNode bundles", [](float) { pollOnServer(); });
    });
}

bool finite(const gn_transform& t) {
    for (int k = 0; k < 4; ++k)
        if (!std::isfinite(t.right[k]) || !std::isfinite(t.up[k]) || !std::isfinite(t.forward[k]) || !std::isfinite(t.trans[k])) return false;
    return std::fabs(t.trans[0]) < 1e6f && std::fabs(t.trans[1]) < 1e6f && std::fabs(t.trans[2]) < 1e6f;
}

} // namespace

int bundleLoad(const void* caller, const char* name, uint32_t* handle) {
    if (!name || !*name || std::strlen(name) > 512 || !handle) return GN_ERR_ARGUMENT;
    start();
    std::lock_guard<std::mutex> lock(g_m);
    if (g_bundles.size() >= 64) return GN_ERR_FULL;
    Bundle b;
    b.handle = g_next++;
    b.name = name;
    b.owner = reinterpret_cast<uintptr_t>(caller);
    b.level = g_level.load();
    *handle = b.handle;
    const uint32_t h = b.handle;
    g_bundles.push_back(std::move(b));
    server().post([h] { requestOnServer(h); });
    return GN_OK;
}

int bundleState(uint32_t handle, uint32_t* ready) {
    std::lock_guard<std::mutex> lock(g_m);
    const Bundle* b = bundleOf(handle);
    if (!b) return GN_ERR_NOT_FOUND;
    if (b->state == Bundle::Failed) return GN_ERR_UNAVAILABLE;
    if (ready) *ready = b->state == Bundle::Ready ? 1u : 0u;
    return GN_OK;
}

int bundleRelease(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto it = g_bundles.begin(); it != g_bundles.end(); ++it)
        if (it->handle == handle) {
            releaseLocked(it);
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

int prefabSpawn(const void* caller, const char* asset, const gn_transform* at, uint32_t bundle, uint32_t* handle) {
    if (!asset || !*asset || !at || !handle || !finite(*at)) return GN_ERR_ARGUMENT;
    start();
    std::lock_guard<std::mutex> lock(g_m);
    if (bundle) {
        const Bundle* b = bundleOf(bundle);
        if (!b) return GN_ERR_NOT_FOUND;
        if (b->state != Bundle::Ready) return GN_ERR_UNAVAILABLE;
    }
    if (g_prefabs.size() >= 256) return GN_ERR_FULL;
    Prefab p;
    p.handle = g_next++;
    p.bundle = bundle;
    p.owner = reinterpret_cast<uintptr_t>(caller);
    p.level = g_level.load();
    *handle = p.handle;
    const uint32_t h = p.handle;
    g_prefabs.push_back(std::move(p));
    const std::string name = asset;
    const gn_transform t = *at;
    server().post([h, name, t] { spawnOnServer(h, name, t); });
    return GN_OK;
}

int prefabState(uint32_t handle, uint32_t* live, uint32_t* entities) {
    std::lock_guard<std::mutex> lock(g_m);
    const Prefab* p = prefabOf(handle);
    if (!p) return GN_ERR_NOT_FOUND;
    if (p->state == Prefab::Failed) return GN_ERR_UNAVAILABLE;
    if (live) *live = p->state == Prefab::Live ? 1u : 0u;
    if (entities) *entities = static_cast<uint32_t>(p->server.size() + p->client.size());
    return GN_OK;
}

int prefabDestroy(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto it = g_prefabs.begin(); it != g_prefabs.end(); ++it)
        if (it->handle == handle) {
            destroyLocked(it);
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

void forgetBundles(const CodeRange& r) {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto it = g_prefabs.begin(); it != g_prefabs.end();)
        if (r.has(reinterpret_cast<void*>(it->owner))) destroyLocked(it), it = g_prefabs.begin();
        else ++it;
    for (auto it = g_bundles.begin(); it != g_bundles.end();)
        if (r.has(reinterpret_cast<void*>(it->owner))) releaseLocked(it), it = g_bundles.begin();
        else ++it;
}

} // namespace gn::host
