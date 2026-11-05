// The console's "sample [seconds]": a small sampling profiler for finding where a frame's time goes.
// It picks the busiest game threads (by cycles over half a second), then every millisecond or so
// suspends one, reads its instruction pointer and resumes it. Samples are only counted into
// preallocated buffers while a thread is suspended (it may hold the heap or log lock), and resolved
// to function starts afterwards. Two profiles (say with and without the free camera) can be compared
// by function address in Ghidra.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gridnode/log.hpp"
#include "host.hpp"

#include <tlhelp32.h>

namespace gn::host {
namespace {
constexpr int kMaxSeconds = 30;
constexpr size_t kPicked = 3;      // threads sampled
constexpr size_t kTopFunctions = 14;
constexpr size_t kDepth = 12; // frames kept per sample

struct Sample {
    uint64_t frames[kDepth];
    uint8_t n;
};

HANDLE g_thread = nullptr;
std::atomic<bool> g_running{false}, g_stop{false};

struct Th {
    DWORD id = 0;
    HANDLE h = nullptr;
    uint64_t cycles = 0;
    uint64_t share = 0; // permille of all threads' cycles
};

std::string moduleOf(uint64_t addr, uint64_t* offset) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &m) || !m) {
        *offset = addr;
        return "?";
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    *offset = addr - reinterpret_cast<uint64_t>(m);
    return base ? base + 1 : path;
}

DWORD WINAPI run(LPVOID param) {
    const int seconds = static_cast<int>(reinterpret_cast<uintptr_t>(param));
    std::vector<Th> all;
    all.reserve(256);
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            Th t;
            t.id = te.th32ThreadID;
            t.h = h;
            all.push_back(t);
        }
        CloseHandle(snap);
    }
    std::vector<uint64_t> before(all.size(), 0);
    for (size_t i = 0; i < all.size(); ++i) QueryThreadCycleTime(all[i].h, &before[i]);
    Sleep(500);
    uint64_t total = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        uint64_t now = 0;
        QueryThreadCycleTime(all[i].h, &now);
        all[i].cycles = now > before[i] ? now - before[i] : 0;
        total += all[i].cycles;
    }
    for (auto& t : all) t.share = total ? t.cycles * 1000 / total : 0;
    std::sort(all.begin(), all.end(), [](const Th& a, const Th& b) { return a.cycles > b.cycles; });

    GN_LOG("sample: busiest threads (share of all threads' cycles over 0.5 s):");
    for (size_t i = 0; i < all.size() && i < 6; ++i) GN_LOG("sample:   thread %lu: %.1f%%", all[i].id, all[i].share / 10.0);

    const size_t n = std::min(kPicked, all.size());
    const size_t cap = static_cast<size_t>(seconds) * 1000 + 256;
    std::vector<std::vector<Sample>> rips(n);
    for (auto& v : rips) v.reserve(cap);
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    uint32_t rounds = 0;
    while (GetTickCount64() < end && !g_stop.load()) {
        for (size_t i = 0; i < n; ++i) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            Sample smp{};
            if (SuspendThread(all[i].h) != static_cast<DWORD>(-1)) {
                if (GetThreadContext(all[i].h, &ctx)) {
                    // Unwind while suspended: only reads the thread's stack and the image's unwind tables.
                    while (smp.n < kDepth && ctx.Rip) {
                        smp.frames[smp.n++] = ctx.Rip;
                        DWORD64 base = 0;
                        const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                        if (f) {
                            PVOID handler = nullptr;
                            DWORD64 frame = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, const_cast<PRUNTIME_FUNCTION>(f), &ctx,
                                             &handler, &frame, nullptr);
                        } else {
                            uint64_t ret = 0;
                            SIZE_T got = 0;
                            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ctx.Rsp), &ret, 8, &got) || got != 8) break;
                            ctx.Rip = ret;
                            ctx.Rsp += 8;
                        }
                    }
                }
                ResumeThread(all[i].h);
            }
            if (smp.n && rips[i].size() < rips[i].capacity()) rips[i].push_back(smp);
        }
        ++rounds;
        Sleep(1);
    }

    for (size_t i = 0; i < n; ++i) {
        const uint64_t exeBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exeBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase)->e_lfanew);
        const uint64_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;
        auto funcStart = [](uint64_t rip) {
            DWORD64 base = 0;
            const RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(rip, &base, nullptr);
            return f ? base + f->BeginAddress : rip;
        };
        std::map<uint64_t, uint32_t> byLeaf, byGame;
        for (const Sample& smp : rips[i]) {
            ++byLeaf[funcStart(smp.frames[0])];
            uint64_t game = 0; // innermost frame inside the game exe: who the time is spent for
            for (uint8_t k = 0; k < smp.n; ++k)
                if (smp.frames[k] >= exeBase && smp.frames[k] < exeEnd) {
                    game = funcStart(smp.frames[k]);
                    break;
                }
            ++byGame[game];
        }
        GN_LOG("sample: thread %lu (%.1f%% of cycles), %zu samples in %u rounds:", all[i].id, all[i].share / 10.0,
               rips[i].size(), rounds);
        for (int pass = 0; pass < 2; ++pass) {
            const auto& m = pass ? byGame : byLeaf;
            std::vector<std::pair<uint64_t, uint32_t>> top(m.begin(), m.end());
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            GN_LOG("sample:  %s", pass ? "innermost game function on the stack (0 = none):" : "function the thread is in:");
            for (size_t k = 0; k < top.size() && k < kTopFunctions; ++k) {
                uint64_t off = 0;
                const std::string mod = moduleOf(top[k].first, &off);
                GN_LOG("sample:   %5.1f%%  0x%llx (%s+0x%llx)", 100.0 * top[k].second / std::max<size_t>(1, rips[i].size()),
                       static_cast<unsigned long long>(top[k].first), mod.c_str(), static_cast<unsigned long long>(off));
            }
        }
    }
    for (auto& t : all) CloseHandle(t.h);
    GN_LOG("sample: done");
    g_running = false;
    return 0;
}
} // namespace

std::string sampleCommand(const std::string& args) {
    int seconds = 5;
    if (!args.empty()) seconds = std::atoi(args.c_str());
    seconds = std::clamp(seconds, 1, kMaxSeconds);
    if (g_running.exchange(true)) return "sample: already running";
    if (g_thread) {
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    g_stop = false;
    g_thread = CreateThread(nullptr, 0, run, reinterpret_cast<LPVOID>(static_cast<uintptr_t>(seconds)), 0, nullptr);
    if (!g_thread) {
        g_running = false;
        return "sample: could not start";
    }
    return "sample: sampling the busiest threads for " + std::to_string(seconds) + " s, result in gridnode.log";
}

void sampleRelease() {
    g_stop = true;
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}
} // namespace gn::host
