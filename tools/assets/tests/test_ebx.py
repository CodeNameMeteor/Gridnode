"""Tests for gridnode_assets.ebx. Run: python -m unittest discover -s tools/assets/tests -t tools/assets

A small EBX file is built from the format as described (ebx.py); nothing of the game's own data is
used or committed. The real check is `gridnode.py assets ebx-check` on the user's install.
"""

from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from gridnode_assets import ebx  # noqa: E402
from gridnode_assets.ebx import Ebx, Field, Instance, Type  # noqa: E402

CLASS, STRUCT, ENUM, ARRAY = 0x35, 0x29, 0xC089, 0x41
F32, U32, BOOL, CSTR, GUID = 0xC13D, 0xC10D, 0xC0AD, 0x407D, 0xC15D


def sample() -> Ebx:
    types = [
        Type("DataContainer", 4, CLASS, 8, 0),
        Type("Vec3", 16, 0xD029, 0x10, 0, [Field("x", F32, 0, 0, 0), Field("y", F32, 0, 4, 4), Field("z", F32, 0, 8, 8)]),
        Type("Kind", 4, ENUM, 4, 0, [Field("Kind_A", 0, 0, 0, 0), Field("Kind_B", 0, 0, 1, 1)]),
        Type("array", 4, ARRAY, 4, 0, [Field("member", 0xD029, 1, 0, 0)]),
        Type("array", 4, ARRAY, 4, 0, [Field("member", CLASS, 0, 0, 0)]),
        Type("Thing", 16, CLASS, 0x50, 0, [
            Field("$", 0, 0, 8, 0),
            Field("Name", CSTR, 0, 0x8, 0x10),
            Field("Kind", ENUM, 2, 0xC, 0x18),
            Field("Pos", 0xD029, 1, 0x10, 0x20),
            Field("Points", ARRAY, 3, 0x20, 0x30),
            Field("Others", ARRAY, 4, 0x24, 0x38),
            Field("Next", CLASS, 0, 0x28, 0x40),
            Field("On", BOOL, 0, 0x2C, 0x48),
            Field("Id", GUID, 0, 0x30, 0x50),
            Field("Count", U32, 0, 0x40, 0x60),
        ]),
        Type("SubThing", 16, CLASS, 0x60, 0, [Field("$", 0, 5, 8, 0), Field("On", BOOL, 0, 0x50, 0x70)]),
    ]
    a = Instance(6, bytes(range(16)), {
        "Name": "Levels/Test", "Kind": "Kind_B", "Pos": {"x": 1.5, "y": -2.0, "z": 0.25},
        "Points": [{"x": 1.0, "y": 2.0, "z": 3.0}, {"x": 4.0, "y": 5.0, "z": 6.0}], "Others": ["#1", "import:0"],
        "Next": "#1", "On": True, "Id": "00112233445566778899aabbccddeeff", "Count": 7, "SubThing.On": False,
    })
    b = Instance(5, None, {"Name": "Levels/Test", "Kind": "Kind_A", "Pos": {"x": 0.0, "y": 0.0, "z": 0.0}, "Points": [],
                           "Others": [], "Next": None, "On": False, "Id": "00" * 16, "Count": 0})
    return Ebx(bytes(range(16, 32)), [(b"\x11" * 16, b"\x22" * 16)], types, [a, b], [])


class EbxTest(unittest.TestCase):
    def test_write_then_read(self):
        x = sample()
        x.runs = ebx.runs_of(x.instances)
        d = ebx.write(x)
        self.assertEqual(struct.unpack_from("<I", d, 0)[0], ebx.MAGIC)
        y = ebx.read(d)
        self.assertEqual([t.name for t in y.types], [t.name for t in x.types])
        self.assertEqual(y.instances[0].guid, x.instances[0].guid)
        self.assertIsNone(y.instances[1].guid)
        v = y.instances[0].values
        self.assertEqual(v["Name"], "Levels/Test")
        self.assertEqual(v["Kind"], "Kind_B")
        self.assertEqual(v["Pos"], {"x": 1.5, "y": -2.0, "z": 0.25})
        self.assertEqual(v["Points"][1], {"x": 4.0, "y": 5.0, "z": 6.0})
        self.assertEqual(v["Others"], ["#1", "import:0"])
        self.assertEqual((v["Next"], v["On"], v["SubThing.On"], v["Count"]), ("#1", True, False, 7))
        self.assertEqual(y.instances[1].values["Points"], [])
        self.assertEqual(ebx.write(y), d)  # read and written back: the same bytes

    def test_yaml_round_trip(self):
        d = ebx.write(sample())
        same, why = ebx.round_trip(d)
        self.assertTrue(same, why)
        text = ebx.to_yaml(ebx.read(d))
        self.assertIn("Others: ['#1', 'import:0']", text)
        self.assertIn("- [Count, 0xc10d, 0, 0x40, 0x60]", text)
        self.assertIn("SubThing.On: false", text)

    def test_floats_short_and_exact(self):
        self.assertEqual(ebx.short_f32(struct.unpack("<f", struct.pack("<f", 7.68))[0]), 7.68)
        x = sample()
        x.instances[0].values["Pos"]["x"] = {"f32": "0000c07f"}  # a NaN is kept as its bits
        d = ebx.write(x)
        self.assertEqual(ebx.read(d).instances[0].values["Pos"]["x"], {"f32": "0000c07f"})
        self.assertTrue(ebx.round_trip(d)[0])

    def test_unknown_field_and_bad_reference(self):
        x = sample()
        x.instances[1].values["Nope"] = 1
        with self.assertRaisesRegex(ebx.EbxError, "no field Nope"):
            ebx.write(x)
        x = sample()
        x.instances[1].values["Next"] = "#9"
        with self.assertRaisesRegex(ebx.EbxError, "a reference is"):
            ebx.write(x)

    def test_types_against_the_game(self):
        game = [
            {"ti": "v", "name": "Vec3", "category": "ValueType", "flags": 0xD029, "fields": [
                {"name": "x", "type": "f"}, {"name": "y", "type": "f"}, {"name": "z", "type": "f"}]},
            {"ti": "f", "name": "Float32", "category": "Primitive", "flags": F32},
            {"ti": "u", "name": "Uint32", "category": "Primitive", "flags": U32},
            {"ti": "k", "name": "Kind", "category": "Enum", "flags": ENUM, "values": [{"name": "Kind_A", "value": 0}, {"name": "Kind_B", "value": 2}]},
            {"ti": "d", "name": "DataContainer", "category": "Class", "flags": CLASS},
            {"ti": "t", "name": "Thing", "category": "Class", "flags": CLASS, "super": "d", "fields": [
                {"name": "Kind", "type": "k"}, {"name": "Pos", "type": "v"}, {"name": "Count", "type": "f"}]},
        ]
        dump = {"by_name": {t["name"]: t for t in game}, "by_ti": {t["ti"]: t for t in game}}
        errors, warnings = ebx.check_types(sample(), dump)
        self.assertIn("Kind.Kind_B = 1 here, 2 in the game", errors)
        self.assertTrue(any(e.startswith("Thing.Count: type code 16 here, 19") for e in errors), errors)
        self.assertIn("Thing.Name: not a field of the game's type (data only the pipeline uses?)", warnings)
        self.assertTrue(any(w.startswith("SubThing: not a type") for w in warnings), warnings)

    def test_negative_zero_and_short_arrays(self):
        # Seen in the user's install (2026-11-12): -0.0 must stay -0.0, and an array of 2-byte
        # elements still starts 4-aligned (its count is 4 bytes).
        x = sample()
        x.instances[0].values["Pos"]["y"] = -0.0
        x.types.append(Type("array", 4, ARRAY, 4, 0, [Field("member", 0xC0DD, 0, 0, 0)]))
        x.types[6].fields.append(Field("Shorts", ARRAY, len(x.types) - 1, 0x50, 0x80))
        x.types[6].fields.append(Field("More", ARRAY, len(x.types) - 1, 0x54, 0x88))
        x.types[6].size = 0x58
        x.instances[0].values["Points"] = [{"x": 1.0, "y": 2.0, "z": 3.0}]
        x.instances[0].values["Shorts"] = [1, 2, 3]  # 6 bytes: the next array would start 2-aligned
        x.instances[0].values["More"] = [4]
        x.instances[1].values["Points"] = []
        d = ebx.write(x)
        y = ebx.read(d)
        self.assertEqual(struct.pack("<f", y.instances[0].values["Pos"]["y"]), struct.pack("<f", -0.0))
        self.assertEqual(y.instances[0].values["Shorts"], [1, 2, 3])
        self.assertTrue(ebx.round_trip(d)[0])
        str_at, = struct.unpack_from("<I", d, 4)
        n_imp, = struct.unpack_from("<I", d, 12)
        n_runs, _, _, n_types, n_fields, name_len = struct.unpack_from("<6H", d, 0x10)
        p = 0x40 + 32 * n_imp + name_len + 16 * n_fields + 16 * n_types + 4 * n_runs
        p = (p + 15) // 16 * 16
        _, n_arrays, _ = struct.unpack_from("<3I", d, 0x1C)
        offsets = [struct.unpack_from("<3I", d, p + 12 * i)[0] for i in range(n_arrays)]
        self.assertTrue(all(o % 4 == 0 for o in offsets), offsets)

    def test_not_ebx(self):
        with self.assertRaises(ebx.EbxError):
            ebx.read(b"\0" * 0x40)


if __name__ == "__main__":
    unittest.main()
