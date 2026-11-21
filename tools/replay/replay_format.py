#!/usr/bin/env python3
"""The game's replay recordings (version 5): read and write them.

Layout read in Ghidra on 2026-11-20 (docs/research/engine-2026-11-15.md, blocks H and I; db/annotations/replay.yaml):
ReplayPlayback_ReadRecordingHeader 0x14394e560, ReplayPlayback_DecodeKeyframe 0x14394e390, BitReader_readQuat
0x142a9d9a0, BitReader_readDeltaVec3 0x142a9d7d0, ReplayKeyframe_Write 0x143951450.

The bit stream is MSB-first in big-endian 32-bit words, which is the same as MSB-first over the bytes in order.

Header: u32 version (5), u32 (read and ignored: a setup hash), u32 bone count N, N x u32 (bone ids), N x u32
(parent ids), u8 translation-bone count T, T x u8 (indices of the bones whose translation is recorded).

Keyframe: f32 time; 2 flag bits (bit 0: full, bit 1: no interpolation into this frame); then per bone:
  full:  translation bones 3 x f32 (others take the bind translation), then a 34-bit quaternion
  delta: translation bones a delta vector (per axis: 1 bit present, then 1 sign bit + 9 bits over 511 times 5.0 m),
         then a 34-bit quaternion
Quaternion (34 bits): 1 bit w sign, then x, y, z each 1 sign bit + 10 bits over 1023; w = sqrt(1 - x^2 - y^2 - z^2).
The game writes a full keyframe every 30 frames and whenever a translation moved more than 5 m since the last one.

Not yet checked against a recording taken from the game: the round trip below is GridNode's own.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from dataclasses import dataclass, field
from typing import List, Optional, Sequence, Tuple

VERSION = 5
QUAT_BITS = 11      # per component: sign + 10 bits
DELTA_BITS = 10     # per axis: sign + 9 bits
DELTA_SCALE = 5.0   # metres
FULL_EVERY = 30

Vec3 = Tuple[float, float, float]
Quat = Tuple[float, float, float, float]  # x, y, z, w


class BitReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0  # in bits
        self.overrun = False

    def bits(self, n: int) -> int:
        v = 0
        for _ in range(n):
            byte = self.pos >> 3
            if byte >= len(self.data):
                self.overrun = True
                bit = 0
            else:
                bit = (self.data[byte] >> (7 - (self.pos & 7))) & 1
            v = (v << 1) | bit
            self.pos += 1
        return v

    def f32(self) -> float:
        return struct.unpack(">f", self.bits(32).to_bytes(4, "big"))[0]


class BitWriter:
    def __init__(self) -> None:
        self.out = bytearray()
        self.pos = 0

    def bits(self, value: int, n: int) -> None:
        for i in range(n - 1, -1, -1):
            if (self.pos >> 3) >= len(self.out):
                self.out.append(0)
            if (value >> i) & 1:
                self.out[self.pos >> 3] |= 1 << (7 - (self.pos & 7))
            self.pos += 1

    def f32(self, v: float) -> None:
        self.bits(int.from_bytes(struct.pack(">f", v), "big"), 32)

    def rewind(self, pos: int) -> None:
        """As BitWriter_rewind: drop everything after bit `pos`."""
        self.pos = pos
        del self.out[(pos + 7) >> 3:]
        if pos & 7:
            self.out[-1] &= (0xFF << (8 - (pos & 7))) & 0xFF

    def data(self) -> bytes:
        return bytes(self.out)


def _unit(r: BitReader, bits: int) -> float:
    return r.bits(bits) / float((1 << bits) - 1)


def _put_unit(w: BitWriter, v: float, bits: int) -> None:
    m = (1 << bits) - 1
    w.bits(int(m * min(max(abs(v), 0.0), 1.0) + 0.5), bits)


def read_quat(r: BitReader, bits: int = QUAT_BITS) -> Quat:
    w_neg = r.bits(1)
    comps = []
    for _ in range(3):
        neg = r.bits(1)
        mag = _unit(r, bits - 1)
        comps.append(-mag if neg else mag)
    x, y, z = comps
    s = min(1.0, x * x + y * y + z * z)
    w = math.sqrt(max(0.0, 1.0 - s))
    if w_neg:
        w = -w
    if w == 0.0:
        n = math.sqrt(x * x + y * y + z * z) or 1.0
        x, y, z = x / n, y / n, z / n
    return (x, y, z, w)


def write_quat(wr: BitWriter, q: Quat, bits: int = QUAT_BITS) -> None:
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    wr.bits(1 if w < 0 else 0, 1)
    for c in (x, y, z):
        wr.bits(1 if c < 0 else 0, 1)
        _put_unit(wr, c, bits - 1)


def read_delta(r: BitReader, bits: int = DELTA_BITS, scale: float = DELTA_SCALE) -> Vec3:
    out = []
    for _ in range(3):
        if r.bits(1):
            neg = r.bits(1)
            mag = _unit(r, bits - 1) * scale
            out.append(-mag if neg else mag)
        else:
            out.append(0.0)
    return (out[0], out[1], out[2])


def write_delta(wr: BitWriter, d: Vec3, bits: int = DELTA_BITS, scale: float = DELTA_SCALE) -> None:
    for c in d:
        if abs(c) < 1e-6:
            wr.bits(0, 1)
            continue
        wr.bits(1, 1)
        wr.bits(1 if c < 0 else 0, 1)
        _put_unit(wr, min(abs(c), scale) / scale, bits - 1)


@dataclass
class Header:
    bone_ids: List[int]
    parent_ids: List[int]
    translation_bones: List[int]
    setup_hash: int = 0
    version: int = VERSION


@dataclass
class Keyframe:
    time: float
    full: bool
    snap: bool
    translations: List[Vec3]
    rotations: List[Quat]


def read_header(r: BitReader) -> Header:
    version = r.bits(32)
    if version != VERSION:
        raise ValueError(f"recording version {version}, not {VERSION}")
    setup_hash = r.bits(32)
    n = r.bits(32)
    if n > 4096:
        raise ValueError(f"{n} bones: not a recording")
    ids = [r.bits(32) for _ in range(n)]
    parents = [r.bits(32) for _ in range(n)]
    t = r.bits(8)
    trans = [r.bits(8) for _ in range(t)]
    if r.overrun:
        raise ValueError("the header runs past the end of the data")
    return Header(ids, parents, trans, setup_hash, version)


def write_header(w: BitWriter, h: Header) -> None:
    w.bits(h.version, 32)
    w.bits(h.setup_hash, 32)
    w.bits(len(h.bone_ids), 32)
    for v in h.bone_ids:
        w.bits(v, 32)
    for v in h.parent_ids:
        w.bits(v & 0xFFFFFFFF, 32)
    w.bits(len(h.translation_bones), 8)
    for v in h.translation_bones:
        w.bits(v, 8)


def read_keyframe(r: BitReader, h: Header, previous: Optional[Keyframe], bind: Optional[Sequence[Vec3]] = None) -> Keyframe:
    time = r.f32()
    flags = r.bits(2)
    full = bool(flags & 1)
    n = len(h.bone_ids)
    has_t = set(h.translation_bones)
    trans: List[Vec3] = []
    rots: List[Quat] = []
    if not full and previous is None:
        raise ValueError("a delta keyframe with no keyframe before it")
    for i in range(n):
        if full:
            t = (r.f32(), r.f32(), r.f32()) if i in has_t else (tuple(bind[i]) if bind else (0.0, 0.0, 0.0))
        else:
            base = previous.translations[i]
            d = read_delta(r) if i in has_t else (0.0, 0.0, 0.0)
            t = (base[0] + d[0], base[1] + d[1], base[2] + d[2])
        trans.append(t)  # type: ignore[arg-type]
        rots.append(read_quat(r))
    return Keyframe(time, full, bool(flags & 2), trans, rots)


def write_keyframe(w: BitWriter, h: Header, k: Keyframe, previous: Optional[Keyframe], force_full: bool = False) -> bool:
    """Writes k; a delta that does not fit (> 5 m on an axis) is rewound and written full, as the game does.
    Returns whether the frame went out full."""
    has_t = set(h.translation_bones)
    w.f32(k.time)
    after_time = w.pos
    full = force_full or previous is None
    while True:
        w.bits((1 if full else 0) | (2 if k.snap else 0), 2)
        ok = True
        for i in range(len(h.bone_ids)):
            if i in has_t:
                if full:
                    for c in k.translations[i]:
                        w.f32(c)
                else:
                    base = previous.translations[i]  # type: ignore[union-attr]
                    d = tuple(k.translations[i][j] - base[j] for j in range(3))
                    if any(abs(c) > DELTA_SCALE for c in d):
                        ok = False
                        break
                    write_delta(w, d)  # type: ignore[arg-type]
            write_quat(w, k.rotations[i])
        if ok:
            return full
        w.rewind(after_time)
        full = True


def write_recording(h: Header, frames: Sequence[Keyframe]) -> bytes:
    """Header and frames; a full keyframe every 30 frames and when a delta does not fit.
    Each delta is taken from what the reader will rebuild, so rounding does not add up."""
    w = BitWriter()
    write_header(w, h)
    rebuilt: Optional[Keyframe] = None
    for i, k in enumerate(frames):
        full = write_keyframe(w, h, k, rebuilt, force_full=(i % FULL_EVERY == 0))
        rebuilt = _rebuild(h, k, rebuilt, full)
    return w.data()


def _rebuild(h: Header, k: Keyframe, previous: Optional[Keyframe], full: bool) -> Keyframe:
    w = BitWriter()
    write_keyframe(w, h, k, previous, force_full=full)
    return read_keyframe(BitReader(w.data()), h, previous)


def read_recording(data: bytes, max_frames: int = 1 << 20) -> Tuple[Header, List[Keyframe]]:
    r = BitReader(data)
    h = read_header(r)
    frames: List[Keyframe] = []
    prev: Optional[Keyframe] = None
    total_bits = len(data) * 8
    while len(frames) < max_frames and total_bits - r.pos >= 34:
        save = r.pos
        k = read_keyframe(r, h, prev)
        if r.overrun:
            r.pos = save
            break
        frames.append(k)
        prev = k
    return h, frames


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="The game's replay recordings (version 5).")
    sub = ap.add_subparsers(dest="cmd", required=True)
    info = sub.add_parser("info", help="header and keyframe count")
    info.add_argument("file")
    dump = sub.add_parser("dump", help="the first frames, bone by bone")
    dump.add_argument("file")
    dump.add_argument("--frames", type=int, default=3)
    a = ap.parse_args(argv)
    data = open(a.file, "rb").read()
    h, frames = read_recording(data, max_frames=1 << 20 if a.cmd == "info" else a.frames)
    print(f"version {h.version}, {len(h.bone_ids)} bones, {len(h.translation_bones)} with translation, setup hash 0x{h.setup_hash:08x}")
    if a.cmd == "info":
        fulls = sum(1 for k in frames if k.full)
        span = (frames[-1].time - frames[0].time) if frames else 0.0
        print(f"{len(frames)} keyframes ({fulls} full), {span:.2f} s")
        return 0
    for k in frames:
        print(f"t={k.time:.3f} {'full' if k.full else 'delta'}{' snap' if k.snap else ''}")
        for i, (t, q) in enumerate(zip(k.translations, k.rotations)):
            print(f"  bone {i:3d} id {h.bone_ids[i]:08x} t=({t[0]:.3f}, {t[1]:.3f}, {t[2]:.3f}) q=({q[0]:.3f}, {q[1]:.3f}, {q[2]:.3f}, {q[3]:.3f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
