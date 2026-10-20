// Assets by name (db/annotations/resources.yaml), as the engine's own code finds them: a partition
// (an EBX asset, e.g. a blueprint) by its path in a resource group, then its primary instance. Every
// loaded resource group is searched, static first. Nothing is loaded: an asset whose bundle is not in
// memory is not found.

#include <windows.h>

#include <cstdint>
#include <mutex>
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
} // namespace

void* assetFind(const char* path, uint32_t* groupOut) {
    if (!path || !*path || !available()) return nullptr;
    const auto compartment = reinterpret_cast<CompartmentFn>(mem::rva(e::function::getCompartment));
    const auto find = reinterpret_cast<FindFn>(mem::rva(e::function::Partition_findByName));
    const uintptr_t table = mem::rva(e::global::ResourceGroups);
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
        if (!asset) continue;
        // Keep it alive while the mod holds it (the engine's own addRef: a locked increment of +8).
        _InterlockedIncrement(reinterpret_cast<volatile long*>(asset + 8));
        if (groupOut) *groupOut = id;
        return reinterpret_cast<void*>(asset);
    }
    return nullptr;
}

void assetRelease(void* asset) {
    if (!asset) return;
    using ReleaseFn = uint32_t (*)(void*);
    reinterpret_cast<ReleaseFn>(mem::rva(e::function::DataContainer_release))(asset);
}

} // namespace gn::host
