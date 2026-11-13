// The game's data registry (db/annotations/spawn.yaml, DataRegistry_*): every data object of every
// loaded bundle (blueprints, entity data, assets), in one table sorted by (registry slot, class id,
// index), and the game's own load / unload listeners. GridNode reads the table to find loaded data by
// class (API 39 data_of_class; the trainer's jump tuning) and adds one listener, only once a mod asks
// for on_data, so mods hear about data coming and going without a hook.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/mod_call.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;

constexpr size_t kMaxEntries = 4u << 20; // a sanity bound on the table (a city load has ~10^5)

using LockFn = void (*)(uintptr_t);
using ListenerFn = void (*)(void* ctx, const uintptr_t* objects, uint16_t slot, bool loaded);
using AddFn = void (*)(uintptr_t registry, const void* listener);
using RemoveFn = bool (*)(uintptr_t registry, const void* listener);

// What DataRegistry_addListener copies: {ctx, fn}. Ours lives in gridnode.dll, so it must come out
// before the DLL is freed (dataRelease).
struct GameListener {
    void* ctx;
    ListenerFn fn;
};

struct DataListener {
    std::string mod;
    gn_data_fn fn;
    void* user;
};

std::mutex g_m;                         // g_listeners, g_attached
std::vector<DataListener> g_listeners;  // (g_m)
bool g_attached = false;                // our GameListener is in the game's tree (g_m)
GameListener g_game{};                  // what was added (the same 16 bytes remove it)
std::atomic<uint64_t> g_loads{0}, g_unloads{0};

class RegistryLock {
public:
    RegistryLock() { reinterpret_cast<LockFn>(mem::rva(e::function::Lock_enter))(mem::rva(e::global::DataRegistry_Lock)); }
    ~RegistryLock() { reinterpret_cast<LockFn>(mem::rva(e::function::Lock_leave))(mem::rva(e::global::DataRegistry_Lock)); }
    RegistryLock(const RegistryLock&) = delete;
    RegistryLock& operator=(const RegistryLock&) = delete;
};

// The bundle id the game registered the data under (DataRegistry_IdOfSlot holds id - 3; 0x7d2 = free).
uint32_t bundleOfSlot(uint16_t slot) {
    if (slot >= 2001) return 0;
    const uint16_t id = mem::readOr<uint16_t>(mem::rva(e::global::DataRegistry_IdOfSlot) + slot * 2u, 0x7d2);
    return id == 0x7d2 ? 0 : id + 3u;
}

// Called by the game on the thread loading or unloading the bundle, under DataRegistry_Lock.
void onGameData(void* /*ctx*/, const uintptr_t* objects, uint16_t slot, bool loaded) {
    guardHook("data registry listener", [&] {
        (loaded ? g_loads : g_unloads).fetch_add(1);
        const uintptr_t begin = objects ? objects[0] : 0, end = objects ? objects[1] : 0;
        if (!begin || end < begin) return;
        const auto count = static_cast<uint32_t>(std::min<uintptr_t>((end - begin) / 8, kMaxEntries));
        std::vector<DataListener> tell;
        {
            std::lock_guard<std::mutex> lock(g_m);
            tell = g_listeners;
        }
        const uint32_t bundle = bundleOfSlot(slot);
        for (const DataListener& l : tell) {
            try {
                modcall::Scope scope(l.fn);
                l.fn(reinterpret_cast<void* const*>(begin), count, bundle, loaded ? 1 : 0, l.user);
            } catch (...) {
                GN_LOG("data: %s's on_data listener threw; ignored", l.mod.c_str());
            }
        }
    });
}

// Adds our listener to the game's (g_m held).
bool attach() {
    if (g_attached) return true;
    g_game = GameListener{nullptr, &onGameData};
    {
        RegistryLock lock; // the game adds its own without it, at startup; a load may be running now
        reinterpret_cast<AddFn>(mem::rva(e::function::DataRegistry_addListener))(mem::rva(e::global::DataRegistry), &g_game);
    }
    g_attached = true;
    GN_LOG("data: listening to the data registry (bundles' data loaded and unloaded)");
    return true;
}

} // namespace

// Every loaded data object of a class: the table is sorted by registry slot first, so it is one pass
// over all of it. Read without the lock (it is a plain vector the loading thread may grow): every read
// is fault-tolerant, and objects are checked by their class id (TypeInfo + 0x28) as the key says.
size_t dataEach(const reflect::Type* cls, bool subclasses, const std::function<bool(uintptr_t)>& fn) {
    if (!cls || !cls->classId) return 0;
    const uint16_t lo = cls->classId;
    const uint16_t hi = subclasses && cls->lastSubClassId >= cls->classId ? cls->lastSubClassId : cls->classId;
    const uintptr_t table = mem::rva(e::global::DataRegistry_Entries);
    const uintptr_t begin = mem::readOr<uintptr_t>(table), end = mem::readOr<uintptr_t>(table + 8);
    if (!begin || end <= begin || (end - begin) % 16 || (end - begin) / 16 > kMaxEntries) return 0;
    const size_t n = (end - begin) / 16;
    std::vector<uint8_t> copy(n * 16);
    if (!mem::read(begin, copy.data(), copy.size())) return 0;
    size_t found = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t key = 0;
        uintptr_t obj = 0;
        std::memcpy(&key, copy.data() + i * 16, 8);
        std::memcpy(&obj, copy.data() + i * 16 + 8, 8);
        const auto id = static_cast<uint16_t>(key >> 16); // class ids fit 16 bits (TypeInfo + 0x28 is a u16)
        if (id < lo || id > hi || !obj) continue;
        ++found;
        if (!fn(obj)) break;
    }
    return found;
}

uintptr_t dataFirstWithVtable(uint64_t vtableVa) {
    const uintptr_t want = mem::rva(vtableVa);
    const uintptr_t table = mem::rva(e::global::DataRegistry_Entries);
    const uintptr_t begin = mem::readOr<uintptr_t>(table), end = mem::readOr<uintptr_t>(table + 8);
    if (!begin || end <= begin || (end - begin) % 16 || (end - begin) / 16 > kMaxEntries) return 0;
    for (uintptr_t p = begin; p < end; p += 16) {
        const uintptr_t obj = mem::readOr<uintptr_t>(p + 8);
        if (obj && mem::readOr<uintptr_t>(obj) == want) return obj;
    }
    return 0;
}

int dataOfClass(const char* className, int subclasses, void** out, uint32_t max) {
    if (!className || !*className || (max && !out)) return -GN_ERR_ARGUMENT;
    const reflect::Type* cls = registry().find(className);
    if (!cls) return -GN_ERR_NOT_FOUND;
    if (!cls->classId) return -GN_ERR_UNAVAILABLE; // a value type or enum: never in the registry
    uint32_t written = 0;
    const size_t n = dataEach(cls, subclasses != 0, [&](uintptr_t obj) {
        if (written < max) out[written++] = reinterpret_cast<void*>(obj);
        return true;
    });
    return static_cast<int>(std::min<size_t>(n, 0x7fffffff));
}

int onData(const std::string& mod, gn_data_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_m);
    if (unloading()) return GN_ERR_UNAVAILABLE;
    if (!attach()) return GN_ERR_UNAVAILABLE;
    g_listeners.push_back({mod, fn, user});
    return GN_OK;
}

void forgetData(const CodeRange& r) {
    std::lock_guard<std::mutex> lock(g_m);
    g_listeners.erase(std::remove_if(g_listeners.begin(), g_listeners.end(),
                                     [&](const DataListener& l) { return r.has(reinterpret_cast<void*>(l.fn)); }),
                      g_listeners.end());
    // The game's listener stays until GridNode unloads: adding and removing it per mod gains nothing.
}

namespace {
std::atomic<bool> g_logging{false};
void logData(void* const* objects, uint32_t count, uint32_t bundle, int loaded, void*) {
    // A few names, to see what came: the first objects' types.
    std::string names;
    for (uint32_t i = 0; i < count && i < 3; ++i) {
        const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(objects[i]));
        names += (i ? ", " : "") + (t ? t->name : std::string("?"));
    }
    GN_LOG("data: bundle %u %s: %u objects (%s%s)", bundle, loaded ? "loaded" : "unloading", count, names.c_str(), count > 3 ? ", ..." : "");
}
} // namespace

// The console's "data": the table's size; "data CLASS": how many loaded objects of that class (and its
// subclasses) and the first few; "data listen": log every bundle's data coming and going (on_data).
std::string dataCommand(const std::string& rest) {
    const size_t a = rest.find_first_not_of(' ');
    const std::string arg = a == std::string::npos ? std::string() : rest.substr(a, rest.find_last_not_of(' ') - a + 1);
    char b[256];
    if (arg.empty()) {
        const uintptr_t table = mem::rva(e::global::DataRegistry_Entries);
        const uintptr_t begin = mem::readOr<uintptr_t>(table), end = mem::readOr<uintptr_t>(table + 8);
        snprintf(b, sizeof(b), "data: %llu loaded data objects in the registry; %llu loads and %llu unloads heard%s. data CLASS lists a class's; data listen logs bundles' data coming and going",
                 static_cast<unsigned long long>(end > begin ? (end - begin) / 16 : 0), static_cast<unsigned long long>(g_loads.load()),
                 static_cast<unsigned long long>(g_unloads.load()), g_attached ? "" : " (not listening yet)");
        return b;
    }
    if (_stricmp(arg.c_str(), "listen") == 0) {
        if (g_logging.exchange(true)) return "data: already logging to gridnode.log";
        const int r = onData("gridnode", &logData, nullptr);
        if (r != GN_OK) g_logging = false;
        return r == GN_OK ? "data: logging each bundle's data to gridnode.log (load a level, or move to stream districts)" : "data: could not listen (" + std::to_string(r) + ")";
    }
    const reflect::Type* cls = registry().find(arg);
    if (!cls) return "data: no class named " + arg;
    std::string out;
    size_t shown = 0;
    const size_t n = dataEach(cls, true, [&](uintptr_t obj) {
        const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), obj);
        snprintf(b, sizeof(b), "\n  %p %s", reinterpret_cast<void*>(obj), t ? t->name.c_str() : "?");
        out += b;
        return ++shown < 10;
    });
    snprintf(b, sizeof(b), "data: %s%s", cls->name.c_str(), cls->classId ? "" : " is not a class (never in the registry)");
    std::string head = b;
    if (!cls->classId) return head;
    // dataEach stopped counting at the tenth: count them all.
    const size_t all = n < 10 ? n : dataEach(cls, true, [](uintptr_t) { return true; });
    snprintf(b, sizeof(b), ": %llu loaded (with subclasses)%s", static_cast<unsigned long long>(all), all > shown ? "; the first 10:" : "");
    return head + b + out;
}

// GridNode's unload: our listener out of the game's tree. Under the registry's lock, so no load is in
// our code once it returns (the game calls listeners only while holding it).
void dataRelease() {
    std::lock_guard<std::mutex> lock(g_m);
    g_listeners.clear();
    if (!g_attached) return;
    bool removed;
    {
        RegistryLock rl;
        removed = reinterpret_cast<RemoveFn>(mem::rva(e::function::DataRegistry_removeListener))(mem::rva(e::global::DataRegistry), &g_game);
    }
    g_attached = false;
    GN_LOG("data: listener %s (%llu loads, %llu unloads heard)", removed ? "removed" : "was not in the game's list",
           static_cast<unsigned long long>(g_loads.load()), static_cast<unsigned long long>(g_unloads.load()));
}

} // namespace gn::host
