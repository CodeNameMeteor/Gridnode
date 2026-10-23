"""Checks the annotations against the game image itself: every hook slot and annotated virtual
holds the function the annotations say, every fingerprint byte matches, every function address
lies in code. Works on a memory dump of the game saved as a PE (e.g. a Scylla dump)."""

from __future__ import annotations

from .annotations import Database, Problems
from .pe_image import PeImage


def check(db: Database, img: PeImage) -> tuple[Problems, int]:
    p = Problems()
    checked = 0

    def slot(where: str, addr: int, expect: int):
        nonlocal checked
        checked += 1
        got = img.q(addr)
        if got is not None and got != expect and not (img.base <= got < img.base + img.size):
            # A pointer outside the game: a hook was in the slot when the dump was made (a dump from an older
            # GridNode, or another mod's hook). Not a different build.
            p.warn(where, f"slot {addr:#x} holds {got:#x} (outside the game: hooked when dumped), expected {expect:#x}")
            return
        if got != expect:
            p.error(where, f"slot {addr:#x} holds {got:#x}, expected {expect:#x}" if got is not None
                    else f"slot {addr:#x} is not in the image")

    for h in db.hooks.values():
        slot(f"{h.meta.file} hook {h.name}", h.slot, h.original)
        if not img.is_code(h.original):
            p.error(f"{h.meta.file} hook {h.name}", f"original {h.original:#x} is not in a code section")
    for c in db.classes.values():
        if c.vtable is None:
            continue
        for v in c.virtuals:
            if v.va is not None:
                slot(f"{c.meta.file} {c.name}::{v.name}", c.vtable + v.offset, v.va)
    for f in db.functions.values():
        checked += 1
        if not img.is_code(f.va):
            p.error(f"{f.meta.file} function {f.name}", f"{f.va:#x} is not in a code section")
    for c in db.code.values():
        checked += 1
        if not img.is_code(c.va):
            p.error(f"{c.meta.file} code {c.name}", f"{c.va:#x} is not in a code section")
    for g in db.globals.values():
        checked += 1
        if img.read(g.va, 8) is None:
            p.error(f"{g.meta.file} global {g.name}", f"{g.va:#x} is not in the image")
    for fc in db.fp_code:
        checked += 1
        got = img.read(fc.at + fc.skip, len(fc.data))
        if got != fc.data:
            msg = f"bytes at {fc.at + fc.skip:#x} are {got.hex(' ') if got else 'unreadable'}, expected {fc.data.hex(' ')}"
            (p.error if fc.required else p.warn)(f"fingerprint code {fc.what}", msg)
    for fs in db.fp_slots:
        slot(f"fingerprint slot {fs.what}", fs.slot, fs.fn)
    return p, checked
