#pragma once
// Fault-tolerant access to game memory from inside the game process.
//
// read()/write() go through the kernel, so a bad pointer returns false instead of crashing.
// That is slower than a plain load: use it for anything the mod did not allocate itself, and
// readFast() for the game image's own data (vtables, type info, globals), which is always mapped.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gridnode/reflect/layout.hpp"
#include "gridnode/reflect/walker.hpp"

namespace gn::mem {

inline uintptr_t moduleBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }

// Image VA (as in Ghidra, base 0x140000000) -> address in the running process.
inline uintptr_t rva(uint64_t va) { return moduleBase() + static_cast<uintptr_t>(va - layout::kImageBase); }
// Address in the running process -> image VA.
inline uint64_t va(uintptr_t addr) { return static_cast<uint64_t>(addr - moduleBase()) + layout::kImageBase; }

inline const IMAGE_NT_HEADERS* ntHeaders() {
    const uintptr_t base = moduleBase();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
}
inline size_t imageSize() { return ntHeaders()->OptionalHeader.SizeOfImage; }
inline bool inImage(uintptr_t a) { return a >= moduleBase() && a < moduleBase() + imageSize(); }

inline bool read(uintptr_t addr, void* out, size_t size) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, size, &got) && got == size;
}
template <class T> inline bool read(uintptr_t addr, T& out) { return read(addr, &out, sizeof(T)); }
template <class T> inline T readOr(uintptr_t addr, T fallback = T{}) {
    T v{};
    return read(addr, v) ? v : fallback;
}

// Fault-tolerant write. NtWriteVirtualMemory skips WriteProcessMemory's protection changes and
// instruction cache flush (made for patching code) and is much cheaper; WriteProcessMemory is
// the fallback (e.g. for a read-only page).
inline bool write(uintptr_t addr, const void* src, size_t size) {
    if (addr < 0x10000) return false;
    using NtWriteFn = LONG(NTAPI*)(HANDLE, PVOID, LPCVOID, SIZE_T, SIZE_T*);
    static const NtWriteFn ntWrite = reinterpret_cast<NtWriteFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtWriteVirtualMemory")));
    SIZE_T done = 0;
    if (ntWrite && ntWrite(GetCurrentProcess(), reinterpret_cast<PVOID>(addr), src, size, &done) >= 0 && done == size)
        return true;
    done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), src, size, &done) && done == size;
}
template <class T> inline bool write(uintptr_t addr, const T& v) { return write(addr, &v, sizeof(T)); }

// The image's sections from its PE section table (code = IMAGE_SCN_MEM_EXECUTE). Page protections
// are not used here: they do not reliably tell code from data in this image.
struct Section {
    char name[9] = {};
    reflect::Region region;
};
const std::vector<Section>& imageSections();
std::vector<reflect::Region> imageSectionRegions();

// Image ranges whose pages are committed and readable (VirtualQuery, found once; the image stays
// mapped for the life of the process). Reads entirely inside one are plain copies. In the running
// game every page of the image is committed and executable-readable, so this is normally the whole
// image.
const std::vector<reflect::Region>& imageReadableRanges();

inline bool inImageReadable(uintptr_t addr, size_t size) {
    for (const reflect::Region& r : imageReadableRanges())
        if (addr >= r.begin && addr + size <= r.end && addr + size >= addr) return true;
    return false;
}

// As read(), but a plain copy when the bytes lie in the image.
inline bool readFast(uintptr_t addr, void* out, size_t size) {
    if (inImageReadable(addr, size)) {
        memcpy(out, reinterpret_cast<const void*>(addr), size);
        return true;
    }
    return read(addr, out, size);
}
template <class T> inline bool readFast(uintptr_t addr, T& out) { return readFast(addr, &out, sizeof(T)); }

// NUL-terminated printable ASCII (false if unreadable or not printable).
bool readCString(uintptr_t addr, std::string& out, size_t maxLen = 256);

// Swaps one pointer in read-only image data (a vtable or creator slot). The aligned 8-byte store
// is atomic on x64, so a concurrent virtual call sees either the old or the new function.
// This is the only kind of write GridNode makes to the game image: code bytes are never patched.
bool patchableSlot(uintptr_t addr); // an aligned pointer slot in the image, outside its code sections
bool patchPointer(uintptr_t addr, uintptr_t value, uintptr_t* old);
// The same swap for a slot of another loaded module (DXGI's swap chain vtable, for the overlay):
// aligned, inside that module's image, in a section that is not code.
bool patchModulePointer(uintptr_t addr, uintptr_t value, uintptr_t* old);
// Executable code of another loaded module (not the game, not GridNode): another mod's hook in a slot
// GridNode also wraps (MECMP's tick hook). `module` gets its file name.
bool foreignCode(uintptr_t fn, std::string* module = nullptr);

// MemoryView over this process (for gn::reflect).
class ProcessMemory final : public reflect::MemoryView {
public:
    bool read(uint64_t addr, void* out, size_t size) const override {
        return readFast(static_cast<uintptr_t>(addr), out, size);
    }
};

} // namespace gn::mem
