#include "gridnode/build.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "gridnode/engine.hpp"
#include "gridnode/mem.hpp"
#include "gridnode/reflect/layout.hpp"

namespace gn::build {
namespace {

// The checks themselves come from db/annotations/fingerprint.yaml (generated gridnode/engine.hpp).
namespace fp = gn::engine::fingerprint;

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
    for (const fp::Type& t : fp::types) {
        const uint64_t got = findType(t.name);
        if (got != mem::rva(t.ti)) {
            ++r.failed;
            line(r, "type %s is at 0x%llx, expected 0x%llx", t.name, static_cast<unsigned long long>(got ? mem::va(got) : 0),
                 static_cast<unsigned long long>(t.ti));
        }
    }
    for (const fp::Slot& s : fp::slots) {
        uintptr_t got = 0;
        if (!mem::read(mem::rva(s.slot), got) || got != mem::rva(s.fn)) {
            ++r.failed;
            line(r, "%s holds 0x%llx, expected 0x%llx", s.what, static_cast<unsigned long long>(got ? mem::va(got) : 0),
                 static_cast<unsigned long long>(s.fn));
        }
    }
    for (const fp::Code& c : fp::code) {
        uint8_t got[32] = {};
        if (!mem::read(mem::rva(c.at) + c.skip, got, c.count) || memcmp(got, c.bytes, c.count) != 0) {
            if (c.required) ++r.failed;
            else ++r.warnings;
            line(r, "%s%s starts with %s, expected %s", c.required ? "" : "warning (optional): ", c.what,
                 hex(got, c.count).c_str(), hex(c.bytes, c.count).c_str());
        }
    }
    r.ok = r.failed == 0;
    return r;
}

bool codeMatches(const char* what) {
    for (const fp::Code& c : fp::code) {
        if (strcmp(c.what, what) != 0) continue;
        uint8_t got[32] = {};
        return mem::read(mem::rva(c.at) + c.skip, got, c.count) && memcmp(got, c.bytes, c.count) == 0;
    }
    return false; // no such fingerprint: treat as not checked
}

} // namespace gn::build
