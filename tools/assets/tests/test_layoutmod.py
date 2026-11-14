"""Tests for gridnode_assets.layoutmod. Run: python -m unittest discover -s tools/assets/tests -t tools/assets

A tiny level is built from made-up types (nothing of the game's data): a zone layer, a second layer to copy a
reference object from, the level file that streams both in, and a prefab in the second zone's bundle. The real
check is `assets layout-fbmod` on the user's install and the mod in game.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from gridnode_assets import ebx, layoutmod  # noqa: E402
from gridnode_assets.ebx import Ebx, Field, Instance, Type  # noqa: E402

CLASS, ARRAY, VEC, BOOL, CSTR = 0x35, 0x41, 0xD029, 0xC0AD, 0x407D
D = "levels/sp/sp_maincity/"


def base_types() -> list[Type]:
    return [
        Type("DataContainer", 4, CLASS, 8, 0),
        Type("Vec3", 16, VEC, 0x10, 0, [Field("x", 0xC13D, 0, 0, 0), Field("y", 0xC13D, 0, 4, 4), Field("z", 0xC13D, 0, 8, 8)]),
        Type("LinearTransform", 16, VEC, 0x40, 0, [Field(n, VEC, 1, o, o) for n, o in (("right", 0), ("up", 0x10), ("forward", 0x20), ("trans", 0x30))]),
        Type("array", 4, ARRAY, 4, 0, [Field("member", CLASS, 0, 0, 0)]),
        Type("WorldPartData", 16, CLASS, 0x10, 0, [Field("$", 0, 0, 8, 0), Field("Name", CSTR, 0, 8, 0x10), Field("Objects", ARRAY, 3, 0xC, 0x18)]),
    ]


def transform(x: float, y: float, z: float) -> dict:
    return {"right": {"x": 1.0, "y": 0.0, "z": 0.0}, "up": {"x": 0.0, "y": 1.0, "z": 0.0},
            "forward": {"x": 0.0, "y": 0.0, "z": 1.0}, "trans": {"x": x, "y": y, "z": z}}


def layer(name: str, extra: list[Type], objects: list[Instance]) -> Ebx:
    types = base_types() + extra
    root = Instance(4, bytes([len(name)]) * 16, {"Name": name, "Objects": [f"#{i + 1}" for i in range(len(objects))]})
    return Ebx(bytes([len(name) + 1]) * 16, [], types, [root, *objects], [])


def rod_type() -> Type:
    return Type("SpatialPrefabReferenceObjectData", 16, CLASS, 0x50, 0, [
        Field("$", 0, 0, 8, 0), Field("Blueprint", CLASS, 0, 8, 0x10), Field("Excluded", BOOL, 0, 0xC, 0x18),
        Field("BlueprintTransform", VEC, 2, 0x10, 0x20)])


def swr_types() -> list[Type]:
    return [Type("array", 4, ARRAY, 4, 0, [Field("member", CSTR, 0, 0, 0)]),
            Type("SubWorldReferenceObjectData", 16, CLASS, 0x10, 0, [
                Field("$", 0, 0, 8, 0), Field("BundleName", CSTR, 0, 8, 0x10), Field("PreloadedBundleNames", ARRAY, 5, 0xC, 0x18)])]


class Store:
    def __init__(self):
        self.files: dict[bytes, bytes] = {}

    def add(self, x: Ebx) -> bytes:
        x.runs = ebx.runs_of(x.instances)
        sha = bytes([len(self.files) + 1]) * 20
        self.files[sha] = ebx.write(x)
        return sha

    def read(self, sha: bytes) -> bytes:
        return self.files[sha]


def level() -> layoutmod.LevelIndex:
    store = Store()
    zone = layer(D + "zone/layer0_zone_layer", [], [])
    other = layer(D + "other/layer0_other_layer", [rod_type()],
                  [Instance(5, None, {"Blueprint": None, "Excluded": True, "BlueprintTransform": transform(500, 0, 0)})])
    top = layer(D + "sp_maincity", swr_types(), [
        Instance(6, None, {"BundleName": "Levels/SP/SP_MainCity/Zone", "PreloadedBundleNames": []}),
        Instance(6, None, {"BundleName": "Levels/SP/SP_MainCity/Other", "PreloadedBundleNames": []})])
    prefab = Ebx(b"\x77" * 16, [], [Type("SpatialPrefabBlueprint", 4, CLASS, 8, 0)], [Instance(0, b"\x78" * 16, {})], [])
    idx = object.__new__(layoutmod.LevelIndex)
    idx.store, idx.level, idx.dir = store, D + "sp_maincity", D
    idx.ebx = {
        zone.instances[0].values["Name"]: (store.add(zone), {D + "zone"}),
        other.instances[0].values["Name"]: (store.add(other), {D + "other"}),
        D + "sp_maincity": (store.add(top), {D + "sp_maincity"}),
        "props/box": (store.add(prefab), {D + "other"}),
    }
    idx.layers = [
        layoutmod.Layer(D + "zone/layer0_zone_layer", D + "zone", b"", [(0.0, 0.0, 0.0), (100.0, 10.0, 100.0)],
                        (0.0, 0.0, 0.0), (100.0, 10.0, 100.0), ["WorldPartData"]),
        layoutmod.Layer(D + "other/layer0_other_layer", D + "other", b"", [(500.0, 0.0, 0.0)],
                        (500.0, 0.0, 0.0), (500.0, 0.0, 0.0), ["SpatialPrefabReferenceObjectData", "WorldPartData"]),
    ]
    idx.refs = [layoutmod.SubWorldRef(D + "sp_maincity", b"", D + "sp_maincity", D + c, []) for c in ("zone", "other")]
    idx.table = {r.child: {"parent": r.bundle, "preload": r.preload} for r in idx.refs}
    return idx


def edit(kind: str, at: list, **more) -> dict:
    return {"kind": kind, "position": at, "right": [1, 0, 0], "up": [0, 1, 0], "forward": [0, 0, 1], **more}


class LayoutModTest(unittest.TestCase):
    def test_prefab_goes_into_its_zone_with_preloads(self):
        idx = level()
        edited, rep = layoutmod.export(idx, [edit("PlacedPrefab", [50, 1, 50], asset="Props/Box"),
                                             edit("MovedObject", [1, 1, 1])])
        self.assertEqual(rep.problems, [])
        self.assertEqual(rep.skipped, {"MovedObject": 1})
        self.assertEqual(rep.placed, [("SpatialPrefabReferenceObjectData", "Props/Box", D + "zone/layer0_zone_layer")])
        self.assertEqual(sorted(edited), [D + "sp_maincity", D + "zone/layer0_zone_layer"])

        x = edited[D + "zone/layer0_zone_layer"]
        x.runs = ebx.runs_of(x.instances)
        y = ebx.read(ebx.write(x))  # the edited layer still writes and reads back
        self.assertEqual(y.instances[0].values["Objects"], ["#1"])
        new = y.instances[1]
        self.assertIsNone(new.guid)
        self.assertEqual(y.types[new.type].name, "SpatialPrefabReferenceObjectData")
        self.assertEqual(new.values["Blueprint"], "import:0")
        self.assertEqual(y.imports, [(b"\x77" * 16, b"\x78" * 16)])
        self.assertEqual(new.values["BlueprintTransform"]["trans"], {"x": 50.0, "y": 1.0, "z": 50.0})
        self.assertFalse(new.values["Excluded"])
        self.assertEqual([t.name for t in y.types].count("LinearTransform"), 1)  # shared, not copied twice

        top = edited[D + "sp_maincity"]
        rows = {i.values["BundleName"]: i.values["PreloadedBundleNames"] for i in top.instances[1:]}
        self.assertEqual(rows["Levels/SP/SP_MainCity/Zone"], [D + "other"])
        self.assertEqual(rows["Levels/SP/SP_MainCity/Other"], [])

    def test_problems_are_reported_not_raised(self):
        idx = level()
        edited, rep = layoutmod.export(idx, [edit("PlacedBlocker", [50, 1, 50], size=[1, 1, 1]),
                                             edit("PlacedPrefab", [50, 1, 50], asset="props/missing")])
        self.assertEqual(rep.placed, [])
        self.assertEqual(len(rep.problems), 2)
        self.assertIn("PamOBBCollisionEntityData", rep.problems[0])
        self.assertIn("props/missing", rep.problems[1])

    def test_layer_for_picks_the_box_then_the_nearest(self):
        idx = level()
        self.assertEqual(idx.layer_for((50, 5, 50)).bundle, D + "zone")
        self.assertEqual(idx.layer_for((480, 0, 0)).bundle, D + "other")  # outside both boxes: nearest object
        self.assertEqual(idx.layer_named("layer0_other_layer").bundle, D + "other")


if __name__ == "__main__":
    unittest.main()
