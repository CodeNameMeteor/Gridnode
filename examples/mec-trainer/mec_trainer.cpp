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
//   - trigger volumes: the level's area triggers drawn by category (volumes.cpp)
//   - a soft anti-cheat: while the trainer is loaded, Dash and time trial times and ghosts are not
//     uploaded (the gateway backend's upload requests are dropped), a new best Dash time is not
//     saved (a code patch, gn::patchCall), and the screen says so
//     (docs/research in db/annotations/online.yaml)
//   - the window lives in GridNode's overlay (INSERT); the HUD stays on screen
//
// Hotkeys (GridNode hotkeys, kept in mods/mec-trainer.ini; defaults as in MEC_Trainer):
//   1 god mode   2 noclip   3 no stumble   4 save position   5 load position
//   F arm the bunny hop   (none) auto bunny hop on / off   (rebound in the trainer's Keys tab)
//   6 free camera   7 teleport to its crosshair   8 teleport to it (GridNode's free camera)
//   E / R noclip slower / faster   (noclip: WASD, Space up, Shift / C down; fixed keys)
//
// Settings (sliders, checkboxes) are kept in mods/mec-trainer.ini between runs (gn::config), except
// god mode, noclip and frozen time, which start off.
//
// Threads: render (window, HUD: reads atomics, posts work), client tick (movement features,
// hotkeys run there), server tick (noclip and teleports). Faith's state is read on the client thread.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/ui.hpp"
#include "volumes.hpp"

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

// ---- what the trainer changed, put back when it is unloaded (render thread) ----------------------------
std::atomic<bool> g_tuned{false};       // a movement tuning value was set
float g_timeScaleOriginal = -1.f;       // GameTimeSettings TimeScale before the slider first moved (-1: never)
std::vector<std::pair<const char*, bool>> g_postOriginal; // post-processing fields as they were

// ---- soft anti-cheat: no Dash or time trial uploads while the trainer is loaded ------------------------
// The gateway backend runs each upload request message as a call to EA's servers. Two message
// listeners lead to it (db/annotations/online.yaml); both are wrapped (pointer swaps through GridNode,
// taken out when the trainer is unloaded) and drop these requests. Everything else passes.
constexpr uint32_t kBlockedUploads[] = {
    0x685f689d, // UploadNamedChallengeStatsRequest: a Dash's finish time
    0x39c9a83d, // UploadStatsRequest: a time trial's finish time and splits
    0xb70b72ba, // UploadReplayRequestMessageBase: the run's ghost
    0xaa067849, // SetPlayerGhostRequest: the ghost others race
};
using ListenerFn = void (*)(void* listener, void* message);
ListenerFn g_backendOrig = nullptr, g_routeOrig = nullptr;
std::atomic<int> g_uploadBlock{0}; // listeners wrapped (2: all)
std::atomic<int> g_blockedCount{0};

bool dropUpload(void* message) {
    uint32_t id = 0;
    if (!message || !gn::read(static_cast<const char*>(message) + 0xc, id)) return false;
    for (uint32_t b : kBlockedUploads)
        if (id == b) {
            if (g_blockedCount.fetch_add(1) < 20) gn::log("anti-cheat: upload %08x not sent (the trainer is loaded)", id);
            return true;
        }
    return false;
}
void backendListener(void* listener, void* message) {
    if (!dropUpload(message)) g_backendOrig(listener, message);
}
void routeListener(void* listener, void* message) {
    if (!dropUpload(message)) g_routeOrig(listener, message);
}
// A new best Dash time is not kept while the trainer is loaded: NamedChallengeStats_onFinished (the one
// call site, patched) runs as usual, so the end screen shows the run, then any best it wrote is put
// back, with the pending list of changes it appended to (saved later) and its "changed" flag.
using FinishFn = void (*)(void* dashState, void* challengeData, void* times, uint32_t runId);
using FindFn = void** (*)(void* map, void** out, const char** key);
FinishFn g_finishOrig = nullptr;
std::atomic<bool> g_bestGuard{false};
std::atomic<int> g_bestsKept{0};

struct BestValue {
    char* at; // a map node's value (+0x10)
    int64_t value;
};
// The values in the DashState maps for the challenge's stats (main, extras), per player index.
std::vector<BestValue> bestsOf(void* state, void* data) {
    std::vector<BestValue> out;
    std::vector<const char*> keys;
    if (const char* k = nullptr; gn::read(static_cast<char*>(data) + 0x58, k) && k) keys.push_back(k);
    char* extras = nullptr;
    if (gn::read(static_cast<char*>(data) + 0x88, extras) && extras) {
        uint32_t n = 0;
        gn::read(extras - 4, n);
        for (uint32_t i = 0; i < n && i < 16; ++i)
            if (const char* k = nullptr; gn::read(extras + i * 0x18, k) && k) keys.push_back(k);
    }
    char* maps = nullptr;
    if (!gn::read(static_cast<char*>(state) + 0x38, maps) || !maps) return out;
    const auto find = reinterpret_cast<FindFn>(gn::rva(gn::engine::function::DashStats_find));
    for (int player = 0; player < 2; ++player) {
        char* map = maps + player * 0x30;
        char* buckets = nullptr;
        uint32_t count = 0;
        if (!gn::read(map + 8, buckets) || !gn::read(map + 0x10, count) || !buckets || !count || count > 1000000) continue;
        void* end = nullptr;
        gn::read(buckets + size_t(count) * 8, end);
        for (const char* key : keys) {
            void* it[2] = {};
            find(map, it, &key);
            if (!it[0] || it[0] == end) continue;
            BestValue b{static_cast<char*>(it[0]) + 0x10, 0};
            if (gn::read(b.at, b.value)) out.push_back(b);
        }
    }
    return out;
}

void keepBest(void* state, void* data, void* times, uint32_t run) {
    if (!state || !data) return g_finishOrig(state, data, times, run);
    const std::vector<BestValue> before = bestsOf(state, data);
    uint8_t pending[0x60] = {}; // +0x218 "changed", +0x220.. the pending changes' list
    const bool havePending = gn::api().read(static_cast<char*>(state) + 0x218, pending, sizeof(pending)) == GN_OK;
    g_finishOrig(state, data, times, run);
    bool changed = false;
    for (const BestValue& b : before) {
        int64_t now = 0;
        if (gn::read(b.at, now) && now != b.value) changed = true, gn::write(b.at, b.value);
    }
    if (!changed) return;
    if (havePending) gn::api().write(static_cast<char*>(state) + 0x218, pending, sizeof(pending));
    g_bestsKept.fetch_add(1);
    gn::log("anti-cheat: a new best Dash time was not saved (the trainer is loaded)");
}

void blockUploads() {
    namespace h = gn::engine::hook;
    int n = 0;
    n += gn::hookSlot(h::GatewayBackendListener_onMessage.slot, h::GatewayBackendListener_onMessage.original, &backendListener, &g_backendOrig);
    n += gn::hookSlot(h::GatewayRouteListener_onMessage.slot, h::GatewayRouteListener_onMessage.original, &routeListener, &g_routeOrig);
    g_uploadBlock = n;
    gn::log(n == 2 ? "anti-cheat: Dash and time trial uploads are blocked while the trainer is loaded"
                   : "anti-cheat: COULD NOT block uploads (%d of 2 listeners wrapped)", n);
    namespace c = gn::engine::code;
    namespace f = gn::engine::function;
    g_bestGuard = gn::patchCall(c::NamedChallengeFinish_statsCall, f::NamedChallengeStats_onFinished, &keepBest, &g_finishOrig);
    gn::log(g_bestGuard ? "anti-cheat: new best Dash times are not saved while the trainer is loaded"
                        : "anti-cheat: COULD NOT stop new best Dash times being saved");
}

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
// Bhop chains from GridNode's movement events, whoever pressed jump: a landing out of a wall run or
// swing (LANDED, old state 8 / 25) that goes straight back into that state is a bhop; any other
// state after the landing ends the chain.
std::atomic<int> g_chain{0}, g_bestChain{0};
std::atomic<bool> g_logMoves{false};
int g_chainFrom = 0; // (client thread) the state she landed out of, 0 when not on a bhop landing

void onMove(const gn::MovementEvent& e, const gn::Movement&) {
    if (g_logMoves)
        gn::log("movement: %s, state %d -> %d", e.kind == GN_MOVE_EVENT_LANDED ? "landed" : e.kind == GN_MOVE_EVENT_AIRBORNE ? "airborne" : "state",
                e.old_state, e.state);
    if (e.kind == GN_MOVE_EVENT_LANDED) {
        g_chainFrom = e.old_state == 8 || e.old_state == 25 ? e.old_state : 0;
        if (!g_chainFrom) g_chain = 0;
    } else if (e.kind == GN_MOVE_EVENT_STATE && e.state != 0 && g_chainFrom) {
        if (e.state == g_chainFrom) {
            const int n = g_chain.fetch_add(1) + 1;
            if (n > g_bestChain) g_bestChain = n;
        } else {
            g_chain = 0;
        }
        g_chainFrom = 0;
    }
}
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

// ---- free camera (GridNode's, the layout editor's: gn::freecam) ------------------------------------
std::atomic<bool> g_keepFlying{true}; // after a teleport, stay in the free camera (the user's choice, 2026-11-01; off: back to Faith)

void toggleFreecam() {
    const bool on = !gn::freecamOn();
    if (!gn::freecam(on)) gn::log("free camera: not available (no Faith)");
}
void teleportCrosshair() {
    if (!gn::teleportToCrosshair(g_keepFlying)) gn::log("teleport: fly the free camera first");
}
void teleportFreecam() {
    if (!gn::teleportToFreecam(g_keepFlying)) gn::log("teleport: fly the free camera first");
}

// A label with its hotkey's key, as the player bound it ("God mode  [1]"; no brackets for no key).
std::string withKey(const char* label, const char* id, const char* suffix = "") {
    const std::string k = gn::hotkeyName(id);
    return std::string(label) + (k.empty() || k == "none" ? "" : "  [" + k + "]") + suffix;
}

// Arming one hop turns auto off: the two would fight over the same landing.
void bhopArm() {
    if (g_bhopAuto.exchange(false)) gn::log("bhop: armed: auto bhop off");
    g_bhopArmed = true;
    g_bhopArmedAt = GetTickCount();
}

// ---- client tick -----------------------------------------------------------------------------------------
bool g_fast = false;   // fast loads running (client thread)
float g_simFps = 30.f; // MaxSimFps before it

void clientTick(float dt) {
    const gn::GameState gs = gn::gameState();
    g_loading = gs.loading != 0;
    g_timeOfDay = gs.time_of_day;

    // Fast loads: let the simulation run flat out, but only while loading.
    const bool wantFast = g_fastLoads && gs.loading;
    if (wantFast != g_fast) {
        g_fast = wantFast;
        gn::Object ts = gn::settings("GameTimeSettings");
        if (g_fast) ts.get("MaxSimFps", g_simFps); // put back exactly what it was
        ts.set("MaxSimFps", g_fast ? 50000.f : g_simFps);
        ts.set("TimeScale", g_fast ? 3000.f : g_timeScale.load());
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
    // Her horizontal speed over the last quarter second of real time, not tick to tick: her position
    // moves in physics steps that do not line up with the client ticks, so a per-tick speed jumps
    // around (user, 2026-11-02: "really jittery").
    {
        struct Sample {
            double t;
            float x, z;
        };
        static std::deque<Sample> samples;
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now), QueryPerformanceFrequency(&freq);
        const double t = double(now.QuadPart) / double(freq.QuadPart);
        if (!havePrev) samples.clear();
        if (!samples.empty()) {
            const Sample& last = samples.back();
            const float dx = p.x - last.x, dz = p.z - last.z;
            if (t > last.t && std::sqrt(dx * dx + dz * dz) / (t - last.t) > 100.f) samples.clear(); // a teleport
        }
        samples.push_back({t, p.x, p.z});
        while (samples.size() > 2 && t - samples[1].t >= 0.25) samples.pop_front();
        const Sample& first = samples.front();
        if (t - first.t > 0.05) {
            const float dx = p.x - first.x, dz = p.z - first.z;
            g_hSpeed = static_cast<float>(std::sqrt(dx * dx + dz * dz) / (t - first.t));
        }
    }
    havePrev = true;
    g_px = p.x, g_py = p.y, g_pz = p.z;
    g_speed = m.speed;
    g_state = m.state;
    g_lastGroundY = m.last_ground_y;
    const bool keys = gameFocused() && !gn::ui::menuOpen(); // noclip's movement keys (fixed, held)

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
        g_tuned = false;
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
        g_tuned = true;
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
        if (ImGui::InputFloat(t.name, &v, 0.f, 0.f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) gn::setTuning(t.name, v), g_tuned = true;
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
        if (g_timeScaleOriginal < 0.f && !gn::settings("GameTimeSettings").get("TimeScale", g_timeScaleOriginal)) g_timeScaleOriginal = 1.f;
        g_timeScale = scale;
        gn::runOnClient([scale] { gn::settings("GameTimeSettings").set("TimeScale", scale); });
    }
    ImGui::SeparatorText("Post-processing");
    static const char* const kPost[3][2] = {{"Bloom", "BloomEnable"}, {"Blur", "BlurEnable"}, {"Vignette", "VignetteEnable"}};
    gn::Object pp = gn::settings("GlobalPostProcessSettings");
    for (const auto& e : kPost) {
        bool on = false;
        if (!pp.get(e[1], on)) continue;
        const bool was = on;
        if (ImGui::Checkbox(e[0], &on)) {
            const char* field = e[1];
            if (std::none_of(g_postOriginal.begin(), g_postOriginal.end(), [&](const auto& o) { return o.first == field; }))
                g_postOriginal.push_back({field, was});
            gn::runOnClient([field, on] { gn::settings("GlobalPostProcessSettings").set(field, on); });
        }
    }
}

void trainerTab() {
    bool v;
    v = g_god;
    if (ImGui::Checkbox(withKey("God mode", "god").c_str(), &v)) g_god = v, setGodMode(v);
    v = g_noclip;
    if (ImGui::Checkbox(withKey("Noclip", "noclip").c_str(), &v)) g_noclip = v;
    if (g_noclip) {
        float s = g_noclipSpeed;
        if (ImGui::SliderFloat("Noclip speed  [E / R]", &s, 0.1f, 5.f, "%.2fx")) g_noclipSpeed = s;
    }
    v = g_noStumble;
    if (ImGui::Checkbox(withKey("No stumble", "no_stumble").c_str(), &v)) g_noStumble = v;
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
    if (ImGui::Button(g_bhopArmed ? "Armed...###arm" : withKey("Arm", "bhop_arm", "###arm").c_str())) bhopArm();
    ImGui::SameLine();
    v = g_bhopAuto;
    if (ImGui::Checkbox(withKey("Auto", "bhop_auto").c_str(), &v)) g_bhopAuto = v;
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
    ImGui::Text("Chain %d, best %d", g_chain.load(), g_bestChain.load());
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset best")) g_bestChain = 0;
    v = g_logMoves;
    if (ImGui::Checkbox("Log movement events", &v)) g_logMoves = v;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every landing, take-off and state change to gridnode.log (gn::onMovement).");
    ImGui::SeparatorText("Position");
    if (ImGui::Button(withKey("Save", "save").c_str())) savePosition();
    ImGui::SameLine();
    if (ImGui::Button(withKey("Load", "load").c_str())) loadPosition();
    ImGui::SameLine();
    if (ImGui::Button("Jump")) gn::jump();
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_status.empty()) ImGui::TextDisabled("%s", g_status.c_str());
    }
    ImGui::SeparatorText("Free camera");
    const bool flying = gn::freecamOn();
    if (ImGui::Button(withKey(flying ? "Back to Faith" : "Fly the free camera", "freecam", "###freecam").c_str())) toggleFreecam();
    ImGui::BeginDisabled(!flying);
    if (ImGui::Button(withKey("Teleport to the crosshair", "tp_crosshair").c_str())) teleportCrosshair();
    ImGui::SameLine();
    if (ImGui::Button(withKey("Teleport to the camera", "tp_freecam").c_str())) teleportFreecam();
    ImGui::EndDisabled();
    v = g_keepFlying;
    if (ImGui::Checkbox("Keep flying after a teleport", &v)) g_keepFlying = v;
    ImGui::TextDisabled("Mouse looks, W A S D fly, Q / E down / up, Shift / Ctrl faster / slower, wheel: speed.");
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
    ImGui::SeparatorText("Unload");
    if (ImGui::Button("Unload the trainer")) gn::unloadSelf(); // everything it changed goes back (GN_MOD_UNLOAD)
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts back what it changed and unloads it; GridNode's Mods list loads it again.");
    ImGui::SameLine();
    if (ImGui::Button("Unload GridNode too")) gn::unloadGridNode();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every mod and GridNode itself: the game as if none of it had loaded.");
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

// Every hotkey, rebound here (kept in mods/mec-trainer.ini as key.<id>).
struct KeyRow {
    const char* id;
    const char* label;
};
const KeyRow kKeyRows[] = {{"god", "God mode on / off"},
                           {"noclip", "Noclip on / off (god mode with it)"},
                           {"no_stumble", "No stumble on / off"},
                           {"save", "Save position"},
                           {"load", "Load position"},
                           {"bhop_arm", "Bunny hop: arm one hop (auto off)"},
                           {"bhop_auto", "Bunny hop: auto on / off"},
                           {"freecam", "Free camera on / off"},
                           {"tp_crosshair", "Free camera: teleport Faith to the crosshair"},
                           {"tp_freecam", "Free camera: teleport Faith to the camera"}};

void keysTab() {
    ImGui::TextDisabled("Click a key, then press the new one. Escape: no key. Backspace: the default.");
    for (const KeyRow& k : kKeyRows) gn::ui::hotkeyRow(k.id, k.label);
    ImGui::TextDisabled("Noclip: WASD, Space up, Shift / C down, E / R slower / faster (fixed).");
}

// ---- settings kept between runs (mods/mec-trainer.ini) -------------------------------------------------
struct BoolSetting {
    const char* key;
    std::atomic<bool>* v;
};
struct FloatSetting {
    const char* key;
    std::atomic<float>* v;
};
// God mode, noclip and frozen time are left out: they start off each run.
const BoolSetting kBools[] = {{"no_stumble", &g_noStumble},        {"same_wall", &g_sameWall},
                              {"inf_climbs", &g_infClimbs},        {"inf_runs", &g_infRuns},
                              {"fast_loads", &g_fastLoads},        {"hud.player_info", &g_hud},
                              {"hud.cheats", &g_cheatList},        {"hud.landing", &g_landHud},
                              {"bhop.auto", &g_bhopAuto},          {"bhop.wall_runs", &g_bhopWall},
                              {"bhop.swings", &g_bhopSwing},       {"bhop.use_prediction", &g_bhopUsePrediction},
                              {"freecam.stay_flying", &g_keepFlying}}; // (a new key: the old one saved the first default, off)
const FloatSetting kFloats[] = {{"noclip.speed", &g_noclipSpeed},  {"bhop.press_below", &g_bhopPredictBelow},
                                {"bhop.contact", &g_bhopContact},   {"bhop.bias_ms", &g_bhopBiasMs}};

void loadSettings() {
    for (const BoolSetting& s : kBools) *s.v = gn::config::getBool(s.key, s.v->load());
    for (const FloatSetting& s : kFloats) *s.v = gn::config::getFloat(s.key, s.v->load());
}

// After the window: GridNode keeps the values and writes the file only when one changed.
void saveSettings() {
    for (const BoolSetting& s : kBools) gn::config::setBool(s.key, s.v->load());
    for (const FloatSetting& s : kFloats) gn::config::setFloat(s.key, s.v->load());
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
        if (ImGui::BeginTabItem("Volumes")) {
            volumes::tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Keys")) {
            keysTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    saveSettings();
}

// Always on screen while the trainer is loaded (not an option): runs made with it are marked.
void trainerMark() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const bool blocked = g_uploadBlock.load() == 2 && g_bestGuard.load();
    char text[160], skipped[48] = "";
    if (const int n = g_blockedCount.load() + g_bestsKept.load()) snprintf(skipped, sizeof(skipped), " (%d stopped)", n);
    snprintf(text, sizeof(text),
             blocked ? "MEC Trainer loaded - Dash and time trial times are not uploaded or saved%s" : "MEC Trainer loaded - times NOT blocked%s",
             skipped);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 at(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f, vp->WorkPos.y + 6.f);
    dl->AddRectFilled(ImVec2(at.x - 8.f, at.y - 3.f), ImVec2(at.x + size.x + 8.f, at.y + size.y + 3.f), IM_COL32(0, 0, 0, 140), 4.f);
    dl->AddText(at, blocked ? IM_COL32(255, 200, 60, 255) : IM_COL32(255, 80, 80, 255), text);
}

void hud() {
    trainerMark();
    if (!g_haveFaith) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
    if (g_hud) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.f, vp->WorkPos.y + 12.f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        if (ImGui::Begin("##mec_hud", nullptr, f)) {
            ImGui::Text("Pos (%.1f, %.1f, %.1f)", g_px.load(), g_py.load(), g_pz.load());
            ImGui::Text("Speed %.1f m/s   State %d", g_hSpeed.load(), g_state.load());
            if (g_landHud) landingHud();
            if (g_chain || g_bestChain) ImGui::Text("Bhop chain %d   best %d", g_chain.load(), g_bestChain.load());
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
        if (volumes::anyShown()) on += "Volumes  ";
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
            volumes::draw();
            hud();
            if (menuOpen) window();
        }))
        gn::log("no overlay: hotkeys only");
    loadSettings();
    volumes::loadSettings();
    blockUploads();
    gn::onLevel([](const char*, bool) { volumes::levelChanged(); });
    // Run on the client thread, as the old key loop did.
    gn::hotkey("god", "God mode on / off", '1', [] { g_god = !g_god; setGodMode(g_god); });
    gn::hotkey("noclip", "Noclip on / off (god mode with it)", '2', [] {
        g_noclip = !g_noclip;
        if (g_noclip && !g_god) g_god = true, setGodMode(true);
    });
    gn::hotkey("no_stumble", "No stumble on / off", '3', [] { g_noStumble = !g_noStumble; });
    gn::hotkey("save", "Save position", '4', savePosition);
    gn::hotkey("load", "Load position", '5', loadPosition);
    gn::hotkey("bhop_arm", "Bunny hop: arm one hop (auto off)", 'F', bhopArm);
    gn::hotkey("bhop_auto", "Bunny hop: auto on / off", 0, [] {
        g_bhopAuto = !g_bhopAuto;
        gn::log("bhop: auto %s", g_bhopAuto ? "on" : "off");
    });
    gn::hotkey("freecam", "Free camera on / off", '6', toggleFreecam);
    gn::hotkey("tp_crosshair", "Free camera: teleport Faith to the crosshair", '7', teleportCrosshair);
    gn::hotkey("tp_freecam", "Free camera: teleport Faith to the camera", '8', teleportFreecam);
    gn::onMovement(onMove);
    gn::onClientTick([](float dt) {
        clientTick(dt);
        volumes::clientTick(dt);
    });
    gn::onServerTick(serverTick);
    gn::log("ready: INSERT for the window; keys in its Keys tab (1 god, 2 noclip, 3 no stumble, 4 save, 5 load, F bunny hop)");
    return true;
}

// Unloading (GridNode's Mods: Reload, or a full unload of GridNode): everything the trainer changed
// in the game goes back. GridNode takes out its callbacks and hotkeys and waits for the ticks, so the
// tasks posted here run before the trainer is freed.
GN_MOD_UNLOAD() {
    // Only what is not kept in the ini (a frame may still save the settings before the UI goes).
    g_noclip = false, g_bhopArmed = false, g_freezeTime = false;
    if (g_god.exchange(false)) gn::runOnServer([] { gn::godMode(false); });
    if (gn::freecamOn()) gn::freecam(false);
    if (g_tuned.exchange(false)) {
        gn::Tuning t;
        for (uint32_t i = 0; i < gn::tuningCount(); ++i)
            if (gn::tuningAt(i, t)) gn::setTuning(t.name, t.default_value);
    }
    const float scale = g_timeScaleOriginal;
    const auto post = g_postOriginal;
    gn::runOnClient([scale, post] {
        gn::Object ts = gn::settings("GameTimeSettings");
        if (g_fast) ts.set("MaxSimFps", g_simFps), g_fast = false;
        if (scale >= 0.f) ts.set("TimeScale", scale);
        for (const auto& [field, on] : post) gn::settings("GlobalPostProcessSettings").set(field, on);
    });
    volumes::unload();
    gn::log("unloading: god mode, noclip, free camera, tuning, time scale and post-processing put back");
    return true;
}
