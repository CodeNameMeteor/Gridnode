// probe: a research mod. Walks pointer chains (from a global, through offsets) and logs every hop:
// the pointer, the object's vtable (as an image VA, to look up in Ghidra) and its reflected type,
// and where it sits relative to Faith's client and server characters. Used to turn MEC_Trainer's
// raw chains into named GridNode findings. F9 runs it; it also runs once, 5 s after Faith appears.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>
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

std::vector<std::pair<std::string, uint64_t>> g_targets; // chain name -> final address (this run)

void run(const char* why) {
    g_targets.clear();
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
            g_targets.push_back({c.name, addr});
        }
        gn::log("%s", line.c_str());
    }
}

// ---- which of Faith's members are objects, and of what type -------------------------------------
void members(const char* what, uint64_t obj, uint32_t size) {
    if (!obj) return;
    std::vector<uint64_t> q(size / 8);
    if (gn::api().read(reinterpret_cast<const void*>(obj), q.data(), size) != GN_OK) return;
    std::string line;
    for (size_t i = 0; i < q.size(); ++i) {
        uint64_t vt = 0;
        if (q[i] < 0x10000 || inImage(q[i]) || !gn::read(reinterpret_cast<const void*>(q[i]), vt) || !inImage(vt)) continue;
        gn::Type t = gn::Type::of(reinterpret_cast<void*>(q[i]));
        char b[128];
        snprintf(b, sizeof(b), " +%llx:%s(vt %llx)", static_cast<unsigned long long>(i * 8), t ? t.name() : "?",
                 static_cast<unsigned long long>(va(vt)));
        line += b;
        if (line.size() > 700) {
            gn::log("members of %s %llx:%s", what, static_cast<unsigned long long>(obj), line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) gn::log("members of %s %llx:%s", what, static_cast<unsigned long long>(obj), line.c_str());
}

// ---- pointer paths from Faith to each chain's final address ----------------------------------------
// Breadth first over heap pointers, from client and server Faith, scanning kSpan bytes of every node.
// A target is reached when it lies inside a node's span: the path is the offsets taken.
constexpr uint32_t kSpan = 0x800;
constexpr int kDepth = 5;
constexpr size_t kMaxNodes = 400000;

struct Node {
    uint64_t addr;
    int parent; // index into the node list, -1 for a root
    uint32_t off; // offset in the parent the pointer was read from
    int depth;
    int root;
};

bool heapPtr(uint64_t p) { return p > 0x100000 && p < 0x7fffffff0000ull && !(p & 7) && !inImage(p); }

void pathfind(std::vector<std::pair<std::string, uint64_t>> targets, std::vector<std::pair<std::string, uint64_t>> roots) {
    std::vector<Node> nodes;
    std::unordered_set<uint64_t> seen;
    std::vector<bool> found(targets.size(), false);
    for (size_t r = 0; r < roots.size(); ++r)
        if (roots[r].second) {
            nodes.push_back({roots[r].second, -1, 0, 0, static_cast<int>(r)});
            seen.insert(roots[r].second);
        }
    std::vector<uint64_t> buf(kSpan / 8);
    size_t remaining = targets.size();
    for (size_t i = 0; i < nodes.size() && remaining; ++i) {
        const Node n = nodes[i];
        // targets inside this node
        for (size_t t = 0; t < targets.size(); ++t) {
            if (found[t] || targets[t].second < n.addr || targets[t].second >= n.addr + kSpan) continue;
            found[t] = true;
            --remaining;
            std::string path;
            char b[48];
            snprintf(b, sizeof(b), "+%llx", static_cast<unsigned long long>(targets[t].second - n.addr));
            path = b;
            for (int k = static_cast<int>(i); nodes[k].parent >= 0; k = nodes[k].parent) {
                gn::Type ty = gn::Type::of(reinterpret_cast<void*>(nodes[k].addr));
                snprintf(b, sizeof(b), "+%x->%s", nodes[k].off, ty ? ty.name() : "");
                path = std::string(b) + " " + path;
            }
            gn::log("path %s: %s %s  (depth %d)", targets[t].first.c_str(), roots[n.root].first.c_str(), path.c_str(), n.depth);
        }
        if (n.depth >= kDepth || nodes.size() >= kMaxNodes) continue;
        // Read what can be read (a span can run off the end of an allocation into a free page).
        uint32_t span = kSpan;
        while (span >= 0x40 && gn::api().read(reinterpret_cast<const void*>(n.addr), buf.data(), span) != GN_OK) span /= 2;
        if (span < 0x40) continue;
        for (uint32_t k = 0; k < span / 8; ++k) {
            const uint64_t p = buf[k];
            if (!heapPtr(p) || !seen.insert(p).second) continue;
            nodes.push_back({p, static_cast<int>(i), k * 8, n.depth + 1, n.root});
        }
    }
    for (size_t t = 0; t < targets.size(); ++t)
        if (!found[t]) gn::log("path %s: not found within depth %d (%llu nodes)", targets[t].first.c_str(), kDepth,
                               static_cast<unsigned long long>(nodes.size()));
    gn::log("==== pathfind done (%llu nodes)", static_cast<unsigned long long>(nodes.size()));
}

void vtable(const char* what, void* obj);

void deep(const char* why) {
    run(why);
    void* cf = gn::localClientCharacter();
    void* sf = gn::localServerCharacter();
    vtable("clientFaith", cf);
    vtable("serverFaith", sf);
    members("clientFaith", reinterpret_cast<uint64_t>(cf), 0x800);
    members("serverFaith", reinterpret_cast<uint64_t>(sf), 0x1600);
    uint64_t mv = 0;
    if (cf && gn::read(static_cast<char*>(cf) + 0xb0, mv)) members("clientFaith+0xb0", mv, 0x200);
    std::vector<std::pair<std::string, uint64_t>> targets;
    for (auto& t : g_targets)
        if (!inImage(t.second) && t.first != std::string("playerX")) targets.push_back(t);
    std::vector<std::pair<std::string, uint64_t>> roots = {{"clientFaith", reinterpret_cast<uint64_t>(cf)},
                                                           {"serverFaith", reinterpret_cast<uint64_t>(sf)}};
    if (strcmp(why, "F9") != 0) return; // the path search only on F9
    // In the background: it reads a lot; reads are fault-safe, and the game keeps running.
    std::thread([targets, roots] { pathfind(targets, roots); }).detach();
}

// ---- the virtual functions of Faith's classes ---------------------------------------------------------
bool isCode(uint64_t a) { return inImage(a) && (va(a) < 0x141878000ull || (va(a) >= 0x14292a000ull && va(a) < 0x147263000ull)); }

void vtable(const char* what, void* obj) {
    uint64_t vt = 0;
    if (!obj || !gn::read(obj, vt)) return;
    std::string line;
    int n = 0;
    for (; n < 512; ++n) {
        uint64_t fn = 0;
        if (!gn::read(reinterpret_cast<const void*>(vt + n * 8), fn) || !isCode(fn)) break;
        char b[40];
        snprintf(b, sizeof(b), " %x:%llx", n * 8, static_cast<unsigned long long>(va(fn)));
        line += b;
        if (line.size() > 800) {
            gn::log("vtable of %s:%s", what, line.c_str());
            line.clear();
        }
    }
    gn::log("vtable of %s (vt %llx, %d slots):%s", what, static_cast<unsigned long long>(va(vt)), n, line.c_str());
}

// ---- what changes while Space is held (F10 arms it) ----------------------------------------------------
// Snapshots a graph of objects around Faith and the input system three times: before Space, while it is
// held, and after it is released. A 4-byte slot that is zero, then non-zero, then zero again is logged.
struct Snap {
    std::vector<Node> nodes;
    std::vector<uint32_t> spans;
    std::vector<std::vector<uint32_t>> a, b, c;
    std::vector<std::string> rootNames;
    int stage = 0; // 0 off, 1 wait for Space, 2 wait for release, 3 diffing
    int ticks = 0;
};
Snap g_snap;

void snapshot(std::vector<std::vector<uint32_t>>& out) {
    out.assign(g_snap.nodes.size(), {});
    for (size_t i = 0; i < g_snap.nodes.size(); ++i) {
        out[i].resize(g_snap.spans[i] / 4);
        if (gn::api().read(reinterpret_cast<const void*>(g_snap.nodes[i].addr), out[i].data(), g_snap.spans[i]) != GN_OK)
            out[i].clear();
    }
}

void armDiff() {
    g_snap = Snap{};
    void* cf = gn::localClientCharacter();
    if (!cf) {
        gn::log("diff: no Faith");
        return;
    }
    // Roots: Faith, and her ClientPlayer (Faith +0x238; also ClientGameContext +0x68 -> +0x568).
    uint64_t player = 0;
    gn::read(static_cast<char*>(cf) + 0x238, player);
    std::vector<std::pair<std::string, uint64_t>> roots = {{"clientFaith", reinterpret_cast<uint64_t>(cf)},
                                                           {"clientPlayer", player}};
    std::unordered_set<uint64_t> seen;
    for (size_t r = 0; r < roots.size(); ++r) {
        g_snap.rootNames.push_back(roots[r].first);
        if (roots[r].second && seen.insert(roots[r].second).second) g_snap.nodes.push_back({roots[r].second, -1, 0, 0, static_cast<int>(r)});
    }
    std::vector<uint64_t> data;
    for (size_t i = 0; i < g_snap.nodes.size(); ++i) {
        const Node n = g_snap.nodes[i];
        // Only the object itself: its reflected size when known, so neighbours in memory are not mistaken for it.
        gn::Type ty = gn::Type::of(reinterpret_cast<void*>(n.addr));
        uint32_t span = ty && ty.size() >= 0x10 ? ty.size() : 0x200;
        if (n.root == 1 && n.depth == 0) span = 0x3000; // ClientPlayer is big and not reflected in full
        span = (span + 7) & ~7u;
        data.assign(span / 8, 0);
        while (span >= 0x40 && gn::api().read(reinterpret_cast<const void*>(n.addr), data.data(), span) != GN_OK) span /= 2;
        g_snap.spans.push_back(span >= 0x40 ? span : 0);
        if (span < 0x40 || n.depth >= 3 || g_snap.nodes.size() >= 40000) continue;
        for (uint32_t k = 0; k < span / 8; ++k) {
            const uint64_t p = data[k];
            if (!heapPtr(p) || !seen.insert(p).second) continue;
            g_snap.nodes.push_back({p, static_cast<int>(i), k * 8, n.depth + 1, n.root});
        }
    }
    g_snap.spans.resize(g_snap.nodes.size(), 0);
    snapshot(g_snap.a);
    g_snap.stage = 1;
    gn::log("diff: %llu objects watched; HOLD SPACE NOW for half a second, then let go", static_cast<unsigned long long>(g_snap.nodes.size()));
}

void diffTick() {
    const bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    if (g_snap.stage == 1) {
        g_snap.ticks = space ? g_snap.ticks + 1 : 0;
        if (g_snap.ticks == 3) {
            snapshot(g_snap.b);
            g_snap.stage = 2;
            g_snap.ticks = 0;
        }
    } else if (g_snap.stage == 2) {
        g_snap.ticks = space ? 0 : g_snap.ticks + 1;
        if (g_snap.ticks == 3) {
            snapshot(g_snap.c);
            g_snap.stage = 3;
            std::thread([] {
                int hits = 0;
                for (size_t i = 0; i < g_snap.nodes.size() && hits < 300; ++i) {
                    const auto &A = g_snap.a[i], &B = g_snap.b[i], &C = g_snap.c[i];
                    if (A.empty() || B.size() != A.size() || C.size() != A.size()) continue;
                    for (size_t k = 0; k < A.size() && hits < 300; ++k) {
                        if (A[k] != 0 || B[k] == 0 || C[k] != 0) continue;
                        std::string path;
                        char b[64];
                        for (int j = static_cast<int>(i); g_snap.nodes[j].parent >= 0; j = g_snap.nodes[j].parent) {
                            gn::Type ty = gn::Type::of(reinterpret_cast<void*>(g_snap.nodes[j].addr));
                            snprintf(b, sizeof(b), "+%x->%s ", g_snap.nodes[j].off, ty ? ty.name() : "");
                            path = b + path;
                        }
                        float f;
                        memcpy(&f, &B[k], 4);
                        gn::Type own = gn::Type::of(reinterpret_cast<void*>(g_snap.nodes[i].addr));
                        gn::log("diff: %s %s+%llx [%s]  held = %u / %g", g_snap.rootNames[g_snap.nodes[i].root].c_str(), path.c_str(),
                                static_cast<unsigned long long>(k * 4), own ? own.name() : "?", B[k], f);
                        ++hits;
                    }
                }
                gn::log("==== diff done (%d slots)", hits);
                g_snap.stage = 0;
            }).detach();
        }
    }
}

// ---- jump tests: write a candidate, see whether Faith rises ---------------------------------------------
struct JumpTest {
    const char* name;
    uint32_t off;  // in ClientPlayer (Faith +0x238), or in the input devices object when raw
    bool raw;
    uint32_t value;
};
const JumpTest kJumpTests[] = {
    {"ClientPlayer+0x1a28 = 1", 0x1a28, false, 1},
    {"ClientPlayer+0x1a9c = 1", 0x1a9c, false, 1},
    {"ClientPlayer+0x1a28 and +0x1a9c = 1", 0, false, 1},
    {"raw keyboard Space (MEC_Trainer's way, the control)", 0x1bb0, true, 1},
};
int g_test = -1;    // the test running, -1 none
int g_testTick = 0;
float g_y0 = 0, g_yMax = 0;

void jumpWrite(const JumpTest& t, uint32_t v) {
    if (t.raw) {
        uint64_t input = 0;
        if (gn::read(reinterpret_cast<const void*>(gn::rva(0x14256D7C0)), input) && input)
            gn::write(reinterpret_cast<void*>(input + t.off), v);
        return;
    }
    uint64_t player = 0;
    void* cf = gn::localClientCharacter();
    if (!cf || !gn::read(static_cast<char*>(cf) + 0x238, player) || !player) return;
    if (t.off) gn::write(reinterpret_cast<void*>(player + t.off), v);
    else {
        gn::write(reinterpret_cast<void*>(player + 0x1a28), v);
        gn::write(reinterpret_cast<void*>(player + 0x1a9c), v);
    }
}

void jumpTick() {
    if (g_test < 0) return;
    const JumpTest& t = kJumpTests[g_test];
    gn::Vec3 p{};
    void* cf = gn::localClientCharacter();
    if (!cf || !gn::positionOf(cf, p)) return;
    if (g_testTick == 0) {
        g_y0 = g_yMax = p.y;
        gn::log("jump test %d: %s", g_test + 1, t.name);
    }
    if (p.y > g_yMax) g_yMax = p.y;
    if (g_testTick < 4) jumpWrite(t, t.value);      // "held" for 4 ticks
    else if (g_testTick == 4) jumpWrite(t, 0);       // released
    if (++g_testTick == 75) {                        // ~2.5 s later
        gn::log("jump test %d: Faith rose %.2f m -> %s", g_test + 1, g_yMax - g_y0, g_yMax - g_y0 > 0.3f ? "JUMPED" : "no jump");
        g_testTick = 0;
        if (++g_test == static_cast<int>(sizeof(kJumpTests) / sizeof(kJumpTests[0]))) {
            g_test = -1;
            gn::log("==== jump tests done");
        }
    }
}

// ---- F12: the EntryInputState (input actions by hash) -----------------------------------------------
// Registry (0x1424aa1a0): hash map at +0x10 (buckets) / +0x18 (bucket count), nodes {u32 hash,
// u32 index, next at +8}; +0x94 analog count, +0x98 first digital index, +0xa4 end. A state is
// 0x530 bytes: analog floats at +0x350 + 4*index, digital bits at +0x4b8 (bit index - first digital).
// ClientPlayer +0x14b0 / +0x14b8 / +0x14c0 hold states; +0x1520 flags (0x40: use +0x14c0).
std::vector<std::pair<uint32_t, uint32_t>> g_actions; // hash, index
int g_watch = 0;                                       // ticks left
uint64_t g_lastBits[3] = {};
uint32_t g_lastFlags = 0;

void dumpRegistry() {
    g_actions.clear();
    uint64_t reg = 0;
    if (!gn::read(reinterpret_cast<const void*>(gn::rva(0x1424aa1a0)), reg) || !reg) return gn::log("input: no registry");
    uint64_t buckets = 0;
    uint32_t count = 0, nums[6] = {};
    gn::read(reinterpret_cast<const void*>(reg + 0x10), buckets);
    gn::read(reinterpret_cast<const void*>(reg + 0x18), count);
    gn::api().read(reinterpret_cast<const void*>(reg + 0x90), nums, sizeof(nums));
    gn::log("input: registry %llx, %u buckets; counts %u analog %u, digital from %u, %u %u, end %u", (unsigned long long)reg, count,
            nums[0], nums[1], nums[2], nums[3], nums[4], nums[5]);
    uint64_t sentinel = 0;
    gn::read(reinterpret_cast<const void*>(buckets + count * 8ull), sentinel);
    for (uint32_t b = 0; b < count && b < 4096; ++b) {
        uint64_t n = 0;
        gn::read(reinterpret_cast<const void*>(buckets + b * 8ull), n);
        for (int guard = 0; n && n != sentinel && guard < 64; ++guard) {
            uint32_t hi[2] = {};
            gn::api().read(reinterpret_cast<const void*>(n), hi, 8);
            g_actions.push_back({hi[0], hi[1]});
            gn::read(reinterpret_cast<const void*>(n + 8), n);
        }
    }
    std::sort(g_actions.begin(), g_actions.end(), [](auto& a, auto& b) { return a.second < b.second; });
    std::string line;
    for (auto& a : g_actions) {
        char t[32];
        snprintf(t, sizeof(t), " %u:%08x", a.second, a.first);
        line += t;
        if (line.size() > 700) gn::log("input actions:%s", line.c_str()), line.clear();
    }
    if (!line.empty()) gn::log("input actions:%s", line.c_str());
}

void watchTick() {
    if (g_watch <= 0) return;
    --g_watch;
    void* cf = gn::localClientCharacter();
    uint64_t player = 0;
    if (!cf || !gn::read(static_cast<char*>(cf) + 0x238, player) || !player) return;
    uint32_t flags = 0;
    gn::read(reinterpret_cast<const void*>(player + 0x1520), flags);
    if (flags != g_lastFlags) gn::log("input: player flags %x -> %x", g_lastFlags, flags), g_lastFlags = flags;
    const uint32_t offs[3] = {0x14b0, 0x14b8, 0x14c0};
    for (int i = 0; i < 3; ++i) {
        uint64_t st = 0, bits = 0;
        if (!gn::read(reinterpret_cast<const void*>(player + offs[i]), st) || !st) continue;
        gn::read(reinterpret_cast<const void*>(st + 0x4b8), bits);
        if (bits != g_lastBits[i]) {
            gn::log("input: state +%x (%llx) digital bits %llx -> %llx (Space %s)", offs[i], (unsigned long long)st,
                    (unsigned long long)g_lastBits[i], (unsigned long long)bits, (GetAsyncKeyState(VK_SPACE) & 0x8000) ? "down" : "up");
            g_lastBits[i] = bits;
        }
    }
    if (g_watch == 0) gn::log("==== input watch done");
}

} // namespace

GN_MOD("probe") {
    gn::onClientTick([](float dt) {
        static bool was = false;
        const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !was) deep("F9");
        was = down;
        static bool was10 = false;
        const bool down10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (down10 && !was10) {
            gn::log("F10 pressed (diff stage %d)", g_snap.stage);
            if (g_snap.stage == 0) armDiff();
        }
        was10 = down10;
        if (g_snap.stage == 1 || g_snap.stage == 2) diffTick();
        static float t = -1.f; // seconds since Faith appeared (-1: not yet / not this level)
        if (!gn::localClientCharacter()) {
            t = -1.f;
            return;
        }
        if (t < 0.f) t = 0.f;
        const float before = t;
        t += dt;
        (void)before; // nothing runs by itself any more: F9, F10, F11
        static bool was12 = false;
        const bool down12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (down12 && !was12 && g_watch <= 0) {
            dumpRegistry();
            g_watch = 30 * 8; // about 8 s
            gn::log("input watch: press Space a few times (and Shift, Ctrl, E) in the next 8 s");
        }
        was12 = down12;
        watchTick();
        static bool was11 = false;
        const bool down11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (down11 && !was11 && g_test < 0) {
            gn::log("jump tests start: stand still on flat ground and do not touch anything for 12 s");
            g_test = 0;
            g_testTick = 0;
        }
        was11 = down11;
        jumpTick();
    });
    gn::log("ready: F9 walks MEC_Trainer's pointer chains; F10 then Space finds what Space changes; F11 runs the jump tests; F12 watches the input actions");
    return true;
}
