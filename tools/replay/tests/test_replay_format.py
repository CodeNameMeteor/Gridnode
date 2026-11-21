"""Round trips for tools/replay/replay_format.py (no game data)."""

import math
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import replay_format as rf  # noqa: E402


def quat(axis, angle):
    n = math.sqrt(sum(c * c for c in axis))
    s = math.sin(angle / 2) / n
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle / 2))


class BitsTest(unittest.TestCase):
    def test_msb_first(self):
        w = rf.BitWriter()
        w.bits(0b101, 3)
        w.bits(0x1F, 5)
        self.assertEqual(w.data(), bytes([0b10111111]))
        r = rf.BitReader(w.data())
        self.assertEqual(r.bits(3), 0b101)
        self.assertEqual(r.bits(5), 0x1F)

    def test_rewind(self):
        w = rf.BitWriter()
        w.bits(0xABCD, 16)
        w.bits(0b111, 3)
        w.rewind(16)
        w.bits(0b010, 3)
        r = rf.BitReader(w.data())
        self.assertEqual(r.bits(16), 0xABCD)
        self.assertEqual(r.bits(3), 0b010)

    def test_quat_is_34_bits_and_close(self):
        for q in (quat((0, 1, 0), 0.3), quat((1, 1, 0), -2.0), quat((0.2, -0.5, 1), 3.0), (0.0, 0.0, 0.0, 1.0)):
            w = rf.BitWriter()
            rf.write_quat(w, q)
            self.assertEqual(w.pos, 34)
            got = rf.read_quat(rf.BitReader(w.data()))
            dot = abs(sum(a * b for a, b in zip(q, got)))
            self.assertGreater(dot, 0.999)

    def test_delta(self):
        w = rf.BitWriter()
        rf.write_delta(w, (1.25, 0.0, -4.9))
        self.assertEqual(w.pos, 11 + 1 + 11)  # present bit, sign, 9 bits; an absent axis is its present bit
        d = rf.read_delta(rf.BitReader(w.data()))
        self.assertAlmostEqual(d[0], 1.25, delta=5.0 / 511)
        self.assertEqual(d[1], 0.0)
        self.assertAlmostEqual(d[2], -4.9, delta=5.0 / 511)


class RecordingTest(unittest.TestCase):
    def setUp(self):
        self.h = rf.Header(bone_ids=[0x10, 0x11, 0x12], parent_ids=[0xFFFFFFFF, 0, 1], translation_bones=[0, 1], setup_hash=0x1234)

    def frames(self, n, jump_at=None):
        out = []
        for i in range(n):
            x = 0.1 * i + (20.0 if jump_at is not None and i >= jump_at else 0.0)
            out.append(rf.Keyframe(time=i / 30.0, full=False, snap=False,
                                   translations=[(x, 1.0, -x), (0.0, 0.9, 0.0), (0.0, 0.0, 0.0)],
                                   rotations=[quat((0, 1, 0), 0.05 * i), quat((1, 0, 0), 0.1), (0.0, 0.0, 0.0, 1.0)]))
        return out

    def test_header(self):
        w = rf.BitWriter()
        rf.write_header(w, self.h)
        h = rf.read_header(rf.BitReader(w.data()))
        self.assertEqual((h.bone_ids, h.parent_ids, h.translation_bones, h.setup_hash), (self.h.bone_ids, self.h.parent_ids, self.h.translation_bones, 0x1234))

    def test_wrong_version(self):
        w = rf.BitWriter()
        w.bits(4, 32)
        with self.assertRaises(ValueError):
            rf.read_header(rf.BitReader(w.data() + bytes(16)))

    def test_round_trip_with_full_frames(self):
        src = self.frames(65, jump_at=40)
        h, got = rf.read_recording(rf.write_recording(self.h, src))
        self.assertEqual(len(got), len(src))
        self.assertTrue(got[0].full and got[30].full and got[60].full)
        self.assertTrue(got[40].full)  # a 20 m jump does not fit a delta
        self.assertFalse(got[1].full)
        for a, b in zip(src, got):
            self.assertAlmostEqual(a.time, b.time, places=5)
            for ta, tb in zip(a.translations[:2], b.translations[:2]):
                for ca, cb in zip(ta, tb):
                    self.assertAlmostEqual(ca, cb, delta=0.02)


if __name__ == "__main__":
    unittest.main()
