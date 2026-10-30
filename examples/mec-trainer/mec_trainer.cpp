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
//   - bunny hop: the game's landing prediction (gn::landingPrediction); jump pressed the tick before the landing
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
// Her horizontal speed from her position each client tick. gn::movement's speed (the physics body's
// +0x438) read 0.00 most of the time in game, so the HUD measures it; the save keeps the game's value.
std::atomic<float> g_hSpeed{0};
// The game's landing prediction on the HUD (off by default: the first read wraps the prediction).
std::atomic<bool> g_landHud{false}, g_landShow{false}, g_landSoft{false};
std::atomic<float> g_landTicks{0.f};
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
    gn::runOnServer([on] {
        if (!gn::godMode(on)) gn::log("god mode: not available (no Faith?)");
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
        // Sideways from the camera, flat on the ground (A goes this way, D the other).
        float rx = f[2], rz = -f[0];
        const float len = std::sqrt(rx * rx + rz * rz);
        if (len > 1e-4f) rx /= len, rz /= len;
        if (GetAsyncKeyState('W') & 0x8000) delta[0] += f[0], delta[1] += f[1], delta[2] += f[2];
        if (GetAsyncKeyState('S') & 0x8000) delta[0] -= f[0], delta[1] -= f[1], delta[2] -= f[2];
        if (GetAsyncKeyState('A') & 0x8000) delta[0] += rx, delta[2] += rz;
        if (GetAsyncKeyState('D') & 0x8000) delta[0] -= rx, delta[2] -= rz;
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
// The wall run jump window (recorded 2026-10-30, docs/recording.md "Faith's movement"): state 8 is
// the wall run, and it lasts through the jump off the wall until she lands. The mag rope swing (25)
// works the same way (speed-2: 25, a landing through state 0 with jump held, then 25 again), and the
// prediction runs there too (1.55 and 1.10 on the tick before its landings). The tick she lands on
// decides: with jump held then she stays in the wall-run state and leaves the ground again (the
// bhop, speed kept); without it she is grounded (state 1) in that same tick. The client ticks at
// 30 a second, and the press has to come on the last tick before the landing: in the logs every hit
// landed one tick after its press, every miss two ticks after (or the press came too late).
// So each client tick predicts her height one tick on (her fall and its growth) and presses when
// that reaches the landing height:
//   - in a chain, the height she last landed at, on the client (exact on the same ground; on flat
//     ground the hops were exactly 20 ticks apart);
//   - otherwise the server ray's ground plus the contact height (~0.15 m by the logs; the ray is
//     cast where the server has her, so it is good to ~0.1 m).
// Timed on the client from the client character: the movement state is the client's, and on a slow
// frame the server's copy of her lags (presses then came after she had landed). Armed with F (once)
// or always (Auto).
// Better: the game's own landing prediction (gn::landingPrediction, its ticks to hit in 60ths of a
// second, recomputed each tick in the air). It read 0.74-1.54 on the tick before a landing and
// 1.68-2.91 the tick before that, so the first tick it drops below ~1.66 is the one to press on; it
// does not depend on the ray, the frame rate or her speed. In auto-bhop-4 every hit was pressed at
// 0.52-1.65 with the hit 0.11-0.35 m below her; every miss was pressed at 1.73-1.81 (a tick early) or
// with the hit above her (she was still rising toward a ledge or wall, not landing), so both wait.
// With 1.66 the next test chained without end (92 hits in 93 presses; the miss was pressed at 1.68). The heights above are the fallback when
// it is not available. A predicted soft landing is skipped: bhops only happen on landings where
// Faith can jump, and a soft landing grounds her whatever the input (tested 2026-10-30).
std::atomic<bool> g_bhopArmed{false}, g_bhopAuto{false};
std::atomic<bool> g_bhopWall{true}, g_bhopSwing{true}; // which air states bhop: wall run (8), mag rope swing (25)
int g_bhopState = 0; // (client tick) the state the last press was made in
bool bhopState(int s) { return (s == 8 && g_bhopWall) || (s == 25 && g_bhopSwing); }
std::atomic<float> g_bhopContact{0.15f}; // ray height (m) at which a landing registers, when there is no last landing
std::atomic<float> g_bhopBiasMs{0.f};    // look this much further ahead (+) or less (-) than one tick
std::atomic<bool> g_bhopUsePrediction{true};
std::atomic<float> g_bhopPredictBelow{1.66f}; // press when the game's ticks to hit (60ths) drop below this
std::atomic<float> g_groundY{0.f};        // the ground under her (server ray cast)
std::atomic<bool> g_haveGround{false};
std::atomic<float> g_groundDist{-1.f};    // her height above it (client), for the window
std::atomic<int> g_bhops{0}, g_bhopHits{0};
DWORD g_bhopArmedAt = 0, g_lastHop = 0;

// Client tick only.
float g_prevY = 0.f, g_prevVy = 0.f;
bool g_havePrevY = false, g_wasInAir = false;
float g_landY = 0.f, g_landGroundY = 0.f; // where she last landed (client y) and the ray's ground then
DWORD g_landAt = 0;
int g_watchHop = 0;    // client ticks left to see how the last press went
bool g_watchLanded = false;

void bhopCheck(const gn::Movement& m, float y, float dt) {
    const bool inAir = m.on_ground != 0; // on_ground reads 1 in the air (the name is historical)
    const float vy = g_havePrevY && dt > 0.f ? (g_prevY - y) / dt : 0.f; // falling speed, m/s (down is +)
    const float acc = g_havePrevY && dt > 0.f ? std::clamp((vy - g_prevVy) / dt, 0.f, 40.f) : 0.f;
    g_prevY = y, g_prevVy = vy;
    g_havePrevY = true;
    const float ground = g_groundY.load();
    g_groundDist = g_haveGround ? y - ground : -1.f;
    if (g_wasInAir && !inAir) { // a landing: remember its height for the next hop
        g_landY = y, g_landGroundY = ground, g_landAt = GetTickCount();
    }
    g_wasInAir = inAir;

    // How the last press went. A hit is airborne in the press's state again within a few ticks of
    // the landing (sometimes through state 0 for two ticks); grounded (1), a plain jump (2) or a hard
    // landing (24) is a miss.
    if (g_watchHop > 0) {
        --g_watchHop;
        if (!inAir || m.state != g_bhopState) g_watchLanded = true;
        if (g_watchLanded && inAir && m.state == g_bhopState) {
            g_bhopHits.fetch_add(1);
            gn::log("bhop: hit");
            g_watchHop = 0;
        } else if (g_watchLanded && m.state != g_bhopState && m.state != 0) {
            gn::log("bhop: missed (state %d at the landing)", m.state);
            g_watchHop = 0;
        } else if (g_watchHop == 0) {
            gn::log("bhop: no result within 60 ticks of the press (state %d)", m.state);
        }
    }
    if (!g_bhopArmed && !g_bhopAuto) return;
    if (g_bhopArmed && GetTickCount() - g_bhopArmedAt > 5000) g_bhopArmed = false; // armed for 5 s
    if (!bhopState(m.state) || !inAir) return;
    // The game's prediction, when it is fresh (it is made every tick while she is in the air).
    gn::Landing lp{};
    if (g_bhopUsePrediction && gn::landingPrediction(lp) && lp.age_ms < 100) {
        if (lp.ticks_to_hit <= 0.f || lp.ticks_to_hit > g_bhopPredictBelow.load()) return;
        if (lp.hit_height >= y) return; // a hit above her feet is not a landing (she is rising into geometry)
        if (GetTickCount() - g_lastHop < 250) return; // one press per landing
        if (lp.soft_landing) { // a soft landing never bhops (the game grounds her whatever the input)
            g_lastHop = GetTickCount();
            gn::log("bhop: soft landing predicted: no bhop possible, not pressed");
            return;
        }
        g_lastHop = GetTickCount();
        gn::jump();
        g_bhops.fetch_add(1);
        g_bhopArmed = false;
        g_watchHop = 60;
        g_watchLanded = false, g_bhopState = m.state;
        gn::log("bhop: jump pressed by the game's prediction: %.2f/60 s to the hit at %.2f (she is at %.2f, state %d)", lp.ticks_to_hit,
                lp.hit_height, y, m.state);
        return;
    }
    if (vy < 1.5f) return; // a wall run itself barely falls
    // The landing height: the last landing's if it was on this ground, else the ray's.
    const bool chain = g_landAt && GetTickCount() - g_landAt < 3000 && g_haveGround && std::fabs(ground - g_landGroundY) < 0.15f;
    if (!chain && !g_haveGround) return;
    const float target = chain ? g_landY : ground + g_bhopContact.load();
    const float ahead = std::max(0.f, dt + g_bhopBiasMs.load() / 1000.f);
    const float next = y - vy * ahead - 0.5f * acc * ahead * ahead; // her height one tick on
    if (next > target) return;
    if (GetTickCount() - g_lastHop < 250) return; // one press per landing
    g_lastHop = GetTickCount();
    gn::jump(); // the game reads it on its next tick: the landing tick
    g_bhops.fetch_add(1);
    g_bhopArmed = false;
    g_watchHop = 60;
    g_watchLanded = false, g_bhopState = m.state;
    gn::log("bhop: jump pressed %.3f m above the %s, falling %.1f m/s; next tick %+.3f m (tick %.1f ms)", y - target,
            chain ? "last landing" : "ray's contact height", vy, next - target, dt * 1000.f);
}

void serverTick(float) {
    void* faith = gn::localServerCharacter();
    gn::Vec3 p;
    if (!faith || !gn::positionOf(faith, p)) return;
    gn::RayHit hit;
    const bool hitGround = gn::raycast({p.x, p.y + 0.5f, p.z}, {p.x, p.y - 3.f, p.z}, hit);
    if (hitGround) g_groundY = hit.position[1];
    g_haveGround = hitGround;
    if (g_god) gn::godMode(true); // again after a respawn (a new health component)
}

// Arming one hop turns auto off: the two would fight over the same landing.
void bhopArm() {
    if (g_bhopAuto.exchange(false)) gn::log("bhop: armed: auto bhop off");
    g_bhopArmed = true;
    g_bhopArmedAt = GetTickCount();
}

// ---- client tick -----------------------------------------------------------------------------------------
void clientTick(float dt) {
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
    static bool havePrev = false;
    if (!have) {
        g_noclipActive = false;
        g_havePrevY = false;
        havePrev = false;
        return;
    }
    if (havePrev && dt > 0.f) {
        const float dx = p.x - g_px.load(), dz = p.z - g_pz.load();
        const float s = std::sqrt(dx * dx + dz * dz) / dt;
        if (s < 100.f) g_hSpeed = s; // more is a teleport (a load, a respawn, noclip's save and load)
    }
    havePrev = true;
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
        // Let go where she is, standing and still. As in MEC_Trainer the last ground stays far below
        // her (-2000), so the drop cannot count as a fall; kept until she is down (the ray cast finds
        // the ground under her feet) and a moment after, at most 15 s.
        g_noclipActive = false;
        gn::Movement w{};
        w.state = 2;
        gn::setMovement(w, GN_MOVE_STATE | GN_MOVE_SPEED);
        g_noclipGrace = 450;
    }
    if (g_noclipGrace > 0) {
        const float gd = g_groundDist.load();
        if (gd >= 0.f && gd < 0.15f && g_noclipGrace > 15) g_noclipGrace = 15; // landed: half a second more
        --g_noclipGrace;
        gn::Movement w{};
        w.last_ground_y = -2000.f;
        gn::setMovement(w, GN_MOVE_LAST_GROUND_Y);
    }
    if (g_noclipActive) {
        noclipClient(m, keys);
        g_havePrevY = false;
        return;
    }
    if (g_landHud) {
        gn::Landing lp{};
        const bool fresh = m.on_ground != 0 && gn::landingPrediction(lp) && lp.age_ms < 100 && lp.ticks_to_hit > 0.f;
        g_landTicks = lp.ticks_to_hit, g_landSoft = lp.soft_landing != 0;
        g_landShow = fresh && lp.hit_height < p.y; // as auto-bhop: a hit above her feet is not a landing
    } else {
        g_landShow = false;
    }
    bhopCheck(m, p.y, dt);

    // No stumble (and god mode: falls are deaths in MEC, not damage): a fall only counts from where
    // Faith is now.
    if ((g_noStumble || g_god) && m.last_ground_y - p.y >= 1.f && m.state != 3 && m.state != 4) {
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
    ImGui::SeparatorText("Bunny hop (wall run / swing jump window)");
    v = g_bhopWall;
    if (ImGui::Checkbox("Wall runs", &v)) g_bhopWall = v;
    ImGui::SameLine();
    v = g_bhopSwing;
    if (ImGui::Checkbox("Mag rope swings", &v)) g_bhopSwing = v;
    if (ImGui::Button(g_bhopArmed ? "Armed..." : "Arm  [F]")) bhopArm();
    ImGui::SameLine();
    v = g_bhopAuto;
    if (ImGui::Checkbox("Auto", &v)) g_bhopAuto = v;
    float contact = g_bhopContact, bias = g_bhopBiasMs;
    bool usePredict = g_bhopUsePrediction;
    if (ImGui::Checkbox("Use the game's landing prediction", &usePredict)) g_bhopUsePrediction = usePredict;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The animation system predicts every tick how long until she lands. Off: the trainer's own\n"
                          "estimate from heights (the settings below).");
    float below = g_bhopPredictBelow;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::SliderFloat("Press below (1/60 s)", &below, 0.5f, 3.f, "%.2f")) g_bhopPredictBelow = below;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Jump is pressed on the first tick the predicted time to the hit is under this. On the tick\n"
                          "before a landing it read 0.52-1.65, the tick before that 1.68-2.91: 1.66 sits between.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::SliderFloat("Contact height (m)", &contact, 0.f, 0.4f, "%.2f")) g_bhopContact = contact;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For the first hop (no landing on this ground yet): how high above the ground, by the ray,\n"
                          "the landing registers (about 0.15 m). Chained hops aim at the height of the last landing.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::SliderFloat("Bias (ms)", &bias, -20.f, 20.f, "%.0f")) g_bhopBiasMs = bias;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Jump is pressed on the last tick before the landing, by her height one tick (33 ms) on.\n"
                          "+ looks further ahead (presses earlier), - less. 0 to start with.");
    ImGui::TextDisabled("state %d, ground %.2f m, presses %d, hits %d", g_state.load(), g_groundDist.load(), g_bhops.load(),
                        g_bhopHits.load());
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
    v = g_landHud;
    if (ImGui::Checkbox("Landing prediction", &v)) g_landHud = v;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("In the air: the game's predicted time to the landing and a bar that runs down to it. Green\n"
                          "is the tick auto-bhop presses on (below \"Press below\"), red a soft landing (no bhop).");
}

// A bar of the predicted time to the landing (12/60 s full), with a tick at the press threshold.
void landingHud() {
    if (!g_landShow) {
        ImGui::TextDisabled("Landing: -");
        return;
    }
    const float t = g_landTicks, below = g_bhopPredictBelow;
    const bool soft = g_landSoft, window = t <= below;
    ImGui::Text("Landing in %.2f/60 s%s", t, soft ? "   soft (no bhop)" : window ? "   bhop window" : "");
    constexpr float kFull = 12.f;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetFontSize() * 12, h = ImGui::GetFontSize() * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = soft ? IM_COL32(220, 60, 60, 255) : window ? IM_COL32(60, 210, 90, 255) : IM_COL32(200, 200, 200, 200);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), IM_COL32(0, 0, 0, 120));
    dl->AddRectFilled(a, ImVec2(a.x + w * std::min(t / kFull, 1.f), a.y + h), col);
    const float mx = a.x + w * std::min(below / kFull, 1.f);
    dl->AddLine(ImVec2(mx, a.y - 2.f), ImVec2(mx, a.y + h + 2.f), IM_COL32(255, 255, 255, 255), 2.f);
    ImGui::Dummy(ImVec2(w, h));
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
            ImGui::Text("Speed %.2f m/s   State %d", g_hSpeed.load(), g_state.load());
            if (g_landHud) landingHud();
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
