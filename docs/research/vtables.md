# Vtable lengths (research, 2026-11-06)

Frostbite is built without RTTI, so a vtable has no pointer before it and nothing after it: vtables sit
back to back in `.rdata`, and a run of pointers into code never ends (399 code slots back and 600+
forward from Faith's view-mode slot). The length of a vtable comes only from the **start of the next
one**, and starts come from the constructors and destructors: each writes its vtable with
`lea reg, [rip + disp32]` then `mov [object + offset], reg`. Collect every such `lea` that targets an
8-byte aligned data address whose first qword is a code pointer, sort them, and a vtable runs to the
next start.

Found by another model's finder (Ghidra script and a Python tool, 15,393 vtables in the game); the two
we use were checked again in Ghidra by hand (references from the constructors at 0x14390c274 and
0x14390dc2b):

| Class / interface | Vtable | Slots | Next vtable |
|---|---|---|---|
| `ClientCharacter` primary | 0x141c3c760 | 127 | 0x141c3cb58 |
| `ClientCharacter` +0x1d8 view-mode interface | 0x141c3cbf0 | 27 (getViewMode is slot 15, 0x141c3cc68) | 0x141c3ccc8 (+0x2b0 interface, 7 slots) |
| `FPSCamera` | 0x141b18e58 | 28 (update slot 13 = +0x68, transforms slot 14 = +0x70) | 0x141b18f38 |

GridNode: `hookBody` accepts exactly the 0x141c3cbf0 vtable and copies its 27 slots. `hookCamera` still
counts a run of code pointers (`vtableSlots`), which stopped at 30 for `FPSCamera` only because a double
literal followed the table: its real length is 28.

## The tools

- `tools/ghidra/FrostbiteVTableLengthFinder.java`: run in Ghidra; prints the key classes. Arguments:
  `export FILE.json` (all 15,393 vtables: start, slots, next start, the constructor stores and code
  references) or `label` (a `vftable_<address>` label on each).
- `tools/ghidra/vtable_finder.py --exe DUMP [--lookup 0xADDR] [--export FILE.json]`: the same from the exe
  dump, in about a second; `--lookup` names the vtable that holds an address, its slot and its length.

Both read the dump's code sections (Scylla dump: `srdata` and `sbss`).
