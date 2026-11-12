""".fbmod files: mods for Frosty's Mod Manager, read and written.

Written from the user's own mods (19 of them, 2026-11-12), not from Frosty's code. Two families:

FBMODV1 / FBMODV2 (old): a typed key/value tree (the same format as the game's .toc and .sb, tree.py)
with the mod's details, `resources` (name, type, SHA-1s, sizes, where the data is in `<mod>_NN.archive`)
and `actions` ("modify" a resource in a bundle). Read only.

FROSTY (versions 1, 3 and 5; current Frosty writes 5), little-endian:

    header      "FROSTY\\0\\x01" (8 bytes), u32 version, u64 offset of the data table, i32 data count,
                the profile name (7-bit length, then the bytes), u32 game version, then zero-terminated:
                title, author, category, version, description (and from version 5 a link)
    resources   i32 count, then per resource: u8 type (0 embedded: icon and screenshots, 1 ebx, 2 res,
                3 chunk), i32 index in the data table (-1: none); then
                  the name (always from version 5, else only with data),
                  with data: SHA-1 (20 bytes) of the stored data, u64 size of the asset, u8 flags,
                    i32 handler hash, and from version 3 a zero-terminated user string,
                  the bundles it is added to (i32 count, then each name's djb2-xor hash, the same hash
                    as EBX names; always from version 5, else only with data),
                  res: u32 res type, u64 res id, i32 size and the res meta bytes,
                  chunk: four u32 (range start, range end, logical offset, logical size), i32 h32,
                    i32 first mip,
                  before version 5, with data: one more i32 count and that many u32 (seen: always 0)
    data table  per entry u64 offset (from the end of the table), u64 size
    data        each entry: the asset in the game's payload blocks (store.blocks)

The SHA-1 is of the stored (compressed) bytes. Flags seen: 0 ebx, 1 res, 2 chunk. Every mod of the
user's has game version 175205 (Frosty takes it from the install).
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass, field

from . import store, tree
from .ebx import name_hash

MAGIC = b"FROSTY\x00\x01"
GAME_VERSION = 175205  # every one of the user's Frosty mods for Catalyst
PROFILE = "MirrorsEdgeCatalyst"
EMBEDDED, EBX, RES, CHUNK = 0, 1, 2, 3
KINDS = {EMBEDDED: "embedded", EBX: "ebx", RES: "res", CHUNK: "chunk"}


class FbmodError(ValueError):
    pass


@dataclass
class Resource:
    type: int
    index: int = -1  # in the data table
    name: str = ""
    sha1: bytes = b""
    size: int = 0
    flags: int = 0
    handler: int = 0
    user: str = ""
    bundles: list[int] = field(default_factory=list)  # djb2-xor of the bundle names it is added to
    res_type: int = 0
    res_rid: int = 0
    res_meta: bytes = b""
    chunk: tuple = (0, 0, 0, 0, 0, -1)  # range start, range end, logical offset, logical size, h32, first mip
    extra: list[int] = field(default_factory=list)  # the list before version 5 (always empty seen)


@dataclass
class Mod:
    version: int = 5
    profile: str = PROFILE
    game_version: int = GAME_VERSION
    title: str = ""
    author: str = ""
    category: str = ""
    mod_version: str = ""
    description: str = ""
    link: str = ""
    resources: list[Resource] = field(default_factory=list)
    data: list[bytes] = field(default_factory=list)  # stored as in the file (payload blocks)

    def payload(self, r: Resource) -> bytes:
        """A resource's asset, decompressed."""
        if r.index < 0:
            return b""
        return store.decompress_payload(self.data[r.index], r.name)


class _R:
    def __init__(self, d: bytes):
        self.d, self.p = d, 0

    def take(self, n: int) -> bytes:
        if self.p + n > len(self.d):
            raise FbmodError(f"runs past the end at {self.p:#x}")
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def u(self, fmt: str):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))[0]

    def cstr(self) -> str:
        end = self.d.find(b"\0", self.p)
        if end < 0:
            raise FbmodError(f"a string at {self.p:#x} has no end")
        v = self.d[self.p:end].decode("utf-8", "replace")
        self.p = end + 1
        return v

    def sized(self) -> str:
        n, shift = 0, 0
        while True:
            b = self.u("<B")
            n |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        return self.take(n).decode("utf-8")


def read(d: bytes) -> Mod:
    if not d.startswith(MAGIC):
        raise FbmodError("not a FROSTY .fbmod (an FBMODV1/V2 one is read with read_legacy)")
    r = _R(d)
    r.take(8)
    m = Mod(version=r.u("<I"))
    if m.version not in (1, 3, 5):
        raise FbmodError(f"version {m.version}: seen only 1, 3 and 5")
    data_at, data_count = r.u("<Q"), r.u("<i")
    m.profile, m.game_version = r.sized(), r.u("<I")
    m.title, m.author, m.category, m.mod_version, m.description = (r.cstr() for _ in range(5))
    if m.version >= 5:
        m.link = r.cstr()
    for _ in range(r.u("<i")):
        x = Resource(r.u("<B"), r.u("<i"))
        if x.type not in KINDS:
            raise FbmodError(f"resource type {x.type} at {r.p - 5:#x}: seen only 0 to 3")
        has = x.index != -1
        if m.version >= 5 or has:
            x.name = r.cstr()
        if has:
            x.sha1, x.size, x.flags, x.handler = r.take(20), r.u("<Q"), r.u("<B"), r.u("<i")
            if m.version >= 3:
                x.user = r.cstr()
        if m.version >= 5 or has:
            x.bundles = [r.u("<I") for _ in range(r.u("<i"))]
        if x.type == RES:
            x.res_type, x.res_rid = r.u("<I"), r.u("<Q")
            x.res_meta = r.take(r.u("<i"))
        elif x.type == CHUNK:
            x.chunk = (r.u("<I"), r.u("<I"), r.u("<I"), r.u("<I"), r.u("<i"), r.u("<i"))
        if m.version < 5 and has:
            x.extra = [r.u("<I") for _ in range(r.u("<i"))]
        m.resources.append(x)
    if r.p != data_at:
        raise FbmodError(f"the resources end at {r.p:#x}, the data table is at {data_at:#x}")
    table = [struct.unpack_from("<QQ", d, data_at + 16 * i) for i in range(data_count)]
    base = data_at + 16 * data_count
    for off, size in table:
        if base + off + size > len(d):
            raise FbmodError(f"data at {base + off:#x} (+{size:#x}) runs past the end")
        m.data.append(d[base + off:base + off + size])
    return m


def write(m: Mod) -> bytes:
    def cstr(s: str) -> bytes:
        return s.encode("utf-8") + b"\0"

    prof = m.profile.encode("utf-8")
    if len(prof) > 0x7F:
        raise FbmodError("a profile name over 127 bytes")
    out = bytearray(MAGIC + struct.pack("<IQi", m.version, 0, len(m.data)))
    out += bytes([len(prof)]) + prof + struct.pack("<I", m.game_version)
    for s in (m.title, m.author, m.category, m.mod_version, m.description) + ((m.link,) if m.version >= 5 else ()):
        out += cstr(s)
    out += struct.pack("<i", len(m.resources))
    for x in m.resources:
        has = x.index != -1
        out += struct.pack("<Bi", x.type, x.index)
        if m.version >= 5 or has:
            out += cstr(x.name)
        if has:
            out += x.sha1 + struct.pack("<QBi", x.size, x.flags, x.handler)
            if m.version >= 3:
                out += cstr(x.user)
        if m.version >= 5 or has:
            out += struct.pack("<i", len(x.bundles)) + b"".join(struct.pack("<I", b) for b in x.bundles)
        if x.type == RES:
            out += struct.pack("<IQi", x.res_type, x.res_rid, len(x.res_meta)) + x.res_meta
        elif x.type == CHUNK:
            out += struct.pack("<IIIIii", *x.chunk)
        if m.version < 5 and has:
            out += struct.pack("<i", len(x.extra)) + b"".join(struct.pack("<I", e) for e in x.extra)
    struct.pack_into("<Q", out, 12, len(out))
    off = 0
    for d in m.data:
        out += struct.pack("<QQ", off, len(d))
        off += len(d)
    for d in m.data:
        out += d
    return bytes(out)


def compress(data: bytes) -> bytes:
    """The game's payload blocks (64 KB each): LZ4 when the lz4 package is there and it shrinks."""
    try:
        import lz4.block  # type: ignore[import-not-found]
    except ImportError:
        lz4 = None
    out = bytearray()
    for i in range(0, max(len(data), 1), 0x10000):
        chunk = data[i:i + 0x10000]
        body = lz4.block.compress(chunk, store_size=False) if lz4 and chunk else b""
        if body and len(body) < len(chunk) and len(body) < 0x10000:
            out += struct.pack(">IHH", len(chunk), 0x0970, len(body)) + body
        else:
            out += struct.pack(">IHH", len(chunk), 0x0070, len(chunk) & 0xFFFF) + chunk
    return bytes(out)


def add_ebx(m: Mod, name: str, data: bytes, bundles: list[str] = ()) -> Resource:
    """Adds (or replaces) an EBX asset: `name` as in its bundle, `bundles` it is added to (names)."""
    stored = compress(data)
    for x in m.resources:
        if x.type == EBX and x.name.lower() == name.lower():
            m.data[x.index] = stored
            x.sha1, x.size = hashlib.sha1(stored).digest(), len(data)
            x.bundles = sorted(set(x.bundles) | {name_hash(b.lower()) for b in bundles})
            return x
    x = Resource(EBX, len(m.data), name.lower(), hashlib.sha1(stored).digest(), len(data),
                 bundles=[name_hash(b.lower()) for b in bundles])
    m.data.append(stored)
    m.resources.append(x)
    return x


def new_mod(title: str, author: str = "", category: str = "", version: str = "1.0", description: str = "",
            link: str = "") -> Mod:
    """A version 5 mod as Frosty makes one: an icon and four screenshot slots, empty."""
    m = Mod(title=title, author=author, category=category, mod_version=version, description=description, link=link)
    m.resources = [Resource(EMBEDDED, -1, n) for n in ("Icon", "Screenshot0", "Screenshot1", "Screenshot2", "Screenshot3")]
    return m


def read_legacy(d: bytes) -> dict:
    """An FBMODV1 / FBMODV2 mod's tree (its data is in <mod>_01.archive beside it)."""
    v = tree.parse(d)
    if not isinstance(v, dict) or v.get("magic") not in ("FBMODV1", "FBMODV2"):
        raise FbmodError("not an FBMODV1 / FBMODV2 .fbmod")
    return v
