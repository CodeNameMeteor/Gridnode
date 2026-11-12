#pragma once
// The game's own lists of a class's entities, one per realm (ClassInfo_addToWorldList, 0x1431661f0):
// head at ClassInfo + (realm + 6) * 0x10, and a second list at + (realm + 8) * 0x10. A node is
// {next, pointer to the previous link}, embedded in the entity at an offset that depends on the class
// (characters +0x40, AI groups +0x30, splines +0x110, zone streamers +0x268). GridNode walks them once
// at startup to find the entities made before it was injected (the creator hook sees only new ones):
// from each node it steps back 8 bytes at a time to the address whose vtable says it is exactly that
// class, and keeps that offset for the class's other nodes. Portable: tested with fake memory.

#include <cstddef>
#include <cstdint>

namespace gn::classlists {

constexpr uint64_t kHeadsAt = 0x60;  // (0 + 6) * 0x10: client realm, first list
constexpr uint64_t kHeadStride = 0x10; // per realm
constexpr uint64_t kSecondList = 0x20; // (+ 8 instead of + 6) * 0x10
constexpr uint32_t kMaxBack = 0x800;   // the largest node offset looked for (0x268 is the largest seen)

// The heads to walk for a class: two lists in each of two realms.
inline void heads(uint64_t classInfo, uint64_t out[4]) {
    for (int realm = 0; realm < 2; ++realm) {
        out[realm * 2] = classInfo + kHeadsAt + realm * kHeadStride;
        out[realm * 2 + 1] = classInfo + kHeadsAt + kSecondList + realm * kHeadStride;
    }
}

// Walks the list at `head`. read(addr, uint64_t&) reads memory (false: unreadable); isClass(addr):
// an object of exactly the list's class is at addr; add(addr) is given each entity. `back` is the
// class's node offset once known (0: not yet), found from the first node that resolves. At most
// maxNodes nodes (a list changing under the walk, or a short ClassInfo whose "head" is not one).
// Returns the entities found; `unresolved` counts nodes no entity was found for.
template <class Read, class IsClass, class Add>
size_t walk(uint64_t head, uint32_t& back, Read read, IsClass isClass, Add add, size_t maxNodes, size_t& unresolved) {
    size_t found = 0;
    uint64_t node = 0;
    if (!read(head, node)) return 0;
    for (size_t n = 0; node && n < maxNodes; ++n) {
        if (node & 7) break; // not a node
        uint64_t entity = 0;
        if (back && node >= back && isClass(node - back)) {
            entity = node - back;
        } else {
            for (uint32_t k = 8; k <= kMaxBack && node >= k; k += 8)
                if (isClass(node - k)) {
                    entity = node - k;
                    if (!back) back = k;
                    break;
                }
        }
        if (entity) {
            add(entity);
            ++found;
        } else {
            ++unresolved;
        }
        uint64_t next = 0;
        if (!read(node, next) || next == node) break;
        node = next;
    }
    return found;
}

} // namespace gn::classlists
