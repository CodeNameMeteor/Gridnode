// trainer: a small speedrun practice tool, and GridNode's worked example of a mod with a window.
//
//   - Save and load positions (5 slots), from the window or with hotkeys (F5 save, F6 load, F7 /
//     F8 previous / next slot). Loading uses the game's own teleport on the server thread.
//   - A speed and position readout, in the window and as a small HUD that stays on screen.
//   - Height above the ground, from a ray cast straight down (server thread).
//   - Slots are forgotten when the level changes.
//
// Threads, which every GridNode mod has to keep apart:
//   render thread  gn::ui::onDraw    draws; never calls the game; reads state under g_m
//   client thread  gn::onClientTick  reads Faith's position, measures speed, polls the hotkeys
//   server thread  gn::runOnServer   saves and teleports (the server owns the character)
//                  gn::onServerTick  casts a ray down for the height above the ground
//   GridNode       gn::onLevel       clears the slots
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "gridnode/ui.hpp"

namespace {

constexpr int kSlots = 5;

struct Slot {
    bool used = false;
    gn::Transform at{};
    std::string level;
};

std::mutex g_m;          // guards g_slots, g_status
Slot g_slots[kSlots];
std::string g_status;    // last thing that happened, shown in the window
std::atomic<int> g_slot{0};
std::atomic<bool> g_hud{true};

// Measured on the client thread, shown on the render thread.
std::atomic<float> g_speed{0}, g_speedH{0}, g_topSpeed{0};
std::atomic<float> g_px{0}, g_py{0}, g_pz{0};
std::atomic<bool> g_haveFaith{false};
std::atomic<float> g_ground{-1.f}; // metres from Faith's feet to the ground below (-1: none found)

void status(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gn::log("%s", buf);
    std::lock_guard<std::mutex> lock(g_m);
    g_status = buf;
}

// Server thread.
void save(int slot) {
    gn::runOnServer([slot] {
        void* faith = gn::localServerCharacter();
        gn::Transform t;
        if (!faith || !gn::transformOf(faith, t)) {
            status("Save: no character to save (in a menu or loading?)");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_slots[slot] = {true, t, gn::levelName()};
        }
        status("Saved slot %d at (%.1f, %.1f, %.1f)", slot + 1, t.trans[0], t.trans[1], t.trans[2]);
    });
}

void load(int slot) {
    gn::runOnServer([slot] {
        Slot s;
        {
            std::lock_guard<std::mutex> lock(g_m);
            s = g_slots[slot];
        }
        if (!s.used) {
            status("Load: slot %d is empty", slot + 1);
            return;
        }
        void* faith = gn::localServerCharacter();
        if (!faith) {
            status("Load: no character to move");
            return;
        }
        const int r = gn::teleport(faith, s.at);
        if (r == GN_OK) status("Loaded slot %d", slot + 1);
        else status("Load: teleport failed (%d)", r);
    });
}

bool gameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Client thread, every tick: hotkeys (edge-triggered) and the speed readout.
void clientTick(float dt) {
    static bool was[4] = {};
    const int keys[4] = {VK_F5, VK_F6, VK_F7, VK_F8};
    const bool focused = gameFocused() && !gn::ui::menuOpen();
    for (int i = 0; i < 4; ++i) {
        const bool down = focused && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !was[i]) {
            const int slot = g_slot.load();
            if (i == 0) save(slot);
            if (i == 1) load(slot);
            if (i == 2) g_slot = (slot + kSlots - 1) % kSlots;
            if (i == 3) g_slot = (slot + 1) % kSlots;
        }
        was[i] = down;
    }

    static gn::Vec3 last{};
    static bool haveLast = false;
    static int reported = -1; // what the log last said: 0 found, 1 no character, 2 no position
    gn::Vec3 p;
    void* faith = gn::localClientCharacter();
    const int state = !faith ? 1 : !gn::positionOf(faith, p) ? 2 : 0;
    static int logged = 0;
    if (state != reported && logged < 20) { // a few lines, not one per tick if it flickers
        ++logged;
        reported = state;
        gn::log(state == 0 ? "client: Faith found (%p)" : state == 1 ? "client: no character (%p)" : "client: Faith %p has no readable position",
                faith);
    }
    if (state != 0) {
        haveLast = false;
        g_haveFaith = false;
        return;
    }
    if (haveLast && dt > 0.f) {
        const float dx = p.x - last.x, dy = p.y - last.y, dz = p.z - last.z;
        const float v = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
        const float vh = std::sqrt(dx * dx + dz * dz) / dt;
        if (v < 200.f) { // a teleport is not speed
            const float k = 0.25f; // smoothing
            g_speed = g_speed + (v - g_speed) * k;
            g_speedH = g_speedH + (vh - g_speedH) * k;
            if (g_speedH > g_topSpeed) g_topSpeed = g_speedH.load();
        }
    }
    last = p;
    haveLast = true;
    g_px = p.x;
    g_py = p.y;
    g_pz = p.z;
    g_haveFaith = true;
}

// Server thread, every tick: how high above the ground Faith is (a ray straight down). The first
// hit can be her own capsule, so a hit above her feet casts again from just below it.
void serverTick(float) {
    void* faith = gn::localServerCharacter();
    gn::Vec3 p;
    if (!faith || !gn::positionOf(faith, p)) {
        g_ground = -1.f;
        return;
    }
    gn::Vec3 from{p.x, p.y + 0.5f, p.z};
    const gn::Vec3 to{p.x, p.y - 200.f, p.z};
    for (int i = 0; i < 3; ++i) {
        gn::RayHit hit;
        if (!gn::raycast(from, to, hit)) break;
        if (hit.position[1] <= p.y + 0.05f) {
            g_ground = p.y - hit.position[1];
            return;
        }
        from.y = hit.position[1] - 0.05f; // her own capsule (or something above her feet): look below it
    }
    g_ground = -1.f;
}

// Render thread.
void hud() {
    if (!g_hud || !g_haveFaith) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 12.f, vp->WorkPos.y + 12.f), ImGuiCond_Always, ImVec2(1.f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##trainer_hud", nullptr, f)) {
        ImGui::Text("%5.1f m/s", g_speedH.load());
        if (g_ground.load() > 0.3f) ImGui::Text("%5.1f m up", g_ground.load());
        ImGui::TextDisabled("slot %d  F5 save  F6 load", g_slot.load() + 1);
    }
    ImGui::End();
}

void window() {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 20, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(30, ImGui::GetFontSize() * 20), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Trainer")) {
        ImGui::End();
        return;
    }
    if (g_haveFaith) {
        ImGui::Text("Position  %.2f, %.2f, %.2f", g_px.load(), g_py.load(), g_pz.load());
        ImGui::Text("Speed     %.1f m/s (%.1f horizontal)", g_speed.load(), g_speedH.load());
        ImGui::Text("Top speed %.1f m/s", g_topSpeed.load());
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) g_topSpeed = 0.f;
        if (g_ground.load() >= 0.f) ImGui::Text("Ground    %.2f m below (ray cast)", g_ground.load());
        else ImGui::TextDisabled("Ground    none within 200 m");
    } else {
        ImGui::TextDisabled("No character (in a menu or loading).");
    }
    ImGui::Separator();

    Slot slots[kSlots];
    std::string last;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (int i = 0; i < kSlots; ++i) slots[i] = g_slots[i];
        last = g_status;
    }
    if (ImGui::BeginTable("slots", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < kSlots; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::RadioButton("##sel", g_slot.load() == i)) g_slot = i;
            ImGui::TableNextColumn();
            if (slots[i].used)
                ImGui::Text("%d: %.0f, %.0f, %.0f", i + 1, slots[i].at.trans[0], slots[i].at.trans[1], slots[i].at.trans[2]);
            else
                ImGui::TextDisabled("%d: empty", i + 1);
            ImGui::TableNextColumn();
            if (ImGui::Button("Save")) save(i);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!slots[i].used);
            if (ImGui::Button("Load")) load(i);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    bool hudOn = g_hud.load();
    if (ImGui::Checkbox("Speed HUD", &hudOn)) g_hud = hudOn;
    ImGui::TextDisabled("F5 save, F6 load, F7/F8 slot (menu closed)");
    if (!last.empty()) ImGui::TextWrapped("%s", last.c_str());
    ImGui::End();
}

} // namespace

GN_MOD("trainer") {
    if (!gn::ui::onDraw([](bool menuOpen) {
            hud();
            if (menuOpen) window();
        }))
        gn::log("no overlay (disabled in gridnode.ini, or another Dear ImGui): hotkeys only");
    gn::onClientTick(clientTick);
    gn::onServerTick(serverTick);
    gn::onLevel([](const char* level, bool loaded) {
        if (!loaded) return;
        std::lock_guard<std::mutex> lock(g_m);
        for (Slot& s : g_slots)
            if (s.used && s.level != level) s = Slot{};
        g_topSpeed = 0.f;
    });
    gn::log("ready: F5 saves, F6 loads (slot %d)", g_slot.load() + 1);
    return true;
}
