"""Read and write binary FBX (7.x) files, keeping every property's original bytes.

An untouched tree writes back byte for byte (checked on Frosty's and Blender's exports); only the
properties a caller replaces are encoded again. Written from the published description of the
binary FBX layout, not from any FBX library.
"""
from __future__ import annotations

import copy
import struct
import zlib
from dataclasses import dataclass, field

_SCALAR = {"Y": "<h", "C": "<?", "I": "<i", "L": "<q", "F": "<f", "D": "<d"}
_ARRAY = {"f": "f", "d": "d", "l": "q", "i": "i", "b": "?"}


@dataclass
class Node:
    name: str
    props: list[bytes]                 # each property's encoded bytes (type code first)
    kids: list["Node"] = field(default_factory=list)
    term: bool = False                 # written with an end-of-children record

    def child(self, name: str) -> "Node | None":
        return next((k for k in self.kids if k.name == name), None)

    def children(self, name: str) -> list["Node"]:
        return [k for k in self.kids if k.name == name]

    def value(self, i: int = 0):
        return decode(self.props[i])

    def clone(self) -> "Node":
        return copy.deepcopy(self)


@dataclass
class Fbx:
    head: bytes
    version: int
    top: list[Node]
    footer: bytes

    def section(self, name: str) -> Node:
        n = next((n for n in self.top if n.name == name), None)
        if n is None:
            raise ValueError(f"no {name} section")
        return n


def decode(p: bytes):
    """A property's value: int, float, bool, str, bytes (raw) or a list (arrays)."""
    t = chr(p[0])
    if t in _SCALAR:
        return struct.unpack_from(_SCALAR[t], p, 1)[0]
    if t == "S":
        return p[5:].decode("utf8", "replace")
    if t == "R":
        return p[5:]
    if t in _ARRAY:
        n, enc, size = struct.unpack_from("<III", p, 1)
        raw = p[13:13 + size]
        if enc:
            raw = zlib.decompress(raw)
        need = n * struct.calcsize(_ARRAY[t])
        if need > len(raw):  # counts come from the file: never trusted past the data there
            raise ValueError(f"array property says {n} items, holds {len(raw)} bytes")
        return list(struct.unpack(f"<{n}{_ARRAY[t]}", raw[:need]))
    raise ValueError(f"property type {t!r}")


def array(t: str, values) -> bytes:
    raw = struct.pack(f"<{len(values)}{_ARRAY[t]}", *values)
    packed = zlib.compress(raw)
    return t.encode() + struct.pack("<III", len(values), 1, len(packed)) + packed


def string(s: str) -> bytes:
    b = s.encode("utf8")
    return b"S" + struct.pack("<I", len(b)) + b


def scalar(t: str, v) -> bytes:
    return t.encode() + struct.pack(_SCALAR[t], v)


def read(path: str) -> Fbx:
    d = open(path, "rb").read()
    if d[:20] != b"Kaydara FBX Binary  ":
        raise ValueError(f"{path}: not a binary FBX (export as binary)")
    version = struct.unpack_from("<I", d, 23)[0]
    wide = version >= 7500
    hs = 25 if wide else 13

    def proplen(o: int) -> int:
        t = chr(d[o])
        if t in _SCALAR:
            return 1 + struct.calcsize(_SCALAR[t])
        if t in "SR":
            return 5 + struct.unpack_from("<I", d, o + 1)[0]
        if t in _ARRAY:
            return 13 + struct.unpack_from("<I", d, o + 9)[0]
        raise ValueError(f"{path}: property type {t!r} at {o}")

    def node(o: int):
        if o + hs > len(d):
            raise ValueError(f"{path}: a node header past the end of the file at {o}")
        end, count, _ = struct.unpack_from("<QQQ" if wide else "<III", d, o)
        nl = d[o + hs - 1]
        p = o + hs
        if end == 0:
            return None, p + nl
        # Offsets and counts come from the file: a node ends after it starts and inside the file.
        if end <= o or end > len(d) or p + nl > end or count > end - p:
            raise ValueError(f"{path}: a broken node at {o} (end {end}, {count} properties)")
        name = d[p:p + nl].decode()
        p += nl
        props = []
        for _ in range(count):
            n = proplen(p)
            if p + n > end:
                raise ValueError(f"{path}: a property past its node's end at {p}")
            props.append(d[p:p + n])
            p += n
        kids, term = [], False
        while p < end:
            k, p2 = node(p)
            if p2 <= p:
                raise ValueError(f"{path}: a node that does not advance at {p}")
            p = p2
            if k is None:
                term = True
                break
            kids.append(k)
        return Node(name, props, kids, term), end

    o, top = 27, []
    while True:
        n, o = node(o)
        if n is None:
            break
        top.append(n)
    return Fbx(d[:27], version, top, d[o:])


def write(fbx: Fbx, path: str) -> None:
    wide = fbx.version >= 7500
    hs = 25 if wide else 13
    out = bytearray(fbx.head)

    def put(n: Node) -> None:
        start = len(out)
        out.extend(b"\0" * hs)
        name = n.name.encode()
        out[start + hs - 1] = len(name)
        out.extend(name)
        p0 = len(out)
        for p in n.props:
            out.extend(p)
        plen = len(out) - p0
        for k in n.kids:
            put(k)
        if n.kids or n.term:
            out.extend(b"\0" * hs)
        struct.pack_into("<QQQ" if wide else "<III", out, start, len(out), len(n.props), plen)

    for n in fbx.top:
        put(n)
    out.extend(b"\0" * hs)
    out.extend(fbx.footer)
    with open(path, "wb") as f:
        f.write(out)
