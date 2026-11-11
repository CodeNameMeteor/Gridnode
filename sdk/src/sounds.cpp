// Sounds mods play at a place (gn_api sound_*): a voice of the game's sound system, as a SoundEntity
// makes one (SoundEntity_onEvent 0x14301a170): the sound system's slot 8 makes a voice from a sound
// asset, the voice's slot 13 places it (its 3D state; none for a sound heard the same everywhere),
// slot 0 sends it an event (a name hash: the asset's graph events), slot 9 lets it go. From the
// cinematic tools' research (Research_Sound_System_2026-10-24) and the decompiler, 2026-11-03.
// Voices: client thread (the sound system is the client's). The mixer's groups (bottom section, bus_*): any
// thread; the gain step detour runs on the mixer's thread, the offsets are published by the client tick.

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <intrin.h>

#include "gridnode/audio_mix.hpp"
#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

// unverified: g_soundSystem (sound_play, API 19): a hypothesis in db/annotations; confirm in Ghidra.

namespace gn::host {
namespace {

namespace e = gn::engine;
namespace c = e::constant;

struct Voice {
    uint32_t handle;
    std::string mod;
    uintptr_t owner; // an address in the code that played it (its mod)
    uintptr_t voice;
    void* asset; // held through assetFind until the voice goes
};
std::mutex g_m;
std::vector<Voice> g_voices; // (g_m)
uint32_t g_next = 1;         // (g_m)

uint32_t nameHash(const char* s) {
    uint32_t h = 5381;
    for (; *s; ++s) h = (h * 33) ^ static_cast<unsigned char>(*s);
    return h;
}

template <class Fn> Fn slot(uintptr_t object, uint32_t offset) {
    const uintptr_t vt = mem::readOr<uintptr_t>(object);
    const uintptr_t fn = vt ? mem::readOr<uintptr_t>(vt + offset) : 0;
    return mem::inImage(fn) ? reinterpret_cast<Fn>(fn) : nullptr;
}

// An event of the asset: SoundPatchAsset's DefaultStartEvent / DefaultStopEvent (an AudioGraphEvent,
// its name hash at +0x14), as reflected.
uint32_t defaultEvent(uintptr_t asset, const char* field) {
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), asset);
    reflect::FieldInfo fi;
    if (!t || !registry().findField(t, field, fi)) return 0;
    const uintptr_t ev = mem::readOr<uintptr_t>(asset + fi.field->offset);
    return ev ? mem::readOr<uint32_t>(ev + 0x14) : 0;
}

bool place(uintptr_t voice, const float* pos) {
    const auto set = slot<void (*)(uintptr_t, const gn_transform*)>(voice, c::Voice_vf_setTransform);
    if (!set) return false;
    alignas(16) gn_transform t{};
    t.right[0] = t.up[1] = t.forward[2] = 1.f;
    t.trans[0] = pos[0], t.trans[1] = pos[1], t.trans[2] = pos[2], t.trans[3] = 1.f;
    set(voice, &t);
    return true;
}

void releaseVoice(const Voice& v) {
    if (const auto rel = slot<void (*)(uintptr_t)>(v.voice, c::Voice_vf_release)) rel(v.voice);
    assetRelease(v.asset);
}

bool finite3(const float* p) { return p && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }
} // namespace

int soundPlay(const char* mod, const char* path, const char* event, const float* pos, uint32_t* out, const void* caller) {
    if (!path || !out || (pos && !finite3(pos))) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    if (!build::codeMatches("SoundSystem_createVoice")) return GN_ERR_UNAVAILABLE;
    const uintptr_t system = mem::readOr<uintptr_t>(mem::rva(e::global::unverified::g_soundSystem));
    if (!system) return GN_ERR_UNAVAILABLE;
    void* asset = assetFind(path, nullptr);
    if (!asset) return GN_ERR_NOT_FOUND;
    const uintptr_t a = reinterpret_cast<uintptr_t>(asset);
    const uint32_t id = event && *event ? nameHash(event) : defaultEvent(a, "DefaultStartEvent");
    const auto create = slot<uintptr_t (*)(uintptr_t, uintptr_t)>(system, c::SoundSystem_vf_createVoice);
    const uintptr_t voice = create ? create(system, a) : 0;
    if (!voice) {
        assetRelease(asset);
        GN_LOG("[%s] sound_play %s: the sound system made no voice (not a sound asset?)", mod ? mod : "?", path);
        return GN_ERR_UNAVAILABLE;
    }
    if (pos) place(voice, pos);
    if (id)
        if (const auto send = slot<void (*)(uintptr_t, uint32_t)>(voice, c::Voice_vf_event)) send(voice, id);
    std::lock_guard<std::mutex> lock(g_m);
    g_voices.push_back({g_next, mod ? mod : "?", reinterpret_cast<uintptr_t>(caller), voice, asset});
    *out = g_next++;
    static int said = 0;
    if (said++ < 10) GN_LOG("[%s] sound_play %s: voice %llx, event %08x%s", mod ? mod : "?", path, static_cast<unsigned long long>(voice), id,
                            id ? "" : " (none: the asset has no default start event)");
    return GN_OK;
}

int soundEvent(uint32_t sound, const char* event) {
    if (!event || !*event) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    std::lock_guard<std::mutex> lock(g_m);
    for (const Voice& v : g_voices)
        if (v.handle == sound) {
            const auto send = slot<void (*)(uintptr_t, uint32_t)>(v.voice, c::Voice_vf_event);
            if (!send) return GN_ERR_UNAVAILABLE;
            send(v.voice, nameHash(event));
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

int soundMove(uint32_t sound, const float* pos) {
    if (!finite3(pos)) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    std::lock_guard<std::mutex> lock(g_m);
    for (const Voice& v : g_voices)
        if (v.handle == sound) return place(v.voice, pos) ? GN_OK : GN_ERR_UNAVAILABLE;
    return GN_ERR_NOT_FOUND;
}

int soundPlaying(uint32_t sound) {
    std::lock_guard<std::mutex> lock(g_m);
    for (const Voice& v : g_voices)
        if (v.handle == sound) {
            const auto playing = slot<bool (*)(uintptr_t)>(v.voice, c::Voice_vf_isPlaying);
            return playing && playing(v.voice) ? 1 : 0;
        }
    return -GN_ERR_NOT_FOUND;
}

int soundStop(uint32_t sound) {
    if (!isClientThread()) return GN_ERR_THREAD;
    std::lock_guard<std::mutex> lock(g_m);
    for (auto it = g_voices.begin(); it != g_voices.end(); ++it)
        if (it->handle == sound) {
            releaseVoice(*it);
            g_voices.erase(it);
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

// A mod unloaded (or every one, an empty range: GridNode unloading): its voices let go, on the
// client thread at its next tick.
void forgetSounds(const CodeRange& r) {
    const bool all = r.begin == 0 && r.end == 0;
    std::vector<Voice> out;
    {
        std::lock_guard<std::mutex> lock(g_m);
        for (auto it = g_voices.begin(); it != g_voices.end();)
            if (all || r.has(reinterpret_cast<void*>(it->owner))) {
                out.push_back(*it);
                it = g_voices.erase(it);
            } else {
                ++it;
            }
    }
    if (!out.empty())
        client().post([out] {
            for (const Voice& v : out) releaseVoice(v);
        });
}

// ---- the mixer's groups (gn_api bus_*; API 35, stable) ----------------------------------------------
// The game rebuilds a group's gain inputs on every update (snapshots rewrite them), so nothing written into a group
// sticks. Instead the property step of the gain processor (vtable slot 5, +0x28, the game's own call per group)
// is wrapped: after the original, the group's offset in dB is added to its final value (block 0, add mode), and
// children read their parent's final. The game's own state is never saved or written, so taking the offset away
// gives the exact game value back. Layout: db/annotations/audio-mix.yaml (MixerManager, MixerGroupInstance);
// game-confirmed in claim-lab, rounds 8-9 (2026-11-10). Groups are named by their MixGroup asset's Name.
namespace {

// Only MixerManager_updateProperty (0x143001260) is hooked: the gain processor's vtable 0x141ac3840 uses it (game log,
// round 9). The other vtable's slot 5 (0x143001710) is unread, so it is not accepted.
constexpr uintptr_t kMxProcessors = 0x1a80, kMxGroups = 0x1a88, kMxStride = 0x1a98, kMxCount = 0x1aa0;
constexpr uint32_t kGroupStride = 0x198;
constexpr uintptr_t kInstAsset = 0x00, kInstFlags = 0x18, kInstBlock = 0x1c; // property 0 (gain) at +0x1c
constexpr uintptr_t kAssetName = 0x10;                                       // MixGroup: char* Name
constexpr uintptr_t kPropStepSlot = 0x28;                                    // vtable slot 5 of the property processor
constexpr uintptr_t kBlockMode = 0x48;                                       // accumulate mode, 0 = add

audio::MixOffsets g_mix;
std::mutex g_mixM;                          // install and publish; never taken in the detour
bool g_stepHooked = false;                  // (g_mixM)
bool g_mixActive = false;                   // (g_mixM) some group has an offset published
uint64_t g_stepOrig = 0;                    // written by HookTable::install before the slot is swapped
std::atomic<uint8_t*> g_mixBuf{nullptr};    // the group buffer, for the detour
std::atomic<uint32_t> g_mixCount{0};
std::atomic<float> g_mixTotal[audio::kGroups]; // dB added to each group's final (published by the tick)
std::atomic<uint32_t> g_mixBadMode{0};

using StepFn = void (*)(void*, uint8_t*, uint8_t*, void*, float);

// Runs on the mixer's thread (not verified which): atomics only, no allocation, no logging, no locks.
void stepDetour(void* proc, uint8_t* inst, uint8_t* block, void* parentBlock, float dt) {
    reinterpret_cast<StepFn>(g_stepOrig)(proc, inst, block, parentBlock, dt);
    if (unloaded()) return;
    uint8_t* buf = g_mixBuf.load(std::memory_order_relaxed);
    if (!buf || inst < buf || block != inst + kInstBlock) return; // another property
    const uint64_t off64 = static_cast<uint64_t>(inst - buf);
    // g_mixBuf can be stale after the mixer reallocates the group table (a level change) until the next publish; that
    // is harmless: only pointers are compared, nothing outside `block` is dereferenced, and publish runs every tick
    // while an offset is active.
    if (off64 % kGroupStride != 0) return; // not a group of this buffer
    const uint64_t g = off64 / kGroupStride;
    if (g >= audio::kGroups || g > g_mixCount.load(std::memory_order_relaxed)) return;
    const float off = g_mixTotal[g].load(std::memory_order_relaxed);
    if (off == 0.f) return;
    const uint32_t mode = *reinterpret_cast<uint32_t*>(block + kBlockMode);
    if (mode != 0) {
        g_mixBadMode.store(mode, std::memory_order_relaxed);
        return;
    }
    *reinterpret_cast<float*>(block) += off;
}

uintptr_t mixer() { return mem::readOr<uintptr_t>(mem::rva(e::global::g_mixerManager)); }

// The group buffer and count when the table looks as verified (stride 0x198), else null.
uint8_t* mixerGroups(uint32_t& count) {
    const uintptr_t m = mixer();
    count = 0;
    if (!m) return nullptr;
    const uintptr_t buf = mem::readOr<uintptr_t>(m + kMxGroups);
    const uint32_t stride = mem::readOr<uint32_t>(m + kMxStride);
    const uint32_t n = mem::readOr<uint32_t>(m + kMxCount);
    if (!buf || stride != kGroupStride || n == 0 || n >= audio::kGroups) return nullptr;
    count = n;
    return reinterpret_cast<uint8_t*>(buf);
}

std::string groupName(const uint8_t* inst) {
    const uintptr_t asset = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(inst) + kInstAsset);
    const uintptr_t s = asset ? mem::readOr<uintptr_t>(asset + kAssetName) : 0;
    std::string out;
    for (int i = 0; s && i < 64; ++i) {
        char c = 0;
        if (!mem::read(s + i, c) || !c) break;
        out.push_back(c);
    }
    return out;
}

bool sameName(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// "master" is the group 'All' (the root of every asset group): the game's own master (#0) is never recomputed, so
// the step is never called for it. The others are the player's option groups.
const char* busAlias(const char* bus) {
    static const char* const map[][2] = {{"master", "All"}, {"music", "Music"}, {"sfx", "Game"},
                                         {"dialogue", "VO"}, {"ambience", "Ambiences"}, {"ui", "UI"}};
    for (const auto& m : map)
        if (sameName(m[0], bus)) return m[1];
    return bus;
}

// The group's index for a bus name. GN_ERR_UNAVAILABLE: no mixer table yet; GN_ERR_NOT_FOUND: no such group.
int findGroup(const char* bus, uint8_t*& buf, int& group) {
    uint32_t count = 0;
    buf = mixerGroups(count);
    if (!buf) return GN_ERR_UNAVAILABLE;
    const char* want = busAlias(bus);
    for (uint32_t i = 1; i <= count; ++i)
        if (sameName(groupName(buf + static_cast<uint64_t>(i) * kGroupStride), want)) {
            group = static_cast<int>(i);
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
}

// The hook goes into the gain processor's vtable (processors[0]); a pointer swap through the hook table.
bool ensureStepHook(std::string& why) {
    std::lock_guard<std::mutex> lock(g_mixM);
    if (g_stepHooked) return true;
    const uintptr_t m = mixer();
    const uintptr_t arr = m ? mem::readOr<uintptr_t>(m + kMxProcessors) : 0; // a pointer to the array of processor pointers
    const uintptr_t proc = arr ? mem::readOr<uintptr_t>(arr) : 0;
    const uintptr_t vt = proc ? mem::readOr<uintptr_t>(proc) : 0;
    if (!mem::inImage(vt)) {
        why = "no processor yet";
        return false;
    }
    const uintptr_t slot = vt + kPropStepSlot;
    // GN_ERR_UNAVAILABLE on an unknown build: the wrapped function's bytes must match, and the slot must still hold the
    // game's function (through the hook table when someone hooked it already: its first entry keeps what the game had).
    if (!build::codeMatches("MixerManager_updateProperty")) {
        why = "the build's mixer code is not the one known (MixerManager_updateProperty)";
        return false;
    }
    uint64_t base = mem::readOr<uint64_t>(slot);
    for (const auto& s : hooks().snapshot())
        if (s.first == slot && !s.second.empty()) base = s.second.front().previous;
    if (mem::va(base) != e::function::MixerManager_updateProperty) {
        why = "the processor's vtable slot holds an unknown function";
        return false;
    }
    if (!mem::patchableSlot(slot)) {
        why = "the processor's vtable slot is not patchable";
        return false;
    }
    if (hooks().install("gridnode", slot, 0, reinterpret_cast<uint64_t>(&stepDetour), &g_stepOrig, &why) != hook::Result::Ok) return false;
    g_stepHooked = true;
    GN_LOG("audio: group gain hook in (vtable slot 0x%llx)", static_cast<unsigned long long>(mem::va(slot)));
    return true;
}

// Publishes the offsets (dt moves the ducks on) and makes the game recompute the groups that changed: the dirty bit
// 0x08 on the group, which the update turns into 0x20 so the children follow. Client tick, unload, mod unload.
void publish(float dt) {
    std::lock_guard<std::mutex> lock(g_mixM);
    if (!g_mix.any() && !g_mixActive) return;
    float out[audio::kGroups];
    const std::vector<int> changed = g_mix.step(dt, out);
    uint32_t count = 0;
    uint8_t* buf = mixerGroups(count);
    g_mixBuf.store(buf, std::memory_order_relaxed);
    g_mixCount.store(count, std::memory_order_relaxed);
    bool active = false;
    for (int g = 0; g < audio::kGroups; ++g) {
        g_mixTotal[g].store(out[g], std::memory_order_relaxed);
        active = active || out[g] != 0.f;
    }
    g_mixActive = active;
    for (const int g : changed) {
        if (!buf || static_cast<uint32_t>(g) > count) continue;
        const uintptr_t flags = reinterpret_cast<uintptr_t>(buf) + static_cast<uint64_t>(g) * kGroupStride + kInstFlags;
        uint8_t f = 0;
        // Atomic: the mixer's update changes other bits of this byte (0x02..0x20) and a plain read-modify-write
        // could lose one (0x04, "snapshot went away", would leave a stale target). The address was read first.
        if (!mem::read(flags, f)) continue;
#if defined(_MSC_VER)
        _InterlockedOr8(reinterpret_cast<volatile char*>(flags), 0x08);
#else
        __atomic_fetch_or(reinterpret_cast<volatile uint8_t*>(flags), uint8_t{0x08}, __ATOMIC_SEQ_CST); // MinGW has no _InterlockedOr8
#endif
    }
}

int prepare(const char* mod, const char* bus, int& group) {
    if (!mod || !bus || !*bus) return GN_ERR_ARGUMENT;
    uint8_t* buf = nullptr;
    if (const int r = findGroup(bus, buf, group); r != GN_OK) return r;
    std::string why;
    if (!ensureStepHook(why)) {
        GN_LOG("[%s] audio: no group gain hook: %s", mod, why.c_str());
        return GN_ERR_UNAVAILABLE;
    }
    return GN_OK;
}
} // namespace

int busVolumeGet(const char* bus, float* out) {
    if (!bus || !out) return GN_ERR_ARGUMENT;
    uint8_t* buf = nullptr;
    int g = 0;
    if (const int r = findGroup(bus, buf, g); r != GN_OK) return r;
    float db = 0.f;
    if (!mem::read(reinterpret_cast<uintptr_t>(buf) + static_cast<uint64_t>(g) * kGroupStride + kInstBlock, db) || !std::isfinite(db))
        return GN_ERR_UNAVAILABLE;
    *out = audio::dbToGain(db);
    return GN_OK;
}

int busVolumeSet(const char* mod, const char* bus, float gain, const void* caller) {
    if (!std::isfinite(gain)) return GN_ERR_ARGUMENT;
    int g = 0;
    if (const int r = prepare(mod, bus, g); r != GN_OK) return r;
    g_mix.set(mod, reinterpret_cast<uintptr_t>(caller), g, gain);
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 40) GN_LOG("[%s] bus_volume_set %s (group %d) %.3f = %.1f dB", mod, bus, g, gain, audio::gainToDb(gain));
    return GN_OK;
}

int busDuck(const char* mod, const char* bus, float gain, float seconds, const void* caller) {
    if (!std::isfinite(gain) || !std::isfinite(seconds) || seconds < 0.f) return GN_ERR_ARGUMENT;
    int g = 0;
    if (const int r = prepare(mod, bus, g); r != GN_OK) return r;
    g_mix.duck(mod, reinterpret_cast<uintptr_t>(caller), g, gain, seconds);
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 40) GN_LOG("[%s] bus_duck %s (group %d) to %.3f for %.2f s", mod, bus, g, gain, seconds);
    return GN_OK;
}

void audioMixTick(float dt) {
    publish(dt);
    static uint32_t said = 0;
    if (const uint32_t m = g_mixBadMode.exchange(0); m && said++ < 5)
        GN_LOG("audio: a group's gain accumulate mode is %u, not 0 (add): its offset is not applied", m);
}

// A mod's code unloads (an empty range: every mod): its offsets and ducks go and the game recomputes at once.
void forgetAudioMix(const CodeRange& r) {
    const bool all = r.begin == 0 && r.end == 0;
    const std::vector<int> gone = g_mix.forget([&](uintptr_t tag) { return all || r.has(reinterpret_cast<void*>(tag)); });
    if (!gone.empty()) GN_LOG("audio: %llu group offset(s) of an unloaded mod dropped; the game recomputes them", static_cast<unsigned long long>(gone.size()));
    publish(0.f);
}

} // namespace gn::host
