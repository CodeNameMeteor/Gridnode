#include "gridnode/build.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "gridnode/mem.hpp"
#include "gridnode/reflect/layout.hpp"

namespace gn::build {
namespace {

// Facts confirmed on the EA app build by MECMP (fingerprint.cpp / engine.h) and the PC dump.

// Runtime TypeInfo addresses of a few types.
struct TypeCheck { const char* name; uint64_t ti; };
const TypeCheck kTypes[] = {
    {"PamReplayPlayerComponentData", 0x14286b890},
    {"ServerCharacterEntity", 0x1427fe1e0},
    {"ObjectBlueprint", 0x1427e7eb0},
};

// A vtable slot and the function it holds.
struct SlotCheck { const char* what; uint64_t vtable; uint32_t offset; uint64_t fn; };
const SlotCheck kSlots[] = {
    {"PamServerCharacterEntity::applyCustomization", 0x141c3fcb0, 0x340, 0x14336cf00},
    {"PamServerCharacterEntity::getComponent", 0x141c3fcb0, 0x108, 0x1431c6eb0},
    {"PamClientCharacterEntity::setVisible", 0x141c3c760, 0x3d8, 0x143229480},
    {"PamClientCharacterEntity::getComponent", 0x141c3c760, 0x108, 0x1431c6eb0},
    {"PamServerCharacterSpawnEntity::spawn", 0x141c4b388, 0xd0, 0x143390830},
    {"ServerCharacterSpawnEntity::spawn", 0x141b4e0c8, 0xd0, 0x143390830},
    {"ServerGameWorld::postUpdate", 0x141b42d00, 0x28, 0x143341ee0},
};

// The first position-independent bytes of functions (RIP-relative displacements do not change
// when the image moves, absolute addresses would).
struct CodeCheck { const char* what; uint64_t at; uint8_t bytes[16]; size_t n; };
const CodeCheck kCode[] = {
    {"NetState::setDirty", 0x14347e420, {0x48, 0x8B, 0x41, 0x18, 0x48, 0x85, 0xC0, 0x74, 0x18}, 9},
    {"SpatialEntity::setTransform", 0x1432eb1f0, {0x48, 0x83, 0xEC, 0x38, 0x8B, 0x41, 0x18, 0x45, 0x0F, 0xB6, 0xC8}, 11},
    {"localClientCharacter", 0x1439655f0, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x05, 0xB5, 0xC6, 0xA9, 0xFE}, 11},
    {"Arena::allocFromMember", 0x142a647d0,
     {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20}, 15},
};

std::string hex(const uint8_t* b, size_t n) {
    std::string s;
    char t[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(t, sizeof(t), "%02X ", b[i]);
        s += t;
    }
    return s;
}

void line(Result& r, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void line(Result& r, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    r.details += buf;
    r.details += '\n';
}

// Runtime TypeInfo* of `name` on the engine's list (0 if absent).
uint64_t findType(const char* name) {
    mem::ProcessMemory m;
    uint64_t ti = m.readOr<uint64_t>(mem::rva(layout::kTypeInfoListHead));
    for (size_t n = 0; ti >= 0x10000 && n < 200000; ++n) {
        const uint64_t data = m.readOr<uint64_t>(ti + layout::TI_Data);
        if (data && m.readCString(m.readOr<uint64_t>(data + layout::TID_Name)) == name) return ti;
        ti = m.readOr<uint64_t>(ti + layout::TI_Next);
    }
    return 0;
}

} // namespace

size_t waitForTypeList(uint32_t timeoutMs, const std::atomic<bool>* stop) {
    mem::ProcessMemory m;
    const DWORD start = GetTickCount();
    size_t last = 0;
    int stable = 0;
    while (!(stop && stop->load()) && GetTickCount() - start < timeoutMs) {
        const size_t n = reflect::countTypes(m, mem::rva(layout::kTypeInfoListHead));
        stable = (n >= kMinTypes && n == last) ? stable + 1 : 0;
        if (stable >= 4) return n;
        last = n;
        Sleep(250);
    }
    return 0;
}

Result check() {
    Result r;
    const IMAGE_NT_HEADERS* nt = mem::ntHeaders();
    if (nt->FileHeader.TimeDateStamp != kTimestamp) {
        ++r.warnings;
        line(r, "warning: PE timestamp is 0x%08lx, expected 0x%08x", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
             kTimestamp);
    }
    if (nt->OptionalHeader.SizeOfImage != kImageSize) {
        ++r.warnings;
        line(r, "warning: image size is 0x%lx, expected 0x%x", static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage),
             kImageSize);
    }
    for (const TypeCheck& t : kTypes) {
        const uint64_t got = findType(t.name);
        if (got != mem::rva(t.ti)) {
            ++r.failed;
            line(r, "type %s is at 0x%llx, expected 0x%llx", t.name, static_cast<unsigned long long>(got ? mem::va(got) : 0),
                 static_cast<unsigned long long>(t.ti));
        }
    }
    for (const SlotCheck& s : kSlots) {
        uintptr_t got = 0;
        if (!mem::read(mem::rva(s.vtable) + s.offset, got) || got != mem::rva(s.fn)) {
            ++r.failed;
            line(r, "%s holds 0x%llx, expected 0x%llx", s.what, static_cast<unsigned long long>(got ? mem::va(got) : 0),
                 static_cast<unsigned long long>(s.fn));
        }
    }
    for (const CodeCheck& c : kCode) {
        uint8_t got[16] = {};
        if (!mem::read(mem::rva(c.at), got, c.n) || memcmp(got, c.bytes, c.n) != 0) {
            ++r.failed;
            line(r, "%s starts with %s, expected %s", c.what, hex(got, c.n).c_str(), hex(c.bytes, c.n).c_str());
        }
    }
    r.ok = r.failed == 0;
    return r;
}

} // namespace gn::build
