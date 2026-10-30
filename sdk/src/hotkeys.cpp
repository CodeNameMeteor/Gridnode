// Mods' hotkeys (gn_api hotkey_*): keys kept in each mod's own settings as key.<id>, rebound from the
// mod's own window (gn::ui::hotkeyButton), so a mod's players need no GridNode tool for it. Read every
// frame on the render thread with GetAsyncKeyState (a tap shorter than a 30 Hz client tick is still
// seen) and run on the client thread.

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/log.hpp"
#include "host.hpp"
#include "imgui.h"

namespace gn::host {
namespace {

struct Hotkey {
    std::string mod, id, label;
    int def = 0;
    int vk = 0;
    gn_hotkey_fn fn = nullptr;
    void* user = nullptr;
    bool was = true; // held at the last frame (true at first: a key already down does not fire)
};

std::mutex g_m;
std::vector<Hotkey> g_keys;      // (g_m)
int g_rebinding = -1;            // (g_m) the hotkey waiting for a key
bool g_downAtStart[256] = {};    // (g_m) keys held when the wait began: they count once let go and pressed again

std::string configKey(const std::string& id) { return "key." + id; }

// Its key from the mod's settings: a key name, or "none".
int storedKey(const std::string& mod, const std::string& id, int def) {
    bool found = false;
    const std::string v = configValue(mod, configKey(id), &found);
    if (!found) return def;
    if (v == "none") return 0;
    const int vk = ui::keyFromName(v);
    return vk ? vk : def;
}

void store(const Hotkey& k) { configSet(k.mod.c_str(), configKey(k.id).c_str(), k.vk ? ui::keyName(k.vk).c_str() : "none"); }

// (g_m)
Hotkey* findKey(const char* mod, const char* id) {
    if (!mod || !id) return nullptr;
    for (Hotkey& k : g_keys)
        if (k.mod == mod && k.id == id) return &k;
    return nullptr;
}

struct Fire {
    gn_hotkey_fn fn;
    void* user;
    void operator()() const { fn(user); }
};

// Keys a hotkey can be: not the left and right mouse buttons (they click the menu) and not a modifier
// alone.
bool bindable(int vk) {
    return vk >= 0x04 && vk <= 0xFE && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU && !(vk >= 0xA0 && vk <= 0xA5);
}

// (g_m) While rebinding: the first key that goes down. Read as held / not held each frame: the
// "pressed since the last call" bit of GetAsyncKeyState is not reliable (the first version waited on
// it, and clicking a key in the menu did nothing for the user, 2026-10-30).
void rebindFrame() {
    for (int vk = 0x04; vk <= 0xFE; ++vk) {
        if (!bindable(vk)) continue;
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (g_downAtStart[vk]) {
            g_downAtStart[vk] = down;
            continue;
        }
        if (!down) continue;
        if (g_rebinding >= 0 && g_rebinding < int(g_keys.size())) {
            Hotkey& k = g_keys[size_t(g_rebinding)];
            k.vk = vk == VK_ESCAPE ? 0 : vk == VK_BACK ? k.def : vk;
            store(k);
            GN_LOG("hotkeys: %s / %s is now %s", k.mod.c_str(), k.id.c_str(), k.vk ? ui::keyName(k.vk).c_str() : "none");
        }
        for (Hotkey& k : g_keys) k.was = true; // the key just pressed does nothing else
        g_rebinding = -1;
        return;
    }
}

void append(std::string& s, const std::string& what) { s += (s.empty() ? "" : ", ") + what; }

} // namespace

int hotkeyAdd(const char* mod, const char* id, const char* label, int defaultVk, gn_hotkey_fn fn, void* user) {
    if (!mod || !id || !*id || !fn || defaultVk < 0 || defaultVk > 0xFE) return GN_ERR_ARGUMENT;
    Hotkey k;
    k.mod = mod, k.id = id, k.label = label && *label ? label : id;
    k.def = defaultVk, k.fn = fn, k.user = user;
    k.vk = storedKey(k.mod, k.id, defaultVk);
    std::lock_guard<std::mutex> lock(g_m);
    if (Hotkey* old = findKey(mod, id)) {
        *old = k;
        return GN_OK;
    }
    g_keys.push_back(k);
    return GN_OK;
}

int hotkeyInfo(const char* mod, const char* id, gn_hotkey_info* out) {
    if (!out) return GN_ERR_ARGUMENT;
    *out = gn_hotkey_info{};
    const std::vector<std::pair<int, std::string>> reserved = ui::reservedKeys();
    std::lock_guard<std::mutex> lock(g_m);
    const Hotkey* k = findKey(mod, id);
    if (!k) return GN_ERR_NOT_FOUND;
    out->vk = k->vk, out->default_vk = k->def;
    out->waiting = g_rebinding >= 0 && &g_keys[size_t(g_rebinding)] == k;
    snprintf(out->name, sizeof(out->name), "%s", k->vk ? ui::keyName(k->vk).c_str() : "none");
    // The same key for something else: both would act on it.
    std::string clash;
    if (k->vk) {
        for (const Hotkey& o : g_keys)
            if (&o != k && o.vk == k->vk) append(clash, (o.mod == k->mod ? "" : o.mod + ": ") + o.label);
        for (const auto& [vk, what] : reserved)
            if (vk == k->vk) append(clash, "GridNode: " + what);
    }
    snprintf(out->clash, sizeof(out->clash), "%s", clash.c_str());
    return GN_OK;
}

int hotkeyRebind(const char* mod, const char* id) {
    std::lock_guard<std::mutex> lock(g_m);
    const Hotkey* k = findKey(mod, id);
    if (!k) return GN_ERR_NOT_FOUND;
    g_rebinding = int(k - g_keys.data());
    for (int vk = 0; vk < 256; ++vk) g_downAtStart[vk] = bindable(vk) && (GetAsyncKeyState(vk) & 0x8000) != 0;
    return GN_OK;
}

int hotkeySet(const char* mod, const char* id, int vk) {
    if (vk < -1 || vk > 0xFE || (vk > 0 && !bindable(vk))) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_m);
    Hotkey* k = findKey(mod, id);
    if (!k) return GN_ERR_NOT_FOUND;
    k->vk = vk < 0 ? k->def : vk;
    k->was = true;
    store(*k);
    return GN_OK;
}

namespace hotkeys {

void frame(bool menuOpen) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_rebinding >= 0) return rebindFrame();
    const bool live = !menuOpen && ui::gameFocused() && !ImGui::GetIO().WantTextInput;
    for (Hotkey& k : g_keys) {
        const bool down = live && k.vk && (GetAsyncKeyState(k.vk) & 0x8000) != 0;
        if (down && !k.was) client().post(Fire{k.fn, k.user});
        k.was = down || !live; // nothing fires when focus comes back with a key held
    }
}

} // namespace hotkeys
} // namespace gn::host
