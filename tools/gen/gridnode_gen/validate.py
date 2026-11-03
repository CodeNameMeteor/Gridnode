"""Cross-checks between the annotations and a reflected dump."""

from __future__ import annotations

from .annotations import Database, Problems, size_hint
from .ctypes_ import PRIMITIVES, TypeRef
from .reflected import Reflected

REALM_OF_CREATOR = {0: "client", 1: "server"}


def known_type(name: str, db: Database, r: Reflected | None) -> bool:
    if name in PRIMITIVES or name in db.structs:
        return True
    return r is not None and name in r.by_name


def check(db: Database, r: Reflected | None) -> Problems:
    p = Problems()
    if r is None:
        return p  # type names and everything else that needs the reflected layer are checked with a dump
    _unknown_types(db, r, p, warn_only=False)

    for name, s in db.structs.items():
        if name in r.by_name:
            p.error(s.meta.file, f"struct {name} has the same name as a reflected type: rename it")

    for name, c in db.classes.items():
        w = f"{c.meta.file} class {name}"
        t = r.by_name.get(name)
        if t is None:
            p.error(w, "no reflected type with this name")
            continue
        if t["category"] != "Class":
            p.error(w, f"reflected type is a {t['category']}, not a Class")
            continue
        if c.size is not None and c.size != t["size"]:
            p.error(w, f"size {c.size:#x} but the reflected size is {t['size']:#x}")
        vts = [int(v, 16) for v in t.get("vtables", [])]
        if c.vtable is not None and vts and c.vtable not in vts:
            p.error(w, f"vtable {c.vtable:#x} is not among the reflected vtables {[hex(v) for v in vts]}")
        elif c.vtable is not None and not vts:  # the dump found none for this class: nothing to contradict
            p.warn(w, f"vtable {c.vtable:#x}: the dump found no vtable for this class to check it against")
        realms = {REALM_OF_CREATOR.get(cr["realm"]) for cr in t.get("creators", [])}
        if c.realm in ("client", "server") and realms and c.realm not in realms:
            p.error(w, f"realm {c.realm} but the class is only created in {sorted(x for x in realms if x)}")
        sup = r.super_of(t)
        base_size = sup["size"] if sup else 0
        fields = sorted(((f["offset"], f["name"]) for f in t.get("fields", [])))
        for m in c.members:
            msize = size_hint(m.type, db, r)
            if m.offset + msize > t["size"]:
                p.error(w, f"member {m.name} (+{m.offset:#x}, {msize:#x} bytes) runs past the class size {t['size']:#x}")
            if m.offset < base_size:
                owner = _owner(r, t, m.offset)
                p.warn(w, f"member {m.name} (+{m.offset:#x}) lies in base class {owner}: consider annotating it there")
            for off, fname in fields:
                if off == m.offset:
                    p.error(w, f"member {m.name} (+{m.offset:#x}) is the reflected field {fname}: do not annotate reflected fields")

    for ft in db.fp_types:
        t = r.by_name.get(ft.name)
        if t is None:
            p.error("fingerprint", f"type {ft.name} is not in the dump")
        elif t["_ti"] != ft.ti:
            p.error("fingerprint", f"type {ft.name} is at {t['ti']} in the dump, the fingerprint says {ft.ti:#x}")
    return p


def _owner(r: Reflected, t: dict, offset: int) -> str:
    chain = r.chain(t)
    owner = chain[-1]["displayName"]
    for base, sub in zip(chain[1:], chain):
        if base["size"] <= offset < sub["size"]:
            return sub["displayName"]
    return owner


def _named(t: TypeRef | None):
    return t.named_type() if t is not None else None


def _unknown_types(db: Database, r: Reflected | None, p: Problems, warn_only: bool):
    def need(name: str | None, where: str):
        if name and not known_type(name, db, r):
            (p.warn if warn_only else p.error)(where, f"unknown type {name!r}" + (" (no dump given)" if warn_only else ""))

    for c in db.classes.values():
        for m in c.members:
            need(_named(m.type), f"{c.meta.file} {c.name}.{m.name}")
        for v in c.virtuals:
            for n in v.signature.named_types():
                need(n, f"{c.meta.file} {c.name}::{v.name}")
        for pr in c.properties:
            need(_named(pr.type), f"{c.meta.file} {c.name} property {pr.name}")
    for s in db.structs.values():
        for m in s.members:
            need(_named(m.type), f"{s.meta.file} {s.name}.{m.name}")
    for g in db.globals.values():
        need(_named(g.type), f"{g.meta.file} global {g.name}")
    for f in db.functions.values():
        for n in (f.signature.named_types() if f.signature else ()):
            need(n, f"{f.meta.file} function {f.name}")
    for h in db.hooks.values():
        if h.signature:
            for n in h.signature.named_types():
                need(n, f"{h.meta.file} hook {h.name}")
