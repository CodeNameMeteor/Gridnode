// detour-demo: the API 16 example. Put detour_demo.dll in the game's mods/ folder.
//
//   - a manifest (GN_MOD_INFO_DEPS): its id, version, and an optional dependency on the MEC trainer,
//     so it loads after it when it is there
//   - function detours (gn::hookFunction): calls to two of the game's functions counted, the game's
//     behaviour kept (each detour calls its original). EntityBus_fireEvent is detoured twice, to show
//     two detours on one function chained (GridNode menu, Hooks)
//   - a function from another mod (gn::importFn): the trainer's god mode, read through its export
//   - a button that crashes the game on purpose, to see the crash notes in gridnode.log
// Its window is in the GridNode menu (INSERT).

#include <atomic>
#include <chrono>

#include "gridnode/engine.hpp"
#include "gridnode/ui.hpp"

namespace f = gn::engine::function;

GN_MOD_INFO_DEPS("detour-demo", "Detour demo", "1.0.0", "GridNode", "Counts calls to two game functions through function detours",
                 "?mec-trainer");

namespace {

// Signatures from db/annotations (EntityBus_fireEvent, EntityBus_postEvent).
using FireEvent = void (*)(void* bus, void* sourcePeer, void* event);
using PostEvent = void (*)(void* busPeer, void* source, uint32_t eventId, void* a4);
FireEvent g_fireA = nullptr, g_fireB = nullptr;
PostEvent g_post = nullptr;
std::atomic<uint64_t> g_fires{0}, g_firesB{0}, g_posts{0};

// Detours run on whatever thread calls the function: only atomics here.
void fireA(void* bus, void* source, void* event) {
    g_fires.fetch_add(1, std::memory_order_relaxed);
    g_fireA(bus, source, event);
}
void fireB(void* bus, void* source, void* event) {
    g_firesB.fetch_add(1, std::memory_order_relaxed);
    g_fireB(bus, source, event);
}
void post(void* busPeer, void* source, uint32_t id, void* a4) {
    g_posts.fetch_add(1, std::memory_order_relaxed);
    g_post(busPeer, source, id, a4);
}

using GodMode = int (*)();
GodMode g_trainerGod = nullptr;
bool g_hooked[3] = {};

} // namespace

GN_MOD("detour-demo") {
    g_hooked[0] = gn::hookFunction(f::EntityBus_fireEvent, &fireA, &g_fireA);
    g_hooked[1] = gn::hookFunction(f::EntityBus_fireEvent, &fireB, &g_fireB); // on top of fireA
    g_hooked[2] = gn::hookFunction(f::EntityBus_postEvent, &post, &g_post);
    std::string trainer;
    if (gn::modVersion("mec-trainer", &trainer)) {
        g_trainerGod = gn::importFn<GodMode>("mec-trainer", "godMode");
        gn::log("the MEC trainer %s is loaded; its godMode %s", trainer.c_str(), g_trainerGod ? "imported" : "is not offered");
    } else {
        gn::log("the MEC trainer is not loaded");
    }
    gn::log("detours: fireEvent %s / %s, postEvent %s", g_hooked[0] ? "ok" : "FAILED", g_hooked[1] ? "ok" : "FAILED", g_hooked[2] ? "ok" : "FAILED");
    gn::ui::onDraw([](bool menuOpen) {
        if (!menuOpen) return;
        static auto last = std::chrono::steady_clock::now();
        static uint64_t f0 = 0, p0 = 0;
        static double fireRate = 0, postRate = 0;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        if (dt >= 1.0) {
            const uint64_t f1 = g_fires.load(), p1 = g_posts.load();
            fireRate = (f1 - f0) / dt, postRate = (p1 - p0) / dt;
            f0 = f1, p0 = p1, last = now;
        }
        ImGui::Begin("Detour demo");
        ImGui::Text("EntityBus_fireEvent: %llu calls (%.0f/s); second detour saw %llu", static_cast<unsigned long long>(g_fires.load()), fireRate,
                    static_cast<unsigned long long>(g_firesB.load()));
        ImGui::Text("EntityBus_postEvent: %llu calls (%.0f/s)", static_cast<unsigned long long>(g_posts.load()), postRate);
        if (g_trainerGod) ImGui::Text("Trainer god mode (through its export): %s", g_trainerGod() ? "on" : "off");
        else ImGui::TextDisabled("The MEC trainer is not loaded (or does not offer godMode).");
        if (ImGui::Button("Unload this mod")) gn::unloadSelf();
        // For checking the crash notes in gridnode.log (API 16): a null read on the render thread, in this mod.
        static bool sure = false;
        ImGui::Checkbox("I want to crash the game", &sure);
        ImGui::BeginDisabled(!sure);
        if (ImGui::Button("Crash it (tests the crash notes)")) {
            volatile int* p = nullptr;
            gn::log("crashing on purpose: %d", *p);
        }
        ImGui::EndDisabled();
        ImGui::End();
    });
    return g_hooked[0] && g_hooked[1] && g_hooked[2];
}

GN_MOD_UNLOAD() { return true; }
