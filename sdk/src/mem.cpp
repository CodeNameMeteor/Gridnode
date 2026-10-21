#include "gridnode/mem.hpp"

namespace gn::mem {

const std::vector<Section>& imageSections() {
    static const std::vector<Section> sections = [] {
        std::vector<Section> out;
        const IMAGE_NT_HEADERS* nt = ntHeaders();
        const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
        const uintptr_t base = moduleBase();
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            Section s;
            memcpy(s.name, sh[i].Name, 8);
            const DWORD size = sh[i].Misc.VirtualSize > sh[i].SizeOfRawData ? sh[i].Misc.VirtualSize : sh[i].SizeOfRawData;
            s.region = {base + sh[i].VirtualAddress, base + sh[i].VirtualAddress + size,
                        (sh[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0};
            out.push_back(s);
        }
        return out;
    }();
    return sections;
}

std::vector<reflect::Region> imageSectionRegions() {
    std::vector<reflect::Region> out;
    for (const Section& s : imageSections()) out.push_back(s.region);
    return out;
}

const std::vector<reflect::Region>& imageReadableRanges() {
    static const std::vector<reflect::Region> ranges = [] {
        std::vector<reflect::Region> out;
        const uintptr_t base = moduleBase();
        const uintptr_t end = base + imageSize();
        for (uintptr_t a = base; a < end;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) || !mbi.RegionSize) break;
            const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t re = rb + mbi.RegionSize < end ? rb + mbi.RegionSize : end;
            const DWORD p = mbi.Protect;
            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (mbi.State == MEM_COMMIT && !(p & (PAGE_GUARD | PAGE_NOACCESS)) && (p & readable)) {
                if (!out.empty() && out.back().end == rb) out.back().end = re;
                else out.push_back({rb, re, false});
            }
            a = rb + mbi.RegionSize;
        }
        return out;
    }();
    return ranges;
}

bool readCString(uintptr_t addr, std::string& out, size_t maxLen) {
    ProcessMemory m;
    out = m.readCString(addr, maxLen);
    return !out.empty();
}

// Only pointer slots of the game's image, outside its code sections (vtables, creator tables, data):
// a wrong address must not rewrite code or someone else's memory.
bool patchableSlot(uintptr_t addr) {
    if ((addr & 7) || !inImage(addr) || !inImage(addr + sizeof(uintptr_t) - 1)) return false;
    for (const Section& sec : imageSections())
        if (sec.region.code && addr >= sec.region.begin && addr < sec.region.end) return false;
    return true;
}

bool patchPointer(uintptr_t addr, uintptr_t value, uintptr_t* old) {
    if (!patchableSlot(addr)) return false; // aligned (so the store is atomic), in the image's data
    // Keep the page executable if it is: in this game every page of the image is, and code may share
    // the page with the slot; taking execute away for the write would fault a thread running it.
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD want = (mbi.Protect & exec) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD prot = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), want, &prot)) return false;
    if (old) *old = *reinterpret_cast<volatile uintptr_t*>(addr);
    *reinterpret_cast<volatile uintptr_t*>(addr) = value;
    DWORD tmp;
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), prot, &tmp);
    return true;
}

} // namespace gn::mem
