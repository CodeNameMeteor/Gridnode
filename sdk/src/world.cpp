// World API: the local characters, transforms, the game's teleport, components, and level events.
// Offsets and functions are from db/annotations (gridnode/engine.hpp); everything read here is
// read fault tolerantly, and game functions are only called on the thread of their realm.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

namespace e = gn::engine;

std::atomic<void*> g_clientCharacter{nullptr}; // as of the last client tick

const reflect::Type* typeNamed(const char* name) {
    static std::mutex m;
    static std::vector<std::pair<std::string, const reflect::Type*>> cache;
    std::lock_guard<std::mutex> lock(m);
    for (const auto& c : cache)
        if (c.first == name) return c.second;
    const reflect::Type* t = registry().find(name);
    cache.emplace_back(name, t);
    return t;
}

bool kindOf(const void* object, const char* className) {
    const reflect::Type* base = typeNamed(className);
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), reinterpret_cast<uint64_t>(object));
    return t && base && registry().isKindOf(t, base);
}

bool isGameCode(uintptr_t fn) {
    if (!mem::inImage(fn)) return false;
    for (const mem::Section& s : mem::imageSections())
        if (s.region.code && fn >= s.region.begin && fn < s.region.end) return true;
    return false;
}

bool finite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1.0e6f) return false;
    return true;
}

// ServerCharacterEntity_teleport reads its argument with aligned loads and starts with
// "mov rdi, rcx; lea rbx, [rcx+410h]" 24 bytes in (checked once, as MECMP does).
bool teleportFunctionOk() {
    static const int ok = [] {
        static const uint8_t kExpect[] = {0x48, 0x89, 0xCF, 0x48, 0x8D, 0x99, 0x10, 0x04, 0x00, 0x00};
        uint8_t got[sizeof(kExpect)] = {};
        const bool same = mem::read(mem::rva(e::function::ServerCharacterEntity_teleport) + 24, got, sizeof(got)) &&
                          memcmp(got, kExpect, sizeof(got)) == 0;
        if (!same) GN_LOG("world: ServerCharacterEntity_teleport does not look as expected; teleport is off");
        return same ? 1 : 0;
    }();
    return ok == 1;
}

// ---- levels ----------------------------------------------------------------------------------
struct LevelListener {
    std::string mod;
    gn_level_fn fn;
    void* user;
};
std::mutex g_levelM;
std::vector<LevelListener> g_levelListeners; // guarded by g_levelM
std::string g_level;                         // the level the listeners were last told about (g_levelM)

// Server characters seen being created (a fallback for localServerCharacter when the player
// manager cannot be read). Few: Faith, AI, ghosts.
std::mutex g_charM;
std::vector<uintptr_t> g_serverChars; // guarded by g_charM

void onServerCharacter(void* entity, void*, void*) {
    std::lock_guard<std::mutex> lock(g_charM);
    if (g_serverChars.size() >= 4096) g_serverChars.erase(g_serverChars.begin(), g_serverChars.begin() + 1024);
    g_serverChars.push_back(reinterpret_cast<uintptr_t>(entity));
}

void* fromCreatedCharacters() {
    std::lock_guard<std::mutex> lock(g_charM);
    for (size_t i = g_serverChars.size(); i-- > 0;) {
        const uintptr_t c = g_serverChars[i];
        if (!kindOf(reinterpret_cast<void*>(c), "ServerCharacterEntity")) {
            g_serverChars.erase(g_serverChars.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const uintptr_t p = mem::readOr<uintptr_t>(c + e::cls::ServerCharacterEntity::off::player);
        if (p && mem::readOr<uint8_t>(p + e::st::ServerPlayer::off::isHuman)) return reinterpret_cast<void*>(c);
    }
    return nullptr;
}

void callLevel(const std::vector<LevelListener>& ls, const std::string& level, bool loaded) {
    for (const LevelListener& l : ls) l.fn(level.c_str(), loaded ? 1 : 0, l.user);
}

} // namespace

// Client tick (ticks.cpp): remember Faith for the other threads.
void worldClientTick() {
    g_clientCharacter.store(reinterpret_cast<void* (*)()>(mem::rva(e::function::LocalClientCharacter))(),
                            std::memory_order_relaxed);
}

void* localClientCharacter() {
    if (isClientThread()) return reinterpret_cast<void* (*)()>(mem::rva(e::function::LocalClientCharacter))();
    return g_clientCharacter.load(std::memory_order_relaxed);
}

void worldStart() { onCreated("gridnode", "ServerCharacterEntity", &onServerCharacter, nullptr); }

namespace {
// MessageManager -> server player manager (+0x68) -> players (+0x268/+0x270). The first human
// player whose character (+0x1478) is a ServerCharacterEntity that points back at the player.
void* fromPlayerManager() {
    using namespace e::st;
    const uintptr_t mm = mem::readOr<uintptr_t>(mem::rva(e::global::MessageManager));
    const uintptr_t pm = mm ? mem::readOr<uintptr_t>(mm + MessageManager::off::serverPlayerManager) : 0;
    if (!pm) return nullptr;
    uintptr_t range[2] = {};
    if (!mem::read(pm + ServerPlayerManager::off::playersBegin, range, sizeof(range))) return nullptr;
    if (!range[0] || range[1] < range[0] || (range[1] - range[0]) / 8 > 64) return nullptr;
    uintptr_t players[64];
    const size_t n = (range[1] - range[0]) / 8;
    if (!n || !mem::read(range[0], players, n * 8)) return nullptr;
    for (size_t i = 0; i < n; ++i) {
        const uintptr_t p = players[i];
        if (!p || !mem::readOr<uint8_t>(p + ServerPlayer::off::isHuman)) continue;
        const uintptr_t c = mem::readOr<uintptr_t>(p + ServerPlayer::off::character);
        if (!c || mem::readOr<uintptr_t>(c + e::cls::ServerCharacterEntity::off::player) != p) continue;
        if (kindOf(reinterpret_cast<void*>(c), "ServerCharacterEntity")) return reinterpret_cast<void*>(c);
    }
    return nullptr;
}
} // namespace

void* localServerCharacter() {
    static std::atomic<int> reported{0}; // 1 player manager, 2 created characters
    if (void* c = fromPlayerManager()) {
        if (reported.exchange(1) != 1) GN_LOG("world: local server character found through the player manager");
        return c;
    }
    void* c = fromCreatedCharacters();
    if (c && reported.exchange(2) != 2) GN_LOG("world: local server character found among created characters (player manager unreadable)");
    return c;
}

bool entityTransform(const void* entity, gn_transform& out) {
    if (!entity) return false;
    const uintptr_t ts = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(entity) + e::cls::ComponentEntity::off::transformSpace);
    uint8_t sel[2] = {};
    if (!ts || !mem::read(ts + 9, sel, 2) || sel[0] > 1 || sel[1] > 1) return false;
    float m[16];
    if (!mem::read(ts + 0x10 + (sel[0] + 2u * sel[1]) * 0x20, m, sizeof(m)) || !finite(m, 16)) return false;
    memcpy(out.right, m, 16);
    memcpy(out.up, m + 4, 16);
    memcpy(out.forward, m + 8, 16);
    memcpy(out.trans, m + 12, 16);
    return true;
}

int teleport(void* character, const gn_transform* to) {
    if (!character || !to || !finite(to->right, 4) || !finite(to->up, 4) || !finite(to->forward, 4) || !finite(to->trans, 4))
        return GN_ERR_ARGUMENT;
    if (!isServerThread()) return GN_ERR_THREAD;
    if (!teleportFunctionOk()) return GN_ERR_UNAVAILABLE;
    if (!kindOf(character, "ServerCharacterEntity")) return GN_ERR_ARGUMENT;
    alignas(16) gn_transform t = *to;
    reinterpret_cast<void (*)(void*, const void*)>(mem::rva(e::function::ServerCharacterEntity_teleport))(character, &t);
    return GN_OK;
}

void* getComponent(void* character, const gn_type* type) {
    if (!character || !type) return nullptr;
    if (!kindOf(character, "ClientCharacterEntity") && !kindOf(character, "ServerCharacterEntity")) return nullptr;
    uintptr_t vt = 0, fn = 0;
    if (!mem::read(reinterpret_cast<uintptr_t>(character), vt) || !mem::read(vt + e::cls::PamClientCharacterEntity::vf::getComponent, fn) ||
        !isGameCode(fn))
        return nullptr;
    const auto* t = reinterpret_cast<const reflect::Type*>(type);
    return reinterpret_cast<void* (*)(void*, const void*)>(fn)(character, reinterpret_cast<const void*>(t->ti));
}

std::string levelName() {
    const uintptr_t world = mem::readOr<uintptr_t>(mem::rva(e::global::ServerGameWorld));
    const uintptr_t level = world ? mem::readOr<uintptr_t>(world + e::st::ServerGameWorld::off::level) : 0;
    const uintptr_t data = level ? mem::readOr<uintptr_t>(level + e::st::ServerLevel::off::data) : 0;
    const uintptr_t name = data ? mem::readOr<uintptr_t>(data + 0x10) : 0;
    std::string s;
    if (name) mem::readCString(name, s, 256);
    return s;
}

int onLevel(const std::string& mod, gn_level_fn fn, void* user) {
    if (!fn) return GN_ERR_ARGUMENT;
    std::string current;
    {
        std::lock_guard<std::mutex> lock(g_levelM);
        g_levelListeners.push_back({mod, fn, user});
        current = g_level;
    }
    if (!current.empty()) fn(current.c_str(), 1, user);
    return GN_OK;
}

// GridNode's thread, about every 0.1 s.
void pollLevel() {
    const std::string now = levelName();
    std::string before;
    std::vector<LevelListener> ls;
    {
        std::lock_guard<std::mutex> lock(g_levelM);
        if (now == g_level) return;
        before = g_level;
        g_level = now;
        ls = g_levelListeners;
    }
    if (!before.empty()) {
        GN_LOG("level: ended %s", before.c_str());
        callLevel(ls, before, false);
    }
    if (!now.empty()) {
        GN_LOG("level: started %s", now.c_str());
        callLevel(ls, now, true);
    }
}

} // namespace gn::host
