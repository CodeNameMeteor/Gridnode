"""Tests for gridnode_assets.fbmod (no game data: mods made here)."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from gridnode_assets import cli, ebx, fbmod, tree  # noqa: E402
from tests.test_ebx import sample  # noqa: E402


class Fbmod(unittest.TestCase):
    def test_each_version_round_trips(self):
        for version in (1, 3, 5):
            m = fbmod.new_mod("T", "me", "Gameplay", "1.0", "desc", "link")
            m.version = version
            fbmod.add_ebx(m, "Objects/A", b"EBX" * 100, ["win32/levels/sp/sp_maincity/sp_maincity"])
            m.resources.append(fbmod.Resource(fbmod.RES, len(m.data), "objects/a_mesh", b"\x01" * 20, 9, 1,
                                              res_type=0x49B156D4, res_rid=0xF1F582422FDCD983, res_meta=bytes(range(16))))
            m.data.append(fbmod.compress(b"resource!"))
            m.resources.append(fbmod.Resource(fbmod.CHUNK, len(m.data), "68ba6c4e-2a43-8a33-bb49-94a049eb7827", b"\x02" * 20,
                                              0, 2, chunk=(0, 0, 0, 2073600, 653092343, -1)))
            m.data.append(fbmod.compress(b"chunk" * 1000))
            d = fbmod.write(m)
            r = fbmod.read(d)
            self.assertEqual(fbmod.write(r), d, version)
            a = next(x for x in r.resources if x.type == fbmod.EBX)
            self.assertEqual((a.name, a.size, r.payload(a)), ("objects/a", 300, b"EBX" * 100))
            self.assertEqual(a.bundles, [0xD78A2FF2])  # djb2-xor of the bundle name, as Frosty's mods have
            self.assertEqual(r.resources[-2].res_rid, 0xF1F582422FDCD983)
            if version == 5:
                self.assertEqual([x.name for x in r.resources[:5]], ["Icon", "Screenshot0", "Screenshot1", "Screenshot2", "Screenshot3"])

    def test_compress_is_the_games_blocks(self):
        from gridnode_assets import store
        data = os.urandom(70000) + b"A" * 70000
        self.assertEqual(store.decompress_payload(fbmod.compress(data)), data)

    def test_not_a_mod(self):
        with self.assertRaises(fbmod.FbmodError):
            fbmod.read(b"nope")
        with self.assertRaises(fbmod.FbmodError):
            fbmod.read_legacy(tree.encode({"magic": "OTHER"}))

    def test_build_from_a_spec(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "thing.yaml"), "w", encoding="utf-8") as f:
                f.write(ebx.to_yaml(ebx.read(ebx.write(sample()))))
            with open(os.path.join(d, "mod.yaml"), "w", encoding="utf-8") as f:
                f.write("title: Test\nauthor: me\nebx:\n  - name: levels/test\n    file: thing.yaml\n")
            out = os.path.join(d, "t.fbmod")
            self.assertEqual(cli.main(["fbmod-build", os.path.join(d, "mod.yaml"), "--out", out]), 0)
            with open(out, "rb") as f:
                m = fbmod.read(f.read())
            res = [x for x in m.resources if x.index >= 0]
            self.assertEqual([x.name for x in res], ["levels/test"])
            self.assertEqual(m.payload(res[0]), ebx.write(sample()))


if __name__ == "__main__":
    unittest.main()
