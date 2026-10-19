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
struct Settings {
    std::wstring dumpMode = L"auto";
    std::wstring dumpFile;
};

Settings readSettings(const std::wstring& dir) {
    Settings s;
    const std::wstring ini = dir + L"gridnode.ini";
    wchar_t buf[MAX_PATH];
    GetPrivateProfileStringW(L"dump", L"mode", L"auto", buf, MAX_PATH, ini.c_str());
    s.dumpMode = buf;
    GetPrivateProfileStringW(L"dump", L"file", L"gridnode-reflected.json", buf, MAX_PATH, ini.c_str());
    const std::wstring file = buf;
    const bool absolute = file.size() > 2 && (file[1] == L':' || (file[0] == L'\\' && file[1] == L'\\'));
    s.dumpFile = absolute ? file : dir + file;
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

// One line per section: range, code flag, and how its pages are protected and readable now.
void logImageLayout() {
    for (const mem::Section& s : mem::imageSections()) {
        size_t pages = 0, unreadable = 0, exec = 0, data = 0, other = 0;
        uint8_t probe;
        for (uintptr_t p = s.region.begin; p < s.region.end; p += 0x1000) {
            ++pages;
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) {
                const DWORD pr = mbi.Protect;
                if (mbi.State != MEM_COMMIT || (pr & (PAGE_GUARD | PAGE_NOACCESS))) ++other;
                else if (pr & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ++exec;
                else ++data;
            }
            if (!mem::read(p, &probe, 1)) ++unreadable;
        }
        GN_LOG("image: %-8s 0x%llx-0x%llx %s  pages %llu: exec %llu, data %llu, other %llu, unreadable %llu", s.name,
               static_cast<unsigned long long>(mem::va(s.region.begin)), static_cast<unsigned long long>(mem::va(s.region.end)),
               s.region.code ? "code" : "data", static_cast<unsigned long long>(pages), static_cast<unsigned long long>(exec),
               static_cast<unsigned long long>(data), static_cast<unsigned long long>(other),
               static_cast<unsigned long long>(unreadable));
    }
}

bool writeDump(const std::wstring& file) {
    logImageLayout();
    const DWORD t0 = GetTickCount();
    mem::ProcessMemory m;
    reflect::WalkOptions opt;
    opt.listHead = mem::rva(layout::kTypeInfoListHead);
    opt.regions = mem::imageSectionRegions();
    const reflect::Reflected r = reflect::walk(m, opt);
    const reflect::WalkStats& s = r.stats;
    using ull = unsigned long long;
    GN_LOG("dump: %llu types (%llu listed), %llu accessors, vtables %llu from ClassInfo + %llu from messages + %llu from data scan "
           "(%llu ambiguous), %llu creators, %llu short ClassInfos, %llu unknown category, %llu unreadable scan pages, in %lu ms",
           ull(r.types.size()), ull(s.listed), ull(s.accessors), ull(s.vtablesFromClassInfo), ull(s.vtablesFromStaticInstance), ull(s.vtablesFromDataScan),
           ull(s.vtableCandidatesAmbiguous), ull(s.creators), ull(s.shortClassInfos), ull(s.unknownCategory), ull(s.unreadablePages),
           static_cast<unsigned long>(GetTickCount() - t0));

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

    const size_t types = build::waitForTypeList(5 * 60 * 1000, &g_stop);
    if (!types) {
        GN_LOG("startup: the engine's type list never filled; GridNode stays passive");
        return;
    }
    GN_LOG("startup: engine type list ready (%llu types)", static_cast<unsigned long long>(types));

    const build::Result b = build::check();
    if (!b.details.empty()) GN_LOG("build check:\n%s", b.details.c_str());
    if (!b.ok) {
        GN_LOG("startup: this is not the game build GridNode supports (%d check(s) failed); GridNode stays passive", b.failed);
        return;
    }
    GN_LOG("startup: build check passed%s", b.warnings ? " (with warnings)" : "");

    const Settings s = readSettings(moduleDir(self));
    const bool exists = GetFileAttributesW(s.dumpFile.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (s.dumpMode == L"always" || (s.dumpMode == L"auto" && !exists)) writeDump(s.dumpFile);
    else GN_LOG("dump: skipped (mode=%s, file %s)", narrow(s.dumpMode).c_str(), exists ? "exists" : "missing");

    GN_LOG("ready");
}

void shutdown() {
    g_stop = true;
    log::close();
}

} // namespace gn::runtime
