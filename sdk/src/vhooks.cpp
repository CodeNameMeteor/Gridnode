// Hooking a class's virtual by name (gn_api hook_virtual, API 36): the slot in every vtable of the
// class, and of its subclasses when asked, through the hook table like hook_slot. The registry found
// ~4,600 vtables at startup and the API exposed none; a mod could hook only the 41 classes
// engine.hpp names. The bookkeeping (one original per vtable) is gridnode/vhook_set.hpp.

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/api.h"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/vhook_set.hpp"
#include "host.hpp"

namespace gn::host {
namespace {
using reflect::Type;

constexpr uint32_t kMaxOffset = 0x2000; // 1,024 slots: past any vtable here (docs/research/vtables.md)

// Handle - 1 is the index. Sets are never freed: the hook table may still point at their originals,
// and a detour may be in hook_original while its mod is being unloaded. hook_original runs on every
// call of a hooked virtual, so it reads g_sets without a lock.
constexpr size_t kMaxSets = 4096;
std::mutex g_m;                                    // adding and removing
std::array<std::atomic<vhook::Set*>, kMaxSets> g_sets{}; // (written under g_m, read anywhere)
std::atomic<size_t> g_count{0};
std::vector<std::unique_ptr<vhook::Set>> g_owned; // (g_m)
std::vector<std::string> g_classOf;               // (g_m) by handle - 1: the class asked for, for the Hooks list

// The type a vtable's objects have: an "object" whose vtable is vt, asked the way objects are.
const Type* typeOfVtable(uintptr_t vt) {
    uintptr_t fake = vt;
    return registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(&fake));
}

// The runtime vtables of `type` (and its subclasses), each checked by its slot 0: a vtable the walker
// found only as a candidate may belong to a sibling sharing getType. A vtable whose slot at `offset`
// holds a getType accessor is left out: the offset is past its end, in the next vtable, and hooking
// that class's slot 0 would make its objects unknown.
std::vector<uint64_t> vtablesOf(const Type* type, bool subclasses, uint32_t offset, size_t* pastEnd) {
    std::vector<uint64_t> out;
    for (const Type& t : registry().data().types) {
        if (t.category != reflect::Category::Class) continue;
        if (subclasses ? !registry().isKindOf(&t, type) : &t != type) continue;
        for (uint64_t va : t.vtables) {
            const uintptr_t vt = mem::rva(va);
            const Type* got = typeOfVtable(vt);
            if (!got || (subclasses ? !registry().isKindOf(got, type) : got != type)) continue;
            if (typeOfVtable(vt + offset)) {
                ++*pastEnd;
                continue;
            }
            out.push_back(vt);
        }
    }
    return out;
}
} // namespace

int typeVtableCount(const Type* type) { return type ? static_cast<int>(type->vtables.size()) : 0; }

uint64_t typeVtableAt(const Type* type, uint32_t index) { return type && index < type->vtables.size() ? type->vtables[index] : 0; }

int typeVirtualFind(const Type* type, const char* name, uint32_t* outOffset) {
    if (!type || !name || !outOffset) return GN_ERR_ARGUMENT;
    // The annotated virtuals (db/annotations, engine.hpp meta::classVirtuals), the class's own first,
    // then each base class's: a virtual keeps its slot in every subclass.
    for (const Type* t = type; t; t = registry().super(t)) {
        const auto& table = engine::meta::classVirtuals;
        const auto* end = table + sizeof(table) / sizeof(table[0]);
        const auto* it = std::lower_bound(table, end, t->name, [](const engine::meta::Virtual& v, const std::string& n) {
            return v.owner && std::strcmp(v.owner, n.c_str()) < 0;
        });
        for (; it != end && it->owner && t->name == it->owner; ++it)
            if (std::strcmp(it->name, name) == 0) {
                *outOffset = it->offset;
                return GN_OK;
            }
    }
    return GN_ERR_NOT_FOUND;
}

int hookVirtual(const char* mod, const Type* type, uint32_t offset, void* detour, uint32_t flags, uint32_t* outHandle) {
    if (!mod || !type || !detour || !outHandle || offset % 8 || offset >= kMaxOffset) return GN_ERR_ARGUMENT;
    if (type->category != reflect::Category::Class) return GN_ERR_ARGUMENT;
    // Slot 0 is getType: Registry::typeOf reads it to know every object's class, and a hook there
    // makes the class's objects unknown (the recorder's tracing did this until 2026-10-29).
    if (offset == 0) return GN_ERR_SLOT;
    if (unloading()) return GN_ERR_UNAVAILABLE;
    const bool subclasses = (flags & GN_HOOK_SUBCLASSES) != 0;
    size_t pastEnd = 0;
    std::vector<uint64_t> vts = vtablesOf(type, subclasses, offset, &pastEnd);
    if (pastEnd) GN_LOG("[%s] hook_virtual %s+0x%x: past the end of %llu vtable(s): those are left alone", mod, type->name.c_str(), offset,
                        static_cast<unsigned long long>(pastEnd));
    if (vts.empty()) {
        GN_LOG("[%s] hook_virtual %s+0x%x: no vtable of the class GridNode knows has that slot", mod, type->name.c_str(), offset);
        return pastEnd ? GN_ERR_SLOT : GN_ERR_NOT_FOUND;
    }
    auto set = std::make_unique<vhook::Set>(mod, std::move(vts), offset, reinterpret_cast<uint64_t>(detour));
    std::string why;
    const size_t ok = set->install(hooks(), &why);
    if (!ok) {
        GN_LOG("[%s] hook_virtual %s+0x%x refused: %s", mod, type->name.c_str(), offset, why.c_str());
        return GN_ERR_SLOT;
    }
    GN_LOG("[%s] hooked virtual %s+0x%x in %llu of %llu vtables%s%s%s", mod, type->name.c_str(), offset, static_cast<unsigned long long>(ok),
           static_cast<unsigned long long>(set->vtableCount()), subclasses ? " (with subclasses)" : "", why.empty() ? "" : "; first refused: ",
           why.c_str());
    std::lock_guard<std::mutex> lock(g_m);
    const size_t i = g_count.load();
    if (i >= kMaxSets) {
        set->remove(hooks());
        GN_LOG("[%s] hook_virtual: %llu class hooks already; refused", mod, static_cast<unsigned long long>(kMaxSets));
        g_owned.push_back(std::move(set)); // the table may still point at it if a removal was refused
        g_classOf.push_back(type->name);
        return GN_ERR_FULL;
    }
    g_sets[i].store(set.get());
    g_owned.push_back(std::move(set));
    g_classOf.push_back(type->name + (subclasses ? " (and subclasses)" : ""));
    g_count.store(i + 1);
    *outHandle = static_cast<uint32_t>(i + 1);
    return GN_OK;
}

void* hookOriginal(uint32_t handle, const void* self) {
    if (!self) return nullptr;
    if (!handle || handle > g_count.load(std::memory_order_acquire)) return nullptr;
    const vhook::Set* set = g_sets[handle - 1].load(std::memory_order_acquire);
    if (!set) return nullptr;
    // A plain load: self is the object the game just called the hook with.
    const uint64_t vt = *static_cast<const uint64_t*>(self);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(set->original(vt)));
}

int hookVirtualRemove(uint32_t handle) {
    std::lock_guard<std::mutex> lock(g_m);
    if (!handle || handle > g_count.load() || !g_sets[handle - 1].load()) return GN_ERR_NOT_FOUND;
    vhook::Set& set = *g_sets[handle - 1].load();
    std::vector<std::string> refused;
    const size_t n = set.remove(hooks(), &refused);
    GN_LOG("[%s] hook_virtual +0x%x taken out of %llu vtables%s", set.owner().c_str(), set.offset(), static_cast<unsigned long long>(n),
           refused.empty() ? "" : " (some stay: another mod's hook is on top)");
    return set.hookedCount() ? GN_ERR_SLOT : GN_OK; // some stay: another mod's hook on top
}

std::vector<VirtualHookInfo> virtualHookList() {
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<VirtualHookInfo> out;
    for (size_t i = 0; i < g_count.load(); ++i) {
        const vhook::Set* s = g_sets[i].load();
        if (s && s->hookedCount()) out.push_back({s->owner(), g_classOf[i], s->offset(), s->hookedCount(), s->vtableCount()});
    }
    return out;
}

void forgetVirtualHooks(const CodeRange& r) {
    // The hook table entries went with forgetHooks; the sets whose detour is in r are only bookkeeping now.
    std::lock_guard<std::mutex> lock(g_m);
    for (size_t i = 0; i < g_owned.size(); ++i) {
        vhook::Set& s = *g_owned[i];
        if (!r.has(reinterpret_cast<void*>(static_cast<uintptr_t>(s.detour()))) || !s.hookedCount()) continue;
        s.remove(hooks()); // recounts what stays
        GN_LOG("[%s] virtual %s+0x%x taken out%s", s.owner().c_str(), g_classOf[i].c_str(), s.offset(),
               s.hookedCount() ? " (some slots stay: another mod's hook is on top)" : "");
    }
}

} // namespace gn::host
