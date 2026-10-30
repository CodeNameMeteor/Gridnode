// Mod settings (gn_api config_get / config_set): one key=value file per mod in the mods folder,
// read when the mod first asks and written a second after its last change (and when the game
// closes), so a slider being dragged does not write the file every frame.

#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "gridnode/config.hpp"
#include "gridnode/log.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

struct ModConfig {
    config::Entries entries;
    bool dirty = false;
    ULONGLONG changedAt = 0;
};

std::mutex g_m;
std::wstring g_folder;                 // (g_m) the mods folder, with a trailing backslash
std::map<std::string, ModConfig> g_mods; // (g_m) by mod name

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

std::wstring pathOf(const std::string& mod) { return g_folder + widen(config::fileName(mod)) + L".ini"; }

// (g_m) The mod's settings, read from its file the first time.
ModConfig& load(const std::string& mod) {
    auto it = g_mods.find(mod);
    if (it != g_mods.end()) return it->second;
    ModConfig& c = g_mods[mod];
    if (g_folder.empty()) return c;
    FILE* f = _wfopen(pathOf(mod).c_str(), L"rb");
    if (!f) return c;
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    fclose(f);
    c.entries = config::parse(text);
    return c;
}

// (g_m) Written to a temporary file and renamed, so a crash never leaves half a file.
void write(const std::string& mod, ModConfig& c) {
    c.dirty = false;
    if (g_folder.empty()) return;
    const std::wstring path = pathOf(mod), tmp = path + L".tmp";
    const std::string text = config::serialize(c.entries, "Settings of the mod " + mod + ", written by GridNode.\n"
                                                           "Edit with the game closed, or GridNode writes over it.");
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        GN_LOG("config: cannot write the settings of %s", mod.c_str());
        return;
    }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        GN_LOG("config: writing the settings of %s failed", mod.c_str());
        DeleteFileW(tmp.c_str());
    }
}

} // namespace

void configStart(const std::wstring& folder) {
    std::lock_guard<std::mutex> lock(g_m);
    g_folder = folder;
    if (!g_folder.empty() && g_folder.back() != L'\\' && g_folder.back() != L'/') g_folder += L'\\';
}

int configGet(const char* mod, const char* key, char* out, size_t size) {
    if (out && size) out[0] = 0;
    if (!mod || !key || !out || !size) return GN_ERR_ARGUMENT;
    if (!config::validKey(key)) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_m);
    const std::string* v = config::find(load(mod).entries, key);
    if (!v) return GN_ERR_NOT_FOUND;
    const size_t n = v->size() < size - 1 ? v->size() : size - 1;
    memcpy(out, v->data(), n);
    out[n] = 0;
    return n == v->size() ? GN_OK : GN_ERR_FULL;
}

std::string configValue(const std::string& mod, const std::string& key, bool* found) {
    std::lock_guard<std::mutex> lock(g_m);
    const std::string* v = config::find(load(mod).entries, key);
    if (found) *found = v != nullptr;
    return v ? *v : std::string();
}

int configSet(const char* mod, const char* key, const char* value) {
    if (!mod || !key || !value || !config::validKey(key)) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_m);
    ModConfig& c = load(mod);
    if (config::set(c.entries, key, value)) c.dirty = true, c.changedAt = GetTickCount64();
    return GN_OK;
}

void configFlush(bool now) {
    // At exit (now) other threads may have been stopped holding the lock: never wait for it there.
    std::unique_lock<std::mutex> lock(g_m, std::defer_lock);
    if (now) {
        if (!lock.try_lock()) return;
    } else {
        lock.lock();
    }
    const ULONGLONG t = GetTickCount64();
    for (auto& [mod, c] : g_mods)
        if (c.dirty && (now || t - c.changedAt >= 1000)) write(mod, c);
}

} // namespace gn::host
