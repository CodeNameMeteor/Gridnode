#pragma once
// C++ helpers for GridNode mods, header-only, over the C API in gridnode/api.h.
//
//   #include <gridnode/mod.hpp>
//
//   GN_MOD("my-mod") {          // runs once, on GridNode's thread, after the build check
//       gn::log("hello from %s", "my mod");
//       gn::onServerTick([](float dt) { ... });
//       return true;            // false: the mod failed to load
//   }
//
// Callbacks are std::function-like lambdas; GridNode keeps them for the life of the process.

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>
#include <type_traits>
#include <utility>

#include "gridnode/api.h"

namespace gn {

namespace detail {
inline const gn_api*& apiPtr() {
    static const gn_api* a = nullptr;
    return a;
}
inline const char*& modName() {
    static const char* n = "mod";
    return n;
}
template <class F> F* keep(F f) { return new F(std::move(f)); } // lives for the process, by design
} // namespace detail

inline const gn_api& api() { return *detail::apiPtr(); }
inline const char* modName() { return detail::modName(); }

// ---- logging ---------------------------------------------------------------------------------
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    api().log(modName(), buf);
}

// ---- addresses -------------------------------------------------------------------------------
inline uint64_t rva(uint64_t va) { return api().rva(va); }
template <class Fn> Fn function(uint64_t va) { return reinterpret_cast<Fn>(static_cast<uintptr_t>(rva(va))); }

// ---- threads ---------------------------------------------------------------------------------
namespace detail {
// A mod's callback, kept from throwing into GridNode (the exception would cross two runtimes): what
// it threw goes to the log.
template <class Fn> void guarded(const char* what, Fn&& fn) {
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        fn();
    } catch (const std::exception& e) {
        log("%s threw: %s", what, e.what());
    } catch (...) {
        log("%s threw", what);
    }
#else
    (void)what;
    fn();
#endif
}
} // namespace detail
template <class F> bool onServerTick(F f) {
    auto* p = detail::keep(std::function<void(float)>(std::move(f)));
    return api().on_server_tick([](float dt, void* u) { detail::guarded("a server tick", [&] { (*static_cast<std::function<void(float)>*>(u))(dt); }); }, p) == GN_OK;
}
template <class F> bool onClientTick(F f) {
    auto* p = detail::keep(std::function<void(float)>(std::move(f)));
    return api().on_client_tick([](float dt, void* u) { detail::guarded("a client tick", [&] { (*static_cast<std::function<void(float)>*>(u))(dt); }); }, p) == GN_OK;
}
template <class F> void runOnServer(F f) {
    auto* p = new std::function<void()>(std::move(f));
    api().run_on_server([](void* u) {
        auto* fn = static_cast<std::function<void()>*>(u);
        detail::guarded("a runOnServer task", [&] { (*fn)(); });
        delete fn;
    }, p);
}
template <class F> void runOnClient(F f) {
    auto* p = new std::function<void()>(std::move(f));
    api().run_on_client([](void* u) {
        auto* fn = static_cast<std::function<void()>*>(u);
        detail::guarded("a runOnClient task", [&] { (*fn)(); });
        delete fn;
    }, p);
}
inline bool isServerThread() { return api().is_server_thread() != 0; }
inline bool isClientThread() { return api().is_client_thread() != 0; }

// ---- hooks -----------------------------------------------------------------------------------
// Swaps the pointer at slotVa for detour; *original receives what it held (call through it).
// expectedVa: the game function the slot must hold (0: any game function).
template <class Fn> bool hookSlot(uint64_t slotVa, uint64_t expectedVa, Fn detour, Fn* original) {
    static_assert(std::is_pointer<Fn>::value, "detour must be a function pointer");
    return api().hook_slot(modName(), slotVa, expectedVa, reinterpret_cast<void*>(detour),
                           reinterpret_cast<void**>(original)) == GN_OK;
}

// Code patches (API 15), for what no pointer reaches; undone when the mod is unloaded. patchCall: the
// call or jmp at siteVa, which must go to expectedTargetVa, goes to detour; call the game's function
// through *original. patchBytes: size bytes at va, which must be `expected`, become `bytes`.
template <class Fn> bool patchCall(uint64_t siteVa, uint64_t expectedTargetVa, Fn detour, Fn* original) {
    static_assert(std::is_pointer<Fn>::value, "detour must be a function pointer");
    return api().patch_call(modName(), siteVa, expectedTargetVa, reinterpret_cast<void*>(detour),
                            reinterpret_cast<void**>(original)) == GN_OK;
}
inline bool patchBytes(uint64_t va, const void* expected, const void* bytes, size_t size) {
    return api().patch_bytes(modName(), va, expected, bytes, size) == GN_OK;
}

// Function detours (API 16): calls to the game's function at fnVa go to detour, with the same
// arguments; call the game's function through *original (before, after, or not at all). Taken out
// when the mod is unloaded. The detour runs on the caller's thread.
template <class Fn> bool hookFunction(uint64_t fnVa, Fn detour, Fn* original) {
    static_assert(std::is_pointer<Fn>::value, "detour must be a function pointer");
    return api().hook_function(modName(), fnVa, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original)) == GN_OK;
}

// ---- other mods (API 16) ----------------------------------------------------------------------
// A mod says who it is with GN_MOD_INFO / GN_MOD_INFO_DEPS (below; gn_mod_info in gridnode/api.h).
// exportFn offers a function of this mod as "<its id>.<name>"; importFn gets another mod's (null when
// it is not loaded or does not offer it); modVersion: whether a mod is loaded, and its version.
template <class Fn> bool exportFn(const char* name, Fn fn) {
    static_assert(std::is_pointer<Fn>::value, "fn must be a function pointer");
    return api().mod_export(name, reinterpret_cast<void*>(fn)) == GN_OK;
}
template <class Fn> Fn importFn(const char* modId, const char* name) {
    static_assert(std::is_pointer<Fn>::value, "Fn must be a function pointer type");
    return reinterpret_cast<Fn>(api().mod_import(modId, name));
}
inline bool modVersion(const char* modId, std::string* version = nullptr) {
    char b[64] = {};
    if (api().mod_version(modId, b, sizeof(b)) != GN_OK) return false;
    if (version) *version = b;
    return true;
}

// ---- reflection ------------------------------------------------------------------------------
class Type {
public:
    Type() = default;
    explicit Type(const gn_type* t) : t_(t) {}
    static Type find(const char* name) { return Type(api().type_find(name)); }
    static Type of(const void* object) { return Type(api().type_of(object)); }

    explicit operator bool() const { return t_ != nullptr; }
    const gn_type* get() const { return t_; }
    const char* name() const { return t_ ? api().type_name(t_) : ""; }
    uint32_t size() const { return t_ ? api().type_size(t_) : 0; }
    int category() const { return api().type_category(t_); }
    Type super() const { return Type(t_ ? api().type_super(t_) : nullptr); }
    uint64_t va() const { return t_ ? api().type_va(t_) : 0; }
    bool isKindOf(const Type& base) const { return t_ && base.t_ && api().is_kind_of(t_, base.t_) != 0; }
    bool field(const char* name, gn_field& out) const { return t_ && api().field_find(t_, name, &out) == GN_OK; }
    uint32_t fieldCount() const { return t_ ? api().field_count(t_) : 0; }
    bool fieldAt(uint32_t i, gn_field& out) const { return t_ && api().field_at(t_, i, &out) == GN_OK; }
    bool operator==(const Type& o) const { return t_ == o.t_; }

private:
    const gn_type* t_ = nullptr;
};

// An engine object with its reflected type, for reading and writing fields by name. Reads and
// writes are fault tolerant and check the field's size against T.
class Object {
public:
    Object() = default;
    Object(void* p, Type t) : p_(p), t_(t) {}
    static Object of(void* p) { return Object(p, Type::of(p)); }

    explicit operator bool() const { return p_ && t_; }
    void* ptr() const { return p_; }
    const Type& type() const { return t_; }

    template <class T> bool get(const char* field, T& out) const {
        static_assert(std::is_trivially_copyable<T>::value, "fields are read as plain bytes");
        gn_field f;
        if (!t_.field(field, f) || f.size != sizeof(T)) return false;
        return api().read(static_cast<const char*>(p_) + f.offset, &out, sizeof(T)) == GN_OK;
    }
    template <class T> bool set(const char* field, const T& value) const {
        static_assert(std::is_trivially_copyable<T>::value, "fields are written as plain bytes");
        gn_field f;
        if (!t_.field(field, f) || f.size != sizeof(T)) return false;
        return api().write(static_cast<char*>(p_) + f.offset, &value, sizeof(T)) == GN_OK;
    }
    // A field that refers to another object (a class field: a pointer).
    Object ref(const char* field) const {
        void* p = nullptr;
        return get(field, p) && p ? Object::of(p) : Object();
    }

private:
    void* p_ = nullptr;
    Type t_;
};

template <class T> bool read(const void* address, T& out) { return api().read(address, &out, sizeof(T)) == GN_OK; }
template <class T> bool write(void* address, const T& value) { return api().write(address, &value, sizeof(T)) == GN_OK; }

// ---- entities --------------------------------------------------------------------------------
// fn(void* entity, void* creationInfo) for every entity of className (or a subclass) created from
// now on, on the thread that created it.
template <class F> bool onCreated(const char* className, F f) {
    auto* p = detail::keep(std::function<void(void*, void*)>(std::move(f)));
    return api().on_created(modName(), className,
                            [](void* e, void* ci, void* u) { (*static_cast<std::function<void(void*, void*)>*>(u))(e, ci); },
                            p) == GN_OK;
}

// ---- world ----------------------------------------------------------------------------------
using Transform = gn_transform;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// Faith on the client (PamClientCharacterEntity). On the client thread: now; elsewhere: as of the
// last client tick. Null when there is none (menus, loading).
inline void* localClientCharacter() { return api().local_client_character(); }
// Faith on the server (ServerCharacterEntity), the one teleport() moves. Null when there is none.
inline void* localServerCharacter() { return api().local_server_character(); }

inline bool transformOf(const void* entity, Transform& out) { return api().entity_transform(entity, &out) == GN_OK; }
inline bool positionOf(const void* entity, Vec3& out) {
    Transform t;
    if (!transformOf(entity, t)) return false;
    out = {t.trans[0], t.trans[1], t.trans[2]};
    return true;
}
// The game's own teleport. Server thread only (see runOnServer). Returns a GN_ result.
inline int teleport(void* serverCharacter, const Transform& to) { return api().teleport(serverCharacter, &to); }
inline void* component(void* character, const Type& type) { return api().get_component(character, type.get()); }

inline std::string levelName() {
    char buf[256];
    api().level_name(buf, sizeof(buf));
    return buf;
}
// fn(const char* level, bool loaded): a level started or ended (GridNode's own thread).
template <class F> bool onLevel(F f) {
    auto* p = detail::keep(std::function<void(const char*, bool)>(std::move(f)));
    return api().on_level(modName(),
                          [](const char* l, int loaded, void* u) { (*static_cast<std::function<void(const char*, bool)>*>(u))(l, loaded != 0); },
                          p) == GN_OK;
}

// fn(const char* level) once per level when Faith is over loaded ground (API 26), on the server
// thread. levelReady(): is the current level ready. groundBelow: a ray down from at (server
// thread): true and y when the ground there has streamed in.
template <class F> bool onLevelReady(F f) {
    auto* p = detail::keep(std::function<void(const char*)>(std::move(f)));
    return api().on_level_ready(modName(), [](const char* l, void* u) { (*static_cast<std::function<void(const char*)>*>(u))(l); }, p) == GN_OK;
}
inline bool levelReady() { return api().level_ready() != 0; }
inline bool groundBelow(const float at[3], float& y, float depth = 100.f) { return api().ground_below(at, depth, &y) == GN_OK; }

// fn(void* entity) for every entity of className (or a subclass) just before it is destroyed (API 26),
// on the thread destroying it: drop it from what the mod keeps.
template <class F> bool onDestroyed(const char* className, F f) {
    auto* p = detail::keep(std::function<void(void*)>(std::move(f)));
    return api().on_destroyed(modName(), className, [](void* e, void* u) { (*static_cast<std::function<void(void*)>*>(u))(e); }, p) == GN_OK;
}

// ---- settings and console --------------------------------------------------------------------
// A settings object by class name, e.g. gn::settings("GameTimeSettings").set("TimeScale", 0.5f).
// Look it up each time: the console can replace it with a changed copy.
inline Object settings(const char* className) {
    void* p = api().settings_find(className);
    return p ? Object(p, Type::find(className)) : Object();
}
// Why a settings class or console group touches online play, or null.
inline const char* onlineWarning(const char* classOrGroup) { return api().online_warning(classOrGroup); }

// Runs a console line (every command works). fn(std::string output) runs on the client thread.
inline bool console(const char* line) { return api().console_run(modName(), line, nullptr, nullptr) == GN_OK; }
template <class F> bool console(const char* line, F f) {
    auto* p = new std::function<void(const char*)>(std::move(f));
    const int r = api().console_run(modName(), line,
                                    [](const char* out, void* u) {
                                        auto* fn = static_cast<std::function<void(const char*)>*>(u);
                                        (*fn)(out);
                                        delete fn;
                                    },
                                    p);
    if (r != GN_OK) delete p;
    return r == GN_OK;
}

// ---- ray casts --------------------------------------------------------------------------------
using RayHit = gn_ray_hit;
// The nearest hit between from and to (radius > 0: a sphere sweep). Server thread only.
inline bool raycast(const Vec3& from, const Vec3& to, RayHit& hit, float radius = 0.f) {
    const float a[3] = {from.x, from.y, from.z}, b[3] = {to.x, to.y, to.z};
    return api().raycast(a, b, radius, &hit) == GN_OK;
}

using ObjectHit = gn_ray_hit_ex;
// A thin ray that also says what it hit: hit.entity (null for level geometry), hit.body, hit.part.
// Server thread only; looks through the physics entities, so per click rather than per tick.
inline bool raycastObject(const Vec3& from, const Vec3& to, ObjectHit& hit) {
    const float a[3] = {from.x, from.y, from.z}, b[3] = {to.x, to.y, to.z};
    hit = ObjectHit{};
    hit.size = sizeof(ObjectHit);
    return api().raycast_object(a, b, &hit) == GN_OK;
}

// ---- collision bodies -------------------------------------------------------------------------
// A physics entity's collision body ids (empty if it is not one). Any thread.
inline std::vector<uint32_t> entityBodies(void* entity) {
    uint32_t ids[64];
    const int n = api().entity_bodies(entity, ids, 64);
    return n > 0 ? std::vector<uint32_t>(ids, ids + (n < 64 ? n : 64)) : std::vector<uint32_t>();
}
// Where a body's collision is, and moving it (a teleport). The entity's realm thread.
inline bool bodyTransform(void* entity, uint32_t body, gn_transform& out) { return api().body_transform(entity, body, &out) == GN_OK; }
inline int bodySetTransform(void* entity, uint32_t body, const gn_transform& to) { return api().body_set_transform(entity, body, &to); }
// The same object in the other realm (its client half for a server entity, and back), or null. Faith
// collides with the client half's bodies: move both. Any thread.
inline void* entityOtherHalf(void* entity) { return api().entity_other_half(entity); }

// ---- collision boxes (API 19) ------------------------------------------------------------------
// A static box in the client's physics world (what Faith collides with) and the server's (AI), made
// at each one's next physics update. 0 if it could not be asked for. Any thread; the mod's boxes go
// when it is unloaded.
inline uint32_t boxAdd(const gn_transform& at, const Vec3& halfExtents) {
    const float h[3] = {halfExtents.x, halfExtents.y, halfExtents.z};
    uint32_t box = 0;
    return api().box_add(modName(), &at, h, &box) == GN_OK ? box : 0;
}
inline bool boxMove(uint32_t box, const gn_transform& at) { return api().box_move(box, &at) == GN_OK; }
inline bool boxRemove(uint32_t box) { return api().box_remove(box) == GN_OK; }
inline int boxState(uint32_t box) { return api().box_state(box); } // 1 in the world, 0 waiting, < 0 gone / failed

// ---- sounds (API 19) ----------------------------------------------------------------------------
// A loaded sound asset played at pos (event null: its default start). 0 on failure. Client thread.
inline uint32_t soundPlay(const char* assetPath, const Vec3* pos = nullptr, const char* event = nullptr) {
    const float p[3] = {pos ? pos->x : 0.f, pos ? pos->y : 0.f, pos ? pos->z : 0.f};
    uint32_t s = 0;
    return api().sound_play(modName(), assetPath, event, pos ? p : nullptr, &s) == GN_OK ? s : 0;
}
inline bool soundEvent(uint32_t sound, const char* event) { return api().sound_event(sound, event) == GN_OK; }
inline bool soundMove(uint32_t sound, const Vec3& pos) {
    const float p[3] = {pos.x, pos.y, pos.z};
    return api().sound_move(sound, p) == GN_OK;
}
inline bool soundPlaying(uint32_t sound) { return api().sound_playing(sound) == 1; }
inline bool soundStop(uint32_t sound) { return api().sound_stop(sound) == GN_OK; }

// ---- despawning characters (API 19) -------------------------------------------------------------
// A server character (an AI) taken away as its spawner would. Not Faith. Server thread.
inline int characterDespawn(void* serverCharacter) { return api().character_despawn(serverCharacter); }

// ---- body velocities and gravity (API 21) -------------------------------------------------------
// A moving body's velocity (world space; zeros for a static one), set (null: keep), or pushed (added
// to its linear velocity). The entity's realm thread.
inline bool bodyVelocity(void* entity, uint32_t body, Vec3& linear, Vec3* angular = nullptr) {
    float l[3] = {}, a[3] = {};
    if (api().body_velocity(entity, body, l, a) != GN_OK) return false;
    linear = {l[0], l[1], l[2]};
    if (angular) *angular = {a[0], a[1], a[2]};
    return true;
}
inline int bodySetVelocity(void* entity, uint32_t body, const Vec3* linear, const Vec3* angular = nullptr) {
    const float l[3] = {linear ? linear->x : 0.f, linear ? linear->y : 0.f, linear ? linear->z : 0.f};
    const float a[3] = {angular ? angular->x : 0.f, angular ? angular->y : 0.f, angular ? angular->z : 0.f};
    return api().body_set_velocity(entity, body, linear ? l : nullptr, angular ? a : nullptr);
}
inline int bodyPush(void* entity, uint32_t body, const Vec3& change) {
    const float c[3] = {change.x, change.y, change.z};
    return api().body_push(entity, body, c);
}
// A realm's gravity (0 client, 1 server), m/s^2; setGravity(realm, nullptr) puts the game's back.
inline bool gravity(int realm, Vec3& out) {
    float g[3] = {};
    if (api().physics_gravity(realm, g) != GN_OK) return false;
    out = {g[0], g[1], g[2]};
    return true;
}
inline int setGravity(int realm, const Vec3* g) {
    const float v[3] = {g ? g->x : 0.f, g ? g->y : 0.f, g ? g->z : 0.f};
    return api().physics_set_gravity(realm, g ? v : nullptr);
}

// ---- health, where to go (API 22) ------------------------------------------------------------------
// A server character's health and max health (Faith: 100; a KSEC: hits, 3). Server thread.
inline bool health(void* serverCharacter, float& health, float& maxHealth) {
    return api().character_health(serverCharacter, &health, &maxHealth) == GN_OK;
}
// Writes the health only (0 is not a death: the next hit acts on it). Server thread.
inline int setHealth(void* serverCharacter, float health) { return api().character_set_health(serverCharacter, health); }
// The map marker; false when none is set. Any thread.
inline bool waypoint(Vec3& out) {
    float v[3];
    if (api().waypoint(v) != GN_OK) return false;
    out = {v[0], v[1], v[2]};
    return true;
}
// Where Faith comes back after a death. Any thread.
inline bool checkpoint(Vec3& out) {
    float v[3];
    if (api().checkpoint(v) != GN_OK) return false;
    out = {v[0], v[1], v[2]};
    return true;
}
// ---- progression (API 22): the save's flags ---------------------------------------------------------
// fn(const gn_progress_flag&) for every flag the game defines, with its value now; how many
// (negative: not available, e.g. no level). Any thread.
template <class F> int progressFlags(F fn) {
    return api().progress_flags([](const gn_progress_flag* f, void* u) { (*static_cast<F*>(u))(*f); }, &fn);
}
// One flag by name (case sensitive). Any thread.
inline bool progressFind(const char* name, gn_progress_flag& out) { return api().progress_find(name, &out) == GN_OK; }
// Sets a flag through the game's setter (it clamps, tells missions, sends the flag update message);
// *now: the value the game kept. Flags marked as synced to online stats are set too (the log notes
// them). Server thread.
inline int progressSet(void* flag, int32_t value, int32_t* now = nullptr) { return api().progress_set(flag, value, now); }
// The active mission's index (0: none) and its name. Any thread.
inline bool mission(int32_t& index, std::string* name = nullptr) {
    char b[128] = {};
    if (api().mission(&index, name ? b : nullptr, sizeof(b)) != GN_OK) return false;
    if (name) *name = b;
    return true;
}

// ---- online services, on purpose (API 23) ---------------------------------------------------------
// Says this mod changes online services on purpose (restoring or replacing an online feature):
// reason is one line, shown in the menu's Mods list and logged with a warning. Until it is called,
// online console commands are refused. Not for leaderboard cheats:
// players see which mods touch online play. Call it in gn_mod_load (or before the first such call).
inline bool allowOnline(const char* reason) { return api().online_allow(modName(), reason) == GN_OK; }

// ---- ANT's types and Faith's move tracker (API 25) --------------------------------------------------
// ANT's own reflection (docs/modding.md, "ANT types"): a type and its fields by name.
using AntType = gn_ant_type;
using AntField = gn_ant_field;
inline bool antType(const char* name, AntType& out) {
    out = AntType{};
    out.size = sizeof(out);
    return api().ant_type_find(name, &out) == GN_OK;
}
inline bool antField(const char* type, const char* field, AntField& out) {
    out = AntField{};
    out.size = sizeof(out);
    return api().ant_field_find(type, field, &out) == GN_OK;
}
// fn(const AntField&) for each of the type's own fields; how many (negative: not available).
template <class F> int antFields(const char* type, F fn) {
    AntType t;
    if (!antType(type, t)) return -GN_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < t.field_count; ++i) {
        AntField f{};
        f.size = sizeof(f);
        if (api().ant_field_at(type, i, &f) == GN_OK) fn(static_cast<const AntField&>(f));
    }
    return static_cast<int>(t.field_count);
}
// Faith's move as ANT tracks it: current move, in a jump, speed. Client thread.
using MoveTracker = gn_move_tracker;
inline bool moveTracker(MoveTracker& out) {
    out = MoveTracker{};
    out.size = sizeof(out);
    return api().move_tracker(&out) == GN_OK;
}

// The camera for mods (API 27): placed at t each frame until cameraRelease(); GN_CAMERA_BODY shows
// Faith's whole body. cameraFov(degrees): the camera's FOV (the game's is 55), 0 gives it back.
inline bool cameraSet(const Transform& t, uint32_t flags = 0) { return api().camera_set(&t, flags) == GN_OK; }
inline void cameraRelease() { api().camera_set(nullptr, 0); }
inline bool cameraFov(float degrees) { return api().camera_fov_set(degrees) == GN_OK; }

// Faith's last 15 seconds (API 27; client thread): the newest maxFrames keyframes, oldest first.
using ReplayBone = gn_replay_bone;
struct ReplayRecent {
    uint32_t bones = 0;              // per keyframe
    std::vector<float> times;        // seconds
    std::vector<ReplayBone> pose;    // times.size() * bones
    const ReplayBone* frame(size_t i) const { return pose.data() + i * bones; }
};
inline bool replayRecent(ReplayRecent& out, uint32_t maxFrames = 450) {
    gn_replay_info info{};
    info.size = sizeof(info);
    out = ReplayRecent{};
    if (api().replay_recent(&info, nullptr, nullptr, 0) < 0) return false;
    const uint32_t n = std::min(maxFrames, info.frames);
    out.bones = info.bones;
    out.times.resize(n);
    out.pose.resize(static_cast<size_t>(n) * info.bones);
    const int got = api().replay_recent(&info, out.times.data(), out.pose.data(), n);
    if (got < 0) return false;
    out.times.resize(static_cast<size_t>(got));
    out.pose.resize(static_cast<size_t>(got) * out.bones);
    return true;
}

// Where an ANT field is in the type's live objects, and those objects (API 26; client thread; research:
// docs/research/ant.md). antObjects can take seconds when it has to scan: keep what it gives.
using AntLiveField = gn_ant_live_field;
inline bool antLiveField(const char* type, const char* field, AntLiveField& out) {
    out = AntLiveField{};
    out.size = sizeof(out);
    return api().ant_live_field(type, field, &out) == GN_OK;
}
inline std::vector<void*> antObjects(const char* type, uint32_t max = 256) {
    std::vector<void*> out(max);
    const int n = api().ant_objects(type, out.data(), max);
    out.resize(n > 0 ? std::min<uint32_t>(static_cast<uint32_t>(n), max) : 0);
    return out;
}

// ---- the mod's folder (API 20) ------------------------------------------------------------------
// Where its DLL is installed (mods\ or mods\<name>\, with the trailing backslash): its own files.
inline std::string modDir() {
    char b[1024] = {};
    return api().mod_dir(b, sizeof(b)) == GN_OK ? std::string(b) : std::string();
}

// ---- messages ---------------------------------------------------------------------------------
// fn(const void* message, const gn_message_info& info) for every message of a type
// ("ClientPlayerLocalSetMessage") or a whole category ("ClientPlayer"), on the sender's thread.
template <class F> bool onMessage(const char* typeOrCategory, F f) {
    auto* p = detail::keep(std::function<void(const void*, const gn_message_info&)>(std::move(f)));
    return api().on_message(modName(), typeOrCategory,
                            [](const void* m, const gn_message_info* i, void* u) {
                                (*static_cast<std::function<void(const void*, const gn_message_info&)>*>(u))(m, *i);
                            },
                            p) == GN_OK;
}

// ---- keys, movement, tuning, game state ----------------------------------------------------------
// A key as the game reads it: keyFind("Space") (an InputDeviceKeys value, -1 if unknown).
inline int keyFind(const char* name) { return api().key_find(name); }
// Holds the key for `ticks` client ticks, then lets go. Any thread.
inline bool keyPress(int key, uint32_t ticks = 4) { return api().key_press(key, ticks) == GN_OK; }
// Makes Faith jump: the jump action, whatever key the player bound (the game decides whether it
// becomes a vault, a coil...).
inline bool jump() { return api().jump() == GN_OK; }
// Holds an input action (GN_ACTION_JUMP, or another action's hash) for `ticks` client ticks.
inline bool actionPress(uint32_t actionHash, uint32_t ticks = 4) { return api().action_press(actionHash, ticks) == GN_OK; }
// Whether the player holds the action now (their real input, whatever it is bound to; API 24): 1 / 0,
// negative when it cannot be read (no Faith, no such action).
inline int actionHeld(uint32_t actionHash) { return api().action_held(actionHash); }

using Movement = gn_movement;
// Faith's movement state, wall counters, ground, speed and camera direction.
inline bool movement(Movement& out) { return api().movement_get(&out) == GN_OK; }
// Writes the fields named in `fields` (GN_MOVE_*).
inline bool setMovement(const Movement& in, uint32_t fields) { return api().movement_set(&in, fields) == GN_OK; }
using Landing = gn_landing;
// The game's own landing prediction for Faith (ticks to hit in 60ths of a second, hit height, soft
// landing); false until she has been in the air.
inline bool landingPrediction(Landing& out) { return api().landing_prediction(&out) == GN_OK; }

// ---- the free camera (API 13) ----------------------------------------------------------------------
// GridNode's free camera (the layout editor's): on / off at the next frame.
using Freecam = gn_freecam;
inline bool freecam(bool on) { return api().freecam_set(on ? 1 : 0) == GN_OK; }
inline bool freecamState(Freecam& out) { return api().freecam_get(&out) == GN_OK; }
inline bool freecamOn() {
    Freecam f;
    return freecamState(f) && f.on;
}
// Faith to where the free camera's crosshair points, or to the camera itself; keepFlying false goes
// back to her view. Only while it flies.
inline bool teleportToCrosshair(bool keepFlying = false) { return api().freecam_teleport(1, keepFlying ? 1 : 0) == GN_OK; }
inline bool teleportToFreecam(bool keepFlying = false) { return api().freecam_teleport(2, keepFlying ? 1 : 0) == GN_OK; }

// ---- events between entities (API 11) ------------------------------------------------------------
// An event's id from its name, as the engine hashes it (djb2-xor): eventHash("StartReplay").
constexpr uint32_t eventHash(const char* name) {
    uint32_t h = 5381;
    for (; *name; ++name) h = (h * 33) ^ static_cast<unsigned char>(*name);
    return h;
}
// An id's name, or the id as hex for an unnamed one.
inline std::string eventName(uint32_t id) {
    char buf[128];
    if (api().event_name(id, buf, sizeof(buf)) == GN_OK) return buf;
    std::snprintf(buf, sizeof(buf), "%08x", id);
    return buf;
}
// Sends an event to an entity as its bus would, at its realm's next tick: sendEvent(replay,
// "StartReplay"). A name or "0x<hash>". sendEventToClass: to every live entity of the class; how many.
inline bool sendEvent(void* entity, const char* event) { return api().event_send(modName(), entity, event) == GN_OK; }
inline int sendEventToClass(const char* className, const char* event) { return api().event_send_class(modName(), className, event); }
// fn(void* entity, gn::Type type, uint32_t eventId) for every event an entity receives, before it
// handles it, on the thread that delivers it. event: a name or "0x<hash>", null for all; className:
// an entity class (and its subclasses), null for all. Filter by name where you can: there are many.
template <class F> bool onEvent(const char* event, const char* className, F f) {
    using Fn = std::function<void(void*, Type, uint32_t)>;
    auto* p = detail::keep(Fn(std::move(f)));
    return api().on_event(modName(), event, className,
                          [](void* e, const gn_type* t, uint32_t id, void* u) { (*static_cast<Fn*>(u))(e, Type(t), id); }, p) == GN_OK;
}

// ---- movement events, settings, hotkeys (API 9) ------------------------------------------------
using MovementEvent = gn_movement_event;
// fn(const MovementEvent& e, const Movement& now) for every landing, take-off and movement state
// change (GN_MOVE_EVENT_*), on the client thread, on the tick it happens.
template <class F> bool onMovement(F f) {
    using Fn = std::function<void(const MovementEvent&, const Movement&)>;
    auto* p = detail::keep(Fn(std::move(f)));
    return api().on_movement(modName(), [](const gn_movement_event* e, const gn_movement* m, void* u) { (*static_cast<Fn*>(u))(*e, *m); },
                             p) == GN_OK;
}

// The mod's settings, kept between runs in mods/<mod>.ini (written a second after a change).
namespace config {
inline std::string get(const char* key, const std::string& def = {}) {
    char buf[1024];
    const int r = api().config_get(modName(), key, buf, sizeof(buf));
    return r == GN_OK || r == GN_ERR_FULL ? std::string(buf) : def;
}
inline float getFloat(const char* key, float def) {
    const std::string v = get(key);
    char* end = nullptr;
    const float f = v.empty() ? def : std::strtof(v.c_str(), &end);
    return end && *end == 0 ? f : def;
}
inline int getInt(const char* key, int def) {
    const std::string v = get(key);
    char* end = nullptr;
    const long i = v.empty() ? def : std::strtol(v.c_str(), &end, 10);
    return end && *end == 0 ? static_cast<int>(i) : def;
}
inline bool getBool(const char* key, bool def) {
    const std::string v = get(key);
    return v == "1" || v == "true" ? true : v == "0" || v == "false" ? false : def;
}
inline bool setString(const char* key, const std::string& value) { return api().config_set(modName(), key, value.c_str()) == GN_OK; }
inline bool setFloat(const char* key, float value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", value);
    return setString(key, buf);
}
inline bool setInt(const char* key, int value) { return setString(key, std::to_string(value)); }
inline bool setBool(const char* key, bool value) { return setString(key, value ? "1" : "0"); }
} // namespace config

// ---- declared settings (API 17) ---------------------------------------------------------------------
// Declared once; kept in the mod's settings file with their description; on the mod's page in the
// GridNode menu (Mod settings) unless gn::setting::page(false). For example:
//     static gn::setting::Setting speed;
//     GN_MOD("my-mod") {
//         speed = gn::setting::addFloat("speed", "Speed", 2.f, 0.1f, 10.f, {"Metres per tick", "Movement"});
//         gn::setting::onChange("speed", [](const char*) { gn::log("speed is now %.2f", speed.value()); });
//         ...
//     }
//     ... speed.value() anywhere, any thread.
namespace setting {
struct Options {
    const char* description = nullptr; // tooltip and file comment
    const char* group = nullptr;       // heading on the page
    double step = 0;                   // int / float without a range: the +/- step
    uint32_t flags = 0;                // GN_SETTING_HIDDEN: only in the file
};
class Setting {
public:
    Setting() = default;
    explicit Setting(std::string id) : id_(std::move(id)) {}
    explicit operator bool() const { return !id_.empty(); }
    const std::string& id() const { return id_; }
    double number() const { return api().setting_number(modName(), id_.c_str()); } // NaN: unknown (or a text)
    bool on() const { return number() != 0; }
    int integer() const { return static_cast<int>(number()); } // INT, or a CHOICE's index
    float value() const { return static_cast<float>(number()); }
    std::string text() const {
        char b[1024];
        const int r = api().setting_text(modName(), id_.c_str(), b, sizeof(b));
        return r == GN_OK || r == GN_ERR_FULL ? std::string(b) : std::string();
    }
    bool set(double v) const { return api().setting_set_number(modName(), id_.c_str(), v) == GN_OK; }
    bool setText(const std::string& v) const { return api().setting_set_text(modName(), id_.c_str(), v.c_str()) == GN_OK; }

private:
    std::string id_;
};
namespace detail {
inline Setting add(const char* id, const char* label, int32_t kind, double def, double min, double max, const char* const* choices,
                   const char* defText, const Options& o) {
    gn_setting s{};
    s.size = sizeof(s), s.kind = kind, s.id = id, s.label = label, s.description = o.description, s.group = o.group;
    s.def = def, s.min = min, s.max = max, s.step = o.step, s.choices = choices, s.def_text = defText, s.flags = o.flags;
    return api().setting_add(modName(), &s) == GN_OK ? Setting(id) : Setting();
}
} // namespace detail
inline Setting addBool(const char* id, const char* label, bool def, const Options& o = {}) {
    return detail::add(id, label, GN_SETTING_BOOL, def ? 1 : 0, 0, 0, nullptr, nullptr, o);
}
inline Setting addInt(const char* id, const char* label, int def, int min, int max, const Options& o = {}) {
    return detail::add(id, label, GN_SETTING_INT, def, min, max, nullptr, nullptr, o);
}
inline Setting addFloat(const char* id, const char* label, float def, float min, float max, const Options& o = {}) {
    return detail::add(id, label, GN_SETTING_FLOAT, def, min, max, nullptr, nullptr, o);
}
inline Setting addChoice(const char* id, const char* label, int def, std::initializer_list<const char*> choices, const Options& o = {}) {
    std::vector<const char*> c(choices);
    c.push_back(nullptr); // copied by GridNode: need not outlive the call
    return detail::add(id, label, GN_SETTING_CHOICE, def, 0, 0, c.data(), nullptr, o);
}
inline Setting addText(const char* id, const char* label, const char* def, const Options& o = {}) {
    return detail::add(id, label, GN_SETTING_TEXT, 0, 0, 0, nullptr, def, o);
}
// f(const char* id) on the client thread when a setting changes (id null: any of the mod's).
template <class F> bool onChange(const char* id, F f) {
    auto* p = ::gn::detail::keep(std::function<void(const char*)>(std::move(f)));
    return api().setting_on_change(modName(), id, [](const char* changed, void* u) { (*static_cast<std::function<void(const char*)>*>(u))(changed); },
                                   p) == GN_OK;
}
// false: no page in the GridNode menu for this mod (its own window shows them).
inline void page(bool shown) { api().settings_page(modName(), shown ? 1 : 0); }
} // namespace setting

// A key the player can rebind, kept in the mod's settings (key.<id>). fn() runs on the client thread
// when it goes down (game focused, menu closed). defaultVk: a Windows virtual-key code ('F', VK_F1),
// 0 none. Show it in the mod's window with gn::ui::hotkeyButton(id) (gridnode/ui.hpp).
template <class F> bool hotkey(const char* id, const char* label, int defaultVk, F f) {
    auto* p = detail::keep(std::function<void()>(std::move(f)));
    return api().hotkey_add(modName(), id, label, defaultVk, [](void* u) { (*static_cast<std::function<void()>*>(u))(); }, p) == GN_OK;
}
using HotkeyInfo = gn_hotkey_info;
inline bool hotkeyInfo(const char* id, HotkeyInfo& out) { return api().hotkey_info(modName(), id, &out) == GN_OK; }
// Its key's name ("F", "none"), "" for no such hotkey: for labels like "God mode  [1]".
inline std::string hotkeyName(const char* id) {
    HotkeyInfo i;
    return hotkeyInfo(id, i) ? std::string(i.name) : std::string();
}
// Waits for the next key the player presses (Escape: none, Backspace: the default).
inline bool hotkeyRebind(const char* id) { return api().hotkey_rebind(modName(), id) == GN_OK; }
// A key directly (a virtual-key code, 0 none, -1 the default).
inline bool hotkeySet(const char* id, int vk) { return api().hotkey_set(modName(), id, vk) == GN_OK; }

using Tuning = gn_tuning;
inline uint32_t tuningCount() { return api().tuning_count(); }
inline bool tuningAt(uint32_t i, Tuning& out) { return api().tuning_at(i, &out) == GN_OK; }
// e.g. setTuning("MaxRunSpeed", 9.f). Refused until GridNode has seen the default at its address.
inline bool setTuning(const char* name, float value) { return api().tuning_set(name, value) == GN_OK; }
inline float tuning(const char* name, float fallback = 0.f) {
    Tuning t;
    for (uint32_t i = 0; i < tuningCount(); ++i)
        if (tuningAt(i, t) && std::string(t.name) == name) return t.available ? t.value : fallback;
    return fallback;
}

using GameState = gn_game_state;
inline GameState gameState() {
    GameState s{};
    api().game_state(&s);
    return s;
}
inline bool setTimeOfDay(float seconds) { return api().time_of_day_set(seconds) == GN_OK; }
// Holds the clock at `seconds` (API 28) against the game moving it on; releaseTimeOfDay lets it run again.
// Released when the mod unloads.
inline bool holdTimeOfDay(float seconds) { return api().time_of_day_hold(seconds) == GN_OK; }
inline void releaseTimeOfDay() { api().time_of_day_hold(-1.f); }
// The visual environment (API 29, experimental): `offset` is within the state (GN_VE_*). A set value is held
// against the game recomputing it every frame; released when the mod unloads. See api.h.
inline bool visualEnvGet(uint32_t offset, void* out, uint32_t size) { return api().visual_env_get(offset, out, size) == GN_OK; }
inline bool visualEnvSet(uint32_t offset, const void* data, uint32_t size) { return api().visual_env_set(offset, data, size) == GN_OK; }
inline void visualEnvRelease(uint32_t offset = GN_VE_ALL) { api().visual_env_release(offset); }
// Streaming focus points (API 30, experimental): the zones around `pos` are loaded as well as those around Faith.
// Returns the handle, 0 when refused (full, or this build's streamer is not the known one). Released when the mod unloads.
inline uint32_t streamingFocusAdd(const Vec3& pos) {
    const float p[3] = {pos.x, pos.y, pos.z};
    uint32_t h = 0;
    return api().streaming_focus_add(p, &h) == GN_OK ? h : 0;
}
inline bool streamingFocusMove(uint32_t handle, const Vec3& pos) {
    const float p[3] = {pos.x, pos.y, pos.z};
    return api().streaming_focus_move(handle, p) == GN_OK;
}
inline void streamingFocusRemove(uint32_t handle) { api().streaming_focus_remove(handle); }
// The game's text for a string id in its current language (API 31); the id itself when there is none.
inline std::string localize(const std::string& id) {
    char buf[512] = {};
    if (api().localize(id.c_str(), buf, sizeof(buf)) == GN_OK) return buf;
    return id;
}
// Timelines (API 29, experimental): doors, gates and lifts. setTimelineTime: server thread.
inline bool timelineTime(const void* timeline, float& seconds) { return api().timeline_time(timeline, &seconds) == GN_OK; }
inline bool setTimelineTime(void* timeline, float seconds, bool force = true) { return api().timeline_set_time(timeline, seconds, force ? 1 : 0) == GN_OK; }
// God mode (the game's immortality flags). Server thread: wrap in runOnServer.
inline bool godMode(bool on) { return api().god_mode(on ? 1 : 0) == GN_OK; }
// Moves Faith without a teleport (the camera stays free): for noclip-style movement every tick.
inline bool setPosition(const Vec3& p) {
    const float a[3] = {p.x, p.y, p.z};
    return api().position_set(a) == GN_OK;
}

// ---- unloading (API 15) ----------------------------------------------------------------------------
// This mod unloaded (its GN_MOD_UNLOAD runs, everything it registered goes) once the current callback
// returns; it stays unloaded until the GridNode menu's Load. False without GN_MOD_UNLOAD. For an
// "Unload" button in the mod's own window.
inline bool unloadSelf() { return api().mod_unload() == GN_OK; }
// Every mod and GridNode itself unloaded, the game put back as it was (the menu's Unload section).
inline void unloadGridNode() { api().gridnode_unload(); }

// ---- entities in place -------------------------------------------------------------------------------
// fn(void* entity, Type type, const Transform& at, float distance) for live entities near pos.
// fn(void* entity, gn::Type type) for every live entity of the class (and subclasses), logic entities
// too; how many (negative: an unknown class).
template <class F> int entitiesOfClass(const char* className, F fn) {
    return api().entities_of_class(className, [](void* e, const gn_type* t, void* u) { (*static_cast<F*>(u))(e, Type(t)); }, &fn);
}

template <class F> int entitiesNear(const Vec3& pos, float radius, F fn) {
    const float p[3] = {pos.x, pos.y, pos.z};
    return api().entities_near(p, radius,
                               [](void* e, const gn_type* t, const gn_transform* at, float d, void* u) {
                                   (*static_cast<F*>(u))(e, Type(t), *at, d);
                               },
                               &fn);
}
// Moves a spatial entity as the game does. On its realm's thread.
inline int setTransform(void* entity, const Transform& to) { return api().entity_set_transform(entity, &to); }

// ---- assets ----------------------------------------------------------------------------------------
// A loaded asset by path, holding a reference until it goes out of scope. Client or server thread.
class Asset {
public:
    Asset() = default;
    explicit Asset(const char* path) { p_ = api().asset_find(path, &group_); }
    ~Asset() { reset(); }
    Asset(const Asset&) = delete;
    Asset& operator=(const Asset&) = delete;
    Asset(Asset&& o) noexcept : p_(o.p_), group_(o.group_) { o.p_ = nullptr; }
    Asset& operator=(Asset&& o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; group_ = o.group_; o.p_ = nullptr; }
        return *this;
    }
    explicit operator bool() const { return p_ != nullptr; }
    void* get() const { return p_; }
    uint32_t group() const { return group_; }
    Type type() const { return Type::of(p_); }
    void reset() { if (p_) api().asset_release(p_); p_ = nullptr; }
private:
    void* p_ = nullptr;
    uint32_t group_ = 0;
};

} // namespace gn

// Defines the mod's entry point. The body runs once and returns true when the mod loaded.
#define GN_MOD(NAME)                                                                              \
    static bool gn_mod_main();                                                                    \
    extern "C" GN_MOD_EXPORT int gn_mod_load(const gn_api* api) {                                 \
        if (!api || api->version < GN_API_VERSION || api->size < sizeof(gn_api)) return 2; /* GridNode too old */ \
        ::gn::detail::apiPtr() = api;                                                             \
        ::gn::detail::modName() = NAME;                                                           \
        return gn_mod_main() ? 0 : 1;                                                             \
    }                                                                                             \
    static bool gn_mod_main()

// Optional: makes the mod reloadable while the game runs (GridNode's menu, Mods: Reload, or when its
// DLL changes). The body runs before GridNode takes out the mod's callbacks and hooks and frees it:
// stop the mod's own threads and undo what GridNode cannot see (memory it wrote, objects it made).
// Return true to go ahead, false to stay loaded. Mods without it are never unloaded.
// The mod's manifest (API 16): who it is and what it needs, read before gn_mod_load. Dependencies:
// "id", "id>=1.2" (that version or newer), "?id" (optional: loaded after it when it is there).
//     GN_MOD_INFO("my-mod", "My mod", "1.0.0", "me", "What it does");
//     GN_MOD_INFO_DEPS("my-mod", "My mod", "1.0.0", "me", "What it does", "mec-trainer>=1.3", "?other");
#define GN_MOD_INFO_DEPS(ID, NAME, VERSION, AUTHOR, DESCRIPTION, ...)                                         \
    extern "C" GN_MOD_EXPORT const gn_mod_manifest* gn_mod_info() {                                           \
        static const char* const deps[] = {__VA_ARGS__, nullptr};                                             \
        static const gn_mod_manifest m{sizeof(gn_mod_manifest), ID, NAME, VERSION, AUTHOR, DESCRIPTION, deps}; \
        return &m;                                                                                            \
    }
#define GN_MOD_INFO(ID, NAME, VERSION, AUTHOR, DESCRIPTION)                                                   \
    extern "C" GN_MOD_EXPORT const gn_mod_manifest* gn_mod_info() {                                           \
        static const gn_mod_manifest m{sizeof(gn_mod_manifest), ID, NAME, VERSION, AUTHOR, DESCRIPTION, nullptr}; \
        return &m;                                                                                            \
    }

#define GN_MOD_UNLOAD()                                                                           \
    static bool gn_mod_unload_main();                                                             \
    extern "C" GN_MOD_EXPORT int gn_mod_unload() {                                                \
        if (!::gn::detail::apiPtr()) return 0; /* never loaded: nothing to stop */                \
        return gn_mod_unload_main() ? 0 : 1;                                                      \
    }                                                                                             \
    static bool gn_mod_unload_main()
