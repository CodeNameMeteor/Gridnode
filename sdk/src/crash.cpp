// Crash notes: access violations (and similar faults) are written to gridnode.log, each fault address once,
// with the faulting address, the thread (server, client, render), registers and the call stack,
// so a crash can be traced to a function. The exception is passed on unchanged.
//
// The stack is unwound with the modules' unwind tables (.pdata: the game ships its own), so each
// frame is a real caller, not a code address that happens to be on the stack. Game frames are named
// from our annotations (gridnode/engine.hpp meta::functionsByAddress) or, unnamed, as Ghidra names
// them (FUN_<address>). When the unwind stops early (code without unwind data), the old scan of the
// stack for code addresses follows it.

#include <windows.h>
#include <cwctype>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <memory>
#include <vector>

#include "gridnode/engine.hpp"
#include "gridnode/log.hpp"
#include "host.hpp"

namespace gn::host::crash {
namespace {

constexpr uintptr_t kGameBase = 0x140000000ull; // the game's preferred base (Ghidra's addresses)

struct Module {
    HMODULE handle = nullptr;
    char name[64] = {};
    bool game = false, mod = false; // the game's exe; a DLL in the mods folder
};

// The loaded modules, made outside the handler (refreshModules): GetModuleHandleEx and
// GetModuleFileName take the loader lock, which another thread may hold while this one crashed
// (review, 2026-11-03). A table replaced is kept: the handler may be reading it.
struct ModuleRow {
    uintptr_t base, end;
    char name[64];
    bool game, mod;
};
std::atomic<const std::vector<ModuleRow>*> g_modules{nullptr};
using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
GetDesc g_getThreadDescription = nullptr;

Module moduleOf(uintptr_t a) {
    Module m;
    if (const std::vector<ModuleRow>* rows = g_modules.load(std::memory_order_acquire))
        for (const ModuleRow& r : *rows)
            if (a >= r.base && a < r.end) {
                m.handle = reinterpret_cast<HMODULE>(r.base);
                memcpy(m.name, r.name, sizeof(m.name));
                m.game = r.game, m.mod = r.mod;
                return m;
            }
    // Loaded since the last refresh: its base, without a name (VirtualQuery takes no loader lock).
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE || !mbi.AllocationBase) return Module{};
    m.handle = static_cast<HMODULE>(mbi.AllocationBase);
    snprintf(m.name, sizeof(m.name), "module@%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
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
    if (auto get = g_getThreadDescription) { // looked up at install: no loader lock in the handler
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

// One fault location is logged once: a vectored handler sees first-chance exceptions, and the game
// (its SEH) and GridNode's own probing reads fault and recover all the time. A plain cap on the
// count used to be spent on those, so the fatal crash was never written (review, 2026-11-04).
// Each distinct (code, address) is logged the first time; repeats are only counted.
constexpr int kSeen = 64;
std::atomic<uint64_t> g_seen[kSeen];
std::atomic<int> g_seenCount{0};
std::atomic<uint32_t> g_repeats{0};

// True when (code, rip) is new; false when it was logged already or the table is full.
bool firstTime(DWORD code, uintptr_t rip) {
    const uint64_t key = (static_cast<uint64_t>(rip) & 0x00ffffffffffffffull) | (static_cast<uint64_t>(code & 0xff) << 56);
    const int n = std::min(g_seenCount.load(std::memory_order_acquire), kSeen);
    for (int i = 0; i < n; ++i)
        if (g_seen[i].load(std::memory_order_relaxed) == key) return false;
    const int slot = g_seenCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kSeen) return false;
    g_seen[slot].store(key, std::memory_order_release);
    return true;
}

// The handler's buffers are static, not on the faulting stack: a CONTEXT alone is 1.2 KB, and on
// EXCEPTION_STACK_OVERFLOW there is little stack left. One thread at a time uses them; a fault
// inside the handler on the same thread passes straight on.
struct alignas(16) Scratch {
    CONTEXT ctx;
    char where[200], thread[160], mods[200], d[200], line[512];
};
Scratch g_scratch;
std::atomic<DWORD> g_owner{0};

bool takeScratch() {
    const DWORD me = GetCurrentThreadId();
    if (g_owner.load(std::memory_order_relaxed) == me) return false; // faulted inside the handler
    for (int i = 0; i < 200; ++i) { // another thread is logging its crash: wait up to ~200 ms
        DWORD none = 0;
        if (g_owner.compare_exchange_strong(none, me, std::memory_order_acquire)) return true;
        Sleep(1);
    }
    return false;
}

template <typename... A> void line(const char* fmt, A... a) {
    snprintf(g_scratch.line, sizeof(g_scratch.line), fmt, a...);
    ::gn::log::writeLineTry(g_scratch.line);
}

void report(EXCEPTION_POINTERS* ep) {
    Scratch& s = g_scratch;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const CONTEXT* c = ep->ContextRecord;
    DWORD64 base0 = 0;
    const PRUNTIME_FUNCTION fe0 = RtlLookupFunctionEntry(c->Rip, &base0, nullptr);
    describe(static_cast<uintptr_t>(c->Rip), fe0 ? static_cast<uintptr_t>(base0 + fe0->BeginAddress) : 0, s.where, sizeof(s.where));
    threadText(s.thread, sizeof(s.thread));
    const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
    const uint32_t repeats = g_repeats.exchange(0);
    line("crash: exception %08lx at %s%s%llx (%s)", code, s.where,
         code == EXCEPTION_ACCESS_VIOLATION ? (info[0] == 1 ? ", writing " : info[0] == 8 ? ", executing " : ", reading ") : ", ",
         static_cast<unsigned long long>(code == EXCEPTION_ACCESS_VIOLATION ? info[1] : 0), s.thread);
    if (repeats) line("crash:   (%u repeat(s) of faults already logged since the last note)", repeats);
    line("crash:   rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->R8, c->R9);
    line("crash:   r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx rbp %llx rsp %llx", c->R10, c->R11, c->R12, c->R13, c->R14, c->R15, c->Rbp,
         c->Rsp);
    // A stack overflow leaves too little stack for the unwinder (RtlVirtualUnwind and the module
    // lookups have their own frames): the address and registers are what can be written safely.
    if (code == EXCEPTION_STACK_OVERFLOW) return;

    // The call stack, unwound. Only within this thread's stack (a bad unwind must not fault here).
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    CONTEXT& ctx = s.ctx;
    ctx = *c;
    int frames = 0, leaves = 0;
    s.mods[0] = 0;
    for (; frames < 32; ++frames) {
        const uintptr_t rip = static_cast<uintptr_t>(ctx.Rip);
        if (!isCode(rip)) break;
        DWORD64 base = 0;
        const PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (frames > 0) { // frame 0 is the line above
            describe(rip, fe ? static_cast<uintptr_t>(base + fe->BeginAddress) : 0, s.d, sizeof(s.d));
            line("crash:   #%02d %s", frames, s.d);
            const Module m = moduleOf(rip);
            if (m.mod && !std::strstr(s.mods, m.name) && std::strlen(s.mods) + std::strlen(m.name) + 3 < sizeof(s.mods))
                snprintf(s.mods + std::strlen(s.mods), sizeof(s.mods) - std::strlen(s.mods), "%s%s", *s.mods ? ", " : "", m.name);
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
    if (*s.mods) line("crash:   mods on this stack: %s", s.mods);
    if (frames < 4) {
        // The unwind stopped early: code addresses found on the stack, as before (callers, or leftovers).
        line("crash:   the unwind stopped after %d frame(s); code addresses on the stack (not all are callers):", frames);
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
        int shown = 0;
        for (int i = 0; i < 256 && shown < 16; ++i) {
            if (!onStack(reinterpret_cast<uintptr_t>(sp + i), lo, hi)) break;
            const uintptr_t v = sp[i];
            if (!isCode(v) || !moduleOf(v).handle) continue;
            DWORD64 b = 0;
            const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(v, &b, nullptr);
            describe(v, f ? static_cast<uintptr_t>(b + f->BeginAddress) : 0, s.d, sizeof(s.d));
            line("crash:   stack +%03x %s", i * 8, s.d);
            ++shown;
        }
    }
}

LONG CALLBACK onException(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!takeScratch()) return EXCEPTION_CONTINUE_SEARCH;
    if (firstTime(code, static_cast<uintptr_t>(ep->ContextRecord->Rip))) report(ep);
    else g_repeats.fetch_add(1, std::memory_order_relaxed);
    g_owner.store(0, std::memory_order_release);
    return EXCEPTION_CONTINUE_SEARCH;
}

void* g_handler = nullptr;

std::wstring g_modsFolder; // lower case, ends in a backslash (gridnode.ini's [mods] folder)

std::wstring lower(std::wstring w) {
    for (wchar_t& ch : w) ch = static_cast<wchar_t>(towlower(ch));
    for (wchar_t& ch : w)
        if (ch == L'/') ch = L'\\';
    return w;
}

} // namespace

void setModsFolder(const std::wstring& folder) {
    wchar_t full[MAX_PATH]; // "..\\" and the like resolved, as module paths are
    const DWORD n = GetFullPathNameW(folder.c_str(), MAX_PATH, full, nullptr);
    g_modsFolder = lower(n > 0 && n < MAX_PATH ? std::wstring(full, n) : folder);
    if (!g_modsFolder.empty() && g_modsFolder.back() != L'\\') g_modsFolder += L'\\';
    refreshModules();
}

void refreshModules() {
    auto rows = std::make_unique<std::vector<ModuleRow>>();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    const uintptr_t game = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        ModuleRow r{};
        r.base = reinterpret_cast<uintptr_t>(me.modBaseAddr), r.end = r.base + me.modBaseSize;
        WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, r.name, sizeof(r.name) - 1, nullptr, nullptr);
        r.game = r.base == game;
        // In the mods folder (its .loaded\ copies too), wherever gridnode.ini puts it; paths compare
        // without case (Windows paths do).
        const std::wstring path = lower(me.szExePath);
        r.mod = g_modsFolder.empty() ? path.find(L"\\mods\\") != std::wstring::npos : path.compare(0, g_modsFolder.size(), g_modsFolder) == 0;
        rows->push_back(r);
    }
    CloseHandle(snap);
    static std::vector<std::unique_ptr<std::vector<ModuleRow>>> kept; // GridNode's thread only
    g_modules.store(rows.get(), std::memory_order_release);
    kept.push_back(std::move(rows));
}

void install() {
    refreshModules();
    g_getThreadDescription = reinterpret_cast<GetDesc>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription")));
    g_handler = AddVectoredExceptionHandler(0, &onException);
}
void uninstall() {
    if (g_handler) RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
}

} // namespace gn::host::crash
