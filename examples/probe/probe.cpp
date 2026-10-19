// probe: a research mod. Walks pointer chains (from a global, through offsets) and logs every hop:
// the pointer, the object's vtable (as an image VA, to look up in Ghidra) and its reflected type,
// and where it sits relative to Faith's client and server characters. Used to turn MEC_Trainer's
// raw chains into named GridNode findings. F9 runs it; it also runs once, 5 s after Faith appears.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "gridnode/mod.hpp"

namespace {

struct Chain {
    const char* name;
    uint64_t base; // image VA of the global the chain starts from
    std::vector<uint32_t> offsets;
};

// MEC_Trainer's chains (include/Game/GameOffsets.hpp, src/Game/GameState.cpp, features).
const std::vector<Chain> kChains = {
    {"loading", 0x14240C2B8, {0x4C1}},
    {"playerX", 0x142106A68, {0xD0, 0x128, 0x30, 0x50}},
    {"velocity", 0x1423E29B8, {0x2378, 0x10, 0x438}},
    {"playerState", 0x142106A68, {0xB0, 0x48, 0x0, 0x28, 0x90}},
    {"wallCount", 0x142106A68, {0xB0, 0x48, 0x0, 0x28, 0x18}},
    {"immortal", 0x142577770, {0x70, 0xC0, 0x0, 0x0}},
    {"inputEnabled", 0x14214B460, {0x8D4}},
    {"lastGroundY", 0x1423DA028, {0x20, 0x20, 0x40, 0x20, 0x4}},
    {"onGround", 0x1423DA028, {0x20, 0x20, 0x40, 0x20, 0x17}},
    {"timeOfDay", 0x14255C2F8, {0x8, 0x28, 0x30}},
    {"contentActive", 0x14257E7C8, {0x28, 0x280}},
    {"dashStarted", 0x14257C9D0, {0xE0}},
    {"camSin", 0x142578A68, {0x70, 0x98, 0x238, 0x18, 0x22C4}},
    {"camFwdX", 0x142401CB0, {0x68, 0x568, 0x14a0, 0x250, 0x70}},
    {"inMenu", 0x1425946E4, {}},
    {"maxRunSpeed", 0x1423BD908, {0x408, 0x380, 0x1C5C}},
    {"jumpTrigger", 0x14256D7C0, {0x1bb0}},
    {"maxSpeedJumpHeight", 0x142105510, {0xA48, 0x120, 0x8B4}},
    {"wcMaxAngleDelta", 0x1423DD0B8, {0x22B0, 0x30, 0x5BC}},
    {"fwcHeight", 0x1423DD0B8, {0x26F0, 0x58, 0x380}},
    {"wrMinimumLength", 0x1423DD0B8, {0x1A78, 0xE0}},
    {"coilHeight", 0x1423DD0B8, {0xC8, 0xC98, 0x1FD0}},
    {"maxSlideSpeed", 0x1423DD0B8, {0x1780, 0xB88, 0x63C}},
    {"pullupAcc", 0x1423DD0B8, {0x778, 0x28, 0x324}},
    {"swingGravity", 0x1423DD0B8, {0x2458, 0xD0, 0xD8}},
    {"visuals", 0x1423DD0A8, {0x2450, 0x8, 0x22b}},
};

uint64_t g_base = 0;
uint64_t va(uint64_t a) { return a - g_base + 0x140000000ull; }
bool inImage(uint64_t a) { return a >= g_base && a < g_base + 0x7300000; }

std::string where(uint64_t p) {
    char b[160] = "";
    auto rel = [&](const char* what, void* obj) {
        const uint64_t o = reinterpret_cast<uint64_t>(obj);
        if (o && p >= o && p < o + 0x4000) snprintf(b, sizeof(b), " = %s+0x%llx", what, static_cast<unsigned long long>(p - o));
    };
    rel("serverFaith", gn::localServerCharacter());
    rel("clientFaith", gn::localClientCharacter());
    return b;
}

std::string describe(uint64_t p) {
    char b[300];
    uint64_t vt = 0;
    std::string s;
    if (gn::read(reinterpret_cast<const void*>(p), vt) && inImage(vt)) {
        gn::Type t = gn::Type::of(reinterpret_cast<void*>(p));
        snprintf(b, sizeof(b), " [vt %llx%s%s]", static_cast<unsigned long long>(va(vt)), t ? " " : "", t ? t.name() : "");
        s = b;
    }
    return s + where(p);
}

void run(const char* why) {
    g_base = gn::api().module_base();
    gn::log("==== probe (%s): client Faith %p, server Faith %p", why, gn::localClientCharacter(), gn::localServerCharacter());
    for (const Chain& c : kChains) {
        uint64_t addr = gn::rva(c.base);
        std::string line = std::string(c.name) + ": [" + std::to_string(0) + "] ";
        char b[96];
        bool ok = true;
        for (size_t i = 0; i < c.offsets.size(); ++i) {
            uint64_t p = 0;
            if (!gn::read(reinterpret_cast<const void*>(addr), p) || !p) {
                snprintf(b, sizeof(b), "*%llx unreadable/null", static_cast<unsigned long long>(inImage(addr) ? va(addr) : addr));
                line += b;
                ok = false;
                break;
            }
            snprintf(b, sizeof(b), "*%llx=%llx", static_cast<unsigned long long>(inImage(addr) ? va(addr) : addr), static_cast<unsigned long long>(p));
            line += b + describe(p) + " +" + [&] { char o[16]; snprintf(o, sizeof(o), "%x", c.offsets[i]); return std::string(o); }() + " | ";
            addr = p + c.offsets[i];
        }
        if (ok) {
            uint32_t u = 0;
            float f = 0;
            gn::read(reinterpret_cast<const void*>(addr), u);
            memcpy(&f, &u, 4);
            snprintf(b, sizeof(b), "final %llx = %u / %g", static_cast<unsigned long long>(inImage(addr) ? va(addr) : addr), u, f);
            line += b + where(addr);
        }
        gn::log("%s", line.c_str());
    }
}

} // namespace

GN_MOD("probe") {
    gn::onClientTick([](float dt) {
        static bool was = false;
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was) run("F9");
        was = down;
        static float t = -1.f; // seconds since Faith appeared (-1: not yet / not this level)
        if (!gn::localClientCharacter()) {
            t = -1.f;
            return;
        }
        if (t < 0.f) t = 0.f;
        const float before = t;
        t += dt;
        if (before < 5.f && t >= 5.f) run("5 s after Faith appeared");
    });
    gn::log("ready: F9 walks MEC_Trainer's pointer chains");
    return true;
}
