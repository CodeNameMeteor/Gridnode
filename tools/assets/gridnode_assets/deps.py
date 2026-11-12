"""What an asset needs: the EBX files it imports (and theirs), the resources its fields name, and
the chunks those name, with the bundles that hold each.

Placing an object from one area in another (the layout editor, exported as a .fbmod) needs all of it
in the target area's bundle, or the object streams in without its data. Found from the files alone:
imports by file GUID (every EBX header has its GUID), resources by the res ids in ResourceRef fields,
chunks by their GUIDs appearing in the EBX or resource bytes (no res format needs to be known).
"""

from __future__ import annotations

import json
import os
from collections import deque
from dataclasses import dataclass, field

from . import ebx
from .store import Store, StoreError, bundles_of, read_toc


@dataclass
class Item:
    kind: str  # "ebx", "res", "chunks"
    name: str
    sha1: bytes = b""
    bundles: set = field(default_factory=set)
    via: str = ""  # what needs it (the first one found)
    here: bool = True  # its data is in this install (or copy of it)
    detail: str = ""


class Index:
    """Every bundle's entries by name, res id and chunk id; EBX file GUIDs from their headers."""

    def __init__(self, store: Store, cache_dir: str | None = None):
        self.store = store
        self.ebx: dict[str, Item] = {}
        self.res: dict[int, Item] = {}
        self.chunks: dict[bytes, Item] = {}
        for toc, sb in store.superbundles():
            if not sb:
                continue
            for b in bundles_of(sb, read_toc(toc)):
                where = f"{b.superbundle}: {b.name}"
                for e in b.entries:
                    if e.kind == "ebx":
                        it = self.ebx.setdefault(e.name.lower(), Item("ebx", e.name, e.sha1))
                    elif e.kind == "res":
                        it = self.res.setdefault(e.res_rid & 0xFFFFFFFFFFFFFFFF, Item("res", e.name, e.sha1, detail=f"resType {e.res_type:#010x}"))
                    else:
                        cid = bytes.fromhex(e.name) if len(e.name) == 32 else b""
                        if not cid:
                            continue
                        it = self.chunks.setdefault(cid, Item("chunks", e.name, e.sha1))
                    it.bundles.add(where)
        self.cache_dir = cache_dir
        self._by_guid: dict[bytes, str] | None = None

    def by_guid(self) -> dict[bytes, str]:
        """File GUID -> EBX name, from each file's header (cached by SHA-1: a game update changes them)."""
        if self._by_guid is not None:
            return self._by_guid
        cache: dict[str, str] = {}
        path = os.path.join(self.cache_dir, "ebx-guids.json") if self.cache_dir else ""
        if path and os.path.isfile(path):
            with open(path, encoding="utf-8") as f:
                cache = json.load(f)
        out: dict[bytes, str] = {}
        changed = False
        for it in self.ebx.values():
            key = it.sha1.hex()
            g = cache.get(key)
            if g is None and self.store.has(it.sha1):
                try:
                    head = self.store.read_head(it.sha1)
                except StoreError:
                    continue
                if len(head) >= 0x38:
                    g = head[0x28:0x38].hex()
                    cache[key] = g
                    changed = True
            if g:
                out[bytes.fromhex(g)] = it.name
        if path and changed:
            os.makedirs(self.cache_dir, exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                json.dump(cache, f)
        self._by_guid = out
        return out


def _chunks_in(data: bytes, chunks: dict[bytes, Item]) -> set[bytes]:
    """Chunk GUIDs that appear in the bytes (4-byte aligned, as every GUID field is)."""
    found = set()
    for i in range(0, len(data) - 15, 4):
        g = data[i:i + 16]
        if g in chunks:
            found.add(g)
    return found


def needs(index: Index, name: str, max_files: int = 5000) -> tuple[list[Item], list[str]]:
    """Everything `name` needs, itself first; and what could not be followed (not here, unknown GUIDs)."""
    start = index.ebx.get(name.lower())
    if start is None:
        raise StoreError(f"{name}: no EBX of that name in any bundle")
    by_guid = index.by_guid()
    out: list[Item] = []
    seen_ebx: set[str] = set()
    seen_res: set[int] = set()
    seen_chunks: set[bytes] = set()
    missing: list[str] = []
    todo = deque([(start, "")])

    def add_chunks(data: bytes, via: str):
        for cid in sorted(_chunks_in(data, index.chunks) - seen_chunks):
            seen_chunks.add(cid)
            c = index.chunks[cid]
            out.append(Item("chunks", c.name, c.sha1, c.bundles, via, index.store.has(c.sha1) if c.sha1 else True))

    while todo and len(seen_ebx) < max_files:
        it, via = todo.popleft()
        if it.name.lower() in seen_ebx:
            continue
        seen_ebx.add(it.name.lower())
        here = index.store.has(it.sha1)
        out.append(Item("ebx", it.name, it.sha1, it.bundles, via, here))
        if not here:
            missing.append(f"ebx {it.name}: its archive is not here (its own needs are not followed)")
            continue
        data = index.store.read(it.sha1)
        x = ebx.read(data)
        for file_guid, _ in x.imports:
            other = by_guid.get(file_guid)
            if other is None:
                missing.append(f"ebx {it.name}: imports file {file_guid.hex()}, which no EBX here has (not in this copy?)")
                continue
            o = index.ebx[other.lower()]
            if other.lower() not in seen_ebx:
                todo.append((o, it.name))
        for rid in sorted(x.resources - seen_res):
            seen_res.add(rid)
            r = index.res.get(rid)
            if r is None:
                missing.append(f"ebx {it.name}: names res id {rid:#018x}, which no bundle lists")
                continue
            rhere = index.store.has(r.sha1)
            out.append(Item("res", r.name, r.sha1, r.bundles, it.name, rhere, r.detail))
            if rhere:
                add_chunks(index.store.read(r.sha1), r.name)
        add_chunks(data, it.name)
    return out, missing


def used_by(index: Index, name: str) -> list[str]:
    """The EBX files that import NAME (read from each header's import table; no values are read)."""
    it = index.ebx.get(name.lower())
    if it is None:
        raise StoreError(f"{name}: no EBX by that name")
    want = next((g for g, n in index.by_guid().items() if n == it.name), None)
    if want is None:
        raise StoreError(f"{name}: its file GUID could not be read")
    out = []
    for other in index.ebx.values():
        if other is it or not index.store.has(other.sha1):
            continue
        try:
            head = index.store.read_head(other.sha1)
        except StoreError:
            continue
        if len(head) < 0x40:
            continue
        n_imp = int.from_bytes(head[0x0C:0x10], "little")
        table = head[0x40:0x40 + 32 * n_imp]
        if any(table[i:i + 16] == want for i in range(0, len(table), 32)):
            out.append(other.name)
    return sorted(out)
