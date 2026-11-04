// Sounds mods play at a place (gn_api sound_*): a voice of the game's sound system, as a SoundEntity
// makes one (SoundEntity_onEvent 0x14301a170): the sound system's slot 8 makes a voice from a sound
// asset, the voice's slot 13 places it (its 3D state; none for a sound heard the same everywhere),
// slot 0 sends it an event (a name hash: the asset's graph events), slot 9 lets it go. From the
// cinematic tools' research (Research_Sound_System_2026-10-24) and the decompiler, 2026-11-03.
// Client thread (the sound system is the client's).

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/build.hpp"
#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

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
    const uintptr_t system = mem::readOr<uintptr_t>(mem::rva(e::global::g_soundSystem));
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

} // namespace gn::host
