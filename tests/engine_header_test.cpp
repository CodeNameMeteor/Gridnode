// Checks the generated gridnode/engine.hpp: it compiles, and its tables are sane.

#include <cstdio>
#include <cstring>

#include "gridnode/engine.hpp"

namespace e = gn::engine;

static int g_failures = 0;
#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failures;                                                                     \
        }                                                                                     \
    } while (0)

static bool inImage(uint64_t va) { return va >= 0x140000000ull && va < 0x140000000ull + 0x727b000ull; }

int main() {
    // Values MECMP relies on, now coming from db/annotations.
    static_assert(e::global::TypeInfoListHead == 0x14215d028, "type list head");
    static_assert(e::hook::ServerGameWorld_postUpdate.slot == 0x141b42d28, "tick hook slot");
    static_assert(e::cls::PamClientCharacterEntity::vf::setVisible == 0x3d8, "setVisible slot");
    static_assert(e::cls::PamClientCharacterEntity::off::ghostShader == 0x334, "ghostShader");
    static_assert(e::st::SpawnInfo::off::busId == 0x5c && e::st::SpawnInfo::size == 0x70, "SpawnInfo");
    static_assert(e::hash::Visible == 0x35bc69ab, "djb2-xor of Visible");

    CHECK(sizeof(e::fingerprint::types) / sizeof(e::fingerprint::types[0]) >= 3);
    CHECK(sizeof(e::fingerprint::slots) / sizeof(e::fingerprint::slots[0]) >= 6);
    for (const auto& t : e::fingerprint::types) CHECK(inImage(t.ti) && std::strlen(t.name) > 0);
    for (const auto& s : e::fingerprint::slots) CHECK(inImage(s.slot) && inImage(s.fn) && s.slot % 8 == 0);
    for (const auto& c : e::fingerprint::code) CHECK(inImage(c.at) && c.count > 0 && c.count <= 32);
    CHECK(std::strcmp(e::constant::GhostBlueprint, "Characters/Projection/ProjectionCharacter") == 0);

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("engine_header_test: all checks passed");
    return 0;
}
