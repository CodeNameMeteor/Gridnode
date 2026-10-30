// Faith's movement, the keyboard as the game reads it, movement tuning and game state
// (db/annotations/faith.yaml). Everything here was found with examples/probe for the MEC_Trainer
// port; nothing is patched. Each chain checks what it can on the way (a vtable or reflected type
// where the object has one), and tuning is only written after GridNode has seen its default value
// at that address, so a wrong object reads as "not available" instead of being written to.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/build.hpp"
#include "gridnode/config.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;
namespace k = gn::engine::constant;

uintptr_t ptr(uintptr_t addr) { return addr ? mem::readOr<uintptr_t>(addr) : 0; }
bool vtableIs(uintptr_t obj, uint64_t vtVa) { return obj && mem::readOr<uintptr_t>(obj) == mem::rva(vtVa); }
bool typeIs(uintptr_t obj, const char* name) {
    const reflect::Type* t = obj ? registry().typeOf(mem::ProcessMemory(), obj) : nullptr;
    return t && t->name == name;
}

// ---- the keyboard --------------------------------------------------------------------------------
uintptr_t inputDevices() {
    const uintptr_t d = ptr(mem::rva(e::global::InputDevices));
    return vtableIs(d, k::InputDevices_Vtable) ? d : 0;
}

constexpr int kKeys = 256;
std::mutex g_keyM;
int g_keyTicks[kKeys] = {}; // ticks left to hold, per InputDeviceKeys value (g_keyM)
bool g_keyHeld[kKeys] = {}; // GridNode is holding it (g_keyM)

bool physicallyDown(int key) {
    const UINT vk = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VSC_TO_VK_EX);
    return vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

void setKey(uintptr_t dev, int key, bool down) {
    mem::write<int32_t>(dev + k::Keyboard_Keys + key * 4, down ? 1 : 0);
}

// ---- tuning ----------------------------------------------------------------------------------------
struct Hop {
    uint32_t offset;
    uint64_t vtable; // 0: not checked (the object there has none)
};
struct Tuning {
    const char* name;
    const char* group;
    uint64_t root; // global VA
    std::vector<Hop> hops;
    uint32_t field;
    float def;
    std::atomic<uintptr_t> verifiedAt{0}; // the address the default was seen at
};

// MEC_Trainer's movement properties (src/Features/MovementProperties.cpp), with the vtables the
// probe saw on the way. Defaults are the game's own values.
constexpr uint64_t kRun = e::global::AntMovementTuning, kJump = e::global::JumpTuningRoot, kAnt = e::global::AntAssetDatabase;
const std::vector<Hop> kRunHops = {{0x408, 0}, {0x380, 0}};
const std::vector<Hop> kRunEnd = {{0, 0x141ab8278}};
const std::vector<Hop> kJumpHops = {{0xa48, 0x141c882f0}, {0x120, 0x141acd428}};
const std::vector<Hop> kWcHops = {{0, 0x141abae70}, {0x22b0, 0x141a9bfa8}, {0x30, 0x141a1eb68}};
const std::vector<Hop> kFwcHops = {{0, 0x141abae70}, {0x26f0, 0x141a9bfa8}, {0x58, 0x141a435c8}};
const std::vector<Hop> kWrHops = {{0, 0x141abae70}, {0x1a78, 0x141aaa8c8}};
const std::vector<Hop> kCoilHops = {{0, 0x141abae70}, {0xc8, 0}, {0xc98, 0}};
const std::vector<Hop> kSlideHops = {{0, 0x141abae70}, {0x1780, 0x141aa9ce0}, {0xb88, 0x141aa9ce0}};
const std::vector<Hop> kPullHops = {{0, 0x141abae70}, {0x778, 0x141a442d0}, {0x28, 0x141a1cde8}};
const std::vector<Hop> kSwingHops = {{0, 0x141abae70}, {0x2458, 0x141a10f68}, {0xd0, 0x141a9bfa8}};

std::deque<Tuning>& tunings() {
    static std::deque<Tuning> t = [] {
        std::deque<Tuning> v;
        auto add = [&](const char* n, const char* g, uint64_t root, const std::vector<Hop>& hops, uint32_t field, float def) {
            v.emplace_back();
            Tuning& x = v.back();
            x.name = n;
            x.group = g;
            x.root = root;
            x.hops = hops;
            x.field = field;
            x.def = def;
        };
        auto run = [&](const char* n, uint32_t f, float d) {
            std::vector<Hop> h = kRunHops;
            h.push_back(kRunEnd[0]);
            add(n, "Running", kRun, h, f, d);
        };
        run("MaxRunSpeed", 0x1c5c, 7.2f);
        run("TurningDeceleration", 0x1c54, 20.f);
        run("DirectionChange", 0x1c50, 7.f);
        run("AboveMaxSpeedDeceleration", 0x1c4c, 10.f);
        run("ForwardFriction", 0x1c40, 3.f);
        run("BackwardFriction", 0x1c44, 6.f);
        run("StrafeFriction", 0x1c48, 5.f);
        auto jump = [&](const char* n, uint32_t f, float d) { add(n, "Jumping", kJump, kJumpHops, f, d); };
        jump("MaxSpeedJumpHeight", 0x8b4, 1.2f);
        jump("MaxSpeedJumpVelocity", 0x8b0, 8.04f);
        jump("NonMaxSpeedJumpHeight", 0x87c, 1.1f);
        jump("NonMaxSpeedJumpVelocity", 0x878, 5.96f);
        jump("LowSpeedJumpHeight", 0x64c, 1.1f);
        jump("LowSpeedJumpVelocity", 0x648, 2.28f);
        jump("StandingJumpHeight", 0x80c, 1.1f);
        jump("StandingJumpVelocity", 0x808, 0.f);
        jump("WalkingJumpHeight", 0x7d4, 1.1f);
        jump("WalkingJumpVelocity", 0x7d0, 4.12f);
        add("WallClimbMaxAngleDelta", "Wall climb", kAnt, kWcHops, 0x5bc, 40.f);
        add("WallClimbRotationSpeed", "Wall climb", kAnt, kWcHops, 0x5c0, 300.f);
        add("FirstWallClimbHeight", "Wall climb", kAnt, kFwcHops, 0x380, 2.6f);
        add("FirstWallClimbTimeMs", "Wall climb", kAnt, kWcHops, 0x5a4, 100.f);
        add("SecondWallClimbHeight", "Wall climb", kAnt, kWcHops, 0x598, 2.6f);
        add("SecondWallClimbTimeMs", "Wall climb", kAnt, kWcHops, 0x5b0, 100.f);
        add("WallRunMinimumLength", "Wall run", kAnt, kWrHops, 0xe0, 4.f);
        add("WallRunMaximumLength", "Wall run", kAnt, kWrHops, 0xe4, 10.f);
        add("FirstWallRunHeight", "Wall run", kAnt, kWrHops, 0xe8, 1.2f);
        add("FirstWallRunTime", "Wall run", kAnt, kWrHops, 0x104, 80.f);
        add("SecondWallRunHeight", "Wall run", kAnt, kWrHops, 0xf8, 1.2f);
        add("SecondWallRunTime", "Wall run", kAnt, kWrHops, 0x110, 64.f);
        add("CoilHeight", "Coil and slide", kAnt, kCoilHops, 0x1fd0, 0.64f);
        add("CoilFallSpeed", "Coil and slide", kAnt, kCoilHops, 0x1fd8, 20.f);
        add("MaxSlideSpeed", "Coil and slide", kAnt, kSlideHops, 0x63c, 12.f);
        add("MaxSlideStrafeSpeed", "Coil and slide", kAnt, kSlideHops, 0x630, 3.f);
        add("SlideRotationSpeed", "Coil and slide", kAnt, kSlideHops, 0x638, 1080.f);
        add("MagRopePullupAcceleration", "Mag rope", kAnt, kPullHops, 0x324, 20.f);
        add("MagRopePullupDeceleration", "Mag rope", kAnt, kPullHops, 0x330, 4.f);
        add("MagRopePullupGravity", "Mag rope", kAnt, kPullHops, 0x334, 14.f);
        add("MagRopeSwingGravity", "Mag rope", kAnt, kSwingHops, 0xd8, 30.f);
        add("MagRopeSwingAcceleration", "Mag rope", kAnt, kSwingHops, 0xc8, 7.f);
        return v;
    }();
    return t;
}

// The field's address, or 0 when a hop is null or not the object expected. A hop {0, vt} checks
// the object reached so far without moving.
uintptr_t resolve(const Tuning& t) {
    uintptr_t obj = ptr(mem::rva(t.root));
    for (const Hop& h : t.hops) {
        if (!obj) return 0;
        if (h.offset) obj = ptr(obj + h.offset);
        if (!obj || (h.vtable && !vtableIs(obj, h.vtable))) return 0;
    }
    return obj ? obj + t.field : 0;
}

// Has the default been seen at this address? (Checked when read; values the user set are kept.)
uintptr_t verified(Tuning& t) {
    const uintptr_t a = resolve(t);
    if (!a) return 0;
    if (t.verifiedAt.load() == a) return a;
    float v = 0;
    if (mem::read(a, v) && std::fabs(v - t.def) <= 1e-3f * (1.f + std::fabs(t.def))) {
        t.verifiedAt = a;
        return a;
    }
    return 0;
}

Tuning* findTuning(const char* name) {
    for (Tuning& t : tunings())
        if (_stricmp(t.name, name) == 0) return &t;
    return nullptr;
}

// ---- Faith's movement -----------------------------------------------------------------------------
struct MovementPtrs {
    uintptr_t ground = 0, state = 0, camera = 0;
};
MovementPtrs movementPtrs() {
    MovementPtrs p;
    const uintptr_t c = reinterpret_cast<uintptr_t>(localClientCharacter());
    if (!c) return p;
    const uintptr_t owner = ptr(c + e::cls::PamClientCharacterEntity::off::movementOwner);
    const uintptr_t mv = ptr(ptr(owner + k::MovementOwner_Movement));
    if (mv) {
        p.ground = ptr(mv + k::Movement_Ground);
        p.state = ptr(mv + k::Movement_State);
    }
    const uintptr_t cam = ptr(c + k::Character_Camera);
    if (typeIs(cam, "PamClientCharacterCameraComponent")) p.camera = cam;
    return p;
}

} // namespace

// ---- keys (client tick applies and releases holds) ----------------------------------------------------
int keyFind(const char* name) {
    if (!name || !*name) return -1;
    const reflect::Type* t = registry().find("InputDeviceKeys");
    if (!t) return -1;
    const std::string want = name;
    for (const reflect::EnumValue& v : t->values)
        if (_stricmp(v.name.c_str(), want.c_str()) == 0 || (v.name.size() > 4 && _stricmp(v.name.c_str() + 4, want.c_str()) == 0))
            return v.value;
    return -1;
}

int keyPress(int key, uint32_t ticks) {
    if (key <= 0 || key >= kKeys) return GN_ERR_ARGUMENT;
    const uintptr_t dev = inputDevices();
    if (!dev) return GN_ERR_UNAVAILABLE;
    std::lock_guard<std::mutex> lock(g_keyM);
    g_keyTicks[key] = static_cast<int>(ticks);
    if (ticks) {
        // Down now, not at the next client tick: a press lands in the very next frame the game reads.
        setKey(dev, key, true);
        g_keyHeld[key] = true;
    }
    return GN_OK;
}

// ---- input actions (db/annotations/input.yaml) ------------------------------------------------------
// Keys become actions in the player's EntryInputState; the animation system's input component reads
// them, and reads ClientPlayer's override state instead when the player has flag 0x40. GridNode
// presses an action by copying the live state into the override, setting the action, and raising
// the flag for as long as the action is held. Actions are what key bindings map to, so this works
// whatever key the player bound.
namespace {
namespace ka = gn::engine::constant;
std::mutex g_actM;
struct HeldAction {
    uint32_t hash;
    int ticks;
};
std::vector<HeldAction> g_actions; // (g_actM)
bool g_overrideOn = false;          // GridNode raised flag 0x40 (g_actM)
std::atomic<uintptr_t> g_override{0}, g_overridePlayer{0};

// Faith's ClientPlayer, checked by its back pointer.
uintptr_t clientPlayer() {
    const uintptr_t c = reinterpret_cast<uintptr_t>(localClientCharacter());
    const uintptr_t p = ptr(c + e::cls::PamClientCharacterEntity::off::player);
    return p && ptr(p + ka::ClientPlayer_Character) == c ? p : 0;
}

// The action's bit in EntryInputState.digitalBits, or -1 (not a digital action / no registry).
int digitalBit(uint32_t hash) {
    const uintptr_t reg = ptr(mem::rva(e::global::EntryInputActionRegistryPtr));
    if (!reg) return -1;
    const uintptr_t buckets = ptr(reg + e::st::EntryInputActionRegistry::off::buckets);
    const uint32_t count = mem::readOr<uint32_t>(reg + e::st::EntryInputActionRegistry::off::bucketCount);
    if (!buckets || !count || count > 0x10000) return -1;
    const uintptr_t sentinel = ptr(buckets + count * 8ull);
    for (uintptr_t n = ptr(buckets + (hash % count) * 8ull), guard = 0; n && n != sentinel && guard < 256; n = ptr(n + 8), ++guard) {
        uint32_t hi[2] = {};
        if (!mem::read(n, hi, sizeof(hi))) return -1;
        if (hi[0] != hash) continue;
        const uint32_t first = mem::readOr<uint32_t>(reg + e::st::EntryInputActionRegistry::off::digitalFirst);
        const uint32_t end = mem::readOr<uint32_t>(reg + e::st::EntryInputActionRegistry::off::end);
        return hi[1] >= first && hi[1] < end && hi[1] - first < 64 ? static_cast<int>(hi[1] - first) : -1;
    }
    return -1;
}

// Copies the live state into the override, sets the held actions, raises the flag (g_actM held).
bool applyActions() {
    const uintptr_t player = g_overridePlayer.load(), st = g_override.load();
    if (!player || !st || player != clientPlayer()) return false;
    const uintptr_t live = ptr(player + ka::ClientPlayer_InputState);
    if (!live) return false;
    uint8_t buf[ka::EntryInputState_Size];
    if (!mem::read(live, buf, sizeof(buf))) return false;
    uint64_t bits = 0;
    memcpy(&bits, buf + ka::EntryInputState_Digital, 8);
    for (const HeldAction& a : g_actions) {
        const int bit = digitalBit(a.hash);
        if (bit >= 0) bits |= 1ull << bit;
    }
    memcpy(buf + ka::EntryInputState_Digital, &bits, 8);
    mem::write(st, buf, sizeof(buf));
    const uint8_t flags = mem::readOr<uint8_t>(player + ka::ClientPlayer_InputFlags);
    mem::write<uint8_t>(player + ka::ClientPlayer_InputFlags, flags | ka::InputFlag_UseOverride);
    g_overrideOn = true;
    return true;
}

void releaseActions() { // g_actM held
    if (!g_overrideOn) return;
    g_overrideOn = false;
    const uintptr_t player = g_overridePlayer.load();
    if (!player || player != clientPlayer()) return;
    const uint8_t flags = mem::readOr<uint8_t>(player + ka::ClientPlayer_InputFlags);
    mem::write<uint8_t>(player + ka::ClientPlayer_InputFlags, flags & ~ka::InputFlag_UseOverride);
}

// Client thread: make sure the override state exists for Faith's player (the game makes it).
void prepareOverride() {
    const uintptr_t player = clientPlayer();
    if (!player) {
        g_overridePlayer = 0;
        g_override = 0;
        return;
    }
    if (player == g_overridePlayer.load() && g_override.load()) return;
    if (!build::codeMatches("ClientPlayer_overrideInputState")) {
        static bool said = false;
        if (!said) GN_LOG("input: the override input function is not as expected; jump() presses Space instead");
        said = true;
        return;
    }
    const uint8_t before = mem::readOr<uint8_t>(player + ka::ClientPlayer_InputFlags);
    using Fn = uintptr_t (*)(uintptr_t);
    const uintptr_t st = reinterpret_cast<Fn>(mem::rva(e::function::ClientPlayer_overrideInputState))(player);
    // Leave the "made" flag as it was: GridNode only uses the state while it holds an action.
    const uint8_t after = mem::readOr<uint8_t>(player + ka::ClientPlayer_InputFlags);
    mem::write<uint8_t>(player + ka::ClientPlayer_InputFlags, static_cast<uint8_t>((after & ~ka::InputFlag_OverrideMade) | (before & ka::InputFlag_OverrideMade)));
    g_override = st;
    g_overridePlayer = st ? player : 0;
    GN_LOG("input: actions ready for player %llx (override state %llx, jump is bit %d)", static_cast<unsigned long long>(player),
           static_cast<unsigned long long>(st), digitalBit(static_cast<uint32_t>(k::Action_Jump)));
}
} // namespace

int actionPress(uint32_t hash, uint32_t ticks) {
    if (digitalBit(hash) < 0) return GN_ERR_NOT_FOUND;
    std::lock_guard<std::mutex> lock(g_actM);
    if (!g_override.load()) return GN_ERR_UNAVAILABLE;
    bool found = false;
    for (HeldAction& a : g_actions)
        if (a.hash == hash) a.ticks = static_cast<int>(ticks), found = true;
    if (!found && ticks) g_actions.push_back({hash, static_cast<int>(ticks)});
    // Pressed now, not at the next client tick, so it lands in the next frame the game reads.
    if (ticks && !applyActions()) return GN_ERR_UNAVAILABLE;
    return GN_OK;
}

void faithClientTick() {
    prepareOverride();
    {
        std::lock_guard<std::mutex> lock(g_actM);
        for (HeldAction& a : g_actions) --a.ticks;
        g_actions.erase(std::remove_if(g_actions.begin(), g_actions.end(), [](const HeldAction& a) { return a.ticks < 0; }), g_actions.end());
        if (g_actions.empty()) releaseActions();
        else if (!applyActions()) releaseActions();
    }
    const uintptr_t dev = inputDevices();
    std::lock_guard<std::mutex> lock(g_keyM);
    for (int key = 1; key < kKeys; ++key) {
        if (g_keyTicks[key] > 0) {
            if (dev) setKey(dev, key, true);
            g_keyHeld[key] = true;
            --g_keyTicks[key];
        } else if (g_keyHeld[key]) {
            if (dev && !physicallyDown(key)) setKey(dev, key, false);
            g_keyHeld[key] = false;
        }
    }
}

int jumpHeld() {
    // The player's own jump input (the live state; an action GridNode presses is in the override).
    const int bit = digitalBit(static_cast<uint32_t>(k::Action_Jump));
    const uintptr_t player = clientPlayer();
    const uintptr_t live = player ? ptr(player + ka::ClientPlayer_InputState) : 0;
    uint64_t bits = 0;
    if (bit < 0 || !live || !mem::read(live + ka::EntryInputState_Digital, bits)) return -1;
    return (bits >> bit) & 1 ? 1 : 0;
}

int jumpHeldByMods() {
    // GridNode (for a mod) is holding the jump action or the Space key it falls back to.
    {
        std::lock_guard<std::mutex> lock(g_actM);
        if (g_overrideOn)
            for (const HeldAction& a : g_actions)
                if (a.hash == static_cast<uint32_t>(k::Action_Jump)) return 1;
    }
    static const int space = keyFind("IDK_Space");
    const int key = space > 0 ? space : 57;
    std::lock_guard<std::mutex> lock(g_keyM);
    return g_keyHeld[key] || g_keyTicks[key] > 0 ? 1 : 0;
}

int jump() {
    // The jump action, whatever key the player bound to it; the game decides what it becomes (a
    // jump, a vault, a coil...). Four ticks, as a quick tap. Falls back to the Space key.
    if (actionPress(static_cast<uint32_t>(k::Action_Jump), 4) == GN_OK) return GN_OK;
    static const int space = keyFind("IDK_Space");
    return keyPress(space > 0 ? space : 57, 4);
}

// ---- movement -------------------------------------------------------------------------------------
int movementGet(gn_movement* out) {
    if (!out) return GN_ERR_ARGUMENT;
    *out = gn_movement{};
    const MovementPtrs p = movementPtrs();
    if (!p.state || !p.ground) return GN_ERR_UNAVAILABLE;
    mem::read(p.state + k::State_Movement, out->state);
    mem::read(p.state + k::State_WallCount, out->wall_count);
    mem::read(p.state + k::State_WallClimbs, out->wall_climbs);
    mem::read(p.state + k::State_WallRuns, out->wall_runs);
    mem::read(p.ground + k::Ground_LastY, out->last_ground_y);
    out->on_ground = mem::readOr<uint8_t>(p.ground + k::Ground_OnGround);
    float vel[3] = {};
    if (mem::read(p.ground + k::Ground_Velocity, vel, sizeof(vel))) out->speed = std::sqrt(vel[0] * vel[0] + vel[2] * vel[2]);
    if (p.camera) mem::read(p.camera + k::Camera_Forward, out->camera_forward, sizeof(out->camera_forward));
    return GN_OK;
}

int movementSet(const gn_movement* in, uint32_t fields) {
    if (!in) return GN_ERR_ARGUMENT;
    if (((fields & GN_MOVE_SPEED) && !std::isfinite(in->speed)) || ((fields & GN_MOVE_LAST_GROUND_Y) && !std::isfinite(in->last_ground_y)))
        return GN_ERR_ARGUMENT; // never nan / inf into the game
    const MovementPtrs p = movementPtrs();
    if (!p.state || !p.ground) return GN_ERR_UNAVAILABLE;
    if (fields & GN_MOVE_STATE) mem::write(p.state + k::State_Movement, in->state);
    if (fields & GN_MOVE_WALL_COUNT) mem::write(p.state + k::State_WallCount, in->wall_count);
    if (fields & GN_MOVE_WALL_CLIMBS) mem::write(p.state + k::State_WallClimbs, in->wall_climbs);
    if (fields & GN_MOVE_WALL_RUNS) mem::write(p.state + k::State_WallRuns, in->wall_runs);
    if (fields & GN_MOVE_LAST_GROUND_Y) mem::write(p.ground + k::Ground_LastY, in->last_ground_y);
    if (fields & GN_MOVE_SPEED) {
        // Her velocity, scaled to the speed (its direction kept; standing still, her camera's). The old
        // trainer's field (round values while running, so likely the speed she is driven at) gets it
        // too, by its proper path: slot +0x1e8 of the object at 0x1423dd6f8. Which one the game keeps
        // through a position load is untested.
        float vel[3] = {};
        if (mem::read(p.ground + k::Ground_Velocity, vel, sizeof(vel))) {
            float len = std::sqrt(vel[0] * vel[0] + vel[2] * vel[2]), dx = vel[0], dz = vel[2];
            float fwd[4] = {};
            if (len < 0.01f && p.camera && mem::read(p.camera + k::Camera_Forward, fwd, sizeof(fwd)))
                dx = fwd[0], dz = fwd[2], len = std::sqrt(dx * dx + dz * dz);
            if (len >= 0.01f) {
                vel[0] = dx / len * in->speed, vel[2] = dz / len * in->speed;
                mem::write(p.ground + k::Ground_Velocity, vel, sizeof(vel));
            }
        }
        const uintptr_t drive = ptr(ptr(ptr(mem::rva(k::Speed_DriveGlobal)) + k::Speed_DriveSlot) + k::Body_Motion);
        if (drive) mem::write(drive + k::Motion_Speed, in->speed);
    }
    return GN_OK;
}

// Faith's position as the animation system holds it (MEC_Trainer's playerX chain): character +0xd0
// -> +0x128 (an Animatable, checked) -> +0x30 -> +0x50 (Vec3). Writing it moves Faith on the client
// without a teleport, so the camera stays free (noclip).
int positionSet(const float pos[3]) {
    if (!pos || !std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) return GN_ERR_ARGUMENT;
    const uintptr_t c = reinterpret_cast<uintptr_t>(localClientCharacter());
    if (!c) return GN_ERR_UNAVAILABLE;
    const uintptr_t anim = ptr(ptr(c + k::Character_AnimatableOwner) + k::AnimatableOwner_Animatable);
    if (!typeIs(anim, "Animatable")) return GN_ERR_UNAVAILABLE;
    const uintptr_t pose = ptr(anim + k::Animatable_Pose);
    if (!pose) return GN_ERR_UNAVAILABLE;
    return mem::write(pose + k::Pose_Position, pos, 3 * sizeof(float)) ? GN_OK : GN_ERR_UNAVAILABLE;
}

// God mode: the IsImmortal property of Faith's server health component. PamServerHealthComponent's
// constructor (0x14391e410) connects +0xc0 to the data bus property "IsImmortal" (hash 0xa1b7b692,
// default PamHealthComponentData.IsImmortal, via 0x14474f280); +0xc0 -> node, node +0 -> the bool.
// Setting that bool is what the game's own logic would do to make her immortal, and it is per
// component, so nothing shared changes. MEC_Trainer wrote 4 bytes there; this writes the one byte.
// The component is new after every respawn: call it again (the trainer does, each server tick).
// Server thread.
namespace {
uintptr_t g_godAt = 0;
uint8_t g_godOriginal = 0;
}

int godModeSet(int on) {
    if (!isServerThread()) return GN_ERR_THREAD;
    const uintptr_t sf = reinterpret_cast<uintptr_t>(localServerCharacter());
    if (!sf) return GN_ERR_UNAVAILABLE;
    const reflect::Type* hcType = registry().find("PamServerHumanHealthComponent");
    const uintptr_t hc = hcType ? reinterpret_cast<uintptr_t>(getComponent(reinterpret_cast<void*>(sf), reinterpret_cast<const gn_type*>(hcType))) : 0;
    if (!typeIs(hc, "PamServerHumanHealthComponent")) return GN_ERR_UNAVAILABLE;
    const uintptr_t at = ptr(ptr(hc + k::HealthComponent_ImmortalProperty));
    if (!at) return GN_ERR_UNAVAILABLE;
    const uint8_t now = mem::readOr<uint8_t>(at, 0xff);
    if (now > 1) { // not a bool: refuse rather than write
        GN_LOG("god mode: the IsImmortal property does not hold a bool (%u); not changed", now);
        return GN_ERR_UNAVAILABLE;
    }
    if (on) {
        if (g_godAt != at) {
            g_godAt = at;
            g_godOriginal = now;
            GN_LOG("god mode: on (health component %llx, IsImmortal was %u)", static_cast<unsigned long long>(hc), now);
        }
        if (!now) mem::write<uint8_t>(at, 1);
    } else if (g_godAt == at) {
        mem::write<uint8_t>(at, g_godOriginal);
        g_godAt = 0;
        GN_LOG("god mode: off");
    }
    return GN_OK;
}

// ---- game state -------------------------------------------------------------------------------------
namespace {
uintptr_t timeOfDayClock() {
    const uintptr_t m = ptr(mem::rva(e::global::TimeOfDayManager));
    const uintptr_t clock = ptr(ptr(m + k::TimeOfDay_Clock1) + k::TimeOfDay_Clock2);
    return typeIs(clock, "ServerTimeOfDayClockEntity") ? clock : 0;
}
uintptr_t sharedContent() {
    const uintptr_t c = ptr(ptr(mem::rva(e::global::SharedContentManagerHolder)) + k::SharedContent_Manager);
    return typeIs(c, "PamServerSharedContentManagerEntity") ? c : 0;
}
} // namespace

int gameStateGet(gn_game_state* out) {
    if (!out) return GN_ERR_ARGUMENT;
    *out = gn_game_state{};
    out->time_of_day = -1.f;
    const uintptr_t wrs = ptr(mem::rva(e::global::LoadingWorldRenderSettings));
    if (typeIs(wrs, "WorldRenderSettings")) out->loading = mem::readOr<uint8_t>(wrs + k::Loading_Flag);
    out->in_menu = mem::readOr<int32_t>(mem::rva(e::global::InMenu));
    if (const uintptr_t c = sharedContent()) out->content_active = mem::readOr<int32_t>(c + k::SharedContent_Active) == 1;
    if (const uintptr_t d = ptr(mem::rva(e::global::DashState))) out->dash_started = mem::readOr<uint8_t>(d + k::Dash_Started);
    if (const uintptr_t clock = timeOfDayClock()) mem::read(clock + k::Clock_Time, out->time_of_day);
    return GN_OK;
}

int timeOfDaySet(float seconds) {
    if (!std::isfinite(seconds)) return GN_ERR_ARGUMENT;
    const uintptr_t clock = timeOfDayClock();
    if (!clock) return GN_ERR_UNAVAILABLE;
    return mem::write(clock + k::Clock_Time, seconds) ? GN_OK : GN_ERR_UNAVAILABLE;
}

// ---- tuning -----------------------------------------------------------------------------------------
uint32_t tuningCount() { return static_cast<uint32_t>(tunings().size()); }

int tuningAt(uint32_t i, gn_tuning* out) {
    if (!out || i >= tunings().size()) return GN_ERR_ARGUMENT;
    Tuning& t = tunings()[i];
    out->name = t.name;
    out->group = t.group;
    out->default_value = t.def;
    const uintptr_t a = verified(t); // once its default has been seen there, the address stays trusted
    out->value = t.def;
    if (a) mem::read(a, out->value);
    out->available = a != 0;
    return GN_OK;
}

int tuningSet(const char* name, float value) {
    if (!std::isfinite(value)) return GN_ERR_ARGUMENT;
    Tuning* t = name ? findTuning(name) : nullptr;
    if (!t) return GN_ERR_NOT_FOUND;
    const uintptr_t a = resolve(*t);
    if (!a || (t->verifiedAt.load() != a && !verified(*t))) return GN_ERR_UNAVAILABLE;
    return mem::write(a, value) ? GN_OK : GN_ERR_UNAVAILABLE;
}


// ---- research: which field holds her speed ---------------------------------------------------------
// gn_movement.speed (then body motion +0x438) read 0.00 most of the time in game (2026-10-30); this
// found her velocity at movement ground +0xd0 (speed-3 and speed-4), now its source. Kept for checking other fields the same way. The
// Recorder's movement watch calls this every frame: when her position moves on, her horizontal speed
// is measured from it and every float (and every x/z pair of a vector) in the objects around her
// movement is scored by how often it matches (within 15%, while she moves faster than 1 m/s). The best
// are logged every 100 samples (about 10 s of moving), at most five times per run of the game.
namespace {
struct ProbeRegion {
    const char* name;
    uint32_t size;
};
constexpr ProbeRegion kProbeRegions[] = {{"motion", 0x800}, {"body", 0x800}, {"physics holder", 0x400}, {"movement state", 0x200},
                                         {"movement ground", 0x100}};
constexpr size_t kProbeSlots = 0x800 / 4;
struct ProbeScore {
    uint32_t hit[2][kProbeSlots] = {}; // [scalar, x/z pair][offset / 4]
};
std::mutex g_probeM;
ProbeScore g_probe[std::size(kProbeRegions)]; // (g_probeM)
uint32_t g_probeSamples = 0, g_probeReports = 0;
float g_probeX = 0.f, g_probeZ = 0.f, g_probeMeasured = -1.f;
LARGE_INTEGER g_probeAt{};
bool g_probeHave = false;

// The user's old trainer read her speed as [[[0x1423e29b8] + 0x2378] + 0x10] + 0x438 (a Cheat Engine
// pointer path). 0x1423e29b8 holds a 0x2180-byte object (made in 0x142fe1fd0, constructed by
// 0x143017420, fields up to +0x2178), so +0x2378 lies past its end: in whatever the allocator put
// next (the next allocation there is a 0x1b50-byte object, global 0x1423dd6f8). No code reads +0x2378.
// The path ends like ours (+0x10, +0x438), so the question is which object the game's motion hangs
// off: this logs both, so one run in game shows whether they differ and what the old one is.
constexpr uint64_t kOldSpeedGlobal = 0x1423e29b8, kNextAllocGlobal = 0x1423dd6f8;
constexpr uint32_t kOldSpeedOff = 0x2378;

std::string describe(uintptr_t obj) {
    if (!obj) return "null";
    const uintptr_t vt = mem::readOr<uintptr_t>(obj);
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), obj);
    char b[160];
    snprintf(b, sizeof(b), "0x%llx (vtable 0x%llx%s%s)", static_cast<unsigned long long>(obj),
             static_cast<unsigned long long>(mem::inImage(vt) ? mem::va(vt) : vt), t ? ", " : "", t ? t->name.c_str() : "");
    return b;
}

void probeChains(uintptr_t c) {
    const uintptr_t g = ptr(mem::rva(kOldSpeedGlobal));
    const uintptr_t oldX = g ? ptr(g + kOldSpeedOff) : 0, oldMotion = ptr(oldX + 0x10);
    const uintptr_t next = ptr(mem::rva(kNextAllocGlobal));
    const uintptr_t holder = ptr(c + k::Character_PhysicsHolder), body = ptr(holder + k::PhysicsHolder_Body);
    const uintptr_t motion = ptr(body + k::Body_Motion);
    GN_LOG("speed probe: old path: [0x1423e29b8] %s, +0x2378 -> %s (%s+0x%llx from the 0x1b50 object at 0x%llx), +0x10 -> %s, "
           "speed %.2f",
           describe(g).c_str(), describe(oldX).c_str(), g + kOldSpeedOff >= next ? "" : "-",
           static_cast<unsigned long long>(g + kOldSpeedOff >= next ? g + kOldSpeedOff - next : next - g - kOldSpeedOff),
           static_cast<unsigned long long>(next), describe(oldMotion).c_str(), oldMotion ? mem::readOr<float>(oldMotion + 0x438) : -1.f);
    GN_LOG("speed probe: our path: Faith +0x28 %s, +0x1f0 -> %s, +0x10 -> %s, speed %.2f; same body %s, same motion %s",
           describe(holder).c_str(), describe(body).c_str(), describe(motion).c_str(), motion ? mem::readOr<float>(motion + 0x438) : -1.f,
           oldX == body ? "yes" : "no", oldMotion == motion ? "yes" : "no");
}

void probeReport() {
    struct Best {
        uint32_t hits;
        size_t region;
        uint32_t off;
        bool pair;
    };
    std::vector<Best> all;
    for (size_t r = 0; r < std::size(kProbeRegions); ++r)
        for (int pair = 0; pair < 2; ++pair)
            for (uint32_t i = 0; i < kProbeRegions[r].size / 4; ++i)
                if (g_probe[r].hit[pair][i]) all.push_back({g_probe[r].hit[pair][i], r, i * 4, pair != 0});
    std::sort(all.begin(), all.end(), [](const Best& a, const Best& b) { return a.hits > b.hits; });
    std::string line;
    for (size_t i = 0; i < all.size() && i < 8; ++i) {
        char b[96];
        snprintf(b, sizeof(b), "%s%s +0x%x%s %u%%", i ? ", " : "", kProbeRegions[all[i].region].name, all[i].off,
                 all[i].pair ? " (x, +0x8 z)" : "", all[i].hits * 100 / g_probeSamples);
        line += b;
    }
    GN_LOG("speed probe: %u samples while moving; best matches: %s", g_probeSamples, line.empty() ? "none" : line.c_str());
}
} // namespace

float oldTrainerSpeed() {
    const uintptr_t g = ptr(mem::rva(kOldSpeedGlobal));
    const uintptr_t m = g ? ptr(ptr(g + kOldSpeedOff) + 0x10) : 0;
    return m ? mem::readOr<float>(m + 0x438, -1.f) : -1.f;
}

float speedProbeSample() {
    const uintptr_t c = reinterpret_cast<uintptr_t>(localClientCharacter());
    gn_transform tr{};
    if (!c || !entityTransform(reinterpret_cast<void*>(c), tr)) {
        g_probeHave = false;
        return -1.f;
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float x = tr.trans[0], z = tr.trans[2];
    std::lock_guard<std::mutex> lock(g_probeM);
    if (!g_probeHave) {
        g_probeHave = true, g_probeX = x, g_probeZ = z, g_probeAt = now;
        return g_probeMeasured;
    }
    // Measured over at least 0.1 s: her position moves every frame (interpolated between ticks), so
    // frame to frame it was too noisy, and the first version, which took only 10-100 ms steps, never
    // sampled at a high frame rate (speed-2, 2026-10-30).
    const double dt = double(now.QuadPart - g_probeAt.QuadPart) / double(freq.QuadPart);
    if (dt < 0.1) return g_probeMeasured;
    const float d = std::sqrt((x - g_probeX) * (x - g_probeX) + (z - g_probeZ) * (z - g_probeZ));
    g_probeX = x, g_probeZ = z, g_probeAt = now;
    if (dt > 0.3) return g_probeMeasured; // a pause, a load or a hitch: no fair measure
    const float measured = static_cast<float>(d / dt);
    if (measured > 100.f) return g_probeMeasured; // a teleport
    g_probeMeasured = measured;
    if (measured < 1.f) return measured;

    const uintptr_t owner = ptr(c + e::cls::PamClientCharacterEntity::off::movementOwner);
    const uintptr_t mv = ptr(ptr(owner + k::MovementOwner_Movement));
    const uintptr_t holder = ptr(c + k::Character_PhysicsHolder);
    const uintptr_t body = ptr(holder + k::PhysicsHolder_Body);
    const uintptr_t base[] = {ptr(body + k::Body_Motion), body, holder, mv ? ptr(mv + k::Movement_State) : 0, mv ? ptr(mv + k::Movement_Ground) : 0};
    static float buf[kProbeSlots];
    if (++g_probeSamples == 1) probeChains(c); // once at the start, then with each report
    for (size_t r = 0; r < std::size(kProbeRegions); ++r) {
        const uint32_t n = kProbeRegions[r].size / 4;
        if (!base[r] || !mem::read(base[r], buf, n * 4)) continue;
        for (uint32_t i = 0; i < n; ++i) {
            // (not "near": a windows.h macro)
            const auto matches = [measured](float v) { return std::isfinite(v) && std::fabs(std::fabs(v) - measured) <= measured * 0.15f; };
            if (matches(buf[i])) ++g_probe[r].hit[0][i];
            if (i + 2 < n && std::isfinite(buf[i]) && std::isfinite(buf[i + 2]) && matches(std::sqrt(buf[i] * buf[i] + buf[i + 2] * buf[i + 2])))
                ++g_probe[r].hit[1][i];
        }
    }
    if (g_probeSamples % 100 == 0 && g_probeReports < 5) ++g_probeReports, probeReport(), probeChains(c);
    return measured;
}


// ---- movement events (gn_api on_movement) ----------------------------------------------------------
// Faith's movement read once per client tick, while a mod listens: a landing (in air 1 -> 0), a take-off
// and a change of movement state, in that order. The landing tick is the bhop tick (state 0 there,
// recorded 2026-10-30), so a listener sees it as LANDED then STATE (8 -> 0).
namespace {
struct MoveListener {
    std::string mod;
    gn_movement_fn fn;
    void* user;
};
std::mutex g_moveM;
std::vector<MoveListener> g_moveListeners; // (g_moveM)
movement::Sample g_moveWas;                // client thread
bool g_moveHave = false;                   // client thread
} // namespace

int onMovement(const std::string& mod, gn_movement_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_moveM);
    g_moveListeners.push_back({mod, fn, user});
    return GN_OK;
}

void movementEventsClientTick() {
    std::vector<MoveListener> listeners;
    {
        std::lock_guard<std::mutex> lock(g_moveM);
        if (g_moveListeners.empty()) return;
        listeners = g_moveListeners;
    }
    gn_movement m{};
    if (movementGet(&m) != GN_OK) {
        g_moveHave = false; // no Faith (loading, a menu): the next reading starts again
        return;
    }
    const movement::Sample now{m.state, m.on_ground != 0};
    const movement::Sample was = g_moveWas;
    const bool had = g_moveHave;
    g_moveWas = now, g_moveHave = true;
    if (!had) return;
    for (const movement::Event& e : movement::diff(was, now)) {
        const gn_movement_event ev{e.kind, e.state, e.oldState, now.inAir ? 1 : 0};
        for (const MoveListener& l : listeners) {
            try {
                l.fn(&ev, &m, l.user);
            } catch (...) {
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 10) GN_LOG("[%s] a movement callback threw; ignored", l.mod.c_str());
            }
        }
    }
}

} // namespace gn::host
