// An entity's properties by name, as its own logic reads and writes them (db/annotations/entity.yaml:
// EntityBus_findProperty, PropertyValue_set). A property is a field of the entity's data that the
// level's logic connects to (a bool a trigger sets, a float a timeline drives): the bus keeps a slot
// for each one, and the entity reads the slot, not its data. Writing the slot is what a connection
// does, so the change reaches the entity and whatever listens to it, without a hook (API 40).

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/mod_call.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;

// The bus interface every entity keeps (class_lists.hpp, gn::buses): the peers and the data's sorted id
// list run in step, so an entity's index among the peers is its data's index in the list.
constexpr uint64_t kEntityBus = 0x20, kPeers = 0x38, kTemplate = 0x60, kFlags = 0x34;
constexpr uint64_t kIds = 0xd0, kIdsOther = 0xf8;
constexpr uint32_t kOtherBit = 1u << 28;
constexpr size_t kMaxPeers = 1u << 16;
// The TypeInfoData's u16 flags at +8: the category in bits 4..8 (3 class, 6 and 7 strings).
constexpr uint32_t kCatClass = 3, kCatString = 6, kCatCString = 7;

uint32_t djb2(const char* s) {
    uint32_t h = 5381;
    for (; *s; ++s) h = (h * 33) ^ static_cast<unsigned char>(*s);
    return h;
}

bool parseId(const char* name, uint32_t& id) {
    if (name[0] != '0' || (name[1] != 'x' && name[1] != 'X') || !name[2]) return false;
    char* end = nullptr;
    const unsigned long long v = strtoull(name + 2, &end, 16);
    if (*end || v > 0xffffffffull) return false;
    id = static_cast<uint32_t>(v);
    return true;
}

struct Property {
    uintptr_t peer = 0, data = 0, slot = 0;
    const reflect::Type* dataType = nullptr;
    reflect::FieldInfo field;
    uint32_t id = 0, category = 0, size = 0;
};

// The field of t (or a base) whose name hashes to id.
bool fieldById(const reflect::Type* t, uint32_t id, reflect::FieldInfo& out) {
    for (int depth = 0; t && depth < 64; ++depth, t = registry().super(t))
        for (const reflect::Field& f : t->fields)
            if (djb2(f.name.c_str()) == id) {
                out = registry().describe(t, f);
                return true;
            }
    return false;
}

// The entity's bus peer and data: its place in its bus's peer array. 0 when it is on no list (a
// bus lists only the entities its data connects; those have no property slots either).
int entityData(uintptr_t entity, uintptr_t& peer, uintptr_t& data) {
    peer = mem::readOr<uintptr_t>(entity + kEntityBus);
    if (!peer || (peer & 7)) return GN_ERR_NOT_FOUND;
    const uintptr_t peers = mem::readOr<uintptr_t>(peer + kPeers), tmpl = mem::readOr<uintptr_t>(peer + kTemplate);
    if (!peers || !tmpl) return GN_ERR_NOT_FOUND;
    const uintptr_t ids = tmpl + ((mem::readOr<uint32_t>(peer + kFlags) & kOtherBit) ? kIdsOther : kIds);
    const uintptr_t begin = mem::readOr<uintptr_t>(ids), end = mem::readOr<uintptr_t>(ids + 8);
    if (!begin || end <= begin || (end - begin) % 8 || (end - begin) / 8 > kMaxPeers) return GN_ERR_NOT_FOUND;
    for (uintptr_t i = 0, n = (end - begin) / 8; i < n; ++i)
        if (mem::readOr<uintptr_t>(peers + i * 8) == entity) {
            data = mem::readOr<uintptr_t>(begin + i * 8);
            return data ? GN_OK : GN_ERR_NOT_FOUND;
        }
    return GN_ERR_NOT_FOUND;
}

bool onRealm(uintptr_t entity) {
    return (mem::readOr<uint32_t>(entity + 0x18) & 1) ? isServerThread() : isClientThread();
}

// Finds the slot (the game's own lookup; it may make a default value, hence the realm thread).
int find(void* entityPtr, const char* name, Property& p) {
    const auto entity = reinterpret_cast<uintptr_t>(entityPtr);
    if (!entity || !name || !*name) return GN_ERR_ARGUMENT;
    const reflect::Type* et = registry().typeOf(mem::ProcessMemory(), entity);
    const reflect::Type* base = registry().find("Entity");
    if (!et || !base || !registry().isKindOf(et, base) || !inspector::alive(entity, et)) return GN_ERR_ARGUMENT;
    if (!onRealm(entity)) return GN_ERR_THREAD;
    if (int r = entityData(entity, p.peer, p.data); r != GN_OK) return r;
    p.dataType = registry().typeOf(mem::ProcessMemory(), p.data);
    if (!p.dataType) return GN_ERR_NOT_FOUND;
    if (!parseId(name, p.id)) p.id = djb2(name);
    if (!fieldById(p.dataType, p.id, p.field) || !p.field.field || !p.field.field->typeTi) return GN_ERR_NOT_FOUND;
    using FindFn = void** (*)(uintptr_t, uintptr_t*, uintptr_t, uint32_t, uintptr_t, const void*);
    reinterpret_cast<FindFn>(mem::rva(e::function::EntityBus_findProperty))(p.peer, &p.slot, p.data, p.id,
                                                                            static_cast<uintptr_t>(p.field.field->typeTi), nullptr);
    if (!p.slot) return GN_ERR_NOT_FOUND; // not connected: the entity reads its data, not a slot
    const uintptr_t ti = mem::readOr<uintptr_t>(p.slot + 8);
    const uintptr_t tid = ti ? mem::readOr<uintptr_t>(ti) : 0;
    if (!tid) return GN_ERR_UNAVAILABLE;
    p.category = (mem::readOr<uint16_t>(tid + 8) >> 4) & 0x1f;
    p.size = (p.category == kCatClass || p.category == kCatString || p.category == kCatCString) ? 8u : mem::readOr<uint16_t>(tid + 10);
    return GN_OK;
}

} // namespace

// Runs fn (returning the console's text) on a realm thread and waits up to 2 s (the console runs on the
// render thread). The result lives in the shared state: a task that runs after the wait gave up writes
// there, not into a caller that has returned.
std::string runOnRealmAndWait(bool serverRealm, std::function<std::string()> fn) {
    struct State {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::string out;
    };
    auto st = std::make_shared<State>();
    auto task = [st, fn = std::move(fn)] {
        std::string r = fn();
        std::lock_guard<std::mutex> lock(st->m);
        st->out = std::move(r);
        st->done = true;
        st->cv.notify_all();
    };
    if (serverRealm) server().post(task);
    else client().post(task);
    std::unique_lock<std::mutex> lock(st->m);
    if (!st->cv.wait_for(lock, std::chrono::seconds(2), [&] { return st->done; }))
        return "(the realm did not tick within 2 s: paused or loading?)";
    return st->out;
}

int propertyGet(void* entity, const char* name, void* out, uint32_t size, const gn_type** type) {
    Property p;
    if (const int r = find(entity, name, p); r != GN_OK) return r;
    if (type) *type = reinterpret_cast<const gn_type*>(registry().byTi(mem::readOr<uintptr_t>(p.slot + 8)));
    if (!out) return GN_OK; // only the type asked
    if (size != p.size) return GN_ERR_ARGUMENT;
    const uintptr_t value = mem::readOr<uintptr_t>(p.slot);
    if (!value) return GN_ERR_UNAVAILABLE;
    if (p.category == kCatClass) {
        std::memcpy(out, &value, 8); // the object itself
        return GN_OK;
    }
    if (p.category == kCatString || p.category == kCatCString) {
        const uintptr_t chars = mem::readOr<uintptr_t>(value); // the game's text, until the property changes
        std::memcpy(out, &chars, 8);
        return GN_OK;
    }
    return mem::read(value, out, size) ? GN_OK : GN_ERR_UNAVAILABLE;
}

int propertySet(void* entity, const char* name, const void* value, uint32_t size) {
    if (!value) return GN_ERR_ARGUMENT;
    Property p;
    if (const int r = find(entity, name, p); r != GN_OK) return r;
    // Strings would be copied into the bus's arena and objects reference counted: not offered yet.
    if (p.category == kCatClass || p.category == kCatString || p.category == kCatCString) return GN_ERR_ARGUMENT;
    if (size != p.size || !p.size) return GN_ERR_ARGUMENT;
    using SetFn = void (*)(uintptr_t, const void*);
    reinterpret_cast<SetFn>(mem::rva(e::function::PropertyValue_set))(p.slot, value);
    return GN_OK;
}

// ---- watching -------------------------------------------------------------------------------------------
// The game's own listener list on a slot (db/annotations/entity.yaml, PropertyListener_attach / _detach):
// a listener is an object whose vtable +0x10 gets (self, message) for every change, with the tag it was
// attached with. GridNode attaches one small object per watch. Its detach is careless (with one
// listener it clears the slot whoever is there; with several and no match it drops the last one), so
// GridNode looks for its own entry first. Deliveries can be queued while the game defers them, with the
// listener's address: after a detach GridNode lets the realm tick before its memory can go.
namespace {

struct Message { // what PropertyDispatcher_deliver passes (+0x18 a flag, +0x20 zero)
    uint32_t tag;
    uint32_t pad;
    uintptr_t value;
    uintptr_t typeInfo;
};

struct Listener {
    const void* const* vtable;
    uint32_t handle;
};

struct Watch {
    uint32_t handle = 0;
    uintptr_t entity = 0, slot = 0;
    std::string mod, name;
    uint32_t id = 0, category = 0;
    gn_property_fn fn = nullptr;
    void* user = nullptr;
    Listener* listener = nullptr;
    bool live = true;     // calls go to the mod (cleared at once by unwatch / the mod's unload)
    bool attached = true; // in the game's list (cleared on the realm thread by the detach)
    bool server = false;
};

std::recursive_mutex g_wm;                              // everything below
std::unordered_map<uint32_t, Watch> g_watches;   // by handle
std::deque<Listener> g_listenerObjects;          // stable addresses; freed with gridnode.dll only
uint32_t g_nextHandle = 1;
bool g_destroyHooked = false;
constexpr size_t kMaxWatches = 4096;

void stub0(Listener*) {}
void stubCleared(Listener*, uint32_t) {}
void onChanged(Listener* self, const Message* msg);
const void* const kListenerVtable[3] = {reinterpret_cast<const void*>(&stub0), reinterpret_cast<const void*>(&stubCleared),
                                        reinterpret_cast<const void*>(&onChanged)};

void onChanged(Listener* self, const Message* msg) {
    guardHook("property watch", [&] {
        if (!self || !msg) return;
        Watch w;
        {
            std::lock_guard<std::recursive_mutex> lock(g_wm);
            const auto it = g_watches.find(self->handle);
            if (it == g_watches.end() || !it->second.live) return;
            w = it->second;
        }
        // The value as property_get gives it: the object for a reference, the char* for a string.
        uintptr_t eight = 0;
        const void* value = reinterpret_cast<const void*>(msg->value);
        if (w.category == kCatClass) eight = msg->value, value = &eight;
        else if (w.category == kCatString || w.category == kCatCString) eight = msg->value ? mem::readOr<uintptr_t>(msg->value) : 0, value = &eight;
        const gn_type* type = reinterpret_cast<const gn_type*>(registry().byTi(msg->typeInfo));
        try {
            modcall::Scope scope(w.fn);
            w.fn(reinterpret_cast<void*>(w.entity), w.name.c_str(), value, type, w.user);
        } catch (...) {
            GN_LOG("prop: %s's property watch threw; ignored", w.mod.c_str());
        }
    });
}

// Is this listener (with this tag) in the slot's list?
bool listed(uintptr_t slot, const Listener* l, uint32_t tag) {
    const auto target = reinterpret_cast<uintptr_t>(l);
    const uint32_t flags = mem::readOr<uint32_t>(slot + 0x1c);
    const uintptr_t head = mem::readOr<uintptr_t>(slot + 0x10);
    if (!(flags & 4)) return head == target && mem::readOr<uint32_t>(slot + 0x18) == tag;
    const uint32_t n = mem::readOr<uint32_t>(slot + 0x18);
    for (uint32_t i = 0; head && i < n && i < 4096; ++i)
        if (mem::readOr<uintptr_t>(head + i * 16ull) == target && mem::readOr<uint32_t>(head + i * 16ull + 8) == tag) return true;
    return false;
}

// (realm thread) Takes the watch's listener out if it is still there. slotKnownGood: the entity is
// alive (its destructor is about to run), so the recorded slot is still the bus's.
void detach(Watch& w, bool slotKnownGood) {
    if (!w.attached) return;
    w.attached = false;
    uintptr_t slot = w.slot;
    if (!slotKnownGood) {
        // The bus may be gone (its teardown detaches and frees its slots): find the slot again.
        Property p;
        if (find(reinterpret_cast<void*>(w.entity), w.name.c_str(), p) != GN_OK || p.slot != w.slot) return;
        slot = p.slot;
    }
    if (!listed(slot, w.listener, w.id)) return;
    using DetachFn = void (*)(uintptr_t, const void*, uint32_t);
    reinterpret_cast<DetachFn>(mem::rva(e::function::PropertyListener_detach))(slot, w.listener, w.id);
}

// A watched entity is about to be destroyed (its realm thread, before the game's destructor): its
// bus is still there, so its slots are, and the listener comes out now.
void watchedDestroyed(void* entity, void*) {
    std::lock_guard<std::recursive_mutex> lock(g_wm);
    for (auto it = g_watches.begin(); it != g_watches.end();) {
        Watch& w = it->second;
        if (w.entity != reinterpret_cast<uintptr_t>(entity)) {
            ++it;
            continue;
        }
        detach(w, true);
        it = g_watches.erase(it);
    }
}

// Detaches on each watch's realm thread (any thread may call this); `which` picks the watches.
template <class Pick> void detachLater(Pick which) {
    std::lock_guard<std::recursive_mutex> lock(g_wm);
    for (auto& [h, w] : g_watches) {
        if (!which(w)) continue;
        w.live = false;
        const uint32_t handle = h;
        auto task = [handle] {
            std::lock_guard<std::recursive_mutex> l(g_wm);
            const auto it = g_watches.find(handle);
            if (it == g_watches.end()) return;
            detach(it->second, false);
            g_watches.erase(it);
        };
        if (w.server) server().post(task);
        else client().post(task);
    }
}

} // namespace

int propertyWatch(const std::string& mod, void* entity, const char* name, gn_property_fn fn, void* user, uint32_t* handle) {
    if (!fn) return GN_ERR_ARGUMENT;
    if (unloading()) return GN_ERR_UNAVAILABLE;
    Property p;
    if (const int r = find(entity, name, p); r != GN_OK) return r;
    {
        std::lock_guard<std::recursive_mutex> lock(g_wm);
        if (g_watches.size() >= kMaxWatches) return GN_ERR_FULL;
    }
    if (!g_destroyHooked) {
        if (onDestroyed("GridNode", "Entity", &watchedDestroyed, nullptr) != GN_OK) return GN_ERR_UNAVAILABLE;
        g_destroyHooked = true;
    }
    std::lock_guard<std::recursive_mutex> lock(g_wm);
    Watch w;
    w.handle = g_nextHandle++;
    w.entity = reinterpret_cast<uintptr_t>(entity);
    w.slot = p.slot;
    w.mod = mod;
    w.name = name;
    w.id = p.id;
    w.category = p.category;
    w.fn = fn;
    w.user = user;
    w.server = (mem::readOr<uint32_t>(w.entity + 0x18) & 1) != 0;
    g_listenerObjects.push_back(Listener{kListenerVtable, w.handle});
    w.listener = &g_listenerObjects.back();
    using AttachFn = void (*)(uintptr_t, const void*, uint32_t);
    reinterpret_cast<AttachFn>(mem::rva(e::function::PropertyListener_attach))(p.slot, w.listener, p.id);
    if (handle) *handle = w.handle;
    g_watches.emplace(w.handle, w);
    return GN_OK;
}

int propertyUnwatch(uint32_t handle) {
    bool found = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_wm);
        const auto it = g_watches.find(handle);
        found = it != g_watches.end() && it->second.live;
    }
    if (!found) return GN_ERR_NOT_FOUND;
    detachLater([handle](const Watch& w) { return w.handle == handle; });
    return GN_OK;
}

void forgetProperties(const CodeRange& r) {
    detachLater([&](const Watch& w) { return r.has(reinterpret_cast<const void*>(w.fn)); });
}

void propertiesRelease() {
    detachLater([](const Watch&) { return true; });
}

bool propertiesReleased() {
    std::lock_guard<std::recursive_mutex> lock(g_wm);
    return g_watches.empty();
}

namespace {
std::string onEntityRealm(uintptr_t entity, std::function<std::string()> fn) {
    return runOnRealmAndWait((mem::readOr<uint32_t>(entity + 0x18) & 1) != 0, std::move(fn));
}

std::string describeValue(const Property& p, const uint8_t* v) {
    const reflect::Type* t = registry().byTi(mem::readOr<uintptr_t>(p.slot + 8));
    const std::string tn = t ? t->name : "?";
    char b[160];
    if (p.category == kCatString || p.category == kCatCString) {
        uintptr_t chars = 0;
        std::memcpy(&chars, v, 8);
        char text[96] = {};
        if (chars) mem::read(chars, text, sizeof(text) - 1);
        snprintf(b, sizeof(b), "\"%s\"", text);
    } else if (p.category == kCatClass) {
        uintptr_t o = 0;
        std::memcpy(&o, v, 8);
        const reflect::Type* ot = o ? registry().typeOf(mem::ProcessMemory(), o) : nullptr;
        snprintf(b, sizeof(b), "%p (%s)", reinterpret_cast<void*>(o), ot ? ot->name.c_str() : "?");
    } else if (tn == "Boolean") snprintf(b, sizeof(b), "%s", v[0] ? "true" : "false");
    else if (tn == "Float32") { float f; std::memcpy(&f, v, 4); snprintf(b, sizeof(b), "%g", f); }
    else if (tn == "Int32" || tn == "Uint32" || p.size == 4) { int32_t i; std::memcpy(&i, v, 4); snprintf(b, sizeof(b), "%d", i); }
    else {
        std::string hex;
        for (uint32_t i = 0; i < p.size && i < 32; ++i) {
            char h[4];
            snprintf(h, sizeof(h), "%02x", v[i]);
            hex += h;
        }
        snprintf(b, sizeof(b), "%s%s", hex.c_str(), p.size > 32 ? "..." : "");
    }
    return tn + " " + b;
}
} // namespace

// The console's "prop ADDRESS NAME [VALUE]": a property of an entity (an address from the inspector),
// written when a value is given (bool true / false / 1 / 0, numbers for Float32 and the 4-byte ints).
// Without NAME: the entity's data and which of its fields have slots.
namespace {
void logChange(void* entity, const char* name, const void* value, const gn_type* type, void*) {
    const auto* t = reinterpret_cast<const reflect::Type*>(type);
    const std::string tn = t ? t->name : "?";
    char b[96] = "?";
    if (!value) snprintf(b, sizeof(b), "(none)");
    else if (tn == "Boolean") snprintf(b, sizeof(b), "%s", *static_cast<const uint8_t*>(value) ? "true" : "false");
    else if (tn == "Float32") snprintf(b, sizeof(b), "%g", *static_cast<const float*>(value));
    else if (tn == "Int32" || tn == "Uint32") snprintf(b, sizeof(b), "%d", *static_cast<const int32_t*>(value));
    else snprintf(b, sizeof(b), "(a %s)", tn.c_str());
    GN_LOG("prop: %s of %p changed: %s %s", name, entity, tn.c_str(), b);
}
} // namespace

std::string propCommand(const std::string& rest) {
    if (rest.compare(0, 6, "watch ") == 0) {
        char addr[32] = {}, name[128] = {};
        if (sscanf(rest.c_str() + 6, "%31s %127s", addr, name) != 2) return "prop watch ADDRESS NAME: logs each change of the property to gridnode.log";
        const uintptr_t entity = static_cast<uintptr_t>(strtoull(addr, nullptr, 16));
        const std::string nameS = name;
        return onEntityRealm(entity, [entity, nameS]() -> std::string {
            uint32_t h = 0;
            const int r = propertyWatch("GridNode", reinterpret_cast<void*>(entity), nameS.c_str(), &logChange, nullptr, &h);
            return r == GN_OK ? "prop: watching " + nameS + " (watch " + std::to_string(h) + "); changes go to gridnode.log"
                              : "prop: could not watch " + nameS + " (" + std::to_string(r) + ")";
        });
    }
    char addr[32] = {}, name[128] = {}, val[64] = {};
    const int n = sscanf(rest.c_str(), "%31s %127s %63s", addr, name, val);
    if (n < 1) return "prop ADDRESS [NAME [VALUE]]: an entity's connected property (address from the inspector)";
    const uintptr_t entity = static_cast<uintptr_t>(strtoull(addr, nullptr, 16));
    const std::string nameS = name, valS = val;
    return onEntityRealm(entity, [entity, n, nameS, valS]() -> std::string {
        const char* name = nameS.c_str();
        const char* val = valS.c_str();
        std::string out;
        if (n == 1) {
            uintptr_t peer = 0, data = 0;
            const reflect::Type* et = registry().typeOf(mem::ProcessMemory(), entity);
            if (!et || !inspector::alive(entity, et)) { out = "prop: not a live entity"; return out; }
            if (entityData(entity, peer, data) != GN_OK) { out = "prop: " + et->name + " is on no bus list (nothing connects to it: no property slots)"; return out; }
            const reflect::Type* dt = registry().typeOf(mem::ProcessMemory(), data);
            out = "prop: " + et->name + ", data " + (dt ? dt->name : "?");
            char b[48];
            snprintf(b, sizeof(b), " %p; connected:", reinterpret_cast<void*>(data));
            out += b;
            int found = 0;
            for (const reflect::Type* t = dt; t; t = registry().super(t))
                for (const reflect::Field& f : t->fields) {
                    Property p;
                    if (find(reinterpret_cast<void*>(entity), f.name.c_str(), p) != GN_OK) continue;
                    uint8_t v[64] = {};
                    const uintptr_t value = mem::readOr<uintptr_t>(p.slot);
                    if (value && p.size <= sizeof(v)) {
                        if (p.category == kCatClass) std::memcpy(v, &value, 8);
                        else if (p.category == kCatString || p.category == kCatCString) { const uintptr_t c = mem::readOr<uintptr_t>(value); std::memcpy(v, &c, 8); }
                        else mem::read(value, v, p.size);
                    }
                    out += "\n  " + f.name + " = " + (value ? describeValue(p, v) : std::string("(no value)"));
                    ++found;
                }
            if (!found) out += " none";
            return out;
        }
        Property p;
        int r = find(reinterpret_cast<void*>(entity), name, p);
        if (r != GN_OK) { out = "prop: " + std::string(name) + ": " + (r == GN_ERR_NOT_FOUND ? "no such connected property" : "error " + std::to_string(r)); return out; }
        if (n == 3) {
            uint8_t v[8] = {};
            const reflect::Type* t = registry().byTi(mem::readOr<uintptr_t>(p.slot + 8));
            const std::string tn = t ? t->name : "";
            if (tn == "Boolean") v[0] = (!_stricmp(val, "true") || !strcmp(val, "1")) ? 1 : 0;
            else if (tn == "Float32") { const float f = strtof(val, nullptr); std::memcpy(v, &f, 4); }
            else if (p.size == 4) { const int32_t i = static_cast<int32_t>(strtol(val, nullptr, 0)); std::memcpy(v, &i, 4); }
            else { out = "prop: writing a " + tn + " from the console is not supported"; return out; }
            r = propertySet(reinterpret_cast<void*>(entity), name, v, p.size);
            if (r != GN_OK) { out = "prop: write refused (" + std::to_string(r) + ")"; return out; }
            GN_LOG("prop: %s of %p set to %s", name, reinterpret_cast<void*>(entity), val);
        }
        uint8_t v[64] = {};
        if (p.size > sizeof(v) || propertyGet(reinterpret_cast<void*>(entity), name, v, p.size, nullptr) != GN_OK) { out = "prop: " + std::string(name) + " has no value yet"; return out; }
        out = "prop: " + std::string(name) + " = " + describeValue(p, v);
        return out;
    });
}

} // namespace gn::host
