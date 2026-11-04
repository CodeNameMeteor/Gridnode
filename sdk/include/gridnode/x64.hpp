#pragma once
// x86-64 instruction lengths and trampolines for function detours (gn_api hook_function): enough
// of the instruction set for function prologues, and every instruction that refers to its own
// address (RIP-relative operands, relative jumps and calls) moved correctly. Portable: tested on
// the host (tests/x64_test.cpp).

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gn::x64 {

struct Insn {
    size_t length = 0;
    int ripDisp = -1;   // offset of a RIP-relative disp32 in the instruction, or -1
    int rel = -1;       // offset of a relative branch's displacement, or -1
    int relSize = 0;    // 1 or 4
    bool ret = false;   // ret / ret imm16
    bool jmp = false;   // an unconditional jmp (rel or indirect): what follows may not be this function
    bool call = false;  // call rel32
    bool jcc = false;   // a conditional branch (rel8 or rel32)
    bool loop = false;  // loop / jrcxz (rel8 only: cannot be moved)
    bool addr32 = false; // a 0x67 address-size prefix: 32-bit addresses (moffs 4 bytes, EIP-relative)
};

// The instruction at p (at most `avail` bytes readable). False: not decoded (an opcode this
// decoder does not know, or invalid in 64-bit mode).
bool decode(const uint8_t* p, size_t avail, Insn& out);

// Copies whole instructions from the start of `code` (at runtime address codeAddr) until at least
// `minBytes` are covered, moved to run at trampAddr, then a jump back to codeAddr + copied:
// "jmp [rip+0]" and the address. Short branches become long ones; RIP-relative operands and
// relative branches are re-aimed (refused when the new displacement does not fit in 32 bits).
// Refused: an instruction not decoded, loop / jrcxz, a branch back into the copied bytes, or the
// function ending (ret, jmp) before minBytes. Not detected: a branch from elsewhere in the function
// (a loop back to its start) into the moved bytes; such a function cannot be detoured safely, and
// only reading it in Ghidra shows that.
// `starts` (may be null) receives each moved instruction's offset in the function and in the
// trampoline: a thread stopped inside the moved bytes is moved to the same instruction there.
bool trampoline(const uint8_t* code, size_t avail, uint64_t codeAddr, size_t minBytes, uint64_t trampAddr,
                std::vector<uint8_t>& out, size_t& copied, std::string* why = nullptr,
                std::vector<std::pair<size_t, size_t>>* starts = nullptr);

} // namespace gn::x64
