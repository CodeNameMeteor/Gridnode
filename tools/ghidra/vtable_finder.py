#!/usr/bin/env python3
"""
Frostbite VTable Length Finder for Mirror's Edge Catalyst (GridNode).

Finds every vtable start address by scanning constructor / destructor LEA instructions
and vptr stores across code sections, sorts them in memory order, and calculates exact lengths:
    Length = min(next_vtable_start, first non-code pointer)

Usage:
    python tools/ghidra/vtable_finder.py [--exe PATH] [--export JSON_OUT] [--lookup ADDR]
"""

import sys
import os
import argparse
import struct
import re
import json


class PE:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        self.image_base = 0x140000000
        self._parse_sections()

    def _parse_sections(self):
        pe_off = struct.unpack_from('<I', self.data, 0x3c)[0]
        num_sec = struct.unpack_from('<H', self.data, pe_off + 6)[0]
        opt_hdr_size = struct.unpack_from('<H', self.data, pe_off + 20)[0]
        sec_tbl = pe_off + 24 + opt_hdr_size
        self.sections = []
        for i in range(num_sec):
            soff = sec_tbl + i * 40
            name = self.data[soff : soff + 8].rstrip(b'\x00').decode('latin1', errors='ignore')
            vsize, sva, rsize, rptr = struct.unpack_from('<IIII', self.data, soff + 8)
            self.sections.append((name, sva, vsize, rptr, rsize))

    def va_to_offset(self, va):
        rva = va - self.image_base
        for name, sva, vsize, rptr, rsize in self.sections:
            if sva <= rva < sva + max(vsize, rsize):
                return rptr + (rva - sva)
        raise ValueError(f"VA 0x{va:x} not mapped in PE")


def run_finder(exe_path, export_json=None, lookup_addr=None):
    print(f"Loading binary: {exe_path}...")
    pe = PE(exe_path)

    # 1. Determine executable and data ranges
    code_ranges = []
    data_ranges = []
    for name, sva, vsize, rptr, rsize in pe.sections:
        start = pe.image_base + sva
        end = start + max(vsize, rsize)
        if any(k in name.lower() for k in ['srdata', 'sbss', 'link', 'code']):
            # In dump: srdata and sbss are code
            if 'sbss' in name.lower() or 'srdata' in name.lower():
                code_ranges.append((start, end, name))
            else:
                data_ranges.append((start, end, name))
        elif any(k in name.lower() for k in ['rsrc', 'rdata', 'sdata']):
            data_ranges.append((start, end, name))

    def is_code_ptr(val):
        return any(s <= val < e for s, e, _ in code_ranges)

    def in_data_sections(val):
        return any(s <= val < e for s, e, _ in data_ranges)

    print("Scanning code sections for LEA [rip+disp] vtable references...")
    # REX prefix + 0x8d + ModRM [rip+disp32]
    pattern = re.compile(b'[\x48\x49\x4c\x4d]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.DOTALL)

    vtable_starts = set()
    ref_map = {}

    for sname, sva, vsize, rptr, rsize in pe.sections:
        if 'sbss' in sname.lower() or 'srdata' in sname.lower():
            data = pe.data[rptr : rptr + rsize]
            for m in pattern.finditer(data):
                idx = m.start()
                if idx + 7 <= len(data):
                    disp = struct.unpack_from('<i', data, idx + 3)[0]
                    insn_addr = pe.image_base + sva + idx
                    target = insn_addr + 7 + disp

                    if (target & 7) == 0 and in_data_sections(target):
                        try:
                            foff = pe.va_to_offset(target)
                            if foff + 8 <= len(pe.data):
                                first_qword = struct.unpack_from('<Q', pe.data, foff)[0]
                                if is_code_ptr(first_qword):
                                    vtable_starts.add(target)
                                    ref_map.setdefault(target, []).append(insn_addr)
                        except Exception:
                            pass

    sorted_vtables = sorted(list(vtable_starts))
    print(f"Found {len(sorted_vtables)} unique vtable starts.")

    # 2. Determine lengths
    vtable_db = {}
    for i, vt in enumerate(sorted_vtables):
        nxt = sorted_vtables[i + 1] if i + 1 < len(sorted_vtables) else None
        slots = 0
        while True:
            cur = vt + slots * 8
            if nxt and cur >= nxt:
                break
            try:
                foff = pe.va_to_offset(cur)
                val = struct.unpack_from('<Q', pe.data, foff)[0]
                if not is_code_ptr(val):
                    break
                slots += 1
            except Exception:
                break

        vtable_db[vt] = {
            'address': vt,
            'slot_count': slots,
            'size_bytes': slots * 8,
            'next_vtable': nxt,
            'end_address': vt + slots * 8,
            'refs': ref_map.get(vt, [])
        }

    # 3. Handle lookup
    if lookup_addr:
        target_addr = int(lookup_addr, 16) if isinstance(lookup_addr, str) else lookup_addr
        # Check if address is a vtable start or within one
        found = False
        for vt in sorted_vtables:
            info = vtable_db[vt]
            if vt <= target_addr < info['end_address']:
                slot_idx = (target_addr - vt) // 8
                nxt_str = hex(info['next_vtable']) if info['next_vtable'] else 'None'
                print(f"\n[Lookup Result for 0x{target_addr:x}]")
                print(f"  Enclosing VTable: 0x{vt:x}")
                print(f"  Slot Index:       {slot_idx}")
                print(f"  Total Slots:      {info['slot_count']}")
                print(f"  VTable End:       0x{info['end_address']:x}")
                print(f"  Next VTable:      {nxt_str}")
                print(f"  Referenced by:    {[hex(x) for x in info['refs']]}")
                found = True
                break
        if not found:
            print(f"Address 0x{target_addr:x} does not fall within any known vtable.")

    # 4. Handle export
    if export_json:
        print(f"\nExporting vtable database to {export_json}...")
        serializable = []
        for vt in sorted_vtables:
            e = vtable_db[vt]
            serializable.append({
                'address': f"0x{e['address']:x}",
                'slot_count': e['slot_count'],
                'size_bytes': e['size_bytes'],
                'next_vtable': f"0x{e['next_vtable']:x}" if e['next_vtable'] else None,
                'end_address': f"0x{e['end_address']:x}",
                'code_refs': [f"0x{r:x}" for r in e['refs']]
            })
        os.makedirs(os.path.dirname(os.path.abspath(export_json)), exist_ok=True)
        with open(export_json, 'w') as f:
            json.dump(serializable, f, indent=2)
        print("Export finished successfully.")

    return vtable_db


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Frostbite VTable Length Finder")
    parser.add_argument('--exe', required=True, help='the game exe dump (Scylla dump; sections srdata / sbss hold the code)')
    parser.add_argument('--export', help='Output JSON file path')
    parser.add_argument('--lookup', help='Hex address to inspect')
    args = parser.parse_args()

    run_finder(args.exe, args.export, args.lookup)
