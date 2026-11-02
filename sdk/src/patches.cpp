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
#include <mutex>
#include <string>
#include <vector>

#include "gridnode/log.hpp"
#include "gridnode/mem.hpp"
#include "host.hpp"

namespace gn::host {
namespace {

struct Patch {
    std::string mod;
    uintptr_t at = 0;
    std::vector<uint8_t> original, patched;
    uintptr_t owner = 0; // an address in the code of whoever made it (a mod's image, or GridNode's)
    std::string what;
};
std::mutex g_m;
std::vector<Patch> g_patches; // (g_m)

// Writes code bytes in one store when they fit in an aligned 8 bytes (a thread may be running it).
bool writeCode(uintptr_t at, const uint8_t* bytes, size_t n) {
    DWORD prot = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &prot)) return false;
    const uintptr_t q = at & ~uintptr_t(7);
    if (at + n <= q + 8) {
        volatile LONG64* p = reinterpret_cast<volatile LONG64*>(q);
        LONG64 old = *p;
        for (;;) {
            LONG64 now = old;
            memcpy(reinterpret_cast<uint8_t*>(&now) + (at - q), bytes, n);
            const LONG64 seen = InterlockedCompareExchange64(p, now, old);
            if (seen == old) break;
            old = seen;
        }
    } else {
        memcpy(reinterpret_cast<void*>(at), bytes, n);
    }
    DWORD tmp;
    VirtualProtect(reinterpret_cast<void*>(at), n, prot, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}

// 16-byte thunks in pages within 2 GB of the call sites. The free regions are found with VirtualQuery,
// above the image first, then below it: in one run nothing could be had by asking at fixed steps
// above the image (2026-11-02).
std::mutex g_thunkM;
uint8_t* g_thunkPage = nullptr; // (g_thunkM)
size_t g_thunkUsed = 0;

uint8_t* allocNear(uintptr_t site) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t lo = site > 0x7ff00000 ? site - 0x7ff00000 : gran, hi = site + 0x7ff00000;
    auto tryRegion = [&](uintptr_t from, uintptr_t to) -> uint8_t* { // a free region [from, to)
        for (uintptr_t a = (from + gran - 1) & ~(gran - 1); a + 0x1000 <= to; a += gran)
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
                return static_cast<uint8_t*>(p);
        return nullptr;
    };
    const uintptr_t imageEnd = mem::moduleBase() + mem::imageSize();
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t a = imageEnd; a < hi && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));) { // above
        const uintptr_t b = reinterpret_cast<uintptr_t>(mbi.BaseAddress), e = b + mbi.RegionSize;
        if (mbi.State == MEM_FREE)
            if (uint8_t* p = tryRegion(std::max(b, a), std::min(e, hi))) return p;
        if (e <= a) break;
        a = e;
    }
    for (uintptr_t a = mem::moduleBase(); a > lo && VirtualQuery(reinterpret_cast<void*>(a - 1), &mbi, sizeof(mbi));) { // below
        const uintptr_t b = reinterpret_cast<uintptr_t>(mbi.BaseAddress), e = b + mbi.RegionSize;
        if (mbi.State == MEM_FREE) {
            const uintptr_t from = std::max(b, lo), to = std::min(e, a);
            if (to > from + 0x1000)
                if (uint8_t* p = tryRegion(((to - 0x1000) & ~(gran - 1)) > from ? (to - 0x1000) & ~(gran - 1) : from, to)) return p;
        }
        if (b >= a || b == 0) break;
        a = b;
    }
    return nullptr;
}

uint8_t* thunkTo(void* detour, uintptr_t site) {
    std::lock_guard<std::mutex> lock(g_thunkM);
    const auto reaches = [site](const uint8_t* p) {
        const intptr_t d = reinterpret_cast<intptr_t>(p) - static_cast<intptr_t>(site + 5);
        return d > INT32_MIN + 0x2000 && d < INT32_MAX - 0x2000;
    };
    if (!g_thunkPage || g_thunkUsed + 16 > 0x1000 || !reaches(g_thunkPage)) {
        g_thunkPage = allocNear(site);
        g_thunkUsed = 0;
        if (!g_thunkPage) return nullptr;
    }
    uint8_t* t = g_thunkPage + g_thunkUsed;
    g_thunkUsed += 16;
    const uint8_t jmp[6] = {0xff, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
    memcpy(t, jmp, 6);
    memcpy(t + 6, &detour, 8);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    return t;
}

bool overlaps(uintptr_t at, size_t n) { // g_m held
    for (const Patch& p : g_patches)
        if (at < p.at + p.original.size() && p.at < at + n) return true;
    return false;
}

int add(const char* mod, uintptr_t at, const uint8_t* bytes, size_t n, uintptr_t owner, const std::string& what) {
    std::lock_guard<std::mutex> lock(g_m);
    if (unloading()) return GN_ERR_UNAVAILABLE;
    if (overlaps(at, n)) {
        GN_LOG("[%s] patch at %llx: another patch is there", mod, static_cast<unsigned long long>(mem::va(at)));
        return GN_ERR_SLOT;
    }
    Patch p{mod, at, std::vector<uint8_t>(n), std::vector<uint8_t>(bytes, bytes + n), owner, what};
    if (!mem::read(at, p.original.data(), n) || !writeCode(at, bytes, n)) {
        GN_LOG("[%s] patch at %llx: the code could not be written (error %lu)", mod, static_cast<unsigned long long>(mem::va(at)),
               static_cast<unsigned long>(GetLastError()));
        return GN_ERR_ARGUMENT;
    }
    GN_LOG("[%s] patched %llu bytes at %llx (%s)", mod, static_cast<unsigned long long>(n), static_cast<unsigned long long>(mem::va(at)), what.c_str());
    g_patches.push_back(std::move(p));
    return GN_OK;
}

bool undo(const Patch& p) {
    std::vector<uint8_t> now(p.patched.size());
    if (!mem::read(p.at, now.data(), now.size()) || now != p.patched) {
        GN_LOG("[%s] patch at %llx was changed since; left as it is", p.mod.c_str(), static_cast<unsigned long long>(mem::va(p.at)));
        return false;
    }
    return writeCode(p.at, p.original.data(), p.original.size());
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
    return add(mod, at, static_cast<const uint8_t*>(bytes), size, reinterpret_cast<uintptr_t>(caller), "bytes");
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
