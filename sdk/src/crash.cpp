// Crash notes: the first few access violations (and similar faults) are written to gridnode.log
// with the faulting address, the thread (server, client, render), registers and the call stack,
// so a crash can be traced to a function. The exception is passed on unchanged.
//
// The stack is unwound with the modules' unwind tables (.pdata: the game ships its own), so each
// frame is a real caller, not a code address that happens to be on the stack. Game frames are named
// from our annotations (gridnode/engine.hpp meta::functionsByAddress) or, unnamed, as Ghidra names
// them (FUN_<address>). When the unwind stops early (code without unwind data), the old scan of the
// stack for code addresses follows it.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "host.hpp"

namespace gn::host::crash {
namespace {

std::atomic<int> g_logged{0};
constexpr uintptr_t kGameBase = 0x140000000ull; // the game's preferred base (Ghidra's addresses)

struct Module {
    HMODULE handle = nullptr;
    char name[64] = {};
    bool game = false, mod = false; // the game's exe; a DLL in the mods folder
};

Module moduleOf(uintptr_t a) {
    Module m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a), &m.handle) ||
        !m.handle)
        return Module{};
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m.handle, path, MAX_PATH);
    const char* name = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') name = p + 1;
    snprintf(m.name, sizeof(m.name), "%s", name);
    m.game = m.handle == GetModuleHandleA(nullptr);
    m.mod = std::strstr(path, "\\mods\\") != nullptr; // mods\ or mods\.loaded\ (reloadable copies)
    return m;
}

// The name of the game function starting at va (annotations), or null.
const char* gameName(uint64_t va) {
    const auto& t = engine::meta::functionsByAddress;
    auto it = std::lower_bound(std::begin(t), std::end(t), va, [](const engine::meta::NamedFunction& f, uint64_t v) { return f.va < v; });
    return it != std::end(t) && it->va == va ? it->name : nullptr;
}

// "MirrorsEdgeCatalyst.exe+3599023 (va 143599023, FUN_143598e00+0x223)" style; fnStart: the
// function's first byte from its unwind data (0: not known).
void describe(uintptr_t a, uintptr_t fnStart, char* out, size_t n) {
    const Module m = moduleOf(a);
    if (!m.handle) {
        snprintf(out, n, "%llx (not in a module: a thunk or trampoline?)", static_cast<unsigned long long>(a));
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(m.handle), off = a - base;
    if (!m.game) {
        snprintf(out, n, "%s+%llx%s", m.name, static_cast<unsigned long long>(off), m.mod ? " (a mod)" : "");
        return;
    }
    const uint64_t va = kGameBase + off;
    if (!fnStart) {
        snprintf(out, n, "%s+%llx (va %llx)", m.name, static_cast<unsigned long long>(off), static_cast<unsigned long long>(va));
        return;
    }
    const uint64_t fva = kGameBase + (fnStart - base);
    const char* name = gameName(fva);
    char fn[96];
    if (name) snprintf(fn, sizeof(fn), "%s", name);
    else snprintf(fn, sizeof(fn), "FUN_%llx", static_cast<unsigned long long>(fva));
    snprintf(out, n, "%s+%llx (va %llx, %s+0x%llx)", m.name, static_cast<unsigned long long>(off), static_cast<unsigned long long>(va), fn,
             static_cast<unsigned long long>(va - fva));
}

bool isCode(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (a < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// What the thread is: GridNode knows the server and client tick threads and the render thread.
void threadText(char* out, size_t n) {
    const char* role = isServerThread() ? "server" : isClientThread() ? "client" : ui::isRenderThread() ? "render" : nullptr;
    char desc[96] = {};
    using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    if (auto get = reinterpret_cast<GetDesc>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription")))) {
        PWSTR w = nullptr;
        if (SUCCEEDED(get(GetCurrentThread(), &w)) && w) {
            if (*w) WideCharToMultiByte(CP_UTF8, 0, w, -1, desc, sizeof(desc) - 1, nullptr, nullptr);
            LocalFree(w);
        }
    }
    snprintf(out, n, "thread %lu%s%s%s%s%s", GetCurrentThreadId(), role ? ", the " : "", role ? role : "", role ? " thread" : "", *desc ? ", named " : "",
             desc);
}

bool onStack(uintptr_t sp, ULONG_PTR lo, ULONG_PTR hi) { return sp >= lo && sp + 8 <= hi && (sp & 7) == 0; }

LONG CALLBACK onException(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_logged.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = ep->ContextRecord;
    DWORD64 base0 = 0;
    const PRUNTIME_FUNCTION fe0 = RtlLookupFunctionEntry(c->Rip, &base0, nullptr);
    char where[200];
    describe(static_cast<uintptr_t>(c->Rip), fe0 ? static_cast<uintptr_t>(base0 + fe0->BeginAddress) : 0, where, sizeof(where));
    char thread[160];
    threadText(thread, sizeof(thread));
    const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
    ::gn::log::writeTry("crash: exception %08lx at %s%s%llx (%s)", code, where,
                        code == EXCEPTION_ACCESS_VIOLATION ? (info[0] == 1 ? ", writing " : info[0] == 8 ? ", executing " : ", reading ") : ", ",
                        static_cast<unsigned long long>(code == EXCEPTION_ACCESS_VIOLATION ? info[1] : 0), thread);
    ::gn::log::writeTry("crash:   rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8,
                        c->R9);
    ::gn::log::writeTry("crash:   r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx rbp %llx rsp %llx", c->R10, c->R11, c->R12, c->R13, c->R14, c->R15, c->Rbp,
                        c->Rsp);

    // The call stack, unwound. Only within this thread's stack (a bad unwind must not fault here).
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    CONTEXT ctx = *c;
    int frames = 0, leaves = 0;
    char mods[200] = {};
    for (; frames < 32; ++frames) {
        const uintptr_t rip = static_cast<uintptr_t>(ctx.Rip);
        if (!isCode(rip)) break;
        DWORD64 base = 0;
        const PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (frames > 0) { // frame 0 is the line above
            char d[200];
            describe(rip, fe ? static_cast<uintptr_t>(base + fe->BeginAddress) : 0, d, sizeof(d));
            ::gn::log::writeTry("crash:   #%02d %s", frames, d);
            const Module m = moduleOf(rip);
            if (m.mod && !std::strstr(mods, m.name) && std::strlen(mods) + std::strlen(m.name) + 3 < sizeof(mods))
                snprintf(mods + std::strlen(mods), sizeof(mods) - std::strlen(mods), "%s%s", *mods ? ", " : "", m.name);
        }
        if (!onStack(static_cast<uintptr_t>(ctx.Rsp), lo, hi)) break;
        if (!fe) {
            // No unwind data: a leaf function (or code that is not described); its return address is on top.
            if (++leaves > 2) break;
            ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
        if (!onStack(static_cast<uintptr_t>(ctx.Rsp), lo, hi)) break;
    }
    if (*mods) ::gn::log::writeTry("crash:   mods on this stack: %s", mods);
    if (frames < 4) {
        // The unwind stopped early: code addresses found on the stack, as before (callers, or leftovers).
        ::gn::log::writeTry("crash:   the unwind stopped after %d frame(s); code addresses on the stack (not all are callers):", frames);
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
        int shown = 0;
        for (int i = 0; i < 256 && shown < 16; ++i) {
            if (!onStack(reinterpret_cast<uintptr_t>(sp + i), lo, hi)) break;
            const uintptr_t v = sp[i];
            if (!isCode(v) || !moduleOf(v).handle) continue;
            DWORD64 b = 0;
            const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(v, &b, nullptr);
            char d[200];
            describe(v, f ? static_cast<uintptr_t>(b + f->BeginAddress) : 0, d, sizeof(d));
            ::gn::log::writeTry("crash:   stack +%03x %s", i * 8, d);
            ++shown;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void* g_handler = nullptr;

} // namespace

void install() { g_handler = AddVectoredExceptionHandler(0, &onException); }
void uninstall() {
    if (g_handler) RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
}

} // namespace gn::host::crash
