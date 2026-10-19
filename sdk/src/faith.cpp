// Faith's movement, the keyboard as the game reads it, movement tuning and game state
// (db/annotations/faith.yaml). Everything here was found with examples/probe for the MEC_Trainer
// port; nothing is patched. Each chain checks what it can on the way (a vtable or reflected type
// where the object has one), and tuning is only written after GridNode has seen its default value
// at that address, so a wrong object reads as "not available" instead of being written to.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

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
    uintptr_t ground = 0, state = 0, motion = 0, camera = 0;
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
    const uintptr_t body = ptr(ptr(c + k::Character_PhysicsHolder) + k::PhysicsHolder_Body);
    p.motion = ptr(body + k::Body_Motion);
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

void faithClientTick() {
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

int jump() {
    // The game decides from the jump input what Faith does (jump, vault, coil...): GridNode
    // presses it for four ticks, as a quick tap would. Space is the default binding.
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
    if (p.motion) mem::read(p.motion + k::Motion_Speed, out->speed);
    if (p.camera) mem::read(p.camera + k::Camera_Forward, out->camera_forward, sizeof(out->camera_forward));
    return GN_OK;
}

int movementSet(const gn_movement* in, uint32_t fields) {
    if (!in) return GN_ERR_ARGUMENT;
    const MovementPtrs p = movementPtrs();
    if (!p.state || !p.ground) return GN_ERR_UNAVAILABLE;
    if (fields & GN_MOVE_STATE) mem::write(p.state + k::State_Movement, in->state);
    if (fields & GN_MOVE_WALL_COUNT) mem::write(p.state + k::State_WallCount, in->wall_count);
    if (fields & GN_MOVE_WALL_CLIMBS) mem::write(p.state + k::State_WallClimbs, in->wall_climbs);
    if (fields & GN_MOVE_WALL_RUNS) mem::write(p.state + k::State_WallRuns, in->wall_runs);
    if (fields & GN_MOVE_LAST_GROUND_Y) mem::write(p.ground + k::Ground_LastY, in->last_ground_y);
    if ((fields & GN_MOVE_SPEED) && p.motion) mem::write(p.motion + k::Motion_Speed, in->speed);
    return GN_OK;
}

// Faith's position as the animation system holds it (MEC_Trainer's playerX chain): character +0xd0
// -> +0x128 (an Animatable, checked) -> +0x30 -> +0x50 (Vec3). Writing it moves Faith on the client
// without a teleport, so the camera stays free (noclip).
int positionSet(const float pos[3]) {
    if (!pos) return GN_ERR_ARGUMENT;
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
    Tuning* t = name ? findTuning(name) : nullptr;
    if (!t) return GN_ERR_NOT_FOUND;
    const uintptr_t a = resolve(*t);
    if (!a || (t->verifiedAt.load() != a && !verified(*t))) return GN_ERR_UNAVAILABLE;
    return mem::write(a, value) ? GN_OK : GN_ERR_UNAVAILABLE;
}

} // namespace gn::host
