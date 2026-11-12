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
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <map>
#include <set>

#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/mod_call.hpp"
#include "gridnode/mod_order.hpp"
#include "gridnode/pe_file.hpp"
#include "host.hpp"

// A mod's handle (gn_mod, API 37): GridNode's own record of one load of a mod, handed to
// gn_mod_load2. Never freed (a mod may keep it and call with it after it was unloaded: then refused);
// a reload makes a new one.
struct gn_mod {
    std::string id;
    gn::host::CodeRange code;
    std::atomic<bool> live{true};
};

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
    gn_mod* handle = nullptr;  // given to its gn_mod_load2 (null for gn_mod_load only)
    std::string backAfter;     // unloaded for this mod's unload or reload: loaded again once that is done
    bool depsAsked = false;    // its optional dependents were asked to unload first (once per request)
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

std::mutex g_handlesM;
std::vector<std::unique_ptr<gn_mod>> g_handles; // (g_handlesM) every handle ever given out (never freed)

std::mutex g_modsM;
std::vector<Mod> g_mods; // guarded by g_modsM; changed only on GridNode's thread
std::wstring g_folder;   // the mods folder, with a trailing backslash

FILETIME writeTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return FILETIME{};
    return a.ftLastWriteTime;
}
bool sameTime(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

// Whether the DLL exports `name`, read from the file (no DllMain, no imports, nothing mapped).
bool fileExports(const std::wstring& path, const char* name) {
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
    return pe::exportsName(bytes.data(), bytes.size(), name);
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
    // Not a mod: never loaded, so its DllMain does not run (every DLL in mods\ was loaded first, 2026-11-11).
    if (!fileExports(m.path, "gn_mod_load")) {
        GN_LOG("mods: %s does not export gn_mod_load; skipped", name.c_str());
        m.info.loaded = false, m.info.result = -1;
        return false;
    }
    m.info.reloadable = fileExports(m.path, "gn_mod_unload");
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
    // A mod may bring DLLs of its own beside it: found from its folder, mods\ itself included (the
    // copy it runs from is in .loaded, where a mod in the root of mods\ did not find them, 2026-11-11).
    const std::wstring own = m.path.substr(0, m.path.find_last_of(L"\\/") + 1);
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(own.c_str());
    m.module = LoadLibraryExW(from.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (cookie) RemoveDllDirectory(cookie);
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
    {
        std::lock_guard<std::mutex> lock(g_handlesM);
        for (const auto& h : g_handles)
            if (h->code.begin == code.begin && h->id == id) h->live = false; // calls with it are refused from now
    }
    const size_t hooksOut = forgetHooks(code, refused);
    forgetVirtualHooks(code);
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
    forgetAudioMix(code);
    forgetGravity(code);
    forgetTimeOfDay(code);
    forgetVisualEnv(code);
    layouts::forgetStreamingFocus(code);
    forgetBundles(code);
    return hooksOut;
}

// Calls its gn_mod_load. The result is in m.info.
void start(Mod& m) {
    const std::string name = m.info.file;
    crash::refreshModules(); // the crash notes name frames in it (and whatever it loads, next time)
    auto entry = reinterpret_cast<gn_mod_load_fn>(reinterpret_cast<void*>(GetProcAddress(m.module, "gn_mod_load")));
    // gn_mod_load2 (API 37) is preferred: the mod gets its handle, and calls made with it need no
    // name and no return address.
    auto entry2 = reinterpret_cast<gn_mod_load2_fn>(reinterpret_cast<void*>(GetProcAddress(m.module, "gn_mod_load2")));
    gn_mod* self = nullptr;
    if (entry2) {
        auto rec = std::make_unique<gn_mod>();
        rec->id = m.info.id, rec->code = m.code;
        self = rec.get();
        std::lock_guard<std::mutex> lock(g_handlesM);
        g_handles.push_back(std::move(rec));
    }
    m.handle = self;
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
    int r;
    {
        modcall::Scope scope(entry); // its gn_mod_load may tail-call the API too
        // Its function detours are written together when gn_mod_load returns: one pause of the game
        // instead of one per detour (API 37; 7 detours paused it 7 times, 2026-11-11).
        hooksBegin();
        r = entry2 ? entry2(api(), self) : entry(api());
        hooksCommit();
    }
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

// The loaded mods that need m only optionally (it is a soft dependency of theirs, or they imported
// from it without a hard dependency): unloaded before it and loaded again after, so each comes back
// as it would without m (or, after a reload, with the new m). False if none, or if one needs m for
// sure or cannot be unloaded: then the unload is refused as before. g_modsM not held.
bool optionalDependents(const Mod& m, std::vector<std::string>& files) {
    std::set<std::string> importers;
    {
        std::lock_guard<std::mutex> lock(g_expM);
        if (auto it = g_importers.find(m.info.id); it != g_importers.end()) importers = it->second;
    }
    std::lock_guard<std::mutex> lock(g_modsM);
    for (const Mod& o : g_mods) {
        if (!o.module || o.info.file == m.info.file) continue;
        bool needs = importers.count(o.info.id) != 0, hard = false;
        for (const modorder::Dep& d : o.deps)
            if (d.id == m.info.id) needs = true, hard = hard || !d.soft;
        if (!needs) continue;
        if (hard || !o.info.reloadable) return false;
        files.push_back(o.info.file);
    }
    return !files.empty();
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

// The mod DLLs in g_folder, sorted: mods\*.dll, and a mod in its own folder (as a release zip unpacks):
// mods\<name>\<name>.dll, its files beside it.
std::vector<std::wstring> modFiles() {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_folder + L"*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
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
    return files;
}

std::atomic<bool> g_reloadAllWanted{false};

// The menu's Reload all (GridNode's thread): every loaded mod that can be reloaded is, and DLLs put in the
// mods folder since the game started are loaded (their dependencies must be loaded or come first by name).
// A mod without gn_mod_unload cannot be reloaded: it stays as it is.
void reloadAll() {
    const std::vector<std::wstring> files = modFiles();
    std::lock_guard<std::mutex> lock(g_modsM);
    int reloads = 0, added = 0, pinned = 0;
    for (Mod& m : g_mods) {
        if (!m.module) continue;
        if (m.info.reloadable) m.reloadWanted = true, ++reloads;
        else ++pinned;
    }
    for (const std::wstring& f : files) {
        const std::string file = narrow(f);
        if (std::any_of(g_mods.begin(), g_mods.end(), [&](const Mod& m) { return m.info.file == file; })) continue;
        Mod m;
        m.info.file = file;
        m.path = g_folder + f;
        m.loadWanted = true;
        g_mods.push_back(std::move(m));
        ++added;
    }
    GN_LOG("mods: reload all: %d reloaded, %d new loaded%s", reloads, added,
           pinned ? (", " + std::to_string(pinned) + " kept as they are (no gn_mod_unload: a restart reloads them)").c_str() : "");
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

void modReloadAll() { g_reloadAllWanted = true; }

void pollMods() {
    if (g_reloadAllWanted.exchange(false)) reloadAll();
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
        // Mods that need it only optionally go first (and come back after), rather than blocking it.
        const bool loadOnly = work.loadWanted && !work.unloadWanted;
        std::vector<std::string> optional;
        if (work.module && !loadOnly && !work.depsAsked && optionalDependents(work, optional)) {
            std::string names;
            std::lock_guard<std::mutex> lock(g_modsM);
            for (Mod& o : g_mods)
                if (std::find(optional.begin(), optional.end(), o.info.file) != optional.end()) {
                    o.unloadWanted = true, o.backAfter = work.info.id;
                    names += (names.empty() ? "" : ", ") + o.info.file;
                }
            Mod& cur = g_mods[i];
            cur.depsAsked = true;
            if (work.unloadWanted) cur.unloadWanted = true;
            else cur.reloadWanted = true;
            GN_LOG("mods: %s: first unloading %s (%s it only optionally), then loading %s again", work.info.file.c_str(), names.c_str(),
                   optional.size() == 1 ? "needs" : "need", optional.size() == 1 ? "it" : "them");
            continue;
        }
        // Without the lock: the reload waits for frames, and the menu reads the list every frame.
        if (work.unloadWanted) {
            work.unloadWanted = false;
            GN_LOG("mods: unloading %s", work.info.file.c_str());
            if (unload(work)) work.info.status = "unloaded (Load brings it back)";
        } else if (work.loadWanted) {
            work.loadWanted = false;
            const bool fresh = work.info.id.empty(); // put in the folder since the game started (Reload all)
            load(work);
            work.info.status = work.info.loaded ? (fresh ? "loaded (new)" : "loaded again") : "load failed";
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
        // Done with it: the mods unloaded for it come back, and a mod just loaded is picked up by the
        // loaded mods that want it optionally (they look for it when they load: reloaded to see it).
        cur.depsAsked = false;
        const std::string id = cur.info.id;
        const bool nowLoaded = cur.module && cur.info.loaded;
        std::set<std::string> importers;
        if (loadOnly && nowLoaded) {
            std::lock_guard<std::mutex> el(g_expM);
            if (auto it = g_importers.find(id); it != g_importers.end()) importers = it->second;
        }
        for (Mod& o : g_mods) {
            if (o.backAfter == id) {
                o.backAfter.clear();
                if (!o.module) o.loadWanted = true;
                continue;
            }
            if (!loadOnly || !nowLoaded || !o.module || !o.info.loaded || !o.info.reloadable || importers.count(o.info.id)) continue;
            for (const modorder::Dep& d : o.deps)
                if (d.soft && d.id == id && !o.reloadWanted) {
                    o.reloadWanted = true;
                    GN_LOG("mods: %s wants %s optionally: reloading it so it sees it", o.info.file.c_str(), cur.info.file.c_str());
                }
        }
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

bool modContext(const gn_mod* m, std::string& id, const void*& caller) {
    if (!m) return false;
    std::lock_guard<std::mutex> lock(g_handlesM);
    for (const auto& h : g_handles)
        if (h.get() == m) {
            if (!h->live) return false;
            id = h->id;
            caller = reinterpret_cast<const void*>(h->code.begin); // an address in its code: owns what the call makes
            return true;
        }
    return false;
}

const void* callerOf(const void* ret) {
    static const CodeRange self = [] {
        HMODULE me = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(reinterpret_cast<const void*>(&callerOf)), &me);
        return me ? imageOf(me) : CodeRange{};
    }();
    return modcall::resolve(ret, [](const void* p) { return self.has(p); });
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
    const std::vector<std::wstring> files = modFiles();
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
        // A hard dependency whose gn_mod_load failed: not started, as load() does (it started anyway).
        std::string why;
        {
            std::lock_guard<std::mutex> lock(g_modsM);
            why = missingDep(opened[i]);
        }
        if (!why.empty()) {
            closeMod(opened[i], why);
            std::lock_guard<std::mutex> lock(g_modsM);
            g_mods.push_back(opened[i]);
            continue;
        }
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
