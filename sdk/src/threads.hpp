#pragma once
// The game's other threads, opened for a pause (threads.cpp).

#include <windows.h>

#include <vector>

namespace gn::host {

struct ThreadHandle {
    HANDLE h = nullptr;
    DWORD id = 0;
};

// Before the first pause (looks the call up in ntdll).
void threadsPrepare();

// Opens every thread of this process but the calling one that is not already in `out`, with
// `access`, up to out.capacity() (reserve first: nothing is allocated, so it can run with threads
// paused). How many were added; the caller closes the handles.
size_t openOtherThreads(std::vector<ThreadHandle>& out, DWORD access);

} // namespace gn::host
