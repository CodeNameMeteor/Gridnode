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
#include "gridnode/reflect/layout.hpp"
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

enum Kind : uint8_t { kMessage, kCreated, kMarker, kLevel, kEvent, kAnim, kField, kSent, kCall, kDestroyed, kList };

struct Ref {           // a pointer found in a message, with what it pointed to then
    uint16_t off;
    uintptr_t ptr;
    const Type* type;
    uint32_t obj = 0;  // its object in the recording's object table (1-based; 0: none)
};

struct ListEntry {     // an entry of a watched list (a vector of pointers)
    uintptr_t ptr = 0;
    const Type* type = nullptr;
    uint32_t obj = 0;
    uint16_t index = 0;
    uint8_t what = 0;  // 0 in the list now, 1 added, 2 removed
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
    // field: a watched field of an entity changed (ptr / type: the entity; text: the field's label)
    char fieldType = 0;            // 'b' bool / byte, 'i' i32, 'f' f32
    uint32_t fieldOff = 0;
    // call: a traced class's virtual function ran (ptr / type: the object, fieldOff: the vtable offset)
    uint64_t callFn = 0;           // the function called (image VA)
    uint64_t callFrom = 0;         // its return address (image VA, or 0 when outside the game)
    uint32_t callSkipped = 0;      // calls of this slot not recorded since the previous one (rate limit)
    // list: a watched list of an entity changed (ptr / type: the entity; text: its label; fieldOff: the
    // vector's offset; value[0] / [1]: count now / before, value[2]: 1 for the first reading)
    std::vector<ListEntry> list;
    bool reordered = false;
    // marker / level / field label / sent event's class
    std::string text;
    std::vector<uint64_t> stack;   // image VAs of game frames, innermost first
    uint32_t obj = 0;              // ptr's object in the object table (1-based; 0: none)
};

// ---- the object table -----------------------------------------------------------------------------
// Every object a recorded event points at (the entity an event went to, a message's pointers, a
// list's entries) gets one entry: its type, and, for entities created while recording, its data and
// blueprint and when it was created; when it was destroyed. An address the game reuses for a new
// object gets a new entry, so an export names each pointer by what it was at that moment. Exported
// as "object" lines after the events; events carry "obj" ids into it.
struct Obj {
    uintptr_t ptr = 0;
    const Type* type = nullptr;
    uintptr_t data = 0;
    const Type* dataType = nullptr;
    std::string blueprint;
    double created = NAN;          // NAN: before the recording (first seen in an event)
    double destroyed = NAN;        // NAN: not destroyed while recording
    double firstSeen = 0;
};
constexpr size_t kMaxObjs = 500000;

std::mutex g_m;
std::deque<Event> g_events;        // (g_m)
uint64_t g_seq = 0;                // (g_m)
uint64_t g_dropped = 0;            // (g_m) the oldest, beyond kMaxEvents
// Rolling buffer: only the last g_rollSeconds are kept (older events drop off as new ones come), and
// End saves them, so a recording can be made after the thing happened instead of started before it.
std::atomic<bool> g_rolling{false};
std::atomic<int> g_rollSeconds{30};
uint64_t g_rolledOff = 0; // (g_m) events past the window, not counted as dropped
// Start on an event: armed with an event's id; the first time it is delivered, the next frame starts
// recording and drops a marker named after it.
std::atomic<uint32_t> g_autoEventId{0};
std::atomic<bool> g_autoStart{false};
std::string g_autoEventName; // render thread
std::atomic<bool> g_recording{false};
std::atomic<bool> g_wantMessages{true}, g_wantCreated{true}, g_wantStacks{true}, g_wantEvents{true};
std::atomic<bool> g_wantAnim{true}, g_wantAnimFloats{false};
LARGE_INTEGER g_freq{}, g_t0{};
std::wstring g_dir;
std::string g_lastExport;          // render thread
std::unordered_map<uint32_t, bool> g_ignored; // message type ids not recorded (g_m)
std::unordered_map<const Type*, bool> g_ignoredTargets; // entity classes whose events are not recorded (g_m)
std::string g_level;               // (g_m)
std::vector<Obj> g_objs;           // (g_m) id = index + 1
std::unordered_map<uintptr_t, uint32_t> g_objAt; // (g_m) address -> the latest object there
std::atomic<bool> g_wantDestroyed{true};
std::atomic<bool> g_wantMovement{false}; // off by default: only for movement research (bhops, wall moves)

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

// The object at `p` at time t (caller holds g_m): the latest one there if it is still the same (the
// same type, not destroyed before t, or `gone` allows it), else a new entry. 0 when there is no type
// to go by or the table is full.
uint32_t objFor(uintptr_t p, const Type* type, double t, bool gone = false) {
    if (!p) return 0;
    if (auto it = g_objAt.find(p); it != g_objAt.end()) {
        const Obj& o = g_objs[it->second - 1];
        const bool sameType = !type || !o.type || o.type == type;
        if (sameType && (gone || std::isnan(o.destroyed) || o.destroyed > t)) return it->second;
    }
    if (!type || g_objs.size() >= kMaxObjs) return 0;
    Obj o;
    o.ptr = p, o.type = type, o.firstSeen = t;
    g_objs.push_back(std::move(o));
    const uint32_t id = static_cast<uint32_t>(g_objs.size());
    g_objAt[p] = id;
    return id;
}

// Links an event's pointers to the object table (caller holds g_m).
void linkObjects(Event& ev) {
    if (ev.kind == kCreated && ev.ptr && g_objs.size() < kMaxObjs) {
        Obj o;
        o.ptr = ev.ptr, o.type = ev.type, o.data = ev.data, o.dataType = ev.dataType, o.blueprint = ev.blueprint;
        o.created = o.firstSeen = ev.t;
        g_objs.push_back(std::move(o));
        ev.obj = static_cast<uint32_t>(g_objs.size());
        g_objAt[ev.ptr] = ev.obj;
    } else if (ev.kind == kDestroyed) {
        ev.obj = objFor(ev.ptr, ev.type, ev.t);
        if (ev.obj) {
            Obj& o = g_objs[ev.obj - 1];
            o.destroyed = ev.t;
            if (!ev.type) ev.type = o.type;
            if (ev.blueprint.empty()) ev.blueprint = o.blueprint;
        }
    } else if (ev.ptr && ev.type) {
        ev.obj = objFor(ev.ptr, ev.type, ev.t);
    }
    for (Ref& r : ev.refs) r.obj = objFor(r.ptr, r.type, ev.t);
    for (ListEntry& le : ev.list) le.obj = objFor(le.ptr, le.type, ev.t, le.what == 2);
}

void push(Event&& ev) {
    std::lock_guard<std::mutex> lock(g_m);
    linkObjects(ev);
    ev.seq = ++g_seq;
    g_events.push_back(std::move(ev));
    while (g_events.size() > kMaxEvents) {
        g_events.pop_front();
        ++g_dropped;
    }
    if (g_rolling.load(std::memory_order_relaxed)) {
        const double oldest = g_events.back().t - g_rollSeconds.load(std::memory_order_relaxed);
        while (!g_events.empty() && g_events.front().t < oldest) {
            g_events.pop_front();
            ++g_rolledOff;
        }
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

void watchCreated(uintptr_t p);   // the watches (below) follow creations and destructions
void watchDestroyed(uintptr_t p);

// An entity is being destroyed (its deleting destructor, hooked in entities.cpp; before the original
// runs, so its vtable still says what it is).
void onDestroyedImpl(void* entity) {
    watchDestroyed(reinterpret_cast<uintptr_t>(entity));
    if (!g_recording.load(std::memory_order_relaxed) || !g_wantDestroyed.load(std::memory_order_relaxed) || !entity) return;
    Event ev;
    ev.t = now();
    ev.kind = kDestroyed;
    ev.thread = GetCurrentThreadId();
    ev.ptr = reinterpret_cast<uintptr_t>(entity);
    ev.type = registry().typeOf(mem::ProcessMemory(), ev.ptr);
    captureStack(ev.stack);
    push(std::move(ev));
}

void onCreated(void* entity, void* ci, void*) {
    if (entity) watchCreated(reinterpret_cast<uintptr_t>(entity));
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
// The wrappers on every entity's onEvent are events.cpp's (shared with mods); the Recorder is their
// tap while it runs (set at start, does nothing unless recording events).
void hookEvents() { events::ensureHooked("recorder"); }
uint32_t djb2(const std::string& s) { return events::hash(s); }
std::string eventName(uint32_t id) { return events::name(id); }

void onEvent(void* peer, void* event) {
    if (const uint32_t armed = g_autoEventId.load(std::memory_order_relaxed); armed && event && !g_recording.load(std::memory_order_relaxed) &&
        mem::readOr<uint32_t>(reinterpret_cast<uintptr_t>(event) + 8) == armed) {
        g_autoEventId = 0;
        g_autoStart = true; // started on the render thread, which owns the recording's controls
    }
    if (!g_recording.load(std::memory_order_relaxed) || !g_wantEvents.load(std::memory_order_relaxed)) return;
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
bool movementRecording() { return g_recording.load(std::memory_order_relaxed) && g_wantMovement.load(std::memory_order_relaxed); }
void predictWrite(uintptr_t writer, uintptr_t state, uint8_t kind, float v); // the landing prediction (below)

template <size_t I> void antIntDetour(void* w, void* state, uint64_t v) {
    if (antRecording() && !t_antCtx.active) {
        const float f[1] = {static_cast<float>(g_antIntKind[I] == kAntBool ? static_cast<uint8_t>(v) : static_cast<int32_t>(v))};
        antCodeWrite(reinterpret_cast<uintptr_t>(w), reinterpret_cast<uintptr_t>(state), g_antIntKind[I], f);
    }
    if (movementRecording())
        predictWrite(reinterpret_cast<uintptr_t>(w), reinterpret_cast<uintptr_t>(state), g_antIntKind[I],
                     static_cast<float>(g_antIntKind[I] == kAntBool ? static_cast<uint8_t>(v) : static_cast<int32_t>(v)));
    reinterpret_cast<void (*)(void*, void*, uint64_t)>(g_antIntOrig[I])(w, state, v);
}
template <size_t I> void antFloatDetour(void* w, void* state, float v) {
    if (antRecording() && !t_antCtx.active) antCodeWrite(reinterpret_cast<uintptr_t>(w), reinterpret_cast<uintptr_t>(state), kAntFloat, &v);
    if (movementRecording()) predictWrite(reinterpret_cast<uintptr_t>(w), reinterpret_cast<uintptr_t>(state), kAntFloat, v);
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
    // The slot may already hold GridNode's own wrapper (landing_prediction): the hook table checks it
    // and chains, so only the vtable itself is checked here.
    const uintptr_t vt = writer ? mem::readOr<uintptr_t>(writer) : 0;
    if (!mem::inImage(vt) || !mem::readOr<uintptr_t>(vt + kAntWriteSlot)) return;
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

// ---- the landing prediction (Faith's movement) --------------------------------------------------
// ANT's PositionPredictionProcessorAsset (vtable 0x141aabb98) predicts where and when a character
// hits the ground. Its update (+0x20, 0x142f28e20) runs the character's trajectory against the world
// (0x142f3b9a0) when a jump or fall trajectory starts, then writes the results through game-state
// writers: hit height (asset +0x20), ticks to hit (+0x28, in 60ths of a second) and soft landing
// (+0x30); each binding's writer is at its +0x20. With Faith's movement on, GridNode wraps that
// update (a pointer swap), learns the writers, and records their writes as field lines on the
// character they belong to: the prediction next to the landing.
constexpr uint64_t kPredictVtable = 0x141aabb98, kPredictSlot = 0x20, kPredictFn = 0x142f28e20;
constexpr uint64_t kAntFloatWriterVtable = 0x141a434d0, kAntBoolWriterVtable = 0x141a435c8; // as recorded (2026-10-30)
struct PredictOut {
    uint32_t off;
    const char* label;
    uint8_t kind;
};
constexpr PredictOut kPredictOut[] = {
    {0x20, "predicted hit height", kAntFloat},
    {0x28, "predicted ticks to hit", kAntFloat}, // 60ths of a second (0x142f3b9a0 multiplies by 60)
    {0x30, "predicted soft landing", kAntBool},
};
std::unordered_map<uintptr_t, const char*> g_predictWriters; // (g_antM) writer -> label
std::unordered_map<uint64_t, float> g_predictLast;           // (g_antM) state ^ writer -> last value
uint64_t g_predictOrig = 0;
std::atomic<bool> g_predictHooked{false};

void predictDetour(void* asset, void* ctx, void* a3, void* a4) {
    if (movementRecording()) {
        try {
            for (const PredictOut& o : kPredictOut) {
                const uintptr_t binding = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(asset) + o.off);
                if (!binding) continue;
                const uintptr_t writer = binding + 0x20;
                // Only the writer classes the Recorder has met with these value types (a float wrapper on
                // an int writer would pass the wrong register through).
                const uint64_t wvt = mem::va(mem::readOr<uintptr_t>(writer));
                if (wvt != (o.kind == kAntBool ? kAntBoolWriterVtable : kAntFloatWriterVtable)) {
                    static std::atomic<int> said{0};
                    if (said.fetch_add(1) < 3) GN_LOG("recorder: landing prediction: %s writer has vtable 0x%llx; not recorded", o.label,
                                                      static_cast<unsigned long long>(wvt));
                    continue;
                }
                hookAntWriter(writer, o.kind);
                std::lock_guard<std::mutex> lock(g_antM);
                g_predictWriters[writer] = o.label;
            }
        } catch (...) {
        }
    }
    reinterpret_cast<void (*)(void*, void*, void*, void*)>(g_predictOrig)(asset, ctx, a3, a4);
}

void hookPredict() {
    if (g_predictHooked.exchange(true)) return;
    // The hook table checks the slot: the game's function, or GridNode's landing_prediction wrapper
    // on top of it (then this one chains).
    const uintptr_t slot = mem::rva(kPredictVtable) + kPredictSlot;
    std::string why;
    const auto r = hooks().install("recorder", slot, mem::rva(kPredictFn), reinterpret_cast<uint64_t>(&predictDetour), &g_predictOrig, &why);
    GN_LOG("recorder: landing prediction (PositionPredictionProcessorAsset +0x20) %s%s", r == hook::Result::Ok ? "wrapped" : "not wrapped: ",
           r == hook::Result::Ok ? "" : why.c_str());
}

void predictWrite(uintptr_t writer, uintptr_t state, uint8_t kind, float v) try {
    const char* label = nullptr;
    float old = 0.f;
    bool first = true;
    {
        std::lock_guard<std::mutex> lock(g_antM);
        const auto it = g_predictWriters.find(writer);
        if (it == g_predictWriters.end()) return;
        label = it->second;
        const uint64_t key = static_cast<uint64_t>(state) * 1000003ull ^ static_cast<uint64_t>(writer);
        const auto last = g_predictLast.find(key);
        if (last != g_predictLast.end()) old = last->second, first = false;
        g_predictLast[key] = v;
    }
    const auto [owner, ownerType] = antOwner(state); // takes g_antM itself
    Event ev;
    ev.t = now();
    ev.kind = kField;
    ev.thread = GetCurrentThreadId();
    ev.ptr = owner, ev.type = ownerType;
    ev.text = label;
    ev.fieldType = kind == kAntBool ? 'b' : 'f', ev.fieldOff = 0;
    ev.value[0] = v, ev.value[1] = old, ev.value[2] = first ? 1.f : 0.f;
    push(std::move(ev)); // every prediction, changed or not: there is one per jump
} catch (...) {
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


// ---- watched fields -----------------------------------------------------------------------------
// Fields of live entities of a class, read every frame while recording (or less often: the menu's rate); each change is
// recorded (kField) with the old value, so a state (an alert's level, a door's flag) lines up with
// the events and messages around it. Instances are added when they are created and dropped when they
// are destroyed (the creator and destructor hooks); entities that existed before a watch was added
// are found among the tracked entities (every 2 s, and when recording starts).
// A list watch ('l') reads a vector of pointers at the offset (begin, end: eastl::vector) and records
// what was added, removed, or moved, with each entry's type (kList).
struct Watch {
    std::string cls;   // class name (subclasses too)
    uint32_t off = 0;
    char type = 'i';   // 'b' byte / bool, 'i' i32, 'f' f32, 'l' a list of pointers
    std::string label;
};
constexpr size_t kListMax = 256;        // entries read per list
constexpr size_t kListExported = 64;    // entries of the whole list written per change
std::mutex g_watchM;
std::vector<Watch> g_watches;                                     // (g_watchM)
std::unordered_map<uint64_t, double> g_watchLast;                 // (g_watchM) (entity ^ field index) -> last value
std::unordered_map<uint64_t, std::vector<ListEntry>> g_listLast;  // (g_watchM) (entity ^ field index) -> last list
// Live instances of the watched classes. `direct`: seen created, so its destruction is sure to reach
// watchDestroyed (which waits for g_watchM): read with plain loads while g_watchM is held. Instances
// found by the scan (they existed before) are read with safe reads instead.
struct WatchedEnt {
    const Type* type;
    bool direct;
};
std::unordered_map<uintptr_t, WatchedEnt> g_watchEnts;            // (g_watchM)
std::vector<const Type*> g_watchKinds;                            // (g_watchM) each watch's class (null: unknown)
std::atomic<bool> g_haveWatches{false};                           // creations / destructions only look when set
double g_watchFoundAt = -10.0, g_watchReadAt = -10.0;
std::atomic<int> g_watchRate{0};       // readings a second; 0: every frame (the default: short-lived changes count)
// How long readings take (watches are read only while recording): the last one, and over the
// recording (reset by Clear) the count, total and worst. Shown in the menu and written in the
// export's header. Written on the render thread only.
std::atomic<float> g_watchMs{0.f};
std::atomic<uint64_t> g_watchReadings{0};
std::atomic<double> g_watchMsTotal{0.0};
std::atomic<float> g_watchMsWorst{0.f};

// Caller holds g_watchM, after g_watches changed.
void watchesChanged() {
    g_watchKinds.clear();
    for (const Watch& w : g_watches) g_watchKinds.push_back(registry().find(w.cls));
    g_watchEnts.clear();
    g_haveWatches = !g_watches.empty();
    g_watchFoundAt = -10.0; // look for instances now
}

bool watchedKind(const Type* ty) { // caller holds g_watchM
    for (const Type* k : g_watchKinds)
        if (k && registry().isKindOf(ty, k)) return true;
    return false;
}

// An address starts or stops being an entity: what the watches remember about it goes (a new entity
// there must not be compared with the old one's values). Caller holds g_watchM.
void forgetWatched(uintptr_t p) {
    for (size_t i = 0; i < g_watches.size(); ++i) {
        const uint64_t key = (static_cast<uint64_t>(p) << 8) ^ i;
        g_watchLast.erase(key);
        g_listLast.erase(key);
    }
}

// A watched value read from an entity: plain loads for one we saw created (alive while g_watchM is
// held), safe reads otherwise.
bool readEnt(uintptr_t addr, void* out, size_t size, bool direct) {
    if (direct) {
        memcpy(out, reinterpret_cast<const void*>(addr), size);
        return true;
    }
    return mem::read(addr, out, size);
}

// A list's entries (begin, end at p + off). False when it does not look like a vector of pointers.
// The buffer belongs to the game and can be freed at any time: always read safely.
bool readListAt(uintptr_t p, uint32_t off, bool direct, uintptr_t (&be)[2], std::vector<uintptr_t>& ptrs, size_t& total) {
    ptrs.clear();
    total = 0;
    if (!readEnt(p + off, be, sizeof(be), direct)) return false;
    const uintptr_t b = be[0], e = be[1];
    if (!b && !e) return true; // never allocated: empty
    if (e < b || (e - b) % 8 || (e - b) / 8 > 65536) return false;
    total = (e - b) / 8;
    ptrs.resize(std::min(total, kListMax));
    return ptrs.empty() || mem::read(b, ptrs.data(), ptrs.size() * 8);
}

std::vector<ListEntry> listEntries(const std::vector<uintptr_t>& ptrs, const std::vector<ListEntry>* old) {
    std::unordered_map<uintptr_t, const Type*> known; // types from the last reading (a removed entry may be gone)
    if (old)
        for (const ListEntry& le : *old) known[le.ptr] = le.type;
    std::vector<ListEntry> now;
    for (size_t i = 0; i < ptrs.size(); ++i) {
        ListEntry le;
        le.ptr = ptrs[i], le.index = static_cast<uint16_t>(i);
        auto k = known.find(ptrs[i]);
        le.type = k != known.end() ? k->second : (ptrs[i] ? registry().typeOf(mem::ProcessMemory(), ptrs[i]) : nullptr);
        now.push_back(le);
    }
    return now;
}

// From the creator hooks (any game thread), whether or not recording: a new entity of a watched class
// is watched from now on. While recording, its values at creation are the baseline (not recorded), so
// whatever happens before the next frame reads it (a spawner filled during a level load) shows as a
// change rather than hiding in a "first" reading.
void watchCreated(uintptr_t p) {
    if (!g_haveWatches.load(std::memory_order_relaxed)) return;
    const Type* ty = registry().typeOf(mem::ProcessMemory(), p);
    if (!ty) return;
    std::lock_guard<std::mutex> lock(g_watchM);
    if (!watchedKind(ty)) return;
    forgetWatched(p);
    g_watchEnts[p] = {ty, true};
    if (!g_recording.load(std::memory_order_relaxed) || g_watchKinds.size() != g_watches.size()) return;
    for (size_t i = 0; i < g_watches.size(); ++i) {
        const Watch& w = g_watches[i];
        if (!g_watchKinds[i] || !registry().isKindOf(ty, g_watchKinds[i])) continue;
        const uint64_t key = (static_cast<uint64_t>(p) << 8) ^ i;
        if (w.type == 'l') {
            uintptr_t be[2];
            std::vector<uintptr_t> ptrs;
            size_t total = 0;
            if (readListAt(p, w.off, true, be, ptrs, total)) g_listLast[key] = listEntries(ptrs, nullptr);
        } else {
            g_watchLast[key] = w.type == 'b' ? *reinterpret_cast<const uint8_t*>(p + w.off)
                             : w.type == 'f' ? *reinterpret_cast<const float*>(p + w.off)
                                             : *reinterpret_cast<const int32_t*>(p + w.off);
        }
    }
}

// From the destructor hooks (any game thread), before the entity goes. Waits while the render thread
// reads the watches (g_watchM), so nothing reads it after this returns.
void watchDestroyed(uintptr_t p) {
    if (!g_haveWatches.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_watchM);
    if (g_watchEnts.erase(p)) forgetWatched(p);
}

void addWatch(const std::string& cls, uint32_t off, char type, const std::string& label) {
    hookDestructors(); // watches drop destroyed entities through it (once; usually done at startup)
    std::lock_guard<std::mutex> lock(g_watchM);
    for (const Watch& w : g_watches)
        if (w.cls == cls && w.off == off) return;
    g_watches.push_back({cls, off, type, label.empty() ? cls + "+" + std::to_string(off) : label});
    watchesChanged();
}

// One list watch on one entity (caller holds g_watchM). Most lists are empty and unchanged: that
// costs one 16-byte read. Only when the result differs from the last reading is begin / end read
// again, to make sure the list was not being changed while it was read (if it was, the next frame
// reads it again). A change becomes a kList line in `out` (pushed after g_watchM is released).
void pollListLocked(uintptr_t p, const WatchedEnt& we, const Watch& w, uint64_t key, double t, std::vector<Event>& out) {
    static thread_local std::vector<uintptr_t> ptrs;
    uintptr_t be[2] = {};
    size_t total = 0;
    if (!readListAt(p, w.off, we.direct, be, ptrs, total)) return;
    auto it = g_listLast.find(key);
    const bool first = it == g_listLast.end();
    std::vector<ListEntry> old;
    if (!first) {
        const std::vector<ListEntry>& last = it->second;
        bool same = last.size() == ptrs.size();
        for (size_t k = 0; same && k < ptrs.size(); ++k) same = last[k].ptr == ptrs[k];
        if (same) return; // the usual case: nothing changed
        old = last;
    }
    uintptr_t be2[2] = {};
    if (!readEnt(p + w.off, be2, sizeof(be2), we.direct) || be2[0] != be[0] || be2[1] != be[1]) return; // moving
    std::vector<uintptr_t> oldPtrs;
    for (const ListEntry& le : old) oldPtrs.push_back(le.ptr);
    std::vector<ListEntry> now = listEntries(ptrs, first ? nullptr : &old);
    g_listLast[key] = now;
    Event ev;
    ev.t = t;
    ev.kind = kList;
    ev.thread = GetCurrentThreadId();
    ev.ptr = p, ev.type = we.type;
    ev.text = w.label;
    ev.fieldType = 'l', ev.fieldOff = w.off;
    ev.value[0] = static_cast<float>(total);
    ev.value[1] = static_cast<float>(old.size());
    ev.value[2] = first ? 1.f : 0.f;
    // Added and removed by pointer (a pointer twice in a list counts by how many times).
    std::unordered_map<uintptr_t, int> count;
    for (uintptr_t q : oldPtrs) ++count[q];
    std::vector<uintptr_t> commonNew, commonOld;
    for (const ListEntry& le : now) {
        auto c = count.find(le.ptr);
        if (c != count.end() && c->second > 0) {
            --c->second;
            commonNew.push_back(le.ptr);
        } else if (!first) {
            ListEntry a = le;
            a.what = 1;
            ev.list.push_back(a);
        }
    }
    count.clear();
    for (uintptr_t q : ptrs) ++count[q];
    for (const ListEntry& le : old) {
        auto c = count.find(le.ptr);
        if (c != count.end() && c->second > 0) {
            --c->second;
            commonOld.push_back(le.ptr);
        } else {
            ListEntry r = le;
            r.what = 2;
            ev.list.push_back(r);
        }
    }
    ev.reordered = !first && commonNew != commonOld; // the entries both readings share, in another order
    for (size_t i = 0; i < now.size() && i < kListExported; ++i) ev.list.push_back(now[i]);
    out.push_back(std::move(ev));
}

// Render thread, every frame while recording. True when it read the watches this time. The reading
// holds g_watchM throughout: a watched entity cannot finish being destroyed meanwhile (its destructor
// hook waits in watchDestroyed), which is what makes plain loads from it safe.
bool pollWatchesImpl() {
    if (!g_haveWatches.load(std::memory_order_relaxed)) return false;
    const double t = now();
    if (t - g_watchFoundAt > 2.0 || t < g_watchFoundAt) {
        // Instances that existed before the watch (new ones arrive through watchCreated), checked
        // alive here, once; read with safe reads, since their creation was not seen.
        g_watchFoundAt = t;
        std::vector<const Type*> kinds;
        {
            std::lock_guard<std::mutex> lock(g_watchM);
            kinds = g_watchKinds;
        }
        std::vector<std::pair<uintptr_t, const Type*>> found;
        for (const auto& [p, ty] : inspector::tracked()) {
            if (!ty) continue;
            for (const Type* k : kinds)
                if (k && registry().isKindOf(ty, k)) {
                    if (inspector::alive(p, ty)) found.push_back({p, ty});
                    break;
                }
        }
        std::lock_guard<std::mutex> lock(g_watchM);
        for (const auto& [p, ty] : found) g_watchEnts.emplace(p, WatchedEnt{ty, false});
    }
    const int rate = g_watchRate.load(std::memory_order_relaxed);
    if (rate > 0 && t - g_watchReadAt < 1.0 / rate && t >= g_watchReadAt) return false;
    g_watchReadAt = t;
    std::vector<Event> out;
    {
        std::lock_guard<std::mutex> lock(g_watchM);
        if (g_watchKinds.size() != g_watches.size()) return false;
        for (const auto& [p, we] : g_watchEnts) {
            for (size_t i = 0; i < g_watches.size(); ++i) {
                const Watch& w = g_watches[i];
                const Type* k = g_watchKinds[i];
                if (!k || !registry().isKindOf(we.type, k)) continue;
                const uint64_t key = (static_cast<uint64_t>(p) << 8) ^ i;
                if (w.type == 'l') {
                    pollListLocked(p, we, w, key, t, out);
                    continue;
                }
                double v = 0;
                if (w.type == 'b') {
                    uint8_t x = 0;
                    if (!readEnt(p + w.off, &x, 1, we.direct)) continue;
                    v = x;
                } else if (w.type == 'f') {
                    float x = 0;
                    if (!readEnt(p + w.off, &x, 4, we.direct)) continue;
                    v = x;
                } else {
                    int32_t x = 0;
                    if (!readEnt(p + w.off, &x, 4, we.direct)) continue;
                    v = x;
                }
                auto it = g_watchLast.find(key);
                const bool first = it == g_watchLast.end();
                const double old = first ? 0.0 : it->second;
                if (!first && old == v) continue;
                g_watchLast[key] = v;
                Event ev;
                ev.t = t;
                ev.kind = kField;
                ev.thread = GetCurrentThreadId();
                ev.ptr = p, ev.type = we.type;
                ev.text = w.label;
                ev.fieldType = w.type, ev.fieldOff = w.off;
                ev.value[0] = static_cast<float>(v);
                ev.value[1] = static_cast<float>(old);
                ev.value[2] = first ? 1.f : 0.f; // the first reading of this entity's field
                out.push_back(std::move(ev));
            }
        }
    }
    for (Event& ev : out) push(std::move(ev));
    return true;
}

// ---- Faith's movement ---------------------------------------------------------------------------
// Every frame while recording (render thread): the movement fields the game keeps outside entities
// (state, in air, wall counters; faith.yaml's PamMovement) and the player's jump action, as
// field lines when they change. Speed is left out: it changes every frame. The offsets in the lines
// are PamMovement's (ground +0x20 -> +0x17, state +0x28 -> ...); the jump action has none.
struct MoveField {
    const char* label;
    uint32_t off;
    char type;
};
constexpr MoveField kMoveFields[] = {
    {"movement state", 0x90, 'i'}, {"in air", 0x17, 'b'}, {"wall count", 0x18, 'i'},
    {"wall climbs", 0x38, 'i'},    {"wall runs", 0x3c, 'i'}, {"jump held", 0, 'b'},
    {"jump held by mods", 0, 'b'}, // gn::jump (a trainer's auto-bhop), which "jump held" does not see
    {"speed (measured)", 0, 'f'},  // from her position; a line when it moves by 0.1 m/s or more
    {"speed (game)", 0xd0, 'f'},   // gn_movement.speed (her velocity, movement ground +0xd0)
    {"speed (old trainer)", 0x438, 'f'}, // [[[0x1423e29b8] + 0x2378] + 0x10] + 0x438, the user's old speedometer
};
std::array<double, std::size(kMoveFields)> g_moveLast{};
bool g_moveRead = false; // g_moveLast holds a reading of this recording (render thread)

void pollMovement() {
    gn_movement m{};
    if (movementGet(&m) != GN_OK) return;
    const int jump = jumpHeld();
    const double v[] = {double(m.state), double(m.on_ground), double(m.wall_count), double(m.wall_climbs), double(m.wall_runs),
                        double(jump < 0 ? 0 : jump), double(jumpHeldByMods()), double(speedProbeSample()), double(m.speed), double(oldTrainerSpeed())};
    static const Type* faithType = registry().find("PamClientCharacterEntity");
    const uintptr_t faith = reinterpret_cast<uintptr_t>(localClientCharacter());
    const double t = now();
    for (size_t i = 0; i < std::size(kMoveFields); ++i) {
        if (g_moveRead && (kMoveFields[i].type == 'f' ? std::fabs(g_moveLast[i] - v[i]) < 0.1 : g_moveLast[i] == v[i])) continue;
        Event ev;
        ev.t = t;
        ev.kind = kField;
        ev.thread = GetCurrentThreadId();
        ev.ptr = faith, ev.type = faithType;
        ev.text = kMoveFields[i].label;
        ev.fieldType = kMoveFields[i].type, ev.fieldOff = kMoveFields[i].off;
        ev.value[0] = static_cast<float>(v[i]);
        ev.value[1] = static_cast<float>(g_moveLast[i]);
        ev.value[2] = g_moveRead ? 0.f : 1.f;
        push(std::move(ev));
        g_moveLast[i] = v[i];
    }
    g_moveRead = true;
}

void pollWatches() {
    const double t0 = now();
    if (!pollWatchesImpl()) return;
    const float ms = static_cast<float>((now() - t0) * 1000.0);
    g_watchMs.store(ms, std::memory_order_relaxed);
    g_watchReadings.fetch_add(1, std::memory_order_relaxed);
    g_watchMsTotal.store(g_watchMsTotal.load(std::memory_order_relaxed) + ms, std::memory_order_relaxed);
    if (ms > g_watchMsWorst.load(std::memory_order_relaxed)) g_watchMsWorst.store(ms, std::memory_order_relaxed);
}

// ---- sending an event to entities (research: does the game act on it?) ---------------------------
// Sent as the bus would (events::send in events.cpp, shared with mods' event_send). Recorded (kSent)
// so the reaction can be seen in the recording.

int sendEventToClass(const std::string& cls, uint32_t id) {
    const Type* k = registry().find(cls);
    if (!k) return -1;
    const auto sent = events::sendToClass(k, id); // delivered on each entity's realm thread at its next tick
    if (g_recording.load())
        for (const auto& [p, ty] : sent) {
            Event ev;
            ev.t = now();
            ev.kind = kSent;
            ev.thread = GetCurrentThreadId();
            ev.ptr = p, ev.type = ty;
            ev.eventId = id;
            push(std::move(ev));
        }
    GN_LOG("recorder: event %s (%08x) sent to %llu %s entities", eventName(id).c_str(), id, static_cast<unsigned long long>(sent.size()), cls.c_str());
    return static_cast<int>(sent.size());
}

// ---- tracing a class's virtual calls ------------------------------------------------------------
// Every slot of a class's vtable(s) is swapped (a pointer swap, through the hook table) for a small
// thunk of our own: it saves the argument registers (rcx, rdx, r8, r9, xmm0-3), calls traceCall
// with the slot's record, the object (rcx) and the return address, restores them and jumps to the
// original. So any signature passes through untouched (stack arguments are never moved). The thunks
// have unwind info (RtlAddFunctionTable), so stack walks through them work. A call is recorded at
// most kCallsPerSecond times a second per slot; the others are counted and the count goes with the
// next recorded one. Tracing a class stays hooked (as every hook); switching it off stops recording.
struct TraceSlot {
    const Type* cls = nullptr;
    uint32_t off = 0;               // vtable offset
    uint64_t fn = 0;                // the function it held (image VA)
    std::atomic<bool>* on = nullptr; // the class's switch
    std::atomic<uint32_t> skipped{0};
    std::atomic<int64_t> windowStart{0}; // QPC ticks
    std::atomic<uint32_t> inWindow{0};
};
struct TracedClass {
    std::string name;
    const Type* type = nullptr;
    std::atomic<bool> on{true};
    size_t vtables = 0, slots = 0, failed = 0;
};
constexpr size_t kThunkSize = 0xb0;       // code (0x95 bytes), padding, the original's address at kCell
constexpr size_t kCell = 0xa0;
constexpr size_t kMaxTraceSlots = 4096;
std::atomic<uint32_t> g_callsPerSecond{20};
std::mutex g_traceM;
std::deque<TracedClass> g_traced;        // (g_traceM) stable addresses
std::deque<TraceSlot> g_traceSlots;      // (g_traceM)
uint8_t* g_thunks = nullptr;             // kMaxTraceSlots thunks, RWX, ours
RUNTIME_FUNCTION* g_thunkFns = nullptr;
size_t g_thunksUsed = 0;                 // (g_traceM)

// The stack from the caller on (the thunk passes the caller's rsp as it will be after the return): the caller's
// frame and its callers, unwound from there with the image's unwind data. RtlCaptureStackBackTrace from inside
// traceCall gave nothing through the thunk.
void callerStack(uintptr_t ret, uintptr_t rsp, std::vector<uint64_t>& out) {
    if (!g_wantStacks.load(std::memory_order_relaxed)) return;
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    ctx.Rip = ret;
    ctx.Rsp = rsp;
    for (int i = 0; i < kStackFrames && ctx.Rip; ++i) {
        if (!mem::inImage(ctx.Rip)) break;
        out.push_back(ctx.Rip - mem::moduleBase() + kImageBase);
        DWORD64 base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!rf) { // a leaf: the return address is at rsp
            uintptr_t r = 0;
            if (!mem::read(static_cast<uintptr_t>(ctx.Rsp), r)) break;
            ctx.Rip = r;
            ctx.Rsp += 8;
            continue;
        }
        void* handler = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &handler, &frame, nullptr);
        if (ctx.Rsp < rsp || ctx.Rsp - rsp > 0x100000) break; // left this thread's stack: stop
    }
}

void traceCall(TraceSlot* s, uintptr_t self, uintptr_t ret, uintptr_t callerRsp) {
    if (!g_recording.load(std::memory_order_relaxed) || !s->on->load(std::memory_order_relaxed)) return;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const int64_t start = s->windowStart.load(std::memory_order_relaxed);
    if (c.QuadPart - start > g_freq.QuadPart) {
        s->windowStart.store(c.QuadPart, std::memory_order_relaxed);
        s->inWindow.store(0, std::memory_order_relaxed);
    }
    if (s->inWindow.fetch_add(1, std::memory_order_relaxed) >= g_callsPerSecond.load(std::memory_order_relaxed)) {
        s->skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        Event ev;
        ev.t = now();
        ev.kind = kCall;
        ev.thread = GetCurrentThreadId();
        ev.ptr = self;
        ev.type = s->cls;
        ev.fieldOff = s->off;
        ev.callFn = s->fn;
        ev.callFrom = mem::inImage(ret) ? mem::va(ret) : 0;
        ev.callSkipped = s->skipped.exchange(0, std::memory_order_relaxed);
        callerStack(ret, callerRsp, ev.stack);
        push(std::move(ev));
    } catch (...) { // recording never breaks the game's call
    }
}

// One thunk at `t` for record `rec`, jumping to *(t + kCell) (the original, written by the hook table).
void writeThunk(uint8_t* t, TraceSlot* rec) {
    static const uint8_t kHead[] = {
        0x48, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, // sub rsp,98h
        0x48, 0x89, 0x4C, 0x24, 0x20,             // mov [rsp+20h],rcx
        0x48, 0x89, 0x54, 0x24, 0x28,             // mov [rsp+28h],rdx
        0x4C, 0x89, 0x44, 0x24, 0x30,             // mov [rsp+30h],r8
        0x4C, 0x89, 0x4C, 0x24, 0x38,             // mov [rsp+38h],r9
        0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x40,       // movdqu [rsp+40h],xmm0
        0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x50,       // movdqu [rsp+50h],xmm1
        0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x60,       // movdqu [rsp+60h],xmm2
        0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x70,       // movdqu [rsp+70h],xmm3
        0x48, 0x89, 0xCA,                         // mov rdx,rcx (the object)
        0x4C, 0x8B, 0x84, 0x24, 0x98, 0x00, 0x00, 0x00, // mov r8,[rsp+98h] (the return address)
        0x4C, 0x8D, 0x8C, 0x24, 0xA0, 0x00, 0x00, 0x00, // lea r9,[rsp+0A0h] (the caller's rsp after the return)
        0x48, 0xB9};                              // mov rcx,imm64 (the record)
    static const uint8_t kMid[] = {0x48, 0xB8};  // mov rax,imm64 (traceCall)
    static const uint8_t kTail[] = {
        0xFF, 0xD0,                               // call rax
        0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x40,       // movdqu xmm0,[rsp+40h]
        0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x50,       // movdqu xmm1,[rsp+50h]
        0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x60,       // movdqu xmm2,[rsp+60h]
        0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x70,       // movdqu xmm3,[rsp+70h]
        0x48, 0x8B, 0x4C, 0x24, 0x20,             // mov rcx,[rsp+20h]
        0x48, 0x8B, 0x54, 0x24, 0x28,             // mov rdx,[rsp+28h]
        0x4C, 0x8B, 0x44, 0x24, 0x30,             // mov r8,[rsp+30h]
        0x4C, 0x8B, 0x4C, 0x24, 0x38,             // mov r9,[rsp+38h]
        0x48, 0x81, 0xC4, 0x98, 0x00, 0x00, 0x00, // add rsp,98h
        0xFF, 0x25};                              // jmp [rip+disp32]
    uint8_t* p = t;
    memcpy(p, kHead, sizeof(kHead)), p += sizeof(kHead);
    const uint64_t recAddr = reinterpret_cast<uint64_t>(rec), fnAddr = reinterpret_cast<uint64_t>(&traceCall);
    memcpy(p, &recAddr, 8), p += 8;
    memcpy(p, kMid, sizeof(kMid)), p += sizeof(kMid);
    memcpy(p, &fnAddr, 8), p += 8;
    memcpy(p, kTail, sizeof(kTail)), p += sizeof(kTail);
    const int32_t disp = static_cast<int32_t>((t + kCell) - (p + 4)); // the original's cell
    memcpy(p, &disp, 4), p += 4;
    memset(p, 0xCC, static_cast<size_t>((t + kCell) - p));
}

bool allocThunks() {
    if (g_thunks) return true;
    g_thunks = static_cast<uint8_t*>(VirtualAlloc(nullptr, kMaxTraceSlots * kThunkSize + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_thunks) return false;
    // Unwind info (one for all thunks, at the end): version 1, prolog 7 bytes, 2 code slots,
    // UWOP_ALLOC_LARGE (op 1, info 0) at offset 7 with size/8 = 0x13.
    uint8_t* ui = g_thunks + kMaxTraceSlots * kThunkSize;
    const uint8_t info[] = {0x01, 0x07, 0x02, 0x00, 0x07, 0x01, 0x13, 0x00};
    memcpy(ui, info, sizeof(info));
    g_thunkFns = new RUNTIME_FUNCTION[kMaxTraceSlots];
    for (size_t i = 0; i < kMaxTraceSlots; ++i) {
        g_thunkFns[i].BeginAddress = static_cast<DWORD>(i * kThunkSize);
        g_thunkFns[i].EndAddress = static_cast<DWORD>(i * kThunkSize + kCell);
        g_thunkFns[i].UnwindData = static_cast<DWORD>(kMaxTraceSlots * kThunkSize);
    }
    if (!RtlAddFunctionTable(g_thunkFns, static_cast<DWORD>(kMaxTraceSlots), reinterpret_cast<DWORD64>(g_thunks)))
        GN_LOG("recorder: thunk unwind info not registered; stacks through traced calls stop at them");
    return true;
}

// Executable code anywhere: the game's, or a hook's (GridNode's own detours, another mod's). A vtable slot
// already hooked holds one of those; the vtable ends at the first slot that holds anything else.
bool isCode(uintptr_t fn) {
    if (mem::inImage(fn)) return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!fn || !VirtualQuery(reinterpret_cast<void*>(fn), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// The vtable(s) of `t` (and, with `sub`, of its subclasses), each slot that holds game code.
std::string traceClass(const std::string& name, bool sub) {
    const Type* base = registry().find(name);
    if (!base || base->category != reflect::Category::Class) return "no class " + name;
    std::lock_guard<std::mutex> lock(g_traceM);
    for (TracedClass& c : g_traced)
        if (c.name == name) {
            c.on = true;
            return name + " is traced again";
        }
    if (!allocThunks()) return "no memory for the thunks";
    g_traced.emplace_back();
    TracedClass& tc = g_traced.back();
    tc.name = name;
    tc.type = base;
    std::vector<std::pair<uintptr_t, const Type*>> vts;
    for (const Type& t : registry().data().types) {
        if (t.category != reflect::Category::Class || !(&t == base || (sub && registry().isKindOf(&t, base)))) continue;
        for (uint64_t va : t.vtables) vts.push_back({static_cast<uintptr_t>(va - kImageBase) + mem::moduleBase(), &t});
    }
    std::sort(vts.begin(), vts.end());
    vts.erase(std::unique(vts.begin(), vts.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), vts.end());
    std::unordered_map<uint64_t, uint64_t> hooked; // slot -> the game's function (the first hook's "previous")
    for (const auto& [slotAddr, chain] : hooks().snapshot())
        if (!chain.empty()) hooked[slotAddr] = chain.front().previous;
    for (const auto& [vt, ty] : vts) {
        ++tc.vtables;
        for (uint32_t off = 0; off < 0x800; off += 8) {
            const uintptr_t fn = mem::readOr<uintptr_t>(vt + off);
            if (!isCode(fn)) break; // the end of the vtable
            // getType stays the game's: GridNode tells an object's class by the function in this slot
            // (Registry::typeOf), so a thunk there made traced objects unknown (dropped by the inspector,
            // skipped by watches). It is called all the time and says nothing anyway.
            if (off == 8u * gn::layout::kGetTypeSlot) continue;
            if (g_thunksUsed == kMaxTraceSlots) {
                ++tc.failed;
                continue;
            }
            g_traceSlots.emplace_back();
            TraceSlot& rec = g_traceSlots.back();
            rec.cls = ty;
            rec.off = off;
            uint64_t game = fn;
            if (!mem::inImage(fn)) // hooked already (by GridNode or another mod): the game's function is the first hook's "previous"
                if (const auto it = hooked.find(vt + off); it != hooked.end()) game = it->second;
            rec.fn = mem::inImage(static_cast<uintptr_t>(game)) ? mem::va(static_cast<uintptr_t>(game)) : 0;
            rec.on = &tc.on;
            uint8_t* t = g_thunks + g_thunksUsed * kThunkSize;
            writeThunk(t, &rec);
            std::string why;
            const auto r = hooks().install("recorder-trace", vt + off, 0, reinterpret_cast<uint64_t>(t), reinterpret_cast<uint64_t*>(t + kCell), &why);
            if (r == hook::Result::Ok) {
                ++g_thunksUsed;
                ++tc.slots;
            } else {
                g_traceSlots.pop_back();
                if (tc.failed++ < 3) GN_LOG("recorder: trace %s +0x%x: %s", ty->name.c_str(), off, why.c_str());
            }
        }
    }
    GN_LOG("recorder: tracing %s%s: %llu slots in %llu vtables (%llu failed)", name.c_str(), sub ? " and subclasses" : "",
           static_cast<unsigned long long>(tc.slots), static_cast<unsigned long long>(tc.vtables), static_cast<unsigned long long>(tc.failed));
    char b[160];
    snprintf(b, sizeof(b), "%s: %llu slots in %llu vtables traced", name.c_str(), static_cast<unsigned long long>(tc.slots),
             static_cast<unsigned long long>(tc.vtables));
    return b;
}

// ---- who called: stack frames named by the class whose vtable holds their function ---------------
// A frame's function is found in the executable's unwind table (RtlLookupFunctionEntry, following
// chained entries to the primary one); functions in reflected vtables are named "Class::vf+0xNN"
// (the first class seen; shared ones say how many share them). Built once, at the first export.
std::mutex g_ownerM;
std::unordered_map<uint64_t, std::string> g_fnOwner; // function start (image VA) -> name (g_ownerM)
bool g_ownersBuilt = false;

void buildOwners() {
    std::unordered_map<uint64_t, std::pair<std::string, int>> m;
    for (const Type& t : registry().data().types) {
        if (t.category != reflect::Category::Class) continue;
        for (uint64_t va : t.vtables) {
            const uintptr_t vt = static_cast<uintptr_t>(va - kImageBase) + mem::moduleBase();
            for (uint32_t slot = 0; slot < 0x400; slot += 8) {
                const uintptr_t fn = mem::readOr<uintptr_t>(vt + slot);
                if (!mem::inImage(fn)) break;
                char b[16];
                snprintf(b, sizeof(b), "::vf+0x%x", slot);
                auto [it, fresh] = m.emplace(mem::va(fn), std::make_pair(t.name + b, 1));
                if (!fresh) ++it->second.second;
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_ownerM);
    for (auto& [fn, v] : m)
        g_fnOwner[fn] = v.second > 1 ? v.first + " (shared by " + std::to_string(v.second) + ")" : v.first;
    g_ownersBuilt = true;
}

// The start of the function holding image VA `va` (0 when the unwind table has none).
uint64_t functionStart(uint64_t va) {
    DWORD64 base = 0;
    const DWORD64 addr = static_cast<DWORD64>(va - kImageBase) + mem::moduleBase();
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(addr, &base, nullptr);
    for (int hops = 0; rf && hops < 8; ++hops) { // chained unwind info: the primary entry
        const uint8_t* ui = reinterpret_cast<const uint8_t*>(base + rf->UnwindData);
        if (!mem::inImage(reinterpret_cast<uintptr_t>(ui)) || !((ui[0] >> 3) & 0x4)) break; // UNW_FLAG_CHAININFO
        const uint8_t codes = ui[2];
        rf = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(ui) + 4 + ((codes + 1) & ~1) * 2);
    }
    return rf ? static_cast<uint64_t>(rf->BeginAddress) + kImageBase : 0;
}

std::string frameOwner(uint64_t va) {
    const uint64_t fn = functionStart(va);
    if (!fn) return {};
    std::lock_guard<std::mutex> lock(g_ownerM);
    const auto it = g_fnOwner.find(fn);
    return it == g_fnOwner.end() ? std::string() : it->second;
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
    if (on && g_wantDestroyed.load()) hookDestructors(); // once; the inspector has usually done it already
    if (on) g_watchFoundAt = -10.0;                      // watches: look for existing instances straight away
    if (on) {
        std::lock_guard<std::mutex> lock(g_antM);
        g_antLast.clear(); // each recording starts with the values as they are
    }
    g_recording = on;
    if (on) messageTap(&onMessage);
    GN_LOG("recorder: %s", on ? "recording" : "paused");
}

void clearAll() {
    {
        std::lock_guard<std::mutex> lock(g_watchM);
        g_watchLast.clear(); // the next recording starts with the values as they are
        g_listLast.clear();
    }
    g_watchReadings = 0, g_watchMsTotal = 0.0, g_watchMsWorst = 0.f;
    std::lock_guard<std::mutex> lock(g_m);
    g_events.clear();
    g_objs.clear();
    g_objAt.clear();
    g_dropped = 0;
    g_rolledOff = 0;
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
                o += (i ? "," : "") + std::string(b) + hexAddr(ev.refs[i].ptr) + ",\"type\":" + jsonStr(ev.refs[i].type->name) +
                     (ev.refs[i].obj ? ",\"obj\":" + std::to_string(ev.refs[i].obj) : std::string()) + "}";
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
    case kDestroyed:
        o += ",\"kind\":\"destroyed\",\"type\":" + jsonStr(ev.type ? ev.type->name : "?") + ",\"ptr\":" + hexAddr(ev.ptr);
        if (!ev.blueprint.empty()) o += ",\"blueprint\":" + jsonStr(ev.blueprint);
        break;
    case kList: {
        char off[16];
        snprintf(off, sizeof(off), "0x%x", ev.fieldOff);
        o += ",\"kind\":\"list\",\"field\":" + jsonStr(ev.text) + ",\"offset\":\"" + off + "\",\"count\":" +
             std::to_string(static_cast<long long>(ev.value[0]));
        if (ev.value[2] != 0.f) o += ",\"first\":true";
        else o += ",\"old_count\":" + std::to_string(static_cast<long long>(ev.value[1]));
        if (ev.reordered) o += ",\"reordered\":true";
        auto entry = [](const ListEntry& le, bool withIndex) {
            std::string x = "{";
            if (withIndex) x += "\"index\":" + std::to_string(le.index) + ",";
            x += "\"ptr\":" + hexAddr(le.ptr) + ",\"type\":" + jsonStr(le.type ? le.type->name : "?");
            if (le.obj) x += ",\"obj\":" + std::to_string(le.obj);
            return x + "}";
        };
        const char* names[3] = {"items", "added", "removed"};
        for (uint8_t what = 0; what < 3; ++what) {
            std::string arr;
            for (const ListEntry& le : ev.list)
                if (le.what == what) arr += (arr.empty() ? "" : ",") + entry(le, what != 2);
            if (!arr.empty() || what == 0) o += ",\"" + std::string(names[what]) + "\":[" + arr + "]";
        }
        o += ",\"target\":" + jsonStr(ev.type ? ev.type->name : "?") + ",\"ptr\":" + hexAddr(ev.ptr);
        break;
    }
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
    case kField: {
        char off[16];
        snprintf(off, sizeof(off), "0x%x", ev.fieldOff);
        auto val = [&](float v) { return ev.fieldType == 'f' ? num(v) : std::to_string(static_cast<long long>(v)); };
        o += ",\"kind\":\"field\",\"field\":" + jsonStr(ev.text) + ",\"offset\":\"" + off + "\",\"type\":\"" + std::string(1, ev.fieldType) +
             "\",\"value\":" + val(ev.value[0]);
        if (ev.value[2] == 0.f) o += ",\"old\":" + val(ev.value[1]);
        else o += ",\"first\":true";
        o += ",\"target\":" + jsonStr(ev.type ? ev.type->name : "?") + ",\"ptr\":" + hexAddr(ev.ptr);
        break;
    }
    case kSent: {
        char id[16];
        snprintf(id, sizeof(id), "%08x", ev.eventId);
        o += ",\"kind\":\"sent\",\"event\":" + jsonStr(eventName(ev.eventId)) + ",\"event_id\":\"" + id + "\",\"target\":" +
             jsonStr(ev.type ? ev.type->name : "?") + ",\"ptr\":" + hexAddr(ev.ptr);
        break;
    }
    case kCall: {
        char off[16];
        snprintf(off, sizeof(off), "0x%x", ev.fieldOff);
        o += ",\"kind\":\"call\",\"class\":" + jsonStr(ev.type ? ev.type->name : "?") + ",\"slot\":\"" + off + "\",\"fn\":" + hexAddr(ev.callFn) +
             ",\"ptr\":" + hexAddr(ev.ptr);
        if (ev.callFrom) o += ",\"from\":" + hexAddr(ev.callFrom);
        if (ev.callSkipped) o += ",\"skipped_before\":" + std::to_string(ev.callSkipped);
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
    if (ev.obj) o += ",\"obj\":" + std::to_string(ev.obj);
    if (ev.havePos) o += ",\"pos\":[" + num(ev.pos[0]) + "," + num(ev.pos[1]) + "," + num(ev.pos[2]) + "]";
    if (!ev.stack.empty()) {
        o += ",\"stack\":[";
        for (size_t i = 0; i < ev.stack.size(); ++i) o += (i ? "," : "") + hexAddr(ev.stack[i]);
        o += "]";
        // Frames inside a reflected class's vtable function: which entity's code made this happen.
        std::string callers;
        for (uint64_t f : ev.stack) {
            const std::string who = frameOwner(f);
            if (who.empty()) continue;
            callers += std::string(callers.empty() ? "" : ",") + "{\"at\":" + hexAddr(f) + ",\"fn\":" + hexAddr(functionStart(f)) + ",\"in\":" + jsonStr(who) + "}";
        }
        if (!callers.empty()) o += ",\"callers\":[" + callers + "]";
    }
    return o + "}\n";
}

std::string objJson(uint32_t id, const Obj& o) {
    std::string x = "{\"kind\":\"object\",\"id\":" + std::to_string(id) + ",\"ptr\":" + hexAddr(o.ptr) + ",\"type\":" +
                    jsonStr(o.type ? o.type->name : "?");
    if (o.dataType) x += ",\"data\":" + jsonStr(o.dataType->name) + ",\"data_ptr\":" + hexAddr(o.data);
    if (!o.blueprint.empty()) x += ",\"blueprint\":" + jsonStr(o.blueprint);
    x += ",\"created\":" + (std::isnan(o.created) ? std::string("null") : num(o.created));
    x += ",\"destroyed\":" + (std::isnan(o.destroyed) ? std::string("null") : num(o.destroyed));
    x += ",\"first_seen\":" + num(o.firstSeen);
    return x + "}\n";
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
    std::vector<std::pair<uint32_t, Obj>> objs; // the ones the exported events point at
    {
        std::lock_guard<std::mutex> lock(g_m);
        events.assign(g_events.begin(), g_events.end());
        dropped = g_dropped;
        level = g_level;
        std::vector<bool> used(g_objs.size() + 1, false);
        auto use = [&](uint32_t id) {
            if (id && id < used.size()) used[id] = true;
        };
        for (const Event& ev : g_events) {
            use(ev.obj);
            for (const Ref& r : ev.refs) use(r.obj);
            for (const ListEntry& le : ev.list) use(le.obj);
        }
        for (uint32_t id = 1; id < used.size(); ++id)
            if (used[id]) objs.push_back({id, g_objs[id - 1]});
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::string file = std::string(stamp) + (name.empty() ? "" : "-" + name) + ".jsonl";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    const std::wstring path = g_dir + std::wstring(file.begin(), file.end());
    g_lastExport = "recordings\\" + file + " (writing...)";
    // How long watch readings took over this recording (none: no watches, or none read).
    std::string watchTiming;
    if (const uint64_t n = g_watchReadings.load()) {
        char b[160];
        snprintf(b, sizeof(b), ",\"watch_readings\":{\"count\":%llu,\"average_ms\":%.4f,\"worst_ms\":%.4f,\"rate\":%d}",
                 static_cast<unsigned long long>(n), g_watchMsTotal.load() / static_cast<double>(n), g_watchMsWorst.load(), g_watchRate.load());
        watchTiming = b;
    }
    if (g_rolling.load()) watchTiming += ",\"rolling_seconds\":" + std::to_string(g_rollSeconds.load()); // only the window was kept
    std::thread([events = std::move(events), objs = std::move(objs), dropped, level, path, file, watchTiming] {
        if (!g_ownersBuilt) buildOwners(); // for "callers" (once)
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) {
            GN_LOG("recorder: cannot write recordings\\%s", file.c_str());
            return;
        }
        std::string head = "{\"kind\":\"header\",\"format\":\"gridnode-recording\",\"version\":1,\"gridnode\":\"" GRIDNODE_VERSION_STRING
                           "\",\"image_base\":\"0x140000000\",\"level\":" +
                           jsonStr(level) + ",\"events\":" + std::to_string(events.size()) + ",\"objects\":" + std::to_string(objs.size()) +
                           ",\"dropped_oldest\":" + std::to_string(dropped) + watchTiming + "}\n";
        fwrite(head.data(), 1, head.size(), f);
        for (const Event& ev : events) {
            const std::string line = toJson(ev);
            fwrite(line.data(), 1, line.size(), f);
        }
        for (const auto& [id, o] : objs) { // after the events: every object they point at, by id
            const std::string line = objJson(id, o);
            fwrite(line.data(), 1, line.size(), f);
        }
        // Then the start of the function holding each recorded frame (from the unwind table), so a
        // reader can name frames by function (summarize.py: "0x143234f21 (Name+0x51)") without a dump.
        std::vector<uint64_t> frames;
        for (const Event& ev : events) frames.insert(frames.end(), ev.stack.begin(), ev.stack.end());
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        std::string fl = "{\"kind\":\"frames\",\"starts\":{";
        bool firstFrame = true;
        for (uint64_t fr : frames)
            if (const uint64_t st = functionStart(fr)) {
                fl += (firstFrame ? "" : ",") + hexAddr(fr) + ":" + hexAddr(st);
                firstFrame = false;
            }
        fl += "}}\n";
        fwrite(fl.data(), 1, fl.size(), f);
        fclose(f);
        GN_LOG("recorder: exported %llu events to recordings\\%s", static_cast<unsigned long long>(events.size()), file.c_str());
    }).detach();
    g_lastExport = "recordings\\" + file;
}

// ---- choosing a class ----------------------------------------------------------------------------
// A class field with a "..." button that opens a searchable list, so nobody needs to know the names:
// the classes alive right now (with how many), the classes this recording has lines about (with how
// many), or every class. Hovering a row shows its base classes. Enter picks the first row shown.
struct PickRow {
    const Type* type;
    int count;
};
struct Picker {
    char search[64] = "";
    int source = 0;          // 0 alive now, 1 in this recording, 2 every class
    int builtSource = -1;
    bool withVtable = true;
    double builtAt = -10.0;
    std::vector<PickRow> rows;
};

std::vector<PickRow> pickRows(int source) {
    std::unordered_map<const Type*, int> n;
    if (source == 0) {
        for (const auto& [p, t] : inspector::tracked())
            if (t) ++n[t];
    } else if (source == 1) {
        std::lock_guard<std::mutex> lock(g_m);
        for (const Event& ev : g_events) {
            if (ev.type) ++n[ev.type];
            for (const Ref& r : ev.refs)
                if (r.type) ++n[r.type];
        }
    } else {
        for (const Type& t : registry().data().types)
            if (t.category == reflect::Category::Class && !t.name.empty()) n[&t] = 0;
    }
    std::vector<PickRow> rows;
    rows.reserve(n.size());
    for (const auto& [t, c] : n)
        if (t->category == reflect::Category::Class) rows.push_back({t, c});
    std::sort(rows.begin(), rows.end(), [](const PickRow& a, const PickRow& b) {
        return a.count != b.count ? a.count > b.count : a.type->name < b.type->name;
    });
    return rows;
}

std::string baseChain(const Type* t) {
    std::string o;
    for (int i = 0; t && i < 16; ++i, t = registry().super(t)) o += (i ? "\n  <- " : "") + t->name;
    return o;
}

bool containsNoCase(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        size_t j = 0;
        while (j < n && tolower(static_cast<unsigned char>(hay[i + j])) == tolower(static_cast<unsigned char>(needle[j]))) ++j;
        if (j == n) return true;
    }
    return false;
}

// The class field and its list. Returns true when a class was picked from the list (written to buf).
bool classPicker(const char* id, char* buf, size_t size, const char* hint, float width, bool needVtable) {
    static std::unordered_map<std::string, Picker> pickers; // render thread
    Picker& pk = pickers[id];
    bool picked = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint("##cls", hint, buf, size);
    ImGui::SameLine(0, 2);
    if (ImGui::Button("...")) {
        pk.withVtable = needVtable;
        pk.builtSource = -1;
        ImGui::OpenPopup("pick");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose a class from a list");
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fs * 28, fs * 26), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("pick")) {
        int src = pk.source;
        ImGui::RadioButton("Alive now", &src, 0);
        ImGui::SameLine();
        ImGui::RadioButton("In this recording", &src, 1);
        ImGui::SameLine();
        ImGui::RadioButton("All classes", &src, 2);
        pk.source = src;
        const double t = now();
        if (pk.builtSource != src || (src == 0 && (t - pk.builtAt > 1.0 || t < pk.builtAt))) {
            pk.rows = pickRows(src);
            pk.builtSource = src;
            pk.builtAt = t;
        }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(fs * 16);
        const bool enter = ImGui::InputTextWithHint("##search", "search: part of a name", pk.search, sizeof(pk.search),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::Checkbox("with a vtable", &pk.withVtable);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only classes GridNode knows a vtable for (needed to trace calls)");
        std::vector<const PickRow*> shown;
        for (const PickRow& r : pk.rows)
            if ((!pk.withVtable || !r.type->vtables.empty()) && containsNoCase(r.type->name, pk.search)) shown.push_back(&r);
        if (src == 1) ImGui::TextDisabled("%d classes (count: lines about them)", static_cast<int>(shown.size()));
        else if (src == 0) ImGui::TextDisabled("%d classes (count: live entities)", static_cast<int>(shown.size()));
        else ImGui::TextDisabled("%d classes", static_cast<int>(shown.size()));
        auto choose = [&](const Type* ty) {
            snprintf(buf, size, "%s", ty->name.c_str());
            picked = true;
            ImGui::CloseCurrentPopup();
        };
        if (enter && !shown.empty()) choose(shown.front()->type);
        if (ImGui::BeginChild("rows", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(shown.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                    const PickRow& r = *shown[static_cast<size_t>(i)];
                    char label[200];
                    if (src == 2) snprintf(label, sizeof(label), "%s##%d", r.type->name.c_str(), i);
                    else snprintf(label, sizeof(label), "%-48s %7d##%d", r.type->name.c_str(), r.count, i);
                    if (ImGui::Selectable(label)) choose(r.type);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", baseChain(r.type).c_str());
                }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return picked;
}

// ---- keys -------------------------------------------------------------------------------------
constexpr int kKeyRecord = VK_F8, kKeyMarker = VK_HOME, kKeySaveRolling = VK_END;
bool g_keyWas[3] = {};
char g_markerName[64] = "marker";
int g_markerCount = 0;

// Exports the rolling window as it is (recording goes on), with a marker where it was saved.
void saveRolling() {
    marker("saved");
    exportAll("last" + std::to_string(g_rollSeconds.load()) + "s");
}

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
    events::setTap(&onEvent);
}

void onDestroyed(void* entity) { onDestroyedImpl(entity); }

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
    const int keys[3] = {kKeyRecord, kKeyMarker, kKeySaveRolling};
    bool pressed[3] = {};
    for (int i = 0; i < 3; ++i) {
        const bool down = focused && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        pressed[i] = down && !g_keyWas[i];
        g_keyWas[i] = down;
    }
    if (pressed[0]) setRecording(!g_recording.load());
    if (pressed[1] && g_recording.load()) marker(nextMarkerName());
    if (pressed[2] && g_rolling.load()) saveRolling();
    if (g_autoStart.exchange(false)) {
        setRecording(true);
        marker("event " + g_autoEventName);
        GN_LOG("recorder: started by the event %s", g_autoEventName.c_str());
    }
    if (g_recording.load()) pollWatches();
    if (g_recording.load() && g_wantMovement.load()) pollMovement(), hookPredict();
    else g_moveRead = false; // the next recording (or switching it on) starts with a first reading
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
    ImGui::TextDisabled("%llu events over %.1f s%s%s", static_cast<unsigned long long>(count), span, dropped ? " (oldest dropped)" : "",
                        g_rolling.load() ? " (rolling)" : "");

    bool m = g_wantMessages.load(), c = g_wantCreated.load(), s = g_wantStacks.load();
    if (ImGui::Checkbox("Messages", &m)) g_wantMessages = m;
    ImGui::SameLine();
    if (ImGui::Checkbox("Entities created", &c)) g_wantCreated = c;
    ImGui::SameLine();
    bool d = g_wantDestroyed.load();
    if (ImGui::Checkbox("destroyed", &d)) {
        g_wantDestroyed = d;
        if (d && rec) hookDestructors();
    }
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
    bool mv = g_wantMovement.load();
    if (ImGui::Checkbox("Faith's movement", &mv)) g_wantMovement = mv;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every frame: Faith's movement state, in air, wall count, wall climbs, wall runs, your jump input and\n"
                          "jumps pressed by mods (gn::jump),\n"
                          "recorded when they change (field lines on PamClientCharacterEntity). For movement research such as bhops;\n"
                          "off by default.");

    ImGui::SeparatorText("Markers");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputText("##marker", g_markerName, sizeof(g_markerName));
    ImGui::SameLine();
    ImGui::BeginDisabled(!rec);
    if (ImGui::Button("Drop marker (Home)")) marker(nextMarkerName());
    ImGui::EndDisabled();
    ImGui::TextDisabled("Press Home in game right when the thing happens (got the KSEC, the door opened).");

    ImGui::SeparatorText("After the fact");
    bool roll = g_rolling.load();
    if (ImGui::Checkbox("Keep only the last", &roll)) {
        g_rolling = roll;
        if (roll && !rec) setRecording(true); // it records all the time; End saves
    }
    ImGui::SameLine();
    int secs = g_rollSeconds.load();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::SliderInt("seconds (End saves them)", &secs, 5, 120)) g_rollSeconds = secs;
    ImGui::BeginDisabled(!roll || !count);
    if (ImGui::Button("Save the last seconds (End)")) saveRolling();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Records all the time but keeps only that window: when something interesting happens,\n"
                        "press End and the seconds before it are exported (as <time>-last<N>s), with a \"saved\" marker.");
    static char autoEvent[64] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::InputTextWithHint("##autoevent", "event name or 0x<hash>", autoEvent, sizeof(autoEvent));
    ImGui::SameLine();
    const bool armed = g_autoEventId.load() != 0;
    if (armed) {
        if (ImGui::Button("Disarm")) g_autoEventId = 0;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "waiting for %s", g_autoEventName.c_str());
    } else {
        ImGui::BeginDisabled(rec || !autoEvent[0]);
        if (ImGui::Button("Start recording on this event")) {
            const std::string n = autoEvent;
            char* end = nullptr;
            const unsigned long v = n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X') ? strtoul(n.c_str() + 2, &end, 16) : 0;
            g_autoEventName = n;
            hookEvents(); // the event is seen through the same wrappers, so they go in now
            g_autoEventId = end && *end == 0 ? static_cast<uint32_t>(v) : djb2(n);
            GN_LOG("recorder: armed: recording starts on the event %s (%08x)", n.c_str(), g_autoEventId.load());
        }
        ImGui::EndDisabled();
    }

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

    ImGui::SeparatorText("Watch fields");
    ImGui::TextDisabled("Fields of live entities of a class, recorded each time they change.\n"
                        "A list is a vector of pointers at the offset (begin, end): adds, removes and order changes.");
    {
        static char cls[64] = "", off[16] = "", label[48] = "";
        static int type = 1;
        classPicker("watch", cls, sizeof(cls), "class", ImGui::GetFontSize() * 11, false);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        ImGui::InputTextWithHint("##woff", "+0x134", off, sizeof(off));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        ImGui::Combo("##wtype", &type, "byte\0i32\0f32\0list\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        ImGui::InputTextWithHint("##wlabel", "label", label, sizeof(label));
        ImGui::SameLine();
        const char* o = off;
        while (*o == '+' || *o == ' ') ++o;
        char* end = nullptr;
        const unsigned long offset = strtoul(o, &end, 0);
        const bool ok = registry().find(cls) && end && end != o && *end == 0 && offset < 0x10000;
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Watch")) addWatch(cls, static_cast<uint32_t>(offset), "bifl"[type], label);
        ImGui::EndDisabled();
        if (ImGui::Button("Add the city alert manager's state")) { // docs/research/city-alert.md
            addWatch("PamServerCityAlertManager", 0x175, 'b', "alert active");
            addWatch("PamServerCityAlertManager", 0x134, 'i', "alert level");
            addWatch("PamServerCityAlertManager", 0x13c, 'i', "AI killed");
            addWatch("PamServerCityAlertManager", 0x130, 'i', "cameras seeing");
            addWatch("PamServerCityAlertManager", 0x140, 'i', "hotspots active");
        }
        ImGui::SameLine();
        if (ImGui::Button("Add the spawners' lists")) { // docs/research/replay-ghosts.md; db/annotations/spawn.yaml
            addWatch("ServerSpawnEntity", 0x148, 'l', "spawned (server)");
            addWatch("ClientSpawnEntity", 0xb0, 'l', "spawned (client)");
        }
        std::vector<Watch> ws;
        size_t found;
        {
            std::lock_guard<std::mutex> lock(g_watchM);
            ws = g_watches;
            found = g_watchEnts.size();
        }
        for (size_t i = 0; i < ws.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton((ws[i].label + "  x").c_str())) {
                std::lock_guard<std::mutex> lock(g_watchM);
                if (i < g_watches.size()) g_watches.erase(g_watches.begin() + static_cast<long>(i));
                g_watchLast.clear(); // keys are by watch index, which just shifted
                g_listLast.clear();
                watchesChanged();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s +0x%x (%c)", ws[i].cls.c_str(), ws[i].off, ws[i].type);
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (!ws.empty()) ImGui::TextDisabled("(%d live)", static_cast<int>(found));
        int rate = g_watchRate.load();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::SliderInt("readings a second", &rate, 0, 60, rate == 0 ? "every frame" : "%d")) g_watchRate = rate;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How often watched fields and lists are read. Every frame catches changes that last only a\n"
                              "moment (a ghost that lives 0.1 s); fewer readings cost less with many entities watched.");
        if (!ws.empty()) {
            ImGui::SameLine();
            if (const uint64_t n = g_watchReadings.load())
                ImGui::TextDisabled("reading takes %.3f ms (average %.3f, worst %.3f over %llu)", g_watchMs.load(),
                                    g_watchMsTotal.load() / static_cast<double>(n), g_watchMsWorst.load(), static_cast<unsigned long long>(n));
            else
                ImGui::TextDisabled("reading time: shown once recording (watches are read only then)");
        }
    }

    ImGui::SeparatorText("Trace a class's calls");
    ImGui::TextDisabled("Records each call to the class's virtual functions: which object, which slot, who called (while recording).\n"
                        "\"...\" lists classes to choose from: alive now, in this recording, or all.");
    {
        static char cls[64] = "";
        static bool sub = false;
        static std::string said;
        classPicker("trace", cls, sizeof(cls), "class, e.g. ClientCharacterSpawnEntity", ImGui::GetFontSize() * 14, true);
        ImGui::SameLine();
        ImGui::Checkbox("and subclasses", &sub);
        ImGui::SameLine();
        // The name as typed, else the one class whose name matches it ignoring case.
        std::string want = cls;
        if (!want.empty() && !registry().find(want)) {
            auto lower = [](std::string v) {
                for (char& ch : v) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                return v;
            };
            const std::string l = lower(want);
            for (const Type& t : registry().data().types)
                if (t.category == reflect::Category::Class && lower(t.name) == l) {
                    want = t.name;
                    break;
                }
        }
        const bool known = !want.empty() && registry().find(want);
        ImGui::BeginDisabled(!known);
        if (ImGui::Button("Trace")) said = traceClass(want, sub);
        ImGui::EndDisabled();
        if (cls[0] && !known) {
            // Up to five classes whose name contains what was typed.
            std::string hint;
            int n = 0;
            for (const Type& t : registry().data().types) {
                if (t.category != reflect::Category::Class || t.vtables.empty()) continue;
                std::string a2 = t.name, b2 = cls;
                for (char& ch : a2) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                for (char& ch : b2) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                if (a2.find(b2) == std::string::npos) continue;
                hint += (n ? ", " : "") + t.name;
                if (++n == 5) break;
            }
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "No class named \"%s\".%s%s", cls, n ? " Did you mean: " : "", hint.c_str());
        }
        int cps = static_cast<int>(g_callsPerSecond.load());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::SliderInt("calls a second per slot", &cps, 1, 200)) g_callsPerSecond = static_cast<uint32_t>(cps);
        if (!said.empty()) ImGui::TextDisabled("%s", said.c_str());
        std::lock_guard<std::mutex> lock(g_traceM);
        for (TracedClass& c : g_traced) {
            bool on = c.on.load();
            ImGui::PushID(&c);
            if (ImGui::Checkbox(c.name.c_str(), &on)) c.on = on;
            ImGui::SameLine();
            ImGui::TextDisabled("%llu slots", static_cast<unsigned long long>(c.slots));
            ImGui::PopID();
        }
    }

    ImGui::SeparatorText("Send an event (research)");
    ImGui::TextDisabled("Delivers an event to every live entity of a class, as a connection would. Use with care.");
    {
        static char cls[64] = "PamServerCityAlertManager", evt[48] = "";
        classPicker("send", cls, sizeof(cls), "class", ImGui::GetFontSize() * 11, false);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        ImGui::InputTextWithHint("##sevt", "name or 0x6bb5e497", evt, sizeof(evt));
        ImGui::SameLine();
        uint32_t id = 0;
        bool ok = evt[0] != 0 && registry().find(cls);
        if (ok) {
            if (evt[0] == '0' && (evt[1] == 'x' || evt[1] == 'X')) {
                char* end = nullptr;
                id = static_cast<uint32_t>(strtoul(evt + 2, &end, 16));
                ok = end && *end == 0;
            } else {
                id = djb2(evt);
            }
        }
        ImGui::BeginDisabled(!ok);
        static std::string said;
        if (ImGui::Button("Send")) {
            const int n = sendEventToClass(cls, id);
            char b[160];
            snprintf(b, sizeof(b), "%s (%08x) sent to %d %s", eventName(id).c_str(), id, n < 0 ? 0 : n, cls);
            said = b;
        }
        ImGui::EndDisabled();
        if (!said.empty()) ImGui::TextDisabled("%s", said.c_str());
    }

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
                lite.fieldType = src.fieldType, lite.fieldOff = src.fieldOff, lite.ptr = src.ptr, lite.obj = src.obj;
                lite.reordered = src.reordered;
                if (src.kind == kList)
                    for (const ListEntry& le : src.list)
                        if (le.what != 0) lite.list.push_back(le);
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
            case kField: {
                char val[48];
                if (ev.fieldType == 'f') snprintf(val, sizeof(val), "%g (was %g)", ev.value[0], ev.value[1]);
                else snprintf(val, sizeof(val), "%lld (was %lld)", static_cast<long long>(ev.value[0]), static_cast<long long>(ev.value[1]));
                snprintf(line, sizeof(line), "%9.3f  field  %s = %s  -> %s", ev.t, ev.text.c_str(), ev.value[2] != 0.f ? "" : val,
                         ev.type ? ev.type->name.c_str() : "?");
                if (ev.value[2] != 0.f)
                    snprintf(line, sizeof(line), "%9.3f  field  %s = %g (first reading)  -> %s", ev.t, ev.text.c_str(), ev.value[0],
                             ev.type ? ev.type->name.c_str() : "?");
                ImGui::TextColored(ImVec4(1.f, 1.f, 0.5f, 1.f), "%s", line);
                break;
            }
            case kCall:
                snprintf(line, sizeof(line), "%9.3f  call   %s::vf+0x%x", ev.t, ev.type ? ev.type->name.c_str() : "?", ev.fieldOff);
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.f, 1.f), "%s", line);
                break;
            case kSent:
                snprintf(line, sizeof(line), "%9.3f  SENT   %s -> %s", ev.t, eventName(ev.eventId).c_str(), ev.type ? ev.type->name.c_str() : "?");
                ImGui::TextColored(ImVec4(1.f, 0.5f, 0.3f, 1.f), "%s", line);
                break;
            case kDestroyed:
                snprintf(line, sizeof(line), "%9.3f  gone   %s%s%s", ev.t, ev.type ? ev.type->name.c_str() : "?", ev.blueprint.empty() ? "" : "  ",
                         ev.blueprint.c_str());
                ImGui::TextDisabled("%s", line);
                break;
            case kList: {
                std::string what;
                for (const ListEntry& le : ev.list)
                    if (what.size() < 120) what += std::string(le.what == 1 ? " +" : " -") + (le.type ? le.type->name : "?");
                if (ev.reordered) what += " (order changed)";
                if (ev.value[2] != 0.f) what = " (first reading)";
                snprintf(line, sizeof(line), "%9.3f  list   %s: %lld entries%s  -> %s", ev.t, ev.text.c_str(), static_cast<long long>(ev.value[0]),
                         what.c_str(), ev.type ? ev.type->name.c_str() : "?");
                ImGui::TextColored(ImVec4(1.f, 1.f, 0.5f, 1.f), "%s", line);
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
