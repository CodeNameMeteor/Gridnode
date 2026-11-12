"""Tests for gridnode_assets.deps on a small install made in a temporary folder (no game data)."""

from __future__ import annotations

import hashlib
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from gridnode_assets import deps, ebx, store, tree  # noqa: E402
from gridnode_assets.ebx import Ebx, Field, Instance, Type  # noqa: E402
from tests.test_store import payload  # noqa: E402

CLASS, RESREF = 0x35, 0x417D
CHUNK = bytes(range(0x40, 0x50))


def asset(guid: bytes, imports: list, rid: int) -> bytes:
    types = [Type("DataContainer", 4, CLASS, 8, 0),
             Type("Thing", 8, CLASS, 0x18, 0, [Field("$", 0, 0, 8, 0), Field("Other", CLASS, 0, 8, 0x10),
                                                Field("Resource", RESREF, 0, 0x10, 0x18)])]
    values = {"Other": "import:0" if imports else None, "Resource": f"{rid:#x}"}
    return ebx.write(Ebx(guid, imports, types, [Instance(1, guid, values)], []))


def install(root: str) -> None:
    ga, gb = b"\xaa" * 16, b"\xbb" * 16
    files = [("ebx", "objects/a", asset(ga, [(gb, gb)], 0)), ("ebx", "objects/b", asset(gb, [], 0x1234)),
             ("res", "objects/b_mesh", b"RES" + bytes(9) + CHUNK + b"tail"), ("chunks", CHUNK.hex(), b"CHUNKDATA")]
    win = os.path.join(root, "Data", "Win32")
    os.makedirs(win)
    cas, cat, lists = bytearray(), bytearray(store.MAGIC) + struct.pack("<II", len(files), 0), {"ebx": [], "res": [], "chunks": []}
    for kind, name, data in files:
        sha1 = hashlib.sha1(data).digest()
        p = payload(data, 0x00)
        cat += store.ENTRY.pack(sha1, len(cas), len(p), 0, 1)
        cas += p
        e = {"sha1": sha1, "size": len(p), "originalSize": len(data)}
        if kind == "chunks":
            e["id"] = CHUNK
        else:
            e["name"] = name
        if kind == "res":
            e.update(resType=0x49B156D4, resRid=0x1234, resMeta=bytes(16))
        lists[kind].append(e)
    with open(os.path.join(win, "cas_01.cas"), "wb") as f:
        f.write(cas)
    with open(os.path.join(win, "cas.cat"), "wb") as f:
        f.write(cat)
    body = tree.encode(lists)
    with open(os.path.join(win, "props.sb"), "wb") as f:
        f.write(body)
    with open(os.path.join(win, "props.toc"), "wb") as f:
        f.write(tree.encode({"bundles": [{"id": "levels/props", "offset": 0, "size": len(body)}], "cas": True}))


class Deps(unittest.TestCase):
    def test_follows_imports_resources_and_chunks(self):
        with tempfile.TemporaryDirectory() as d:
            install(d)
            index = deps.Index(store.Store(os.path.join(d, "Data")), os.path.join(d, "cache"))
            items, missing = deps.needs(index, "Objects/A")
            got = [(i.kind, i.name, i.via) for i in items]
            self.assertEqual(got, [("ebx", "objects/a", ""), ("ebx", "objects/b", "objects/a"),
                                   ("res", "objects/b_mesh", "objects/b"), ("chunks", CHUNK.hex(), "objects/b_mesh")])
            self.assertEqual(missing, [])
            self.assertTrue(all("props.sb: levels/props" in i.bundles for i in items))
            self.assertTrue(os.path.isfile(os.path.join(d, "cache", "ebx-guids.json")))  # GUIDs kept for next time


if __name__ == "__main__":
    unittest.main()
