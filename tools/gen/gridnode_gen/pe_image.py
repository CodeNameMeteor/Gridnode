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
            self.sections.append((self.base + rva, max(vsize, raw_size), raw_ptr, raw_size, bool(ch & 0x20000000), name))

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
