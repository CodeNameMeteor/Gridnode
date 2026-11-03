// gridnode.dll entry point. Loaded into the running game by the version.dll proxy or by
// gridnode_inject.exe. All work happens on GridNode's own thread.

#include <cwchar>

#include "runtime.hpp"

namespace {
HMODULE g_self = nullptr;

DWORD WINAPI mainThread(LPVOID) {
    // One GridNode per process: a second copy (proxy-loaded and injected) stays idle.
    wchar_t name[64];
    swprintf(name, 64, L"Local\\GridNode_core_%lu", GetCurrentProcessId());
    HANDLE instance = CreateMutexW(nullptr, TRUE, name);
    if (instance && GetLastError() == ERROR_ALREADY_EXISTS) {
        OutputDebugStringW(L"[GridNode] already active in this process; this copy stays idle\n");
        return 0;
    }
    const bool canFree = gn::runtime::run(g_self);
    // Stopped either way (passive, or unloaded but kept in memory): another GridNode may start. The
    // injector loads it as a copy under another name, as this one is still mapped as gridnode.dll.
    if (!canFree && instance) ReleaseMutex(instance), CloseHandle(instance), instance = nullptr;
    if (canFree) {
        // A full unload put everything back: let a later GridNode (injected again) start, and free
        // this one. The loader's LoadLibrary is the only reference, so this unmaps it.
        if (instance) ReleaseMutex(instance), CloseHandle(instance);
        FreeLibraryAndExitThread(g_self, 0);
    }
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        // Not pinned: hooks point into this DLL, so only a full unload (unload.cpp), which takes them all
        // out first, frees it (mainThread). Nothing else calls FreeLibrary on it.
        if (HANDLE t = CreateThread(nullptr, 0, mainThread, nullptr, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        gn::runtime::shutdown(); // process exit (a full unload's detach has reserved null and needs nothing)
    }
    return TRUE;
}
