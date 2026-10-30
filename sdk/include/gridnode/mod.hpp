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

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
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
template <class F> bool onServerTick(F f) {
    auto* p = detail::keep(std::function<void(float)>(std::move(f)));
    return api().on_server_tick([](float dt, void* u) { (*static_cast<std::function<void(float)>*>(u))(dt); }, p) == GN_OK;
}
template <class F> bool onClientTick(F f) {
    auto* p = detail::keep(std::function<void(float)>(std::move(f)));
    return api().on_client_tick([](float dt, void* u) { (*static_cast<std::function<void(float)>*>(u))(dt); }, p) == GN_OK;
}
template <class F> void runOnServer(F f) {
    auto* p = new std::function<void()>(std::move(f));
    api().run_on_server([](void* u) {
        auto* fn = static_cast<std::function<void()>*>(u);
        (*fn)();
        delete fn;
    }, p);
}
template <class F> void runOnClient(F f) {
    auto* p = new std::function<void()>(std::move(f));
    api().run_on_client([](void* u) {
        auto* fn = static_cast<std::function<void()>*>(u);
        (*fn)();
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

using Movement = gn_movement;
// Faith's movement state, wall counters, ground, speed and camera direction.
inline bool movement(Movement& out) { return api().movement_get(&out) == GN_OK; }
// Writes the fields named in `fields` (GN_MOVE_*).
inline bool setMovement(const Movement& in, uint32_t fields) { return api().movement_set(&in, fields) == GN_OK; }
using Landing = gn_landing;
// The game's own landing prediction for Faith (ticks to hit in 60ths of a second, hit height, soft
// landing); false until she has been in the air.
inline bool landingPrediction(Landing& out) { return api().landing_prediction(&out) == GN_OK; }

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
// God mode (the game's immortality flags). Server thread: wrap in runOnServer.
inline bool godMode(bool on) { return api().god_mode(on ? 1 : 0) == GN_OK; }
// Moves Faith without a teleport (the camera stays free): for noclip-style movement every tick.
inline bool setPosition(const Vec3& p) {
    const float a[3] = {p.x, p.y, p.z};
    return api().position_set(a) == GN_OK;
}

// ---- entities in place -------------------------------------------------------------------------------
// fn(void* entity, Type type, const Transform& at, float distance) for live entities near pos.
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
        if (!api || api->version < GN_API_VERSION || api->size < sizeof(gn_api)) return 1;         \
        ::gn::detail::apiPtr() = api;                                                             \
        ::gn::detail::modName() = NAME;                                                           \
        return gn_mod_main() ? 0 : 1;                                                             \
    }                                                                                             \
    static bool gn_mod_main()
