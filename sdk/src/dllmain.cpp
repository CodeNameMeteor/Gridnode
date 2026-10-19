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
    gn::runtime::run(g_self);
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        // Pin: hooks will point into this DLL, so it must never be unloaded while the game runs.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        if (HANDLE t = CreateThread(nullptr, 0, mainThread, nullptr, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        gn::runtime::shutdown(); // process exit (the DLL is pinned, so there is no other detach)
    }
    return TRUE;
}
