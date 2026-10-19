// Loads the mods: every DLL in the mods folder that exports gn_mod_load, in name order.

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "gridnode/log.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

} // namespace

void loadMods(const std::wstring& folder) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        GN_LOG("mods: none (no DLLs in %s)", narrow(folder).c_str());
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end());

    int loaded = 0;
    for (const std::wstring& f : files) {
        const std::string name = narrow(f);
        HMODULE m = LoadLibraryW((folder + f).c_str());
        if (!m) {
            GN_LOG("mods: %s could not be loaded (error %lu)", name.c_str(), static_cast<unsigned long>(GetLastError()));
            continue;
        }
        auto load = reinterpret_cast<gn_mod_load_fn>(reinterpret_cast<void*>(GetProcAddress(m, "gn_mod_load")));
        if (!load) {
            GN_LOG("mods: %s does not export gn_mod_load; skipped", name.c_str());
            FreeLibrary(m);
            continue;
        }
        // Mods are never unloaded: their hooks and callbacks point into them.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(reinterpret_cast<void*>(load)), &pinned);
        const int r = load(api());
        if (r == GN_OK) {
            ++loaded;
            GN_LOG("mods: %s loaded", name.c_str());
        } else {
            GN_LOG("mods: %s returned %d from gn_mod_load", name.c_str(), r);
        }
    }
    GN_LOG("mods: %d of %llu loaded", loaded, static_cast<unsigned long long>(files.size()));
}

} // namespace gn::host
