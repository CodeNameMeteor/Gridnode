// The gn_api table handed to mods, and the runtime's singletons.

#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/mod_call.hpp"
#include "gridnode/version.hpp"
#include "host.hpp"

namespace gn::host {
namespace e = gn::engine;

void logHookException(const char* where, const char* what) {
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 50) GN_LOG("%s threw%s%s%s; ignored (the game's own code went on)", where, what ? " (" : "", what ? what : "", what ? ")" : "");
}

reflect::Registry& registry() {
    static reflect::Registry r;
    return r;
}

hook::HookTable& hooks() {
    static hook::HookTable t({
        [](uint64_t slot, uint64_t& v) { return mem::read(static_cast<uintptr_t>(slot), v); },
        [](uint64_t slot, uint64_t v) {
            if (isDetourSlot(slot)) { // a function detour's slot: GridNode's own read-write data
                InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot), static_cast<LONG64>(v));
                return true;
            }
            return mem::patchPointer(static_cast<uintptr_t>(slot), static_cast<uintptr_t>(v), nullptr);
        },
        [](uint64_t fn) {
            if (!mem::inImage(static_cast<uintptr_t>(fn))) return false;
            for (const mem::Section& s : mem::imageSections())
                if (s.region.code && fn >= s.region.begin && fn < s.region.end) return true;
            return false;
        },
        [](uint64_t fn) {
            // Another module's code in the slot: taken as its hook and chained over (GridNode and MECMP in
            // either order). Said each time, with the module, since any module passes (an overlay too).
            if (!mem::foreignCode(static_cast<uintptr_t>(fn))) return false;
            HMODULE m = nullptr;
            char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(fn)), &m))
                GetModuleFileNameA(m, path, MAX_PATH);
            GN_LOG("hooks: a slot holds %llx, in %s: taken as that module's hook, GridNode's goes on top of it",
                   static_cast<unsigned long long>(fn), path);
            return true;
        },
    });
    return t;
}

thread::Dispatcher& server() {
    static thread::Dispatcher d;
    return d;
}

thread::Dispatcher& client() {
    static thread::Dispatcher d;
    return d;
}

namespace {

const reflect::Type* T(const gn_type* t) { return reinterpret_cast<const reflect::Type*>(t); }
const gn_type* G(const reflect::Type* t) { return reinterpret_cast<const gn_type*>(t); }

int category(const reflect::Type* t) {
    if (!t) return GN_CATEGORY_UNKNOWN;
    switch (t->category) {
    case reflect::Category::Primitive: return GN_CATEGORY_PRIMITIVE;
    case reflect::Category::ValueType: return GN_CATEGORY_VALUETYPE;
    case reflect::Category::Class: return GN_CATEGORY_CLASS;
    case reflect::Category::Array: return GN_CATEGORY_ARRAY;
    case reflect::Category::FixedArray: return GN_CATEGORY_FIXEDARRAY;
    case reflect::Category::Enum: return GN_CATEGORY_ENUM;
    case reflect::Category::Unknown: return GN_CATEGORY_UNKNOWN;
    default: return GN_CATEGORY_OTHER;
    }
}

void fill(const reflect::FieldInfo& fi, gn_field* out) {
    out->name = fi.field->name.c_str();
    out->offset = fi.field->offset;
    out->size = fi.size;
    out->flags = fi.field->flags;
    out->category = static_cast<uint16_t>(category(fi.type));
    out->type = G(fi.type);
    out->owner = G(fi.owner);
}

void apiLog(const char* mod, const char* message) { GN_LOG("[%s] %s", mod ? mod : "?", message ? message : ""); }
uint64_t apiModuleBase() { return mem::moduleBase(); }
uint64_t apiRva(uint64_t va) { return mem::rva(va); }

struct CallTick {
    gn_tick_fn fn;
    void* user;
    void operator()(float dt) const {
        modcall::Scope scope(fn);
        fn(dt, user);
    }
};
struct CallTask {
    gn_task_fn fn;
    void* user;
    void operator()() const {
        modcall::Scope scope(fn);
        fn(user);
    }
};

int apiOnServerTick(gn_tick_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    const std::string who = modNameAt(reinterpret_cast<const void*>(fn)); // the mod whose code fn is in
    server().addListener(who.empty() ? "mod" : who, CallTick{fn, user});
    return GN_OK;
}
int apiOnClientTick(gn_tick_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    const std::string who = modNameAt(reinterpret_cast<const void*>(fn));
    client().addListener(who.empty() ? "mod" : who, CallTick{fn, user});
    return GN_OK;
}
void apiRunOnServer(gn_task_fn fn, void* user) {
    if (fn) server().post(CallTask{fn, user});
}
void apiRunOnClient(gn_task_fn fn, void* user) {
    if (fn) client().post(CallTask{fn, user});
}
int apiIsServerThread() { return isServerThread() ? 1 : 0; }
int apiIsClientThread() { return isClientThread() ? 1 : 0; }

int apiHookSlot(const char* mod, uint64_t slotVa, uint64_t expectedVa, void* detour, void** original) {
    if (!detour || !original || !slotVa) return GN_ERR_ARGUMENT;
    if (!mem::patchableSlot(mem::rva(slotVa))) {
        GN_LOG("[%s] hook refused: 0x%llx is not a pointer slot in the game's data (aligned, in the image, outside code)", mod ? mod : "?",
               static_cast<unsigned long long>(slotVa));
        return GN_ERR_SLOT;
    }
    std::string why;
    const auto r = hooks().install(mod ? mod : "?", mem::rva(slotVa), expectedVa ? mem::rva(expectedVa) : 0,
                                   reinterpret_cast<uint64_t>(detour), reinterpret_cast<uint64_t*>(original), &why);
    if (r == hook::Result::Ok) {
        GN_LOG("[%s] hooked slot 0x%llx", mod ? mod : "?", static_cast<unsigned long long>(slotVa));
        noteSharedHook(mod ? mod : "?", mem::rva(slotVa));
        return GN_OK;
    }
    GN_LOG("[%s] hook refused: %s", mod ? mod : "?", why.c_str());
    return r == hook::Result::Argument ? GN_ERR_ARGUMENT : GN_ERR_SLOT;
}

const gn_type* apiTypeFind(const char* name) { return name ? G(registry().find(name)) : nullptr; }
const gn_type* apiTypeOf(const void* object) {
    return G(registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uint64_t>(object)));
}
int apiIsKindOf(const gn_type* t, const gn_type* base) { return registry().isKindOf(T(t), T(base)) ? 1 : 0; }
const char* apiTypeName(const gn_type* t) { return t ? T(t)->name.c_str() : ""; }
uint32_t apiTypeSize(const gn_type* t) { return t ? T(t)->size : 0; }
int apiTypeCategory(const gn_type* t) { return category(T(t)); }
const gn_type* apiTypeSuper(const gn_type* t) { return G(registry().super(T(t))); }
uint64_t apiTypeVa(const gn_type* t) { return t ? mem::va(static_cast<uintptr_t>(T(t)->ti)) : 0; }

int apiFieldFind(const gn_type* t, const char* name, gn_field* out) {
    if (!t || !name || !out) return GN_ERR_ARGUMENT;
    reflect::FieldInfo fi;
    if (!registry().findField(T(t), name, fi)) return GN_ERR_NOT_FOUND;
    fill(fi, out);
    return GN_OK;
}
uint32_t apiFieldCount(const gn_type* t) { return t ? static_cast<uint32_t>(T(t)->fields.size()) : 0; }
int apiFieldAt(const gn_type* t, uint32_t i, gn_field* out) {
    if (!t || !out || i >= T(t)->fields.size()) return GN_ERR_ARGUMENT;
    fill(registry().describe(T(t), T(t)->fields[i]), out);
    return GN_OK;
}

int apiRead(const void* address, void* out, size_t size) {
    return out && mem::read(reinterpret_cast<uintptr_t>(address), out, size) ? GN_OK : GN_ERR_ARGUMENT;
}
int apiWrite(void* address, const void* data, size_t size) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(address);
    // write() reaches the game's code too (WriteProcessMemory changes the protection), but such a
    // write is not in the patch list: nothing undoes it on unload. Said once per run; patch_bytes is
    // the recorded way (docs/modding.md).
    if (data && size && mem::inImage(at)) {
        static std::atomic<bool> told{false};
        for (const mem::Section& sec : mem::imageSections())
            if (sec.region.code && at < sec.region.end && at + size > sec.region.begin) {
                if (!told.exchange(true))
                    GN_LOG("write: a mod wrote %llu bytes of the game's code at %llx; that is not recorded or undone on unload (use patch_bytes)",
                           static_cast<unsigned long long>(size), static_cast<unsigned long long>(mem::va(at)));
                break;
            }
    }
    return data && mem::write(at, data, size) ? GN_OK : GN_ERR_ARGUMENT;
}

int apiOnCreated(const char* mod, const char* className, gn_created_fn fn, void* user) {
    if (!className) return GN_ERR_ARGUMENT;
    return onCreated(mod ? mod : "?", className, fn, user);
}

void* apiLocalClientCharacter() { return localClientCharacter(); }
void* apiLocalServerCharacter() { return localServerCharacter(); }
int apiEntityTransform(const void* entity, gn_transform* out) {
    return out && entityTransform(entity, *out) ? GN_OK : GN_ERR_ARGUMENT;
}
int apiTeleport(void* character, const gn_transform* to) { return teleport(character, to); }
void* apiGetComponent(void* character, const gn_type* type) { return getComponent(character, type); }
size_t apiLevelName(char* out, size_t size) {
    const std::string n = levelName();
    if (out && size) {
        const size_t k = n.size() < size - 1 ? n.size() : size - 1;
        memcpy(out, n.data(), k);
        out[k] = 0;
    }
    return n.size();
}
int apiOnLevel(const char* mod, gn_level_fn fn, void* user) { return onLevel(mod ? mod : "?", fn, user); }
int apiOnLevelReady(const char* mod, gn_level_ready_fn fn, void* user) { return onLevelReady(mod ? mod : "?", fn, user); }
int apiOnDestroyed(const char* mod, const char* cls, gn_destroyed_fn fn, void* user) {
    return cls ? onDestroyed(mod ? mod : "?", cls, fn, user) : GN_ERR_ARGUMENT;
}
int apiOnEvent(const char* mod, const char* event, const char* cls, gn_event_fn fn, void* user) {
    return onEvent(mod ? mod : "?", event, cls, fn, user);
}
int apiViewGet(gn_view* out) { return out && layouts::view(*out) ? GN_OK : out ? GN_ERR_UNAVAILABLE : GN_ERR_ARGUMENT; }
int apiOnMovement(const char* mod, gn_movement_fn fn, void* user) { return onMovement(mod ? mod : "?", fn, user); }
int apiOnUi(const char* mod, gn_ui_fn fn, void* user) { return ui::onUi(mod ? mod : "?", fn, user); }
void* apiUiContext() { return ui::context(); }
void apiUiAllocators(void** a, void** f, void** u) { ui::allocators(a, f, u); }
int apiUiCheck(const char* mod, const char* v, size_t io, size_t style, size_t vec2, size_t vec4, size_t vert, size_t idx) {
    return ui::check(mod, v, io, style, vec2, vec4, vert, idx);
}
int apiUiMenuOpen() { return ui::menuOpen() ? 1 : 0; }
int apiUiMenuSet(int open) {
    return ui::setMenu(open != 0) ? GN_OK : GN_ERR_UNAVAILABLE;
}

uint32_t apiSettingsCount() { return static_cast<uint32_t>(settingsCount()); }
int apiSettingsAt(uint32_t i, const gn_type** type, void** object) {
    SettingsEntry e;
    if (!settingsAt(i, e)) return GN_ERR_ARGUMENT;
    if (type) *type = G(e.type);
    if (object) *object = reinterpret_cast<void*>(e.object);
    return GN_OK;
}
const char* apiOnlineWarning(const char* name) { return name ? onlineWarning(name) : nullptr; }

int consoleRunFor(const char* mod, const char* line, gn_console_fn fn, void* user, const void* caller) {
    if (!line) return GN_ERR_ARGUMENT;
    if (!consoleAvailable()) return GN_ERR_UNAVAILABLE;
    const std::string who = mod ? mod : "?";
    const std::string l = line;
    if (const char* why = commandOnline(l)) {
        // Online commands need the mod to have said it changes online services (online_allow):
        // enforced here, where the command runs, not only in the menu (review, 2026-11-04).
        if (!onlineAllowed(caller)) {
            GN_LOG("[%s] console: \"%s\" refused (online: %s). A mod that changes online services on purpose calls online_allow first.",
                   who.c_str(), l.c_str(), why);
            return GN_ERR_UNAVAILABLE;
        }
        GN_LOG("!!! ONLINE: [%s] console: \"%s\" (%s)", who.c_str(), l.c_str(), why);
    }
    client().post([who, l, fn, user] {
        const std::string out = consoleRun(l);
        // Queued, then its mod unloaded: its callback is gone with it (the command still ran).
        if (fn && modCodeLive(reinterpret_cast<const void*>(fn))) {
            modcall::Scope scope(fn);
            fn(out.c_str(), user);
        }
    });
    return GN_OK;
}

// A mod that takes an online settings object without having called online_allow: GridNode cannot
// see the writes it makes (raw memory), so it warns once per mod and class instead.
void* settingsFindFor(const char* name, const void* caller) {
    if (!name) return nullptr;
    void* obj = reinterpret_cast<void*>(settingsFind(name));
    if (const char* why = obj ? onlineWarning(name) : nullptr; why && !onlineAllowed(caller)) {
        static std::mutex m;
        static std::set<std::string> told;
        const std::string mod = modNameAt(caller), key = mod + "/" + name;
        std::lock_guard<std::mutex> lock(m);
        if (told.insert(key).second)
            GN_LOG("WARNING: mod %s took %s (online: %s). Changing it can affect online play; a mod that does so on purpose calls "
                   "online_allow.", mod.c_str(), name, why);
    }
    return obj;
}

uint32_t apiConsoleCommandCount() { return static_cast<uint32_t>(consoleCommandCount()); }
int apiConsoleCommandAt(uint32_t i, gn_command* out) {
    ConsoleCommand c;
    if (!out || !consoleCommandAt(i, c)) return GN_ERR_ARGUMENT;
    out->name = c.name;
    out->group = c.group;
    out->full = c.full;
    out->help = c.help;
    out->flags = (c.setting ? GN_COMMAND_SETTING : 0u) | (c.online ? GN_COMMAND_ONLINE : 0u);
    return GN_OK;
}

int apiOnMessage(const char* mod, const char* name, gn_message_fn fn, void* user) {
    return name ? onMessage(mod ? mod : "?", name, fn, user) : GN_ERR_ARGUMENT;
}

int apiRaycast(const float* from, const float* to, float radius, gn_ray_hit* out) { return raycast(from, to, radius, out); }

} // namespace

namespace {
// The mod is the one whose code called: the return address is in its image (a direct call through
// the table, so never inlined into the mod's caller).
#if defined(_MSC_VER)
#define GN_CALLER callerOf(_ReturnAddress())
#define GN_NOINLINE __declspec(noinline)
#else
#define GN_CALLER callerOf(__builtin_return_address(0))
#define GN_NOINLINE __attribute__((noinline))
#endif
GN_NOINLINE int apiCameraSet(const gn_transform* t, uint32_t flags) { return layouts::cameraSet(GN_CALLER, t, flags); }
GN_NOINLINE int apiTimeOfDayHold(float seconds) { return timeOfDayHold(GN_CALLER, seconds); }
GN_NOINLINE int apiStreamingFocusAdd(const float pos[3], uint32_t* handle) { return layouts::streamingFocusAdd(GN_CALLER, pos, handle); }
GN_NOINLINE int apiVisualEnvSet(uint32_t offset, const void* data, uint32_t size) { return visualEnvSet(GN_CALLER, offset, data, size); }
GN_NOINLINE int apiCameraFovSet(float degrees) { return layouts::cameraFovSet(GN_CALLER, degrees); }
GN_NOINLINE int apiBundleLoad(const char* name, uint32_t* handle) { return bundleLoad(GN_CALLER, name, handle); }
GN_NOINLINE int apiPrefabSpawn(const char* asset, const gn_transform* at, uint32_t bundle, uint32_t* handle) {
    return prefabSpawn(GN_CALLER, asset, at, bundle, handle);
}

// The UI's text lookup. Without a localiser (or an unknown id) the game leaves the buffer empty; the id is
// given back then, and when the "text" is the id itself or not printable.
int apiLocalize(const char* id, char* out, uint32_t cap) {
    if (!id || !out || !cap) return GN_ERR_ARGUMENT;
    out[0] = 0;
    char buf[512] = {};
    void* noArgs = nullptr; // the game's own callers pass a pointer to the arguments, none here
    const bool ok = reinterpret_cast<uint8_t (*)(char*, uint32_t, const char*, void*)>(mem::rva(e::function::Localize_format))(
                        buf, sizeof(buf), id, &noArgs) != 0;
    buf[sizeof(buf) - 1] = 0;
    bool printable = buf[0] != 0;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(buf); *c; ++c) printable = printable && *c >= 0x20;
    const char* text = ok && printable && std::strcmp(buf, id) != 0 ? buf : id;
    std::snprintf(out, cap, "%s", text);
    return text == buf ? GN_OK : GN_ERR_NOT_FOUND;
}

// The game's pause request counters (the controller PamUISystem +0x8c8 points to: the ints at +0x08 and +0x0c). Read only; the object is
// checked by its vtable because the global is zeroed when the UI system is destroyed.
int apiUiPauseState(int32_t* menus, int32_t* full) {
    if (!menus || !full) return GN_ERR_ARGUMENT;
    const uintptr_t ui = mem::readOr<uintptr_t>(mem::rva(e::global::PamUISystemInstance));
    if (!ui || mem::readOr<uintptr_t>(ui) != mem::rva(e::cls::PamUISystem::vtable)) return GN_ERR_UNAVAILABLE;
    const uintptr_t ctrl = mem::readOr<uintptr_t>(ui + 0x8c8); // a pointer, not an inline struct
    if (ctrl < 0x10000) return GN_ERR_UNAVAILABLE;
    int32_t pair[2] = {};
    if (!mem::read(ctrl + 0x08, pair, sizeof(pair))) return GN_ERR_UNAVAILABLE;
    *menus = pair[0];
    *full = pair[1];
    return GN_OK;
}
} // namespace

namespace {
// The mod is the one whose code called: the return address is in its image (a direct call through
// the table, so never inlined into the mod's caller).
#if defined(_MSC_VER)
__declspec(noinline) int apiModUnload() { return modUnloadAt(callerOf(_ReturnAddress())); }
#else
__attribute__((noinline)) int apiModUnload() { return modUnloadAt(callerOf(__builtin_return_address(0))); }
#endif
#if defined(_MSC_VER)
#define GN_CALLER callerOf(_ReturnAddress())
#define GN_NOINLINE __declspec(noinline)
#else
#define GN_CALLER callerOf(__builtin_return_address(0))
#define GN_NOINLINE __attribute__((noinline))
#endif
// A patch belongs to the code that made it (undone when that mod is unloaded).
GN_NOINLINE int apiPatchBytes(const char* mod, uint64_t va, const void* expected, const void* bytes, size_t size) {
    return patchBytes(mod, va, expected, bytes, size, GN_CALLER);
}
GN_NOINLINE int apiPatchCall(const char* mod, uint64_t site, uint64_t target, void* detour, void** original) {
    return patchCall(mod, site, target, detour, original, GN_CALLER);
}
GN_NOINLINE int apiModExport(const char* name, void* fn) { return modExport(GN_CALLER, name, fn); }
GN_NOINLINE void* apiModImport(const char* modId, const char* name) { return modImport(GN_CALLER, modId, name); }
GN_NOINLINE int apiModDir(char* out, size_t size) { return modDir(GN_CALLER, out, size); }
GN_NOINLINE int apiConsoleRun(const char* mod, const char* line, gn_console_fn fn, void* user) { return consoleRunFor(mod, line, fn, user, GN_CALLER); }
GN_NOINLINE void* apiSettingsFind(const char* name) { return settingsFindFor(name, GN_CALLER); }
GN_NOINLINE int apiOnlineAllow(const char* mod, const char* reason) { return onlineAllow(GN_CALLER, mod, reason); }
GN_NOINLINE int apiViewGetEx(gn_view* out, uint32_t size) {
    // The caller's size says how much of gn_view it has: a later gn_view may grow at its end, and
    // a host fills only the fields both know (the rest stay as the caller set them).
    if (!out || size < sizeof(gn_view)) return GN_ERR_ARGUMENT;
    gn_view v{};
    const int r = apiViewGet(&v);
    if (r == GN_OK) memcpy(out, &v, sizeof(v) < size ? sizeof(v) : size);
    return r;
}
GN_NOINLINE int apiSetGravity(int realm, const float* g) { return physicsSetGravity(realm, g, GN_CALLER); }
GN_NOINLINE int apiSettingAdd(const char* mod, const gn_setting* s) { return settingAdd(mod, s, GN_CALLER); }
GN_NOINLINE int apiBoxAdd(const char* mod, const gn_transform* at, const float* half, uint32_t* out) { return boxAdd(mod, at, half, out, GN_CALLER); }
GN_NOINLINE int apiShapeAdd(const char* mod, uint32_t shape, const gn_transform* at, const float* size, uint32_t motion, float mass, uint32_t* out) {
    return shapeAdd(mod, shape, at, size, motion, mass, out, GN_CALLER);
}
GN_NOINLINE int apiSoundPlay(const char* mod, const char* path, const char* event, const float* pos, uint32_t* out) {
    return soundPlay(mod, path, event, pos, out, GN_CALLER);
}
GN_NOINLINE int apiBusVolumeSet(const char* mod, const char* bus, float gain) { return busVolumeSet(mod, bus, gain, GN_CALLER); }
GN_NOINLINE int apiBusDuck(const char* mod, const char* bus, float gain, float seconds) { return busDuck(mod, bus, gain, seconds, GN_CALLER); }
} // namespace

namespace {
// API 37: the same calls with the mod's handle (gn_mod_load2) instead of its name and return address.
#define GN_SELF(fail)                                                                                            \
    std::string id;                                                                                              \
    const void* who = nullptr;                                                                                   \
    if (!modContext(self, id, who)) return fail;
int mUnload(gn_mod* self) { GN_SELF(GN_ERR_ARGUMENT) return modUnloadAt(who); }
int mPatchBytes(gn_mod* self, uint64_t va, const void* e, const void* b, size_t n) { GN_SELF(GN_ERR_ARGUMENT) return patchBytes(id.c_str(), va, e, b, n, who); }
int mPatchCall(gn_mod* self, uint64_t site, uint64_t target, void* detour, void** original) {
    GN_SELF(GN_ERR_ARGUMENT) return patchCall(id.c_str(), site, target, detour, original, who);
}
int mExport(gn_mod* self, const char* name, void* fn) { GN_SELF(GN_ERR_ARGUMENT) return modExport(who, name, fn); }
void* mImport(gn_mod* self, const char* modId, const char* name) { GN_SELF(nullptr) return modImport(who, modId, name); }
int mDir(gn_mod* self, char* out, size_t size) { GN_SELF(GN_ERR_ARGUMENT) return modDir(who, out, size); }
int mConsoleRun(gn_mod* self, const char* line, gn_console_fn fn, void* user) { GN_SELF(GN_ERR_ARGUMENT) return consoleRunFor(id.c_str(), line, fn, user, who); }
void* mSettingsFind(gn_mod* self, const char* name) { GN_SELF(nullptr) return settingsFindFor(name, who); }
int mOnlineAllow(gn_mod* self, const char* reason) { GN_SELF(GN_ERR_ARGUMENT) return onlineAllow(who, id.c_str(), reason); }
int mSettingAdd(gn_mod* self, const gn_setting* s) { GN_SELF(GN_ERR_ARGUMENT) return settingAdd(id.c_str(), s, who); }
int mBoxAdd(gn_mod* self, const gn_transform* at, const float* half, uint32_t* out) { GN_SELF(GN_ERR_ARGUMENT) return boxAdd(id.c_str(), at, half, out, who); }
int mSoundPlay(gn_mod* self, const char* path, const char* event, const float* pos, uint32_t* out) {
    GN_SELF(GN_ERR_ARGUMENT) return soundPlay(id.c_str(), path, event, pos, out, who);
}
int mBusVolumeSet(gn_mod* self, const char* bus, float gain) { GN_SELF(GN_ERR_ARGUMENT) return busVolumeSet(id.c_str(), bus, gain, who); }
int mBusDuck(gn_mod* self, const char* bus, float gain, float seconds) { GN_SELF(GN_ERR_ARGUMENT) return busDuck(id.c_str(), bus, gain, seconds, who); }
int mSetGravity(gn_mod* self, int realm, const float* g) { GN_SELF(GN_ERR_ARGUMENT) return physicsSetGravity(realm, g, who); }
int mCameraSet(gn_mod* self, const gn_transform* t, uint32_t flags) { GN_SELF(GN_ERR_ARGUMENT) return layouts::cameraSet(who, t, flags); }
int mCameraFovSet(gn_mod* self, float degrees) { GN_SELF(GN_ERR_ARGUMENT) return layouts::cameraFovSet(who, degrees); }
int mTimeOfDayHold(gn_mod* self, float seconds) { GN_SELF(GN_ERR_ARGUMENT) return timeOfDayHold(who, seconds); }
int mVisualEnvSet(gn_mod* self, uint32_t offset, const void* data, uint32_t size) { GN_SELF(GN_ERR_ARGUMENT) return visualEnvSet(who, offset, data, size); }
int mStreamingFocusAdd(gn_mod* self, const float* pos, uint32_t* handle) { GN_SELF(GN_ERR_ARGUMENT) return layouts::streamingFocusAdd(who, pos, handle); }
int mBundleLoad(gn_mod* self, const char* name, uint32_t* handle) { GN_SELF(GN_ERR_ARGUMENT) return bundleLoad(who, name, handle); }
int mPrefabSpawn(gn_mod* self, const char* asset, const gn_transform* at, uint32_t bundle, uint32_t* handle) {
    GN_SELF(GN_ERR_ARGUMENT) return prefabSpawn(who, asset, at, bundle, handle);
}
#undef GN_SELF
} // namespace

const gn_api* api() {
    static const gn_api table = [] {
        gn_api a{};
        a.version = GN_API_VERSION;
        a.size = sizeof(gn_api);
        a.gridnode_version = GRIDNODE_VERSION_STRING;
        a.log = apiLog;
        a.module_base = apiModuleBase;
        a.rva = apiRva;
        a.on_server_tick = apiOnServerTick;
        a.on_client_tick = apiOnClientTick;
        a.run_on_server = apiRunOnServer;
        a.run_on_client = apiRunOnClient;
        a.is_server_thread = apiIsServerThread;
        a.is_client_thread = apiIsClientThread;
        a.hook_slot = apiHookSlot;
        a.type_find = apiTypeFind;
        a.type_of = apiTypeOf;
        a.is_kind_of = apiIsKindOf;
        a.type_name = apiTypeName;
        a.type_size = apiTypeSize;
        a.type_category = apiTypeCategory;
        a.type_super = apiTypeSuper;
        a.type_va = apiTypeVa;
        a.field_find = apiFieldFind;
        a.field_count = apiFieldCount;
        a.field_at = apiFieldAt;
        a.read = apiRead;
        a.write = apiWrite;
        a.on_created = apiOnCreated;
        a.local_client_character = apiLocalClientCharacter;
        a.local_server_character = apiLocalServerCharacter;
        a.entity_transform = apiEntityTransform;
        a.teleport = apiTeleport;
        a.get_component = apiGetComponent;
        a.level_name = apiLevelName;
        a.on_level = apiOnLevel;
        a.on_ui = apiOnUi;
        a.ui_context = apiUiContext;
        a.ui_allocators = apiUiAllocators;
        a.ui_check = apiUiCheck;
        a.ui_menu_open = apiUiMenuOpen;
        a.settings_find = apiSettingsFind;
        a.settings_count = apiSettingsCount;
        a.settings_at = apiSettingsAt;
        a.online_warning = apiOnlineWarning;
        a.console_run = apiConsoleRun;
        a.console_command_count = apiConsoleCommandCount;
        a.console_command_at = apiConsoleCommandAt;
        a.on_message = apiOnMessage;
        a.raycast = apiRaycast;
        a.key_find = keyFind;
        a.key_press = keyPress;
        a.jump = jump;
        a.movement_get = movementGet;
        a.movement_set = movementSet;
        a.tuning_count = tuningCount;
        a.tuning_at = tuningAt;
        a.tuning_set = tuningSet;
        a.game_state = gameStateGet;
        a.time_of_day_set = timeOfDaySet;
        a.position_set = positionSet;
        a.god_mode = godModeSet;
        a.action_press = actionPress;
        a.asset_find = assetFind;
        a.asset_release = assetRelease;
        a.entities_near = entitiesNear;
        a.entity_set_transform = entitySetTransform;
        a.landing_prediction = landingPrediction;
        a.config_get = configGet;
        a.config_set = configSet;
        a.hotkey_add = hotkeyAdd;
        a.hotkey_info = hotkeyInfo;
        a.hotkey_rebind = hotkeyRebind;
        a.hotkey_set = hotkeySet;
        a.on_movement = apiOnMovement;
        a.view_get = apiViewGet;
        a.on_event = apiOnEvent;
        a.event_name = eventName;
        a.event_send = [](const char* mod, void* entity, const char* event) { return eventSend(mod ? mod : "?", entity, event); };
        a.entities_of_class = entitiesOfClass;
        a.freecam_set = [](int on) { return layouts::freecamSet(on != 0); };
        a.freecam_get = layouts::freecamGet;
        a.freecam_teleport = [](int to, int keep) { return layouts::teleportRequest(to, keep != 0); };
        a.event_send_class = [](const char* mod, const char* cls, const char* event) { return eventSendClass(mod ? mod : "?", cls, event); };
        a.mod_unload = apiModUnload;
        a.patch_bytes = apiPatchBytes;
        a.patch_call = apiPatchCall;
        a.hook_function = [](const char* mod, uint64_t fn, void* detour, void** original) {
            return hookFunction(mod ? mod : "?", fn, detour, original);
        };
        a.mod_export = apiModExport;
        a.mod_import = apiModImport;
        a.mod_version = modVersion;
        a.setting_add = apiSettingAdd;
        a.setting_number = settingNumber;
        a.setting_text = settingText;
        a.setting_set_number = settingSetNumber;
        a.setting_set_text = settingSetText;
        a.setting_on_change = settingOnChange;
        a.settings_page = settingsPage;
        a.raycast_object = apiRaycastObject;
        a.entity_bodies = apiEntityBodies;
        a.body_transform = apiBodyTransform;
        a.body_set_transform = apiBodySetTransform;
        a.entity_other_half = apiEntityOtherHalf;
        a.box_add = apiBoxAdd;
        a.box_move = boxMove;
        a.box_remove = boxRemove;
        a.box_state = boxState;
        a.sound_play = apiSoundPlay;
        a.sound_event = soundEvent;
        a.sound_move = soundMove;
        a.sound_playing = soundPlaying;
        a.sound_stop = soundStop;
        a.character_despawn = characterDespawn;
        a.mod_dir = apiModDir;
        a.body_velocity = apiBodyVelocity;
        a.body_set_velocity = apiBodySetVelocity;
        a.body_push = apiBodyPush;
        a.physics_gravity = physicsGravity;
        a.physics_set_gravity = apiSetGravity;
        a.character_health = characterHealth;
        a.character_set_health = characterSetHealth;
        a.waypoint = waypoint;
        a.checkpoint = checkpoint;
        a.progress_flags = progressFlags;
        a.progress_find = progressFind;
        a.progress_set = progressSet;
        a.mission = mission;
        a.online_allow = apiOnlineAllow;
        a.view_get_ex = apiViewGetEx;
        a.action_held = actionHeld;
        a.ant_type_count = antTypeCount;
        a.ant_type_at = antTypeAt;
        a.ant_type_find = antTypeFind;
        a.ant_field_at = antFieldAt;
        a.ant_field_find = antFieldFind;
        a.move_tracker = moveTracker;
        a.on_destroyed = apiOnDestroyed;
        a.on_level_ready = apiOnLevelReady;
        a.level_ready = levelReady;
        a.ground_below = groundBelow;
        a.ant_live_field = antLiveField;
        a.ant_objects = antObjects;
        a.camera_set = apiCameraSet;
        a.camera_fov_set = apiCameraFovSet;
        a.replay_recent = replayRecent;
        a.ui_menu_set = apiUiMenuSet;
        a.time_of_day_hold = apiTimeOfDayHold;
        a.streaming_focus_add = apiStreamingFocusAdd;
        a.streaming_focus_move = layouts::streamingFocusMove;
        a.streaming_focus_remove = layouts::streamingFocusRemove;
        a.localize = apiLocalize;
        a.ui_pause_state = apiUiPauseState;
        a.raycast_filtered = apiRaycastFiltered;
        a.box_backend = boxBackend;
        a.bus_volume_get = busVolumeGet;
        a.bus_volume_set = apiBusVolumeSet;
        a.bus_duck = apiBusDuck;
        a.type_vtable_count = [](const gn_type* t) { return typeVtableCount(T(t)); };
        a.type_vtable_at = [](const gn_type* t, uint32_t i) { return typeVtableAt(T(t), i); };
        a.type_virtual_find = [](const gn_type* t, const char* name, uint32_t* out) { return typeVirtualFind(T(t), name, out); };
        a.hook_virtual = [](const char* mod, const gn_type* t, uint32_t offset, void* detour, uint32_t flags, uint32_t* out) {
            return hookVirtual(mod, T(t), offset, detour, flags, out);
        };
        a.hook_original = hookOriginal;
        a.hook_virtual_remove = hookVirtualRemove;
        a.entity_component = entityComponent;
        a.entity_components = entityComponents;
        a.data_path = dataPathCopy;
        a.hooks_begin = hooksBegin;
        a.hooks_commit = hooksCommit;
        a.mod_unload_m = mUnload;
        a.patch_bytes_m = mPatchBytes;
        a.patch_call_m = mPatchCall;
        a.mod_export_m = mExport;
        a.mod_import_m = mImport;
        a.mod_dir_m = mDir;
        a.console_run_m = mConsoleRun;
        a.settings_find_m = mSettingsFind;
        a.online_allow_m = mOnlineAllow;
        a.setting_add_m = mSettingAdd;
        a.box_add_m = mBoxAdd;
        a.sound_play_m = mSoundPlay;
        a.bus_volume_set_m = mBusVolumeSet;
        a.bus_duck_m = mBusDuck;
        a.physics_set_gravity_m = mSetGravity;
        a.camera_set_m = mCameraSet;
        a.camera_fov_set_m = mCameraFovSet;
        a.time_of_day_hold_m = mTimeOfDayHold;
        a.visual_env_set_m = mVisualEnvSet;
        a.streaming_focus_add_m = mStreamingFocusAdd;
        a.bundle_load = apiBundleLoad;
        a.bundle_state = bundleState;
        a.bundle_release = bundleRelease;
        a.prefab_spawn = apiPrefabSpawn;
        a.prefab_state = prefabState;
        a.prefab_destroy = prefabDestroy;
        a.bundle_load_m = mBundleLoad;
        a.prefab_spawn_m = mPrefabSpawn;
        a.data_of_class = dataOfClass;
        a.on_data = [](const char* mod, gn_data_fn fn, void* user) { return onData(mod ? mod : "?", fn, user); };
        a.property_get = propertyGet;
        a.property_set = propertySet;
        a.property_watch = [](const char* mod, void* entity, const char* name, gn_property_fn fn, void* user, uint32_t* handle) {
            return propertyWatch(mod ? mod : "?", entity, name, fn, user, handle);
        };
        a.property_unwatch = propertyUnwatch;
        a.message_init = messageInit;
        a.message_send = messageSend;
        a.shape_add = apiShapeAdd;
        a.ui_type_view = [](const char* mod, const char* cls, gn_type_view_fn fn, void* user) { return ui::typeView(mod ? mod : "?", cls, fn, user); };
        a.visual_env_get = visualEnvGet;
        a.visual_env_set = apiVisualEnvSet;
        a.visual_env_release = visualEnvRelease;
        a.timeline_time = timelineTime;
        a.timeline_set_time = timelineSetTime;
        a.gridnode_unload = [] {
            unloadRequest();
            return GN_OK;
        };
        return a;
    }();
    return &table;
}

void forgetTicks(const CodeRange& r) {
    auto mine = [r](const thread::Dispatcher::Tick& t) {
        const CallTick* c = t.target<CallTick>();
        return c && r.has(reinterpret_cast<void*>(c->fn));
    };
    server().removeListeners(mine);
    client().removeListeners(mine);
}

size_t forgetTasks(const CodeRange& r) {
    // run_on_server / run_on_client tasks still queued when the mod's DLL is about to be freed (a
    // realm that did not tick during the unload's wait). A dropped task's user pointer is not freed:
    // that is the mod's memory (gridnode/mod.hpp frees its closure inside the task), so a few bytes
    // leak rather than freed code run.
    auto mine = [r](const thread::Dispatcher::Task& t) {
        const CallTask* c = t.target<CallTask>();
        return c && r.has(reinterpret_cast<void*>(c->fn));
    };
    const size_t n = server().removeTasks(mine) + client().removeTasks(mine);
    if (n) GN_LOG("mods: %llu queued task(s) of an unloading mod dropped", static_cast<unsigned long long>(n));
    return n;
}

bool taskLive(const thread::Dispatcher::Task& t) {
    const CallTask* c = t.target<CallTask>();
    return !c || modCodeLive(reinterpret_cast<const void*>(c->fn));
}

size_t forgetHooks(const CodeRange& r, std::vector<std::string>* refused) {
    return hooks().remove([&](const hook::Entry& e) { return r.has(reinterpret_cast<void*>(e.detour)); }, refused);
}

} // namespace gn::host
