// mec-trainer: MEC_Trainer (by the GridNode author) ported onto GridNode.
//
// The original read and wrote the game through raw pointer chains and drew its own ImGui by hooking
// Present. This port uses GridNode for all of it:
//   - Faith's movement, wall counters, ground and speed: gn::movement / gn::setMovement
//   - jumping: gn::jump() (the game's jump input, whatever it turns into)
//   - movement tuning by name: gn::setTuning("MaxRunSpeed", ...) - refused until verified
//   - time scale, sim rate, post-processing, god mode: settings by name (GameTimeSettings, ...)
//   - time of day: gn::setTimeOfDay, the level's own clock (checked by type)
//   - save / load position: the game's own teleport on the server thread
//   - noclip: gn::setPosition every client tick (no teleport, so the camera stays free)
//   - bunny hop: ray cast + movement state on the server tick, then gn::jump()
//   - the window lives in GridNode's overlay (INSERT); the HUD stays on screen
//
// Hotkeys (as in MEC_Trainer; ignored while the GridNode menu is open or the game is not focused):
//   1 god mode   2 noclip   3 no stumble   4 save position   5 load position
//   F arm the bunny hop   E / R noclip slower / faster   (noclip: WASD, Space up, Shift / C down)
//
// Threads: render (window, HUD: reads atomics, posts work), client tick (movement features,
// hotkeys), server tick (noclip and teleports). Faith's state is read on the client thread.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>

#include "gridnode/ui.hpp"

namespace {

// ---- settings (render thread writes, ticks read) ------------------------------------------------------
std::atomic<bool> g_god{false}, g_noclip{false}, g_noStumble{false}, g_sameWall{false}, g_infClimbs{false},
    g_infRuns{false}, g_fastLoads{false}, g_freezeTime{false}, g_hud{true}, g_cheatList{true};
std::atomic<float> g_noclipSpeed{0.5f}, g_frozenTime{0.f}, g_timeScale{1.f};

// ---- what the client tick saw (render thread shows it) ------------------------------------------------
std::atomic<bool> g_haveFaith{false};
std::atomic<float> g_px{0}, g_py{0}, g_pz{0}, g_speed{0}, g_lastGroundY{0}, g_timeOfDay{-1};
std::atomic<int> g_state{0};
std::atomic<bool> g_loading{false};

// ---- save / load ---------------------------------------------------------------------------------------
std::mutex g_m;
bool g_saved = false;
gn::Transform g_savedAt{};
float g_savedSpeed = 0.f;
std::string g_status;

void status(const char* s) {
    gn::log("%s", s);
    std::lock_guard<std::mutex> lock(g_m);
    g_status = s;
}

bool gameFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void setGodMode(bool on) {
    gn::runOnClient([on] {
        if (!gn::settings("GameSettings").set("IsGodMode", on)) gn::log("god mode: GameSettings.IsGodMode not available");
    });
}

void savePosition() {
    gn::runOnServer([] {
        void* faith = gn::localServerCharacter();
        gn::Transform t;
        if (!faith || !gn::transformOf(faith, t)) return status("Save: no Faith (menu or loading?)");
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_saved = true;
            g_savedAt = t;
            g_savedSpeed = g_speed.load();
        }
        char b[96];
        snprintf(b, sizeof(b), "Saved (%.1f, %.1f, %.1f)", t.trans[0], t.trans[1], t.trans[2]);
        status(b);
    });
}

void loadPosition() {
    gn::runOnServer([] {
        gn::Transform t;
        float speed;
        {
            std::lock_guard<std::mutex> lock(g_m);
            if (!g_saved) return status("Load: nothing saved");
            t = g_savedAt;
            speed = g_savedSpeed;
        }
        void* faith = gn::localServerCharacter();
        if (!faith || gn::teleport(faith, t) != GN_OK) return status("Load: teleport refused");
        gn::runOnClient([speed] {
            gn::Movement m{};
            m.last_ground_y = -2000.f; // no fall damage from where Faith was
            m.speed = speed;           // keep the saved run speed
            gn::setMovement(m, GN_MOVE_LAST_GROUND_Y | GN_MOVE_SPEED);
        });
        status("Loaded");
    });
}

// ---- noclip: Faith's position written directly every client tick (as MEC_Trainer did), so the camera
// stays free; she flies where the camera looks. -----------------------------------------------------------
std::atomic<bool> g_noclipActive{false};
float g_noclipPos[3] = {}; // client thread only

void noclipClient(const gn::Movement& m, bool keys) {
    float delta[3] = {};
    if (keys) {
        const float* f = m.camera_forward; // includes up / down: W flies where the camera points
        // Right of the camera, flat on the ground.
        float rx = f[2], rz = -f[0];
        const float len = std::sqrt(rx * rx + rz * rz);
        if (len > 1e-4f) rx /= len, rz /= len;
        if (GetAsyncKeyState('W') & 0x8000) delta[0] += f[0], delta[1] += f[1], delta[2] += f[2];
        if (GetAsyncKeyState('S') & 0x8000) delta[0] -= f[0], delta[1] -= f[1], delta[2] -= f[2];
        if (GetAsyncKeyState('D') & 0x8000) delta[0] += rx, delta[2] += rz;
        if (GetAsyncKeyState('A') & 0x8000) delta[0] -= rx, delta[2] -= rz;
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) delta[1] += 0.6f;
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState('C') & 0x8000)) delta[1] -= 0.6f;
        if (GetAsyncKeyState('R') & 0x8000) g_noclipSpeed = std::min(g_noclipSpeed.load() + 0.05f, 10.f);
        if (GetAsyncKeyState('E') & 0x8000) g_noclipSpeed = std::max(g_noclipSpeed.load() - 0.05f, 0.1f);
    }
    for (int i = 0; i < 3; ++i) g_noclipPos[i] += delta[i] * g_noclipSpeed.load();
    gn::setPosition({g_noclipPos[0], g_noclipPos[1], g_noclipPos[2]});
    // Keep Faith out of falling and wall states while she floats.
    gn::Movement w{};
    w.state = 2;
    w.speed = 0.f;
    w.last_ground_y = -2000.f;
    gn::setMovement(w, GN_MOVE_STATE | GN_MOVE_SPEED | GN_MOVE_LAST_GROUND_Y | GN_MOVE_WALL_COUNT | GN_MOVE_WALL_CLIMBS |
                           GN_MOVE_WALL_RUNS);
}

// ---- bunny hop -------------------------------------------------------------------------------------------
// The wall run jump window: after jumping off a wall run Faith touches the ground for a few frames
// while her state still says jumping (8), and a jump then keeps her speed. Armed with F (once) or
// always (Auto), the server tick casts a ray down every tick and jumps the moment both are true.
std::atomic<bool> g_bhopArmed{false}, g_bhopAuto{false};
std::atomic<float> g_bhopTolerance{0.10f}; // metres between her feet and the ground that count as "on it"
std::atomic<float> g_groundDist{-1.f};     // last ray cast (-1: nothing below)
std::atomic<int> g_bhops{0};
DWORD g_bhopArmedAt = 0, g_lastHop = 0;

void serverTick(float) {
    void* faith = gn::localServerCharacter();
    gn::Vec3 p;
    if (!faith || !gn::positionOf(faith, p)) return;
    gn::RayHit hit;
    const bool hitGround = gn::raycast({p.x, p.y + 0.5f, p.z}, {p.x, p.y - 3.f, p.z}, hit);
    const float dist = hitGround ? std::max(0.f, p.y - hit.position[1]) : -1.f;
    g_groundDist = dist;
    if (!g_bhopArmed && !g_bhopAuto) return;
    if (g_bhopArmed && GetTickCount() - g_bhopArmedAt > 5000) g_bhopArmed = false; // armed for 5 s
    gn::Movement m;
    if (!gn::movement(m) || m.state != 8 || dist < 0.f || dist > g_bhopTolerance) return;
    if (GetTickCount() - g_lastHop < 250) return; // one jump per window
    g_lastHop = GetTickCount();
    gn::jump();
    g_bhops.fetch_add(1);
    g_bhopArmed = false;
}

void bhopArm() {
    g_bhopArmed = true;
    g_bhopArmedAt = GetTickCount();
}

// ---- client tick -----------------------------------------------------------------------------------------
void clientTick(float) {
    const gn::GameState gs = gn::gameState();
    g_loading = gs.loading != 0;
    g_timeOfDay = gs.time_of_day;

    // Fast loads: let the simulation run flat out, but only while loading.
    static bool fast = false;
    static float simFps = 30.f;
    const bool wantFast = g_fastLoads && gs.loading;
    if (wantFast != fast) {
        fast = wantFast;
        gn::Object ts = gn::settings("GameTimeSettings");
        if (fast) ts.get("MaxSimFps", simFps); // put back exactly what it was
        ts.set("MaxSimFps", fast ? 50000.f : simFps);
        ts.set("TimeScale", fast ? 3000.f : g_timeScale.load());
    }
    if (g_freezeTime) gn::setTimeOfDay(g_frozenTime);

    void* faith = gn::localClientCharacter();
    gn::Vec3 p;
    gn::Movement m{};
    const bool have = faith && gn::positionOf(faith, p) && gn::movement(m);
    g_haveFaith = have;
    if (!have) {
        g_noclipActive = false;
        return;
    }
    g_px = p.x, g_py = p.y, g_pz = p.z;
    g_speed = m.speed;
    g_state = m.state;
    g_lastGroundY = m.last_ground_y;

    // Hotkeys (edge-triggered).
    const bool keys = gameFocused() && !gn::ui::menuOpen();
    static bool was[6] = {};
    const int vk[6] = {'1', '2', '3', '4', '5', 'F'};
    for (int i = 0; i < 6; ++i) {
        const bool down = keys && (GetAsyncKeyState(vk[i]) & 0x8000) != 0;
        if (down && !was[i]) {
            switch (i) {
            case 0: g_god = !g_god; setGodMode(g_god); break;
            case 1: g_noclip = !g_noclip; if (g_noclip && !g_god) { g_god = true; setGodMode(true); } break;
            case 2: g_noStumble = !g_noStumble; break;
            case 3: savePosition(); break;
            case 4: loadPosition(); break;
            case 5: bhopArm(); break;
            }
        }
        was[i] = down;
    }

    // Noclip.
    static int g_noclipGrace = 0;
    if (g_noclip && !g_noclipActive) {
        g_noclipPos[0] = p.x, g_noclipPos[1] = p.y, g_noclipPos[2] = p.z;
        g_noclipActive = true;
    } else if (!g_noclip && g_noclipActive) {
        // Let go where she is: standing, still, and any fall counted from here.
        g_noclipActive = false;
        gn::Movement w{};
        w.state = 2;
        w.last_ground_y = p.y;
        gn::setMovement(w, GN_MOVE_STATE | GN_MOVE_SPEED | GN_MOVE_LAST_GROUND_Y);
        g_noclipGrace = 300; // and keep it that way until she lands (at most 10 s)
    }
    if (g_noclipGrace > 0) {
        // The fall is measured from last_ground_y: keep it at her height until she is on the ground.
        g_noclipGrace = m.on_ground ? 0 : g_noclipGrace - 1;
        gn::Movement w{};
        w.last_ground_y = p.y;
        gn::setMovement(w, GN_MOVE_LAST_GROUND_Y);
    }
    if (g_noclipActive) {
        noclipClient(m, keys);
        return;
    }

    // No stumble: a fall only counts from where Faith is now.
    if (g_noStumble && m.last_ground_y - p.y >= 1.f && m.state != 3 && m.state != 4) {
        gn::Movement w{};
        w.last_ground_y = p.y;
        gn::setMovement(w, GN_MOVE_LAST_GROUND_Y);
    }

    // Walls.
    gn::Movement w = m;
    uint32_t fields = 0;
    if (g_infClimbs && m.wall_climbs) {
        w.wall_climbs = 0, fields |= GN_MOVE_WALL_CLIMBS;
        if (m.wall_count > 2) w.wall_count = 2, fields |= GN_MOVE_WALL_COUNT;
    }
    if (g_infRuns && m.wall_runs) {
        w.wall_runs = 0, fields |= GN_MOVE_WALL_RUNS;
        if (m.wall_count > 2) w.wall_count = 2, fields |= GN_MOVE_WALL_COUNT;
    }
    if (g_sameWall && m.wall_count) w.wall_count = 0, fields |= GN_MOVE_WALL_COUNT;
    if (fields) gn::setMovement(w, fields);

}

// ---- window -------------------------------------------------------------------------------------------
void movementTab() {
    if (ImGui::Button("Reset all to default")) {
        gn::Tuning t;
        for (uint32_t i = 0; i < gn::tuningCount(); ++i)
            if (gn::tuningAt(i, t)) gn::setTuning(t.name, t.default_value);
    }
    ImGui::SameLine();
    if (ImGui::Button("Randomize")) {
        static std::mt19937 rng{std::random_device{}()};
        gn::Tuning t;
        for (uint32_t i = 0; i < gn::tuningCount(); ++i) {
            if (!gn::tuningAt(i, t)) continue;
            const float d = t.default_value;
            std::uniform_real_distribution<float> dist(d > 0.001f ? d * 0.5f : 0.1f, d > 0.001f ? d * 2.5f : 15.f);
            gn::setTuning(t.name, dist(rng));
        }
    }
    std::string group;
    bool open = false;
    gn::Tuning t;
    for (uint32_t i = 0; i < gn::tuningCount(); ++i) {
        if (!gn::tuningAt(i, t)) continue;
        if (group != t.group) {
            group = t.group;
            open = ImGui::CollapsingHeader(t.group, group == "Running" ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        }
        if (!open) continue;
        ImGui::BeginDisabled(!t.available);
        float v = t.value;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
        if (ImGui::InputFloat(t.name, &v, 0.f, 0.f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) gn::setTuning(t.name, v);
        ImGui::EndDisabled();
        if (!t.available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Not verified yet: load into a level (GridNode only writes it once it has seen the default %.3f there).",
                              t.default_value);
        else if (v != t.default_value) {
            ImGui::SameLine();
            ImGui::TextDisabled("(default %.3g)", t.default_value);
        }
    }
}

void worldTab() {
    float raw = g_freezeTime ? g_frozenTime.load() : g_timeOfDay.load();
    if (raw >= 0.f) {
        float norm = std::fmod(raw, 86400.f);
        if (norm < 0.f) norm += 86400.f;
        float hours = norm / 3600.f;
        if (ImGui::SliderFloat("Time of day", &hours, 0.f, 23.99f, "%.2f h")) {
            g_frozenTime = hours * 3600.f;
            const float s = hours * 3600.f;
            gn::runOnClient([s] { gn::setTimeOfDay(s); });
        }
        bool freeze = g_freezeTime;
        if (ImGui::Checkbox("Freeze time of day", &freeze)) {
            if (freeze) g_frozenTime = norm;
            g_freezeTime = freeze;
        }
    } else {
        ImGui::TextDisabled("No time of day clock in this level");
    }
    float scale = g_timeScale;
    if (ImGui::SliderFloat("Game time scale", &scale, 0.1f, 10.f)) {
        g_timeScale = scale;
        gn::runOnClient([scale] { gn::settings("GameTimeSettings").set("TimeScale", scale); });
    }
    ImGui::SeparatorText("Post-processing");
    static const char* const kPost[3][2] = {{"Bloom", "BloomEnable"}, {"Blur", "BlurEnable"}, {"Vignette", "VignetteEnable"}};
    gn::Object pp = gn::settings("GlobalPostProcessSettings");
    for (const auto& e : kPost) {
        bool on = false;
        if (!pp.get(e[1], on)) continue;
        if (ImGui::Checkbox(e[0], &on)) {
            const char* field = e[1];
            gn::runOnClient([field, on] { gn::settings("GlobalPostProcessSettings").set(field, on); });
        }
    }
}

void trainerTab() {
    bool v;
    v = g_god;
    if (ImGui::Checkbox("God mode  [1]", &v)) g_god = v, setGodMode(v);
    v = g_noclip;
    if (ImGui::Checkbox("Noclip  [2]", &v)) g_noclip = v;
    if (g_noclip) {
        float s = g_noclipSpeed;
        if (ImGui::SliderFloat("Noclip speed  [E / R]", &s, 0.1f, 5.f, "%.2fx")) g_noclipSpeed = s;
    }
    v = g_noStumble;
    if (ImGui::Checkbox("No stumble  [3]", &v)) g_noStumble = v;
    v = g_sameWall;
    if (ImGui::Checkbox("Same wall climbs / runs", &v)) g_sameWall = v;
    v = g_infClimbs;
    if (ImGui::Checkbox("Infinite wall climbs", &v)) g_infClimbs = v;
    v = g_infRuns;
    if (ImGui::Checkbox("Infinite wall runs", &v)) g_infRuns = v;
    v = g_fastLoads;
    if (ImGui::Checkbox("Fast loads", &v)) g_fastLoads = v;
    ImGui::SeparatorText("Bunny hop (wall run jump window)");
    if (ImGui::Button(g_bhopArmed ? "Armed..." : "Arm  [F]")) bhopArm();
    ImGui::SameLine();
    v = g_bhopAuto;
    if (ImGui::Checkbox("Auto", &v)) g_bhopAuto = v;
    float tol = g_bhopTolerance;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::SliderFloat("Ground tolerance (m)", &tol, 0.f, 0.5f, "%.2f")) g_bhopTolerance = tol;
    ImGui::TextDisabled("state %d, ground %.2f m, hops %d", g_state.load(), g_groundDist.load(), g_bhops.load());
    ImGui::SeparatorText("Position");
    if (ImGui::Button("Save  [4]")) savePosition();
    ImGui::SameLine();
    if (ImGui::Button("Load  [5]")) loadPosition();
    ImGui::SameLine();
    if (ImGui::Button("Jump")) gn::jump();
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_status.empty()) ImGui::TextDisabled("%s", g_status.c_str());
    }
    ImGui::SeparatorText("HUD");
    v = g_hud;
    if (ImGui::Checkbox("Player info", &v)) g_hud = v;
    ImGui::SameLine();
    v = g_cheatList;
    if (ImGui::Checkbox("Active cheats", &v)) g_cheatList = v;
}

void window() {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26, ImGui::GetFontSize() * 30), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MEC Trainer")) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Trainer")) {
            trainerTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Movement")) {
            movementTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("World")) {
            worldTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void hud() {
    if (!g_haveFaith) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
    if (g_hud) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.f, vp->WorkPos.y + 12.f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        if (ImGui::Begin("##mec_hud", nullptr, f)) {
            ImGui::Text("Pos (%.1f, %.1f, %.1f)", g_px.load(), g_py.load(), g_pz.load());
            ImGui::Text("Speed %.2f m/s   State %d", g_speed.load(), g_state.load());
        }
        ImGui::End();
    }
    if (g_cheatList) {
        std::string on;
        if (g_god) on += "God  ";
        if (g_noclip) on += "Noclip  ";
        if (g_noStumble) on += "NoStumble  ";
        if (g_sameWall) on += "SameWall  ";
        if (g_infClimbs) on += "InfClimbs  ";
        if (g_infRuns) on += "InfRuns  ";
        if (g_freezeTime) on += "FrozenTime  ";
        if (g_bhopAuto) on += "AutoBhop  ";
        else if (g_bhopArmed) on += "BhopArmed  ";
        if (!on.empty())
            ImGui::GetForegroundDrawList()->AddText(ImVec2(vp->WorkPos.x + 12.f, vp->WorkPos.y + vp->WorkSize.y - 30.f),
                                                    IM_COL32(50, 255, 120, 255), on.c_str());
    }
}

} // namespace

GN_MOD("mec-trainer") {
    if (!gn::ui::onDraw([](bool menuOpen) {
            hud();
            if (menuOpen) window();
        }))
        gn::log("no overlay: hotkeys only");
    gn::onClientTick(clientTick);
    gn::onServerTick(serverTick);
    gn::log("ready: INSERT for the window; 1 god, 2 noclip, 3 no stumble, 4 save, 5 load, F bunny hop");
    return true;
}
