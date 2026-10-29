"""Reads a PE file (an exe, or a memory dump of the running game saved as a PE) by image VA."""

from __future__ import annotations

import struct


class PeImage:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError(f"{path}: not a PE file")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: not a PE file")
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        self.timestamp = struct.unpack_from("<I", d, pe + 8)[0]
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x20B:
            raise ValueError(f"{path}: not a 64-bit image")
        self.base = struct.unpack_from("<Q", d, opt + 24)[0]
        self.size = struct.unpack_from("<I", d, opt + 56)[0]
        self.sections = []  # (va, vsize, raw_ptr, raw_size, executable, name)
        for i in range(nsec):
            h = opt + opt_size + i * 40
            name = d[h:h + 8].rstrip(b"\0").decode("latin-1")
            vsize, rva, raw_size, raw_ptr = struct.unpack_from("<IIII", d, h + 8)
            ch = struct.unpack_from("<I", d, h + 36)[0]
            if raw_size and raw_ptr + raw_size > len(d):  # zeros past the raw data are real (vsize > raw size); a short file is not
                raise ValueError(f"{path}: truncated: section {name} needs bytes up to {raw_ptr + raw_size:#x}, the file has {len(d):#x}")
            self.sections.append((self.base + rva, max(vsize, raw_size), raw_ptr, raw_size, bool(ch & 0x20000000), name))
        # Exception directory (data directory 3): the unwind table, one RUNTIME_FUNCTION per function part.
        ndirs = struct.unpack_from("<I", d, opt + 108)[0]
        self.pdata = (0, 0)
        if ndirs > 3:
            rva, size = struct.unpack_from("<II", d, opt + 112 + 3 * 8)
            self.pdata = (self.base + rva, size) if rva and size else (0, 0)
        self._pdata_begins = None

    def read(self, va: int, n: int) -> bytes | None:
        for sva, size, raw_ptr, raw_size, _, _ in self.sections:
            if sva <= va and va + n <= sva + size:
                off = va - sva
                chunk = self.data[raw_ptr + off: raw_ptr + min(off + n, raw_size)]
                return chunk + b"\0" * (n - len(chunk))
        return None

    def q(self, va: int) -> int | None:
        b = self.read(va, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def is_code(self, va: int) -> bool:
        return any(s[0] <= va < s[0] + s[1] and s[4] for s in self.sections)

    def _load_pdata(self) -> None:
        va, size = self.pdata
        raw = self.read(va, size) if size else None
        n = len(raw) // 12 if raw else 0
        entries = sorted(struct.unpack_from("<III", raw, i * 12) for i in range(n))
        self._pdata = [e for e in entries if e[0] and e[1] > e[0]]
        self._pdata_begins = [e[0] for e in self._pdata]

    def function_start(self, va: int) -> int | None:
        """The start of the function that contains `va`, from the unwind table: the entry covering it,
        then its chain (a function split into parts has one entry per part, each pointing back to the
        one before through UNW_FLAG_CHAININFO). None if no entry covers `va` (a leaf function, or not code)."""
        import bisect
        if self._pdata_begins is None:
            self._load_pdata()
        rva = va - self.base
        i = bisect.bisect_right(self._pdata_begins, rva) - 1
        if i < 0 or not (self._pdata[i][0] <= rva < self._pdata[i][1]):
            return None
        begin, _, unwind = self._pdata[i]
        for _ in range(32):
            info = self.read(self.base + unwind, 4)
            if not info or not (info[0] >> 3) & 0x4:   # no chain: this part is the function's start
                break
            count = info[2]
            chained = self.read(self.base + unwind + 4 + 2 * ((count + 1) & ~1), 12)
            if not chained:
                break
            begin, _, unwind = struct.unpack("<III", chained)
        return self.base + begin
