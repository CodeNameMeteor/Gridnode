"""Fingerprints from the game itself (gridnode.py fingerprint).

A fingerprint's bytes used to be typed by hand from Ghidra's listing, which shows instructions, not
their encoding: an instruction has several encodings (mov rbx,r9 is 49 8B D9 or 4C 89 CB), and a
guess turned a feature off on the right game (MECMP, 2026-10-23). These functions read the bytes
from a dump of the running game (GridNode menu -> Game image) instead.

  check_gridnode(db, img)                 every fingerprint code entry against the image
  write_gridnode(path, results)           puts the image's bytes into fingerprint.yaml
  add_gridnode(path, db, img, ...)        a new code entry, its bytes from the image
  check_mecmp(mecmp_dir, img)             MECMP's src/game/fingerprint.cpp (slots and code)
  write_mecmp(mecmp_dir, results)         puts the image's bytes into it

x64 code has almost no absolute addresses (RIP-relative operands are relative), so the first bytes
of a function are position-independent; the game also loads at its preferred base.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass

from .annotations import Database
from .pe_image import PeImage


@dataclass
class Result:
    what: str          # the entry's name
    va: int            # where the bytes are read (at + skip)
    expected: bytes    # what the entry has now
    actual: bytes | None  # what the image has
    required: bool = True
    kind: str = "code"    # code | slot
    ok: bool = False
    detail: str = ""


def _hex(b: bytes | None) -> str:
    return b.hex(" ").upper() if b else "unreadable"


def check_gridnode(db: Database, img: PeImage) -> list[Result]:
    out = []
    for fc in db.fp_code:
        got = img.read(fc.at + fc.skip, len(fc.data))
        out.append(Result(fc.what, fc.at + fc.skip, fc.data, got, fc.required, ok=got == fc.data))
    for fs in db.fp_slots:
        out.append(_slot(img, fs.what, fs.slot, fs.fn))
    return out


def _slot(img: PeImage, what: str, slot: int, fn: int) -> Result:
    got = img.q(slot)
    r = Result(what, slot, fn.to_bytes(8, "little"), got.to_bytes(8, "little") if got is not None else None, kind="slot",
               ok=got == fn)
    if got is None:
        r.detail = "slot unreadable"
    elif not r.ok and not (img.base <= got < img.base + img.size):
        r.required = False  # a hook was in it when the dump was made, not a different build
        r.detail = f"slot {slot:#x} holds {got:#x}: outside the game, hooked when dumped (expected {fn:#x})"
    elif not r.ok:
        r.detail = f"slot {slot:#x} holds {got:#x}, expected {fn:#x}"
    return r


def report(results: list[Result], label: str) -> str:
    bad = [r for r in results if not r.ok]
    hooked = [r for r in bad if r.kind == "slot" and "hooked when dumped" in r.detail]
    o = [f"{label}: {len(results)} checked, {len(bad) - len(hooked)} differ" +
         (f", {len(hooked)} slot(s) hooked in the dump (not checked)" if hooked else "")]
    for r in bad:
        if r.kind == "slot":
            o.append(f"  slot {r.what}: {r.detail}")
        else:
            o.append(f"  code {r.what} @ {r.va:#x}{'' if r.required else ' (optional)'}:")
            o.append(f"      expected {_hex(r.expected)}")
            o.append(f"      image    {_hex(r.actual)}")
    return "\n".join(o) + "\n"


def _at_pattern(what: str) -> re.Pattern:
    # `at: NAME` as written in fingerprint.yaml (a name, or an address like 0x142a3ad50)
    return re.compile(r"(\{\s*at:\s*" + re.escape(what) + r"\s*,(?:[^}]*?))bytes:\s*\"([0-9A-Fa-f ]*)\"")


def write_gridnode(path: str, results: list[Result]) -> int:
    with open(path, encoding="utf-8") as f:
        text = f.read()
    changed = 0
    for r in results:
        if r.ok or r.kind != "code" or not r.actual:
            continue
        pat = _at_pattern(r.what)
        hits = [m for m in pat.finditer(text) if bytes.fromhex(m.group(2).replace(" ", "")) == r.expected]
        if len(hits) != 1:
            continue  # same name twice with the same bytes, or not found: left for a person
        m = hits[0]
        text = text[:m.start(2)] + _hex(r.actual) + text[m.end(2):]
        changed += 1
    if changed:
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    return changed


def add_gridnode(path: str, db: Database, img: PeImage, at: str, skip: int = 0, length: int = 16,
                 required: bool = False, note: str = "") -> str:
    va = db.symbol_va(at)
    if va is None:
        try:
            va = int(at, 0)
        except ValueError:
            raise ValueError(f"{at}: not an annotated function, global or code site, nor an address")
    if any(fc.what == at for fc in db.fp_code):
        raise ValueError(f"{at} already has a fingerprint code entry")
    if not 1 <= length <= 32:
        raise ValueError("--length must be 1..32")
    if not img.is_code(va):
        raise ValueError(f"{at} ({va:#x}) is not in a code section of the image")
    data = img.read(va + skip, length)
    if data is None:
        raise ValueError(f"{va + skip:#x} is not in the image")
    parts = [f"at: {at}"]
    if skip:
        parts.append(f"skip: {skip:#x}" if skip > 9 else f"skip: {skip}")
    parts.append(f'bytes: "{_hex(data)}"')
    if not required:
        parts.append("required: false")
    if note:
        parts.append('note: "' + note.replace('"', "'") + '"')
    line = "    - { " + ", ".join(parts) + " }\n"
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if not text.endswith("\n"):
        text += "\n"
    with open(path, "w", encoding="utf-8") as f:
        f.write(text + line)  # the code list is the last block of fingerprint.yaml
    return line


# ---- MECMP -----------------------------------------------------------------------------------------
_CONST = re.compile(r"constexpr\s+(?:uintptr_t|size_t|uint64_t|uint32_t)\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;")
_CONST_MULTI = re.compile(r"constexpr\s+size_t\s+((?:\w+\s*=\s*(?:0x[0-9A-Fa-f]+|\d+)\s*,\s*)+\w+\s*=\s*(?:0x[0-9A-Fa-f]+|\d+))\s*;")
_SLOT = re.compile(r"\{\s*\"([^\"]+)\"\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\}")
_CODE = re.compile(r"\{\s*\"([^\"]+)\"\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*\{([^}]*)\}\s*,\s*(\d+)\s*\}", re.S)


def _constants(*texts: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for t in texts:
        for m in _CONST.finditer(t):
            out[m.group(1)] = int(m.group(2), 0)
        for m in _CONST_MULTI.finditer(t):
            for part in m.group(1).split(","):
                k, _, v = part.partition("=")
                out[k.strip()] = int(v.strip(), 0)
    return out


def _value(tok: str, consts: dict[str, int]) -> int | None:
    try:
        return int(tok, 0)
    except ValueError:
        return consts.get(tok)


def _mecmp_files(mecmp_dir: str) -> tuple[str, str]:
    game = os.path.join(mecmp_dir, "src", "game")
    fp, eng = os.path.join(game, "fingerprint.cpp"), os.path.join(game, "engine.h")
    for p in (fp, eng):
        if not os.path.isfile(p):
            raise ValueError(f"{p} not found (is {mecmp_dir} a MECMP checkout?)")
    return fp, eng


def check_mecmp(mecmp_dir: str, img: PeImage) -> list[Result]:
    fp, eng = _mecmp_files(mecmp_dir)
    with open(fp, encoding="utf-8") as f:
        fp_text = f.read()
    with open(eng, encoding="utf-8") as f:
        eng_text = f.read()
    consts = _constants(eng_text, fp_text)
    out: list[Result] = []
    for m in _SLOT.finditer(fp_text):
        what, vt, off, fn = m.groups()
        vt_v, off_v, fn_v = _value(vt, consts), _value(off, consts), _value(fn, consts)
        if None in (vt_v, off_v, fn_v):
            out.append(Result(what, 0, b"", None, kind="slot", detail=f"constant not found ({vt}, {off}, {fn})"))
            continue
        out.append(_slot(img, what, vt_v + off_v, fn_v))
    for m in _CODE.finditer(fp_text):
        what, fn, skip, body, n = m.groups()
        fn_v, skip_v = _value(fn, consts), _value(skip, consts)
        try:
            data = bytes(int(x, 16) for x in body.replace(",", " ").split())
        except ValueError:
            continue
        if fn_v is None or skip_v is None:
            out.append(Result(what, 0, data, None, detail=f"constant not found ({fn}, {skip})"))
            continue
        if int(n) != len(data):
            out.append(Result(what, fn_v + skip_v, data, None, detail=f"count {n} but {len(data)} bytes listed"))
            continue
        got = img.read(fn_v + skip_v, len(data))
        out.append(Result(what, fn_v + skip_v, data, got, ok=got == data))
    return out


def write_mecmp(mecmp_dir: str, results: list[Result]) -> int:
    fp, _ = _mecmp_files(mecmp_dir)
    with open(fp, encoding="utf-8") as f:
        text = f.read()
    changed = 0
    for r in results:
        if r.ok or r.kind != "code" or not r.actual:
            continue
        hits = [m for m in _CODE.finditer(text) if m.group(1) == r.what]
        if len(hits) != 1:
            continue
        m = hits[0]
        new = ", ".join(f"0x{b:02X}" for b in r.actual)
        text = text[:m.start(4)] + new + text[m.end(4):]
        changed += 1
    if changed:
        with open(fp, "w", encoding="utf-8") as f:
            f.write(text)
    return changed
