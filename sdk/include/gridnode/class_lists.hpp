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
#include <unordered_set>
#include <utility>
#include <vector>

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

// The game's entity buses (read in Ghidra: EntityBus_deliverEvent 0x14314eac0, EntityBus_release
// 0x1431547b0). Every entity keeps its bus's interface I (bus + 8) at entity +0x20; offsets below are
// from I. Buses form a tree (parent I+8, first child I+0x28, siblings I+0x18 / I+0x20), and each bus
// holds its peers (the entities its data made) in an array at I+0x38, in the order of its data's
// sorted id list (data at I+0x60, list {begin, end} at data+0xd0, or +0xf8 when I+0x34 bit 28 is
// set). From a few known entities GridNode walks up to every root and down the whole tree, which
// finds the logic entities (NotEntity, DelayEntity, ...) no class list has.
namespace gn::buses {

constexpr uint64_t kEntityBus = 0x20;
constexpr uint64_t kParent = 0x08;
constexpr uint64_t kNext = 0x20;
constexpr uint64_t kFirstChild = 0x28;
constexpr uint64_t kFlagsWord = 0x30; // u32 flags at +0x34: the high half of this word
constexpr uint64_t kPeers = 0x38;
constexpr uint64_t kData = 0x60;
constexpr uint64_t kIds = 0xd0;
constexpr uint64_t kIdsOther = 0xf8;
constexpr uint64_t kOtherBit = 1ull << (32 + 28);
constexpr size_t kMaxBuses = 1u << 20;
constexpr size_t kMaxPeers = 1u << 16;

struct Stats {
    size_t roots = 0, buses = 0, peers = 0, rejected = 0;
};

// seeds: known entities. isEntity(addr): an entity is at addr (its vtable is an entity class).
// add(entity) for each peer that is one. Returns the stats; stops at kMaxBuses.
template <class Read, class IsEntity, class Add>
Stats walk(const std::vector<uint64_t>& seeds, Read read, IsEntity isEntity, Add add, std::unordered_set<uint64_t>* all = nullptr) {
    Stats s;
    std::unordered_set<uint64_t> roots, seen;
    for (uint64_t e : seeds) {
        uint64_t b = 0;
        if (!read(e + kEntityBus, b) || !b || (b & 7)) continue;
        if (seen.count(b)) continue; // its chain was walked from another entity
        for (int depth = 0; depth < 64; ++depth) {
            seen.insert(b);
            uint64_t p = 0;
            if (!read(b + kParent, p) || !p || (p & 7) || seen.count(p)) break;
            b = p;
        }
        uint64_t p = 0;
        if (read(b + kParent, p) && !p) roots.insert(b);
    }
    s.roots = roots.size();
    seen.clear();
    std::vector<uint64_t> todo(roots.begin(), roots.end());
    while (!todo.empty() && seen.size() < kMaxBuses) {
        const uint64_t b = todo.back();
        todo.pop_back();
        if (!seen.insert(b).second) continue;
        ++s.buses;
        uint64_t flags = 0, data = 0, peers = 0, begin = 0, end = 0;
        if (read(b + kFlagsWord, flags) && read(b + kData, data) && data && read(b + kPeers, peers) && peers) {
            const uint64_t ids = data + ((flags & kOtherBit) ? kIdsOther : kIds);
            if (read(ids, begin) && read(ids + 8, end) && end >= begin && (end - begin) / 8 <= kMaxPeers) {
                for (uint64_t i = 0, n = (end - begin) / 8; i < n; ++i) {
                    uint64_t e = 0;
                    if (!read(peers + i * 8, e) || !e || (e & 7)) continue;
                    if (isEntity(e)) add(e), ++s.peers;
                    else ++s.rejected;
                }
            }
        }
        uint64_t c = 0;
        if (!read(b + kFirstChild, c)) continue;
        for (size_t k = 0; c && !(c & 7) && k < kMaxBuses; ++k) {
            todo.push_back(c);
            if (!read(c + kNext, c)) break;
        }
    }
    if (all) *all = std::move(seen);
    return s;
}

// Not every entity is a bus peer: a bus keeps only those its data lists by id (the ones with
// connections; FUN_143150100 fills the slot), so a plain barrier or trigger is on no list. The last
// pass reads the game's heap: an entity starts with its vtable (in the image) and keeps its bus at
// +0x20, so a word in [imageLo, imageHi) with one of the known buses 0x20 bytes on is a candidate,
// checked by the caller (its vtable must be an entity class). `words` is a copy of memory from
// `base`; the last kEntityBus / 8 words only look ahead and are not candidates themselves.
template <class Candidate>
size_t scanWords(const uint64_t* words, size_t n, uint64_t base, uint64_t imageLo, uint64_t imageHi,
                 const std::unordered_set<uint64_t>& busSet, Candidate candidate) {
    size_t hits = 0;
    const size_t ahead = kEntityBus / 8;
    for (size_t i = 0; i + ahead < n; ++i) {
        const uint64_t w = words[i];
        if (w < imageLo || w >= imageHi || (w & 7)) continue;
        const uint64_t b = words[i + ahead];
        if (!b || (b & 7) || !busSet.count(b)) continue;
        if (candidate(base + i * 8)) ++hits;
    }
    return hits;
}

} // namespace gn::buses
