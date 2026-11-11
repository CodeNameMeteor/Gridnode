// The game's threads, listed for a pause (patches.cpp, quiet.cpp). NtGetNextThread walks this
// process's threads only and hands back open handles; CreateToolhelp32Snapshot copies every thread
// of the system, twice per pause, and a function detour took 99 to 132 ms each (gridnode.prev.log,
// 2026-11-10). The snapshot stays as the fallback when ntdll does not offer the call.
//
// Nothing here allocates once `out` is reserved, so a later round can run with the threads paused.

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>

#include "threads.hpp"

namespace gn::host {
namespace {
using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes, ULONG flags, HANDLE* next);

NtGetNextThreadFn nextThreadFn() {
    static const auto fn = reinterpret_cast<NtGetNextThreadFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtGetNextThread")));
    return fn;
}

bool known(const std::vector<ThreadHandle>& v, DWORD id) {
    return std::any_of(v.begin(), v.end(), [id](const ThreadHandle& t) { return t.id == id; });
}
} // namespace

void threadsPrepare() { (void)nextThreadFn(); } // GetProcAddress may take the loader lock: before any pause

size_t openOtherThreads(std::vector<ThreadHandle>& out, DWORD access) {
    const DWORD me = GetCurrentThreadId();
    size_t added = 0;
    if (const NtGetNextThreadFn next = nextThreadFn()) {
        access |= THREAD_QUERY_LIMITED_INFORMATION; // GetThreadId
        HANDLE prev = nullptr;
        bool prevKept = false; // the caller's now: walked on from, not closed
        for (;;) {
            HANDLE n = nullptr;
            const LONG status = next(GetCurrentProcess(), prev, access, 0, 0, &n);
            if (prev && !prevKept) CloseHandle(prev);
            if (status < 0 || !n) break; // STATUS_NO_MORE_ENTRIES
            const DWORD id = GetThreadId(n);
            prevKept = id && id != me && !known(out, id) && out.size() < out.capacity();
            if (prevKept) out.push_back({n, id}), ++added;
            prev = n;
        }
        return added;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || known(out, te.th32ThreadID) || out.size() >= out.capacity()) continue;
        if (HANDLE h = OpenThread(access, FALSE, te.th32ThreadID)) {
            out.push_back({h, te.th32ThreadID});
            ++added;
        }
    }
    CloseHandle(snap);
    return added;
}

} // namespace gn::host
