// The game's own landing prediction for Faith (gn_api::landing_prediction), read where the game
// makes it. ANT's PositionPredictionProcessorAsset (vtable 0x141aabb98) predicts, every tick while a
// character is in the air, when and where it will hit the ground: its update (+0x20, 0x142f28e20)
// runs the trajectory against the world (ant_predictTrajectoryHit, 0x142f3b9a0) and writes the
// results through game-state writers ((binding + 0x20)->vtable +8: writer, state, value): hit height
// (asset +0x20), ticks to hit (+0x28, 60ths of a second) and soft landing (+0x30). GridNode wraps
// that update and the write of the two writer classes those outputs use (pointer swaps, chained with
// the Recorder's wrappers on the same slots), and keeps what is written for Faith's game state.
// On the tick before a landing the prediction read 0.74-1.54 in testing (docs/progress.md,
// 2026-10-30). The wrappers go in on the first call, so a game where no mod asks pays nothing.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "gridnode/log.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/reflect/layout.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

// The three slots (db/annotations/ant.yaml), each checked to hold the expected function before it is
// wrapped (the writers were taken as found until 2026-11-03).
namespace eh = gn::engine::hook;
constexpr uint64_t kFloatWriterVtable = eh::AntFloatWriter_write.slot - 0x8, kBoolWriterVtable = eh::AntBoolWriter_write.slot - 0x8;
constexpr uint32_t kHitHeight = 0x20, kTicksToHit = 0x28, kSoftLanding = 0x30, kBindingWriter = 0x20;
// Faith's animated entity (a ClientAntAnimatableEntity) and its game state, as the Recorder finds a
// state from its entity: +0x158 -> +0x10, then +0x40. Her +0xd0 leads to her animation, but in game it
// held vtable 0x141b06828, not the entity's own (0x141b06770): an object inside it, at an offset the
// game's stripped type info does not give. So the entity is found as the one on her bus (+0x20):
// the parts of a blueprint share one bus.
constexpr uint32_t kCharacterAnimatable = 0xd0, kEntityBus = 0x20;

uint64_t g_predictOrig = 0, g_floatOrig = 0, g_boolOrig = 0;
thread_local uintptr_t t_asset = 0; // the prediction being written on this thread

std::atomic<float> g_ticks{0.f}, g_height{0.f};
std::atomic<int> g_soft{0};
std::atomic<uint32_t> g_count{0};
std::atomic<uint64_t> g_at{0}; // GetTickCount64 at the last prediction for Faith
std::mutex g_hookM;
int g_hooked = 0; // (g_hookM) 0 not tried, 1 in, -1 could not
uintptr_t stateOfEntity(uintptr_t anim) {
    const uintptr_t holder = mem::readOr<uintptr_t>(anim + 0x158);
    const uintptr_t inner = holder ? mem::readOr<uintptr_t>(holder + 0x10) : 0;
    return inner ? inner + 0x40 : 0;
}

std::mutex g_faithM;
uintptr_t g_faithChar = 0;           // (g_faithM)
uint64_t g_faithAt = 0;              // (g_faithM) GetTickCount64 of the last search
std::atomic<uintptr_t> g_faithState{0}; // read by the wrappers on the animation threads

// Found again when Faith changes (a respawn, a new level), and while not found at most once a second.
// Her +0xd0 leads to her animation: it is the entity itself, or (as seen in game) an object inside it,
// found by walking back to the entity's type, or among the tracked entities. Failing those, the
// entity on her bus. When none fits, the evidence goes to the log.
uintptr_t findFaithState(uintptr_t c) {
    const reflect::Type* animType = registry().find("ClientAntAnimatableEntity");
    if (!animType) return 0;
    const uintptr_t direct = mem::readOr<uintptr_t>(c + kCharacterAnimatable);
    if (const reflect::Type* t = direct ? registry().typeOf(mem::ProcessMemory(), direct) : nullptr; t && registry().isKindOf(t, animType)) {
        GN_LOG("landing: Faith's animated entity is her +0x%x (%s)", kCharacterAnimatable, t->name.c_str());
        return stateOfEntity(direct);
    }
    // +0xd0 points into the entity (0x30 in, 2026-10-30): walk back to the object it is part of, by its
    // type. Needs no list of created entities, so it also works when GridNode was injected after the
    // level loaded (then nothing is tracked: a friend's log, 2026-10-30).
    for (uint32_t back = 8; direct && back <= 0x100; back += 8) {
        const uintptr_t p = direct - back;
        const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), p);
        if (!t || !registry().isKindOf(t, animType)) continue;
        GN_LOG("landing: Faith's animated entity is the %s her +0x%x points into (+0x%x)", t->name.c_str(), kCharacterAnimatable, back);
        return stateOfEntity(p);
    }
    const uintptr_t bus = mem::readOr<uintptr_t>(c + kEntityBus);
    uintptr_t below = 0, belowBus = 0, onBus = 0;
    size_t candidates = 0;
    for (const auto& [p, ty] : inspector::trackedOf(animType)) { // not a copy of every entity, once a second
        if (!ty) continue;
        ++candidates;
        const uint32_t size = ty->size ? ty->size : 0x760;
        if (direct >= p && direct < p + size) {
            GN_LOG("landing: Faith's animated entity is the %s her +0x%x points into (+0x%llx), state 0x%llx", ty->name.c_str(),
                   kCharacterAnimatable, static_cast<unsigned long long>(direct - p), static_cast<unsigned long long>(stateOfEntity(p)));
            return stateOfEntity(p);
        }
        if (p <= direct && p > below) below = p, belowBus = mem::readOr<uintptr_t>(p + kEntityBus);
        if (bus && mem::readOr<uintptr_t>(p + kEntityBus) == bus) onBus = p;
    }
    if (onBus) {
        GN_LOG("landing: Faith's animated entity is the one on her bus (0x%llx)", static_cast<unsigned long long>(onBus));
        return stateOfEntity(onBus);
    }
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 3)
        GN_LOG("landing: Faith's animated entity not found: Faith 0x%llx, +0x%x 0x%llx (vtable 0x%llx), bus 0x%llx; %llu "
               "ClientAntAnimatableEntity tracked, nearest below +0x%x: 0x%llx (bus 0x%llx)",
               static_cast<unsigned long long>(c), kCharacterAnimatable, static_cast<unsigned long long>(direct),
               static_cast<unsigned long long>(direct ? mem::va(mem::readOr<uintptr_t>(direct)) : 0), static_cast<unsigned long long>(bus),
               static_cast<unsigned long long>(candidates), kCharacterAnimatable, static_cast<unsigned long long>(below),
               static_cast<unsigned long long>(belowBus));
    return 0;
}

// On the caller's thread (landing_prediction), never in the wrappers: the search walks every tracked
// entity, which the animation threads should not wait for.
void refreshFaithState() {
    const uintptr_t c = reinterpret_cast<uintptr_t>(localClientCharacter());
    std::lock_guard<std::mutex> lock(g_faithM);
    if (!c) {
        g_faithChar = 0, g_faithState = 0;
        return;
    }
    if (c == g_faithChar && g_faithState.load()) return;
    const uint64_t now = GetTickCount64();
    if (c == g_faithChar && now - g_faithAt < 1000) return;
    g_faithChar = c, g_faithAt = now;
    g_faithState = findFaithState(c);
}

uintptr_t faithAntState() { return g_faithState.load(std::memory_order_relaxed); }

// Which output of the prediction on this thread a writer is, or 0.
uint32_t outputOf(uintptr_t writer) {
    const uintptr_t asset = t_asset;
    if (!asset) return 0;
    for (uint32_t off : {kTicksToHit, kHitHeight, kSoftLanding}) {
        const uintptr_t binding = mem::readOr<uintptr_t>(asset + off);
        if (binding && binding + kBindingWriter == writer) return off;
    }
    return 0;
}

void predictDetour(void* asset, void* ctx, void* a3, void* a4) {
    t_asset = reinterpret_cast<uintptr_t>(asset);
    reinterpret_cast<void (*)(void*, void*, void*, void*)>(g_predictOrig)(asset, ctx, a3, a4);
    t_asset = 0;
}

void floatDetour(void* w, void* state, float v) {
    if (t_asset) { // only while a prediction is being written: every other ANT float write costs one check
        const uint32_t off = outputOf(reinterpret_cast<uintptr_t>(w));
        if (off && reinterpret_cast<uintptr_t>(state) == faithAntState()) {
            if (off == kHitHeight) g_height = v;
            if (off == kTicksToHit) {
                g_ticks = v;
                g_at = GetTickCount64();
                g_count.fetch_add(1);
            }
        }
    }
    reinterpret_cast<void (*)(void*, void*, float)>(g_floatOrig)(w, state, v);
}

void boolDetour(void* w, void* state, uint64_t v) {
    if (t_asset && outputOf(reinterpret_cast<uintptr_t>(w)) == kSoftLanding && reinterpret_cast<uintptr_t>(state) == faithAntState())
        g_soft = static_cast<uint8_t>(v) ? 1 : 0;
    reinterpret_cast<void (*)(void*, void*, uint64_t)>(g_boolOrig)(w, state, v);
}

bool install(const char* what, uint64_t slotVa, uint64_t expectedVa, uint64_t detour, uint64_t* orig) {
    std::string why;
    const auto r = hooks().install("gridnode", mem::rva(slotVa), expectedVa ? mem::rva(expectedVa) : 0, detour, orig, &why);
    if (r != hook::Result::Ok) GN_LOG("landing: %s not wrapped: %s", what, why.c_str());
    return r == hook::Result::Ok;
}

bool ensureHooked() {
    std::lock_guard<std::mutex> lock(g_hookM);
    if (g_hooked) return g_hooked > 0;
    // The writers first, so the prediction's own wrapper never runs without them.
    const bool ok = install("the float game-state writer", eh::AntFloatWriter_write.slot, eh::AntFloatWriter_write.original,
                            reinterpret_cast<uint64_t>(&floatDetour), &g_floatOrig) &&
                    install("the bool game-state writer", eh::AntBoolWriter_write.slot, eh::AntBoolWriter_write.original,
                            reinterpret_cast<uint64_t>(&boolDetour), &g_boolOrig) &&
                    install("the landing prediction", eh::PositionPrediction_update.slot, eh::PositionPrediction_update.original,
                            reinterpret_cast<uint64_t>(&predictDetour), &g_predictOrig);
    g_hooked = ok ? 1 : -1;
    GN_LOG("landing: %s", ok ? "the game's landing prediction is read (PositionPredictionProcessorAsset +0x20)" : "not available");
    return ok;
}

} // namespace

int landingPrediction(gn_landing* out) {
    if (!out) return GN_ERR_ARGUMENT;
    *out = gn_landing{};
    if (!ensureHooked()) return GN_ERR_UNAVAILABLE;
    refreshFaithState();
    const uint32_t n = g_count.load();
    if (!n) return GN_ERR_UNAVAILABLE; // no prediction for Faith yet (she has not been in the air)
    out->ticks_to_hit = g_ticks.load();
    out->hit_height = g_height.load();
    out->soft_landing = g_soft.load();
    out->count = n;
    const uint64_t age = GetTickCount64() - g_at.load();
    out->age_ms = static_cast<uint32_t>(age > 0xffffffffull ? 0xffffffffull : age);
    return GN_OK;
}

} // namespace gn::host
