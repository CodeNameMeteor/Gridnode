#pragma once
// Thread-safe log: gridnode.log next to gridnode.dll (else next to the game exe, else
// %LOCALAPPDATA%\GridNode), and the debugger output (Sysinternals DebugView).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace gn::log {

void open(HMODULE self);
// Folder the log is in, with a trailing backslash (empty if none could be opened).
std::wstring directory();
void close();

void write(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

void writeTry(const char* fmt, ...); // as write, but skips the line rather than wait for the lock (crash handler)
void writeLineTry(const char* line); // writeTry for a line formatted by the caller (no stack buffer)
} // namespace gn::log

#define GN_LOG(...) ::gn::log::write(__VA_ARGS__)
