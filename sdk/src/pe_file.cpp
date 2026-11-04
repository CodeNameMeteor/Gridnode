// A DLL's export names read from the file (gridnode/pe_file.hpp). LoadLibraryEx with
// DONT_RESOLVE_DLL_REFERENCES did this before; Microsoft says not to use that flag (it maps the DLL
// as a module without initialising it, which later loads of it can pick up).

#include "gridnode/pe_file.hpp"

#include <cstring>

namespace gn::pe {
namespace {

template <class T> bool at(const uint8_t* file, size_t size, size_t off, T& out) {
    if (off > size || size - off < sizeof(T)) return false;
    memcpy(&out, file + off, sizeof(T));
    return true;
}

} // namespace

bool exportsName(const uint8_t* file, size_t size, const char* name) {
    if (!file || !name) return false;
    uint16_t mz = 0;
    uint32_t peOff = 0, sig = 0;
    if (!at(file, size, 0, mz) || mz != 0x5a4d || !at(file, size, 0x3c, peOff) || !at(file, size, peOff, sig) || sig != 0x00004550) return false;
    const size_t coff = size_t(peOff) + 4;
    uint16_t sections = 0, optSize = 0, magic = 0;
    if (!at(file, size, coff + 2, sections) || !at(file, size, coff + 16, optSize)) return false;
    const size_t opt = coff + 20;
    if (!at(file, size, opt, magic) || magic != 0x20b) return false; // PE32+ only (the game is x64)
    uint32_t dirCount = 0, exportRva = 0, exportSize = 0;
    if (!at(file, size, opt + 108, dirCount) || dirCount < 1 || !at(file, size, opt + 112, exportRva) || !at(file, size, opt + 116, exportSize))
        return false;
    if (!exportRva || !exportSize) return false;
    const size_t secTable = opt + optSize;
    // An RVA to a file offset, through the section it is in.
    auto offsetOf = [&](uint32_t rva, size_t& out) {
        for (uint16_t i = 0; i < sections; ++i) {
            const size_t s = secTable + size_t(i) * 40;
            uint32_t vsize = 0, va = 0, rawSize = 0, raw = 0;
            if (!at(file, size, s + 8, vsize) || !at(file, size, s + 12, va) || !at(file, size, s + 16, rawSize) || !at(file, size, s + 20, raw))
                return false;
            const uint32_t span = vsize > rawSize ? vsize : rawSize;
            if (rva >= va && rva - va < span) {
                if (rva - va >= rawSize) return false; // not in the file (zero-filled)
                out = size_t(raw) + (rva - va);
                return out < size;
            }
        }
        return false;
    };
    size_t dir = 0;
    if (!offsetOf(exportRva, dir)) return false;
    uint32_t count = 0, namesRva = 0;
    if (!at(file, size, dir + 24, count) || !at(file, size, dir + 32, namesRva)) return false;
    size_t names = 0;
    if (count && !offsetOf(namesRva, names)) return false;
    const size_t want = strlen(name);
    for (uint32_t i = 0; i < count && i < 65536; ++i) {
        uint32_t nameRva = 0;
        size_t n = 0;
        if (!at(file, size, names + size_t(i) * 4, nameRva) || !offsetOf(nameRva, n)) continue;
        if (size - n > want && memcmp(file + n, name, want) == 0 && file[n + want] == 0) return true;
    }
    return false;
}

} // namespace gn::pe
