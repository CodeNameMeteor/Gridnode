// Recorder (M8): what the game does around an event, for research. Records every engine message
// (with its reflected fields, and the type of anything its unreflected slots point to), every
// entity creation (class, data, the blueprint of its bus, position), level changes and markers the
// user drops, each with the game's call stack at that moment. Pause, then export as JSON lines
// (recordings\<time>-<name>.jsonl next to gridnode.dll) to read, or to send for Ghidra work.
//
// Nothing in the game is changed: messages come from GridNode's own listeners (messages.cpp),
// creations from the creator slots GridNode already watches (entities.cpp). Stacks are taken with
// RtlCaptureStackBackTrace on the game's thread; frames in the game are written as image VAs
// (0x14...), which is what Ghidra shows.

#include <windows.h>

#include <algorithm>
#include <array>
#include <utility>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/version.hpp"
#include "host.hpp"
#include "imgui.h"

namespace gn::host::recorder {
namespace {

namespace e = gn::engine;
using reflect::Type;

constexpr uint64_t kImageBase = 0x140000000ull;
constexpr size_t kMaxEvents = 250000;
constexpr int kStackFrames = 16;

enum Kind : uint8_t { kMessage, kCreated, kMarker, kLevel, kEvent, kAnim };

struct Ref {           // a pointer found in a message, with what it pointed to then
    uint16_t off;
    uintptr_t ptr;
    const Type* type;
};

struct Event {
    uint64_t seq = 0;
    double t = 0;                  // seconds since the recording started
    Kind kind = kMessage;
    uint32_t thread = 0;
    // message
    int manager = 0;
    uint32_t category = 0, typeId = 0, realm = 0;
    std::vector<uint8_t> bytes;
    std::vector<Ref> refs;
    // created
    uintptr_t ptr = 0;
    const Type* type = nullptr;
    uintptr_t data = 0;
    const Type* dataType = nullptr;
    std::string blueprint;         // the bus's blueprint (by name), if it has one
    float pos[3] = {};
    bool havePos = false;
    // event (ptr / type: the entity it was delivered to)
    uint32_t eventId = 0;
    // anim: a write to an animation (ANT) game state; ptr / type / pos: the animated entity, when known
    uint8_t valueKind = 0;         // AntValue
    float value[16] = {};          // bool / int (as numbers) / float / vector / transform
    uint32_t propHash = 0;         // the writing entity's property for it (data writes)
    uint8_t guid[16] = {};         // the game state's ANT asset (data writes)
    bool haveGuid = false;
    uintptr_t antWriter = 0, antState = 0, writerEnt = 0;
    const Type* writerType = nullptr; // the WriteAntGameState entity, or none: written by game code
    const Type* dataClass = nullptr;  // Write*GameState
    // marker / level
    std::string text;
    std::vector<uint64_t> stack;   // image VAs of game frames, innermost first
};

std::mutex g_m;
std::deque<Event> g_events;        // (g_m)
uint64_t g_seq = 0;                // (g_m)
uint64_t g_dropped = 0;            // (g_m) the oldest, beyond kMaxEvents
std::atomic<bool> g_recording{false};
std::atomic<bool> g_wantMessages{true}, g_wantCreated{true}, g_wantStacks{true}, g_wantEvents{true};
std::atomic<bool> g_wantAnim{true}, g_wantAnimFloats{false};
LARGE_INTEGER g_freq{}, g_t0{};
std::wstring g_dir;
std::string g_lastExport;          // render thread
std::unordered_map<uint32_t, bool> g_ignored; // message type ids not recorded (g_m)
std::unordered_map<const Type*, bool> g_ignoredTargets; // entity classes whose events are not recorded (g_m)
std::string g_level;               // (g_m)

double now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return g_freq.QuadPart ? static_cast<double>(c.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart) : 0.0;
}

// The game's frames of the current call stack (GridNode's own frames and system DLLs left out).
void captureStack(std::vector<uint64_t>& out) {
    if (!g_wantStacks.load(std::memory_order_relaxed)) return;
    void* frames[kStackFrames + 4];
    const USHORT n = RtlCaptureStackBackTrace(2, kStackFrames + 4, frames, nullptr);
    for (USHORT i = 0; i < n && out.size() < kStackFrames; ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        if (mem::inImage(a)) out.push_back(a - mem::moduleBase() + kImageBase);
    }
}

void push(Event&& ev) {
    std::lock_guard<std::mutex> lock(g_m);
    ev.seq = ++g_seq;
    g_events.push_back(std::move(ev));
    while (g_events.size() > kMaxEvents) {
        g_events.pop_front();
        ++g_dropped;
    }
}

// A CString field or an asset's Name: a char pointer.
std::string readCString(uintptr_t p) {
    if (!p) return {};
    char b[257] = {};
    for (size_t i = 0; i < 256; i += 32) {
        if (!mem::read(p + i, b + i, 32)) break;
        if (memchr(b + i, 0, 32)) break;
    }
    b[256] = 0;
    std::string s = b;
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) return {};
    return s;
}

// The Name of an asset (a Blueprint, ...), when its type has one.
std::string assetName(uintptr_t obj) {
    const Type* t = obj ? registry().typeOf(mem::ProcessMemory(), obj) : nullptr;
    reflect::FieldInfo fi;
    if (!t || !registry().findField(t, "Name", fi) || !fi.type || fi.type->name != "CString") return {};
    return readCString(mem::readOr<uintptr_t>(obj + fi.field->offset));
}

// The blueprint behind an entity bus: the bus's reference data (+0x38) points to it.
std::string busBlueprint(uintptr_t bus) {
    const uintptr_t ref = bus ? mem::readOr<uintptr_t>(bus + 0x38) : 0;
    const Type* rt = ref ? registry().typeOf(mem::ProcessMemory(), ref) : nullptr;
    if (!rt) return {};
    reflect::FieldInfo fi;
    if (registry().findField(rt, "Blueprint", fi)) {
        const std::string n = assetName(mem::readOr<uintptr_t>(ref + fi.field->offset));
        if (!n.empty()) return n;
    }
    return assetName(ref);
}

// ---- sources ----------------------------------------------------------------------------------
void onMessage(int manager, const uint8_t* msg, uint32_t category, uint32_t typeId, uint32_t realm) {
    if (!g_recording.load(std::memory_order_relaxed) || !g_wantMessages.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_ignored.count(typeId)) return;
    }
    Event ev;
    ev.t = now();
    ev.kind = kMessage;
    ev.thread = GetCurrentThreadId();
    ev.manager = manager, ev.category = category, ev.typeId = typeId, ev.realm = realm;
    const Type* t = messageTypeById(typeId);
    const size_t n = std::min<size_t>(t && t->size >= 0x30 ? t->size : 0x80, 0x400);
    ev.bytes.resize(n);
    if (!mem::read(reinterpret_cast<uintptr_t>(msg), ev.bytes.data(), n)) {
        ev.bytes.resize(0x30);
        memcpy(ev.bytes.data(), msg, 0x30);
    }
    // What its pointers point to now (the objects may be gone by the time anyone reads the export).
    for (size_t off = 0x30; off + 8 <= ev.bytes.size(); off += 8) {
        uint64_t q;
        memcpy(&q, &ev.bytes[off], 8);
        if (q < 0x10000 || (q & 7)) continue;
        if (const Type* pt = registry().typeOf(mem::ProcessMemory(), static_cast<uintptr_t>(q)))
            ev.refs.push_back({static_cast<uint16_t>(off), static_cast<uintptr_t>(q), pt});
    }
    captureStack(ev.stack);
    push(std::move(ev));
}

void onCreated(void* entity, void* ci, void*) {
    if (!g_recording.load(std::memory_order_relaxed) || !g_wantCreated.load(std::memory_order_relaxed) || !entity) return;
    Event ev;
    ev.t = now();
    ev.kind = kCreated;
    ev.thread = GetCurrentThreadId();
    ev.ptr = reinterpret_cast<uintptr_t>(entity);
    ev.type = registry().typeOf(mem::ProcessMemory(), ev.ptr);
    if (ci) {
        namespace c = e::st::EntityCreationInfo::off;
        const uintptr_t info = reinterpret_cast<uintptr_t>(ci);
        ev.havePos = mem::read(info + c::transform + 0x30, ev.pos, sizeof(ev.pos)) && std::isfinite(ev.pos[0]);
        ev.data = mem::readOr<uintptr_t>(info + c::data);
        ev.dataType = ev.data ? registry().typeOf(mem::ProcessMemory(), ev.data) : nullptr;
        ev.blueprint = busBlueprint(mem::readOr<uintptr_t>(info + c::bus));
    }
    captureStack(ev.stack);
    push(std::move(ev));
}

// ---- events between entities ------------------------------------------------------------------
// An entity receives an event through its vtable +0x20 (EntityBusPeer onEvent), called by
// EntityBusPeer_sendEvent after EntityBus_fireEvent followed the bus's event connections. The event
// is an EntityEvent {vtable, +8 id, +0xc kind}; ids are djb2-xor hashes of the event names. GridNode
// wraps that slot in every entity vtable (a pointer swap per vtable, through the hook table, as for
// creators), the first time events are recorded; the wrappers do nothing while not recording.
using OnEventFn = void (*)(void* peer, void* event, void* a3, void* a4);
constexpr size_t kEventPool = 4096;
constexpr uint64_t kOnEventSlot = 0x20;
std::array<uint64_t, kEventPool> g_eventOrig{};

void onEvent(void* peer, void* event);

template <size_t I> void eventDetour(void* peer, void* event, void* a3, void* a4) {
    if (g_recording.load(std::memory_order_relaxed) && g_wantEvents.load(std::memory_order_relaxed)) {
        try {
            onEvent(peer, event);
        } catch (...) { // recording never breaks the game's event
        }
    }
    reinterpret_cast<OnEventFn>(g_eventOrig[I])(peer, event, a3, a4);
}
template <size_t... I> constexpr std::array<OnEventFn, sizeof...(I)> makeEventDetours(std::index_sequence<I...>) {
    return {&eventDetour<I>...};
}
const std::array<OnEventFn, kEventPool> g_eventDetours = makeEventDetours(std::make_index_sequence<kEventPool>{});
std::atomic<bool> g_eventsHooked{false};

// Wraps onEvent in every vtable of an EntityBusPeer class (checked: its slot 0 says so, slot +0x20
// is game code). Once.
void hookEvents() {
    if (g_eventsHooked.exchange(true)) return;
    const Type* peer = registry().find("EntityBusPeer");
    if (!peer) {
        GN_LOG("recorder: no EntityBusPeer type; events between entities are not recorded");
        return;
    }
    std::vector<uint64_t> vtables;
    for (const Type& t : registry().data().types) {
        if (t.category != reflect::Category::Class || !registry().isKindOf(&t, peer)) continue;
        for (uint64_t va : t.vtables) {
            const uintptr_t vt = static_cast<uintptr_t>(va - kImageBase) + mem::moduleBase();
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
        if (used == kEventPool) break;
        std::string why;
        const auto r = hooks().install("recorder", vt + kOnEventSlot, 0, reinterpret_cast<uint64_t>(g_eventDetours[used]), &g_eventOrig[used], &why);
        if (r == hook::Result::Ok) ++used;
        else if (failed++ < 5) GN_LOG("recorder: onEvent of vtable 0x%llx: %s", static_cast<unsigned long long>(mem::va(vt)), why.c_str());
    }
    GN_LOG("recorder: events between entities: onEvent wrapped in %llu entity vtables (%llu failed, %llu found)",
           static_cast<unsigned long long>(used), static_cast<unsigned long long>(failed), static_cast<unsigned long long>(vtables.size()));
}

uint32_t djb2(const std::string& s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = (h * 33) ^ c;
    return h;
}

// Event names from their hashes: every reflected type, field and enum value name, common event
// words, and recordings\event-names.txt (one name per line, "#" comments, anything after a tab
// ignored) if present: tools/events/build_names.py makes one from an EBX dump, the game's strings
// and earlier recordings.
const std::unordered_map<uint32_t, std::string>& eventNames() {
    static const std::unordered_map<uint32_t, std::string> names = [] {
        std::unordered_map<uint32_t, std::string> m;
        auto add = [&](const std::string& n) {
            if (!n.empty()) m.emplace(djb2(n), n);
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
        FILE* f = _wfopen((g_dir + L"event-names.txt").c_str(), L"rb");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                std::string n = line;
                if (n.empty() || n[0] == '#') continue;              // a comment
                if (const size_t tab = n.find('\t'); tab != std::string::npos) n.resize(tab); // "name<TAB># note"
                while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' ')) n.pop_back();
                // Generated names are checked against their hashes; a name the file gives is taken
                // over any built-in one with the same hash.
                if (!n.empty()) m[djb2(n)] = n;
            }
            fclose(f);
        }
        return m;
    }();
    return names;
}

std::string eventName(uint32_t id) {
    const auto& m = eventNames();
    const auto it = m.find(id);
    if (it != m.end()) return it->second;
    char b[16];
    snprintf(b, sizeof(b), "%08x", id);
    return b;
}

void onEvent(void* peer, void* event) {
    if (!peer || !event) return;
    const Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uintptr_t>(peer));
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (t && g_ignoredTargets.count(t)) return;
    }
    Event ev;
    ev.t = now();
    ev.kind = kEvent;
    ev.thread = GetCurrentThreadId();
    ev.ptr = reinterpret_cast<uintptr_t>(peer);
    ev.type = t;
    ev.eventId = mem::readOr<uint32_t>(reinterpret_cast<uintptr_t>(event) + 8);
    captureStack(ev.stack);
    push(std::move(ev));
}

// ---- animation (ANT) game state writes ------------------------------------------------------------
// Doors, props and characters are animated by ANT, which reads per-object "game state" values
// (bools, ints, enums, floats, vectors). Logic writes them through a WriteAntGameStateEntity: its
// data lists Write<Type>GameState objects (WriteBoolGameState, ...), each naming an ANT asset by
// AntRef (+0x18) and a property of the entity (PropertyHash, +0x10). Every frame it writes, the
// entity's updater (0x1433b09e0) calls 0x1431f7d10, which calls each data object's vtable +0x18
//   (data, props = entity + 0x30, &writerIndex, &valueIndex, state)
// which reads the value from the entity's typed property slots (props[6] + valueIndex * 0x10,
// TypedValue_getValuePtr) and hands it to the ANT asset's game-state interface (writer =
// props[0][writerIndex], vtable +8: (writer, state, value); bool / int as integers, float in a
// float register, vectors and quaternions by pointer). state is the animated object's game state:
// *(*(animatable + 0x158) + 0x10) + 0x40.
//
// GridNode wraps +0x18 of each Write<Type>GameState vtable (what was written, by which entity, for
// which property), and +8 of each ANT writer interface it meets (writes made by game code rather
// than logic, which have no entity). Only changes are recorded: a value written again unchanged
// (most are written every frame) is not.
// The data classes' kinds; a write records bool / int / enum / float, vec3 / quat (4 floats) or
// matrix (the LinearTransform a quaternion or vector-and-quaternion write is made from).
enum AntValue : uint8_t { kAntBool, kAntInt, kAntEnum, kAntFloat, kAntVec3, kAntQuat, kAntTransform, kAntVec3Quat, kAntMatrix };
const char* antValueName(uint8_t k) {
    static const char* n[] = {"bool", "int", "enum", "float", "vec3", "quat", "vec3", "matrix", "matrix"};
    return k < 9 ? n[k] : "?";
}

struct AntCtx {           // set by the data wrapper around the write it makes
    bool active = false;
};
thread_local AntCtx t_antCtx;

std::mutex g_antM;
struct AntLast {
    float v[16];
    double t;
};
std::unordered_map<uint64_t, AntLast> g_antLast;                // (g_antM) (state ^ writer) -> last value
std::unordered_map<uintptr_t, std::pair<uintptr_t, const Type*>> g_antOwner; // (g_antM) state -> animated entity
double g_antOwnersAt = -10.0;                                   // (g_antM)

uintptr_t antStateOf(uintptr_t animatable) {
    const uintptr_t holder = mem::readOr<uintptr_t>(animatable + 0x158);
    const uintptr_t inner = holder ? mem::readOr<uintptr_t>(holder + 0x10) : 0;
    return inner ? inner + 0x40 : 0;
}

// The entity a game state belongs to: known ones first, else every tracked entity is checked
// (at most once a second).
std::pair<uintptr_t, const Type*> antOwner(uintptr_t state) {
    {
        std::lock_guard<std::mutex> lock(g_antM);
        const auto it = g_antOwner.find(state);
        if (it != g_antOwner.end()) return it->second;
        const double t = now();
        if (t - g_antOwnersAt < 1.0) return {0, nullptr};
        g_antOwnersAt = t;
    }
    std::unordered_map<uintptr_t, std::pair<uintptr_t, const Type*>> found;
    for (const auto& [p, ty] : inspector::tracked())
        if (const uintptr_t st = antStateOf(p)) found.emplace(st, std::make_pair(p, ty));
    std::lock_guard<std::mutex> lock(g_antM);
    for (const auto& kv : found) g_antOwner[kv.first] = kv.second;
    const auto it = g_antOwner.find(state);
    return it != g_antOwner.end() ? it->second : std::make_pair(uintptr_t{0}, static_cast<const Type*>(nullptr));
}

// Whether this write changes the value (and is worth recording); remembers it.
bool antChanged(uintptr_t state, uintptr_t writer, uint8_t kind, const float* v, int n) {
    const bool scalarish = kind == kAntBool || kind == kAntInt || kind == kAntEnum;
    if (!scalarish && !g_wantAnimFloats.load(std::memory_order_relaxed)) return false;
    const uint64_t key = static_cast<uint64_t>(state) * 1000003ull ^ static_cast<uint64_t>(writer);
    const double t = now();
    std::lock_guard<std::mutex> lock(g_antM);
    auto it = g_antLast.find(key);
    if (it == g_antLast.end()) {
        AntLast l{};
        memcpy(l.v, v, sizeof(float) * n);
        l.t = t;
        g_antLast.emplace(key, l);
        return true;
    }
    bool diff = false;
    for (int i = 0; i < n; ++i) diff = diff || (scalarish ? it->second.v[i] != v[i] : std::fabs(it->second.v[i] - v[i]) > 1e-3f);
    if (!diff) return false;
    if (!scalarish && t - it->second.t < 0.1) return false; // a moving value: at most 10 a second
    memcpy(it->second.v, v, sizeof(float) * n);
    it->second.t = t;
    return true;
}

void recordAnim(Event&& ev, int n) {
    if (!antChanged(ev.antState, ev.antWriter, ev.valueKind, ev.value, n)) return;
    ev.t = now();
    ev.kind = kAnim;
    ev.thread = GetCurrentThreadId();
    const auto [owner, ownerType] = antOwner(ev.antState);
    ev.ptr = owner, ev.type = ownerType;
    if (owner) {
        gn_transform tr{};
        if (entityTransform(reinterpret_cast<void*>(owner), tr)) memcpy(ev.pos, tr.trans, sizeof(ev.pos)), ev.havePos = true;
    }
    captureStack(ev.stack);
    push(std::move(ev));
}

int antCount(uint8_t kind) { return kind <= kAntFloat ? 1 : kind == kAntVec3Quat || kind == kAntMatrix ? 16 : 4; }

// ANT writer interfaces (vtable +8), wrapped once each, by the kind of value they take.
constexpr size_t kAntPool = 16;
constexpr uint64_t kAntWriteSlot = 0x8;
std::array<uint64_t, kAntPool> g_antIntOrig{}, g_antFloatOrig{}, g_antPtrOrig{};
std::array<uint8_t, kAntPool> g_antIntKind{}, g_antPtrKind{};

void antCodeWrite(uintptr_t writer, uintptr_t state, uint8_t kind, const float* v) try {
    Event ev;
    ev.valueKind = kind;
    ev.antWriter = writer, ev.antState = state;
    memcpy(ev.value, v, sizeof(float) * antCount(kind));
    recordAnim(std::move(ev), antCount(kind));
} catch (...) {
}
bool antRecording() { return g_recording.load(std::memory_order_relaxed) && g_wantAnim.load(std::memory_order_relaxed); }

template <size_t I> void antIntDetour(void* w, void* state, uint64_t v) {
    if (antRecording() && !t_antCtx.active) {
        const float f[1] = {static_cast<float>(g_antIntKind[I] == kAntBool ? static_cast<uint8_t>(v) : static_cast<int32_t>(v))};
        antCodeWrite(reinterpret_cast<uintptr_t>(w), reinterpret_cast<uintptr_t>(state), g_antIntKind[I], f);
    }
    reinterpret_cast<void (*)(void*, void*, uint64_t)>(g_antIntOrig[I])(w, state, v);
}
template <size_t I> void antFloatDetour(void* w, void* state, float v) {
    if (antRecording() && !t_antCtx.active) antCodeWrite(reinterpret_cast<uintptr_t>(w), reinterpret_cast<uintptr_t>(state), kAntFloat, &v);
    reinterpret_cast<void (*)(void*, void*, float)>(g_antFloatOrig[I])(w, state, v);
}
template <size_t I> void antPtrDetour(void* w, void* state, const float* v) {
    if (antRecording() && !t_antCtx.active && v) {
        float f[4] = {};
        mem::read(reinterpret_cast<uintptr_t>(v), f, sizeof(f));
        antCodeWrite(reinterpret_cast<uintptr_t>(w), reinterpret_cast<uintptr_t>(state), g_antPtrKind[I], f);
    }
    reinterpret_cast<void (*)(void*, void*, const float*)>(g_antPtrOrig[I])(w, state, v);
}
template <size_t... I> constexpr auto makeAntInt(std::index_sequence<I...>) { return std::array<void (*)(void*, void*, uint64_t), sizeof...(I)>{&antIntDetour<I>...}; }
template <size_t... I> constexpr auto makeAntFloat(std::index_sequence<I...>) { return std::array<void (*)(void*, void*, float), sizeof...(I)>{&antFloatDetour<I>...}; }
template <size_t... I> constexpr auto makeAntPtr(std::index_sequence<I...>) { return std::array<void (*)(void*, void*, const float*), sizeof...(I)>{&antPtrDetour<I>...}; }
const auto g_antIntDetours = makeAntInt(std::make_index_sequence<kAntPool>{});
const auto g_antFloatDetours = makeAntFloat(std::make_index_sequence<kAntPool>{});
const auto g_antPtrDetours = makeAntPtr(std::make_index_sequence<kAntPool>{});
std::mutex g_antHookM;
std::unordered_map<uintptr_t, bool> g_antWriterHooked; // (g_antHookM) writer vtables
size_t g_antIntUsed = 0, g_antFloatUsed = 0, g_antPtrUsed = 0; // (g_antHookM)

// Wraps the writer's interface (+8) the first time its vtable is met.
void hookAntWriter(uintptr_t writer, uint8_t kind) {
    const uintptr_t vt = writer ? mem::readOr<uintptr_t>(writer) : 0;
    if (!mem::inImage(vt) || !mem::inImage(mem::readOr<uintptr_t>(vt + kAntWriteSlot))) return;
    std::lock_guard<std::mutex> lock(g_antHookM);
    if (g_antWriterHooked.count(vt)) return;
    g_antWriterHooked[vt] = true;
    std::string why;
    hook::Result r = hook::Result::Argument;
    if (kind == kAntFloat) {
        if (g_antFloatUsed == kAntPool) return;
        r = hooks().install("recorder", vt + kAntWriteSlot, 0, reinterpret_cast<uint64_t>(g_antFloatDetours[g_antFloatUsed]),
                            &g_antFloatOrig[g_antFloatUsed], &why);
        if (r == hook::Result::Ok) ++g_antFloatUsed;
    } else if (kind <= kAntEnum) {
        if (g_antIntUsed == kAntPool) return;
        g_antIntKind[g_antIntUsed] = kind;
        r = hooks().install("recorder", vt + kAntWriteSlot, 0, reinterpret_cast<uint64_t>(g_antIntDetours[g_antIntUsed]),
                            &g_antIntOrig[g_antIntUsed], &why);
        if (r == hook::Result::Ok) ++g_antIntUsed;
    } else {
        if (g_antPtrUsed == kAntPool) return;
        g_antPtrKind[g_antPtrUsed] = static_cast<uint8_t>(kind == kAntVec3Quat || kind == kAntTransform ? static_cast<uint8_t>(kAntVec3) : kind);
        r = hooks().install("recorder", vt + kAntWriteSlot, 0, reinterpret_cast<uint64_t>(g_antPtrDetours[g_antPtrUsed]),
                            &g_antPtrOrig[g_antPtrUsed], &why);
        if (r == hook::Result::Ok) ++g_antPtrUsed;
    }
    GN_LOG("recorder: ANT writer vtable 0x%llx (%s values) %s%s", static_cast<unsigned long long>(mem::va(vt)), antValueName(kind),
           r == hook::Result::Ok ? "wrapped" : "not wrapped: ", r == hook::Result::Ok ? "" : why.c_str());
}

// The data objects' write (+0x18), one wrapper per Write<Type>GameState class.
constexpr uint64_t kAntDataSlot = 0x18;
using AntDataFn = void (*)(void* data, void* props, uint32_t* wIdx, uint32_t* vIdx, void* state);
struct AntDataClass {
    const char* name;
    uint8_t kind;
    const Type* type = nullptr;
    uint64_t orig = 0;
};
AntDataClass g_antData[] = {
    {"WriteBoolGameState", kAntBool},           {"WriteIntGameState", kAntInt},
    {"WriteEnumerationGameState", kAntEnum},    {"WriteFloatGameState", kAntFloat},
    {"WriteVector3GameState", kAntVec3},        {"WriteQuaternionGameState", kAntQuat},
    {"WriteVector3TransformGameState", kAntTransform}, {"WriteVector3AndQuaternionGameState", kAntVec3Quat},
};
constexpr size_t kAntDataCount = sizeof(g_antData) / sizeof(g_antData[0]);

// The typed property slot's value (TypedValue_getValuePtr, 0x142a424b0).
uintptr_t typedValuePtr(uintptr_t slot) {
    const uintptr_t first = mem::readOr<uintptr_t>(slot);
    if (first && (mem::readOr<uint8_t>(first + 0x1c) & 0x40)) return mem::readOr<uintptr_t>(first);
    return mem::readOr<uintptr_t>(slot + 8);
}

void antDataWrite(size_t I, void* data, void* propsP, uint32_t* wIdx, uint32_t* vIdx, void* state) {
    const AntDataClass& dc = g_antData[I];
    const uintptr_t props = reinterpret_cast<uintptr_t>(propsP);
    const uint32_t wi = wIdx ? *wIdx : 0, vi = vIdx ? *vIdx : 0;
    const uintptr_t writers = mem::readOr<uintptr_t>(props);
    const uintptr_t writer = writers ? mem::readOr<uintptr_t>(writers + wi * 8ull) : 0;
    hookAntWriter(writer, dc.kind);
    if (dc.kind == kAntVec3Quat && writers) hookAntWriter(mem::readOr<uintptr_t>(writers + (wi + 1) * 8ull), kAntQuat);
    const uintptr_t slots = mem::readOr<uintptr_t>(props + 0x30);
    const uintptr_t vp = slots ? typedValuePtr(slots + vi * 0x10ull) : 0;
    Event ev;
    ev.valueKind = dc.kind;
    if (vp) {
        switch (dc.kind) {
        case kAntBool: ev.value[0] = mem::readOr<uint8_t>(vp) ? 1.f : 0.f; break;
        case kAntInt:
        case kAntEnum: ev.value[0] = static_cast<float>(mem::readOr<int32_t>(vp)); break;
        case kAntFloat: ev.value[0] = mem::readOr<float>(vp); break;
        case kAntVec3: mem::read(vp, ev.value, sizeof(float) * 4); break;
        default: mem::read(vp, ev.value, sizeof(float) * 16); break; // a LinearTransform
        }
    }
    if (dc.kind == kAntTransform) memmove(ev.value, ev.value + 12, sizeof(float) * 4), ev.valueKind = kAntVec3; // it writes the translation
    if (dc.kind == kAntQuat || dc.kind == kAntVec3Quat) ev.valueKind = kAntMatrix;
    ev.antWriter = writer, ev.antState = reinterpret_cast<uintptr_t>(state);
    ev.propHash = mem::readOr<uint32_t>(reinterpret_cast<uintptr_t>(data) + 0x10);
    ev.haveGuid = mem::read(reinterpret_cast<uintptr_t>(data) + 0x18, ev.guid, sizeof(ev.guid));
    ev.dataClass = dc.type;
    ev.writerEnt = props - 0x30;
    ev.writerType = registry().typeOf(mem::ProcessMemory(), ev.writerEnt);
    // Its targets (entity + 0x88..0x90: the animated entities, through their +8 base) name the state's owner.
    const uintptr_t b = mem::readOr<uintptr_t>(ev.writerEnt + 0x88), en = mem::readOr<uintptr_t>(ev.writerEnt + 0x90);
    for (uintptr_t q = b; q && q < en && q < b + 64 * 8; q += 8) {
        const uintptr_t ref = mem::readOr<uintptr_t>(q);
        const uintptr_t base = ref ? mem::readOr<uintptr_t>(ref) : 0;
        if (!base) continue;
        const uintptr_t anim = base - 8;
        if (antStateOf(anim) == ev.antState) {
            std::lock_guard<std::mutex> lock(g_antM);
            g_antOwner[ev.antState] = {anim, registry().typeOf(mem::ProcessMemory(), anim)};
        }
    }
    const int n = antCount(ev.valueKind);
    recordAnim(std::move(ev), n);
}

template <size_t I> void antDataDetour(void* data, void* props, uint32_t* wIdx, uint32_t* vIdx, void* state) {
    if (antRecording()) {
        try {
            antDataWrite(I, data, props, wIdx, vIdx, state);
        } catch (...) {
        }
        t_antCtx.active = true; // the writer interface it calls is not recorded a second time
        reinterpret_cast<AntDataFn>(g_antData[I].orig)(data, props, wIdx, vIdx, state);
        t_antCtx.active = false;
        return;
    }
    reinterpret_cast<AntDataFn>(g_antData[I].orig)(data, props, wIdx, vIdx, state);
}
template <size_t... I> constexpr auto makeAntData(std::index_sequence<I...>) { return std::array<AntDataFn, sizeof...(I)>{&antDataDetour<I>...}; }
const auto g_antDataDetours = makeAntData(std::make_index_sequence<kAntDataCount>{});
std::atomic<bool> g_antHooked{false};

void hookAnim() {
    if (g_antHooked.exchange(true)) return;
    if (!build::codeMatches("WriteBoolGameState_write") || !build::codeMatches("TypedValue_getValuePtr")) {
        GN_LOG("recorder: the game state writers are not as expected; animation flags are not recorded");
        return;
    }
    size_t done = 0;
    for (size_t i = 0; i < kAntDataCount; ++i) {
        AntDataClass& dc = g_antData[i];
        dc.type = registry().find(dc.name);
        if (!dc.type || dc.type->vtables.empty()) {
            GN_LOG("recorder: no %s type or vtable; its writes are not recorded", dc.name);
            continue;
        }
        const uintptr_t vt = static_cast<uintptr_t>(dc.type->vtables[0] - kImageBase) + mem::moduleBase();
        std::string why;
        if (hooks().install("recorder", vt + kAntDataSlot, 0, reinterpret_cast<uint64_t>(g_antDataDetours[i]), &dc.orig, &why) == hook::Result::Ok)
            ++done;
        else
            GN_LOG("recorder: %s write: %s", dc.name, why.c_str());
    }
    GN_LOG("recorder: animation game state writes: %llu of %llu writer classes wrapped", static_cast<unsigned long long>(done),
           static_cast<unsigned long long>(kAntDataCount));
}

void marker(const std::string& name) {
    Event ev;
    ev.t = now();
    ev.kind = kMarker;
    ev.thread = GetCurrentThreadId();
    ev.text = name.empty() ? "marker" : name;
    gn_transform f{};
    if (void* faith = localClientCharacter(); faith && entityTransform(faith, f)) {
        memcpy(ev.pos, f.trans, sizeof(ev.pos));
        ev.havePos = true;
    }
    push(std::move(ev));
}

void setRecording(bool on) {
    if (on == g_recording.load()) return;
    if (on) {
        std::lock_guard<std::mutex> lock(g_m);
        if (g_events.empty()) QueryPerformanceCounter(&g_t0); // time 0 is the start of a new recording
    }
    if (on && g_wantEvents.load()) hookEvents();
    if (on && g_wantAnim.load()) hookAnim();
    if (on) {
        std::lock_guard<std::mutex> lock(g_antM);
        g_antLast.clear(); // each recording starts with the values as they are
    }
    g_recording = on;
    if (on) messageTap(&onMessage);
    GN_LOG("recorder: %s", on ? "recording" : "paused");
}

void clearAll() {
    std::lock_guard<std::mutex> lock(g_m);
    g_events.clear();
    g_dropped = 0;
    QueryPerformanceCounter(&g_t0);
}

// ---- export (JSON lines) ----------------------------------------------------------------------
std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += static_cast<char>(c);
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else o += static_cast<char>(c);
    }
    return o + "\"";
}

std::string hexAddr(uint64_t a) {
    char b[24];
    snprintf(b, sizeof(b), "\"0x%llx\"", static_cast<unsigned long long>(a));
    return b;
}

std::string num(double v) {
    if (!std::isfinite(v)) return "null";
    char b[32];
    snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

const char* managerName(int m) { return m == 0 ? "core" : m == 1 ? "client" : "server"; }

// A reflected field of a copied message, as JSON (null when its type is not one we decode).
std::string fieldValue(const reflect::FieldInfo& fi, const std::vector<uint8_t>& b, size_t off) {
    if (!fi.type || off + std::max<uint32_t>(fi.size, 1) > b.size()) return "null";
    const std::string& n = fi.type->name;
    auto rd = [&](auto v) {
        memcpy(&v, &b[off], sizeof(v));
        return v;
    };
    if (fi.type->category == reflect::Category::Primitive || fi.type->category == reflect::Category::Enum) {
        if (n == "Boolean") return rd(uint8_t{}) ? "true" : "false";
        if (n == "Int8") return std::to_string(rd(int8_t{}));
        if (n == "Uint8") return std::to_string(rd(uint8_t{}));
        if (n == "Int16") return std::to_string(rd(int16_t{}));
        if (n == "Uint16") return std::to_string(rd(uint16_t{}));
        if (n == "Int32" || fi.type->category == reflect::Category::Enum) return std::to_string(rd(int32_t{}));
        if (n == "Uint32") return std::to_string(rd(uint32_t{}));
        if (n == "Int64") return std::to_string(rd(int64_t{}));
        if (n == "Uint64") return std::to_string(rd(uint64_t{}));
        if (n == "Float32") return num(rd(float{}));
        if (n == "Float64") return num(rd(double{}));
        if (n == "CString") return jsonStr(readCString(static_cast<uintptr_t>(rd(uint64_t{}))));
    }
    if (fi.type->category == reflect::Category::Class) return hexAddr(rd(uint64_t{}));
    if (n == "Vec2" || n == "Vec3" || n == "Vec4" || n == "LinearTransform" || n == "Quat") {
        const size_t count = n == "Vec2" ? 2 : n == "LinearTransform" ? 16 : n == "Vec3" ? 3 : 4;
        std::string o = "[";
        for (size_t i = 0; i < count && off + 4 * i + 4 <= b.size(); ++i) {
            float f;
            memcpy(&f, &b[off + 4 * i], 4);
            o += (i ? "," : "") + num(f);
        }
        return o + "]";
    }
    return "null";
}

std::string toJson(const Event& ev) {
    std::string o = "{\"seq\":" + std::to_string(ev.seq) + ",\"t\":" + num(ev.t) + ",\"thread\":" + std::to_string(ev.thread);
    switch (ev.kind) {
    case kMessage: {
        const Type* t = messageTypeById(ev.typeId);
        char id[16];
        snprintf(id, sizeof(id), "%08x", ev.typeId);
        o += ",\"kind\":\"message\",\"manager\":\"" + std::string(managerName(ev.manager)) + "\"";
        o += ",\"category\":" + jsonStr(messageCategoryName(ev.category));
        o += ",\"type\":" + jsonStr(t ? t->name : std::string(id));
        o += ",\"realm\":" + std::to_string(ev.realm);
        if (t && !t->fields.empty()) {
            o += ",\"fields\":{";
            bool first = true;
            for (const reflect::Field& f : t->fields) {
                const reflect::FieldInfo fi = registry().describe(t, f);
                o += (first ? "" : ",") + jsonStr(f.name) + ":" + fieldValue(fi, ev.bytes, f.offset);
                first = false;
            }
            o += "}";
        }
        if (!ev.refs.empty()) {
            o += ",\"pointers\":[";
            for (size_t i = 0; i < ev.refs.size(); ++i) {
                char b[48];
                snprintf(b, sizeof(b), "{\"off\":%u,\"ptr\":", ev.refs[i].off);
                o += (i ? "," : "") + std::string(b) + hexAddr(ev.refs[i].ptr) + ",\"type\":" + jsonStr(ev.refs[i].type->name) + "}";
            }
            o += "]";
        }
        std::string hex;
        hex.reserve(ev.bytes.size() * 2);
        for (uint8_t c : ev.bytes) {
            char b[3];
            snprintf(b, sizeof(b), "%02x", c);
            hex += b;
        }
        o += ",\"bytes\":\"" + hex + "\"";
        break;
    }
    case kCreated:
        o += ",\"kind\":\"created\",\"type\":" + jsonStr(ev.type ? ev.type->name : "?") + ",\"ptr\":" + hexAddr(ev.ptr);
        if (ev.dataType) o += ",\"data\":" + jsonStr(ev.dataType->name) + ",\"data_ptr\":" + hexAddr(ev.data);
        if (!ev.blueprint.empty()) o += ",\"blueprint\":" + jsonStr(ev.blueprint);
        break;
    case kMarker: o += ",\"kind\":\"marker\",\"name\":" + jsonStr(ev.text); break;
    case kLevel: o += ",\"kind\":\"level\",\"level\":" + jsonStr(ev.text); break;
    case kAnim: {
        o += ",\"kind\":\"anim\",\"value_type\":\"" + std::string(antValueName(ev.valueKind)) + "\",\"value\":";
        const int n = antCount(ev.valueKind);
        if (ev.valueKind == kAntBool) o += ev.value[0] != 0.f ? "true" : "false";
        else if (n == 1) o += num(ev.value[0]);
        else {
            o += "[";
            for (int i = 0; i < n; ++i) o += (i ? "," : "") + num(ev.value[i]);
            o += "]";
        }
        if (ev.writerType) {
            char id[16];
            snprintf(id, sizeof(id), "%08x", ev.propHash);
            o += ",\"property\":" + jsonStr(eventName(ev.propHash)) + ",\"property_id\":\"" + id + "\"";
            o += ",\"by\":" + jsonStr(ev.writerType->name) + ",\"by_ptr\":" + hexAddr(ev.writerEnt);
            if (ev.dataClass) o += ",\"writer_class\":" + jsonStr(ev.dataClass->name);
        } else {
            o += ",\"by\":\"game code\"";
        }
        if (ev.haveGuid) {
            char g[40];
            snprintf(g, sizeof(g), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", ev.guid[3], ev.guid[2], ev.guid[1], ev.guid[0],
                     ev.guid[5], ev.guid[4], ev.guid[7], ev.guid[6], ev.guid[8], ev.guid[9], ev.guid[10], ev.guid[11], ev.guid[12], ev.guid[13],
                     ev.guid[14], ev.guid[15]);
            o += ",\"game_state\":\"" + std::string(g) + "\"";
        }
        o += ",\"ant_writer\":" + hexAddr(ev.antWriter) + ",\"ant_writer_vtable\":" +
             hexAddr(ev.antWriter ? mem::va(mem::readOr<uintptr_t>(ev.antWriter)) : 0) + ",\"state\":" + hexAddr(ev.antState);
        o += ",\"target\":" + jsonStr(ev.type ? ev.type->name : "?") + ",\"ptr\":" + hexAddr(ev.ptr);
        break;
    }
    case kEvent: {
        char id[16];
        snprintf(id, sizeof(id), "%08x", ev.eventId);
        o += ",\"kind\":\"event\",\"event\":" + jsonStr(eventName(ev.eventId)) + ",\"event_id\":\"" + id + "\",\"target\":" +
             jsonStr(ev.type ? ev.type->name : "?") + ",\"ptr\":" + hexAddr(ev.ptr);
        break;
    }
    }
    if (ev.havePos) o += ",\"pos\":[" + num(ev.pos[0]) + "," + num(ev.pos[1]) + "," + num(ev.pos[2]) + "]";
    if (!ev.stack.empty()) {
        o += ",\"stack\":[";
        for (size_t i = 0; i < ev.stack.size(); ++i) o += (i ? "," : "") + hexAddr(ev.stack[i]);
        o += "]";
    }
    return o + "}\n";
}

bool validName(const std::string& n) {
    for (char c : n)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    return n.size() <= 40;
}

// Writes everything recorded (a copy, on its own thread so the game does not stall).
void exportAll(const std::string& name) {
    std::vector<Event> events;
    uint64_t dropped;
    std::string level;
    {
        std::lock_guard<std::mutex> lock(g_m);
        events.assign(g_events.begin(), g_events.end());
        dropped = g_dropped;
        level = g_level;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::string file = std::string(stamp) + (name.empty() ? "" : "-" + name) + ".jsonl";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    const std::wstring path = g_dir + std::wstring(file.begin(), file.end());
    g_lastExport = "recordings\\" + file + " (writing...)";
    std::thread([events = std::move(events), dropped, level, path, file] {
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) {
            GN_LOG("recorder: cannot write recordings\\%s", file.c_str());
            return;
        }
        std::string head = "{\"kind\":\"header\",\"format\":\"gridnode-recording\",\"version\":1,\"gridnode\":\"" GRIDNODE_VERSION_STRING
                           "\",\"image_base\":\"0x140000000\",\"level\":" +
                           jsonStr(level) + ",\"events\":" + std::to_string(events.size()) + ",\"dropped_oldest\":" + std::to_string(dropped) +
                           "}\n";
        fwrite(head.data(), 1, head.size(), f);
        for (const Event& ev : events) {
            const std::string line = toJson(ev);
            fwrite(line.data(), 1, line.size(), f);
        }
        fclose(f);
        GN_LOG("recorder: exported %llu events to recordings\\%s", static_cast<unsigned long long>(events.size()), file.c_str());
    }).detach();
    g_lastExport = "recordings\\" + file;
}

// ---- keys -------------------------------------------------------------------------------------
constexpr int kKeyRecord = VK_F8, kKeyMarker = VK_HOME;
bool g_keyWas[2] = {};
char g_markerName[64] = "marker";
int g_markerCount = 0;

std::string nextMarkerName() {
    ++g_markerCount;
    char b[96];
    snprintf(b, sizeof(b), "%s %d", g_markerName[0] ? g_markerName : "marker", g_markerCount);
    return b;
}

} // namespace

void start(const std::wstring& gridnodeDir) {
    g_dir = gridnodeDir + L"recordings\\";
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    gn::host::onCreated("gridnode", "Entity", &onCreated, nullptr);
}

void onLevel(const std::string& level) {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_level = level;
    }
    if (!g_recording.load()) return;
    Event ev;
    ev.t = now();
    ev.kind = kLevel;
    ev.text = level;
    push(std::move(ev));
}

void frame() {
    const bool focused = ui::gameFocused() && !ImGui::GetIO().WantTextInput;
    const int keys[2] = {kKeyRecord, kKeyMarker};
    bool pressed[2] = {};
    for (int i = 0; i < 2; ++i) {
        const bool down = focused && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        pressed[i] = down && !g_keyWas[i];
        g_keyWas[i] = down;
    }
    if (pressed[0]) setRecording(!g_recording.load());
    if (pressed[1] && g_recording.load()) marker(nextMarkerName());
    if (g_recording.load()) { // a small red dot, top right, while recording
        const ImGuiIO& io = ImGui::GetIO();
        const float r = ImGui::GetFontSize() * 0.4f;
        ImGui::GetForegroundDrawList()->AddCircleFilled(ImVec2(io.DisplaySize.x - r * 3.f, r * 3.f), r, IM_COL32(230, 40, 40, 220));
    }
}

void draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, ImGui::GetFontSize() * 30), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Recorder", open)) {
        ImGui::End();
        return;
    }
    const bool rec = g_recording.load();
    size_t count;
    uint64_t dropped;
    double span = 0;
    {
        std::lock_guard<std::mutex> lock(g_m);
        count = g_events.size();
        dropped = g_dropped;
        if (!g_events.empty()) span = g_events.back().t - g_events.front().t;
    }
    if (ImGui::Button(rec ? "Pause (F8)" : count ? "Resume (F8)" : "Record (F8)")) setRecording(!rec);
    ImGui::SameLine();
    ImGui::BeginDisabled(rec || !count);
    if (ImGui::Button("Clear")) clearAll(), g_markerCount = 0;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (rec) ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "recording");
    else ImGui::TextDisabled(count ? "paused" : "not recording");
    ImGui::SameLine();
    ImGui::TextDisabled("%llu events over %.1f s%s", static_cast<unsigned long long>(count), span, dropped ? " (oldest dropped)" : "");

    bool m = g_wantMessages.load(), c = g_wantCreated.load(), s = g_wantStacks.load();
    if (ImGui::Checkbox("Messages", &m)) g_wantMessages = m;
    ImGui::SameLine();
    if (ImGui::Checkbox("Entities created", &c)) g_wantCreated = c;
    ImGui::SameLine();
    bool ev = g_wantEvents.load();
    if (ImGui::Checkbox("Entity events", &ev)) {
        g_wantEvents = ev;
        if (ev && rec) hookEvents();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Call stacks", &s)) g_wantStacks = s;
    bool an = g_wantAnim.load(), af = g_wantAnimFloats.load();
    if (ImGui::Checkbox("Animation flags", &an)) {
        g_wantAnim = an;
        if (an && rec) hookAnim();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!an);
    if (ImGui::Checkbox("with floats and vectors", &af)) g_wantAnimFloats = af;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Changes to animation (ANT) game state values: the flags that drive doors and other animated objects.\n"
                          "Bools, ints and enums are recorded when they change; floats and vectors (which change all the time) only with\n"
                          "the second box, at most 10 a second each.");

    ImGui::SeparatorText("Markers");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputText("##marker", g_markerName, sizeof(g_markerName));
    ImGui::SameLine();
    ImGui::BeginDisabled(!rec);
    if (ImGui::Button("Drop marker (Home)")) marker(nextMarkerName());
    ImGui::EndDisabled();
    ImGui::TextDisabled("Press Home in game right when the thing happens (got the KSEC, the door opened).");

    ImGui::SeparatorText("Export");
    static char name[48] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##name", "name (optional)", name, sizeof(name));
    ImGui::SameLine();
    const bool nameOk = validName(name);
    ImGui::BeginDisabled(!count || !nameOk);
    if (ImGui::Button("Export")) exportAll(name);
    ImGui::EndDisabled();
    if (!nameOk) ImGui::TextDisabled("Names: letters, digits, - and _");
    if (!g_lastExport.empty()) ImGui::TextDisabled("Last export: %s", g_lastExport.c_str());

    ImGui::SeparatorText("Ignore");
    ImGui::TextDisabled("Not recorded (frequent ones you do not need). Right-click a message or an event below to ignore its type or class.");
    std::vector<uint32_t> ignored;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const auto& kv : g_ignored) ignored.push_back(kv.first);
    }
    for (uint32_t id : ignored) {
        const Type* t = messageTypeById(id);
        ImGui::PushID(static_cast<int>(id));
        if (ImGui::SmallButton((std::string(t ? t->name : "?") + "  x").c_str())) {
            std::lock_guard<std::mutex> lock(g_m);
            g_ignored.erase(id);
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    std::vector<const Type*> ignoredT;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (const auto& kv : g_ignoredTargets) ignoredT.push_back(kv.first);
    }
    for (const Type* t : ignoredT) {
        ImGui::PushID(t);
        if (ImGui::SmallButton(("events to " + t->name + "  x").c_str())) {
            std::lock_guard<std::mutex> lock(g_m);
            g_ignoredTargets.erase(t);
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (ignored.empty() && ignoredT.empty()) ImGui::TextDisabled("(none)");
    else ImGui::NewLine();

    ImGui::SeparatorText("Latest");
    if (ImGui::BeginChild("latest", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        std::vector<Event> tail;
        {
            std::lock_guard<std::mutex> lock(g_m);
            const size_t from = g_events.size() > 300 ? g_events.size() - 300 : 0;
            for (size_t i = from; i < g_events.size(); ++i) {
                const Event& src = g_events[i];
                Event lite; // without the bytes and the stack
                lite.seq = src.seq, lite.t = src.t, lite.kind = src.kind, lite.manager = src.manager, lite.typeId = src.typeId;
                lite.type = src.type, lite.blueprint = src.blueprint, lite.text = src.text, lite.eventId = src.eventId;
                lite.valueKind = src.valueKind, lite.propHash = src.propHash, lite.writerType = src.writerType;
                memcpy(lite.value, src.value, sizeof(lite.value));
                tail.push_back(std::move(lite));
            }
        }
        for (const Event& ev : tail) {
            char line[300];
            switch (ev.kind) {
            case kMessage: {
                const Type* t = messageTypeById(ev.typeId);
                snprintf(line, sizeof(line), "%9.3f  %-6s %s", ev.t, managerName(ev.manager), t ? t->name.c_str() : "?");
                ImGui::TextUnformatted(line);
                if (ImGui::BeginPopupContextItem(std::to_string(ev.seq).c_str())) {
                    if (ImGui::MenuItem("Do not record this type")) {
                        std::lock_guard<std::mutex> lock(g_m);
                        g_ignored[ev.typeId] = true;
                    }
                    ImGui::EndPopup();
                }
                break;
            }
            case kCreated:
                snprintf(line, sizeof(line), "%9.3f  new    %s%s%s", ev.t, ev.type ? ev.type->name.c_str() : "?", ev.blueprint.empty() ? "" : "  ",
                         ev.blueprint.c_str());
                ImGui::TextDisabled("%s", line);
                break;
            case kMarker:
                snprintf(line, sizeof(line), "%9.3f  ---- %s ----", ev.t, ev.text.c_str());
                ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "%s", line);
                break;
            case kEvent: {
                snprintf(line, sizeof(line), "%9.3f  event  %s -> %s", ev.t, eventName(ev.eventId).c_str(), ev.type ? ev.type->name.c_str() : "?");
                ImGui::TextColored(ImVec4(0.6f, 1.f, 0.6f, 1.f), "%s", line);
                if (ImGui::BeginPopupContextItem(std::to_string(ev.seq).c_str())) {
                    if (ImGui::MenuItem("Do not record events to this class") && ev.type) {
                        std::lock_guard<std::mutex> lock(g_m);
                        g_ignoredTargets[ev.type] = true;
                    }
                    ImGui::EndPopup();
                }
                break;
            }
            case kAnim: {
                char val[64];
                if (ev.valueKind == kAntBool) snprintf(val, sizeof(val), "%s", ev.value[0] != 0.f ? "true" : "false");
                else if (antCount(ev.valueKind) == 1) snprintf(val, sizeof(val), "%g", ev.value[0]);
                else snprintf(val, sizeof(val), "(%.2f %.2f %.2f)", ev.value[0], ev.value[1], ev.value[2]);
                snprintf(line, sizeof(line), "%9.3f  anim   %s = %s  -> %s", ev.t, ev.writerType ? eventName(ev.propHash).c_str() : "(game code)", val,
                         ev.type ? ev.type->name.c_str() : "?");
                ImGui::TextColored(ImVec4(1.f, 0.6f, 1.f, 1.f), "%s", line);
                break;
            }
            case kLevel:
                snprintf(line, sizeof(line), "%9.3f  level %s", ev.t, ev.text.c_str());
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "%s", line);
                break;
            }
        }
        if (rec && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f) ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace gn::host::recorder
