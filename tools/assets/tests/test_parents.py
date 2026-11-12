"""Tests for gridnode_assets.parents (bundle parents from level files; models built in memory)."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from gridnode_assets import parents  # noqa: E402
from gridnode_assets.ebx import Ebx, Instance, Type  # noqa: E402


def level(*refs: tuple[str, list[str]]) -> Ebx:
    types = [Type("SubWorldData", 0, 0, 0, 0), Type(parents.KIND, 0, 0, 0, 0)]
    insts = [Instance(0, None, {})] + [Instance(1, None, {"BundleName": n, "PreloadedBundleNames": p}) for n, p in refs]
    return Ebx(b"\0" * 16, [], types, insts, [])


class Parents(unittest.TestCase):
    def test_table_and_chain(self):
        files = [("Win32/levels/sp/sp_maincity/sp_maincity", level(("Levels/SP/SP_MainCity/Anchor", ["Levels/SP/SP_MainCity/Shared"]))),
                 ("Win32/levels/sp/sp_maincity/anchor", level(("Levels/SP/SP_MainCity/AnchorArea3", [])))]
        table = parents.parents_of(files)
        self.assertEqual(table["levels/sp/sp_maincity/anchorarea3"], {"parent": "levels/sp/sp_maincity/anchor", "preload": []})
        self.assertEqual(parents.chain(table, "Levels/SP/SP_MainCity/AnchorArea3"),
                         ["levels/sp/sp_maincity/shared", "levels/sp/sp_maincity/sp_maincity", "levels/sp/sp_maincity/anchor"])
        self.assertEqual(parents.chain(table, "levels/sp/sp_maincity/unknown"), [])
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "p.txt")
            parents.write_table(table, path)
            lines = open(path, encoding="utf-8").read().splitlines()
            self.assertIn("levels/sp/sp_maincity/anchor\tlevels/sp/sp_maincity/sp_maincity\tlevels/sp/sp_maincity/shared\t", lines)
        # a blueprint in a shared bundle and two sublevels: the sublevel with the shortest chain
        self.assertEqual(parents.prefab_bundle(["Win32/shared/stuff", "Win32/levels/sp/sp_maincity/anchorarea3",
                                                "Win32/levels/sp/sp_maincity/anchor"], table), "levels/sp/sp_maincity/anchor")
        self.assertEqual(parents.prefab_bundle(["Win32/shared/stuff"], table), "")
        contents = {"a": {"x", "y"}, "b": {"y"}, "c": {"z"}, "lvl": {"x"}}
        self.assertEqual(parents.shares_of(contents, ["a", "c"], {"lvl"}), {"a": ["b"], "c": []})


if __name__ == "__main__":
    unittest.main()
