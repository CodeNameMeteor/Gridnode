// Loads the mods: every DLL in the mods folder that exports gn_mod_load, in name order.
//
// A mod that also exports gn_mod_unload can be reloaded without restarting the game (the GridNode
// menu's Mods list, or when its file changes): it is loaded from a copy in mods\.loaded\, so its DLL
// can be rebuilt while the game runs. Unloading asks the mod to stop (gn_mod_unload: its own
// threads, its state), takes out everything it registered (each part of GridNode drops the callbacks
// and hooks whose code is in the mod's image), waits for the game's threads to leave its code (a few
// frames and ticks), then frees it. Other mods are pinned: never unloaded, as their hooks and
// callbacks point into them.

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/log.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

using UnloadFn = int (*)();

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

struct Mod {
    ModInfo info;
    std::wstring path;   // mods\<file>
    HMODULE module = nullptr;
    FILETIME written{};  // the file's time when loaded
    unsigned copies = 0; // copies made (each load of a reloadable mod gets a new name)
    bool reloadWanted = false;
    ULONGLONG changedAt = 0; // the file changed (auto reload waits for it to settle)
};

std::mutex g_modsM;
std::vector<Mod> g_mods; // guarded by g_modsM; changed only on GridNode's thread
std::wstring g_folder;   // the mods folder, with a trailing backslash

FILETIME writeTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return FILETIME{};
    return a.ftLastWriteTime;
}
bool sameTime(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

// Whether the DLL exports gn_mod_unload, read without running it (no DllMain, no imports).
bool exportsUnload(const std::wstring& path) {
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) return false;
    const bool has = GetProcAddress(m, "gn_mod_unload") != nullptr;
    FreeLibrary(m);
    return has;
}

CodeRange imageOf(HMODULE m) {
    const auto base = reinterpret_cast<uintptr_t>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return {base, base + nt->OptionalHeader.SizeOfImage};
}

// Loads one mod and calls its gn_mod_load. The result is in m.info.
void load(Mod& m) {
    const std::string name = m.info.file;
    m.written = writeTime(m.path);
    m.info.reloadable = exportsUnload(m.path);
    std::wstring from = m.path;
    if (m.info.reloadable) {
        // A copy under a new name each time: the original stays free to be rebuilt, and Windows never
        // hands back an older copy still mapped.
        const std::wstring dir = g_folder + L".loaded\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring stem = m.path.substr(g_folder.size());
        stem.resize(stem.size() - 4); // ".dll"
        from = dir + stem + L"." + std::to_wstring(++m.copies) + L".dll";
        if (!CopyFileW(m.path.c_str(), from.c_str(), FALSE)) {
            GN_LOG("mods: %s could not be copied to .loaded (error %lu); loaded in place, not reloadable", name.c_str(),
                   static_cast<unsigned long>(GetLastError()));
            from = m.path;
            m.info.reloadable = false;
        }
    }
    m.module = LoadLibraryW(from.c_str());
    if (!m.module) {
        m.info.loaded = false, m.info.result = -1;
        GN_LOG("mods: %s could not be loaded (error %lu)", name.c_str(), static_cast<unsigned long>(GetLastError()));
        return;
    }
    auto entry = reinterpret_cast<gn_mod_load_fn>(reinterpret_cast<void*>(GetProcAddress(m.module, "gn_mod_load")));
    if (!entry) {
        GN_LOG("mods: %s does not export gn_mod_load; skipped", name.c_str());
        FreeLibrary(m.module);
        m.module = nullptr;
        m.info.loaded = false, m.info.result = -1;
        return;
    }
    if (!m.info.reloadable) {
        // Never unloaded: their hooks and callbacks point into them.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(reinterpret_cast<void*>(entry)), &pinned);
    }
    const int r = entry(api());
    m.info.loaded = r == GN_OK, m.info.result = r;
    if (r == GN_OK) GN_LOG("mods: %s loaded%s", name.c_str(), m.info.reloadable ? " (reloadable)" : "");
    else GN_LOG("mods: %s returned %d from gn_mod_load", name.c_str(), r);
}

// Takes the mod out and frees it; false (and it stays loaded, inert or not) when that is not safe.
bool unload(Mod& m) {
    const std::string name = m.info.file;
    if (!m.module) return true;
    if (auto stop = reinterpret_cast<UnloadFn>(reinterpret_cast<void*>(GetProcAddress(m.module, "gn_mod_unload")))) {
        int r = 0;
        try {
            r = stop();
        } catch (...) {
            r = -1;
        }
        if (r != 0) {
            m.info.status = "refused to unload (gn_mod_unload returned " + std::to_string(r) + ")";
            GN_LOG("mods: %s %s", name.c_str(), m.info.status.c_str());
            return false;
        }
    }
    const CodeRange code = imageOf(m.module);
    std::vector<std::string> refused;
    const size_t hooksOut = forgetHooks(code, &refused);
    forgetTicks(code);
    forgetCreated(code);
    forgetLevel(code);
    forgetMessages(code);
    forgetMovement(code);
    forgetEvents(code);
    hotkeys::forget(code);
    ui::forgetUi(code);
    configFlush(true);
    if (!refused.empty()) {
        // One of its hooks has another mod's hook on top: freeing it would send that hook into freed
        // code. It stays loaded (everything else of it is out).
        m.info.status = "kept loaded: " + refused.front();
        GN_LOG("mods: %s %s", name.c_str(), m.info.status.c_str());
        return false;
    }
    // The game's threads may be inside its code right now (a tick, a frame, a hook): wait until the
    // removals reached the tick threads and a few frames passed. A realm that is not ticking (a load)
    // applies the removal before it next calls anyone, so the wait is capped.
    const uint64_t frames = ui::frameCount();
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 3000) {
        Sleep(50);
        const bool ticks = !server().removing() && !client().removing();
        // No frames at all for a second: the overlay is off or the window is minimised.
        const bool drawn = ui::frameCount() >= frames + 3 || (ui::frameCount() == frames && GetTickCount64() - start > 1000);
        if (ticks && drawn && GetTickCount64() - start >= 300) break;
    }
    FreeLibrary(m.module);
    m.module = nullptr;
    m.info.loaded = false;
    GN_LOG("mods: %s unloaded (%llu hooks taken out)", name.c_str(), static_cast<unsigned long long>(hooksOut));
    return true;
}

void reload(Mod& m) {
    GN_LOG("mods: reloading %s", m.info.file.c_str());
    if (!unload(m)) return;
    load(m);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char b[64];
    snprintf(b, sizeof(b), "%s at %02u:%02u:%02u", m.info.loaded ? "reloaded" : "reload failed", t.wHour, t.wMinute, t.wSecond);
    m.info.status = b;
}

} // namespace

std::vector<ModInfo> modList() {
    std::lock_guard<std::mutex> lock(g_modsM);
    std::vector<ModInfo> out;
    for (const Mod& m : g_mods) out.push_back(m.info);
    return out;
}

void modReload(const std::string& file) {
    std::lock_guard<std::mutex> lock(g_modsM);
    for (Mod& m : g_mods)
        if (m.info.file == file && m.info.reloadable) m.reloadWanted = true;
}

void modAutoReload(const std::string& file, bool on) {
    std::lock_guard<std::mutex> lock(g_modsM);
    for (Mod& m : g_mods)
        if (m.info.file == file) m.info.autoReload = on;
}

void pollMods() {
    const ULONGLONG now = GetTickCount64();
    size_t count;
    {
        std::lock_guard<std::mutex> lock(g_modsM);
        count = g_mods.size();
    }
    for (size_t i = 0; i < count; ++i) {
        Mod work;
        {
            std::lock_guard<std::mutex> lock(g_modsM);
            Mod& m = g_mods[i];
            if (!m.info.reloadable) continue;
            if (m.info.autoReload && !m.reloadWanted) {
                // Changed, and left alone for half a second (a build writes the file in parts).
                const FILETIME w = writeTime(m.path);
                if (!sameTime(w, m.written) && (w.dwLowDateTime || w.dwHighDateTime)) {
                    if (!m.changedAt) m.changedAt = now;
                    else if (now - m.changedAt >= 500) m.reloadWanted = true;
                }
            }
            if (!m.reloadWanted) continue;
            m.reloadWanted = false, m.changedAt = 0;
            work = m;
        }
        // Without the lock: the reload waits for frames, and the menu reads the list every frame.
        reload(work);
        std::lock_guard<std::mutex> lock(g_modsM);
        const bool autoReload = g_mods[i].info.autoReload; // may have been changed meanwhile
        g_mods[i] = work;
        g_mods[i].info.autoReload = autoReload;
    }
}

void loadMods(const std::wstring& folder) {
    g_folder = folder;
    if (!g_folder.empty() && g_folder.back() != L'\\' && g_folder.back() != L'/') g_folder += L'\\';
    // Copies from the last run (their names would be reused).
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_folder + L".loaded\\*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do DeleteFileW((g_folder + L".loaded\\" + fd.cFileName).c_str());
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::vector<std::wstring> files;
    h = FindFirstFileW((g_folder + L"*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        GN_LOG("mods: none (no DLLs in %s)", narrow(g_folder).c_str());
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end());

    int loaded = 0;
    for (const std::wstring& f : files) {
        Mod m;
        m.info.file = narrow(f);
        m.path = g_folder + f;
        load(m);
        if (m.info.loaded) ++loaded;
        if (!m.module) continue; // not a mod
        std::lock_guard<std::mutex> lock(g_modsM);
        g_mods.push_back(std::move(m));
    }
    GN_LOG("mods: %d of %llu loaded", loaded, static_cast<unsigned long long>(files.size()));
}

} // namespace gn::host
