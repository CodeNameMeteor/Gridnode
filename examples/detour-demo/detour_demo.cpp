// detour-demo: the API 16 example. Put detour_demo.dll in the game's mods/ folder.
//
//   - a manifest (GN_MOD_INFO_DEPS): its id, version, and an optional dependency on the MEC trainer,
//     so it loads after it when it is there
//   - function detours (gn::hookFunction): calls to two of the game's functions counted, the game's
//     behaviour kept (each detour calls its original). EntityBus_fireEvent is detoured twice, to show
//     two detours on one function chained (GridNode menu, Hooks)
//   - a function from another mod (gn::importFn): the trainer's god mode, read through its export
//   - a button that crashes the game on purpose, to see the crash notes in gridnode.log
//   - declared settings (API 17): no UI code for them here, GridNode draws them (Mod settings)
//   - API 36: a class's virtual hooked by name (gn::VirtualHook on Faith's setVisible, with its
//     subclasses), a field handle (gn::Field), any entity's components, the game's data folder, and
//     a view of Faith's class in the inspector (gn::ui::typeView)
// Its window is in the GridNode menu (INSERT).

// It runs on any GridNode from API 17 (declared settings) on: the API 36 calls are checked with gn::has.
#define GN_MOD_MIN_API 17

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/ui.hpp"

namespace f = gn::engine::function;

GN_MOD_INFO_DEPS("detour-demo", "Detour demo", "1.1.0", "GridNode", "Counts calls to two game functions through function detours",
                 "?mec-trainer");

namespace {

// Signatures from db/annotations (EntityBus_fireEvent, EntityBus_postEvent).
using FireEvent = void (*)(void* bus, void* sourcePeer, void* event);
using PostEvent = void (*)(void* busPeer, void* source, uint32_t eventId, void* a4);
FireEvent g_fireA = nullptr, g_fireB = nullptr;
PostEvent g_post = nullptr;
std::atomic<uint64_t> g_fires{0}, g_firesB{0}, g_posts{0};
// Declared settings: on the GridNode menu's Mod settings page and in mods\detour-demo.ini.
gn::setting::Setting g_countPosts, g_window, g_rateEvery, g_unit, g_title;
std::atomic<bool> g_countPostsOn{true}; // read in the detour: an atomic, not a call per event

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
    if (g_countPostsOn.load(std::memory_order_relaxed)) g_posts.fetch_add(1, std::memory_order_relaxed);
    g_post(busPeer, source, id, a4);
}


// API 36. setVisible is annotated on PamClientCharacterEntity (vtable +0x3d8): found by name.
gn::VirtualHook<void(void* self, bool visible)> g_setVisible;
std::atomic<uint64_t> g_setVisibleCalls{0};
std::atomic<int> g_lastVisible{-1};
void setVisible(void* self, bool visible) {
    g_setVisibleCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastVisible.store(visible ? 1 : 0, std::memory_order_relaxed);
    if (auto original = g_setVisible.original(self)) original(self, visible);
}
const gn::Field<float> kTimeScale("GameTimeSettings", "TimeScale");
std::mutex g_compM;
std::vector<std::string> g_components; // (g_compM) Faith's components, read on the client thread
bool g_virtualHooked = false;

using GodMode = int (*)();
GodMode g_trainerGod = nullptr;
bool g_hooked[3] = {};

} // namespace

GN_MOD("detour-demo") {
    using namespace gn::setting;
    g_window = addBool("window", "Show the window", true, {"The Detour demo window, while the menu is open", "Window"});
    g_title = addText("title", "Window title", "Detour demo", {"Its title bar", "Window"});
    g_countPosts = addBool("count_posts", "Count postEvent calls", true, {"Off: the postEvent detour only passes calls on", "Counting"});
    g_rateEvery = addFloat("rate_every", "Rate over (s)", 1.f, 0.25f, 5.f, {"How long the calls per second are averaged over", "Counting"});
    g_unit = addChoice("unit", "Rate as", 0, {"per second", "per frame (60 Hz)"}, {"How the rates are shown", "Counting"});
    g_countPostsOn = g_countPosts.on();
    onChange(nullptr, [](const char* id) {
        g_countPostsOn = g_countPosts.on();
        gn::log("setting %s is now %s", id, gn::setting::Setting(id).text().c_str());
    });
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
    // API 36 (optional: this mod still loads on an older GridNode without them).
    g_virtualHooked = g_setVisible.install("PamClientCharacterEntity", "setVisible", &setVisible);
    gn::log("hook_virtual PamClientCharacterEntity::setVisible: %s; data folder: %s", g_virtualHooked ? "ok" : "FAILED",
            gn::dataPath().c_str());
    gn::onClientTick([](float) { // components are read on the entity's realm thread
        static int every = 0;
        if (++every % 30) return;
        std::vector<std::string> names;
        if (void* faith = gn::localClientCharacter())
            gn::entityComponents(faith, [&](void* c, gn::Type t) {
                char line[160];
                snprintf(line, sizeof(line), "%s  %p", t.name(), c);
                names.push_back(line);
            });
        std::lock_guard<std::mutex> lock(g_compM);
        g_components = std::move(names);
    });
    gn::ui::typeView("PamClientCharacterEntity", [](void* object, gn::Type type) {
        ImGui::Text("detour-demo's view of a %s at %p", type.name(), object);
        ImGui::Text("setVisible calls (all characters): %llu, last %d", static_cast<unsigned long long>(g_setVisibleCalls.load()), g_lastVisible.load());
    });
    gn::ui::onDraw([](bool menuOpen) {
        if (!menuOpen || !g_window.on()) return;
        static auto last = std::chrono::steady_clock::now();
        static uint64_t f0 = 0, p0 = 0;
        static double fireRate = 0, postRate = 0;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        if (dt >= g_rateEvery.value()) {
            const uint64_t f1 = g_fires.load(), p1 = g_posts.load();
            fireRate = (f1 - f0) / dt, postRate = (p1 - p0) / dt;
            f0 = f1, p0 = p1, last = now;
        }
        const std::string title = g_title.text() + "###detour-demo"; // the id stays when the title changes
        ImGui::Begin(title.c_str());
        const bool perFrame = g_unit.integer() == 1;
        const double k = perFrame ? 1.0 / 60.0 : 1.0;
        const char* unit = perFrame ? "/frame" : "/s";
        ImGui::Text("EntityBus_fireEvent: %llu calls (%.0f%s); second detour saw %llu", static_cast<unsigned long long>(g_fires.load()), fireRate * k,
                    unit, static_cast<unsigned long long>(g_firesB.load()));
        ImGui::Text("EntityBus_postEvent: %llu calls (%.0f%s)%s", static_cast<unsigned long long>(g_posts.load()), postRate * k, unit,
                    g_countPostsOn ? "" : " (not counting)");
        if (ImGui::CollapsingHeader("API 36", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("setVisible (hook_virtual): %s, %llu calls, last visible=%d", g_virtualHooked ? "hooked" : "NOT hooked",
                        static_cast<unsigned long long>(g_setVisibleCalls.load()), g_lastVisible.load());
            gn::Object settings = gn::settings("GameTimeSettings");
            ImGui::Text("GameTimeSettings.TimeScale (gn::Field, +0x%x): %.3f", kTimeScale.offset(), kTimeScale.get(settings.ptr()));
            ImGui::Text("Data folder: %s", gn::dataPath().c_str());
            std::lock_guard<std::mutex> lock(g_compM);
            ImGui::Text("Faith's components (entity_components): %d", static_cast<int>(g_components.size()));
            for (const std::string& c : g_components) ImGui::BulletText("%s", c.c_str());
        }
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
