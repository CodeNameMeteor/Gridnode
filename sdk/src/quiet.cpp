// Before a module's code is freed (a mod being unloaded, or gridnode.dll itself): is any thread of
// the game still running in it, or about to return into it? Waiting a few frames and ticks covers the
// threads GridNode knows; a game thread inside a detour whose original runs for seconds (a level
// load), or a thread a mod started itself, is only found by looking (review, 2026-11-03).
//
// Every other thread is paused and its call stack unwound with the modules' unwind data (.pdata, read
// by hand: RtlLookupFunctionEntry takes a lock a paused thread may hold); a frame in the module's
// code means the thread is in it. Where the unwind cannot go on (code with no unwind data outside any
// module: GridNode's thunks, a detour's trampoline), the rest of the stack is scanned for addresses
// in the module instead, which can also find stale values: the caller retries, and keeps the module
// in memory when it never clears. The first version scanned every stack whole, and a value the
// trainer's draw callback had left in a waiting render thread's frame kept it loaded for good
// (2026-11-03). Nothing is allocated or logged while threads are paused (one of them may hold the
// heap's or the log's lock).

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <vector>

#include "gridnode/log.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

struct Range {
    uintptr_t begin, end;
};

std::vector<Range> codeOf(HMODULE m) {
    std::vector<Range> out;
    const auto base = reinterpret_cast<uintptr_t>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            out.push_back({base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize});
    return out;
}

bool inside(const std::vector<Range>& rs, uintptr_t a) {
    for (const Range& r : rs)
        if (a >= r.begin && a < r.end) return true;
    return false;
}

// The loaded modules' unwind tables, read before any thread is paused.
struct ModuleUnwind {
    uintptr_t base, end;
    const RUNTIME_FUNCTION* fns;
    size_t count;
};

std::vector<ModuleUnwind> unwindTables() {
    std::vector<ModuleUnwind> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        const auto base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        out.push_back({base, base + me.modBaseSize, dir.VirtualAddress ? reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress) : nullptr,
                       dir.Size / sizeof(RUNTIME_FUNCTION)});
    }
    CloseHandle(snap);
    return out;
}

// The unwind entry for pc (sorted table, binary search); inModule: pc is in some module.
const RUNTIME_FUNCTION* lookup(const std::vector<ModuleUnwind>& mods, uintptr_t pc, uintptr_t& base, bool& inModule) {
    inModule = false;
    for (const ModuleUnwind& m : mods) {
        if (pc < m.base || pc >= m.end) continue;
        inModule = true;
        base = m.base;
        if (!m.fns) return nullptr;
        const auto rva = static_cast<DWORD>(pc - m.base);
        size_t lo = 0, hi = m.count;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (m.fns[mid].EndAddress <= rva) lo = mid + 1;
            else if (m.fns[mid].BeginAddress > rva) hi = mid;
            else {
                const RUNTIME_FUNCTION* f = &m.fns[mid];
                if (f->UnwindData & 1) f = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + (f->UnwindData & ~DWORD(1))); // indirect
                return f;
            }
        }
        return nullptr;
    }
    return nullptr;
}

using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
struct ThreadBasicInformation {
    LONG exitStatus;
    PVOID tebBase;
    PVOID clientId[2];
    ULONG_PTR affinity;
    LONG priority, basePriority;
};

} // namespace

ThreadsInCode threadsInCode(void* module, bool registers) {
    const HMODULE m = static_cast<HMODULE>(module);
    ThreadsInCode out;
    if (!m) return out;
    const std::vector<Range> code = codeOf(m);
    const std::vector<ModuleUnwind> mods = unwindTables();
    static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread")));
    std::vector<DWORD> ids;
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        const DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) ids.push_back(te.th32ThreadID);
        CloseHandle(snap);
    }
    struct T {
        HANDLE h;
        DWORD id;
        uintptr_t stackBase;
    };
    std::vector<T> threads;
    threads.reserve(ids.size());
    for (DWORD id : ids) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!h) continue;
        uintptr_t base = 0;
        ThreadBasicInformation tbi{};
        if (query && query(h, 0, &tbi, sizeof(tbi), nullptr) >= 0 && tbi.tebBase) // TEB starts with NT_TIB
            base = reinterpret_cast<uintptr_t>(static_cast<const NT_TIB*>(tbi.tebBase)->StackBase);
        threads.push_back({h, id, base});
    }
    // Paused from here: no allocation, no logging.
    std::vector<bool> paused(threads.size(), false); // allocated before the pause
    const ULONGLONG pausedAt = GetTickCount64();
    for (size_t i = 0; i < threads.size(); ++i) paused[i] = SuspendThread(threads[i].h) != DWORD(-1);
    for (size_t i = 0; i < threads.size(); ++i) {
        if (!paused[i]) continue;
        alignas(16) CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (!GetThreadContext(threads[i].h, &ctx)) continue;
        const uintptr_t base = threads[i].stackBase;
        bool found = false, scan = false;
        uintptr_t at = 0;
        // A thread that loaded a hook's address and has not called it yet holds it only in a register
        // (review, 2026-11-03). Asked for in the first polls only: a register can also keep a stale value
        // for good, while that window closes within microseconds.
        if (registers)
            for (const DWORD64 r : {ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.R8, ctx.R9, ctx.R10, ctx.R11, ctx.R12,
                                    ctx.R13, ctx.R14, ctx.R15})
                if (inside(code, static_cast<uintptr_t>(r))) {
                    found = true, at = static_cast<uintptr_t>(r);
                    out.inRegister = true;
                    break;
                }
        // The frames, innermost first.
        for (int f = 0, leaves = 0; f < 512 && !found; ++f) {
            const auto pc = static_cast<uintptr_t>(ctx.Rip), sp = static_cast<uintptr_t>(ctx.Rsp);
            if (!pc) break; // the outermost frame
            if (inside(code, pc)) {
                found = true, at = pc;
                break;
            }
            if (!base || sp < 0x10000 || sp >= base || (sp & 7)) break;
            uintptr_t imageBase = 0;
            bool inModule = false;
            const RUNTIME_FUNCTION* fe = lookup(mods, pc, imageBase, inModule);
            if (!inModule) { // a thunk or trampoline: no unwind data, so look at the rest of the stack
                scan = true;
                break;
            }
            if (!fe) { // a leaf function: its return address is on top
                if (++leaves > 1) {
                    scan = true;
                    break;
                }
                ctx.Rip = *reinterpret_cast<const DWORD64*>(sp);
                ctx.Rsp += 8;
                continue;
            }
            leaves = 0;
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, const_cast<PRUNTIME_FUNCTION>(fe), &ctx, &handlerData, &establisher, nullptr);
            if (static_cast<uintptr_t>(ctx.Rsp) <= sp) { // no progress: a broken frame
                scan = true;
                break;
            }
        }
        const uintptr_t sp = static_cast<uintptr_t>(ctx.Rsp) & ~uintptr_t(7);
        if (!found && scan && base > sp && base - sp <= 64ull * 1024 * 1024) {
            MEMORY_BASIC_INFORMATION mbi{};
            for (uintptr_t a = sp; a < base && !found;) {
                // Committed, readable pages of the stack only.
                if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
                const uintptr_t regionEnd = std::min<uintptr_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize, base);
                if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                    for (uintptr_t q = a; q + 8 <= regionEnd; q += 8)
                        if (const uintptr_t v = *reinterpret_cast<const uintptr_t*>(q); inside(code, v)) {
                            found = true, at = v;
                            break;
                        }
                a = regionEnd;
            }
            out.onStack = out.onStack || found;
        }
        if (found) {
            if (!out.threads) out.firstThread = threads[i].id, out.firstAt = at;
            ++out.threads;
        }
    }
    for (size_t i = 0; i < threads.size(); ++i)
        if (paused[i]) ResumeThread(threads[i].h);
    out.pausedMs = static_cast<unsigned long>(GetTickCount64() - pausedAt);
    for (const T& t : threads) CloseHandle(t.h);
    return out;
}

bool waitCodeQuiet(void* m, const char* what, unsigned long ms) {
    const ULONGLONG start = GetTickCount64();
    ThreadsInCode t;
    unsigned long longest = 0;
    int polls = 0;
    for (;;) {
        t = threadsInCode(m, GetTickCount64() - start < 1000);
        ++polls, longest = std::max(longest, t.pausedMs);
        if (!t.threads) {
            if (longest >= 10) GN_LOG("%s: threads paused up to %lu ms to check it was left (%d checks)", what, longest, polls);
            return true;
        }
        if (GetTickCount64() - start >= ms) break;
        Sleep(100);
    }
    GN_LOG("%s: %d thread(s) still in its code after %lu ms (thread %lu at %p%s); it stays in memory", what, t.threads,
           static_cast<unsigned long>(ms), static_cast<unsigned long>(t.firstThread), reinterpret_cast<void*>(t.firstAt),
           t.onStack ? ", found by scanning a stack past code with no unwind data: maybe stale" : ", a frame of its call stack");
    return false;
}

} // namespace gn::host
