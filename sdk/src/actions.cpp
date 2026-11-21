// Things done the game's own way (API 42; blocks P, Q, J and G of 2026-11-15, checked in Ghidra 2026-11-20):
// pause requests through the UI's pause controller, a character's ghost look, a spawner asked to spawn, and the
// camera view manager's own override (its transform, and an FOV through its listener list). Untested in game.
//
// Threads: pause requests, looks and the camera on the client thread; spawners on the server thread. Every change
// a mod makes is taken back when it unloads (forgetActions) and all of them at GridNode's unload (actionsRelease,
// done in the ticks the unload waits for).

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
namespace k = gn::engine::constant;

std::atomic<bool> g_release{false};
std::atomic<bool> g_clientHolds{false}; // the FOV node is linked or pause counts are applied (set at the end of each client tick)

// ---- pause requests --------------------------------------------------------------------------------------------
// UIPauseController_request(controller, on, full) (PamUISystem +0x8c8, a pointer): counter A (+0x08) always, counter
// B (+0x0c) when `full`. UIPauseController_applyToClient pauses when A >= 1 and (B >= 1 or the view's timescale is ~0),
// so a request without `full` pauses nothing by itself. No screen is pushed and input routing does not change.
struct PauseReq {
    uintptr_t owner;
    bool full;
};
std::mutex g_pm;
std::vector<PauseReq> g_pauseWanted;  // (g_pm) what mods asked for
std::vector<PauseReq> g_pauseApplied; // client thread: what GridNode passed to the controller

uintptr_t pauseController() {
    const uintptr_t ui = mem::readOr<uintptr_t>(mem::rva(e::global::PamUISystemInstance));
    if (!ui || mem::readOr<uintptr_t>(ui) != mem::rva(e::cls::PamUISystem::vtable)) return 0;
    const uintptr_t ctrl = mem::readOr<uintptr_t>(ui + 0x8c8);
    return ctrl >= 0x10000 ? ctrl : 0;
}

// Client thread: make the controller's counts match what is wanted (one request at a time).
void pauseTick() {
    std::vector<PauseReq> want;
    {
        std::lock_guard<std::mutex> lock(g_pm);
        if (g_release.load()) g_pauseWanted.clear();
        want = g_pauseWanted;
    }
    if (want.empty() && g_pauseApplied.empty()) return;
    static const bool ok = build::codeMatches("UIPauseController_request");
    if (!ok) return;
    const uintptr_t ctrl = pauseController();
    if (!ctrl) {
        g_pauseApplied.clear(); // the UI system went (a level change): its counts went with it
        return;
    }
    using RequestFn = void (*)(uintptr_t, int8_t, int8_t);
    const auto request = reinterpret_cast<RequestFn>(mem::rva(e::function::UIPauseController_request));
    auto same = [](const PauseReq& a, const PauseReq& b) { return a.owner == b.owner && a.full == b.full; };
    for (size_t i = g_pauseApplied.size(); i-- > 0;) {
        auto it = std::find_if(want.begin(), want.end(), [&](const PauseReq& w) { return same(w, g_pauseApplied[i]); });
        if (it != want.end()) {
            want.erase(it);
            continue;
        }
        request(ctrl, 0, g_pauseApplied[i].full ? 1 : 0);
        g_pauseApplied.erase(g_pauseApplied.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (const PauseReq& w : want) {
        request(ctrl, 1, w.full ? 1 : 0);
        g_pauseApplied.push_back(w);
    }
}

// ---- looks -----------------------------------------------------------------------------------------------------
// PamClientCharacterEntity +0x334 (drawn with the ghost projection shader) and +0x335 (render view mask 0x80): set
// from its data when it is made (vf65) and read by its renderer every frame, so a change shows on the next frame.
struct Look {
    uintptr_t entity;
    uintptr_t owner;
    uint8_t was334, was335;
};
std::vector<Look> g_looks; // client thread

bool lookAlive(uintptr_t p) {
    const reflect::Type* t = registry().typeOf(mem::ProcessMemory(), p);
    static const reflect::Type* cls = registry().find("PamClientCharacterEntity");
    return t && cls && registry().isKindOf(t, cls) && inspector::alive(p, t) && !entityGoing(p);
}

void restoreLook(const Look& l) {
    if (!lookAlive(l.entity)) return;
    mem::write<uint8_t>(l.entity + k::PamClientCharacter_GhostShader, l.was334);
    mem::write<uint8_t>(l.entity + k::PamClientCharacter_GhostMask, l.was335);
}

// ---- the camera view manager's override ------------------------------------------------------------------------
// ClientCameraViewManager_setOverride copies a transform to +0x40 and an FOV (degrees) to +0x80 and sets +0x84; the
// frame's view snapshot then uses them (ClientCameraViewManager_snapshotView). postSimUpdate clears +0x84 every frame,
// so it is set again each client tick. Whether the client tick comes between that clear and the snapshot is not known:
// untested. The FOV alone goes through the manager's listener list (+0x1b90, empty in the game): frameUpdate asks each
// node's slot 1 for degrees and takes the first > 0. GridNode's node lives in gridnode.dll: linked and unlinked on the
// client thread, where frameUpdate runs.
std::mutex g_cm;
bool g_viewOn = false;         // (g_cm)
gn_transform g_view{};         // (g_cm)
float g_viewFov = 0.f;         // (g_cm)
uintptr_t g_viewOwner = 0;     // (g_cm)
std::atomic<float> g_fovHold{0.f};
uintptr_t g_fovOwner = 0;      // (g_cm)

struct FovNode {
    const void* const* vtable;
    FovNode* next;
};
void fovNodeUpdate(FovNode*, void*) {}
float fovNodeGet(FovNode*) { return g_fovHold.load(std::memory_order_relaxed); }
const void* const kFovNodeVtable[2] = {reinterpret_cast<const void*>(&fovNodeUpdate), reinterpret_cast<const void*>(&fovNodeGet)};
FovNode g_fovNode{kFovNodeVtable, nullptr};
uintptr_t g_fovLinkedTo = 0; // client thread: the manager whose list holds g_fovNode

uintptr_t viewManager() {
    namespace cv = e::st::ClientCameraViewManagers::off;
    const uintptr_t managers = mem::readOr<uintptr_t>(mem::rva(e::global::ClientCameraViewManagers));
    const uintptr_t begin = managers ? mem::readOr<uintptr_t>(managers + cv::managersBegin) : 0;
    const uintptr_t end = managers ? mem::readOr<uintptr_t>(managers + cv::managersEnd) : 0;
    return begin && end > begin ? mem::readOr<uintptr_t>(begin) : 0;
}

void fovUnlink() {
    if (!g_fovLinkedTo) return;
    const uintptr_t head = g_fovLinkedTo + e::st::ClientCameraViewManager::off::postUpdateListeners;
    uintptr_t prevNext = head; // the address holding the pointer to the current node
    for (int guard = 0; guard < 256; ++guard) {
        const uintptr_t node = mem::readOr<uintptr_t>(prevNext);
        if (!node) break;
        if (node == reinterpret_cast<uintptr_t>(&g_fovNode)) {
            mem::write<uintptr_t>(prevNext, reinterpret_cast<uintptr_t>(g_fovNode.next));
            break;
        }
        prevNext = node + 8;
    }
    g_fovNode.next = nullptr;
    g_fovLinkedTo = 0;
}

void cameraTick() {
    const uintptr_t mgr = viewManager();
    const bool wantFov = !g_release.load() && g_fovHold.load() > 0.f;
    if (g_fovLinkedTo && (g_fovLinkedTo != mgr || !wantFov)) {
        if (g_fovLinkedTo != mgr) {
            g_fovLinkedTo = 0; // that manager is gone (a level change): its list went with it
            g_fovNode.next = nullptr;
        } else {
            fovUnlink();
        }
    }
    if (wantFov && mgr && !g_fovLinkedTo) {
        const uintptr_t head = mgr + e::st::ClientCameraViewManager::off::postUpdateListeners;
        g_fovNode.next = reinterpret_cast<FovNode*>(mem::readOr<uintptr_t>(head));
        mem::write<uintptr_t>(head, reinterpret_cast<uintptr_t>(&g_fovNode));
        g_fovLinkedTo = mgr;
    }
    bool on;
    gn_transform view;
    float fov;
    {
        std::lock_guard<std::mutex> lock(g_cm);
        if (g_release.load()) g_viewOn = false;
        on = g_viewOn, view = g_view, fov = g_viewFov;
    }
    if (!on || !mgr) return;
    static const bool ok = build::codeMatches("ClientCameraViewManager_setOverride");
    if (!ok) return;
    using SetFn = void (*)(uintptr_t, const gn_transform*, float, uint8_t, uint8_t);
    reinterpret_cast<SetFn>(mem::rva(e::function::ClientCameraViewManager_setOverride))(mgr, &view, fov > 0.f ? fov : -1.f, 0, 0);
}

bool finiteTransform(const gn_transform& t) {
    const float* f = &t.right[0];
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(f[i])) return false;
    return true;
}

} // namespace

// ---- the API ---------------------------------------------------------------------------------------------------

int uiPauseRequest(const void* caller, int on, int full) {
    if (!build::codeMatches("UIPauseController_request")) return GN_ERR_UNAVAILABLE;
    const PauseReq r{reinterpret_cast<uintptr_t>(caller), full != 0};
    std::lock_guard<std::mutex> lock(g_pm);
    auto it = std::find_if(g_pauseWanted.begin(), g_pauseWanted.end(), [&](const PauseReq& w) { return w.owner == r.owner && w.full == r.full; });
    if (on) {
        if (it == g_pauseWanted.end()) g_pauseWanted.push_back(r); // one request per mod and kind: a second is the same
    } else if (it != g_pauseWanted.end()) {
        g_pauseWanted.erase(it);
    }
    return GN_OK;
}

int characterLook(const void* caller, void* character, int ghost, int mask) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(character);
    if (!p || !lookAlive(p)) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    auto it = std::find_if(g_looks.begin(), g_looks.end(), [&](const Look& l) { return l.entity == p; });
    if (ghost < 0 && mask < 0) { // given back
        if (it != g_looks.end()) {
            restoreLook(*it);
            g_looks.erase(it);
        }
        return GN_OK;
    }
    if (it == g_looks.end()) {
        if (g_looks.size() >= 256) return GN_ERR_FULL;
        g_looks.push_back({p, reinterpret_cast<uintptr_t>(caller), mem::readOr<uint8_t>(p + k::PamClientCharacter_GhostShader),
                           mem::readOr<uint8_t>(p + k::PamClientCharacter_GhostMask)});
    }
    if (ghost >= 0) mem::write<uint8_t>(p + k::PamClientCharacter_GhostShader, ghost ? 1 : 0);
    if (mask >= 0) mem::write<uint8_t>(p + k::PamClientCharacter_GhostMask, mask ? 1 : 0);
    return GN_OK;
}

// A spawner asked to spawn the way the game's own Spawn event does it (ServerSpawnEntity vf4 / vf2, block G): a spawn is
// wanted (+0x108 |= 0x8), the delay is over (+0xf0 = 0), and it is in the spawner updater, whose next tick spawns with
// every check the game makes (enabled, max count, a free spawn point).
int spawnerSpawn(void* spawner) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(spawner);
    static const reflect::Type* cls = registry().find("ServerCharacterSpawnEntity");
    const reflect::Type* t = p ? registry().typeOf(mem::ProcessMemory(), p) : nullptr;
    if (!t || !cls || !registry().isKindOf(t, cls) || !inspector::alive(p, t)) return GN_ERR_ARGUMENT;
    if (entityGoing(p)) return GN_ERR_UNAVAILABLE;
    if (!isServerThread()) return GN_ERR_THREAD;
    if (!build::codeMatches("EntityUpdater_add")) return GN_ERR_UNAVAILABLE;
    if (!mem::readOr<uint8_t>(p + k::Spawner_Enabled)) return GN_ERR_UNAVAILABLE; // createCharacter refuses a disabled spawner
    const uintptr_t updater = mem::readOr<uintptr_t>(mem::rva(e::global::ServerSpawnEntityUpdater));
    if (!updater) return GN_ERR_UNAVAILABLE;
    mem::write<uint8_t>(p + k::Spawner_StateFlags, static_cast<uint8_t>((mem::readOr<uint8_t>(p + k::Spawner_StateFlags) & 0xef) | 0x08));
    mem::write<float>(p + k::Spawner_Delay, 0.f);
    reinterpret_cast<void (*)(uintptr_t, uintptr_t)>(mem::rva(e::function::EntityUpdater_add))(updater, p);
    return GN_OK;
}

int cameraOverride(const void* caller, const gn_transform* view, float fovDegrees) {
    if (view && !finiteTransform(*view)) return GN_ERR_ARGUMENT;
    if (!std::isfinite(fovDegrees) || fovDegrees > 179.f) return GN_ERR_ARGUMENT;
    if (!build::codeMatches("ClientCameraViewManager_setOverride")) return GN_ERR_UNAVAILABLE;
    std::lock_guard<std::mutex> lock(g_cm);
    if (!view) {
        g_viewOn = false;
        return GN_OK;
    }
    g_view = *view, g_viewFov = fovDegrees, g_viewOwner = reinterpret_cast<uintptr_t>(caller), g_viewOn = true;
    return GN_OK;
}

int cameraFovHold(const void* caller, float fovDegrees) {
    if (!std::isfinite(fovDegrees) || fovDegrees > 179.f) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_cm);
    g_fovOwner = reinterpret_cast<uintptr_t>(caller);
    g_fovHold = fovDegrees > 0.f ? fovDegrees : 0.f;
    return GN_OK;
}

// Client tick (ticks.cpp).
void actionsClientTick() {
    pauseTick();
    cameraTick();
    if (g_release.load())
        for (const Look& l : g_looks) restoreLook(l);
    if (g_release.load()) g_looks.clear();
    else g_looks.erase(std::remove_if(g_looks.begin(), g_looks.end(), [](const Look& l) { return !lookAlive(l.entity); }), g_looks.end());
    g_clientHolds = g_fovLinkedTo != 0 || !g_pauseApplied.empty();
}

void forgetActions(const CodeRange& r) {
    {
        std::lock_guard<std::mutex> lock(g_pm);
        g_pauseWanted.erase(std::remove_if(g_pauseWanted.begin(), g_pauseWanted.end(), [&](const PauseReq& w) { return r.has(reinterpret_cast<void*>(w.owner)); }),
                            g_pauseWanted.end());
    }
    {
        std::lock_guard<std::mutex> lock(g_cm);
        if (g_viewOn && r.has(reinterpret_cast<void*>(g_viewOwner))) g_viewOn = false;
        if (g_fovHold.load() > 0.f && r.has(reinterpret_cast<void*>(g_fovOwner))) g_fovHold = 0.f;
    }
    client().post([r] {
        for (size_t i = g_looks.size(); i-- > 0;)
            if (r.has(reinterpret_cast<void*>(g_looks[i].owner))) {
                restoreLook(g_looks[i]);
                g_looks.erase(g_looks.begin() + static_cast<std::ptrdiff_t>(i));
            }
    });
}

void actionsRelease() { g_release = true; }

// Whether nothing of GridNode's is left in the game's lists (the FOV node in a view manager, pause counts).
bool actionsReleased() { return !g_clientHolds.load(); }

// ---- the console -----------------------------------------------------------------------------------------------
// "pause [on|full|off]", "look ADDRESS ghost|mask|off [0|1]", "spawn ADDRESS", "destroy ADDRESS",
// "gcam fov DEGREES|off", "gcam here|off".
std::string pauseCommand(const std::string& rest) {
    if (rest.empty()) {
        std::lock_guard<std::mutex> lock(g_pm);
        return "pause: " + std::to_string(g_pauseWanted.size()) + " request(s) from the console and mods (pause on | full | off)";
    }
    static const char kConsole = 0; // the console's own owner
    if (!_stricmp(rest.c_str(), "off")) {
        uiPauseRequest(&kConsole, 0, 0);
        uiPauseRequest(&kConsole, 0, 1);
        return "pause: the console's requests taken back (next client tick)";
    }
    const bool full = !_stricmp(rest.c_str(), "full");
    const int r = uiPauseRequest(&kConsole, 1, full ? 1 : 0);
    return r == GN_OK ? std::string("pause: requested") + (full ? " (full: pauses the world)" : " (not full: pauses only if a screen's timescale is 0)")
                      : "pause: refused (" + std::to_string(r) + ")";
}

std::string lookCommand(const std::string& rest) {
    unsigned long long addr = 0;
    char what[16] = {};
    int v = 1;
    if (sscanf(rest.c_str(), "%llx %15s %d", &addr, what, &v) < 2) return "look ADDRESS ghost|mask|off [0|1]: a PamClientCharacterEntity's ghost shader or view mask";
    const std::string w = what;
    return runOnRealmAndWait(false, [addr, w, v]() -> std::string {
        static const char kConsole = 0;
        void* c = reinterpret_cast<void*>(addr);
        const int r = !_stricmp(w.c_str(), "off") ? characterLook(&kConsole, c, -1, -1)
                      : !_stricmp(w.c_str(), "mask") ? characterLook(&kConsole, c, -1, v)
                                                     : characterLook(&kConsole, c, v, -1);
        return r == GN_OK ? "look: done (next frame)" : "look: refused (" + std::to_string(r) + ")";
    });
}

std::string spawnCommand(const std::string& rest) {
    unsigned long long addr = 0;
    if (sscanf(rest.c_str(), "%llx", &addr) != 1) return "spawn ADDRESS: a ServerCharacterSpawnEntity spawns at its next tick (the game's checks apply)";
    return runOnRealmAndWait(true, [addr]() -> std::string {
        const int r = spawnerSpawn(reinterpret_cast<void*>(addr));
        return r == GN_OK ? "spawn: asked (the spawner's next tick spawns if the game allows it)" : "spawn: refused (" + std::to_string(r) + ")";
    });
}

std::string destroyCommand(const std::string& rest) {
    unsigned long long addr = 0;
    if (sscanf(rest.c_str(), "%llx", &addr) != 1) return "destroy ADDRESS: the game destroys that entity (not Faith, not her safe spawn)";
    const bool server = (mem::readOr<uint32_t>(static_cast<uintptr_t>(addr) + 0x18) & 1) != 0;
    return runOnRealmAndWait(server, [addr]() -> std::string {
        const int r = entityDestroy(reinterpret_cast<void*>(addr));
        return r == GN_OK ? "destroy: queued (freed in the world's next update)" : "destroy: refused (" + std::to_string(r) + ")";
    });
}

std::string gcamCommand(const std::string& rest) {
    static const char kConsole = 0;
    char a[16] = {}, b[16] = {};
    sscanf(rest.c_str(), "%15s %15s", a, b);
    if (!_stricmp(a, "fov")) {
        if (!_stricmp(b, "off") || !*b) {
            cameraFovHold(&kConsole, 0.f);
            return "gcam: FOV given back";
        }
        const int r = cameraFovHold(&kConsole, strtof(b, nullptr));
        return r == GN_OK ? std::string("gcam: FOV held at ") + b + " degrees (the view manager's listener list)" : "gcam: refused (" + std::to_string(r) + ")";
    }
    if (!_stricmp(a, "here")) {
        return runOnRealmAndWait(false, []() -> std::string {
            const uintptr_t mgr = viewManager();
            if (!mgr) return "gcam: no camera view manager";
            gn_transform t{};
            if (!mem::read(mgr + e::st::ClientCameraViewManager::off::transform, &t, sizeof(t))) return "gcam: cannot read the view";
            const int r = cameraOverride(&kConsole, &t, -1.f);
            return r == GN_OK ? "gcam: the view held where it is (the view manager's override; gcam off gives it back)" : "gcam: refused (" + std::to_string(r) + ")";
        });
    }
    if (!_stricmp(a, "off")) {
        cameraOverride(&kConsole, nullptr, 0.f);
        cameraFovHold(&kConsole, 0.f);
        return "gcam: override and FOV given back";
    }
    return "gcam fov DEGREES|off | here | off: the camera view manager's own override (experimental)";
}

} // namespace gn::host
