"""The game's content-addressed store: catalogues (cas.cat), archives (cas_NN.cas), superbundle
indexes (.toc) and bundles (.sb), and the payload blocks every stored file is cut into.

    cas.cat     "NyanNyanNyanNyan", counts, then 0x24-byte entries (SHA-1, offset, size, logical
                offset, archive number), then patch entries (three SHA-1s: result, base, delta)
    cas_NN.cas  the payloads the catalogue points into, beside it
    <name>.toc  a tree (tree.py) listing the bundles in the matching .sb (offset, size) and loose
                chunks; may begin with a 0x22C-byte header, which is skipped
    <name>.sb   per bundle, a tree with three lists: ebx, res, chunks
    sources     Patch, Update, Data: searched in that order

Checked on the user's install (2026-11-11): every cas.cat sits behind the signed header, and two counts
(entries, patch entries) follow the magic in all 11. The reader still takes whichever layout fills the
file exactly, and refuses one where none does.
"""

from __future__ import annotations

import os
import struct
import zlib
from collections import Counter
from dataclasses import dataclass, field

from . import tree

MAGIC = b"NyanNyanNyanNyan"
ENTRY = struct.Struct("<20sIIII")  # sha1, offset, size, logical offset, archive
PATCH = struct.Struct("<20s20s20s")  # result, base, delta
SOURCES = ("Patch", "Update", "Data")
TOC_HEADER = 0x22C
TOC_HEADER_MAGICS = (b"\x00\xd1\xce\x00", b"\x00\xd1\xce\x01", b"\x00\xd1\xce\x03")


class StoreError(ValueError):
    pass


@dataclass(frozen=True)
class CatEntry:
    sha1: bytes
    offset: int
    size: int
    logical_offset: int
    archive: int
    folder: str  # the catalogue's folder (its archives are there)

    def archive_path(self) -> str:
        return os.path.join(self.folder, f"cas_{self.archive:02d}.cas")


@dataclass
class Catalogue:
    path: str
    entries: list[CatEntry]
    patches: list[tuple[bytes, bytes, bytes]]
    counts_layout: int  # how many u32 counts followed the magic


def unwrap(data: bytes) -> bytes:
    """A file's content without the signed header some files carry (00 d1 ce 0x, 0x22C bytes: every
    cas.cat and .toc of the user's install, 2026-11-11). When the content behind it does not read as
    plain, it may be XORed with the 257-byte key at 0x128 (each byte with key[i % 257] ^ 0x7B):
    unwrap_xor tries that."""
    return data[TOC_HEADER:] if data[:4] in TOC_HEADER_MAGICS and len(data) >= TOC_HEADER else data


def unwrap_xor(data: bytes) -> bytes:
    key = data[0x128:0x128 + 257]
    body = data[TOC_HEADER:]
    if len(key) < 257:
        return body
    return bytes(b ^ key[i % 257] ^ 0x7B for i, b in enumerate(body))


def read_catalogue(path: str) -> Catalogue:
    with open(path, "rb") as f:
        raw = f.read()
    data = unwrap(raw)
    if not data.startswith(MAGIC) and raw[:4] in TOC_HEADER_MAGICS:
        x = unwrap_xor(raw)
        if x.startswith(MAGIC):
            data = x
    if not data.startswith(MAGIC):
        raise StoreError(f"{path}: not a catalogue as described (no NyanNyanNyanNyan, plain or behind the header); {len(raw)} bytes, "
                         f"starting {raw[:48].hex(' ')}; after the header {raw[TOC_HEADER:TOC_HEADER + 48].hex(' ')}")
    folder = os.path.dirname(os.path.abspath(path))
    # Two or three counts: entries, patch entries (and one more in some builds). The right layout is
    # the one whose tables fill the file exactly.
    for ncounts in (2, 3, 1):
        head = len(MAGIC) + 4 * ncounts
        if len(data) < head:
            continue
        counts = struct.unpack_from(f"<{ncounts}I", data, len(MAGIC))
        n = counts[0]
        p = counts[1] if ncounts > 1 else 0
        if head + n * ENTRY.size + p * PATCH.size != len(data):
            continue
        entries = [CatEntry(*ENTRY.unpack_from(data, head + i * ENTRY.size), folder=folder) for i in range(n)]
        at = head + n * ENTRY.size
        patches = [PATCH.unpack_from(data, at + i * PATCH.size) for i in range(p)]
        return Catalogue(path, entries, patches, ncounts)
    raise StoreError(f"{path}: {len(data)} bytes match no known catalogue layout (counts after the magic: "
                     f"{struct.unpack_from('<3I', data, len(MAGIC)) if len(data) >= len(MAGIC) + 12 else '?'}); "
                     "the format needs looking at")


# ---- payload blocks -------------------------------------------------------------------------------
# Each block: uncompressed size (big-endian u32), a compression type (big-endian u16), compressed
# size (big-endian u16). The high byte of the type says the codec. Catalyst uses LZ4 (0x09) and
# stored blocks (0x00): 3,092 and 2,068 blocks in the first 2,000 entries of the user's install
# (2026-11-11); no zlib, zstd or Oodle seen there. The others are what other Frostbite games use.
CODECS = {0x00: "none", 0x02: "zlib", 0x09: "lz4", 0x0F: "zstd", 0x11: "oodle", 0x15: "oodle", 0x19: "oodle"}


def codec_of(ctype: int) -> int:
    return (ctype >> 8) & 0x7F


def _decompress(code: int, data: bytes, size: int, where: str) -> bytes:
    name = CODECS.get(code)
    if name == "none":
        return data
    if name == "zlib":
        try:
            return zlib.decompress(data)
        except zlib.error as e:
            raise StoreError(f"{where}: a zlib block does not decompress ({e})") from e
    if name == "zstd":
        try:
            import zstandard  # type: ignore[import-not-found]
        except ImportError as e:
            raise StoreError(f"{where}: zstd block: pip install zstandard") from e
        return zstandard.ZstdDecompressor().decompress(data, max_output_size=size)
    if name == "lz4":
        try:
            import lz4.block  # type: ignore[import-not-found]
        except ImportError as e:
            raise StoreError(f"{where}: lz4 block: pip install lz4") from e
        return lz4.block.decompress(data, uncompressed_size=size)
    if name == "oodle":
        raise StoreError(f"{where}: an Oodle block (codec {code:#04x}): needs the game's own Oodle DLL, not supported yet")
    raise StoreError(f"{where}: unknown compression codec {code:#04x}")


def blocks(data: bytes, where: str = "payload"):
    """(codec, uncompressed size, compressed bytes) for each block, without decompressing."""
    pos = 0
    while pos < len(data):
        if pos + 8 > len(data):
            raise StoreError(f"{where}: a block header runs past the end at {pos:#x}")
        size, ctype, csize = struct.unpack_from(">IHH", data, pos)
        pos += 8
        code = codec_of(ctype)
        if CODECS.get(code) == "none":
            csize = size  # stored: the 16-bit field cannot hold a full 64 KB block
        elif csize == 0:
            csize = 0x10000
        if pos + csize > len(data):
            raise StoreError(f"{where}: a block of {csize} bytes runs past the end at {pos:#x}")
        yield code, size, data[pos:pos + csize]
        pos += csize


def decompress_payload(data: bytes, where: str = "payload") -> bytes:
    out = bytearray()
    for code, size, raw in blocks(data, where):
        got = _decompress(code, raw, size, where)
        if len(got) != size:
            raise StoreError(f"{where}: a block gave {len(got)} bytes, its header says {size}")
        out += got
    return bytes(out)


# ---- superbundles and bundles -----------------------------------------------------------------------
def read_toc(path: str):
    with open(path, "rb") as f:
        data = f.read()
    try:
        return tree.parse(unwrap(data))
    except tree.FormatError as e:
        raise StoreError(f"{path}: {e}") from e


@dataclass
class BundleEntry:
    kind: str  # "ebx", "res", "chunks"
    name: str  # a chunk's GUID as hex
    sha1: bytes
    size: int
    original_size: int
    res_type: int = 0
    res_rid: int = 0
    res_meta: bytes = b""
    raw: dict = field(default_factory=dict, repr=False)


@dataclass
class Bundle:
    superbundle: str
    name: str
    entries: list[BundleEntry]


def _entries(kind: str, items: list) -> list[BundleEntry]:
    out = []
    for e in items or []:
        if not isinstance(e, dict):
            continue
        name = e.get("name") if kind != "chunks" else (e.get("id") or b"").hex()
        out.append(BundleEntry(kind, str(name), e.get("sha1", b""), int(e.get("size", 0) or 0), int(e.get("originalSize", 0) or 0),
                               int(e.get("resType", 0) or 0) & 0xFFFFFFFF, int(e.get("resRid", 0) or 0), e.get("resMeta", b"") or b"", e))
    return out


def bundles_of(sb_path: str, toc) -> list[Bundle]:
    """The bundles a superbundle's index lists, each read from the .sb at its offset."""
    out = []
    listed = toc.get("bundles", []) if isinstance(toc, dict) else []
    if not listed:
        return out
    with open(sb_path, "rb") as f:
        sb = f.read()
    for b in listed:
        if not isinstance(b, dict) or "offset" not in b:
            continue
        name = str(b.get("id", "?"))
        off, size = int(b["offset"]), int(b.get("size", 0))
        if off + size > len(sb):
            raise StoreError(f"{sb_path}: bundle {name} at {off:#x} (+{size:#x}) runs past the file")
        try:
            body = tree.parse(sb, off)
        except tree.FormatError as e:
            raise StoreError(f"{sb_path}: bundle {name}: {e}") from e
        if not isinstance(body, dict):
            raise StoreError(f"{sb_path}: bundle {name} is not an object (a delta bundle, or a format this reader does not know)")
        entries = []
        for kind in ("ebx", "res", "chunks"):
            entries += _entries(kind, body.get(kind))
        out.append(Bundle(os.path.basename(sb_path), name, entries))
    return out


# ---- the whole data folder ------------------------------------------------------------------------------
class Store:
    """Every catalogue under a data root (data_path in game), and the superbundles."""

    def __init__(self, root: str):
        # PowerShell passes "C:\...\Data\" with the quote kept: the backslash escaped it.
        root = root.strip().strip('"').rstrip("\\/") or root
        self.root = os.path.abspath(root)
        self.problems: list[str] = []  # catalogues that did not read: listed, the rest still read
        if not os.path.isdir(self.root):
            raise StoreError(f"{root}: not a folder (give the game's Data folder, or gridnode.log's 'game data from')")
        self.catalogues: list[Catalogue] = []
        self.by_sha1: dict[bytes, CatEntry] = {}
        for src in self._sources():
            for dirpath, _dirs, files in os.walk(src):
                if "cas.cat" in files:
                    try:
                        cat = read_catalogue(os.path.join(dirpath, "cas.cat"))
                    except (StoreError, OSError) as e:
                        self.problems.append(str(e))
                        continue
                    self.catalogues.append(cat)
                    for e in cat.entries:
                        self.by_sha1.setdefault(e.sha1, e)  # Patch before Update before Data

    def _sources(self) -> list[str]:
        # root is Data (or a -dataPath folder); Patch and Update sit beside or inside the install.
        parent = os.path.dirname(self.root)
        found = [os.path.join(parent, s) for s in SOURCES[:2] if os.path.isdir(os.path.join(parent, s))]
        found += [os.path.join(self.root, s) for s in SOURCES[:2] if os.path.isdir(os.path.join(self.root, s))]
        return found + [self.root]

    def superbundles(self) -> list[tuple[str, str]]:
        """(toc, sb) pairs, Data's own folder layout."""
        out = []
        for dirpath, _dirs, files in os.walk(self.root):
            for f in sorted(files):
                if f.endswith(".toc"):
                    sb = os.path.join(dirpath, f[:-4] + ".sb")
                    out.append((os.path.join(dirpath, f), sb if os.path.isfile(sb) else ""))
        return out

    def read(self, sha1: bytes) -> bytes:
        e = self.by_sha1.get(sha1)
        if e is None:
            raise StoreError(f"{sha1.hex()} is in no catalogue")
        with open(e.archive_path(), "rb") as f:
            f.seek(e.offset)
            raw = f.read(e.size)
        if len(raw) != e.size:
            raise StoreError(f"{e.archive_path()}: {e.size} bytes at {e.offset:#x} run past the file")
        return decompress_payload(raw, f"{sha1.hex()} in {os.path.basename(e.archive_path())}")

    def codecs(self, sample: int = 0) -> Counter:
        """Blocks by codec over the catalogues' entries (all, or the first `sample`)."""
        seen: Counter = Counter()
        for n, e in enumerate(self.by_sha1.values()):
            if sample and n >= sample:
                break
            with open(e.archive_path(), "rb") as f:
                f.seek(e.offset)
                raw = f.read(e.size)
            for code, _size, _raw in blocks(raw, e.sha1.hex()):
                seen[CODECS.get(code, f"unknown {code:#04x}")] += 1
        return seen
