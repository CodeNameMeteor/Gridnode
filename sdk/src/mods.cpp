// Loads the mods: every DLL in the mods folder that exports gn_mod_load, dependencies first, otherwise
// in name order. A mod may say who it is (gn_mod_info: id, version, dependencies): every DLL is opened
// first, the order worked out (gridnode/mod_order.hpp), then each gn_mod_load called in it; a mod whose
// hard dependency is missing or too old is not started (the Mods list says why).
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
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <map>
#include <set>

#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/mod_order.hpp"
#include "gridnode/pe_file.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

using UnloadFn = int (*)();
using InfoFn = const gn_mod_manifest* (*)();

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
    bool unloadWanted = false; // unload and stay unloaded (the menu, or the mod itself: gn_api mod_unload)
    bool loadWanted = false;   // load again after that
    bool inert = false;        // taken out, but a thread was still in its code: not freed (Unload tries again)
    CodeRange code;            // its image while loaded (to find a mod by an address in it)
    ULONGLONG changedAt = 0; // the file changed (auto reload waits for it to settle)
    std::vector<modorder::Dep> deps;
};

// Functions mods offer each other (gn_api mod_export / mod_import), by "<id>.<name>".
struct Export {
    void* fn;
    CodeRange owner;
};
std::mutex g_expM;
std::map<std::string, Export> g_exports;            // (g_expM)
std::map<std::string, std::set<std::string>> g_importers; // (g_expM) exporter id -> ids that imported from it
// (g_expM) The mod whose gn_mod_load is running: not in g_mods yet (or only as its old copy), but its
// exports and imports are made from there (the trainer's godMode was "not offered", 2026-11-03).
std::string g_startingId;
CodeRange g_startingCode;
std::wstring g_startingPath;
// (g_expM) Mods that called online_allow, by id: their reason. Taken out with the mod (takeOut).
std::map<std::string, std::string> g_online;

std::mutex g_modsM;
std::vector<Mod> g_mods; // guarded by g_modsM; changed only on GridNode's thread
std::wstring g_folder;   // the mods folder, with a trailing backslash

FILETIME writeTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return FILETIME{};
    return a.ftLastWriteTime;
}
bool sameTime(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

// Whether the DLL exports gn_mod_unload, read from the file (no DllMain, no imports, nothing mapped).
bool exportsUnload(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::vector<uint8_t> bytes;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (256ll << 20)) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) || got != bytes.size()) bytes.clear();
    }
    CloseHandle(f);
    return pe::exportsName(bytes.data(), bytes.size(), "gn_mod_unload");
}

CodeRange imageOf(HMODULE m) {
    const auto base = reinterpret_cast<uintptr_t>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return {base, base + nt->OptionalHeader.SizeOfImage};
}

// A string from a mod's manifest: at most 256 bytes read (a mod's data, not trusted to end).
std::string str(const char* s) { return s ? std::string(s, strnlen(s, 256)) : std::string(); }

// Who the mod says it is (gn_mod_info, optional): id (default: the file name), name, version, author,
// description, dependencies.
void readManifest(Mod& m) {
    std::string stem = m.info.file;
    if (stem.size() > 4) stem.resize(stem.size() - 4);
    m.info.id = stem, m.info.name.clear(), m.info.version.clear(), m.info.author.clear(), m.info.description.clear();
    m.info.depends.clear();
    m.deps.clear();
    auto info = m.module ? reinterpret_cast<InfoFn>(reinterpret_cast<void*>(GetProcAddress(m.module, "gn_mod_info"))) : nullptr;
    const gn_mod_manifest* mf = info ? info() : nullptr;
    if (!mf || mf->size < sizeof(uint32_t) + 6 * sizeof(void*)) return;
    if (mf->id && *mf->id) m.info.id = str(mf->id);
    m.info.name = str(mf->name), m.info.version = str(mf->version), m.info.author = str(mf->author), m.info.description = str(mf->description);
    int n = 0;
    for (const char* const* d = mf->depends; d && *d && n < 64; ++d, ++n) { // at most 64
        m.deps.push_back(modorder::parseDep(str(*d)));
        m.info.depends += (m.info.depends.empty() ? "" : ", ") + str(*d);
    }
}

// Opens the mod's DLL (a copy, when reloadable) and reads its manifest; false when it is not a mod.
bool openMod(Mod& m) {
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
        for (wchar_t& c : stem)
            if (c == L'\\' || c == L'/') c = L'_'; // a mod in its own folder: mods\<name>\<name>.dll
        // A name still mapped (a copy kept in memory, maybe by an earlier GridNode) cannot be written: the next.
        bool copied = false;
        for (int tries = 0; tries < 50 && !copied; ++tries) {
            from = dir + stem + L"." + std::to_wstring(++m.copies) + L".dll";
            copied = CopyFileW(m.path.c_str(), from.c_str(), FALSE) != 0;
        }
        if (!copied) {
            GN_LOG("mods: %s could not be copied to .loaded (error %lu); loaded in place, not reloadable", name.c_str(),
                   static_cast<unsigned long>(GetLastError()));
            from = m.path;
            m.info.reloadable = false;
        }
    }
    // A mod in its own folder may bring DLLs of its own there: found from that folder (the copy it
    // runs from is in .loaded).
    const std::wstring own = m.path.substr(0, m.path.find_last_of(L"\\/") + 1);
    if (own != g_folder) {
        DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(own.c_str());
        m.module = LoadLibraryExW(from.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
        if (cookie) RemoveDllDirectory(cookie);
    } else {
        m.module = LoadLibraryW(from.c_str());
    }
    if (!m.module) {
        m.info.loaded = false, m.info.result = -1;
        GN_LOG("mods: %s could not be loaded (error %lu)", name.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    if (!GetProcAddress(m.module, "gn_mod_load")) {
        GN_LOG("mods: %s does not export gn_mod_load; skipped", name.c_str());
        FreeLibrary(m.module);
        m.module = nullptr;
        m.info.loaded = false, m.info.result = -1;
        return false;
    }
    m.code = imageOf(m.module);
    readManifest(m);
    return true;
}

// A mod opened but not started (its dependencies are not there): closed again.
void closeMod(Mod& m, const std::string& why) {
    m.info.loaded = false, m.info.result = -1, m.info.status = why;
    GN_LOG("mods: %s not loaded: %s", m.info.file.c_str(), why.c_str());
    if (m.module) FreeLibrary(m.module);
    m.module = nullptr;
    m.code = CodeRange{};
}

void forgetExports(const CodeRange& r, const std::string& id);

// Everything the mod's code registered, taken out: hooks, patches, callbacks, keys, settings, exports
// (an unload, or a gn_mod_load that failed half way). A hook with another mod's hook on top stays and
// is listed in `refused`. How many hooks came out.
size_t takeOut(const CodeRange& code, const std::string& id, std::vector<std::string>* refused) {
    const size_t hooksOut = forgetHooks(code, refused);
    forgetPatches(code);
    forgetTicks(code);
    forgetCreated(code);
    forgetDestroyed(code);
    layouts::forgetCamera(code);
    forgetLevel(code);
    forgetLevelReady(code);
    forgetMessages(code);
    forgetMovement(code);
    forgetEvents(code);
    hotkeys::forget(code);
    forgetSettings(code);
    ui::forgetUi(code);
    forgetExports(code, id);
    {
        std::lock_guard<std::mutex> lock(g_expM);
        g_online.erase(id);
    }
    forgetBoxes(code);
    forgetSounds(code);
    forgetGravity(code);
    forgetTimeOfDay(code);
    forgetVisualEnv(code);
    layouts::forgetStreamingFocus(code);
    return hooksOut;
}

// Calls its gn_mod_load. The result is in m.info.
void start(Mod& m) {
    const std::string name = m.info.file;
    crash::refreshModules(); // the crash notes name frames in it (and whatever it loads, next time)
    auto entry = reinterpret_cast<gn_mod_load_fn>(reinterpret_cast<void*>(GetProcAddress(m.module, "gn_mod_load")));
    if (!m.info.reloadable) {
        // Never unloaded: their hooks and callbacks point into them.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(reinterpret_cast<void*>(entry)), &pinned);
    }
    {
        std::lock_guard<std::mutex> lock(g_expM);
        g_startingId = m.info.id, g_startingCode = m.code, g_startingPath = m.path;
    }
    const int r = entry(api());
    {
        std::lock_guard<std::mutex> lock(g_expM);
        g_startingId.clear(), g_startingCode = CodeRange{}, g_startingPath.clear();
    }
    m.info.loaded = r == GN_OK, m.info.result = r;
    const std::string who = m.info.version.empty() ? m.info.id : m.info.id + " " + m.info.version;
    if (r == GN_OK) {
        GN_LOG("mods: %s loaded (%s)%s", name.c_str(), who.c_str(), m.info.reloadable ? ", reloadable" : "");
    } else {
        // What it set up before failing goes: a failed mod is not left half running (its detours stayed in, 2026-11-03).
        const size_t hooks = takeOut(m.code, m.info.id, nullptr);
        GN_LOG("mods: %s returned %d from gn_mod_load; what it had set up is taken out (%llu hooks)%s", name.c_str(), r,
               static_cast<unsigned long long>(hooks), r == 2 ? ": it was built for a newer GridNode (GN_MOD returns 2 then)" : "");
        if (r == 2) m.info.status = "needs a newer GridNode";
    }
}

// The loaded mods other than `self` (g_modsM held): for a reload's dependency check.
std::string missingDep(const Mod& m) {
    for (const modorder::Dep& d : m.deps) {
        if (d.soft) continue;
        const Mod* found = nullptr;
        for (const Mod& o : g_mods)
            if (o.info.loaded && o.info.id == d.id && o.info.file != m.info.file) found = &o;
        if (!found) return "needs " + d.id + ", which is not loaded";
        if (!d.minVersion.empty() && modorder::compareVersions(found->info.version, d.minVersion) < 0)
            return "needs " + d.id + " " + d.minVersion + " or newer (it is " + found->info.version + ")";
    }
    return {};
}

// Opens and starts one mod on its own (a reload, the menu's Load): its dependencies must be loaded.
void load(Mod& m) {
    if (!openMod(m)) return;
    std::string why;
    {
        std::lock_guard<std::mutex> lock(g_modsM);
        why = missingDep(m);
    }
    if (!why.empty()) return closeMod(m, why);
    start(m);
}

// The loaded mods that need m (a hard dependency, or a function imported from it); g_modsM not held.
std::string dependentsOf(const Mod& m) {
    std::set<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(g_expM);
        if (auto it = g_importers.find(m.info.id); it != g_importers.end()) ids = it->second;
    }
    std::string out;
    std::lock_guard<std::mutex> lock(g_modsM);
    for (const Mod& o : g_mods) {
        if (!o.module || o.info.file == m.info.file) continue;
        bool needs = ids.count(o.info.id) != 0;
        for (const modorder::Dep& d : o.deps) needs = needs || (!d.soft && d.id == m.info.id);
        if (needs) out += (out.empty() ? "" : ", ") + o.info.id;
    }
    return out;
}

void forgetExports(const CodeRange& r, const std::string& id) {
    std::lock_guard<std::mutex> lock(g_expM);
    for (auto it = g_exports.begin(); it != g_exports.end();) it = r.has(it->second.fn) ? g_exports.erase(it) : std::next(it);
    g_importers.erase(id);
    for (auto& [exporter, importers] : g_importers) importers.erase(id);
}

// Takes the mod out and frees it; false (and it stays loaded, inert or not) when that is not safe.
bool unload(Mod& m, bool all = false) {
    const std::string name = m.info.file;
    if (!m.module) return true;
    if (m.inert) { // everything of it is out already: only the free is left
        if (!waitCodeQuiet(m.module, ("mods: " + name).c_str(), 1000)) return false;
        forgetTasks(imageOf(m.module)); // queued by a thread that was still in its code
        FreeLibrary(m.module);
        m.module = nullptr, m.code = CodeRange{}, m.inert = false;
        m.info.status = "unloaded (Load brings it back)";
        GN_LOG("mods: %s freed", name.c_str());
        return true;
    }
    if (!all) {
        // Mods that call into it would be left calling freed code.
        if (const std::string deps = dependentsOf(m); !deps.empty()) {
            m.info.status = "kept loaded: " + deps + " need" + (deps.find(',') == std::string::npos ? "s" : "") + " it (unload them first)";
            GN_LOG("mods: %s %s", name.c_str(), m.info.status.c_str());
            return false;
        }
    }
    // Only a mod whose gn_mod_load succeeded is asked to stop: one that refused to load (an older
    // GridNode than it needs) has no API to stop with, and its unload crashed on that (2026-11-04).
    auto stop = m.info.loaded ? reinterpret_cast<UnloadFn>(reinterpret_cast<void*>(GetProcAddress(m.module, "gn_mod_unload"))) : nullptr;
    if (stop) {
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
    const size_t hooksOut = takeOut(code, m.info.id, &refused);
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
    // A game thread may still be in a detour of it (its original running a long time), or in a thread
    // the mod started: only freed once no thread runs there or returns there.
    if (!waitCodeQuiet(m.module, ("mods: " + name).c_str(), 3000)) {
        m.inert = true;
        m.info.loaded = false;
        m.info.status = "taken out, but a thread was still in its code: kept in memory (Unload tries again)";
        GN_LOG("mods: %s %s", name.c_str(), m.info.status.c_str());
        forgetExports(code, m.info.id);
        return false;
    }
    // Its queued tasks ran during the wait above: gn_mod_unload posts the ones that put the game
    // back (the trainer's time scale and god mode), so they are not taken out with its callbacks
    // (taking them out at once left those undone, 2026-11-04). Only what a realm that was not
    // ticking still holds goes now, before the free: nothing can queue more, no thread is in its code.
    forgetTasks(code);
    FreeLibrary(m.module);
    m.module = nullptr;
    m.code = CodeRange{};
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

// ---- mods that hook the same place -----------------------------------------------------------------
// Hooks on one slot chain (each calls the one below), so two mods there both run; but one may skip
// the other (returning early) or both change the same thing. Said in the log when it happens and in
// the Mods list, so a report names both mods.
namespace {
bool modOwner(const std::string& owner) { // GridNode's own parts hook under these names
    static const std::set<std::string> ours{"gridnode", "GridNode", "events", "layouts", "recorder", "recorder-trace"};
    return !ours.count(owner);
}

std::string placeName(uint64_t slot) {
    char b[48];
    if (const uint64_t fn = detourFunction(slot)) snprintf(b, sizeof(b), "function 0x%llx", static_cast<unsigned long long>(fn));
    else snprintf(b, sizeof(b), "slot 0x%llx", static_cast<unsigned long long>(mem::va(slot)));
    return b;
}
} // namespace

void noteSharedHook(const std::string& mod, uint64_t slot) {
    const std::vector<std::string> others = hooks().othersAt(slot, mod, modOwner);
    if (others.empty()) return;
    std::string with;
    for (const std::string& o : others) with += (with.empty() ? "" : ", ") + o;
    GN_LOG("[%s] shares %s with %s: both run (the last hooked first); if one of them misbehaves, try without the other",
           mod.c_str(), placeName(slot).c_str(), with.c_str());
}

std::vector<ModInfo> modList() {
    std::vector<ModInfo> out;
    {
        std::lock_guard<std::mutex> lock(g_modsM);
        for (const Mod& m : g_mods) {
            out.push_back(m.info);
            out.back().inMemory = m.module != nullptr;
        }
    }
    std::lock_guard<std::mutex> lock(g_expM); // not nested in g_modsM
    for (ModInfo& i : out)
        if (const auto it = g_online.find(i.id); it != g_online.end() && i.loaded) i.online = it->second;
    // Hook owners are the names mods give GN_MOD, usually their id.
    for (const hook::HookTable::Shared& sh : hooks().shared(modOwner))
        for (ModInfo& i : out) {
            if (!i.loaded || std::find(sh.owners.begin(), sh.owners.end(), i.id) == sh.owners.end()) continue;
            std::string with;
            for (const std::string& o : sh.owners)
                if (o != i.id) with += (with.empty() ? "" : ", ") + o;
            i.shares.push_back(placeName(sh.slot) + " with " + with);
        }
    return out;
}

void modReload(const std::string& file) {
    std::lock_guard<std::mutex> lock(g_modsM);
    for (Mod& m : g_mods)
        if (m.info.file == file && m.info.reloadable) m.reloadWanted = true;
}

void modUnload(const std::string& file) {
    std::lock_guard<std::mutex> lock(g_modsM);
    for (Mod& m : g_mods)
        if (m.info.file == file && m.info.reloadable && m.module) m.unloadWanted = true;
}

void modLoad(const std::string& file) {
    std::lock_guard<std::mutex> lock(g_modsM);
    for (Mod& m : g_mods)
        if (m.info.file == file && !m.module) m.loadWanted = true;
}

int modUnloadAt(const void* address) {
    std::lock_guard<std::mutex> lock(g_modsM);
    for (Mod& m : g_mods) {
        if (!m.module || !m.code.has(address)) continue;
        if (!m.info.reloadable) return GN_ERR_UNAVAILABLE;
        m.unloadWanted = true;
        return GN_OK;
    }
    return GN_ERR_NOT_FOUND;
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
            if (m.unloadWanted || m.loadWanted) {
                work = m;
                m.unloadWanted = m.loadWanted = false;
            } else {
                if (!m.info.reloadable || !m.module) continue;
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
        }
        // Without the lock: the reload waits for frames, and the menu reads the list every frame.
        if (work.unloadWanted) {
            work.unloadWanted = false;
            GN_LOG("mods: unloading %s", work.info.file.c_str());
            if (unload(work)) work.info.status = "unloaded (Load brings it back)";
        } else if (work.loadWanted) {
            work.loadWanted = false;
            load(work);
            work.info.status = work.info.loaded ? "loaded again" : "load failed";
        } else {
            reload(work);
        }
        // Requests made while this ran (the menu's Reload clicked twice, the mod asking to unload,
        // Auto reload turned on) are kept, not overwritten by the copy: the copy's own were taken
        // (cleared) under the lock above, so any set now are new.
        std::lock_guard<std::mutex> lock(g_modsM);
        Mod& cur = g_mods[i];
        const bool reloadWanted = cur.reloadWanted, unloadWanted = cur.unloadWanted, loadWanted = cur.loadWanted;
        const bool autoReload = cur.info.autoReload;
        cur = std::move(work);
        cur.info.autoReload = autoReload;
        cur.reloadWanted = reloadWanted && cur.module; // only what still applies to the new state
        cur.unloadWanted = unloadWanted && cur.module;
        cur.loadWanted = loadWanted && !cur.module;
    }
}

bool unloadAllMods() {
    std::vector<Mod> mods;
    {
        std::lock_guard<std::mutex> lock(g_modsM);
        mods = g_mods;
    }
    bool allFreed = true;
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) { // in load order: dependents first, going back
        Mod& m = *it;
        if (!m.module) continue;
        if (m.info.reloadable) {
            if (!unload(m, true)) allFreed = false;
            continue;
        }
        // Pinned (no gn_mod_unload): it cannot be freed or asked to stop. Everything it registered goes,
        // so GridNode no longer calls it; it may still have its own threads, so GridNode stays loaded.
        const CodeRange code = imageOf(m.module);
        takeOut(code, m.info.id, nullptr);
        m.info.loaded = false;
        m.info.status = "taken out (no gn_mod_unload: it stays in memory)";
        GN_LOG("mods: %s has no gn_mod_unload: its callbacks and hooks are out, but it stays in memory", m.info.file.c_str());
        allFreed = false;
    }
    configFlush(true);
    std::lock_guard<std::mutex> lock(g_modsM);
    g_mods = std::move(mods);
    return allFreed;
}

namespace {
// The mod whose code holds `caller`: the one being started, or a loaded one.
bool modAtAddress(const void* caller, std::string& id, CodeRange& code) {
    {
        std::lock_guard<std::mutex> lock(g_expM);
        if (!g_startingId.empty() && g_startingCode.has(caller)) {
            id = g_startingId, code = g_startingCode;
            return true;
        }
    }
    std::lock_guard<std::mutex> lock(g_modsM);
    for (const Mod& m : g_mods)
        if (m.module && m.code.has(caller)) {
            id = m.info.id, code = m.code;
            return true;
        }
    return false;
}
} // namespace

int modDir(const void* caller, char* out, size_t size) {
    if (!out || !size) return GN_ERR_ARGUMENT;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(g_expM);
        if (!g_startingId.empty() && g_startingCode.has(caller)) path = g_startingPath;
    }
    if (path.empty()) {
        std::lock_guard<std::mutex> lock(g_modsM);
        for (const Mod& m : g_mods)
            if (m.module && m.code.has(caller)) path = m.path;
    }
    if (path.empty()) return GN_ERR_NOT_FOUND;
    const std::string dir = narrow(path.substr(0, path.find_last_of(L"\\/") + 1));
    if (dir.size() >= size) return GN_ERR_FULL;
    memcpy(out, dir.c_str(), dir.size() + 1);
    return GN_OK;
}

int modExport(const void* caller, const char* name, void* fn) {
    if (!name || !*name || !fn) return GN_ERR_ARGUMENT;
    std::string id;
    CodeRange code;
    modAtAddress(caller, id, code);
    if (id.empty()) return GN_ERR_NOT_FOUND;
    if (!code.has(fn)) return GN_ERR_ARGUMENT; // only its own code: taken out when it is unloaded
    std::lock_guard<std::mutex> lock(g_expM);
    g_exports[id + "." + name] = {fn, code};
    return GN_OK;
}

void* modImport(const void* caller, const char* modId, const char* name) {
    if (!modId || !name) return nullptr;
    std::string me;
    CodeRange unused;
    modAtAddress(caller, me, unused);
    std::lock_guard<std::mutex> lock(g_expM);
    auto it = g_exports.find(std::string(modId) + "." + name);
    if (it == g_exports.end()) return nullptr;
    if (!me.empty() && me != modId) g_importers[modId].insert(me); // it now needs that mod loaded
    return it->second.fn;
}

std::string modNameAt(const void* caller) {
    std::string id;
    CodeRange unused;
    modAtAddress(caller, id, unused);
    return id;
}

int onlineAllow(const void* caller, const char* mod, const char* reason) {
    if (!reason || !*reason) return GN_ERR_ARGUMENT;
    std::string id;
    CodeRange unused;
    if (!modAtAddress(caller, id, unused)) return GN_ERR_ARGUMENT; // only a mod declares this
    const std::string why(reason, strnlen(reason, 200));
    {
        std::lock_guard<std::mutex> lock(g_expM);
        if (const auto it = g_online.find(id); it != g_online.end() && it->second == why) return GN_OK;
        g_online[id] = why;
    }
    GN_LOG("!!! ONLINE: mod %s (%s) changes online services on purpose: %s", id.c_str(), mod ? mod : "?", why.c_str());
    GN_LOG("!!! ONLINE: its online console commands are allowed from now on, and every one is logged.");
    GN_LOG("!!! ONLINE: this can affect your EA account, online stats and leaderboards, and services such as Beat Revival.");
    GN_LOG("!!! ONLINE: use it only if you trust this mod. Cheating on leaderboards is not what this is for.");
    return GN_OK;
}

bool onlineAllowed(const void* caller) {
    std::string id;
    CodeRange unused;
    if (!modAtAddress(caller, id, unused)) return true; // GridNode's own code (the menu asks the player itself)
    std::lock_guard<std::mutex> lock(g_expM);
    return g_online.count(id) != 0;
}

bool modCodeLive(const void* fn) {
    std::lock_guard<std::mutex> lock(g_modsM);
    for (const Mod& m : g_mods)
        if (m.module && m.code.has(fn)) return m.info.loaded && !m.inert;
    return true; // not a mod's: GridNode's own, or the game's
}

int modVersion(const char* modId, char* out, size_t size) {
    if (!modId) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_modsM);
    for (const Mod& m : g_mods)
        if (m.info.loaded && m.info.id == modId) {
            if (out && size) snprintf(out, size, "%s", m.info.version.c_str());
            return GN_OK;
        }
    return GN_ERR_NOT_FOUND;
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
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // A mod in its own folder (as a release zip unpacks): mods\<name>\<name>.dll, its files beside it.
    h = FindFirstFileW((g_folder + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring d = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.empty() || d[0] == L'.') continue;
            const std::wstring dll = d + L"\\" + d + L".dll";
            if (GetFileAttributesW((g_folder + dll).c_str()) != INVALID_FILE_ATTRIBUTES) files.push_back(dll);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        GN_LOG("mods: none (no DLLs in %s or its folders)", narrow(g_folder).c_str());
        return;
    }

    // Every DLL opened (manifests read), then started dependencies first.
    std::vector<Mod> opened;
    for (const std::wstring& f : files) {
        Mod m;
        m.info.file = narrow(f);
        m.path = g_folder + f;
        if (openMod(m)) opened.push_back(std::move(m));
    }
    std::vector<modorder::Entry> entries;
    for (const Mod& m : opened) entries.push_back({m.info.id, m.info.version, m.deps});
    const modorder::Plan plan = modorder::plan(entries);
    int loaded = 0;
    std::vector<bool> placed(opened.size(), false);
    for (size_t i : plan.order) {
        placed[i] = true;
        start(opened[i]);
        if (opened[i].info.loaded) ++loaded;
        std::lock_guard<std::mutex> lock(g_modsM);
        g_mods.push_back(opened[i]); // in load order (a full unload goes back through it)
    }
    for (size_t i = 0; i < opened.size(); ++i) {
        if (placed[i]) continue;
        closeMod(opened[i], plan.problem[i]);
        std::lock_guard<std::mutex> lock(g_modsM);
        g_mods.push_back(opened[i]);
    }
    GN_LOG("mods: %d of %llu loaded", loaded, static_cast<unsigned long long>(files.size()));
}

} // namespace gn::host
