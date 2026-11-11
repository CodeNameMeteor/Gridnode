// Code patches (gn_api patch_bytes, patch_call): the game's code changed in place, by a mod or by
// GridNode, every one listed and undone when its mod is unloaded or GridNode is (unload.cpp). Pointer
// swaps (hooks) stay the first choice; a patch is for what no pointer reaches (a direct call).
//
// patch_call rewrites the rel32 of a call or jmp at a site to go through a thunk near the image (a
// rel32 reaches +-2 GB; mods load far above): "jmp [rip+0]" then the detour's address. The original
// target goes back in the rel32 when the patch is undone; the thunk page is kept (a thread may still
// be on its way through it), so it is never reused.

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/near_alloc.hpp"
#include "gridnode/x64.hpp"
#include "host.hpp"
#include "threads.hpp"

namespace gn::host {
namespace {

struct Patch {
    std::string mod;
    uintptr_t at = 0;
    std::vector<uint8_t> original, patched;
    uintptr_t owner = 0; // an address in the code of whoever made it (a mod's image, or GridNode's)
    std::string what;
    bool whole = false;  // starts on an instruction (patch_bytes): written paused when it spans more than one
};
std::mutex g_m;
std::vector<Patch> g_patches; // (g_m)

using QueryThreadFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
QueryThreadFn g_queryThread = nullptr; // NtQueryInformationThread, looked up before any pause

// Every other thread of the game paused (handles in `out`, to resume). The list is made before the
// first pause: nothing may allocate or log while threads are paused (one may hold the heap's lock).
// GridNode's own threads are paused too; that is safe whatever locks the caller holds, because the
// paused section (thread contexts, VirtualProtect, a memcpy, FlushInstructionCache) takes no lock
// and so never waits on a paused thread (review, 2026-11-03).
//
// A thread started between the listing and the pauses would run on: once every thread listed is
// paused no new one can be started (only a running thread starts one), so a second listing then
// finds any that slipped through, and they are paused too (review, 2026-11-04). The listing
// (threads.cpp: NtGetNextThread, this process only) takes no heap lock, and the vectors were
// reserved first. It replaced two system-wide toolhelp snapshots per pause (2026-11-11).
void pauseOthers(std::vector<HANDLE>& out) {
    constexpr DWORD kAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT;
    if (!g_queryThread) // before any pause (GetProcAddress may take the loader lock)
        g_queryThread = reinterpret_cast<QueryThreadFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    threadsPrepare();
    std::vector<ThreadHandle> ts;
    ts.reserve(4096);
    openOtherThreads(ts, kAccess);
    out.clear();
    out.reserve(ts.capacity());
    size_t done = 0;
    const auto suspendNew = [&] { // reserved: no allocation
        bool any = false;
        for (; done < ts.size(); ++done) {
            if (SuspendThread(ts[done].h) != DWORD(-1)) out.push_back(ts[done].h), any = true;
            else CloseHandle(ts[done].h);
        }
        return any;
    };
    suspendNew();
    // With the others paused: anything new since the first listing (a few rounds: one that slipped
    // through may have started another before it was paused).
    for (int round = 0; round < 4 && openOtherThreads(ts, kAccess) && suspendNew(); ++round) {
    }
}

void resumeAll(std::vector<HANDLE>& hs) {
    for (HANDLE h : hs) {
        ResumeThread(h);
        CloseHandle(h);
    }
    hs.clear();
}

// The top of a paused thread's stack (its TEB's StackBase), or 0.
uintptr_t stackBase(HANDLE h) {
    struct {
        LONG exitStatus;
        void* teb;
        void* clientId[2];
        ULONG_PTR affinity;
        LONG priority, basePriority;
    } info{};
    if (!g_queryThread || g_queryThread(h, 0 /* ThreadBasicInformation */, &info, sizeof(info), nullptr) < 0 || !info.teb) return 0;
    uintptr_t base = 0; // NT_TIB.StackBase at +8 (the thread's own TEB, readable from here)
    return mem::read(reinterpret_cast<uintptr_t>(info.teb) + 8, base) ? base : 0;
}

// v looks like a return address: a call instruction (E8 rel32, FF /2 with a ModRM) ends there.
bool afterCall(uintptr_t v) {
    uint8_t b[7] = {};
    if (!mem::read(v - 7, b, sizeof(b))) return false;
    if (b[2] == 0xe8) return true;                                 // call rel32
    if (b[5] == 0xff && ((b[6] >> 3) & 7) == 2) return true;     // call r/m (2 bytes: call rax...)
    if (b[4] == 0xff && ((b[5] >> 3) & 7) == 2) return true;     // call [r+d8]
    if (b[1] == 0xff && ((b[2] >> 3) & 7) == 2) return true;     // call [rip+d32] / [r+d32]
    return false;
}

// A paused thread whose next instruction is in [at, at + n), or (returns) that will return into it:
// a call whose return address lands inside the bytes (patch_bytes over a call). The stack is scanned
// for such return addresses rather than unwound (the unwinder can take a lock a paused thread holds);
// a stale one only makes the write wait and try again.
bool anyThreadIn(const std::vector<HANDLE>& hs, uintptr_t at, size_t n, bool returns = false) {
    for (HANDLE h : hs) {
        alignas(16) CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(h, &ctx)) continue;
        if (ctx.Rip >= at && ctx.Rip < at + n) return true;
        if (!returns) continue;
        const uintptr_t top = stackBase(h);
        if (!top) continue;
        constexpr size_t kScan = 64 * 1024; // the frames that matter are near the top
        for (uintptr_t sp = ctx.Rsp & ~uintptr_t(7); sp + 8 <= top && sp < ctx.Rsp + kScan; sp += 8) {
            uintptr_t v = 0;
            if (!mem::read(sp, v)) break;
            if (v > at && v < at + n && afterCall(v)) return true;
        }
    }
    return false;
}

// Whether the code at `at` is more than one instruction over n bytes (or cannot be decoded): such
// bytes are written with the threads paused, even when one store would do.
bool spansInstructions(uintptr_t at, size_t n) {
    uint8_t code[16] = {};
    x64::Insn in;
    if (!mem::read(at, code, sizeof(code)) || !x64::decode(code, sizeof(code), in)) return true;
    return in.length < n;
}

// Writes code a thread may be running: in one store when the bytes fit in an aligned 8 or 16 bytes;
// otherwise with every other thread paused and none about to run them (patch_bytes over 16 bytes, or
// across a 16-byte boundary), so no thread ever runs half old, half new code (review, 2026-11-03).
// othersPaused: the caller paused them already (a detour's entry). paused: always the paused path
// (bytes over more than one instruction: one store is atomic, but a thread stopped between two of
// the old instructions would go on in the middle of the new ones, review 2026-11-11).
bool writeCode(uintptr_t at, const uint8_t* bytes, size_t n, bool othersPaused = false, bool paused = false) {
    DWORD prot = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &prot)) return false;
    const uintptr_t q = at & ~uintptr_t(7);
    if (!paused && at + n <= q + 8) {
        volatile LONG64* p = reinterpret_cast<volatile LONG64*>(q);
        LONG64 old = *p;
        for (;;) {
            LONG64 now = old;
            memcpy(reinterpret_cast<uint8_t*>(&now) + (at - q), bytes, n);
            const LONG64 seen = InterlockedCompareExchange64(p, now, old);
            if (seen == old) break;
            old = seen;
        }
    } else if (const uintptr_t o = at & ~uintptr_t(15); !paused && at + n <= o + 16) {
        // A detour's 5-byte jump at a function entry not inside one aligned 8 bytes: one 16-byte store.
        alignas(16) uint8_t now[16];
        for (;;) {
            memcpy(now, reinterpret_cast<const void*>(o), 16);
            alignas(16) uint8_t want[16];
            memcpy(want, now, 16);
            memcpy(want + (at - o), bytes, n);
#if defined(_MSC_VER)
            LONG64 cmp[2];
            memcpy(cmp, now, 16);
            LONG64 hiLo[2];
            memcpy(hiLo, want, 16);
            if (_InterlockedCompareExchange128(reinterpret_cast<volatile LONG64*>(o), hiLo[1], hiLo[0], cmp)) break;
#else
            unsigned __int128 expect, desired;
            memcpy(&expect, now, 16);
            memcpy(&desired, want, 16);
            if (__sync_bool_compare_and_swap(reinterpret_cast<volatile unsigned __int128*>(o), expect, desired)) break;
#endif
        }
    } else if (othersPaused) {
        memcpy(reinterpret_cast<void*>(at), bytes, n);
    } else {
        bool written = false;
        for (int attempt = 0; attempt < 100 && !written; ++attempt) {
            std::vector<HANDLE> paused;
            pauseOthers(paused);
            if (!anyThreadIn(paused, at, n, true)) {
                memcpy(reinterpret_cast<void*>(at), bytes, n);
                written = true;
            }
            resumeAll(paused);
            if (!written) Sleep(1);
        }
        if (!written) {
            DWORD tmp;
            VirtualProtect(reinterpret_cast<void*>(at), n, prot, &tmp);
            return false; // a thread kept running those bytes
        }
    }
    DWORD tmp;
    VirtualProtect(reinterpret_cast<void*>(at), n, prot, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}

// 16-byte thunks in pages within 2 GB of the call sites. The free regions are found with VirtualQuery,
// above the image first, then below it: in one run nothing could be had by asking at fixed steps
// above the image (2026-11-02). Another run hung loading the trainer (2026-11-02, 11:39): with every
// allocation there refused, trying each 64 KB of 2 GB takes minutes. So the tries are capped and
// timed. A page that cannot be had read-write-execute (some protections refuse that) is taken
// read-write and made executable after its one thunk is written: a thunk in use is never made
// unexecutable again.
std::mutex g_thunkM;
uint8_t* g_thunkPage = nullptr; // (g_thunkM)
size_t g_thunkUsed = 0;
bool g_thunkWritable = false;   // the page is read-write-execute
bool g_thunkSealed = false;     // (not writable) it was made executable: later writes need the threads paused

constexpr int kMaxTries = 64;          // VirtualAlloc calls per search
constexpr int kTriesPerRegion = 8;     // the lowest addresses of each free region
constexpr ULONGLONG kMaxSearchMs = 2000;

// exec: code (read-write-execute, else read-write: writable says which); otherwise data (read-write).
// size: 0x1000, or more for the pool below (a multiple of the allocation granularity).
uint8_t* allocNear(uintptr_t site, bool exec, bool& writable, size_t size = 0x1000) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t lo = site > 0x7ff00000 ? site - 0x7ff00000 : gran, hi = site + 0x7ff00000;
    GN_LOG("patches: looking for a %s page near %llx", exec ? "code" : "data", static_cast<unsigned long long>(mem::va(site)));
    const ULONGLONG start = GetTickCount64();
    int tries = 0;
    DWORD firstError = 0;
    const auto out = [&] { return tries >= kMaxTries || GetTickCount64() - start > kMaxSearchMs; };
    const auto tryAt = [&](uintptr_t a) -> uint8_t* {
        ++tries;
        for (DWORD prot : {DWORD(PAGE_EXECUTE_READWRITE), DWORD(PAGE_READWRITE)}) {
            if (!exec && prot == PAGE_EXECUTE_READWRITE) continue;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, prot)) {
                writable = prot == PAGE_EXECUTE_READWRITE;
                return static_cast<uint8_t*>(p);
            }
            if (!firstError) firstError = GetLastError();
        }
        return nullptr;
    };
    // From the site outwards, the near side first: a site below the image (a page made earlier) looks
    // down first, then up. Each direction has half the time: walking up from the image can pass
    // thousands of the game's regions without a free one (2 s and no try at all, 2026-11-03), and the
    // free space just below the image was never reached. The walks are in near_alloc.hpp (tested on
    // the host); the downward one asks about each candidate itself (2026-11-10: it took every free
    // block to be one page long, so the 64 KB reserve was never made).
    const uintptr_t imageBase = mem::moduleBase(), imageEnd = imageBase + mem::imageSize();
    const nearmem::Query query = [](uint64_t a, nearmem::Region& r) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
        r.base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        r.size = mbi.RegionSize;
        r.free = mbi.State == MEM_FREE;
        r.allocationBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        return true;
    };
    uint8_t* found = nullptr;
    const nearmem::TryAt take = [&](uint64_t a) { return (found = tryAt(static_cast<uintptr_t>(a))) != nullptr; };
    const auto up = [&](uintptr_t from, ULONGLONG until) {
        if (!found) nearmem::searchUp(query, from, hi, size, gran, kTriesPerRegion, take, [&] { return out() || GetTickCount64() >= until; });
    };
    const auto down = [&](uintptr_t from, ULONGLONG until) {
        if (!found) nearmem::searchDown(query, from, lo, size, gran, kTriesPerRegion, take, [&] { return out() || GetTickCount64() >= until; });
    };
    const ULONGLONG half = start + kMaxSearchMs / 2, all = start + kMaxSearchMs;
    if (site < imageBase) {
        // From the end of the granule holding the site: the reserve asks for the page below the image,
        // and the 64 KB just under the image would otherwise never be a candidate.
        down(std::min<uintptr_t>((site + gran) & ~(gran - 1), imageBase), half);
        up(site, all);
    } else {
        up(std::max(site, imageEnd), half);
        down(std::min(site, imageBase), all);
    }
    const unsigned long long ms = GetTickCount64() - start;
    if (found)
        GN_LOG("patches: %s page at %p (%s, %d tries, %llu ms)", exec ? "code" : "data", static_cast<void*>(found),
               !exec ? "rw" : writable ? "rwx" : "rw then rx", tries, ms);
    else
        GN_LOG("patches: no page within 2 GB of %llx (%d tries, %llu ms, first error %lu)", static_cast<unsigned long long>(mem::va(site)),
               tries, ms, static_cast<unsigned long>(firstError));
    return found;
}

// Pages taken near the image when GridNode starts, before the game's heaps fill the 2 GB around it:
// in one run (2026-11-04) no page within reach could be found later at all, so every detour and
// call patch failed. 8 pages of code, then 8 of data (pointer slots); searched for only when used up.
constexpr size_t kPoolPages = 16, kPoolCodePages = 8;
uint8_t* g_pool = nullptr;
bool g_poolWritable = false; // read-write-execute
size_t g_poolCode = 0, g_poolData = 0; // pages handed out (code under g_thunkM, data under g_detM)

uint8_t* poolPage(bool exec, bool& writable) {
    if (!g_pool) return nullptr;
    if (exec) {
        if (g_poolCode >= kPoolCodePages) return nullptr;
        writable = g_poolWritable;
        return g_pool + 0x1000 * g_poolCode++;
    }
    if (g_poolData >= kPoolPages - kPoolCodePages) return nullptr;
    writable = true;
    return g_pool + 0x1000 * (kPoolCodePages + g_poolData++);
}

bool reaches(const uint8_t* p, uintptr_t site) { // a rel32 at site reaches the page p is in
    const intptr_t d = reinterpret_cast<intptr_t>(p) - static_cast<intptr_t>(site + 5);
    return d > INT32_MIN + 0x2000 && d < INT32_MAX - 0x2000;
}

// (g_thunkM) Room for n bytes of code within reach of site; codeDone once they are written.
uint8_t* codeRoom(uintptr_t site, size_t n) {
    n = (n + 15) & ~size_t(15);
    if (n > 0x1000) return nullptr;
    if (!g_thunkPage || g_thunkUsed + n > 0x1000 || !reaches(g_thunkPage, site)) {
        g_thunkPage = nullptr;
        if (uint8_t* p = poolPage(true, g_thunkWritable); p && reaches(p, site)) g_thunkPage = p;
        if (!g_thunkPage) g_thunkPage = allocNear(site, true, g_thunkWritable);
        g_thunkUsed = 0, g_thunkSealed = false;
        if (!g_thunkPage) return nullptr;
    }
    uint8_t* t = g_thunkPage + g_thunkUsed;
    g_thunkUsed += n;
    return t;
}

// (g_thunkM) Room from codeRoom that was never used (the patch failed before anything could run
// it) taken back, when it is the newest; otherwise it stays unused.
void codeGiveBack(uint8_t* t, size_t n) {
    n = (n + 15) & ~size_t(15);
    if (g_thunkPage && t + n == g_thunkPage + g_thunkUsed) g_thunkUsed -= n;
}

// (g_thunkM) Writes code into room from codeRoom. A page that could not be had read-write-execute
// is read-write until its first code is done, then executable: a later write makes it writable for
// a moment with every other thread paused (thunks already on it may be running), instead of
// searching for a new page for each thunk (up to 2 s each, review 2026-11-03).
bool codeWrite(uint8_t* dst, const uint8_t* src, size_t n) {
    if (g_thunkWritable || !g_thunkSealed) {
        memcpy(dst, src, n);
        return true;
    }
    std::vector<HANDLE> paused;
    pauseOthers(paused);
    for (HANDLE h : paused) { // SuspendThread only asks: a thread is stopped once its context can be read
        alignas(16) CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(h, &ctx);
    }
    DWORD old = 0, tmp = 0;
    const bool ok = VirtualProtect(g_thunkPage, 0x1000, PAGE_READWRITE, &old) != 0;
    if (ok) memcpy(dst, src, n);
    const bool back = !ok || VirtualProtect(g_thunkPage, 0x1000, PAGE_EXECUTE_READ, &tmp) != 0;
    resumeAll(paused);
    if (!back) g_thunkPage = nullptr; // not executable any more: no further thunks on it
    return ok && back;
}

bool codeDone(uint8_t* t, size_t n) { // (g_thunkM)
    DWORD tmp = 0;
    if (!g_thunkWritable && !g_thunkSealed) {
        if (!VirtualProtect(g_thunkPage, 0x1000, PAGE_EXECUTE_READ, &tmp)) {
            GN_LOG("patches: the thunk page could not be made executable (error %lu)", static_cast<unsigned long>(GetLastError()));
            g_thunkPage = nullptr;
            return false;
        }
        g_thunkSealed = true;
    }
    FlushInstructionCache(GetCurrentProcess(), t, n);
    return true;
}

uint8_t* thunkTo(void* detour, uintptr_t site) {
    std::lock_guard<std::mutex> lock(g_thunkM);
    uint8_t* t = codeRoom(site, 16);
    if (!t) return nullptr;
    uint8_t code[16] = {0xff, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
    memcpy(code + 6, &detour, 8);
    return codeWrite(t, code, sizeof(code)) && codeDone(t, 16) ? t : nullptr;
}

const Patch* overlapping(uintptr_t at, size_t n) { // g_m held
    for (const Patch& p : g_patches)
        if (at < p.at + p.original.size() && p.at < at + n) return &p;
    return nullptr;
}
bool overlaps(uintptr_t at, size_t n) { return overlapping(at, n) != nullptr; } // g_m held

// written: the bytes are already there (a detour's entry, written with the other threads stopped);
// `original` then holds what was there before.
// (g_m held)
int addLocked(const char* mod, uintptr_t at, const uint8_t* bytes, size_t n, uintptr_t owner, const std::string& what,
              const uint8_t* written = nullptr, bool whole = false) {
    if (unloading()) return GN_ERR_UNAVAILABLE;
    if (const Patch* other = overlapping(at, n)) {
        // Name the other one: two mods patching the same code is a conflict to report with both.
        GN_LOG("[%s] patch at %llx refused: %s's patch is there (%s)", mod, static_cast<unsigned long long>(mem::va(at)), other->mod.c_str(),
               other->what.c_str());
        return GN_ERR_SLOT;
    }
    Patch p{mod, at, std::vector<uint8_t>(n), std::vector<uint8_t>(bytes, bytes + n), owner, what, whole};
    if (written) memcpy(p.original.data(), written, n);
    else if (!mem::read(at, p.original.data(), n) || !writeCode(at, bytes, n, false, whole && spansInstructions(at, n))) {
        GN_LOG("[%s] patch at %llx: the code could not be written (error %lu)", mod, static_cast<unsigned long long>(mem::va(at)),
               static_cast<unsigned long>(GetLastError()));
        return GN_ERR_ARGUMENT;
    }
    GN_LOG("[%s] patched %llu bytes at %llx (%s)", mod, static_cast<unsigned long long>(n), static_cast<unsigned long long>(mem::va(at)), what.c_str());
    g_patches.push_back(std::move(p));
    return GN_OK;
}

int add(const char* mod, uintptr_t at, const uint8_t* bytes, size_t n, uintptr_t owner, const std::string& what, bool whole = false) {
    std::lock_guard<std::mutex> lock(g_m);
    return addLocked(mod, at, bytes, n, owner, what, nullptr, whole);
}

bool undo(const Patch& p) {
    std::vector<uint8_t> now(p.patched.size());
    if (!mem::read(p.at, now.data(), now.size()) || now != p.patched) {
        GN_LOG("[%s] patch at %llx was changed since; left as it is", p.mod.c_str(), static_cast<unsigned long long>(mem::va(p.at)));
        return false;
    }
    // The bytes now there are what a thread may be stopped in.
    return writeCode(p.at, p.original.data(), p.original.size(), false, p.whole && spansInstructions(p.at, p.patched.size()));
}

} // namespace

int patchBytes(const char* mod, uint64_t va, const void* expected, const void* bytes, size_t size, const void* caller) {
    if (!mod || !expected || !bytes || !size || size > 64) return GN_ERR_ARGUMENT;
    const uintptr_t at = mem::rva(va);
    if (!mem::inImage(at) || !mem::inImage(at + size - 1)) return GN_ERR_ARGUMENT;
    std::vector<uint8_t> now(size);
    if (!mem::read(at, now.data(), size)) return GN_ERR_ARGUMENT;
    if (memcmp(now.data(), expected, size) != 0) {
        GN_LOG("[%s] patch at %llx: the bytes there are not the expected ones (another game build?)", mod, static_cast<unsigned long long>(va));
        return GN_ERR_SLOT;
    }
    return add(mod, at, static_cast<const uint8_t*>(bytes), size, reinterpret_cast<uintptr_t>(caller), "bytes", true);
}

int patchCall(const char* mod, uint64_t siteVa, uint64_t expectedTargetVa, void* detour, void** original, const void* caller) {
    if (!mod || !detour || !original || !expectedTargetVa) return GN_ERR_ARGUMENT;
    const uintptr_t site = mem::rva(siteVa);
    uint8_t op[5] = {};
    if (!mem::inImage(site) || !mem::read(site, op, sizeof(op)) || (op[0] != 0xe8 && op[0] != 0xe9)) {
        GN_LOG("[%s] patch_call at %llx: not a call or jmp rel32", mod, static_cast<unsigned long long>(siteVa));
        return GN_ERR_SLOT;
    }
    int32_t rel = 0;
    memcpy(&rel, op + 1, 4);
    const uintptr_t target = site + 5 + static_cast<intptr_t>(rel);
    if (target != mem::rva(expectedTargetVa)) {
        GN_LOG("[%s] patch_call at %llx: it goes to %llx, not %llx", mod, static_cast<unsigned long long>(siteVa),
               static_cast<unsigned long long>(mem::va(target)), static_cast<unsigned long long>(expectedTargetVa));
        return GN_ERR_SLOT;
    }
    uint8_t* thunk = thunkTo(detour, site);
    const intptr_t to = thunk ? reinterpret_cast<intptr_t>(thunk) - static_cast<intptr_t>(site + 5) : 0;
    if (!thunk || to < INT32_MIN || to > INT32_MAX) {
        GN_LOG("[%s] patch_call at %llx: no memory within 2 GB of it for the thunk", mod, static_cast<unsigned long long>(siteVa));
        return GN_ERR_UNAVAILABLE;
    }
    *original = reinterpret_cast<void*>(target); // published before the call goes anywhere new
    const int32_t rel2 = static_cast<int32_t>(to);
    char what[64];
    snprintf(what, sizeof(what), "call to %llx", static_cast<unsigned long long>(expectedTargetVa));
    return add(mod, site + 1, reinterpret_cast<const uint8_t*>(&rel2), 4, reinterpret_cast<uintptr_t>(caller), what);
}

// ---- function detours (hook_function) ----------------------------------------------------------
// The function's first instructions move to a trampoline near the image (gn::x64::trampoline) and a
// 5-byte jmp at its entry goes to a stub, "jmp [rip+x]" through a pointer slot in GridNode's data
// page. That slot is chained by the hook table like any vtable slot: each detour gets the one
// below it as its original, the trampoline (the game's function) last; a mod's detours come out when
// it is unloaded. The entry jump is GridNode's own patch: it stays, going straight through the slot
// to the trampoline, until GridNode is unloaded.
namespace {
// Writes a detour's entry jump with every other thread of the game stopped: a thread stopped inside
// the instructions being replaced (most functions start with a 1-byte push, so the jump covers
// several) goes on from the same instruction in the trampoline. Nothing here allocates or logs while
// the threads are stopped (one of them may hold the heap's or the log's lock).
bool writeEntry(uintptr_t fn, const uint8_t (&jmp)[5], size_t copied, const std::vector<std::pair<size_t, size_t>>& starts, uint8_t* tramp,
                int& moved) {
    std::vector<HANDLE> stopped;
    pauseOthers(stopped);
    moved = 0;
    for (HANDLE h : stopped) {
        alignas(16) CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(h, &ctx) || ctx.Rip < fn || ctx.Rip >= fn + copied) continue;
        for (const auto& [from, to] : starts)
            if (ctx.Rip == fn + from) {
                ctx.Rip = reinterpret_cast<uintptr_t>(tramp) + to;
                if (SetThreadContext(h, &ctx)) ++moved;
                break;
            }
    }
    const bool ok = writeCode(fn, jmp, 5, true);
    resumeAll(stopped);
    return ok;
}

struct Detour {
    uint8_t* tramp = nullptr;
    uint64_t* slot = nullptr;
};
std::mutex g_detM;
std::map<uintptr_t, Detour> g_detours; // (g_detM) by function
uint8_t* g_slotPage = nullptr;          // (g_detM) pointer slots, read-write data
size_t g_slotUsed = 0;

bool isGameCode(uintptr_t fn) {
    if (!mem::inImage(fn)) return false;
    for (const mem::Section& sec : mem::imageSections())
        if (sec.region.code && fn >= sec.region.begin && fn < sec.region.end) return true;
    return false;
}

// (g_detM) The function's slot, made on first use. GN_OK, or an error already logged.
int detourSlot(const char* mod, uintptr_t fn, Detour& out) {
    if (auto it = g_detours.find(fn); it != g_detours.end()) {
        out = it->second;
        return GN_OK;
    }
    uint8_t code[64] = {};
    if (!mem::read(fn, code, sizeof(code))) return GN_ERR_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_thunkM);
    uint8_t* room = codeRoom(fn, 128); // trampoline (up to 0x60), then the stub at +0x70
    if (!room) {
        GN_LOG("[%s] hook_function %llx: no memory within 2 GB of it", mod, static_cast<unsigned long long>(mem::va(fn)));
        return GN_ERR_UNAVAILABLE;
    }
    std::vector<uint8_t> tramp;
    std::vector<std::pair<size_t, size_t>> starts;
    size_t copied = 0;
    std::string why;
    if (!x64::trampoline(code, sizeof(code), fn, 5, reinterpret_cast<uint64_t>(room), tramp, copied, &why, &starts) || tramp.size() > 0x60) {
        GN_LOG("[%s] hook_function %llx: %s", mod, static_cast<unsigned long long>(mem::va(fn)), why.empty() ? "trampoline too long" : why.c_str());
        codeGiveBack(room, 128);
        return GN_ERR_SLOT;
    }
    uint8_t* stub = room + 0x70;
    if (!g_slotPage || g_slotUsed + 8 > 0x1000 || !reaches(g_slotPage, reinterpret_cast<uintptr_t>(stub))) {
        bool unused = false;
        g_slotPage = nullptr;
        if (uint8_t* p = poolPage(false, unused); p && reaches(p, reinterpret_cast<uintptr_t>(stub))) g_slotPage = p;
        if (!g_slotPage) g_slotPage = allocNear(reinterpret_cast<uintptr_t>(stub), false, unused);
        g_slotUsed = 0;
        if (!g_slotPage) {
            codeGiveBack(room, 128);
            return GN_ERR_UNAVAILABLE;
        }
    }
    auto* slot = reinterpret_cast<uint64_t*>(g_slotPage + g_slotUsed);
    g_slotUsed += 8;
    // Until the entry jump is written nothing can reach the room or the slot: a failure before that
    // gives both back (they leaked, review 2026-11-04).
    auto giveBack = [&] {
        if (reinterpret_cast<uint8_t*>(slot) + 8 == g_slotPage + g_slotUsed) g_slotUsed -= 8;
        codeGiveBack(room, 128);
    };
    *slot = reinterpret_cast<uint64_t>(room);
    uint8_t block[128] = {};
    memcpy(block, tramp.data(), tramp.size());
    const int32_t toSlot = static_cast<int32_t>(reinterpret_cast<intptr_t>(slot) - reinterpret_cast<intptr_t>(stub + 6));
    block[0x70] = 0xff, block[0x71] = 0x25; // jmp [rip+toSlot]
    memcpy(block + 0x72, &toSlot, 4);
    if (!codeWrite(room, block, sizeof(block)) || !codeDone(room, 128)) {
        giveBack();
        return GN_ERR_UNAVAILABLE;
    }
    // The entry: jmp rel32 to the stub, GridNode's patch (undone when GridNode is unloaded).
    uint8_t entry[5] = {0xe9};
    const int32_t toStub = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - static_cast<intptr_t>(fn + 5));
    memcpy(entry + 1, &toStub, 4);
    // The overlap check, the write and the record under one lock: an unload or another patch
    // between the write and the record left a live jump that unload did not know of (review,
    // 2026-11-04).
    std::lock_guard<std::mutex> patchLock(g_m);
    if (unloading()) {
        giveBack();
        return GN_ERR_UNAVAILABLE;
    }
    if (overlaps(fn, 5)) {
        GN_LOG("[%s] hook_function %llx: a code patch is there", mod, static_cast<unsigned long long>(mem::va(fn)));
        giveBack();
        return GN_ERR_SLOT;
    }
    int moved = 0;
    // One instruction of 5 bytes or more at the entry: the jump replaces only it, no thread can be
    // half way through it, and one store is enough (writeCode pauses only when the 5 bytes cross a
    // 16-byte line). Three of the seven detours in gridnode.prev.log (2026-11-10) were this case.
    const size_t firstLength = starts.size() > 1 ? starts[1].first : copied;
    const bool single = firstLength >= 5;
    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    const bool entryWritten = single ? writeCode(fn, entry, 5) : writeEntry(fn, entry, copied, starts, room, moved);
    QueryPerformanceCounter(&t1);
    const double pauseMs = freq.QuadPart ? double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart) : 0.0;
    if (!entryWritten) {
        GN_LOG("[%s] hook_function %llx: the entry could not be written (error %lu)", mod, static_cast<unsigned long long>(mem::va(fn)),
               static_cast<unsigned long>(GetLastError()));
        giveBack();
        return GN_ERR_SLOT;
    }
    char what[128];
    snprintf(what, sizeof(what), "detour entry (%llu bytes moved, %s %.1f ms%s)", static_cast<unsigned long long>(copied),
             single ? "one instruction, written in" : "threads paused for", pauseMs, moved ? ", a thread moved into the trampoline" : "");
    // Listed, undone at a full unload. Cannot fail here (checked above, under the same lock); if it
    // ever does, the entry goes back at once rather than stay unlisted.
    if (addLocked("GridNode", fn, entry, 5, reinterpret_cast<uintptr_t>(&detourSlot), what, code) != GN_OK) {
        if (!writeCode(fn, code, 5)) GN_LOG("[%s] hook_function %llx: the entry could not be put back", mod, static_cast<unsigned long long>(mem::va(fn)));
        // The room and slot stay: a thread may already be in the trampoline.
        return GN_ERR_SLOT;
    }
    out = {room, slot};
    g_detours[fn] = out;
    return GN_OK;
}
} // namespace

void patchesReserve() {
    std::lock_guard<std::mutex> lock(g_thunkM);
    if (g_pool) return;
    // Below the image first: the game's heaps grow up from above it.
    g_pool = allocNear(mem::moduleBase() - 0x1000, true, g_poolWritable, kPoolPages * 0x1000);
    // Then above it: in one run (2026-11-04) the space below was all small regions (1 s, no free one)
    // while a page above the image was found at once.
    if (!g_pool) g_pool = allocNear(mem::moduleBase() + mem::imageSize(), true, g_poolWritable, kPoolPages * 0x1000);
    if (g_pool) GN_LOG("patches: %llu KB kept near the game's code for detours and call patches", static_cast<unsigned long long>(kPoolPages * 4));
}

int hookFunction(const char* mod, uint64_t fnVa, void* detour, void** original) {
    if (!mod || !detour || !original || !fnVa) return GN_ERR_ARGUMENT;
    const uintptr_t fn = mem::rva(fnVa);
    if (!isGameCode(fn)) {
        GN_LOG("[%s] hook_function %llx: not in the game's code", mod, static_cast<unsigned long long>(fnVa));
        return GN_ERR_ARGUMENT;
    }
    if (unloading()) return GN_ERR_UNAVAILABLE;
    Detour d;
    {
        std::lock_guard<std::mutex> lock(g_detM);
        if (const int r = detourSlot(mod, fn, d); r != GN_OK) return r;
    }
    // Not under g_detM: the hook table writes the slot through isDetourSlot.
    std::string why;
    const auto r = hooks().install(mod, reinterpret_cast<uint64_t>(d.slot), reinterpret_cast<uint64_t>(d.tramp), reinterpret_cast<uint64_t>(detour),
                                   reinterpret_cast<uint64_t*>(original), &why);
    if (r != hook::Result::Ok) {
        GN_LOG("[%s] hook_function %llx refused: %s", mod, static_cast<unsigned long long>(fnVa), why.c_str());
        return GN_ERR_SLOT;
    }
    GN_LOG("[%s] hooked function %llx", mod, static_cast<unsigned long long>(fnVa));
    noteSharedHook(mod, reinterpret_cast<uint64_t>(d.slot));
    return GN_OK;
}

bool isDetourSlot(uint64_t slot) {
    std::lock_guard<std::mutex> lock(g_detM);
    for (const auto& [fn, d] : g_detours)
        if (reinterpret_cast<uint64_t>(d.slot) == slot) return true;
    return false;
}

uint64_t detourFunction(uint64_t slot) {
    std::lock_guard<std::mutex> lock(g_detM);
    for (const auto& [fn, d] : g_detours)
        if (reinterpret_cast<uint64_t>(d.slot) == slot) return mem::va(fn);
    return 0;
}

size_t forgetPatches(const CodeRange& r) {
    std::lock_guard<std::mutex> lock(g_m);
    size_t n = 0;
    for (auto it = g_patches.end(); it != g_patches.begin();) { // newest first
        --it;
        if (!r.has(reinterpret_cast<void*>(it->owner))) continue;
        undo(*it);
        it = g_patches.erase(it);
        ++n;
    }
    return n;
}

size_t removeAllPatches() {
    std::lock_guard<std::mutex> lock(g_m);
    size_t n = 0;
    for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it) n += undo(*it) ? 1 : 0;
    g_patches.clear();
    return n;
}

std::vector<PatchInfo> patchList() {
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<PatchInfo> out;
    for (const Patch& p : g_patches) out.push_back({p.mod, mem::va(p.at), p.original.size(), p.what});
    return out;
}

std::vector<std::pair<uintptr_t, uint64_t>> patchOriginals() {
    // Aligned 8-byte values with the original bytes in, for an image dump.
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<std::pair<uintptr_t, uint64_t>> out;
    for (const Patch& p : g_patches)
        for (uintptr_t q = p.at & ~uintptr_t(7); q < p.at + p.original.size(); q += 8) {
            uint64_t v = 0;
            if (!mem::read(q, v)) continue;
            for (size_t i = 0; i < 8; ++i)
                if (q + i >= p.at && q + i < p.at + p.original.size()) reinterpret_cast<uint8_t*>(&v)[i] = p.original[q + i - p.at];
            out.push_back({q, v});
        }
    return out;
}

} // namespace gn::host
