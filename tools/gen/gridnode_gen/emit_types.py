"""types.hpp: C++ structs for every reflected class and value type (from the user's dump), merged
with the annotations (runtime class members, non-reflected structs, function signatures).

Layout rules, so the header matches the game on both MSVC and Itanium compilers:
  - members are placed at their exact offsets with explicit padding, and every struct is padded
    explicitly to its full size (no tail padding a derived class could reuse);
  - a member whose offset does not suit its C++ alignment becomes a byte array;
  - sizeof and the offsets of annotated members are checked with static_assert.
"""

from __future__ import annotations

import io
from dataclasses import dataclass, field

from . import __version__
from .annotations import Database, Member
from .ctypes_ import PRIMITIVES, TypeRef
from .reflected import Reflected, c_ident

# Primitive name -> (C++ type, size, alignment). Anything else (or a size mismatch) becomes a blob.
PRIM_CPP = {
    "Boolean": ("bool", 1, 1), "Int8": ("int8_t", 1, 1), "Uint8": ("uint8_t", 1, 1),
    "Int16": ("int16_t", 2, 2), "Uint16": ("uint16_t", 2, 2), "Int32": ("int32_t", 4, 4),
    "Uint32": ("uint32_t", 4, 4), "Int64": ("int64_t", 8, 8), "Uint64": ("uint64_t", 8, 8),
    "Float32": ("float", 4, 4), "Float64": ("double", 8, 8), "CString": ("const char*", 8, 8),
    "FileRef": ("FileRef", 8, 8), "ResourceRef": ("ResourceRef", 8, 8), "Guid": ("Guid", 16, 4),
    "String": ("String", 0x20, 8), "DbObject": ("DbObject", 0x10, 8), "SHA1": ("SHA1", 0x14, 1),
}

PRELUDE = """\
// Frostbite Array<T>: a pointer to the elements; the count is the u32 just before them.
template <typename T> struct Array {
    T* data;
    uint32_t size() const { return data ? reinterpret_cast<const uint32_t*>(data)[-1] : 0; }
    T& operator[](uint32_t i) const { return data[i]; }
    T* begin() const { return data; }
    T* end() const { return data + size(); }
};
static_assert(sizeof(Array<int>) == 8, "Array");

// Primitives without a plain C++ type.
struct FileRef { const char* path; };
struct ResourceRef { uint64_t id; };
struct Guid { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; };
struct String { uint8_t _[0x20]; };   // engine string, layout not described
struct DbObject { uint8_t _[0x10]; };
struct SHA1 { uint8_t bytes[0x14]; };
static_assert(sizeof(FileRef) == 8 && sizeof(ResourceRef) == 8 && sizeof(Guid) == 16 && sizeof(String) == 0x20 &&
              sizeof(DbObject) == 0x10 && sizeof(SHA1) == 0x14, "primitives");
"""


@dataclass
class CType:
    text: str          # declaration text, with {} where the name goes: "float {}", "Foo* {}", "uint8_t {}[4]"
    size: int
    align: int
    dep: str | None = None  # emitted type this one needs by value


@dataclass
class Slot:
    offset: int
    name: str
    ctype: CType
    comment: str = ""
    annotated: bool = False


@dataclass
class Emittable:
    ident: str
    kind: str                  # "reflected" or "struct"
    size: int
    align: int                 # alignas, 0 = none
    base: str | None = None
    slots: list[Slot] = field(default_factory=list)
    header: str = ""
    statics: list[str] = field(default_factory=list)
    deps: set[str] = field(default_factory=set)
    vftable: bool = False


def blob(size: int) -> CType:
    return CType("uint8_t {}" + f"[{size:#x}]", size, 1)


class Generator:
    def __init__(self, db: Database, r: Reflected):
        self.db, self.r = db, r
        self.items: dict[str, Emittable] = {}
        self.enums: list[dict] = []
        self.type_idents = {t["_ident"] for t in r.types} | set(db.structs)
        self.notes: list[str] = []
        self.emitted_sizes: dict[str, tuple[int, int]] = {}  # ident -> (size, align)

    # ---- types -------------------------------------------------------------------------------
    def reflected_ctype(self, ti: int) -> CType | None:
        t = self.r.get(ti)
        if t is None:
            return None
        cat = t["category"]
        if cat == "Primitive":
            spec = PRIM_CPP.get(t["displayName"])
            if spec and spec[1] == t["size"]:
                return CType(spec[0] + " {}", spec[1], spec[2])
            return blob(t["size"]) if t["size"] else None
        if cat == "Enum":
            return CType(t["_ident"] + " {}", t["size"], min(t["size"], 8) or 1)
        if cat == "ValueType":
            return CType(t["_ident"] + " {}", t["size"], 0, dep=t["_ident"])  # alignment resolved later
        if cat == "Class":
            return CType(t["_ident"] + "* {}", 8, 8)
        if cat == "Array":
            el = self.r.get(t["_element"])
            return CType(f"Array<{self.array_element(el)}> {{}}", 8, 8)
        if cat == "FixedArray":
            el = self.r.get(t["_element"])
            et = self.reflected_ctype(el["_ti"]) if el else None
            if et and et.size and t["size"] % et.size == 0 and "[" not in et.text:
                n = t["size"] // et.size
                return CType(et.text + f"[{n}]", t["size"], et.align, et.dep)
            return blob(t["size"])
        return blob(t["size"]) if t["size"] else None

    def array_element(self, el: dict | None) -> str:
        if el is None:
            return "uint8_t"
        if el["category"] == "Class":
            return el["_ident"] + "*"
        ct = self.reflected_ctype(el["_ti"])
        if ct is None or "[" in ct.text:
            return "uint8_t"
        return ct.text.replace(" {}", "")

    def annotation_ctype(self, t: TypeRef) -> CType:
        if t.is_primitive:
            cpp, size, align = PRIMITIVES[t.base]
            dep = None
        elif t.base in self.db.structs:
            s = self.db.structs[t.base]
            cpp, size, align, dep = t.base, s.size, s.align, t.base
        else:
            rt = self.r.by_name.get(t.base)
            if rt is None:
                cpp, size, align, dep = "void", 0, 1, None
            else:
                cpp, size, align = rt["_ident"], rt["size"], 0
                dep = rt["_ident"] if rt["category"] == "ValueType" else None
                if rt["category"] == "Enum":
                    align = min(rt["size"], 8) or 1
                if rt["category"] == "Primitive":
                    ct = self.reflected_ctype(rt["_ti"])
                    cpp, size, align = ct.text.replace(" {}", ""), ct.size, ct.align
        if t.const:
            cpp = "const " + cpp
        if t.is_pointer:
            cpp, size, align, dep = cpp + "*" * t.pointers, 8, 8, None
        n = t.count or 1
        text = cpp + " {}" + (f"[{t.count}]" if t.count else "")
        return CType(text, size * n, align, dep)

    def cpp_type(self, t: TypeRef) -> str:
        """For signatures, outside gn::fb: arrays decay to pointers, type names are fully qualified."""
        ct = self.annotation_ctype(TypeRef(t.base, t.pointers + (1 if t.count else 0), t.const))
        text = ct.text.replace(" {}", "")
        if not t.is_primitive and (t.base in self.db.structs or t.base in self.r.by_name):
            ident = t.base if t.base in self.db.structs else self.r.by_name[t.base]["_ident"]
            text = text.replace(ident, "::gn::fb::" + ident, 1)
        return text

    # ---- building ----------------------------------------------------------------------------
    def build(self):
        anns = self.db.classes
        for t in self.r.types:
            if t["category"] == "Enum":
                self.enums.append(t)
            elif t["category"] in ("Class", "ValueType"):
                self.items[t["_ident"]] = self.reflected_item(t, anns.get(t["displayName"]))
        for s in self.db.structs.values():
            self.items[s.name] = self.struct_item(s)

    def field_name(self, name: str, used: set[str]) -> str:
        n = c_ident(name)
        if n in self.type_idents or n.startswith("_pad"):
            n += "_"
        base, i = n, 2
        while n in used:
            n = f"{base}_{i}"
            i += 1
        used.add(n)
        return n

    def reflected_item(self, t: dict, ann) -> Emittable:
        sup = self.r.super_of(t) if t["category"] == "Class" else None
        ra = t.get("alignment", 0)
        e = Emittable(t["_ident"], "reflected", t["size"], ra if ra in (16, 32, 64) else 0,
                      base=sup["_ident"] if sup else None)
        e.header = (f"// {t['category']} {t['displayName']}  size {t['size']:#x}  ti {t['ti']}  module {t.get('module', '')}"
                    + (f"  classId {t['classId']}..{t['lastSubClassId']}" if "classId" in t else ""))
        if t.get("creators"):
            e.header += "\n// created from " + ", ".join(
                f"{self.r.get(int(c['data'], 16))['displayName']} ({'client' if c['realm'] == 0 else 'server'})"
                for c in t["creators"] if self.r.get(int(c["data"], 16)))
        if sup:
            e.deps.add(sup["_ident"])
        e.statics.append(f"static constexpr uint64_t kTypeInfo = {t['ti']};")
        vts = t.get("vtables", [])
        if len(vts) == 1:
            e.statics.append(f"static constexpr uint64_t kVtable = {vts[0]};")
        used: set[str] = set()
        slots: list[Slot] = []
        for f in t.get("fields", []):
            ct = self.reflected_ctype(f["_type"])
            if ct is None:
                continue
            slots.append(Slot(f["offset"], self.field_name(f["name"], used), ct))
        start = sup["size"] if sup else 0
        if ann is not None:
            for m in ann.members:
                if m.offset < start:
                    continue  # lies in a base class; constants still in engine.hpp
                slots.append(Slot(m.offset, self.field_name(m.name, used), self.annotation_ctype(m.type),
                                  comment=_member_comment(m), annotated=True))
        root_polymorphic = (t["category"] == "Class" and not sup) or (t["category"] == "ValueType" and t.get("vtableConfirmed"))
        if root_polymorphic and t["size"] >= 8 and not any(s.offset < 8 for s in slots):
            e.vftable = True
        e.slots = sorted(slots, key=lambda s: (s.offset, not s.annotated))
        return e

    def struct_item(self, s) -> Emittable:
        e = Emittable(s.name, "struct", s.size, s.align if s.align >= 16 else 0)
        e.header = f"// struct {s.name}  size {s.size:#x}  (annotation: {s.meta.file})" + (
            f"\n// {' '.join(s.meta.note.split())}" if s.meta.note else "")
        used: set[str] = set()
        e.slots = sorted((Slot(m.offset, self.field_name(m.name, used), self.annotation_ctype(m.type),
                               comment=_member_comment(m), annotated=True) for m in s.members), key=lambda x: x.offset)
        return e

    def align_of(self, ident: str) -> int:
        if ident in self.emitted_sizes:
            return self.emitted_sizes[ident][1]
        e = self.items.get(ident)
        if e is None:
            return 1
        a = max([e.align or 1, 8 if e.vftable else 1] + [self.slot_align(s) for s in e.slots] +
                ([self.align_of(e.base)] if e.base else []))
        self.emitted_sizes[ident] = (e.size, a)
        return a

    def slot_align(self, s: Slot) -> int:
        if s.ctype.dep and s.ctype.align == 0:
            return self.align_of(s.ctype.dep)
        return s.ctype.align or 1

    # ---- output ------------------------------------------------------------------------------
    def order(self) -> list[Emittable]:
        out, state = [], {}

        def visit(ident: str):
            if state.get(ident) == 2 or ident not in self.items:
                return
            if state.get(ident) == 1:
                raise ValueError(f"by-value cycle at {ident}")
            state[ident] = 1
            e = self.items[ident]
            deps = set(e.deps) | {s.ctype.dep for s in e.slots if s.ctype.dep}
            for d in sorted(d for d in deps if d):
                visit(d)
            state[ident] = 2
            out.append(e)

        for ident in sorted(self.items):
            visit(ident)
        return out

    def emit_struct(self, w, e: Emittable):
        w(e.header + "\n")
        align = self.align_of(e.ident)
        alignas = f"alignas({e.align}) " if e.align else ""
        base = f" : {e.base}" if e.base else ""
        w(f"struct {alignas}{e.ident}{base} {{\n")
        for st in e.statics:
            w(f"    {st}\n")
        cur = self.items[e.base].size if e.base else 0
        pad = 0
        if e.vftable:
            w("    void* __vftable; // +0x0\n")
            cur = 8
        for s in e.slots:
            size = s.ctype.size
            a = self.slot_align(s)
            if s.offset < cur:
                w(f"    // overlaps the member before: {s.ctype.text.format(s.name)}; // +{s.offset:#x}\n")
                continue
            if s.offset + size > e.size:
                w(f"    // runs past the end: {s.ctype.text.format(s.name)}; // +{s.offset:#x}\n")
                continue
            if s.offset > cur:
                w(f"    uint8_t _pad{pad}[{s.offset - cur:#x}];\n")
                pad += 1
                cur = s.offset
            text = s.ctype.text
            if a and s.offset % a:
                text = f"uint8_t {{}}[{size:#x}]"  # misaligned for its C++ type: keep the bytes
                s.comment = (s.comment + " " if s.comment else "") + f"(as bytes: {s.ctype.text.format('').strip()})"
            w(f"    {text.format(s.name)}; // +{s.offset:#x}{(' ' + s.comment) if s.comment else ''}\n")
            cur = s.offset + size
        if e.size > cur:
            w(f"    uint8_t _pad{pad}[{e.size - cur:#x}];\n")
        w("};\n")
        if e.size:
            w(f"static_assert(sizeof({e.ident}) == {e.size:#x}, \"size of {e.ident}\");\n")
        for s in e.slots:
            if s.annotated and s.offset >= (self.items[e.base].size if e.base else 0):
                w(f"static_assert(offsetof({e.ident}, {s.name}) == {s.offset:#x}, \"{e.ident}::{s.name}\");\n")
        w("\n")

    def emit(self) -> str:
        self.build()
        o = io.StringIO()
        w = o.write
        w("#pragma once\n")
        w(f"// Generated by gridnode-gen {__version__} from {self.r.doc.get('image', {}).get('name', 'the game')} "
          f"(dump {self.r.doc.get('created', '')}) and db/annotations.\n")
        w("// Contains the game's reflection data: generated on your machine, never committed or shared.\n")
        w("#include <cstddef>\n#include <cstdint>\n\n")
        w("#if defined(__GNUC__) || defined(__clang__)\n#pragma GCC diagnostic push\n"
          "#pragma GCC diagnostic ignored \"-Winvalid-offsetof\"\n#endif\n\n")
        w("namespace gn::fb {\n\n" + PRELUDE + "\n")
        w("// ---- forward declarations ----\n")
        for ident in sorted(self.items):
            w(f"struct {ident};\n")
        w("\n// ---- enums ----\n")
        for t in sorted(self.enums, key=lambda t: t["_ident"]):
            under = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}.get(t["size"], "int32_t")
            w(f"enum class {t['_ident']} : {under} {{\n")
            used: set[str] = set()
            for v in t.get("values", []):
                n = c_ident(v["name"])
                if n in used:
                    continue
                used.add(n)
                w(f"    {n} = {v['value']},\n")
            w("};\n")
        w("\n// ---- classes, value types and annotation structs ----\n\n")
        for e in self.order():
            self.emit_struct(w, e)
        w("} // namespace gn::fb\n\n")
        self.emit_signatures(w)
        w("#if defined(__GNUC__) || defined(__clang__)\n#pragma GCC diagnostic pop\n#endif\n")
        return o.getvalue()

    def emit_signatures(self, w):
        def fptr(sig, this_type: str | None = None) -> str:
            params = ([this_type] if this_type else []) + [self.cpp_type(p.type) for p in sig.params]
            return f"{self.cpp_type(sig.returns)} (*)({', '.join(params)})"

        w("// ---- function pointer types (call through gn::mem::rva of the VA in gridnode/engine.hpp) ----\n")
        w("namespace gn::fn {\n")
        for f in sorted(self.db.functions.values(), key=lambda f: f.name):
            w(f"using {f.name} = {fptr(f.signature)};\n")
        for h in sorted(self.db.hooks.values(), key=lambda h: h.name):
            if h.signature:
                w(f"using {h.name} = {fptr(h.signature)};\n")
        w("} // namespace gn::fn\n\n")
        w("// ---- virtual function pointer types: gn::vfn::<Class>::<virtual> (this first) ----\n")
        w("namespace gn::vfn {\n")
        for c in sorted(self.db.classes.values(), key=lambda c: c.name):
            if not c.virtuals:
                continue
            rt = self.r.by_name.get(c.name)
            this = ("::gn::fb::" + rt["_ident"] if rt else "void") + "*"
            w(f"namespace {c.name} {{\n")
            for v in sorted(c.virtuals, key=lambda v: v.offset):
                w(f"using {v.name} = {fptr(v.signature, this)};\n")
            w(f"}} // namespace {c.name}\n")
        w("} // namespace gn::vfn\n\n")


def _member_comment(m: Member) -> str:
    parts = []
    if m.meta.confidence != "confirmed":
        parts.append(f"[{m.meta.confidence}]")
    if m.meta.note:
        n = " ".join(m.meta.note.split())
        parts.append(n if len(n) <= 120 else n[:117] + "...")
    return " ".join(parts)


def emit(db: Database, r: Reflected) -> str:
    return Generator(db, r).emit()
