#include "gridnode/mem.hpp"

namespace gn::mem {

const std::vector<reflect::Region>& imageRegions() {
    static const std::vector<reflect::Region> regions = [] {
        std::vector<reflect::Region> out;
        const uintptr_t base = moduleBase();
        const uintptr_t end = base + imageSize();
        for (uintptr_t a = base; a < end;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) || !mbi.RegionSize) break;
            const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t re = rb + mbi.RegionSize < end ? rb + mbi.RegionSize : end;
            const DWORD p = mbi.Protect;
            const bool usable = mbi.State == MEM_COMMIT && !(p & (PAGE_GUARD | PAGE_NOACCESS));
            const bool code = (p & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            const bool data = (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) != 0;
            if (usable && (code || data)) {
                if (!out.empty() && out.back().end == rb && out.back().code == code) out.back().end = re;
                else out.push_back({rb, re, code});
            }
            a = rb + mbi.RegionSize;
        }
        return out;
    }();
    return regions;
}

bool readCString(uintptr_t addr, std::string& out, size_t maxLen) {
    ProcessMemory m;
    out = m.readCString(addr, maxLen);
    return !out.empty();
}

bool patchPointer(uintptr_t addr, uintptr_t value, uintptr_t* old) {
    if (addr & 7) return false; // must be an aligned slot for the store to be atomic
    DWORD prot = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), PAGE_READWRITE, &prot)) return false;
    if (old) *old = *reinterpret_cast<volatile uintptr_t*>(addr);
    *reinterpret_cast<volatile uintptr_t*>(addr) = value;
    DWORD tmp;
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), prot, &tmp);
    return true;
}

} // namespace gn::mem
