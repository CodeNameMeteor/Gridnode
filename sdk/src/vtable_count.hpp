#pragma once
// How many entries a vtable has, judged by its run of code pointers (host-testable; layouts.cpp supplies
// the reads and the code test). Up to cap + 1, to tell "more than cap".

#include <cstddef>
#include <cstdint>

namespace gn::host {

template <class Read, class IsCode> size_t countVtableSlots(Read read, IsCode isCode, size_t cap) {
    size_t n = 0;
    for (; n <= cap; ++n) {
        const uintptr_t fn = read(n);
        if (!fn || !isCode(fn)) break;
    }
    return n;
}

} // namespace gn::host
