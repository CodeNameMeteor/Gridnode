// x86-64 decoding for detours: lengths, and what in an instruction depends on where it is.
// The tables cover the general-purpose, x87, SSE and AVX (VEX) encodings; EVEX (AVX-512) and 3DNow!
// are refused (not in the game's prologues).

#include "gridnode/x64.hpp"

#include <cstdio>
#include <cstring>

namespace gn::x64 {
namespace {

// One-byte opcodes with a ModRM byte.
bool modrm1(uint8_t op) {
    if (op < 0x40) return (op & 7) < 4;
    switch (op) {
    case 0x63: case 0x69: case 0x6b: case 0x80: case 0x81: case 0x83:
    case 0xc0: case 0xc1: case 0xc6: case 0xc7: case 0xf6: case 0xf7: case 0xfe: case 0xff:
        return true;
    default:
        break;
    }
    return (op >= 0x84 && op <= 0x8f) || (op >= 0xd0 && op <= 0xd3) || (op >= 0xd8 && op <= 0xdf);
}

// Two-byte (0F xx) opcodes without a ModRM byte.
bool noModrm2(uint8_t op) {
    switch (op) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0b: case 0x0e:
    case 0x77: case 0xa0: case 0xa1: case 0xa2: case 0xa8: case 0xa9: case 0xaa:
        return true;
    default:
        break;
    }
    return (op >= 0x30 && op <= 0x37) || (op >= 0x80 && op <= 0x8f) || (op >= 0xc8 && op <= 0xcf);
}

bool imm8Map1(uint8_t op) { // 0F xx with an imm8 after the ModRM
    return (op >= 0x70 && op <= 0x73) || op == 0xa4 || op == 0xac || op == 0xba || op == 0xc2 || (op >= 0xc4 && op <= 0xc6);
}

// Bytes taken by ModRM, SIB and displacement at p; sets ripDisp (relative to p) for RIP-relative.
bool modrmLen(const uint8_t* p, size_t avail, size_t& n, int& ripAt) {
    if (avail < 1) return false;
    const uint8_t m = p[0], mod = m >> 6, rm = m & 7;
    n = 1;
    if (mod == 3) return true;
    if (rm == 4) { // SIB
        if (avail < 2) return false;
        const uint8_t base = p[1] & 7;
        n = 2;
        if (mod == 0 && base == 5) n += 4;
    } else if (mod == 0 && rm == 5) {
        ripAt = 1;
        n += 4;
    }
    if (mod == 1) n += 1;
    if (mod == 2) n += 4;
    return n <= avail;
}

} // namespace

bool decode(const uint8_t* p, size_t avail, Insn& out) {
    out = Insn{};
    size_t i = 0;
    bool opsize = false, rexW = false, addr32 = false;
    // Legacy prefixes, then REX.
    for (; i < avail && i < 14; ++i) {
        const uint8_t b = p[i];
        if (b == 0x66) opsize = true;
        else if (b == 0x67) addr32 = true;
        else if (b == 0xf0 || b == 0xf2 || b == 0xf3 || b == 0x2e || b == 0x36 || b == 0x3e || b == 0x26 || b == 0x64 || b == 0x65) continue;
        else break;
    }
    if (i < avail && (p[i] & 0xf0) == 0x40) rexW = (p[i++] & 8) != 0;
    if (i >= avail) return false;
    const uint8_t op = p[i++];
    const size_t immZ = opsize ? 2 : 4;
    size_t imm = 0;
    bool modrm = false;
    int ripAt = -1;

    if (op == 0xc4 || op == 0xc5) { // VEX
        size_t vlen = op == 0xc5 ? 1 : 2;
        if (i + vlen >= avail) return false;
        int map = 1;
        if (op == 0xc4) map = p[i] & 0x1f;
        i += vlen;
        const uint8_t vop = p[i++];
        if (map < 1 || map > 3) return false;
        modrm = !(map == 1 && vop == 0x77); // vzeroupper / vzeroall
        if (map == 3 || (map == 1 && imm8Map1(vop))) imm = 1;
    } else if (op == 0x0f) {
        if (i >= avail) return false;
        const uint8_t op2 = p[i++];
        if (op2 == 0x0f) return false; // 3DNow!
        if (op2 == 0x38 || op2 == 0x3a) {
            if (i >= avail) return false;
            ++i;
            modrm = true;
            if (op2 == 0x3a) imm = 1;
        } else if (op2 >= 0x80 && op2 <= 0x8f) {
            out.jcc = true, out.rel = int(i), out.relSize = 4;
            imm = 4;
        } else {
            modrm = !noModrm2(op2);
            if (imm8Map1(op2)) imm = 1;
        }
    } else {
        switch (op) {
        case 0x06: case 0x07: case 0x0e: case 0x16: case 0x17: case 0x1e: case 0x1f: case 0x27: case 0x2f: case 0x37: case 0x3f:
        case 0x60: case 0x61: case 0x62: case 0x82: case 0x9a: case 0xce: case 0xd4: case 0xd5: case 0xd6: case 0xea:
            return false; // invalid in 64-bit mode
        default: break;
        }
        modrm = modrm1(op);
        if (op < 0x40 && (op & 7) == 4) imm = 1;
        else if (op < 0x40 && (op & 7) == 5) imm = immZ;
        else if (op == 0x68 || op == 0x69 || op == 0x81 || op == 0xa9 || op == 0xc7) imm = immZ;
        else if (op == 0x6a || op == 0x6b || op == 0x80 || op == 0x83 || op == 0xa8 || op == 0xc0 || op == 0xc1 || op == 0xc6 ||
                 op == 0xcd || (op >= 0xb0 && op <= 0xb7) || (op >= 0xe4 && op <= 0xe7))
            imm = 1;
        else if (op >= 0xb8 && op <= 0xbf) imm = rexW ? 8 : immZ;
        else if (op >= 0xa0 && op <= 0xa3) imm = addr32 ? 4 : 8; // moffs (32-bit with 0x67)
        else if (op == 0xc2 || op == 0xca) imm = 2;
        else if (op == 0xc8) imm = 3;
        else if (op >= 0x70 && op <= 0x7f) out.jcc = true, out.rel = int(i), out.relSize = 1, imm = 1;
        else if (op == 0xeb) out.jmp = true, out.rel = int(i), out.relSize = 1, imm = 1;
        else if (op == 0xe9) out.jmp = true, out.rel = int(i), out.relSize = 4, imm = 4;
        else if (op == 0xe8) out.call = true, out.rel = int(i), out.relSize = 4, imm = 4;
        else if (op >= 0xe0 && op <= 0xe3) out.loop = true, out.rel = int(i), out.relSize = 1, imm = 1;
        if (op == 0xc3 || op == 0xc2 || op == 0xcb || op == 0xca) out.ret = true;
    }
    if (modrm) {
        if (i >= avail) return false;
        const uint8_t reg = (p[i] >> 3) & 7;
        if (op == 0xf6 && reg < 2) imm = 1;
        if (op == 0xf7 && reg < 2) imm = immZ;
        if (op == 0xff && (reg == 4 || reg == 5)) out.jmp = true; // jmp [x]
        size_t n = 0;
        if (!modrmLen(p + i, avail - i, n, ripAt)) return false;
        if (ripAt >= 0) out.ripDisp = int(i) + ripAt;
        i += n;
    }
    i += imm;
    if (i > avail || i > 15) return false;
    out.length = i;
    out.addr32 = addr32;
    return true;
}

bool trampoline(const uint8_t* code, size_t avail, uint64_t codeAddr, size_t minBytes, uint64_t trampAddr,
                std::vector<uint8_t>& out, size_t& copied, std::string* why, std::vector<std::pair<size_t, size_t>>* starts) {
    auto fail = [&](const std::string& m) {
        if (why) *why = m;
        out.clear();
        return false;
    };
    out.clear();
    copied = 0;
    if (starts) starts->clear();
    std::vector<uint64_t> targets; // of the branches moved
    while (copied < minBytes) {
        Insn in;
        if (!decode(code + copied, avail - copied, in)) {
            char b[96];
            snprintf(b, sizeof(b), "an instruction at +%zu could not be decoded (bytes %02x %02x %02x)", copied, code[copied],
                     copied + 1 < avail ? code[copied + 1] : 0, copied + 2 < avail ? code[copied + 2] : 0);
            return fail(b);
        }
        const uint8_t* src = code + copied;
        const uint64_t here = codeAddr + copied;
        if (starts) starts->push_back({copied, out.size()});
        if (in.loop) return fail("a loop / jrcxz in the first bytes");
        // 32-bit addressing: an EIP-relative operand or a short absolute address cannot be re-aimed
        // as the 64-bit ones are; never in the game's prologues, so refused (review, 2026-11-03).
        if (in.addr32) return fail("an instruction with an address-size prefix (0x67) in the first bytes");
        if (in.ret && copied + in.length < minBytes) return fail("the function returns within its first bytes (too short)");
        if (in.rel >= 0) {
            int64_t disp = 0;
            if (in.relSize == 1) disp = static_cast<int8_t>(src[in.rel]);
            else {
                int32_t d32;
                memcpy(&d32, src + in.rel, 4);
                disp = d32;
            }
            const uint64_t target = here + in.length + static_cast<uint64_t>(disp);
            targets.push_back(target);
            // The branch, re-encoded with a rel32 from where it will be.
            std::vector<uint8_t> enc;
            if (in.call) enc = {0xe8};
            else if (in.jmp) enc = {0xe9};
            else { // jcc: 7x rel8 or 0F 8x rel32 (prefixes before it are kept as they were)
                const uint8_t cc = src[in.rel - 1] & 0x0f; // 7x rel8 or 0F 8x rel32
                enc = {0x0f, uint8_t(0x80 | cc)};
            }
            const uint64_t at = trampAddr + out.size() + enc.size() + 4;
            const int64_t nd = static_cast<int64_t>(target - at);
            if (nd < INT32_MIN || nd > INT32_MAX) return fail("a branch target too far from the trampoline");
            const int32_t d = static_cast<int32_t>(nd);
            out.insert(out.end(), enc.begin(), enc.end());
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(&d), reinterpret_cast<const uint8_t*>(&d) + 4);
        } else {
            const size_t at = out.size();
            out.insert(out.end(), src, src + in.length);
            if (in.ripDisp >= 0) {
                int32_t d32;
                memcpy(&d32, src + in.ripDisp, 4);
                const uint64_t target = here + in.length + static_cast<uint64_t>(static_cast<int64_t>(d32));
                const int64_t nd = static_cast<int64_t>(target - (trampAddr + at + in.length));
                if (nd < INT32_MIN || nd > INT32_MAX) return fail("a RIP-relative operand too far from the trampoline");
                const int32_t d = static_cast<int32_t>(nd);
                memcpy(out.data() + at + in.ripDisp, &d, 4);
            }
        }
        copied += in.length;
        if (in.jmp && copied < minBytes) return fail("the function jumps away within its first bytes (too short)");
    }
    for (uint64_t t : targets) // into the bytes that will hold the jump to the detour
        if (t > codeAddr && t < codeAddr + copied) return fail("a branch into the bytes being moved");
    // Back to the rest of the function.
    const uint8_t back[6] = {0xff, 0x25, 0, 0, 0, 0}; // jmp [rip+0]
    out.insert(out.end(), back, back + 6);
    const uint64_t cont = codeAddr + copied;
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&cont), reinterpret_cast<const uint8_t*>(&cont) + 8);
    return true;
}

} // namespace gn::x64
