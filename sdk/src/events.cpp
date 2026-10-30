// Events between entities, for the Recorder and for mods (gn_api on_event). An entity receives an
// event through its vtable +0x20 (EntityBusPeer onEvent), called by EntityBusPeer_sendEvent after
// EntityBus_fireEvent followed the bus's event connections. The event is an EntityEvent {vtable,
// +8 id, +0xc kind}; ids are djb2-xor hashes of the event names. GridNode wraps that slot in every
// entity vtable (a pointer swap per vtable, through the hook table, as for creators) the first time
// the Recorder or a mod asks; the wrappers cost two atomic loads while nobody listens.

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/reflect/layout.hpp"
#include "host.hpp"

namespace gn::host::events {
namespace {

namespace e = gn::engine;
using reflect::Type;

using OnEventFn = void (*)(void* peer, void* event, void* a3, void* a4);
constexpr size_t kPool = 4096;
constexpr uint64_t kOnEventSlot = 0x20;

struct Listener {
    std::string mod;
    uint32_t id = 0;              // 0: every event
    const Type* cls = nullptr;    // null: every class
    gn_event_fn fn = nullptr;
    void* user = nullptr;
};
using Listeners = std::vector<Listener>;

std::array<uint64_t, kPool> g_orig{};
std::atomic<Tap> g_tap{nullptr};
std::atomic<const Listeners*> g_listeners{nullptr}; // replaced whole on each add; old lists are kept (a few)
std::mutex g_addM;
std::vector<std::unique_ptr<Listeners>> g_lists; // (g_addM) every list ever published
std::atomic<bool> g_hooked{false};
std::wstring g_namesFile; // recordings\event-names.txt (set at start)

void deliver(void* peer, void* event) {
    if (const Tap tap = g_tap.load(std::memory_order_relaxed)) tap(peer, event);
    const Listeners* ls = g_listeners.load(std::memory_order_acquire);
    if (!ls || !peer || !event) return;
    const uint32_t id = mem::readOr<uint32_t>(reinterpret_cast<uintptr_t>(event) + 8);
    const Type* t = nullptr;
    bool typed = false;
    for (const Listener& l : *ls) {
        if (l.id && l.id != id) continue;
        if (!typed) t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(peer)), typed = true;
        if (l.cls && (!t || !registry().isKindOf(t, l.cls))) continue;
        try {
            l.fn(peer, reinterpret_cast<const gn_type*>(t), id, l.user);
        } catch (...) {
            static std::atomic<int> said{0};
            if (said.fetch_add(1) < 10) GN_LOG("[%s] an event callback threw; ignored", l.mod.c_str());
        }
    }
}

template <size_t I> void detour(void* peer, void* event, void* a3, void* a4) {
    if (g_tap.load(std::memory_order_relaxed) || g_listeners.load(std::memory_order_relaxed)) {
        try {
            deliver(peer, event);
        } catch (...) { // a listener never breaks the game's event
        }
    }
    reinterpret_cast<OnEventFn>(g_orig[I])(peer, event, a3, a4);
}
template <size_t... I> constexpr std::array<OnEventFn, sizeof...(I)> makeDetours(std::index_sequence<I...>) {
    return {&detour<I>...};
}
const std::array<OnEventFn, kPool> g_detours = makeDetours(std::make_index_sequence<kPool>{});

} // namespace

void start(const std::wstring& gridnodeDir) { g_namesFile = gridnodeDir + L"recordings\\event-names.txt"; }

void setTap(Tap tap) { g_tap = tap; }

// Wraps onEvent in every vtable of an EntityBusPeer class (checked: its slot 0 says so, slot +0x20
// is game code). Once.
bool ensureHooked(const char* who) {
    if (g_hooked.exchange(true)) return true;
    const Type* peer = registry().find("EntityBusPeer");
    if (!peer) {
        GN_LOG("events: no EntityBusPeer type; events between entities are not available (asked by %s)", who);
        return false;
    }
    std::vector<uintptr_t> vtables;
    for (const Type& t : registry().data().types) {
        if (t.category != reflect::Category::Class || !registry().isKindOf(&t, peer)) continue;
        for (uint64_t va : t.vtables) {
            const uintptr_t vt = mem::rva(va);
            uintptr_t fake = vt; // an "object" whose vtable is vt: its type must be an EntityBusPeer
            const Type* vtType = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(&fake));
            if (!vtType || !registry().isKindOf(vtType, peer)) continue;
            if (!mem::inImage(mem::readOr<uintptr_t>(vt + kOnEventSlot))) continue;
            vtables.push_back(vt);
        }
    }
    std::sort(vtables.begin(), vtables.end());
    vtables.erase(std::unique(vtables.begin(), vtables.end()), vtables.end());
    size_t used = 0, failed = 0;
    for (uintptr_t vt : vtables) {
        if (used == kPool) break;
        std::string why;
        const auto r = hooks().install("events", vt + kOnEventSlot, 0, reinterpret_cast<uint64_t>(g_detours[used]), &g_orig[used], &why);
        if (r == hook::Result::Ok) ++used;
        else if (failed++ < 5) GN_LOG("events: onEvent of vtable 0x%llx: %s", static_cast<unsigned long long>(mem::va(vt)), why.c_str());
    }
    GN_LOG("events: onEvent wrapped in %llu entity vtables (%llu failed, %llu found; first asked by %s)",
           static_cast<unsigned long long>(used), static_cast<unsigned long long>(failed), static_cast<unsigned long long>(vtables.size()), who);
    return used > 0;
}

uint32_t hash(const std::string& s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = (h * 33) ^ c;
    return h;
}

// Event names from their hashes: every reflected type, field and enum value name, common event
// words, recordings\event-names.txt (one name per line, "#" comments, anything after a tab ignored)
// if present (tools/events/build_names.py makes one from an EBX dump, the game's strings and earlier
// recordings), and the events and properties named in db/annotations (engine.hpp), which win.
const std::unordered_map<uint32_t, std::string>& names() {
    static const std::unordered_map<uint32_t, std::string> table = [] {
        std::unordered_map<uint32_t, std::string> m;
        auto add = [&](const std::string& n) {
            if (!n.empty()) m.emplace(hash(n), n);
        };
        for (const Type& t : registry().data().types) {
            add(t.name);
            for (const reflect::Field& f : t.fields) add(f.name);
            for (const reflect::EnumValue& v : t.values) add(v.name);
        }
        static const char* const kCommon[] = {
            "In", "Out", "Start", "Stop", "Enable", "Disable", "Enabled", "Disabled", "Reset", "Trigger", "Triggered", "OnTrigger",
            "OnTriggered", "OnStart", "OnStop", "OnEnter", "OnExit", "OnEnable", "OnDisable", "OnFinished", "OnCompleted", "Finished",
            "Completed", "Activate", "Deactivate", "Activated", "Deactivated", "Show", "Hide", "Open", "Close", "Opened", "Closed",
            "Spawn", "Unspawn", "OnSpawn", "OnUnspawn", "OnSpawned", "OnUnspawned", "OnKilled", "OnDestroyed", "OnPlay", "Play",
            "Pause", "Resume", "OnTrue", "OnFalse", "True", "False", "Set", "Clear", "Toggle", "Next", "Previous", "Begin", "End",
            "OnBegin", "OnEnd", "Load", "Unload", "OnLoaded", "OnUnloaded", "Loaded", "Unloaded", "Success", "Failure", "OnSuccess",
            "OnFailure", "Cancel", "OnCancel", "Confirm", "OnConfirm", "Select", "OnSelect", "Pickup", "OnPickup", "Collected",
            "OnCollected", "Teleport", "OnTeleport", "Respawn", "OnRespawn", "Checkpoint", "OnCheckpoint", "Save", "OnSave"};
        for (const char* n : kCommon) add(n);
        FILE* f = g_namesFile.empty() ? nullptr : _wfopen(g_namesFile.c_str(), L"rb");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                std::string n = line;
                if (n.empty() || n[0] == '#') continue;                                      // a comment
                if (const size_t tab = n.find('\t'); tab != std::string::npos) n.resize(tab); // "name<TAB># note"
                while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' ')) n.pop_back();
                // Generated names are checked against their hashes; a name the file gives is taken
                // over any built-in one with the same hash.
                if (!n.empty()) m[hash(n)] = n;
            }
            fclose(f);
        }
        // Named by our research (db/annotations, checked against their hashes): these win.
        for (const e::hash::Name& n : e::hash::names)
            if (n.name) m[n.hash] = n.name;
        return m;
    }();
    return table;
}

std::string name(uint32_t id) {
    const auto& m = names();
    const auto it = m.find(id);
    if (it != m.end()) return it->second;
    char b[16];
    snprintf(b, sizeof(b), "%08x", id);
    return b;
}

} // namespace gn::host::events

namespace gn::host {

int onEvent(const std::string& mod, const char* event, const char* className, gn_event_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    events::Listener l;
    l.mod = mod, l.fn = fn, l.user = user;
    if (event && *event) {
        // A name, or its hash as hex ("0x2734a476") for events nobody has named yet.
        char* end = nullptr;
        const unsigned long v = (event[0] == '0' && (event[1] == 'x' || event[1] == 'X')) ? strtoul(event + 2, &end, 16) : 0;
        l.id = end && *end == 0 && end != event + 2 ? static_cast<uint32_t>(v) : events::hash(event);
    }
    if (className && *className) {
        l.cls = registry().find(className);
        if (!l.cls) return GN_ERR_NOT_FOUND;
    }
    if (!events::ensureHooked(mod.c_str())) return GN_ERR_UNAVAILABLE;
    std::lock_guard<std::mutex> lock(events::g_addM);
    const events::Listeners* old = events::g_listeners.load();
    auto next = std::make_unique<events::Listeners>(old ? *old : events::Listeners{});
    next->push_back(l);
    events::g_listeners.store(next.get(), std::memory_order_release);
    events::g_lists.push_back(std::move(next));
    return GN_OK;
}

int eventName(uint32_t id, char* out, size_t size) {
    if (!out || !size) return GN_ERR_ARGUMENT;
    const auto& m = events::names();
    const auto it = m.find(id);
    snprintf(out, size, "%s", it != m.end() ? it->second.c_str() : "");
    return it != m.end() ? GN_OK : GN_ERR_NOT_FOUND;
}

} // namespace gn::host
