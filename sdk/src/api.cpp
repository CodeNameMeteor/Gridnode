// The gn_api table handed to mods, and the runtime's singletons.

#include <windows.h>

#include <cstring>

#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/version.hpp"
#include "host.hpp"

namespace gn::host {

reflect::Registry& registry() {
    static reflect::Registry r;
    return r;
}

hook::HookTable& hooks() {
    static hook::HookTable t({
        [](uint64_t slot, uint64_t& v) { return mem::read(static_cast<uintptr_t>(slot), v); },
        [](uint64_t slot, uint64_t v) { return mem::patchPointer(static_cast<uintptr_t>(slot), static_cast<uintptr_t>(v), nullptr); },
        [](uint64_t fn) {
            if (!mem::inImage(static_cast<uintptr_t>(fn))) return false;
            for (const mem::Section& s : mem::imageSections())
                if (s.region.code && fn >= s.region.begin && fn < s.region.end) return true;
            return false;
        },
        [](uint64_t fn) { return mem::foreignCode(static_cast<uintptr_t>(fn)); },
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
    void operator()(float dt) const { fn(dt, user); }
};
struct CallTask {
    gn_task_fn fn;
    void* user;
    void operator()() const { fn(user); }
};

int apiOnServerTick(gn_tick_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    server().addListener("mod", CallTick{fn, user});
    return GN_OK;
}
int apiOnClientTick(gn_tick_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    client().addListener("mod", CallTick{fn, user});
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
    return data && mem::write(reinterpret_cast<uintptr_t>(address), data, size) ? GN_OK : GN_ERR_ARGUMENT;
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

void* apiSettingsFind(const char* name) { return name ? reinterpret_cast<void*>(settingsFind(name)) : nullptr; }
uint32_t apiSettingsCount() { return static_cast<uint32_t>(settingsList().size()); }
int apiSettingsAt(uint32_t i, const gn_type** type, void** object) {
    const std::vector<SettingsEntry> l = settingsList();
    if (i >= l.size()) return GN_ERR_ARGUMENT;
    if (type) *type = G(l[i].type);
    if (object) *object = reinterpret_cast<void*>(l[i].object);
    return GN_OK;
}
const char* apiOnlineWarning(const char* name) { return name ? onlineWarning(name) : nullptr; }

int apiConsoleRun(const char* mod, const char* line, gn_console_fn fn, void* user) {
    if (!line) return GN_ERR_ARGUMENT;
    if (!consoleAvailable()) return GN_ERR_UNAVAILABLE;
    const std::string who = mod ? mod : "?";
    const std::string l = line;
    if (const char* why = commandOnline(l)) GN_LOG("[%s] console: \"%s\" (online: %s)", who.c_str(), l.c_str(), why);
    client().post([who, l, fn, user] {
        const std::string out = consoleRun(l);
        if (fn) fn(out.c_str(), user);
    });
    return GN_OK;
}
uint32_t apiConsoleCommandCount() { return static_cast<uint32_t>(consoleCommands().size()); }
int apiConsoleCommandAt(uint32_t i, gn_command* out) {
    const std::vector<ConsoleCommand> c = consoleCommands();
    if (i >= c.size() || !out) return GN_ERR_ARGUMENT;
    out->name = c[i].name;
    out->group = c[i].group;
    out->full = c[i].full;
    out->help = c[i].help;
    out->flags = (c[i].setting ? GN_COMMAND_SETTING : 0u) | (c[i].online ? GN_COMMAND_ONLINE : 0u);
    return GN_OK;
}

int apiOnMessage(const char* mod, const char* name, gn_message_fn fn, void* user) {
    return name ? onMessage(mod ? mod : "?", name, fn, user) : GN_ERR_ARGUMENT;
}

int apiRaycast(const float* from, const float* to, float radius, gn_ray_hit* out) { return raycast(from, to, radius, out); }

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
        return a;
    }();
    return &table;
}

} // namespace gn::host
