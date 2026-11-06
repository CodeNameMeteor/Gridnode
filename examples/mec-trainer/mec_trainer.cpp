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
//   - noclip: GridNode's free camera flies (no collision, nothing to grab, no falling); Faith floats
//     where she was, and is teleported to the camera when noclip goes off, with no fall damage until she lands
//   - bunny hop: the game's landing prediction (gn::landingPrediction); jump pressed the tick before the landing
//   - trigger volumes: the level's area triggers drawn by category (volumes.cpp)
//   - World: kill volumes off, movement blockers (invisible barriers) off, no city alerts in free roam
//   - a soft anti-cheat: while the trainer is loaded, Dash and time trial times and ghosts are not
//     uploaded (the gateway backend's upload requests are dropped), a new best Dash time is not
//     saved (a code patch, gn::patchCall); the screen says "MEC Trainer Loaded", the log the rest
//     (docs/research in db/annotations/online.yaml)
//   - the window lives in GridNode's overlay (INSERT); the HUD stays on screen
//
// Hotkeys (GridNode hotkeys, kept in mods/mec-trainer.ini; defaults as in MEC_Trainer):
//   1 god mode   2 noclip   4 save position   5 load position
//   F arm the bunny hop   (none) auto bunny hop on / off   (rebound in the trainer's Settings tab)
//   6 free camera   7 teleport to its crosshair   8 teleport to it (GridNode's free camera)
//   noclip flies as the free camera does: WASD, Q / E down / up, Shift / Ctrl faster / slower, wheel
//
// Settings (sliders, checkboxes) are kept in mods/mec-trainer.ini between runs (gn::config), except
// god mode, noclip and frozen time, which start off.
//
// Threads: render (window, HUD: reads atomics, posts work), client tick (movement features,
// hotkeys run there), server tick (noclip and teleports). Faith's state is read on the client thread.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/ui.hpp"
#include "volumes.hpp"

namespace {

// ---- settings (render thread writes, ticks read) ------------------------------------------------------
std::atomic<bool> g_god{false}, g_noclip{false}, g_sameWall{false}, g_infClimbs{false}, g_infRuns{false}, g_fastLoads{false},
    g_freezeTime{false}, g_hud{true}, g_cheatList{true}, g_killOff{false}, g_barriersOff{false}, g_noAlerts{false};
std::atomic<float> g_frozenTime{0.f}, g_timeScale{1.f};

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
// Faith's move as ANT tracks it (gn::moveTracker, API 25): shown on the Player tab.
std::atomic<int> g_antMove{-1};
std::atomic<bool> g_antJump{false}, g_haveTracker{false};
std::atomic<float> g_antSpeed{0.f};
std::atomic<bool> g_loading{false};

// ---- what the trainer changed, put back when it is unloaded (render thread) ----------------------------
std::atomic<bool> g_tuned{false};       // a movement tuning value was set
// Game speed (client thread): the game's own TimeScale (its slow motion eases it and writes 1.0 when
// it ends, SlowMotionEntity 0x1433362a0 / 0x143332d10), and what the trainer last wrote on top of it
// (-1: nothing; the game's value stands).
float g_speedBase = 1.f, g_speedWritten = -1.f;
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

// ---- kill volumes off -------------------------------------------------------------------------------------
// A kill volume tests each character in it (KillVolume_testCharacter) and kills one inside; skipped,
// nobody is killed by one: the street kill floors, fans, the monorail (AI too). The streets have no
// collision of their own, so falling through one is caught below (back to where she last stood).
// It returns whether the character is inside (the shape test's al, passed on): a void stand-in left
// whatever was in al, the setting's 1, and every volume killed her at once (2026-11-04).
using KillTestFn = bool (*)(void* volume, void* character);
KillTestFn g_killOrig = nullptr;
bool g_killHooked = false;
std::atomic<uint32_t> g_killSpared{0};

bool killTest(void* volume, void* character) {
    if (g_killOff) {
        g_killSpared.fetch_add(1, std::memory_order_relaxed);
        return false; // not inside
    }
    return g_killOrig(volume, character);
}

void hookKillVolumes() {
    namespace f = gn::engine::function;
    const uint8_t want[11] = {0x53, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x02};
    uint8_t got[11] = {};
    // After a reload GridNode's detour entry is still there (a jmp over the first 5 bytes; it stays
    // until GridNode unloads): the rest must still be the game's. Checking all 11 bytes refused the
    // hook on every reload (2026-11-04).
    const bool ok = gn::api().read(reinterpret_cast<void*>(f::KillVolume_testCharacter), got, sizeof(got)) == GN_OK &&
                    (std::memcmp(got, want, sizeof(got)) == 0 || (got[0] == 0xE9 && std::memcmp(got + 5, want + 5, sizeof(got) - 5) == 0));
    if (!ok) {
        gn::log("kill volumes off: the kill volume test does not look as expected; not available");
        return;
    }
    g_killHooked = gn::hookFunction(f::KillVolume_testCharacter, &killTest, &g_killOrig);
    if (!g_killHooked) gn::log("kill volumes off: could not hook the kill volume test; not available");
}

// ---- movement blockers off -------------------------------------------------------------------------------
// The invisible barriers (PamOBBCollisionEntity, both realms; docs/research/barriers-and-magropes.md):
// each one's body is taken out of its world with the barrier's own disable (0x143335ff0), and put back
// with its enable (server 0x143333310, client 0x143333590), all three reading the realm from the
// entity. Faith collides with the client's, AI with the server's, so both. Barriers stream in with the
// level and their logic can enable them again: each realm looks once a second, on its own thread.
struct BarrierRealm {
    std::vector<void*> off; // barriers this trainer disabled (that realm's thread only)
    float t = 1.f;
    bool was = false;       // g_barriersOff at the last look
};
BarrierRealm g_barServer, g_barClient;
std::atomic<int> g_barriersDisabled{0};

bool isBarrier(void* e) { // the class GridNode read this layout off (its deinit at vtable +0x88)
    void* vt = nullptr;
    void* deinit = nullptr;
    return gn::read(e, vt) && vt && gn::read(static_cast<char*>(vt) + 0x88, deinit) &&
           reinterpret_cast<uint64_t>(deinit) == gn::rva(gn::engine::function::PamOBBCollision_deinit);
}

// One realm's barriers: disabled while the option is on, the trainer's ones enabled again when it goes
// off (or the trainer unloads: putBack). Only live barriers are touched (one that streamed out is gone).
void barriersTick(bool server, float dt, bool putBack = false) {
    namespace f = gn::engine::function;
    BarrierRealm& r = server ? g_barServer : g_barClient;
    const bool want = g_barriersOff && !putBack;
    if (!putBack && (r.t += dt) < 1.f && want == r.was) return;
    r.t = 0.f;
    r.was = want;
    if (!want && r.off.empty()) return;
    using Fn = void (*)(void*);
    const auto disable = gn::function<Fn>(f::PamOBBCollision_disable);
    const auto enable = gn::function<Fn>(server ? f::PamOBBCollision_enable : f::PamOBBCollision_enableClient);
    namespace k = gn::engine::constant;
    std::vector<void*> still;
    gn::entitiesOfClass("OBBCollisionEntity", [&](void* e, gn::Type) {
        uint32_t flags = 0, body = 0xffffffffu;
        uint8_t enabled = 0;
        if (!gn::read(static_cast<char*>(e) + 0x18, flags) || ((flags & 1) != 0) != server || !isBarrier(e)) return;
        if (!gn::read(static_cast<char*>(e) + k::OBBCollision_Enabled, enabled) || !gn::read(static_cast<char*>(e) + k::OBBCollision_BodyId, body))
            return;
        const bool ours = std::find(r.off.begin(), r.off.end(), e) != r.off.end();
        // Only barriers the trainer itself took out go back in, and only with a body: enabling one the
        // game never enabled builds its body from data it may not have (no PhysicsBodies), which read
        // a bad pointer and crashed (PamOBBCollision_enableClient+0x82, 2026-11-04).
        if (want) {
            if (enabled && body != 0xffffffffu) disable(e), still.push_back(e);
            else if (ours) still.push_back(e); // still out (ours since an earlier look)
        } else if (ours && !enabled && body != 0xffffffffu) {
            enable(e);
        }
    });
    r.off = want ? still : std::vector<void*>{};
    if (server) g_barriersDisabled = static_cast<int>(r.off.size());
}

// ---- no city alerts (free roam) ---------------------------------------------------------------------------
// The city alert has one owner, PamServerCityAlertManager (docs/research/city-alert.md); its own
// "deactivate" event (0x798161be, what reaching a safehouse sends) ends an alert. Only outside missions:
// a story mission's chase is its own. KSEC already out in the streets are not taken away (to research).
std::atomic<int> g_alertsEnded{0};

void alertsTick(float dt, bool freeRoam) {
    static float t = 0.f;
    if (!g_noAlerts || !freeRoam || (t += dt) < 0.5f) return;
    t = 0.f;
    gn::entitiesOfClass("PamServerCityAlertManager", [](void* e, gn::Type) {
        uint8_t active = 0;
        if (!gn::read(static_cast<char*>(e) + 0x175, active) || !active) return;
        if (gn::sendEvent(e, "0x798161be") && g_alertsEnded.fetch_add(1) < 20) gn::log("no city alerts: an alert was ended");
    });
}

// ---- no free-roam KSEC ------------------------------------------------------------------------------------
// Every character in the city comes from a PamServerCharacterSpawnEntity (db/annotations/spawn.yaml;
// docs/research/ksec-spawning.md). The spawner's data (+0x30, a CharacterSpawnReferenceObjectData) has
// its Template at +0x198, an asset whose name (+0x10) says what it makes: KSEC are
// "Characters/SpawnTemplates/Enemy/..." (SubProtector, Protector, Enforcer); runners, civilians, Black
// November and the story's people are ".../Male/" and ".../Female/", ghosts ".../Projection/" (the
// trainer's spawner log, 2026-11-04). Team does not tell them apart: all were team 2. A character
// points back at its spawner at +0x138.
// While on and no mission runs: the spawn (spawner vtable +0xd0, ServerCharacterSpawnEntity_
// spawnAtSpawnPoint, which already returns null when the game refuses a spawn) gives null for a KSEC
// spawner, and KSEC already out are despawned. Missions have KSEC of the same templates ("Be Like
// Water"), so a running mission is left alone.
std::atomic<bool> g_noKsec{false};
std::atomic<int> g_ksecBlocked{0}, g_ksecRemoved{0};
using SpawnFn = void* (*)(void* spawner, void* info);
SpawnFn g_spawnOrig = nullptr;
bool g_spawnHooked = false;

std::string assetName(void* asset) {
    void* name = nullptr;
    char b[160] = {};
    if (!asset || !gn::read(static_cast<char*>(asset) + 0x10, name) || !name) return "-";
    gn::api().read(name, b, sizeof(b) - 1);
    size_t n = 0;
    while (n < sizeof(b) - 1 && b[n] >= 32) ++n;
    return n ? std::string(b, n) : std::string("-");
}

std::string spawnerTemplate(void* spawner) {
    void *data = nullptr, *tpl = nullptr;
    if (!spawner || !gn::read(static_cast<char*>(spawner) + 0x30, data) || !data || !gn::read(static_cast<char*>(data) + 0x198, tpl)) return "-";
    return assetName(tpl);
}

// A KSEC spawner, by its template; remembered per spawner (the server thread only: spawns and its tick).
bool ksecSpawner(void* spawner) {
    static std::unordered_map<void*, bool> known;
    if (known.size() > 4096) known.clear(); // spawners come and go with streaming
    if (auto it = known.find(spawner); it != known.end()) return it->second;
    std::string t = spawnerTemplate(spawner);
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool ksec = t.find("/spawntemplates/enemy/") != std::string::npos;
    known.emplace(spawner, ksec);
    return ksec;
}

std::atomic<bool> g_freeRoam{false}; // no mission runs (server tick)

bool faithSpawner(void* spawner) { // her own safe spawn is one of these spawners: never touched
    void* faith = gn::localServerCharacter();
    void* hers = nullptr;
    return faith && gn::read(static_cast<char*>(faith) + 0x138, hers) && hers == spawner;
}

// g_freeRoam is the server tick's view; a mission's KSEC can spawn in the very tick its mission starts
// (the mission's flag is set before its spawners fire), so the mission is asked again before blocking.
bool stillFreeRoam() {
    int32_t mission = 0;
    return gn::mission(mission) && mission == 0;
}

void* spawnDetour(void* spawner, void* info) {
    if (g_noKsec && g_freeRoam && !faithSpawner(spawner) && ksecSpawner(spawner) && stillFreeRoam()) {
        if (g_ksecBlocked.fetch_add(1) < 10) gn::log("no free-roam KSEC: a spawn of %s stopped", spawnerTemplate(spawner).c_str());
        return nullptr; // as the game does when it refuses a spawn
    }
    return g_spawnOrig(spawner, info);
}

void hookSpawns() {
    namespace k = gn::engine::cls::PamServerCharacterSpawnEntity;
    g_spawnHooked = gn::hookSlot(k::vtable + k::vf::spawn, k::va::spawn, &spawnDetour, &g_spawnOrig);
    if (!g_spawnHooked) gn::log("no free-roam KSEC: the spawner's spawn could not be hooked; not available");
}

// Server tick, once a second while on: KSEC already in the city go (a mission's are left).
void ksecTick(float dt) {
    static float t = 0.f;
    if (!g_noKsec || !g_freeRoam || (t += dt) < 1.f) return;
    t = 0.f;
    if (!stillFreeRoam()) return;
    void* faith = gn::localServerCharacter();
    gn::entitiesOfClass("ServerCharacterEntity", [&](void* c, gn::Type) {
        void* spawner = nullptr;
        if (c == faith || !gn::read(static_cast<char*>(c) + 0x138, spawner) || !spawner || !ksecSpawner(spawner)) return;
        if (gn::characterDespawn(c) == GN_OK && g_ksecRemoved.fetch_add(1) < 20) gn::log("no free-roam KSEC: a %s taken away", spawnerTemplate(spawner).c_str());
    });
}

// The hunter drone is not a character: it is a PamServerDroneActorEntity (a ServerActorEntity) that the
// city alert manager calls in with its output event 0xcef736cc (the drone), fired through
// Entity_fireOutputEvent from its update, also before an alert when the AI senses Faith (read in Ghidra
// 2026-11-05, docs/research/city-alert.md). Left alone it came with No KSEC on and its alert never
// ended (user, 2026-11-05). So that one event from that one manager is dropped in free roam.
constexpr uint32_t kDroneEvent = 0xcef736cc;
using FireOutputFn = uint64_t (*)(void* entity, void* bus, void* data, uint32_t hash);
FireOutputFn g_fireOrig = nullptr;
std::atomic<int> g_dronesStopped{0}, g_dronesSeen{0};

uint64_t fireOutputDetour(void* entity, void* bus, void* data, uint32_t hash) {
    if (hash == kDroneEvent && g_noKsec && g_freeRoam && std::strcmp(gn::Type::of(entity).name(), "PamServerCityAlertManager") == 0 &&
        stillFreeRoam()) {
        if (g_dronesStopped.fetch_add(1) < 10) gn::log("no free-roam KSEC: the city alert's drone call stopped");
        return 0;
    }
    return g_fireOrig(entity, bus, data, hash);
}

void hookDrone() {
    namespace f = gn::engine::function;
    if (!gn::hookFunction(f::Entity_fireOutputEvent, &fireOutputDetour, &g_fireOrig))
        gn::log("no free-roam KSEC: the output event could not be hooked; the drone still comes");
    // A drone that arrives anyway (another way in) is logged, to find that way.
    gn::onCreated("PamServerDroneActorEntity", [](void*, void*) {
        if (g_noKsec && g_freeRoam && g_dronesSeen.fetch_add(1) < 10) gn::log("no free-roam KSEC: a drone was made anyway");
    });
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

void setGodMode(bool on) {
    gn::runOnServer([on] {
        if (!gn::godMode(on)) gn::log("god mode: not available (no Faith?)");
    });
}

// ---- arriving somewhere far ------------------------------------------------------------------------
// After a long teleport the game has not streamed in the ground there yet: she would fall through it
// (or into a kill volume). So she is held at the target (client tick, as noclip does) while the
// server tick casts a ray down, and let go onto the ground once it is there; at most 15 s.
struct Arrival {
    bool on = false, found = false;
    float at[3] = {};
    float groundY = 0.f;
    ULONGLONG start = 0;
    std::string what;
} g_arrival; // (g_m)

void arriveAt(const char* what, const float at[3]) {
    std::lock_guard<std::mutex> lock(g_m);
    g_arrival = Arrival{};
    g_arrival.on = true, g_arrival.what = what, g_arrival.start = GetTickCount64();
    for (int i = 0; i < 3; ++i) g_arrival.at[i] = at[i];
}

// Server tick: is the ground there yet?
void arrivalServer() {
    float at[3];
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_arrival.on || g_arrival.found) return;
        for (int i = 0; i < 3; ++i) at[i] = g_arrival.at[i];
    }
    gn::RayHit hit;
    if (!gn::raycast({at[0], at[1] + 2.f, at[2]}, {at[0], at[1] - 60.f, at[2]}, hit)) return;
    std::lock_guard<std::mutex> lock(g_m);
    if (g_arrival.on) g_arrival.found = true, g_arrival.groundY = hit.position[1];
}

// Client tick: hold her, or let her go. True while she is held.
bool arrivalClient() {
    Arrival a;
    {
        std::lock_guard<std::mutex> lock(g_m);
        a = g_arrival;
    }
    if (!a.on) return false;
    const float waited = (GetTickCount64() - a.start) / 1000.f;
    gn::Movement w{};
    if (a.found || waited > 15.f) {
        {
            std::lock_guard<std::mutex> lock(g_m);
            g_arrival.on = false;
        }
        const float y = a.found ? a.groundY + 0.1f : a.at[1];
        gn::setPosition({a.at[0], y, a.at[2]});
        w.last_ground_y = y;
        w.state = 2;
        gn::setMovement(w, GN_MOVE_LAST_GROUND_Y | GN_MOVE_STATE | GN_MOVE_SPEED);
        char b[128];
        if (a.found) snprintf(b, sizeof(b), "At the %s: the ground loaded after %.1f s", a.what.c_str(), waited);
        else snprintf(b, sizeof(b), "At the %s: no ground under her after 15 s, let go", a.what.c_str());
        status(b);
        return false;
    }
    gn::setPosition({a.at[0], a.at[1], a.at[2]});
    w.last_ground_y = -2000.f; // no fall damage from the hold
    gn::setMovement(w, GN_MOVE_LAST_GROUND_Y | GN_MOVE_SPEED);
    return true;
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
        bool saved;
        {
            std::lock_guard<std::mutex> lock(g_m);
            saved = g_saved;
            t = g_savedAt;
            speed = g_savedSpeed;
        }
        // status() takes g_m itself: called under the lock above it threw (a crash, 2026-11-04).
        if (!saved) return status("Load: nothing saved");
        void* faith = gn::localServerCharacter();
        gn::Vec3 was{};
        const bool distant = faith && gn::positionOf(faith, was) &&
                         std::hypot(was.x - t.trans[0], was.z - t.trans[2]) > 150.f; // likely not streamed in
        if (!faith || gn::teleport(faith, t) != GN_OK) return status("Load: teleport refused");
        if (distant) {
            arriveAt("saved position", t.trans);
            return status("Loaded far away: waiting for the ground there");
        }
        gn::runOnClient([speed] {
            gn::Movement m{};
            m.last_ground_y = -2000.f; // no fall damage from where Faith was
            m.speed = speed;           // keep the saved run speed
            gn::setMovement(m, GN_MOVE_LAST_GROUND_Y | GN_MOVE_SPEED);
        });
        status("Loaded");
    });
}

// ---- noclip: GridNode's free camera flies, and Faith is put where it is when noclip goes off --------------
// Writing her position every tick (as MEC_Trainer did) left her character in the game's physics: she
// grabbed ledges and walls on the way and fell between ticks (user, 2026-11-04). The free camera has no
// collision. No god mode with it: her last ground is kept far below (-300) so the landing is not a fall.
std::atomic<bool> g_noclipActive{false};
bool g_noclipFlew = false; // the free camera was seen flying since noclip went on (it starts at the next frame; client thread)
constexpr float kNoFallGround = -300.f;
float g_noclipHold[3] = {}; // where Faith floats while noclip flies (client thread)

void noclipOn() {
    if (!gn::freecamOn() && !gn::freecam(true)) {
        g_noclip = false;
        return status("Noclip: not available (no Faith)");
    }
    g_noclipActive = true;
    g_noclipFlew = false;
    // Faith floats where she was while the camera flies (user, 2026-11-05): held there each client tick.
    // Sending her to her safe spawn instead killed her when noclip went on in the air.
    g_noclipHold[0] = g_px.load(), g_noclipHold[1] = g_py.load(), g_noclipHold[2] = g_pz.load();
}

// Client tick while noclip flies: Faith stays put, no speed, no fall counted.
void noclipHoldFaith() {
    gn::setPosition({g_noclipHold[0], g_noclipHold[1], g_noclipHold[2]});
    gn::Movement w{};
    w.last_ground_y = -2000.f;
    gn::setMovement(w, GN_MOVE_LAST_GROUND_Y | GN_MOVE_SPEED);
}

// Faith to the camera, facing the way it looks; held there while the ground streams in when far.
void noclipOff() {
    g_noclipActive = false;
    {
        std::lock_guard<std::mutex> lock(g_m); // an arrival still running would hold her elsewhere
        g_arrival.on = false;
    }
    gn::Freecam c{};
    if (!gn::freecamState(c) || !c.on) return; // the free camera was left some other way: she stays
    const float at[3] = {c.position[0], c.position[1], c.position[2]};
    const bool distant = std::hypot(at[0] - g_px.load(), at[2] - g_pz.load()) > 150.f; // likely not streamed in
    if (!gn::teleportToFreecam(false)) {
        gn::freecam(false);
        return status("Noclip: the teleport was refused");
    }
    if (distant) arriveAt("noclip's end", at);
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

DWORD g_bhopArmedAt = 0, g_lastHop = 0;

// ---- bhop practice: how the player's own jump lined up with the landing --------------------------------
// The player's jump comes from gn::actionHeld (their real input, whatever it is bound to): its press is
// the first client tick it is held. A bhop landing is a LANDED event out of a wall run or swing; the
// state right after it says hit or miss (onMove). A miss with a press before the landing was early
// (released before it); one with the press after the landing, within 0.4 s, was late. Times are client
// ticks, 33 ms apart, so a result is good to a tick. Presses the trainer made (Arm, Auto) are not judged.
struct Practice {
    ULONGLONG pressMs = 0;   // the player's last jump press
    ULONGLONG airMs = 0;     // the last take-off: the press that made it is not the bhop's
    ULONGLONG landMs = 0;    // the last bhop landing
    int landFrom = 0;        // its state (8 / 25)
    ULONGLONG waitLate = 0;  // a miss with no press before it: a press until this time is "late"
    std::string text;        // the last result, shown for a moment
    ImU32 colour = 0;
    ULONGLONG shownAt = 0;
};
std::mutex g_practiceM;
Practice g_practice; // (g_practiceM)
bool g_jumpWas = false; // client thread

ULONGLONG nowMs() { return GetTickCount64(); }

void practiceResult(const std::string& text, ImU32 colour) {
    std::lock_guard<std::mutex> lock(g_practiceM);
    g_practice.text = text, g_practice.colour = colour, g_practice.shownAt = nowMs();
}

// Client tick: the player's jump presses.
void practiceTick() {
    const int held = gn::actionHeld(GN_ACTION_JUMP);
    if (held < 0) return;
    const bool down = held != 0;
    if (down && !g_jumpWas) {
        const ULONGLONG t = nowMs();
        ULONGLONG lateFrom = 0;
        {
            std::lock_guard<std::mutex> lock(g_practiceM);
            g_practice.pressMs = t;
            if (g_practice.waitLate && t <= g_practice.waitLate) lateFrom = g_practice.landMs, g_practice.waitLate = 0;
        }
        if (lateFrom) {
            char b[64];
            snprintf(b, sizeof(b), "LATE by %llu ms", static_cast<unsigned long long>(t - lateFrom));
            practiceResult(b, IM_COL32(255, 120, 60, 255));
        }
    }
    g_jumpWas = down;
}

void onMove(const gn::MovementEvent& e, const gn::Movement&) {
    if (g_logMoves)
        gn::log("movement: %s, state %d -> %d", e.kind == GN_MOVE_EVENT_LANDED ? "landed" : e.kind == GN_MOVE_EVENT_AIRBORNE ? "airborne" : "state",
                e.old_state, e.state);
    if (e.kind == GN_MOVE_EVENT_AIRBORNE) {
        std::lock_guard<std::mutex> lock(g_practiceM);
        g_practice.airMs = nowMs();
    }
    if (e.kind == GN_MOVE_EVENT_LANDED) {
        g_chainFrom = e.old_state == 8 || e.old_state == 25 ? e.old_state : 0;
        if (!g_chainFrom) g_chain = 0;
        if (g_chainFrom == 8) { // the practice judges wall run landings only
            std::lock_guard<std::mutex> lock(g_practiceM);
            g_practice.landMs = nowMs(), g_practice.landFrom = g_chainFrom;
        }
    } else if (e.kind == GN_MOVE_EVENT_STATE && e.state != 0 && g_chainFrom) {
        const int from = g_chainFrom;
        const bool hit = e.state == from;
        if (hit) {
            const int n = g_chain.fetch_add(1) + 1;
            if (n > g_bestChain) g_bestChain = n;
        } else {
            g_chain = 0;
        }
        g_chainFrom = 0;
        // The practice result, for the player's own presses only (not one the trainer just made).
        if (g_landHud && from == 8 && GetTickCount() - g_lastHop > 500) {
            ULONGLONG press, land, air;
            {
                std::lock_guard<std::mutex> lock(g_practiceM);
                press = g_practice.pressMs, land = g_practice.landMs, air = g_practice.airMs;
            }
            // A press in this jump's air time (after the take-off it made: the wall jump's own press is not it).
            const bool before = press && press <= land && press > air + 100 && land - press < 1000;
            char b[64];
            if (hit) {
                snprintf(b, sizeof(b), before ? "ON TIME (%llu ms before the landing)" : "ON TIME",
                         static_cast<unsigned long long>(before ? land - press : 0));
                practiceResult(b, IM_COL32(60, 230, 100, 255));
            } else if (before) {
                snprintf(b, sizeof(b), "EARLY by %llu ms", static_cast<unsigned long long>(land - press));
                practiceResult(b, IM_COL32(255, 210, 60, 255));
            } else {
                std::lock_guard<std::mutex> lock(g_practiceM);
                g_practice.waitLate = land + 400; // a press soon after is late; none at all: missed
                g_practice.text = "MISSED (no jump)", g_practice.colour = IM_COL32(220, 70, 70, 255), g_practice.shownAt = nowMs();
            }
        }
    }
}

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

// ---- health and the mission (API 22) ------------------------------------------------------------------
std::atomic<float> g_health{-1.f}, g_maxHealth{-1.f};
std::atomic<int> g_mission{0};
std::atomic<bool> g_haveMission{false};
std::string g_missionName; // (g_m)

void healthTick(void* faith) {
    float h = -1.f, m = -1.f;
    if (gn::health(faith, h, m)) g_health = h, g_maxHealth = m;
    else g_health = -1.f;
    int32_t id = 0;
    std::string name;
    g_haveMission = gn::mission(id, &name);
    g_mission = id;
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_missionName = name;
    }
}

void serverTick(float dt) {
    void* faith = gn::localServerCharacter();
    if (faith) healthTick(faith);
    barriersTick(true, dt);
    g_freeRoam = g_haveMission && g_mission == 0;
    alertsTick(dt, g_freeRoam);
    ksecTick(dt);
    arrivalServer();
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
constexpr float kFastLoadSpeed = 3000.f;

// The slider multiplies the game's own time scale rather than replacing it: the game's slow motion
// writes TimeScale every frame while it eases and 1.0 when it ends, which would wipe a plain value. A
// value the trainer didn't write is the game's, kept as the base (from the cinematic tools' research,
// Research_GameSpeed_2026-10-25). Client thread.
// Fast loads use the same path at 3000x while loading (written once, the game's later writes wiped it,
// and the slider at 4x loaded faster; the user, 2026-11-03).
void applyGameSpeed() {
    gn::Object ts = gn::settings("GameTimeSettings");
    float now = 1.f;
    if (!ts.get("TimeScale", now)) return;
    if (g_speedWritten < 0.f || std::fabs(now - g_speedWritten) > 1e-5f) g_speedBase = now;
    const float speed = g_fast ? kFastLoadSpeed : g_timeScale.load();
    if (std::fabs(speed - 1.f) < 1e-4f) {
        if (g_speedWritten >= 0.f) ts.set("TimeScale", g_speedBase), g_speedWritten = -1.f; // hand it back
        return;
    }
    const float want = g_speedBase * speed;
    if (std::fabs(now - want) > 1e-6f) ts.set("TimeScale", want);
    g_speedWritten = want;
}

void clientTick(float dt) {
    const gn::GameState gs = gn::gameState();
    g_loading = gs.loading != 0;
    g_timeOfDay = gs.time_of_day;

    // Fast loads: the game's clock at 3000x, but only while loading.
    g_fast = g_fastLoads && gs.loading;
    applyGameSpeed();

    void* faith = gn::localClientCharacter();
    gn::Vec3 p;
    gn::Movement m{};
    const bool have = faith && gn::positionOf(faith, p) && gn::movement(m);
    g_haveFaith = have;
    static bool havePrev = false;
    if (!have) {
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
    gn::MoveTracker mt;
    g_haveTracker = gn::moveTracker(mt);
    if (g_haveTracker) g_antMove = mt.current_move, g_antJump = mt.in_jump_state != 0, g_antSpeed = mt.moving_speed_xz;

    // Noclip: on and off from here (the client thread, like its hotkey); no fall damage while it flies
    // and until she is down after it (the ray finds the ground under her), at most 15 s.
    static int noclipGrace = 0;
    if (g_noclip && !g_noclipActive) noclipOn();
    else if (!g_noclip && g_noclipActive) noclipOff(), noclipGrace = 450;
    else if (g_noclipActive) {
        const bool flying = gn::freecamOn();
        if (flying) g_noclipFlew = true;
        else if (g_noclipFlew) g_noclip = false, g_noclipActive = false, noclipGrace = 450; // the camera was left another way
    }
    if (g_noclipActive || noclipGrace > 0) {
        if (!g_noclipActive) {
            const float gd = g_groundDist.load();
            // Landed: half a second more. Not in the first third of a second: the teleport is at the next
            // frame, and until then the ray still finds the ground where she stood.
            if (gd >= 0.f && gd < 0.15f && noclipGrace > 15 && noclipGrace < 440) noclipGrace = 15;
            --noclipGrace;
        }
        gn::Movement w{};
        w.last_ground_y = kNoFallGround;
        gn::setMovement(w, GN_MOVE_LAST_GROUND_Y);
    }
    if (g_noclipActive) {
        noclipHoldFaith();
        g_havePrevY = false;
        return;
    }
    if (arrivalClient()) {
        g_havePrevY = false;
        return;
    }
    if (g_landHud) {
        practiceTick();
        gn::Landing lp{};
        const bool fresh = m.on_ground != 0 && gn::landingPrediction(lp) && lp.age_ms < 100 && lp.ticks_to_hit > 0.f;
        g_landTicks = lp.ticks_to_hit, g_landSoft = lp.soft_landing != 0;
        // Only in the wall run jump (state 8), the one bhop the practice is for (user, 2026-11-04); a hit
        // above her feet is not a landing (as auto-bhop).
        g_landShow = fresh && m.state == 8 && lp.hit_height < p.y;
    } else {
        g_landShow = false;
    }
    bhopCheck(m, p.y, dt);

    // God mode: a fall only counts from where Faith is now (falls are deaths in MEC, not damage), so no
    // stumble or fall death either.
    // Kill volumes off: where she last stood, and a fall out of the level (through a street with no
    // collision) puts her back there. Below -300: the game has ground below -50 (user, 2026-11-06).
    static gn::Transform lastStood{};
    static bool haveStood = false;
    if (m.state == 1 && p.y > -250.f) {
        lastStood = gn::Transform{};
        lastStood.right[0] = lastStood.up[1] = lastStood.forward[2] = 1.f;
        lastStood.trans[0] = p.x, lastStood.trans[1] = p.y + 0.5f, lastStood.trans[2] = p.z;
        haveStood = true;
    }
    // The game also kills a long uncontrolled fall (state 0, 100 or more below her last ground; user, 2026-11-06).
    const bool longFall = m.state == 0 && m.last_ground_y - p.y >= 100.f;
    if (g_killOff && haveStood && (p.y < -300.f || longFall) && !g_noclip) {
        const gn::Transform to = lastStood;
        gn::log("kill volumes off: Faith %s; back to (%.1f, %.1f, %.1f)", longFall ? "fell 100 in the air" : "fell below y -300", to.trans[0],
                to.trans[1], to.trans[2]);
        gn::runOnServer([to] {
            if (void* f = gn::localServerCharacter()) gn::teleport(f, to);
        });
        gn::Movement w{};
        w.last_ground_y = to.trans[1];
        gn::setMovement(w, GN_MOVE_LAST_GROUND_Y);
        haveStood = false; // once per fall
    }

    static float killLogT = 0.f;
    if ((killLogT += dt) >= 10.f) {
        killLogT = 0.f;
        if (const uint32_t n = g_killSpared.exchange(0)) gn::log("kill volumes off: %u kill volume tests skipped in 10 s", n);
    }

    if (g_god && m.last_ground_y - p.y >= 1.f && m.state != 3 && m.state != 4) {
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
void playerTab() {
    ImGui::SeparatorText("Faith");
    if (!g_haveFaith) {
        ImGui::TextDisabled("Not in a level");
    } else {
        if (const float h = g_health.load(); h >= 0.f) {
            ImGui::Text("Health %.1f / %.1f", h, g_maxHealth.load());
            ImGui::SameLine();
            if (ImGui::SmallButton("Full")) gn::runOnServer([] {
                float hh, mm;
                if (void* f = gn::localServerCharacter(); f && gn::health(f, hh, mm)) gn::setHealth(f, mm);
            });
        }
        ImGui::Text("Position (%.1f, %.1f, %.1f)", g_px.load(), g_py.load(), g_pz.load());
        ImGui::Text("Speed %.1f m/s   state %d   last ground %.1f", g_hSpeed.load(), g_state.load(), g_lastGroundY.load());
        if (g_haveTracker)
            ImGui::TextDisabled("ANT: move %d%s, speed %.1f m/s", g_antMove.load(), g_antJump ? ", in a jump" : "", g_antSpeed.load());
        if (g_haveMission) {
            std::lock_guard<std::mutex> lock(g_m);
            if (g_mission) ImGui::Text("Mission %d %s", g_mission.load(), g_missionName.c_str());
            else ImGui::TextDisabled("Free roam (no mission)");
        }
    }
    ImGui::SeparatorText("Movement");
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

// ---- look: the visual environment (API 29, untested in game) -----------------------------------------
// Each slider holds its value while its box is ticked; unticked, the game has it back. Offsets are in
// the state (docs/research/visual-environment.md).
struct VeSlider {
    const char* label;
    uint32_t offset;
    float lo, hi;
    bool on = false;
    float value = 0.f;
    bool read = false;
};
VeSlider g_ve[] = {
    {"Sun intensity", GN_VE_OUTDOOR_LIGHT + 0x94, 0.f, 20.f},
    {"Sun rotation X", GN_VE_OUTDOOR_LIGHT + 0x84, -360.f, 360.f},
    {"Sun rotation Y", GN_VE_OUTDOOR_LIGHT + 0x88, -360.f, 360.f},
    {"Exposure compensation", GN_VE_TONEMAP + 0x98, -5.f, 5.f},
    {"Vignette opacity", GN_VE_VIGNETTE + 0x20, 0.f, 1.f},
    {"Focus distance (DoF)", GN_VE_DEPTH_OF_FIELD + 0x04, 0.f, 200.f},
};

// ---- streaming around the free camera (API 30, untested in game) ----------------------------------------
void streamSection() {
    static bool on = false;
    static uint32_t handle = 0;
    ImGui::SeparatorText("Streaming");
    ImGui::Checkbox("Load the world around the free camera", &on);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A focus point follows the free camera so the zones around it load. The game may already do\n"
                          "this: if nothing changes far from Faith, it does. Dropped when the camera or this goes off.");
    gn::Freecam f{};
    const bool flying = on && gn::freecamState(f) && f.on;
    if (flying) {
        const gn::Vec3 at{f.position[0], f.position[1], f.position[2]};
        if (handle) gn::streamingFocusMove(handle, at);
        else handle = gn::streamingFocusAdd(at);
        ImGui::TextDisabled(handle ? "Focus point on the camera" : "No focus point (refused: see the log)");
    } else if (handle) {
        gn::streamingFocusRemove(handle);
        handle = 0;
    }
}

std::string g_localized; // the last text lookup (API 31)

void lookSection() {
    for (VeSlider& v : g_ve) {
        ImGui::PushID(v.label);
        if (!v.read) {
            v.read = true;
            gn::visualEnvGet(v.offset, &v.value, sizeof(float));
        }
        bool on = v.on;
        if (ImGui::Checkbox("##on", &on)) {
            v.on = on;
            if (on) gn::visualEnvGet(v.offset, &v.value, sizeof(float));
            else gn::visualEnvRelease(v.offset);
        }
        ImGui::SameLine();
        if (ImGui::SliderFloat(v.label, &v.value, v.lo, v.hi) && v.on) gn::visualEnvSet(v.offset, &v.value, sizeof(float));
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Release all looks")) {
        for (VeSlider& v : g_ve) v.on = false;
        gn::visualEnvRelease();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Each ticked slider holds its value (the game recomputes the look every frame).\n"
                          "The offsets are the cinematic tools'; a wrong one shows as no change, or a flicker.");
}

void worldTab() {
    ImGui::SeparatorText("Hazards");
    bool v;
    if (g_killHooked) {
        v = g_killOff;
        if (ImGui::Checkbox("Kill volumes off", &v)) g_killOff = v;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("No kill volume kills anyone (street kill floors, fans; AI too). The streets have no\n"
                              "collision: falling below y -300 puts her back where she last stood.");
    }
    v = g_barriersOff;
    if (ImGui::Checkbox("Movement blockers off", &v)) g_barriersOff = v;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The level's invisible barriers (no standing, climbing, vaulting or wall running there) are\n"
                          "taken out, and put back when this goes off.");
    if (g_barriersOff) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d nearby)", g_barriersDisabled.load());
    }
    if (g_spawnHooked) {
        v = g_noKsec;
        if (ImGui::Checkbox("No KSEC (free roam)", &v)) g_noKsec = v;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("KSEC and the hunter drone are not spawned in the city, and KSEC out there are taken away. Runners and the\n"
                              "story's people stay. Not during missions: a mission's KSEC are left alone.");
        if (g_noKsec) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%d stopped, %d taken away, %d drone calls stopped)", g_ksecBlocked.load(), g_ksecRemoved.load(), g_dronesStopped.load());
        }
    }
    v = g_noAlerts;
    if (ImGui::Checkbox("No city alerts (free roam)", &v)) g_noAlerts = v;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A KSEC alert ends as soon as it starts, and one already running ends. Not during missions.");
    ImGui::Separator();
    float raw = g_freezeTime ? g_frozenTime.load() : g_timeOfDay.load();
    if (raw >= 0.f) {
        float norm = std::fmod(raw, 86400.f);
        if (norm < 0.f) norm += 86400.f;
        float hours = norm / 3600.f;
        if (ImGui::SliderFloat("Time of day", &hours, 0.f, 23.99f, "%.2f h")) {
            g_frozenTime = hours * 3600.f;
            const float s = hours * 3600.f;
            gn::runOnClient([s] {
                if (g_freezeTime) gn::holdTimeOfDay(s);
                else gn::setTimeOfDay(s);
            });
        }
        bool freeze = g_freezeTime;
        if (ImGui::Checkbox("Freeze time of day", &freeze)) {
            if (freeze) g_frozenTime = norm;
            g_freezeTime = freeze;
            const float held = g_frozenTime;
            gn::runOnClient([freeze, held] {
                if (freeze) gn::holdTimeOfDay(held); // the clock is written back after each world update (API 28)
                else gn::releaseTimeOfDay();
            });
        }
    } else {
        ImGui::TextDisabled("No time of day clock in this level");
    }
    ImGui::Separator();
    float scale = g_timeScale;
    if (ImGui::SliderFloat("Game speed", &scale, 0.05f, 4.f, "%.2fx", ImGuiSliderFlags_Logarithmic)) g_timeScale = scale;
    ImGui::SameLine();
    if (ImGui::SmallButton("1x")) g_timeScale = 1.f;
    // The game simulates at most one 30 Hz tick a frame, so above 1x it reaches frame rate / 30.
    if (g_timeScale > 1.f) {
        const float most = ImGui::GetIO().Framerate / 30.f;
        if (most < g_timeScale) ImGui::TextDisabled("At %.0f fps the game reaches at most %.1fx", ImGui::GetIO().Framerate, most);
    }
    ImGui::TextDisabled("On top of the game's own slow motion");
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
    if (ImGui::CollapsingHeader("Look and streaming (experimental)")) {
        lookSection();
        streamSection();
        ImGui::SeparatorText("Ray test (API 33)");
        if (ImGui::SmallButton("Cast down from Faith, then past the first hit")) {
            const gn::Vec3 from{g_px.load(), g_py.load() + 1.f, g_pz.load()};
            const gn::Vec3 to{from.x, from.y - 40.f, from.z};
            gn::runOnServer([from, to] {
                gn::ObjectHit h1{}, h2{};
                if (!gn::raycastObject(from, to, h1)) return gn::log("ray test: nothing below");
                gn::log("ray test: first hit body %08x at y %.2f (%.2f m)", h1.body, h1.hit.position[1], h1.hit.distance);
                if (gn::raycastFiltered(from, to, {h1.body}, h2))
                    gn::log("ray test: past it, body %08x at y %.2f (%.2f m)", h2.body, h2.hit.position[1], h2.hit.distance);
                else
                    gn::log("ray test: past it, nothing");
            });
        }
        ImGui::SeparatorText("Text lookup");
        ImGui::TextDisabled("Game menu open (pause requests): %s", gn::menuOpen() ? "yes" : "no");
        static char id[96] = "ID_NC_THEVIEW02";
        ImGui::InputText("String id", id, sizeof(id));
        if (ImGui::SmallButton("Look up (shown, and in the log)")) {
            g_localized = gn::localize(id);
            gn::log("localize %s -> %s", id, g_localized.c_str());
        }
        if (!g_localized.empty()) ImGui::TextWrapped("%s", g_localized.c_str());
    }
}

void trainerTab() {
    bool v;
    v = g_god;
    if (ImGui::Checkbox(withKey("God mode", "god").c_str(), &v)) g_god = v, setGodMode(v);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("No damage, no fall deaths, no stumbles.");
    v = g_noclip;
    if (ImGui::Checkbox(withKey("Noclip", "noclip").c_str(), &v)) g_noclip = v;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fly through anything: W A S D, Q / E down / up, Shift / Ctrl faster / slower, wheel: speed.\n"
                          "Off: Faith lands where you are (no fall damage).");
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
}

// Bhop practice, in the middle of the screen (below the crosshair): the game's predicted time to the
// landing as a bar running down to the press threshold, and how the player's last jump lined up.
void landingPractice() {
    if (!g_landHud || !g_haveFaith) return;
    std::string text;
    ImU32 colour = 0;
    {
        std::lock_guard<std::mutex> lock(g_practiceM);
        if (nowMs() - g_practice.shownAt < 1500) text = g_practice.text, colour = g_practice.colour;
    }
    if (!g_landShow && text.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.62f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##mec_bhop", nullptr, f)) {
        const float w = ImGui::GetFontSize() * 16, h = ImGui::GetFontSize() * 0.7f;
        if (g_landShow) {
            const float t = g_landTicks, below = g_bhopPredictBelow;
            const bool soft = g_landSoft, window = t <= below;
            ImGui::Text("Landing in %.0f ms%s", t * 1000.f / 60.f, soft ? "   soft landing: no bhop" : window ? "   JUMP NOW" : "");
            constexpr float kFull = 12.f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = soft ? IM_COL32(220, 60, 60, 255) : window ? IM_COL32(60, 210, 90, 255) : IM_COL32(200, 200, 200, 200);
            dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(0, 0, 0, 120));
            dl->AddRectFilled(at, ImVec2(at.x + w * std::min(t / kFull, 1.f), at.y + h), col);
            const float mx = at.x + w * std::min(below / kFull, 1.f);
            dl->AddLine(ImVec2(mx, at.y - 3.f), ImVec2(mx, at.y + h + 3.f), IM_COL32(255, 255, 255, 255), 2.f);
            ImGui::Dummy(ImVec2(w, h));
        } else {
            ImGui::Dummy(ImVec2(w, 0.f));
        }
        if (!text.empty()) {
            const float x = (ImGui::GetWindowWidth() - ImGui::CalcTextSize(text.c_str()).x) * 0.5f;
            if (x > 0.f) ImGui::SetCursorPosX(x);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colour), "%s", text.c_str());
        }
    }
    ImGui::End();
}

// Every hotkey, rebound here (kept in mods/mec-trainer.ini as key.<id>).
struct KeyRow {
    const char* id;
    const char* label;
};
const KeyRow kKeyRows[] = {{"god", "God mode on / off"},
                           {"noclip", "Noclip on / off"},
                           {"save", "Save position"},
                           {"load", "Load position"},
                           {"bhop_arm", "Bunny hop: arm one hop (auto off)"},
                           {"bhop_auto", "Bunny hop: auto on / off"},
                           {"freecam", "Free camera on / off"},
                           {"tp_crosshair", "Free camera: teleport Faith to the crosshair"},
                           {"tp_freecam", "Free camera: teleport Faith to the camera"},
                           {"vol_show", "Show / hide trigger volumes"},
                           {"vol_pick", "Volumes: picking on / off"},
                           {"vol_log", "Volumes: log the picked trigger"}};

void settingsTab() {
    ImGui::SeparatorText("Keys");
    ImGui::TextDisabled("Click a key, then press the new one. Escape: no key. Backspace: the default.");
    for (const KeyRow& k : kKeyRows) gn::ui::hotkeyRow(k.id, k.label);
    ImGui::TextDisabled("Noclip and the free camera: W A S D, Q / E down / up, Shift / Ctrl faster / slower, wheel: speed.");
    ImGui::SeparatorText("HUD");
    bool v = g_hud;
    if (ImGui::Checkbox("Player info", &v)) g_hud = v;
    ImGui::SameLine();
    v = g_cheatList;
    if (ImGui::Checkbox("Active cheats", &v)) g_cheatList = v;
    v = g_landHud;
    if (ImGui::Checkbox("Bhop practice", &v)) g_landHud = v;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("In the middle of the screen: the time to the landing (the game's prediction) as a bar running\n"
                          "down to the jump window (green: jump now, red: a soft landing, no bhop), and after each wall\n"
                          "run jump's landing whether your own jump was on time, early or late. Wall run jumps only.");
    ImGui::SeparatorText("Unload");
    if (ImGui::Button("Unload the trainer")) gn::unloadSelf(); // everything it changed goes back (GN_MOD_UNLOAD)
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts back what it changed and unloads it; GridNode's Mods list loads it again.");
    ImGui::SameLine();
    if (ImGui::Button("Unload GridNode too")) gn::unloadGridNode();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every mod and GridNode itself: the game as if none of it had loaded.");
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
const BoolSetting kBools[] = {{"same_wall", &g_sameWall},          {"kill_volumes_off", &g_killOff},
                              {"barriers_off", &g_barriersOff},    {"no_city_alerts", &g_noAlerts},
                              {"no_ksec", &g_noKsec},
                              {"inf_climbs", &g_infClimbs},        {"inf_runs", &g_infRuns},
                              {"fast_loads", &g_fastLoads},        {"hud.player_info", &g_hud},
                              {"hud.cheats", &g_cheatList},        {"hud.landing", &g_landHud},
                              {"bhop.auto", &g_bhopAuto},          {"bhop.wall_runs", &g_bhopWall},
                              {"bhop.swings", &g_bhopSwing},       {"bhop.use_prediction", &g_bhopUsePrediction},
                              {"freecam.stay_flying", &g_keepFlying}}; // (a new key: the old one saved the first default, off)
const FloatSetting kFloats[] = {{"bhop.press_below", &g_bhopPredictBelow},
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
        if (ImGui::BeginTabItem("Player")) {
            playerTab();
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
        if (ImGui::BeginTabItem("Settings")) {
            settingsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    saveSettings();
}

// The trainer's keys under GridNode's in the free camera's key list (top right while it flies).
void freecamKeys() {
    if (!gn::freecamOn() || gn::ui::menuOpen()) return;
    auto key = [](const char* id) {
        const std::string k = gn::hotkeyName(id);
        return k.empty() ? std::string("none") : k;
    };
    gn::ui::freecamSeparator("MEC Trainer");
    if (g_noclipActive) gn::ui::freecamRow(key("noclip"), "noclip off: Faith lands here");
    else gn::ui::freecamRow(key("freecam"), "back to Faith");
    gn::ui::freecamRow(key("tp_crosshair"), "teleport Faith to the crosshair");
    gn::ui::freecamRow(key("tp_freecam"), "teleport Faith to the camera");
    gn::ui::freecamRow(key("vol_show"), volumes::anyShown() ? "hide trigger volumes" : "show trigger volumes");
    if (volumes::anyShown()) {
        gn::ui::freecamRow(key("vol_pick"), "pick the trigger in the middle");
        gn::ui::freecamRow(key("vol_log"), "log the picked trigger");
    }
}

// Always on screen while the trainer is loaded (not an option): runs made with it are marked. It says
// nothing about the upload block (user, 2026-11-04); the log does.
void trainerMark() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const char* text = "MEC Trainer Loaded";
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 at(vp->WorkPos.x + (vp->WorkSize.x - size.x) * 0.5f, vp->WorkPos.y + 6.f);
    dl->AddRectFilled(ImVec2(at.x - 8.f, at.y - 3.f), ImVec2(at.x + size.x + 8.f, at.y + size.y + 3.f), IM_COL32(0, 0, 0, 140), 4.f);
    dl->AddText(at, IM_COL32(255, 200, 60, 255), text);
}

void hud() {
    trainerMark();
    landingPractice();
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
            if (g_chain || g_bestChain) ImGui::Text("Bhop chain %d   best %d", g_chain.load(), g_bestChain.load());
        }
        ImGui::End();
    }
    if (g_cheatList) {
        std::string on;
        if (g_god) on += "God  ";
        if (g_noclip) on += "Noclip  ";
        if (g_killOff) on += "NoKillVolumes  ";
        if (g_barriersOff) on += "NoBlockers  ";
        if (g_noAlerts) on += "NoAlerts  ";
        if (g_noKsec) on += "NoKSEC  ";
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

GN_MOD_INFO("mec-trainer", "MEC trainer", "1.9.0", "GridNode", "Practice tools: god mode, noclip, saved positions, bunny hops, free camera, trigger volumes");

namespace {
int trainerGodMode() { return g_god ? 1 : 0; } // offered to other mods as mec-trainer.godMode (int(void))
} // namespace

GN_MOD("mec-trainer") {
    gn::exportFn("godMode", &trainerGodMode);
    if (!gn::ui::onDraw([](bool menuOpen) {
            volumes::draw();
            freecamKeys();
            hud();
            if (menuOpen) window();
        }))
        gn::log("no overlay: hotkeys only");
    loadSettings();
    volumes::loadSettings();
    blockUploads();
    hookKillVolumes();
    hookSpawns();
    hookDrone();
    gn::onLevel([](const char*, bool) { volumes::levelChanged(); });
    // Run on the client thread, as the old key loop did.
    gn::hotkey("god", "God mode on / off", '1', [] { g_god = !g_god; setGodMode(g_god); });
    gn::hotkey("noclip", "Noclip on / off", '2', [] { g_noclip = !g_noclip; });
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
    gn::hotkey("vol_show", "Show / hide trigger volumes", '3', volumes::toggleShow);
    gn::hotkey("vol_pick", "Volumes: picking on / off (the trigger in the middle of the screen lights up)", '0', volumes::togglePick);
    gn::hotkey("vol_log", "Volumes: log the picked trigger", '9', volumes::logAimed);
    gn::onMovement(onMove);
    gn::onClientTick([](float dt) {
        clientTick(dt);
        barriersTick(false, dt);
        volumes::clientTick(dt);
    });
    gn::onServerTick(serverTick);
    gn::log("ready: INSERT for the window; keys in its Settings tab (1 god, 2 noclip, 4 save, 5 load, F bunny hop)");
    return true;
}

// Unloading (GridNode's Mods: Reload, or a full unload of GridNode): everything the trainer changed
// in the game goes back. GridNode takes out its callbacks and hotkeys and waits for the ticks, so the
// tasks posted here run before the trainer is freed.
GN_MOD_UNLOAD() {
    // Only what is not kept in the ini (a frame may still save the settings before the UI goes).
    g_noclip = false, g_noclipActive = false, g_bhopArmed = false, g_freezeTime = false;
    if (g_god.exchange(false)) gn::runOnServer([] { gn::godMode(false); });
    if (gn::freecamOn()) gn::freecam(false);
    if (g_tuned.exchange(false)) {
        gn::Tuning t;
        for (uint32_t i = 0; i < gn::tuningCount(); ++i)
            if (gn::tuningAt(i, t)) gn::setTuning(t.name, t.default_value);
    }
    const auto post = g_postOriginal;
    gn::runOnClient([post] {
        gn::Object ts = gn::settings("GameTimeSettings");
        g_fast = false;
        if (g_speedWritten >= 0.f) ts.set("TimeScale", g_speedBase), g_speedWritten = -1.f;
        for (const auto& [field, on] : post) gn::settings("GlobalPostProcessSettings").set(field, on);
    });
    // The barriers it took out, back in (each realm on its own thread; these tasks run before the free).
    gn::runOnServer([] { barriersTick(true, 0.f, true); });
    gn::runOnClient([] { barriersTick(false, 0.f, true); });
    volumes::unload();
    gn::log("unloading: god mode, noclip, free camera, tuning, time scale, post-processing and barriers put back");
    return true;
}
