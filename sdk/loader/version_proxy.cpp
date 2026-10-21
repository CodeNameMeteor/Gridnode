// GridNode loader: a proxy version.dll.
//
// The game (or one of its dependencies) loads version.dll by name, and Windows
// looks in the game folder first. This DLL forwards every version.dll export to
// the real one in System32 and loads gridnode.dll (the GridNode runtime) from the same folder.
// It does not touch the game's code or its protection; it only loads our DLL.
//
// All version.dll exports take only integer/pointer arguments, so a generic
// 8-argument forwarder is ABI-safe on x64 (extra slots are read but unused).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace {

HMODULE g_real = nullptr;
HMODULE g_self = nullptr;

// Folder this DLL lives in (the game folder), with a trailing backslash.
bool selfDir(wchar_t* path) {
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    for (DWORD i = n; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') { path[i] = 0; return true; }
    }
    return false;
}

HMODULE loadRealVersion() {
    // Chaining: if another mod (e.g. a custom-server loader) also ships a version.dll,
    // rename theirs to version_next.dll and we load it and forward to it instead of
    // System32, so both mods run.
    wchar_t next[MAX_PATH];
    if (selfDir(next) && lstrlenW(next) + 17 < MAX_PATH) {
        lstrcatW(next, L"version_next.dll");
        if (GetFileAttributesW(next) != INVALID_FILE_ATTRIBUTES)
            if (HMODULE m = LoadLibraryW(next)) return m;
    }
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return nullptr;
    lstrcatW(path, L"\\version.dll");
    return LoadLibraryW(path);
}

// Loaded once whichever thread asks first (the loader thread, or a forwarder): published with a
// compare-exchange, so a thread that loses the race drops its extra reference.
HMODULE realVersion() {
    if (HMODULE m = static_cast<HMODULE>(InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_real), nullptr, nullptr)))
        return m;
    HMODULE mine = loadRealVersion();
    if (!mine) return nullptr;
    void* prev = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_real), mine, nullptr);
    if (prev) {
        FreeLibrary(mine); // the same module: LoadLibrary counted it twice
        return static_cast<HMODULE>(prev);
    }
    return mine;
}

using Fn8 = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

Fn8 resolve(const char* name) {
    HMODULE m = realVersion();
    return m ? reinterpret_cast<Fn8>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

DWORD WINAPI loadCore(LPVOID) {
    // Load a chained version_next.dll early too, so its own DllMain runs at startup
    // exactly as it would without us.
    realVersion();
    wchar_t path[MAX_PATH];
    if (!selfDir(path) || lstrlenW(path) + 13 >= MAX_PATH) return 1;
    lstrcatW(path, L"gridnode.dll");
    if (!LoadLibraryW(path)) {
        OutputDebugStringW(L"[GridNode] version.dll proxy: could not load gridnode.dll\n");
        return 1;
    }
    return 0;
}

} // namespace

#define GN_FORWARD(name)                                                                       \
    extern "C" uintptr_t Proxy_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,        \
                                       uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) {      \
        static Fn8 fn = resolve(#name);                                                           \
        return fn ? fn(a, b, c, d, e, f, g, h) : 0;                                               \
    }

GN_FORWARD(GetFileVersionInfoA)
GN_FORWARD(GetFileVersionInfoByHandle)
GN_FORWARD(GetFileVersionInfoExA)
GN_FORWARD(GetFileVersionInfoExW)
GN_FORWARD(GetFileVersionInfoSizeA)
GN_FORWARD(GetFileVersionInfoSizeExA)
GN_FORWARD(GetFileVersionInfoSizeExW)
GN_FORWARD(GetFileVersionInfoSizeW)
GN_FORWARD(GetFileVersionInfoW)
GN_FORWARD(VerFindFileA)
GN_FORWARD(VerFindFileW)
GN_FORWARD(VerInstallFileA)
GN_FORWARD(VerInstallFileW)
GN_FORWARD(VerLanguageNameA)
GN_FORWARD(VerLanguageNameW)
GN_FORWARD(VerQueryValueA)
GN_FORWARD(VerQueryValueW)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        // Loading another DLL inside DllMain risks a loader-lock deadlock, so do it on a
        // thread; it runs as soon as the loader lock is released.
        HANDLE t = CreateThread(nullptr, 0, loadCore, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
