#pragma once
// The search for free memory within reach of a rel32 (patches.cpp, allocNear), kept apart from
// Windows so it can be tested on any OS with a fake address map. The query is a callback, the way
// HookTable receives SlotAccess.
//
// Windows' VirtualQuery describes a free region from the queried page upwards only: asked about the
// page below an address, it says the free block starts there and is one page long. Searching down by
// that answer made every free block look one page long, so a 16-page request never fitted and
// VirtualAlloc was never called (both logs of 2026-11-10: "no page within 2 GB ... (0 tries)").
// So the downward search steps candidate addresses on the allocation granularity and asks about each
// candidate itself.

#include <cstdint>
#include <functional>

namespace gn::nearmem {

// What VirtualQuery says about the page holding an address: [base, base + size) has one state;
// allocationBase is the start of the allocation a taken page belongs to (0 for free memory).
struct Region {
    uint64_t base = 0, size = 0;
    bool free = false;
    uint64_t allocationBase = 0;
};

using Query = std::function<bool(uint64_t addr, Region& out)>;
using TryAt = std::function<bool(uint64_t addr)>; // true: the memory was had there
using Stop = std::function<bool()>;               // true: give up (tries or time used up)

// Upwards from `from` to `hi`: the lowest granularity-aligned addresses of each free region (at most
// perRegion of them). 0 when nothing was had.
inline uint64_t searchUp(const Query& query, uint64_t from, uint64_t hi, uint64_t size, uint64_t gran, int perRegion, const TryAt& tryAt,
                         const Stop& stop) {
    Region r;
    for (uint64_t a = from; a < hi && !stop() && query(a, r);) {
        const uint64_t e = r.base + r.size;
        if (r.free) {
            const uint64_t to = e < hi ? e : hi;
            uint64_t x = ((r.base > a ? r.base : a) + gran - 1) & ~(gran - 1);
            for (int i = 0; i < perRegion && x + size <= to && !stop(); ++i, x += gran)
                if (tryAt(x)) return x;
        }
        if (e <= a) break;
        a = e;
    }
    return 0;
}

// Downwards from `from` (exclusive) to `lo`: the highest granularity-aligned addresses below it whose
// `size` bytes are all free, nearest the site first; at most perRegion tries in each run of free
// memory. 0 when nothing was had.
inline uint64_t searchDown(const Query& query, uint64_t from, uint64_t lo, uint64_t size, uint64_t gran, int perRegion, const TryAt& tryAt,
                           const Stop& stop) {
    int triesHere = 0; // in the current run of free memory
    for (uint64_t a = from; a > lo && a >= size && !stop();) {
        const uint64_t x = (a - size) & ~(gran - 1);
        if (x < lo || x == 0) break;
        Region r;
        if (!query(x, r)) break;
        uint64_t next;
        if (r.free) {
            const uint64_t top = r.base + r.size; // the free block runs at least from x up to here
            if (x + size <= top) {
                if (triesHere < perRegion) {
                    ++triesHere;
                    if (tryAt(x)) return x;
                }
                next = x; // the next candidate is a granule lower
            } else {
                next = top; // taken memory starts inside [x, x + size): look below it
            }
        } else {
            triesHere = 0;
            // A taken page: on below its whole allocation (one reservation can be many small regions).
            next = r.allocationBase && r.allocationBase < r.base ? r.allocationBase : r.base;
        }
        if (next >= a) break; // no progress: stop rather than loop
        a = next;
    }
    return 0;
}

} // namespace gn::nearmem
