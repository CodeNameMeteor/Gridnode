// Crash notes: the first few access violations (and similar faults) are written to gridnode.log
// with the faulting address, registers and the code addresses found on the stack, so a crash in
// research code can be traced to a function. The exception is passed on unchanged.

#include <windows.h>

#include <atomic>
#include <cstdint>

#include "gridnode/log.hpp"

namespace gn::host::crash {
namespace {

std::atomic<int> g_logged{0};

// "MirrorsEdgeCatalyst.exe+0x123" style, with the game's own (preferred-base) address for Ghidra.
void describe(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a), &m) || !m) {
        snprintf(out, n, "%llx", static_cast<unsigned long long>(a));
        return;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') name = p + 1;
    const uintptr_t off = a - reinterpret_cast<uintptr_t>(m);
    if (m == GetModuleHandleA(nullptr)) snprintf(out, n, "%s+%llx (va %llx)", name, static_cast<unsigned long long>(off), static_cast<unsigned long long>(0x140000000ull + off));
    else snprintf(out, n, "%s+%llx", name, static_cast<unsigned long long>(off));
}

bool isCode(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (a < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 && mbi.Type == MEM_IMAGE;
}

LONG CALLBACK onException(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_logged.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = ep->ContextRecord;
    char where[160];
    describe(static_cast<uintptr_t>(c->Rip), where, sizeof(where));
    const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
    GN_LOG("crash: exception %08lx at %s%s%llx (thread %lu)", code, where,
           code == EXCEPTION_ACCESS_VIOLATION ? (info[0] == 1 ? ", writing " : info[0] == 8 ? ", executing " : ", reading ") : ", ",
           static_cast<unsigned long long>(code == EXCEPTION_ACCESS_VIOLATION ? info[1] : 0), GetCurrentThreadId());
    GN_LOG("crash:   rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8,
           c->R9);
    GN_LOG("crash:   r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx rbp %llx rsp %llx", c->R10, c->R11, c->R12, c->R13, c->R14, c->R15, c->Rbp,
           c->Rsp);
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
    int shown = 0;
    for (int i = 0; i < 256 && shown < 16; ++i) {
        uintptr_t v = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, sizeof(v), nullptr)) break;
        if (!isCode(v)) continue;
        char d[160];
        describe(v, d, sizeof(d));
        GN_LOG("crash:   stack +%03x %s", i * 8, d);
        ++shown;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void install() { AddVectoredExceptionHandler(0, &onException); }

} // namespace gn::host::crash
