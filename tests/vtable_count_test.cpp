// Host-side test for the vtable length count behind the free camera's vtable copies.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../sdk/src/vtable_count.hpp"

using gn::host::countVtableSlots;

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

namespace {
// Fake memory: code pointers are 0x1000..0x1fff; 0x5000 is a data pointer (an RTTI-like entry).
bool isCode(uintptr_t p) { return p >= 0x1000 && p < 0x2000; }

size_t count(const std::vector<uintptr_t>& v, size_t cap) {
    return countVtableSlots([&](size_t n) { return n < v.size() ? v[n] : uintptr_t(0); }, isCode, cap);
}
} // namespace

int main() {
    CHECK(count({0x1000, 0x1010, 0x1020, 0}, 256) == 3);            // ends at the null
    CHECK(count({0x1000, 0x1010, 0x5000, 0x1020}, 256) == 2);       // ends at a data pointer
    CHECK(count({0x5000}, 256) == 0);                               // nothing
    CHECK(count({}, 256) == 0);                                     // unreadable
    // Vtables back to back with no marker (Frostbite): the run never ends, which must read as "more than cap".
    std::vector<uintptr_t> run(1000, 0x1000);
    CHECK(count(run, 256) == 257);
    CHECK(count(run, 8) == 9);
    // Exactly cap entries is not "more than cap".
    std::vector<uintptr_t> exact(8, 0x1000);
    CHECK(count(exact, 8) == 8);
    std::vector<uintptr_t> one(9, 0x1000);
    CHECK(count(one, 8) == 9);
    if (g_failures) return 1;
    std::puts("vtable_count_test: ok");
    return 0;
}
