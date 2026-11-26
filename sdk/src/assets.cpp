// Assets by name (db/annotations/resources.yaml), as the engine's own code finds them: a partition
// (an EBX asset, e.g. a blueprint) by its path in a resource group, then its primary instance. Every
// loaded resource group is searched, static first. Nothing is loaded: an asset whose bundle is not in
// memory is not found.

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <utility>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

// getCompartment and Partition_findByName are game-tested since 2026-11-12; DataContainer_release and
// ResourceGroup_Count checked in Ghidra (runtime-data, registry-hardening).

namespace gn::host {
namespace {
namespace e = gn::engine;
namespace k = gn::engine::constant;

bool available() {
    static const bool ok = [] {
        const bool fine = build::codeMatches("getCompartment") && build::codeMatches("Partition_findByName");
        if (!fine) GN_LOG("assets: the lookup functions are not as expected; assets are off");
        return fine;
    }();
    return ok;
}

using CompartmentFn = void* (*)(uint16_t id);
using FindFn = void* (*)(void* compartment, const char* path);

// Assets handed to mods and not yet released, with how many times (each find adds a reference):
// assetRelease only releases what assetFind gave out.
std::mutex g_heldM;
std::unordered_map<uintptr_t, uint32_t> g_held; // (g_heldM)

bool isDataContainer(uintptr_t p) {
    static const reflect::Type* dc = registry().find("DataContainer");
    const reflect::Type* t = p ? registry().typeOf(mem::ProcessMemory(), p) : nullptr;
    return t && dc && registry().isKindOf(t, dc);
}
} // namespace

void* assetFind(const char* path, uint32_t* groupOut) {
    if (!path || !*path || !available()) return nullptr;
    const auto compartment = reinterpret_cast<CompartmentFn>(mem::rva(e::function::getCompartment));
    const auto find = reinterpret_cast<FindFn>(mem::rva(e::function::Partition_findByName));
    const uintptr_t table = mem::rva(e::global::ResourceGroups);
    // The resource manager's lock (+0x26300, a CRITICAL_SECTION) while the groups are walked: a bundle loading or
    // going on the streaming or server thread adds or frees a group under it (ResourceManager_registerBundleGroup,
    // its unregister), and a group freed mid-walk is read after it is gone (the 2026-11-13 audit).
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(mem::rva(e::global::ResourceManager) + 0x26300);
    struct Held {
        CRITICAL_SECTION* cs;
        ~Held() { LeaveCriticalSection(cs); }
    };
    if (!lock->DebugInfo && !lock->SpinCount) return nullptr; // not made yet (the game is still starting): nothing is loaded
    EnterCriticalSection(lock);
    const Held held{lock};
    for (uint32_t id = 0; id < k::ResourceGroup_Count; ++id) {
        // Only groups that exist; the game's own getter gives the group's compartment (its +0x68).
        if (!mem::readOr<uintptr_t>(table + id * 8ull)) continue;
        void* comp = compartment(static_cast<uint16_t>(id));
        if (!comp) continue;
        void* partition = find(comp, path);
        if (!partition) continue;
        // Partition_primaryInstance: **(partition + 0x20).
        const uintptr_t list = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(partition) + 0x20);
        const uintptr_t asset = list ? mem::readOr<uintptr_t>(list) : 0;
        if (!asset || !isDataContainer(asset)) continue; // a reflected data object, or nothing is touched
        // Keep it alive while the mod holds it (the engine's own addRef: a locked increment of +8).
        _InterlockedIncrement(reinterpret_cast<volatile long*>(asset + 8));
        {
            std::lock_guard<std::mutex> lock(g_heldM);
            ++g_held[asset];
        }
        if (groupOut) *groupOut = id;
        return reinterpret_cast<void*>(asset);
    }
    return nullptr;
}

// An asset by its 16-byte GUID (API 41): every partition of every loaded group's compartment is looked at, under the
// resource manager's lock (as assetFind) and the compartment's own (+0xc0, as Partition_findByName). The compartment's
// partitions are a hash map at +0x158 (buckets +0x160, count +0x168; node +0x08 the partition, +0x10 the next: read in
// Ghidra 2026-11-14, 0x142aeb940); a partition has its GUID at +0x08 and its instances at +0x20 (count u16 +0x28), an
// instance its own GUID 16 bytes before it when its flags (+0x0a) have 0x100 (Partition_findByGuid). Either GUID
// matches: the partition's gives its primary instance. The game's Compartment_findByGuid did not find Faith's
// blueprint by its instance GUID (game test 2026-11-14), so it is not used.
void* assetFindGuid(const uint8_t* guid, uint32_t* groupOut) {
    if (!guid || !available()) return nullptr;
    const auto compartment = reinterpret_cast<CompartmentFn>(mem::rva(e::function::getCompartment));
    const uintptr_t table = mem::rva(e::global::ResourceGroups);
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(mem::rva(e::global::ResourceManager) + 0x26300);
    struct Held {
        CRITICAL_SECTION* cs;
        ~Held() { LeaveCriticalSection(cs); }
    };
    if (!lock->DebugInfo && !lock->SpinCount) return nullptr;
    EnterCriticalSection(lock);
    const Held held{lock};
    auto same = [guid](uintptr_t at) {
        uint8_t g[16];
        return mem::read(at, g, 16) && memcmp(g, guid, 16) == 0;
    };
    for (uint32_t id = 0; id < k::ResourceGroup_Count; ++id) {
        if (!mem::readOr<uintptr_t>(table + id * 8ull)) continue;
        const uintptr_t comp = reinterpret_cast<uintptr_t>(compartment(static_cast<uint16_t>(id)));
        if (!comp) continue;
        auto* clock = reinterpret_cast<CRITICAL_SECTION*>(comp + 0xc0);
        EnterCriticalSection(clock);
        const Held heldComp{clock};
        const uintptr_t buckets = mem::readOr<uintptr_t>(comp + 0x160);
        const uint32_t count = mem::readOr<uint32_t>(comp + 0x168);
        if (!buckets || count > (1u << 22)) continue;
        uintptr_t asset = 0;
        for (uint32_t b = 0; b < count && !asset; ++b)
            for (uintptr_t node = mem::readOr<uintptr_t>(buckets + b * 8ull), steps = 0; node && steps < 100000 && !asset;
                 node = mem::readOr<uintptr_t>(node + 0x10), ++steps) {
                const uintptr_t part = mem::readOr<uintptr_t>(node + 8);
                if (!part) continue;
                const uintptr_t inst = mem::readOr<uintptr_t>(part + 0x20);
                const uint16_t n = mem::readOr<uint16_t>(part + 0x28);
                if (!inst) continue;
                if (same(part + 8)) {
                    asset = mem::readOr<uintptr_t>(inst);
                    break;
                }
                for (uint16_t i = 0; i < n; ++i) {
                    const uintptr_t obj = mem::readOr<uintptr_t>(inst + i * 8ull);
                    if (obj && (mem::readOr<uint16_t>(obj + 0x0a) & 0x100) && same(obj - 0x10)) {
                        asset = obj;
                        break;
                    }
                }
            }
        if (!asset || !isDataContainer(asset)) continue;
        _InterlockedIncrement(reinterpret_cast<volatile long*>(asset + 8));
        {
            std::lock_guard<std::mutex> l(g_heldM);
            ++g_held[asset];
        }
        if (groupOut) *groupOut = id;
        return reinterpret_cast<void*>(asset);
    }
    return nullptr;
}

// The resource group whose compartment holds obj as one of its partitions' instances (-1 if none): the same walk as
// assetFindGuid, by pointer. Effects use it to stop theirs before that group (the bundle) goes.
int assetGroupOf(uintptr_t obj) {
    if (!obj || !available()) return -1;
    const auto compartment = reinterpret_cast<CompartmentFn>(mem::rva(e::function::getCompartment));
    const uintptr_t table = mem::rva(e::global::ResourceGroups);
    auto* lock = reinterpret_cast<CRITICAL_SECTION*>(mem::rva(e::global::ResourceManager) + 0x26300);
    struct Held {
        CRITICAL_SECTION* cs;
        ~Held() { LeaveCriticalSection(cs); }
    };
    if (!lock->DebugInfo && !lock->SpinCount) return -1;
    EnterCriticalSection(lock);
    const Held held{lock};
    for (uint32_t id = 0; id < k::ResourceGroup_Count; ++id) {
        if (!mem::readOr<uintptr_t>(table + id * 8ull)) continue;
        const uintptr_t comp = reinterpret_cast<uintptr_t>(compartment(static_cast<uint16_t>(id)));
        if (!comp) continue;
        auto* clock = reinterpret_cast<CRITICAL_SECTION*>(comp + 0xc0);
        EnterCriticalSection(clock);
        const Held heldComp{clock};
        const uintptr_t buckets = mem::readOr<uintptr_t>(comp + 0x160);
        const uint32_t count = mem::readOr<uint32_t>(comp + 0x168);
        if (!buckets || count > (1u << 22)) continue;
        for (uint32_t b = 0; b < count; ++b)
            for (uintptr_t node = mem::readOr<uintptr_t>(buckets + b * 8ull), steps = 0; node && steps < 100000; node = mem::readOr<uintptr_t>(node + 0x10), ++steps) {
                const uintptr_t part = mem::readOr<uintptr_t>(node + 8);
                const uintptr_t inst = part ? mem::readOr<uintptr_t>(part + 0x20) : 0;
                const uint16_t n = part ? mem::readOr<uint16_t>(part + 0x28) : 0;
                for (uint16_t i = 0; inst && i < n; ++i)
                    if (mem::readOr<uintptr_t>(inst + i * 8ull) == obj) return static_cast<int>(id);
            }
    }
    return -1;
}

// "assetguid GUID": 32 hex digits are the bytes in memory order; the dashed form (8-4-4-4-12) is read as
// Frosty and .NET print a GUID (the first three groups little-endian).
std::string assetGuidCommand(const std::string& rest) {
    std::string hex;
    bool dashed = false;
    for (char c : rest) {
        if (c == '-') dashed = true;
        else if (isxdigit(static_cast<unsigned char>(c))) hex += c;
    }
    if (hex.size() != 32) return "assetguid GUID (32 hex digits, or 8-4-4-4-12 as Frosty shows it)";
    uint8_t g[16];
    for (int i = 0; i < 16; ++i) g[i] = static_cast<uint8_t>(strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
    if (dashed) {
        std::swap(g[0], g[3]), std::swap(g[1], g[2]); // Data1
        std::swap(g[4], g[5]);                         // Data2
        std::swap(g[6], g[7]);                         // Data3
    }
    uint32_t group = 0;
    void* a = assetFindGuid(g, &group);
    if (!a) return "assetguid: not in any loaded resource group";
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(a));
    char b[160];
    snprintf(b, sizeof(b), "assetguid: %p (%s) in group %u", a, t ? t->name.c_str() : "?", group);
    assetRelease(a);
    return b;
}

void assetRelease(void* asset) {
    if (!asset) return;
    {
        std::lock_guard<std::mutex> lock(g_heldM);
        const auto it = g_held.find(reinterpret_cast<uintptr_t>(asset));
        if (it == g_held.end()) { // not from assetFind, or already released: refusing beats a double free
            GN_LOG("assets: release of %p refused: not an asset GridNode handed out (or released already)", asset);
            return;
        }
        if (--it->second == 0) g_held.erase(it);
    }
    using ReleaseFn = uint32_t (*)(void*);
    reinterpret_cast<ReleaseFn>(mem::rva(e::function::DataContainer_release))(asset);
}

} // namespace gn::host
