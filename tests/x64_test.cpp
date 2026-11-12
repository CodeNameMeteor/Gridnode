// Host-side tests for the x86-64 decoder and trampolines behind function detours.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gridnode/x64.hpp"

using namespace gn::x64;

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

namespace {

struct Case {
    std::vector<uint8_t> bytes;
    size_t length;
    int ripDisp;
};

void lengths() {
    const Case cases[] = {
        {{0x48, 0x89, 0x5c, 0x24, 0x08}, 5, -1},                         // mov [rsp+8], rbx
        {{0x57}, 1, -1},                                                 // push rdi
        {{0x40, 0x53}, 2, -1},                                           // push rbx (REX)
        {{0x48, 0x83, 0xec, 0x20}, 4, -1},                               // sub rsp, 0x20
        {{0x48, 0x81, 0xec, 0x10, 0x06, 0, 0}, 7, -1},                   // sub rsp, 0x610
        {{0x48, 0x8b, 0x05, 1, 2, 3, 4}, 7, 3},                          // mov rax, [rip+x]
        {{0x48, 0x8d, 0x0d, 1, 2, 3, 4}, 7, 3},                          // lea rcx, [rip+x]
        {{0xf3, 0x0f, 0x10, 0x05, 1, 2, 3, 4}, 8, 4},                    // movss xmm0, [rip+x]
        {{0x0f, 0x29, 0x74, 0x24, 0x30}, 5, -1},                         // movaps [rsp+0x30], xmm6
        {{0xc5, 0xf8, 0x29, 0x74, 0x24, 0x30}, 6, -1},                   // vmovaps [rsp+0x30], xmm6
        {{0xc4, 0xe3, 0x79, 0x04, 0xc0, 0x01}, 6, -1},                   // vpermilps xmm0, xmm0, 1
        {{0x48, 0xb8, 1, 2, 3, 4, 5, 6, 7, 8}, 10, -1},                  // mov rax, imm64
        {{0x41, 0xb8, 1, 2, 3, 4}, 6, -1},                               // mov r8d, imm32
        {{0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, 6, -1},                   // nop word [rax+rax]
        {{0xc7, 0x44, 0x24, 0x30, 1, 2, 3, 4}, 8, -1},                   // mov dword [rsp+0x30], imm32
        {{0xf6, 0x05, 1, 2, 3, 4, 0x01}, 7, 2},                          // test byte [rip+x], 1
        {{0x66, 0xc7, 0x05, 1, 2, 3, 4, 5, 6}, 9, 3},                    // mov word [rip+x], imm16
        {{0x48, 0x8b, 0x84, 0x24, 0x00, 0x01, 0, 0}, 8, -1},             // mov rax, [rsp+0x100]
        {{0x8b, 0x04, 0x25, 1, 2, 3, 4}, 7, -1},                         // mov eax, [abs32] (SIB, no base)
        {{0x0f, 0xb6, 0x41, 0x10}, 4, -1},                               // movzx eax, byte [rcx+0x10]
        {{0x66, 0x0f, 0x3a, 0x0f, 0xc1, 0x08}, 6, -1},                   // palignr xmm0, xmm1, 8
        {{0x66, 0x48, 0xc7, 0xc0, 1, 2, 3, 4}, 8, -1},                   // mov rax, imm32: REX.W wins over 0x66
        {{0x66, 0x48, 0x05, 1, 2, 3, 4}, 7, -1},                         // add rax, imm32 (0x66 ignored)
        {{0x66, 0x05, 1, 2}, 4, -1},                                     // add ax, imm16
        {{0xc3}, 1, -1},                                                 // ret
    };
    for (const Case& c : cases) {
        Insn in;
        const bool ok = decode(c.bytes.data(), c.bytes.size(), in);
        CHECK(ok);
        CHECK(in.length == c.length);
        CHECK(in.ripDisp == c.ripDisp);
        if (!ok || in.length != c.length || in.ripDisp != c.ripDisp)
            std::fprintf(stderr, "  bytes %02x %02x %02x: length %zu (want %zu), rip %d (want %d)\n", c.bytes[0],
                         c.bytes.size() > 1 ? c.bytes[1] : 0, c.bytes.size() > 2 ? c.bytes[2] : 0, in.length, c.length, in.ripDisp, c.ripDisp);
    }
    Insn in;
    const uint8_t jz[] = {0x74, 0x05};
    CHECK(decode(jz, 2, in) && in.jcc && in.rel == 1 && in.relSize == 1);
    const uint8_t call[] = {0xe8, 1, 2, 3, 4};
    CHECK(decode(call, 5, in) && in.call && in.rel == 1 && in.relSize == 4);
    const uint8_t jmpm[] = {0xff, 0x25, 0, 0, 0, 0};
    CHECK(decode(jmpm, 6, in) && in.jmp && in.ripDisp == 2);
    const uint8_t invalid[] = {0x06};
    CHECK(!decode(invalid, 1, in));
    const uint8_t moffs32[] = {0x67, 0xa1, 1, 2, 3, 4}; // mov eax, [abs32] with an address-size prefix
    CHECK(decode(moffs32, sizeof(moffs32), in) && in.length == 6 && in.addr32);
    const uint8_t moffs64[] = {0x48, 0xa1, 1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(decode(moffs64, sizeof(moffs64), in) && in.length == 10 && !in.addr32);
}

int32_t rel32(const std::vector<uint8_t>& v, size_t at) {
    int32_t d;
    memcpy(&d, v.data() + at, 4);
    return d;
}

void trampolines() {
    const uint64_t code = 0x140001000, tramp = 0x150000000;
    // mov rax, [rip+0xff9] (-> 0x140002000); call 0x140002000; ...
    const uint8_t f1[] = {0x48, 0x8b, 0x05, 0xf9, 0x0f, 0, 0, 0xe8, 0xf4, 0x0f, 0, 0, 0x90, 0x90};
    std::vector<uint8_t> out;
    size_t copied = 0;
    CHECK(trampoline(f1, sizeof(f1), code, 5, tramp, out, copied));
    CHECK(copied == 7);
    CHECK(out.size() == 7 + 14);
    CHECK(tramp + 7 + rel32(out, 3) == 0x140002000);
    uint64_t back = 0;
    memcpy(&back, out.data() + 7 + 6, 8);
    CHECK(back == code + 7);

    CHECK(trampoline(f1, sizeof(f1), code, 8, tramp, out, copied));
    CHECK(copied == 12);
    CHECK(out[7] == 0xe8);
    CHECK(tramp + 12 + rel32(out, 8) == 0x140002000);

    std::vector<std::pair<size_t, size_t>> starts;
    CHECK(trampoline(f1, sizeof(f1), code, 8, tramp, out, copied, nullptr, &starts));
    CHECK((starts == std::vector<std::pair<size_t, size_t>>{{0, 0}, {7, 7}}));

    // jz +0x20 (rel8) becomes 0F 84 rel32 to the same place.
    const uint8_t f2[] = {0x74, 0x20, 0x48, 0x83, 0xec, 0x20, 0x90};
    CHECK(trampoline(f2, sizeof(f2), code, 5, tramp, out, copied));
    CHECK(copied == 6);
    CHECK(out[0] == 0x0f && out[1] == 0x84);
    CHECK(tramp + 6 + rel32(out, 2) == code + 2 + 0x20);
    CHECK(trampoline(f2, sizeof(f2), code, 5, tramp, out, copied, nullptr, &starts));
    CHECK((starts == std::vector<std::pair<size_t, size_t>>{{0, 0}, {2, 6}})); // the jcc grew by 4

    // Too short: ret in the first bytes.
    const uint8_t f3[] = {0x31, 0xc0, 0xc3, 0xcc, 0xcc, 0xcc};
    CHECK(!trampoline(f3, sizeof(f3), code, 5, tramp, out, copied));
    // A branch back into the moved bytes.
    const uint8_t f4[] = {0x90, 0x90, 0x90, 0x75, 0xfc, 0x90};
    CHECK(!trampoline(f4, sizeof(f4), code, 5, tramp, out, copied));
    // An address-size prefix in the moved bytes.
    const uint8_t f5[] = {0x67, 0x8b, 0x05, 1, 2, 3, 4, 0x90};
    CHECK(!trampoline(f5, sizeof(f5), code, 5, tramp, out, copied));
    // A RIP-relative operand out of reach of the trampoline.
    CHECK(!trampoline(f1, sizeof(f1), code, 5, 0x7ff000000000ull, out, copied));
}

} // namespace

int main() {
    lengths();
    trampolines();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("x64: all checks passed");
    return 0;
}
