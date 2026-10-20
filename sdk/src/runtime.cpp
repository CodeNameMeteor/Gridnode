#include "runtime.hpp"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>

#include "gridnode/build.hpp"
#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/reflect/dump_json.hpp"
#include "gridnode/reflect/layout.hpp"
#include "gridnode/reflect/walker.hpp"
#include "gridnode/version.hpp"
#include "host.hpp"

namespace gn::runtime {
namespace {

std::atomic<bool> g_stop{false};

std::wstring moduleDir(HMODULE m) {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// gridnode.ini next to gridnode.dll:
//   [dump]
//   mode=auto     ; auto: write the dump when the file is missing; always; never
//   file=gridnode-reflected.json
//   [mods]
//   enabled=1
//   folder=mods   ; relative to gridnode.dll, or absolute
//   [ui]
//   enabled=1     ; the Dear ImGui overlay (mods' windows need it)
//   menu_key=INSERT
//   scale=1.0
//   inspector=1   ; the inspector window
//   [tools]       ; developer tools (a player build has none of them)
//   editor=1      ; Layout editor, free camera and their keys (saved layouts load either way)
//   recorder=1
//   console=1
//   dev_info=1    ; the menu's World and Hooks sections, Dear ImGui metrics
struct Settings {
    std::wstring dumpMode = L"auto";
    std::wstring dumpFile;
    bool modsEnabled = true;
    std::wstring modsFolder;
    host::ui::Settings ui;
};

Settings readSettings(const std::wstring& dir) {
    Settings s;
    const std::wstring ini = dir + L"gridnode.ini";
    wchar_t buf[MAX_PATH];
#if GRIDNODE_PLAYER_BUILD
    GetPrivateProfileStringW(L"dump", L"mode", L"never", buf, MAX_PATH, ini.c_str()); // players do not need the 4 MB type dump
#else
    GetPrivateProfileStringW(L"dump", L"mode", L"auto", buf, MAX_PATH, ini.c_str());
#endif
    s.dumpMode = buf;
    GetPrivateProfileStringW(L"dump", L"file", L"gridnode-reflected.json", buf, MAX_PATH, ini.c_str());
    const std::wstring file = buf;
    const bool absolute = file.size() > 2 && (file[1] == L':' || (file[0] == L'\\' && file[1] == L'\\'));
    s.dumpFile = absolute ? file : dir + file;
    s.modsEnabled = GetPrivateProfileIntW(L"mods", L"enabled", 1, ini.c_str()) != 0;
    GetPrivateProfileStringW(L"mods", L"folder", L"mods", buf, MAX_PATH, ini.c_str());
    std::wstring mods = buf;
    const bool modsAbsolute = mods.size() > 2 && (mods[1] == L':' || (mods[0] == L'\\' && mods[1] == L'\\'));
    if (!modsAbsolute) mods = dir + mods;
    if (!mods.empty() && mods.back() != L'\\' && mods.back() != L'/') mods += L'\\';
    s.modsFolder = mods;
    s.ui.enabled = GetPrivateProfileIntW(L"ui", L"enabled", 1, ini.c_str()) != 0;
#if GRIDNODE_PLAYER_BUILD
    constexpr int kTools = 0; // a player build: the loader, the API and mods' windows only
#else
    constexpr int kTools = 1;
#endif
    auto tool = [&](const wchar_t* section, const wchar_t* key) { return kTools && GetPrivateProfileIntW(section, key, 1, ini.c_str()) != 0; };
    s.ui.inspector = tool(L"ui", L"inspector");
    s.ui.editor = tool(L"tools", L"editor");
    s.ui.recorder = tool(L"tools", L"recorder");
    s.ui.console = tool(L"tools", L"console");
    s.ui.devInfo = tool(L"tools", L"dev_info");
    GetPrivateProfileStringW(L"ui", L"menu_key", L"INSERT", buf, MAX_PATH, ini.c_str());
    const int key = host::ui::keyFromName(narrow(buf));
    if (!key) GN_LOG("gridnode.ini: menu_key '%s' is not a key; INSERT opens the menu", narrow(buf).c_str());
    s.ui.menuKey = key ? key : VK_INSERT;
    GetPrivateProfileStringW(L"ui", L"scale", L"1.0", buf, MAX_PATH, ini.c_str());
    s.ui.scale = static_cast<float>(_wtof(buf));
    if (!(s.ui.scale > 0.f)) s.ui.scale = 1.f;
    return s;
}

std::string utcNow() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char b[32];
    snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

std::string exeName() {
    const std::wstring dir = moduleDir(nullptr);
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return n ? narrow(std::wstring(path + dir.size(), n - dir.size())) : std::string();
}

// One line per section: range, code flag, and how its pages are protected now (summed over
// VirtualQuery regions, so it is cheap). In the running game every page of the image is
// executable, which is why GridNode goes by the section table instead.
void logImageLayout() {
    for (const mem::Section& s : mem::imageSections()) {
        unsigned long long exec = 0, data = 0, other = 0;
        for (uintptr_t a = s.region.begin; a < s.region.end;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) || !mbi.RegionSize) break;
            const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t re = rb + mbi.RegionSize;
            if (re > s.region.end) re = s.region.end;
            const unsigned long long pages = (re - a + 0xfff) / 0x1000;
            const DWORD pr = mbi.Protect;
            if (mbi.State != MEM_COMMIT || (pr & (PAGE_GUARD | PAGE_NOACCESS))) other += pages;
            else if (pr & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) exec += pages;
            else data += pages;
            a = re;
        }
        GN_LOG("image: %-8s 0x%llx-0x%llx %s  pages: exec %llu, data %llu, other %llu", s.name,
               static_cast<unsigned long long>(mem::va(s.region.begin)), static_cast<unsigned long long>(mem::va(s.region.end)),
               s.region.code ? "code" : "data", exec, data, other);
    }
}

reflect::Reflected walkTypes() {
    const DWORD t0 = GetTickCount();
    mem::ProcessMemory m;
    reflect::WalkOptions opt;
    opt.listHead = mem::rva(layout::kTypeInfoListHead);
    opt.regions = mem::imageSectionRegions();
    reflect::Reflected r = reflect::walk(m, opt);
    const reflect::WalkStats& s = r.stats;
    using ull = unsigned long long;
    GN_LOG("types: %llu (%llu listed), %llu accessors, vtables %llu from ClassInfo + %llu from messages + %llu from data scan "
           "(%llu ambiguous), %llu creators, %llu short ClassInfos, %llu unknown category, %llu unreadable scan pages, in %lu ms",
           ull(r.types.size()), ull(s.listed), ull(s.accessors), ull(s.vtablesFromClassInfo), ull(s.vtablesFromStaticInstance), ull(s.vtablesFromDataScan),
           ull(s.vtableCandidatesAmbiguous), ull(s.creators), ull(s.shortClassInfos), ull(s.unknownCategory), ull(s.unreadablePages),
           static_cast<unsigned long>(GetTickCount() - t0));
    return r;
}

bool writeDump(const reflect::Reflected& r, const std::wstring& file) {
    reflect::DumpInfo info;
    const IMAGE_NT_HEADERS* nt = mem::ntHeaders();
    info.image = {exeName(), mem::moduleBase(), layout::kImageBase, nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp};
    info.generator = "gridnode " GRIDNODE_VERSION_STRING;
    info.createdUtc = utcNow();

    // Write to a temporary file and rename, so a crash never leaves half a dump behind.
    const std::wstring tmp = file + L".tmp";
    std::ostringstream os;
    reflect::writeJson(os, r, info);
    const std::string json = os.str();
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) {
        GN_LOG("dump: cannot write %s", narrow(tmp).c_str());
        return false;
    }
    const bool written = fwrite(json.data(), 1, json.size(), f) == json.size();
    if (fclose(f) != 0 || !written) {
        GN_LOG("dump: write to %s failed", narrow(tmp).c_str());
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        GN_LOG("dump: cannot rename to %s (error %lu)", narrow(file).c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    GN_LOG("dump: written to %s", narrow(file).c_str());
    return true;
}

} // namespace

void run(HMODULE self) {
    log::open(self);
    GN_LOG("GridNode %s loading into %s (base 0x%llx)", GRIDNODE_VERSION_STRING, exeName().c_str(),
           static_cast<unsigned long long>(mem::moduleBase()));

    const size_t typeCount = build::waitForTypeList(5 * 60 * 1000, &g_stop);
    if (!typeCount) {
        GN_LOG("startup: the engine's type list never filled; GridNode stays passive");
        return;
    }
    GN_LOG("startup: engine type list ready (%llu types)", static_cast<unsigned long long>(typeCount));

    const build::Result b = build::check();
    if (!b.details.empty()) GN_LOG("build check:\n%s", b.details.c_str());
    if (!b.ok) {
        GN_LOG("startup: this is not the game build GridNode supports (%d check(s) failed); GridNode stays passive", b.failed);
        return;
    }
    GN_LOG("startup: build check passed%s", b.warnings ? " (with warnings)" : "");

    const Settings s = readSettings(moduleDir(self));
    logImageLayout();
    reflect::Reflected types = walkTypes();
    const bool exists = GetFileAttributesW(s.dumpFile.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool dumping = s.dumpMode == L"always" || (s.dumpMode == L"auto" && !exists);
    size_t dumpedCreators = types.stats.creators;
    if (dumping) writeDump(types, s.dumpFile);
    else GN_LOG("dump: skipped (mode=%s, file %s)", narrow(s.dumpMode).c_str(), exists ? "exists" : "missing");
    host::registry().build(std::move(types));

    const host::TickStatus ticks = host::installTicks();
    GN_LOG("ticks: server %s, client %s", ticks.server ? "hooked" : "UNAVAILABLE", ticks.client ? "hooked" : "UNAVAILABLE");

    host::crash::install();
    host::worldStart();
    host::layouts::start(moduleDir(self));
    if (s.ui.recorder) host::recorder::start(moduleDir(self));
    host::messageTypes(); // reads the message types' static instances once (logged)
    host::inspector::start(); // entity tracking: the inspector shows it, layouts apply edits with it
    GN_LOG("startup: %s build; tools: inspector %d, editor %d, recorder %d, console %d",
           GRIDNODE_PLAYER_BUILD ? "player" : "developer", s.ui.inspector, s.ui.editor, s.ui.recorder, s.ui.console);
    host::ui::install(s.ui);

    if (s.modsEnabled) host::loadMods(s.modsFolder);
    else GN_LOG("mods: disabled in gridnode.ini");

    GN_LOG("ready (%llu hooks installed)", static_cast<unsigned long long>(host::hooks().count()));

    // The engine binds its entity creators when each realm starts, which can be after this point
    // (GridNode injected early): keep attaching on_created listeners as creators appear, and
    // rewrite this session's dump once the creator count has settled.
    size_t creators = dumpedCreators;
    DWORD changedAt = GetTickCount();
    for (unsigned i = 1; !g_stop; ++i) {
        Sleep(100);
        host::pollLevel();
        if (i % 10) continue;
        host::refreshCreators();
        const size_t n = host::boundCreatorCount();
        if (n != creators) {
            GN_LOG("creators: %llu bound (was %llu)", static_cast<unsigned long long>(n), static_cast<unsigned long long>(creators));
            creators = n;
            changedAt = GetTickCount();
        }
        if (dumping && creators != dumpedCreators && GetTickCount() - changedAt > 3000) {
            GN_LOG("dump: the engine bound %llu creators since the dump; writing it again",
                   static_cast<unsigned long long>(creators));
            writeDump(walkTypes(), s.dumpFile);
            dumpedCreators = creators;
        }
    }
}

void shutdown() {
    g_stop = true;
    log::close();
}

} // namespace gn::runtime
