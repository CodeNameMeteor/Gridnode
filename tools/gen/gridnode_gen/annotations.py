"""Loading and checking db/annotations/*.yaml (format: docs/annotations.md)."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Any

from .ctypes_ import Signature, TypeError_, TypeRef, djb2_xor, parse_signature, parse_type, PRIMITIVES

try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None

CONFIDENCE = ("confirmed", "inferred", "hypothesis")
REALMS = ("client", "server", "any")
HOOK_KINDS = ("vtable-slot", "creator-slot", "pointer")
SECTIONS = ("defaults", "classes", "structs", "globals", "functions", "hooks", "code", "constants", "events", "fingerprint")


@dataclass
class Meta:
    file: str
    confidence: str = "confirmed"
    sources: list[str] = field(default_factory=list)
    note: str = ""


@dataclass
class Member:
    offset: int
    name: str
    type: TypeRef
    meta: Meta


@dataclass
class Virtual:
    offset: int
    name: str
    va: int | None
    signature: Signature
    meta: Meta


@dataclass
class Property:
    name: str
    hash: int
    type: TypeRef | None
    meta: Meta


@dataclass
class ClassAnn:
    name: str
    realm: str
    vtable: int | None
    size: int | None
    members: list[Member]
    virtuals: list[Virtual]
    properties: list[Property]
    meta: Meta


@dataclass
class StructAnn:
    name: str
    size: int
    align: int
    opaque: bool
    members: list[Member]
    meta: Meta


@dataclass
class Global:
    name: str
    va: int
    type: TypeRef
    meta: Meta


@dataclass
class Function:
    name: str
    va: int
    signature: Signature | None  # None: not known yet (a name only; no function pointer type)
    meta: Meta


@dataclass
class Hook:
    name: str
    kind: str
    slot: int          # address of the pointer that gets swapped
    original: int      # what it holds in this build
    vtable: int | None
    offset: int | None
    signature: Signature | None
    meta: Meta


@dataclass
class CodeSite:
    name: str
    va: int
    meta: Meta


@dataclass
class Constant:
    name: str
    value: Any
    meta: Meta


@dataclass
class EventAnn:
    """An entity event (or any name Frostbite keeps only as a djb2-xor hash), named by our research."""
    name: str
    hash: int
    sent_to: list[str]   # classes seen receiving it (documentation; not checked against the dump)
    meta: Meta


@dataclass
class FpType:
    name: str
    ti: int


@dataclass
class FpSlot:
    what: str
    slot: int
    fn: int


@dataclass
class FpCode:
    what: str
    at: int
    skip: int
    data: bytes
    required: bool
    note: str


@dataclass
class Database:
    classes: dict[str, ClassAnn] = field(default_factory=dict)
    structs: dict[str, StructAnn] = field(default_factory=dict)
    globals: dict[str, Global] = field(default_factory=dict)
    functions: dict[str, Function] = field(default_factory=dict)
    hooks: dict[str, Hook] = field(default_factory=dict)
    code: dict[str, CodeSite] = field(default_factory=dict)
    constants: dict[str, Constant] = field(default_factory=dict)
    events: dict[str, EventAnn] = field(default_factory=dict)
    fp_types: list[FpType] = field(default_factory=list)
    fp_slots: list[FpSlot] = field(default_factory=list)
    fp_code: list[FpCode] = field(default_factory=list)
    files: list[str] = field(default_factory=list)

    def symbol_va(self, name: str) -> int | None:
        for table in (self.functions, self.globals, self.code):
            if name in table:
                return table[name].va
        return None


class Problems:
    """Errors and warnings with their source file."""

    def __init__(self):
        self.errors: list[str] = []
        self.warnings: list[str] = []

    def error(self, where: str, msg: str):
        self.errors.append(f"{where}: {msg}")

    def warn(self, where: str, msg: str):
        self.warnings.append(f"{where}: {msg}")

    def extend(self, other: "Problems"):
        self.errors += other.errors
        self.warnings += other.warnings


def _int(v: Any, what: str) -> int:
    if isinstance(v, bool) or not isinstance(v, int):
        if isinstance(v, str):
            try:
                return int(v, 0)
            except ValueError:
                pass
        raise ValueError(f"{what} must be an integer (got {v!r})")
    return v


def _meta(item: dict, defaults: dict, file: str, p: Problems, where: str) -> Meta:
    conf = item.get("confidence", defaults.get("confidence", "confirmed"))
    if conf not in CONFIDENCE:
        p.error(where, f"confidence must be one of {CONFIDENCE} (got {conf!r})")
        conf = "hypothesis"
    sources = item.get("sources", defaults.get("sources", []))
    if isinstance(sources, str):
        sources = [sources]
    note = item.get("note", "") or ""
    return Meta(file=file, confidence=conf, sources=list(sources), note=str(note).strip())


def _check_keys(item: dict, allowed: set[str], p: Problems, where: str):
    for k in item:
        if k not in allowed:
            p.error(where, f"unknown key {k!r} (allowed: {', '.join(sorted(allowed))})")


def _members(items: list, defaults: dict, file: str, p: Problems, where: str) -> list[Member]:
    out = []
    names = set()
    for i, m in enumerate(items or []):
        w = f"{where} member {m.get('name', i) if isinstance(m, dict) else i}"
        if not isinstance(m, dict):
            p.error(w, "must be a mapping")
            continue
        _check_keys(m, {"offset", "name", "type", "note", "confidence", "sources"}, p, w)
        try:
            off = _int(m.get("offset"), "offset")
            t = parse_type(m.get("type", ""))
        except (ValueError, TypeError_) as e:
            p.error(w, str(e))
            continue
        name = m.get("name")
        if not isinstance(name, str) or not name.isidentifier():
            p.error(w, "name must be an identifier")
            continue
        if name in names:
            p.error(w, f"duplicate member name {name!r}")
        names.add(name)
        out.append(Member(off, name, t, _meta(m, defaults, file, p, w)))
    offs = sorted(out, key=lambda m: m.offset)
    for a, b in zip(offs, offs[1:]):
        if a.offset == b.offset:
            p.error(where, f"members {a.name} and {b.name} share offset {a.offset:#x}")
    return out


def _sig(text: Any, p: Problems, where: str) -> Signature | None:
    if text is None:
        return None
    try:
        return parse_signature(text)
    except TypeError_ as e:
        p.error(where, str(e))
        return None


def load(directory: str) -> tuple[Database, Problems]:
    if yaml is None:
        raise SystemExit("gridnode-gen needs PyYAML: pip install pyyaml")
    db, p = Database(), Problems()
    files = sorted(f for f in os.listdir(directory) if f.endswith((".yaml", ".yml")))
    for fn in files:
        path = os.path.join(directory, fn)
        db.files.append(fn)
        with open(path, encoding="utf-8") as f:
            try:
                doc = yaml.safe_load(f) or {}
            except yaml.YAMLError as e:
                p.error(fn, f"YAML error: {e}")
                continue
        if not isinstance(doc, dict):
            p.error(fn, "top level must be a mapping")
            continue
        _check_keys(doc, set(SECTIONS), p, fn)
        d = doc.get("defaults", {}) or {}
        _load_file(doc, d, fn, db, p)
    _resolve_fingerprint(db, p)
    _check_symbols(db, p)
    return db, p


def _load_file(doc: dict, d: dict, fn: str, db: Database, p: Problems):
    for c in doc.get("classes", []) or []:
        w = f"{fn} class {c.get('name', '?')}"
        _check_keys(c, {"name", "realm", "vtable", "size", "members", "virtuals", "properties", "note", "confidence", "sources"}, p, w)
        name = c.get("name")
        if not isinstance(name, str) or not name.isidentifier():
            p.error(w, "name must be an identifier")
            continue
        if name in db.classes:
            p.error(w, f"class {name} is already annotated in {db.classes[name].meta.file}")
            continue
        realm = c.get("realm", "any")
        if realm not in REALMS:
            p.error(w, f"realm must be one of {REALMS}")
        virtuals, vnames = [], set()
        for v in c.get("virtuals", []) or []:
            vw = f"{w} virtual {v.get('name', '?')}"
            _check_keys(v, {"offset", "name", "va", "signature", "note", "confidence", "sources"}, p, vw)
            try:
                off = _int(v.get("offset"), "offset")
                va = _int(v["va"], "va") if "va" in v else None
            except ValueError as e:
                p.error(vw, str(e))
                continue
            if off % 8:
                p.error(vw, f"vtable offset {off:#x} is not a multiple of 8")
            sig = _sig(v.get("signature"), p, vw)
            if sig is None:
                p.error(vw, "signature is required")
                continue
            if v.get("name") in vnames:
                p.error(vw, "duplicate virtual name")
            vnames.add(v.get("name"))
            virtuals.append(Virtual(off, v.get("name"), va, sig, _meta(v, d, fn, p, vw)))
        props = []
        for pr in c.get("properties", []) or []:
            pw = f"{w} property {pr.get('name', '?')}"
            _check_keys(pr, {"name", "hash", "type", "note", "confidence", "sources"}, p, pw)
            h = djb2_xor(pr.get("name", ""))
            if "hash" in pr:
                try:
                    stated = _int(pr["hash"], "hash")
                except ValueError as e:
                    p.error(pw, str(e))
                    stated = h
                if stated != h:
                    p.error(pw, f"hash {stated:#010x} does not match djb2-xor({pr.get('name')!r}) = {h:#010x}")
            t = None
            if "type" in pr:
                try:
                    t = parse_type(pr["type"])
                except TypeError_ as e:
                    p.error(pw, str(e))
            props.append(Property(pr.get("name"), h, t, _meta(pr, d, fn, p, pw)))
        try:
            vt = _int(c["vtable"], "vtable") if "vtable" in c else None
            size = _int(c["size"], "size") if "size" in c else None
        except ValueError as e:
            p.error(w, str(e))
            vt = size = None
        db.classes[name] = ClassAnn(name, realm, vt, size, _members(c.get("members"), d, fn, p, w), virtuals, props,
                                    _meta(c, d, fn, p, w))

    for s in doc.get("structs", []) or []:
        w = f"{fn} struct {s.get('name', '?')}"
        _check_keys(s, {"name", "size", "align", "opaque", "members", "note", "confidence", "sources"}, p, w)
        name = s.get("name")
        if not isinstance(name, str) or not name.isidentifier():
            p.error(w, "name must be an identifier")
            continue
        if name in db.structs:
            p.error(w, f"struct {name} is already defined in {db.structs[name].meta.file}")
            continue
        try:
            size = _int(s.get("size"), "size")
            align = _int(s.get("align", 8), "align")
        except ValueError as e:
            p.error(w, str(e))
            continue
        if align not in (1, 2, 4, 8, 16, 32, 64):
            p.error(w, "align must be a power of two up to 64")
        if size % align:
            p.error(w, f"size {size:#x} is not a multiple of align {align}")
        db.structs[name] = StructAnn(name, size, align, bool(s.get("opaque", False)),
                                     _members(s.get("members"), d, fn, p, w), _meta(s, d, fn, p, w))

    for g in doc.get("globals", []) or []:
        w = f"{fn} global {g.get('name', '?')}"
        _check_keys(g, {"name", "va", "type", "note", "confidence", "sources"}, p, w)
        try:
            db.globals[g["name"]] = Global(g["name"], _int(g.get("va"), "va"), parse_type(g.get("type", "void*")),
                                           _meta(g, d, fn, p, w))
        except (ValueError, TypeError_, KeyError) as e:
            p.error(w, str(e))

    for f_ in doc.get("functions", []) or []:
        w = f"{fn} function {f_.get('name', '?')}"
        _check_keys(f_, {"name", "va", "signature", "note", "confidence", "sources"}, p, w)
        sig = _sig(f_.get("signature"), p, w) if "signature" in f_ else None
        try:
            if "signature" in f_ and sig is None:
                raise ValueError("signature does not parse")
            db.functions[f_["name"]] = Function(f_["name"], _int(f_.get("va"), "va"), sig, _meta(f_, d, fn, p, w))
        except (ValueError, KeyError) as e:
            p.error(w, str(e))

    for h in doc.get("hooks", []) or []:
        w = f"{fn} hook {h.get('name', '?')}"
        _check_keys(h, {"name", "kind", "slot", "vtable", "offset", "original", "signature", "note", "confidence", "sources"}, p, w)
        kind = h.get("kind")
        if kind not in HOOK_KINDS:
            p.error(w, f"kind must be one of {HOOK_KINDS}")
            continue
        try:
            vt = _int(h["vtable"], "vtable") if "vtable" in h else None
            off = _int(h["offset"], "offset") if "offset" in h else None
            if "slot" in h:
                slot = _int(h["slot"], "slot")
                if vt is not None and off is not None and vt + off != slot:
                    p.error(w, f"slot {slot:#x} != vtable + offset ({vt + off:#x})")
            elif vt is not None and off is not None:
                slot = vt + off
            else:
                raise ValueError("give either slot, or vtable and offset")
            original = _int(h.get("original"), "original")
        except ValueError as e:
            p.error(w, str(e))
            continue
        if slot % 8:
            p.error(w, f"slot {slot:#x} is not 8-byte aligned (pointer swaps must be atomic)")
        db.hooks[h["name"]] = Hook(h["name"], kind, slot, original, vt, off, _sig(h.get("signature"), p, w),
                                   _meta(h, d, fn, p, w))

    for c in doc.get("code", []) or []:
        w = f"{fn} code {c.get('name', '?')}"
        _check_keys(c, {"name", "va", "note", "confidence", "sources"}, p, w)
        try:
            db.code[c["name"]] = CodeSite(c["name"], _int(c.get("va"), "va"), _meta(c, d, fn, p, w))
        except (ValueError, KeyError) as e:
            p.error(w, str(e))

    for c in doc.get("constants", []) or []:
        w = f"{fn} constant {c.get('name', '?')}"
        _check_keys(c, {"name", "value", "note", "confidence", "sources"}, p, w)
        if not isinstance(c.get("value"), (int, str)) or isinstance(c.get("value"), bool):
            p.error(w, "value must be an integer or a string")
            continue
        db.constants[c["name"]] = Constant(c["name"], c["value"], _meta(c, d, fn, p, w))

    for e in doc.get("events", []) or []:
        w = f"{fn} event {e.get('name', '?') if isinstance(e, dict) else '?'}"
        if not isinstance(e, dict):
            p.error(w, "must be a mapping")
            continue
        _check_keys(e, {"name", "hash", "sent_to", "note", "confidence", "sources"}, p, w)
        name = e.get("name")
        if not isinstance(name, str) or not name or any(c.isspace() for c in name):
            p.error(w, "name must be a non-empty string without spaces")
            continue
        h = djb2_xor(name)
        if "hash" in e:
            try:
                stated = _int(e["hash"], "hash")
            except ValueError as ex:
                p.error(w, str(ex))
                stated = h
            if stated != h:
                p.error(w, f"hash {stated:#010x} does not match djb2-xor({name!r}) = {h:#010x}")
        if name in db.events:
            p.error(w, f"event {name!r} is already in {db.events[name].meta.file}")
            continue
        sent_to = e.get("sent_to", []) or []
        if isinstance(sent_to, str):
            sent_to = [sent_to]
        db.events[name] = EventAnn(name, h, [str(x) for x in sent_to], _meta(e, d, fn, p, w))

    fp = doc.get("fingerprint")
    if fp:
        db._fp_raw = getattr(db, "_fp_raw", []) + [(fn, fp)]


def _resolve_fingerprint(db: Database, p: Problems):
    for fn, fp in getattr(db, "_fp_raw", []):
        _check_keys(fp, {"types", "slots", "code"}, p, f"{fn} fingerprint")
        for t in fp.get("types", []) or []:
            try:
                db.fp_types.append(FpType(t["name"], _int(t["ti"], "ti")))
            except (KeyError, ValueError) as e:
                p.error(f"{fn} fingerprint type", str(e))
        for ref in fp.get("slots", []) or []:
            w = f"{fn} fingerprint slot {ref}"
            if ref in db.hooks:
                h = db.hooks[ref]
                db.fp_slots.append(FpSlot(ref, h.slot, h.original))
                continue
            cls, _, vname = str(ref).partition(".")
            c = db.classes.get(cls)
            v = next((v for v in c.virtuals if v.name == vname), None) if c else None
            if not c or not v:
                p.error(w, "not a hook name or Class.virtual")
            elif c.vtable is None or v.va is None:
                p.error(w, "the class needs a vtable and the virtual a va")
            else:
                db.fp_slots.append(FpSlot(f"{cls}::{vname}", c.vtable + v.offset, v.va))
        for c in fp.get("code", []) or []:
            w = f"{fn} fingerprint code {c.get('at', '?')}"
            _check_keys(c, {"at", "bytes", "skip", "required", "note"}, p, w)
            at = c.get("at")
            va = db.symbol_va(at) if isinstance(at, str) else None
            if va is None:
                try:
                    va = _int(at, "at")
                except ValueError:
                    p.error(w, "at must be a function, global or code site name, or an address")
                    continue
            try:
                data = bytes(int(b, 16) for b in str(c.get("bytes", "")).split())
            except ValueError:
                p.error(w, "bytes must be hex bytes separated by spaces")
                continue
            if not 1 <= len(data) <= 32:
                p.error(w, "between 1 and 32 bytes")
                continue
            db.fp_code.append(FpCode(str(at), va, int(c.get("skip", 0)), data, bool(c.get("required", True)),
                                     str(c.get("note", ""))))


def _check_symbols(db: Database, p: Problems):
    seen: dict[str, str] = {}
    for kind, table in (("function", db.functions), ("global", db.globals), ("hook", db.hooks), ("code site", db.code),
                        ("constant", db.constants)):
        for name, item in table.items():
            if not name.isidentifier():
                p.error(item.meta.file, f"{kind} name {name!r} is not an identifier")
            if name in seen:
                p.error(item.meta.file, f"{kind} {name} has the same name as a {seen[name]}")
            seen[name] = kind
    for name in db.structs:
        if name in db.classes:
            p.error(db.structs[name].meta.file, f"{name} is both a struct and an annotated class")
    # Every named type must be known (reflected types are checked later, when a dump is given).
    for name, s in db.structs.items():
        for m in s.members:
            if m.offset + size_hint(m.type, db) > s.size:
                p.error(s.meta.file, f"struct {name}: member {m.name} (+{m.offset:#x}) runs past its size {s.size:#x}")


def size_hint(t: TypeRef, db: Database, reflected=None) -> int:
    """Size of a member of type t (pointers 8). Unknown named types count as 1 byte here."""
    n = t.count or 1
    if t.is_pointer:
        return 8 * n
    if t.is_primitive:
        return PRIMITIVES[t.base][1] * n
    if t.base in db.structs:
        return db.structs[t.base].size * n
    if reflected is not None:
        r = reflected.by_name.get(t.base)
        if r is not None:
            return r["size"] * n
    return n
