#include "gridnode/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace gn::log {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_dir;
constexpr size_t kMaxLogBytes = 16u << 20; // the debugger output keeps everything after this
size_t g_written = 0;
bool g_full = false;

bool moduleDir(HMODULE module, std::wstring& dir) {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    dir = p.substr(0, slash + 1);
    return true;
}

FILE* tryOpen(const std::wstring& dir) {
    const std::wstring path = dir + L"gridnode.log";
    const std::wstring prev = dir + L"gridnode.prev.log";
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING); // keep the previous run's log
    FILE* f = _wfopen(path.c_str(), L"w");
    if (f) {
        OutputDebugStringW(L"[GridNode] log file: ");
        OutputDebugStringW(path.c_str());
        OutputDebugStringW(L"\n");
    }
    return f;
}
} // namespace

void open(HMODULE self) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    std::wstring dir;
    if (moduleDir(self, dir) && (g_file = tryOpen(dir)) != nullptr) { g_dir = dir; return; }
    if (moduleDir(nullptr, dir) && (g_file = tryOpen(dir)) != nullptr) { g_dir = dir; return; }
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        dir = std::wstring(local) + L"\\GridNode\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        if ((g_file = tryOpen(dir)) != nullptr) g_dir = dir;
    }
}

std::wstring directory() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_dir;
}

void close() {
    if (!g_mutex.try_lock()) return; // process exit: a thread holding the lock may be gone
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_mutex.unlock();
}

// The formatted line out (g_mutex held).
void emitLocked(const char* msg, size_t len) {
    // Only for a debugger: without one, each call raises and handles an exception inside Windows,
    // twice per line (review, 2026-11-11). gridnode.log has every line either way.
    if (IsDebuggerPresent()) {
        OutputDebugStringA("[GridNode] ");
        OutputDebugStringA(msg);
    }
    if (g_file && !g_full) {
        if (g_written + len + 1 > kMaxLogBytes) {
            fputs("[log] gridnode.log reached 16 MB; nothing more is written to it this session\n", g_file);
            g_full = true;
        } else {
            fputs(msg, g_file);
            g_written += len + 1;
        }
        fflush(g_file);
    }
}

// For the crash handler: never waits for the log's lock (the faulting thread may hold it), so a
// line is lost rather than the game deadlocked.
void writeTry(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);
    const size_t len = strlen(msg);
    msg[len] = '\n';
    msg[len + 1] = 0;
    if (!g_mutex.try_lock()) return;
    emitLocked(msg, len);
    g_mutex.unlock();
}

void writeLineTry(const char* line) {
    // A line already formatted (the crash handler formats into static memory): no stack buffer here.
    if (!g_mutex.try_lock()) return;
    const size_t len = strlen(line);
    emitLocked(line, len);
    if (len == 0 || line[len - 1] != '\n') {
        if (IsDebuggerPresent()) OutputDebugStringA("\n");
        if (g_file && !g_full) {
            fputc('\n', g_file);
            fflush(g_file);
        }
    }
    g_mutex.unlock();
}

void write(const char* fmt, ...) {
    char msg[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    const int n = snprintf(msg, sizeof(msg), "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg + n, sizeof(msg) - n - 2, fmt, ap);
    va_end(ap);
    const size_t len = strlen(msg);
    msg[len] = '\n';
    msg[len + 1] = 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    emitLocked(msg, len);
}

} // namespace gn::log
