"""The typed key / value tree of .toc and .sb files.

Each value: one type byte (low 5 bits the type; the top bit set means it has no name), a
zero-terminated name, then the value. Lists and objects carry a 7-bit-encoded byte length and end
with a zero byte. Unknown type codes raise, naming the code and the offset.
"""

from __future__ import annotations

import struct

LIST, OBJECT, BOOL, STRING, INT, LONG, FLOAT, DOUBLE, GUID, SHA1, BYTES = 0x01, 0x02, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0C, 0x0F, 0x10, 0x13
NO_NAME = 0x80


class FormatError(ValueError):
    pass


def read_varint(b: bytes, pos: int) -> tuple[int, int]:
    """A 7-bit encoded length (low groups first, top bit: more follows)."""
    value = shift = 0
    while True:
        if pos >= len(b):
            raise FormatError(f"a length runs past the end at {pos:#x}")
        c = b[pos]
        pos += 1
        value |= (c & 0x7F) << shift
        if not c & 0x80:
            return value, pos
        shift += 7
        if shift > 63:
            raise FormatError(f"a length longer than 64 bits at {pos:#x}")


def _value(b: bytes, pos: int) -> tuple[str | None, object, int]:
    if pos >= len(b):
        raise FormatError(f"a value runs past the end at {pos:#x}")
    at = pos
    t = b[pos]
    pos += 1
    name = None
    if not t & NO_NAME:
        end = b.find(b"\0", pos)
        if end < 0:
            raise FormatError(f"an unterminated name at {pos:#x}")
        name = b[pos:end].decode("utf-8", "replace")
        pos = end + 1
    kind = t & 0x1F
    if kind in (LIST, OBJECT):
        size, pos = read_varint(b, pos)
        end = pos + size
        if end > len(b):
            raise FormatError(f"a {'list' if kind == LIST else 'object'} at {at:#x} runs past the end")
        items: list = []
        obj: dict = {}
        while pos < end and b[pos] != 0:
            n, v, pos = _value(b, pos)
            if kind == LIST:
                items.append(v)
            else:
                obj[n] = v
        return name, (items if kind == LIST else obj), end
    if kind == BOOL:
        return name, b[pos] != 0, pos + 1
    if kind in (STRING, BYTES):
        size, pos = read_varint(b, pos)
        raw = b[pos:pos + size]
        if len(raw) != size:
            raise FormatError(f"a string or byte array at {at:#x} runs past the end")
        return name, (raw.rstrip(b"\0").decode("utf-8", "replace") if kind == STRING else raw), pos + size
    fixed = {INT: ("<i", 4), LONG: ("<q", 8), FLOAT: ("<f", 4), DOUBLE: ("<d", 8)}
    if kind in fixed:
        fmt, n = fixed[kind]
        return name, struct.unpack_from(fmt, b, pos)[0], pos + n
    if kind == GUID:
        return name, b[pos:pos + 16], pos + 16
    if kind == SHA1:
        return name, b[pos:pos + 20], pos + 20
    raise FormatError(f"unknown value type {kind:#04x} (byte {t:#04x}) at {at:#x}")


def parse(b: bytes, pos: int = 0):
    """The value at pos (a whole .toc or one bundle in a .sb)."""
    _, v, _ = _value(b, pos)
    return v


# ---- writing, for tests (and later for the writer of plan step 3) ----------------------------------
def _varint(n: int) -> bytes:
    out = bytearray()
    while True:
        c = n & 0x7F
        n >>= 7
        out.append(c | (0x80 if n else 0))
        if not n:
            return bytes(out)


def encode(value, name: str | None = None) -> bytes:
    """The tree bytes of a Python value: dict (object), list, bool, str, int (LONG), float (DOUBLE),
    bytes of 20 (SHA1) / 16 (GUID) / other (BYTES)."""
    def head(kind: int) -> bytes:
        return bytes([kind | (0 if name is not None else NO_NAME)]) + (name.encode() + b"\0" if name is not None else b"")

    if isinstance(value, dict):
        body = b"".join(encode(v, k) for k, v in value.items()) + b"\0"
        return head(OBJECT) + _varint(len(body)) + body
    if isinstance(value, list):
        body = b"".join(encode(v) for v in value) + b"\0"
        return head(LIST) + _varint(len(body)) + body
    if isinstance(value, bool):
        return head(BOOL) + bytes([1 if value else 0])
    if isinstance(value, str):
        raw = value.encode() + b"\0"
        return head(STRING) + _varint(len(raw)) + raw
    if isinstance(value, int):
        return head(LONG) + struct.pack("<q", value)
    if isinstance(value, float):
        return head(DOUBLE) + struct.pack("<d", value)
    if isinstance(value, (bytes, bytearray)):
        if len(value) == 20:
            return head(SHA1) + bytes(value)
        if len(value) == 16:
            return head(GUID) + bytes(value)
        return head(BYTES) + _varint(len(value)) + bytes(value)
    raise TypeError(f"no tree type for {type(value).__name__}")
