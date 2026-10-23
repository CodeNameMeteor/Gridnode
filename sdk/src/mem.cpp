#include "gridnode/mem.hpp"

#include <cstdio>
#include <vector>

#include "gridnode/log.hpp"

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

bool foreignCode(uintptr_t fn, std::string* module) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!fn || inImage(fn) || !VirtualQuery(reinterpret_cast<void*>(fn), &mbi, sizeof(mbi))) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & exec)) return false;
    HMODULE owner = nullptr, self = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(fn), &owner) || !owner) return false;
    GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&foreignCode), &self);
    if (owner == self || owner == GetModuleHandleW(nullptr)) return false;
    if (module) {
        char name[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(owner, name, MAX_PATH);
        const char* base = name;
        for (DWORD i = 0; i < n; ++i)
            if (name[i] == '\\' || name[i] == '/') base = name + i + 1;
        *module = base;
    }
    return true;
}

bool dumpImage(const std::wstring& path, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    const uintptr_t base = moduleBase();
    const IMAGE_NT_HEADERS* nt = ntHeaders();
    const size_t size = nt->OptionalHeader.SizeOfImage;
    const size_t headers = nt->OptionalHeader.SizeOfHeaders;
    if (!size || headers > size) return fail("the image headers are unreadable");
    std::vector<uint8_t> img(size, 0);
    // Page by page: a page that cannot be read (a guard page, decommitted) stays zero.
    size_t unreadable = 0;
    for (size_t off = 0; off < size; off += 0x1000) {
        const size_t n = (size - off) < 0x1000 ? size - off : 0x1000;
        if (!read(base + off, img.data() + off, n)) ++unreadable;
    }
    // Headers: the image base as loaded, and every section's file offset = its RVA (a memory-layout file).
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.data());
    auto* fileNt = reinterpret_cast<IMAGE_NT_HEADERS*>(img.data() + dos->e_lfanew);
    fileNt->OptionalHeader.ImageBase = static_cast<ULONGLONG>(layout::kImageBase);
    fileNt->OptionalHeader.FileAlignment = fileNt->OptionalHeader.SectionAlignment;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(fileNt);
    for (WORD i = 0; i < fileNt->FileHeader.NumberOfSections; ++i) {
        const DWORD vsize = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        sec[i].PointerToRawData = sec[i].VirtualAddress;
        sec[i].SizeOfRawData = sec[i].VirtualAddress + vsize <= size ? vsize : static_cast<DWORD>(size - sec[i].VirtualAddress);
    }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return fail("the file could not be created");
    const bool ok = fwrite(img.data(), 1, img.size(), f) == img.size();
    fclose(f);
    if (!ok) return fail("the file could not be written (disk full?)");
    GN_LOG("image: %llu MB written to %ls (%llu unreadable pages left as zeros)", static_cast<unsigned long long>(size >> 20),
           path.c_str(), static_cast<unsigned long long>(unreadable));
    return true;
}

namespace {
bool writeSlot(uintptr_t addr, uintptr_t value, uintptr_t* old) {
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

// The slot lies in a loaded module's image, in a section that holds no code.
bool moduleDataSlot(uintptr_t addr) {
    if (addr & 7) return false;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod) ||
        !mod)
        return false;
    const auto base = reinterpret_cast<uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        const uintptr_t b = base + sec->VirtualAddress;
        const uintptr_t e = b + (sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData);
        if (addr >= b && addr + sizeof(uintptr_t) <= e)
            return !(sec->Characteristics & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE));
    }
    return false;
}
} // namespace

bool patchPointer(uintptr_t addr, uintptr_t value, uintptr_t* old) {
    if (!patchableSlot(addr)) return false; // aligned (so the store is atomic), in the image's data
    return writeSlot(addr, value, old);
}

bool patchModulePointer(uintptr_t addr, uintptr_t value, uintptr_t* old) {
    if (inImage(addr) || !moduleDataSlot(addr)) return false; // the game's image goes through patchPointer
    return writeSlot(addr, value, old);
}

} // namespace gn::mem
