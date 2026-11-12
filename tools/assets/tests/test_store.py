"""Tests for gridnode_assets. Run: python -m unittest discover -s tools/assets/tests -t tools/assets

A small install is made in a temporary folder from the format as described (store.py); nothing of
the game's own data is used or committed.
"""

from __future__ import annotations

import hashlib
import io
import os
import struct
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stdout

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from gridnode_assets import cli, store, tree  # noqa: E402


def block(data: bytes, codec: int) -> bytes:
    body = zlib.compress(data) if codec == 0x02 else data
    if codec == 0x02 and len(body) >= len(data):  # as the game does: a block that does not shrink is stored
        codec, body = 0x00, data
    return struct.pack(">IHH", len(data), (codec << 8) | 0x70, len(body) & 0xFFFF) + body


def payload(data: bytes, codec: int, size: int = 0x10000) -> bytes:
    return b"".join(block(data[i:i + size], codec) for i in range(0, len(data), size)) or block(b"", codec)


def make_install(root: str, *, counts: int = 2, codec: int = 0x02) -> dict[str, bytes]:
    files = {
        "levels/sp/prologue/prologue": b"EBX-PROLOGUE" * 10000,  # ~120 KB: two blocks
        "ui/textures/logo": b"TEXTURE-HEADER",
    }
    win = os.path.join(root, "Data", "Win32")
    os.makedirs(win)
    cas = bytearray()
    entries = []
    for name, data in files.items():
        sha1 = hashlib.sha1(data).digest()
        p = payload(data, codec if name.startswith("levels") else 0x00)
        entries.append((sha1, len(cas), len(p), 0, 1))
        cas += p
    with open(os.path.join(win, "cas_01.cas"), "wb") as f:
        f.write(cas)
    cat = bytearray(store.MAGIC) + struct.pack("<I", len(entries)) + (struct.pack("<I", 0) if counts >= 2 else b"")
    if counts == 3:
        cat += struct.pack("<I", 0)
    for e in entries:
        cat += store.ENTRY.pack(*e)
    with open(os.path.join(win, "cas.cat"), "wb") as f:
        f.write(b"\x00\xd1\xce\x01" + os.urandom(store.TOC_HEADER - 4) + cat)  # signed, as the user's install
    names = list(files)
    body = tree.encode({
        "ebx": [{"name": names[0], "sha1": hashlib.sha1(files[names[0]]).digest(), "size": entries[0][2], "originalSize": len(files[names[0]])}],
        "res": [{"name": names[1], "sha1": hashlib.sha1(files[names[1]]).digest(), "size": entries[1][2],
                 "originalSize": len(files[names[1]]), "resType": 0x6BDE20BA, "resRid": 7, "resMeta": bytes(16)}],
        "chunks": [],
    })
    sb = b"\0" * 16 + body  # a bundle need not start the file
    with open(os.path.join(win, "levels.sb"), "wb") as f:
        f.write(sb)
    toc = tree.encode({"bundles": [{"id": "levels/sp/prologue", "offset": 16, "size": len(body)}], "cas": True})
    with open(os.path.join(win, "levels.toc"), "wb") as f:
        f.write(b"\x00\xd1\xce\x01" + b"\0" * (store.TOC_HEADER - 4) + toc)
    return files


class Tree(unittest.TestCase):
    def test_round_trip(self):
        v = {"a": [1, 2, {"b": "text", "on": True}], "sha1": bytes(range(20)), "guid": bytes(16), "blob": b"xyz", "f": 1.5}
        self.assertEqual(tree.parse(tree.encode(v)), v)

    def test_long_list_length(self):
        v = {"items": ["x" * 50] * 10}  # the length needs two 7-bit groups
        self.assertEqual(tree.parse(tree.encode(v)), v)

    def test_unknown_type_is_named(self):
        with self.assertRaisesRegex(tree.FormatError, "unknown value type 0x1f"):
            tree.parse(bytes([0x9F]))

    def test_truncated(self):
        with self.assertRaises(tree.FormatError):
            tree.parse(tree.encode({"a": "long text"})[:-4])


class Store(unittest.TestCase):
    def test_blocks(self):
        data = os.urandom(200000)
        self.assertEqual(store.decompress_payload(payload(data, 0x02)), data)
        self.assertEqual(store.decompress_payload(payload(data, 0x00)), data)  # stored 64 KB blocks
        with self.assertRaisesRegex(store.StoreError, "unknown compression codec 0x7e"):
            store.decompress_payload(block(b"abc", 0x7E))
        with self.assertRaisesRegex(store.StoreError, "Oodle"):
            store.decompress_payload(block(b"abc", 0x15))

    def test_catalogue_layouts(self):
        for counts in (2, 3):
            with tempfile.TemporaryDirectory() as d:
                make_install(d, counts=counts)
                cat = store.read_catalogue(os.path.join(d, "Data", "Win32", "cas.cat"))
                self.assertEqual((len(cat.entries), cat.counts_layout), (2, counts))

    def test_xored_catalogue(self):
        with tempfile.TemporaryDirectory() as d:
            head = bytearray(os.urandom(store.TOC_HEADER))
            head[:4] = b"\x00\xd1\xce\x00"
            key = head[0x128:0x128 + 257]
            body = store.MAGIC + struct.pack("<II", 0, 0)
            p = os.path.join(d, "cas.cat")
            with open(p, "wb") as f:
                f.write(bytes(head) + bytes(b ^ key[i % 257] ^ 0x7B for i, b in enumerate(body)))
            self.assertEqual(len(store.read_catalogue(p).entries), 0)

    def test_catalogue_that_fits_no_layout(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "cas.cat")
            with open(p, "wb") as f:
                f.write(store.MAGIC + struct.pack("<II", 5, 0) + b"\0" * 7)
            with self.assertRaisesRegex(store.StoreError, "no known catalogue layout"):
                store.read_catalogue(p)

    def test_odd_catalogue_is_listed_not_fatal(self):
        with tempfile.TemporaryDirectory() as d:
            make_install(d)
            odd = os.path.join(d, "Data", "Win32", "other")
            os.makedirs(odd)
            with open(os.path.join(odd, "cas.cat"), "wb") as f:
                f.write(b"\x00\x01something else")
            s = store.Store(os.path.join(d, "Data") + '\\"')  # as PowerShell passes "...\Data\"
            self.assertEqual(len(s.catalogues), 1)
            self.assertEqual(len(s.problems), 1)
            self.assertIn("00 01 73", s.problems[0])
            r, text = Cli().run_cli("check", "--data", os.path.join(d, "Data"))
            self.assertEqual(r, 1)
            self.assertIn("bundles: 1", text)  # the rest still read

    def test_install(self):
        with tempfile.TemporaryDirectory() as d:
            files = make_install(d)
            s = store.Store(os.path.join(d, "Data"))
            [(toc, sb)] = s.superbundles()
            [b] = store.bundles_of(sb, store.read_toc(toc))
            self.assertEqual(b.name, "levels/sp/prologue")
            ebx, res = b.entries
            self.assertEqual((ebx.kind, res.kind, res.res_type, res.res_rid), ("ebx", "res", 0x6BDE20BA, 7))
            self.assertEqual(s.read(ebx.sha1), files[ebx.name])
            self.assertEqual(s.read(res.sha1), files[res.name])
            self.assertEqual(s.codecs(), {"zlib": 2, "none": 1})


class Cli(unittest.TestCase):
    def run_cli(self, *args) -> tuple[int, str]:
        out = io.StringIO()
        with redirect_stdout(out):
            r = cli.main(list(args))
        return r, out.getvalue()

    def test_check_list_extract(self):
        with tempfile.TemporaryDirectory() as d:
            files = make_install(d)
            data = os.path.join(d, "Data")
            r, text = self.run_cli("check", "--data", data)
            self.assertEqual(r, 0, text)
            self.assertIn("bundles: 1", text)
            self.assertIn("'zlib': 2", text)
            r, text = self.run_cli("list", "--data", data, "--type", "res")
            self.assertEqual(r, 0)
            self.assertIn("ui/textures/logo", text)
            self.assertNotIn("prologue  ", text)
            out = os.path.join(d, "out")
            r, text = self.run_cli("extract", "levels/sp/prologue/prologue", "--data", data, "--out", out)
            self.assertEqual(r, 0, text)
            with open(os.path.join(out, "ebx", "levels", "sp", "prologue", "prologue.ebx"), "rb") as f:
                self.assertEqual(f.read(), files["levels/sp/prologue/prologue"])
            self.assertEqual(self.run_cli("extract", "no/such", "--data", data)[0], 1)


if __name__ == "__main__":
    unittest.main()
