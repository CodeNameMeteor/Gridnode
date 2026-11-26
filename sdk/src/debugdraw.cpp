// The game's own debug renderer (DebugRender2), switched on after start-up (experimental, untested in game).
// The shipped game never makes it (its creation is gated off), but everything that draws through it only checks
// DebugRender2_global, and the render engine flips and draws it every frame once that is set
// (audit 2026-11-13, scratch\audit-2026-11-13\debugrender-verified.md; the calls read in Ghidra 2026-11-14).
// GridNode makes it as the game would (DebugRender2Impl_ctor, the Impl vtable, DebugRender2_registerGlobal) on the
// render thread, then draws 3D lines through the recorder of that thread. Text needs the legacy renderer's font
// index buffer, which is never made: no text. The objects live in the game's arena; at unload only the global is
// cleared (nothing in them points into gridnode.dll).
//
// unverified: DebugRenderer_fontIndexBuffer (read only, to say whether text could work).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;
namespace k = gn::engine::constant;

struct Line {
    float a[3], b[3];
    uint32_t abgr;
    uint8_t flags = 0; // GN_DEBUG_* (debug_line_ex): the command's state byte
};

std::atomic<int> g_want{0};       // 1 on, -1 off (asked from the console; done on the render thread)
std::atomic<bool> g_ours{false};  // GridNode made the global (so it clears it at unload)
std::atomic<bool> g_test{false};  // a test cage around Faith every frame
std::atomic<uint8_t> g_testFlags{0}; // the cage's flags ("debugrender depth")
std::mutex g_m;
std::vector<Line> g_lines;        // (g_m) drawn once, at the next frame
std::atomic<uint64_t> g_drawn{0}, g_frames{0};
std::string g_status = "off";     // (g_m)

uintptr_t global() { return mem::readOr<uintptr_t>(mem::rva(e::global::DebugRender2_global)); }

void setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_m);
    g_status = s;
}

// Render thread. The game's own steps (DebugRenderer_initializeSystem's gated part), DebugRender2 only.
void create() {
    if (global()) {
        setStatus("on (the game's own: it was already there)");
        GN_LOG("debug render: DebugRender2 is already there; drawing through it");
        return;
    }
    const uintptr_t mgr = mem::readOr<uintptr_t>(mem::rva(e::global::SettingsManager));
    using FindFn = uintptr_t (*)(uintptr_t, const char*);
    const uintptr_t settings = mgr ? reinterpret_cast<FindFn>(mem::rva(e::function::SettingsManager_findByName))(mgr, "DebugRender") : 0;
    if (!settings) {
        // drawFrame reads Enable through the Impl's settings without a check: no settings, no renderer.
        setStatus("off: the game has no \"DebugRender\" settings");
        GN_LOG("debug render: not made: the game has no \"DebugRender\" settings (only the legacy renderer registers them)");
        return;
    }
    // Inline mode: text and spheres expanded in drawFrame, not a job (the job's buffers belong to the legacy renderer).
    mem::write<int32_t>(settings + k::DebugRenderSettings_InlineMode, 0);
    using AllocFn = void* (*)(void*, uint64_t, uint64_t);
    void* impl = reinterpret_cast<AllocFn>(mem::rva(e::function::Arena_allocAligned))(reinterpret_cast<void*>(mem::rva(e::global::GlobalArena)), 0xa0, 16);
    if (!impl) {
        setStatus("off: no memory from the game's arena");
        return;
    }
    std::memset(impl, 0, 0xa0);
    reinterpret_cast<void* (*)(void*)>(mem::rva(e::function::DebugRender2Impl_ctor))(impl);
    const uintptr_t vt = mem::rva(k::DebugRender2Impl_vtable);
    std::memcpy(impl, &vt, 8);
    reinterpret_cast<void (*)(void*)>(mem::rva(e::function::DebugRender2_registerGlobal))(impl);
    g_ours = global() != 0;
    const bool text = mem::readOr<uintptr_t>(mem::rva(e::global::unverified::DebugRenderer_fontIndexBuffer)) != 0;
    setStatus(g_ours ? std::string("on (made by GridNode") + (text ? "" : "; no text: the font buffer is not made") + ")" : "off: the global stayed empty");
    GN_LOG("debug render: DebugRender2 made (Impl %p, settings %llx, Enable %u); text %s", impl, static_cast<unsigned long long>(settings),
           mem::readOr<uint8_t>(settings + 0x28), text ? "possible" : "not possible (no font index buffer)");
}

void destroy() {
    if (g_ours.exchange(false)) {
        mem::write<uintptr_t>(mem::rva(e::global::DebugRender2_global), 0); // the Impl, wrapper and recorders stay in the arena
        GN_LOG("debug render: off (DebugRender2 global cleared)");
    }
    setStatus("off");
}

void cage(std::vector<Line>& out) {
    gn_transform t{};
    void* faith = localClientCharacter();
    if (!faith || !entityTransform(faith, t)) return;
    const float* p = t.trans;
    const float h = 0.5f, y0 = p[1], y1 = p[1] + 1.8f;
    const float c[4][2] = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        const uint8_t f = g_testFlags.load();
        out.push_back({{p[0] + c[i][0], y0, p[2] + c[i][1]}, {p[0] + c[j][0], y0, p[2] + c[j][1]}, 0xff00ff00u, f});
        out.push_back({{p[0] + c[i][0], y1, p[2] + c[i][1]}, {p[0] + c[j][0], y1, p[2] + c[j][1]}, 0xff00ff00u, f});
        out.push_back({{p[0] + c[i][0], y0, p[2] + c[i][1]}, {p[0] + c[i][0], y1, p[2] + c[i][1]}, 0xff0000ffu, f});
    }
}

} // namespace

// Render thread, every frame (ui.cpp's Present hook, before the overlay).
void debugDrawFrame() {
    const int want = g_want.exchange(0);
    if (want > 0) guardHook("debug render on", [] { create(); });
    if (want < 0) guardHook("debug render off", [] { destroy(); });
    if (!global()) return;
    std::vector<Line> lines;
    {
        std::lock_guard<std::mutex> lock(g_m);
        lines.swap(g_lines);
    }
    if (g_test.load()) cage(lines);
    if (lines.empty()) return;
    guardHook("debug render lines", [&] {
        void* ctx = reinterpret_cast<void* (*)()>(mem::rva(e::function::DebugRender2_getThreadContext))();
        if (!ctx) return;
        using VertsFn = uint8_t* (*)(void*, int32_t, uint32_t);
        const auto verts = reinterpret_cast<VertsFn>(mem::rva(e::function::DebugRender2_allocVertices));
        // Each command copies the recorder's state byte (**(ctx + 0x10)) as its flags when it is made
        // (DebugRender2_flushStaging; read in Ghidra 2026-11-20): 0x2 no culling, 0x4 depth test, 0x8 depth write.
        // Lines with flags are staged in their own run: the staged vertices before them are flushed with the byte as it
        // was, ours with ours, then the byte is put back.
        static const bool flagsOk = build::codeMatches("DebugRender2_flushStaging");
        const auto flushStaging = reinterpret_cast<void (*)(void*)>(mem::rva(e::function::DebugRender2_flushStaging));
        uint8_t* state = flagsOk ? reinterpret_cast<uint8_t*>(mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(ctx) + k::DebugRender2Recorder_State)) : nullptr;
        uint8_t probe = 0;
        if (state && !mem::read(reinterpret_cast<uintptr_t>(state), probe)) state = nullptr;
        std::stable_sort(lines.begin(), lines.end(), [](const Line& x, const Line& y) { return x.flags < y.flags; });
        for (size_t i = 0; i < lines.size();) {
            const uint8_t flags = lines[i].flags;
            size_t runEnd = i;
            while (runEnd < lines.size() && lines[runEnd].flags == flags) ++runEnd;
            const bool own = state && flags != *state;
            uint8_t was = 0;
            if (own) {
                flushStaging(ctx);
                was = *state;
                *state = flags;
            }
            while (i < runEnd) {
                const uint32_t n = static_cast<uint32_t>(std::min<size_t>(runEnd - i, 1024));
                uint8_t* v = verts(ctx, 1, n * 2); // Line3D: two vertices a line
                if (!v) {
                    i = runEnd;
                    break;
                }
                for (uint32_t j = 0; j < n; ++j, ++i) {
                    uint8_t* a = v + j * 0x40;
                    std::memset(a, 0, 0x40);
                    std::memcpy(a, lines[i].a, 12);
                    std::memcpy(a + 0x0c, &lines[i].abgr, 4);
                    std::memcpy(a + 0x20, lines[i].b, 12);
                    std::memcpy(a + 0x2c, &lines[i].abgr, 4);
                }
            }
            if (own) {
                flushStaging(ctx);
                *state = was;
            }
        }
        // The render engine publishes the render thread's recorder before it draws; whether the Present thread is that
        // thread is not known, so this one is published here (the game's own flush call).
        reinterpret_cast<void (*)(void*, uint32_t, uint64_t)>(mem::rva(e::function::DebugRender2_flushThread))(ctx, 0, 0);
        g_drawn.fetch_add(lines.size(), std::memory_order_relaxed);
        g_frames.fetch_add(1, std::memory_order_relaxed);
    });
}

// Any thread: one line for the next frame (world space; colour 0xAABBGGRR). Lines from several frames are kept up
// to 65,536; a mod draws its lines every frame.
int debugLine(const float* a, const float* b, uint32_t abgr) { return debugLineEx(a, b, abgr, 0); }

// API 42: with the command's flags (GN_DEBUG_NO_CULL 0x2, GN_DEBUG_DEPTH_TEST 0x4, GN_DEBUG_DEPTH_WRITE 0x8 with 0x4).
int debugLineEx(const float* a, const float* b, uint32_t abgr, uint32_t flags) {
    if (!a || !b || (flags & ~0x0eu)) return GN_ERR_ARGUMENT;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return GN_ERR_ARGUMENT;
    if (!global()) return GN_ERR_UNAVAILABLE;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_lines.size() >= 65536) return GN_ERR_FULL;
    g_lines.push_back({{a[0], a[1], a[2]}, {b[0], b[1], b[2]}, abgr, static_cast<uint8_t>(flags)});
    return GN_OK;
}

void debugDrawRelease() {
    // At unload the render thread may be gone or still drawing: the global is a pointer the game reads per frame,
    // and clearing it is what the getter checks first.
    if (g_ours.exchange(false)) {
        mem::write<uintptr_t>(mem::rva(e::global::DebugRender2_global), 0);
        GN_LOG("debug render: off (GridNode unloading)");
    }
}

// Console: "debugrender on | off | test | (nothing: status)".
std::string debugRenderCommand(const std::string& rest) {
    const size_t a = rest.find_first_not_of(' ');
    const std::string arg = a == std::string::npos ? std::string() : rest.substr(a);
    if (!_stricmp(arg.c_str(), "on")) {
        g_want = 1;
        return "debugrender: switching on at the next frame (see gridnode.log); debugrender test draws a cage around Faith";
    }
    if (!_stricmp(arg.c_str(), "off")) {
        g_want = -1;
        g_test = false;
        return "debugrender: off at the next frame";
    }
    if (!_stricmp(arg.c_str(), "depth")) {
        g_testFlags = g_testFlags.load() ? 0 : 0x04;
        return std::string("debugrender: test cage ") + (g_testFlags.load() ? "with depth test (hidden behind walls)" : "without depth test (over everything)");
    }
    if (!_stricmp(arg.c_str(), "test")) {
        g_test = !g_test.load();
        return std::string("debugrender: test cage ") + (g_test.load() ? "on (green square at her feet and head, blue posts)" : "off");
    }
    std::lock_guard<std::mutex> lock(g_m);
    char b[200];
    snprintf(b, sizeof(b), "debugrender: %s; %llu lines drawn in %llu frames (debugrender on | off | test | depth)", g_status.c_str(),
             static_cast<unsigned long long>(g_drawn.load()), static_cast<unsigned long long>(g_frames.load()));
    return b;
}

} // namespace gn::host
