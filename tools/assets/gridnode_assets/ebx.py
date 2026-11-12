"""EBX version 2 (magic 0x0FB2D1CE): Catalyst's asset files, read into a plain model and written back.

Written from the format as the user's own files show it (checked on them, 2026-11-12), not from any
other tool's code. The file describes itself: type and field names, sizes and offsets are in it, so
no SDK is needed.

    header      0x40 bytes: magic, offset of the strings, bytes from there to the end, import count,
                instance-run count, exported-run count (the first runs are exported), count of the types instances are of, type count, field count, name
                bytes, string bytes, array count, payload bytes, the file's GUID
    imports     (file GUID, instance GUID) pairs: objects in other files this one points to
    names       zero-terminated; types and fields name them by djb2-xor hash
    fields      16 bytes: name hash, flags (type code in bits 4..8), type index, offset, runtime offset
    types       16 bytes: name hash, first field, field count, alignment, flags, size, runtime size
    runs        4 bytes: type index, count (the instances, in payload order)
    arrays      12 bytes: offset in the array section, count, type index (after a 16-byte pad)
    strings     at the offset in the header; then the payload, then the array section

An instance's fields are at offsets that count an 8-byte header the file does not store: the data
starts 8 bytes after the (aligned) instance. Exported instances (the first ones) have their GUID in
the 16 bytes before the data. An array is its elements at its offset with the count in the 4 bytes
before; array 0 is the empty one (typed as the first array field met). Class references are 0 (null), import index | 0x80000000, or
instance index + 1. A field of type code 0 is the inherited part ("$", the parent type).

`read` gives an `Ebx`; `write` turns one back into bytes; `to_yaml` / `from_yaml` are the text form.
The round trip (read, to YAML, from YAML, write) must give the same bytes: that is the test.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

MAGIC = 0x0FB2D1CE
HEADER = 0x40

# Type codes (flags >> 4 & 0x1f), as in the game's own type info.
C_INHERITED, C_STRUCT, C_CLASS, C_ARRAY, C_CSTRING, C_ENUM, C_FILEREF = 0, 2, 3, 4, 7, 8, 9
C_BOOL, C_GUID, C_SHA1, C_RESREF, C_F32, C_F64 = 10, 21, 22, 23, 19, 20
_INTS = {11: "<b", 12: "<B", 13: "<h", 14: "<H", 15: "<i", 16: "<I", 17: "<q", 18: "<Q"}
_SIZES = {C_CLASS: 4, C_ARRAY: 4, C_CSTRING: 4, C_ENUM: 4, C_FILEREF: 4, C_BOOL: 1, C_GUID: 16, C_SHA1: 20, C_RESREF: 8,
          C_F32: 4, C_F64: 8, **{k: struct.calcsize(v) for k, v in _INTS.items()}}


class EbxError(ValueError):
    pass


def name_hash(s: str) -> int:
    h = 5381
    for c in s.encode("utf-8"):
        h = ((h * 33) ^ c) & 0xFFFFFFFF
    return h


def code_of(flags: int) -> int:
    return (flags >> 4) & 0x1F


def short_f32(v: float) -> float:
    """The shortest decimal that is the same float32 (7.68, not 7.679999828338623)."""
    want = struct.pack("<f", v)
    for digits in range(1, 10):
        s = float(f"{v:.{digits}g}")
        if struct.pack("<f", s) == want:
            return s
    return v


def key_of(t: "Type", f: "Field", taken) -> str:
    """A field's key in an object: its name, or "Type.name" when a parent already has that name
    (PamGameSplineEntityData.Enabled next to GameComponentEntityData's)."""
    return f.name if f.name not in taken else f"{t.name}.{f.name}"


def _align(n: int, a: int) -> int:
    return (n + a - 1) // a * a if a > 1 else n


@dataclass
class Field:
    name: str
    flags: int
    ref: int  # type index (struct, array, enum, inherited), else 0
    offset: int
    offset2: int  # in memory (the runtime layout)


@dataclass
class Type:
    name: str
    align: int
    flags: int
    size: int
    size2: int
    fields: list[Field] = field(default_factory=list)
    first: int = -1  # the index of its first field in the field table (read; the writer recomputes)


@dataclass
class Instance:
    type: int
    guid: bytes | None
    values: dict


@dataclass
class Ebx:
    guid: bytes
    imports: list[tuple[bytes, bytes]]
    types: list[Type]
    instances: list[Instance]
    runs: list[tuple[int, int]]  # (type, count) in payload order; the writer makes them when empty


class _Reader:
    def __init__(self, d: bytes, x: Ebx, strings: bytes, arrays_at: int, arrays: list[tuple[int, int, int]]):
        self.d, self.x, self.strings, self.arrays_at, self.arrays = d, x, strings, arrays_at, arrays

    def cstring(self, off: int):
        if off == 0xFFFFFFFF:
            return None
        if off >= len(self.strings):
            raise EbxError(f"string offset {off:#x} past the strings ({len(self.strings):#x} bytes)")
        end = self.strings.index(b"\0", off)
        raw = self.strings[off:end]
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            return {"bytes": raw.hex()}

    def value(self, f: Field, at: int, size: int):
        d, c = self.d, code_of(f.flags)
        if c == C_STRUCT:
            return self.struct(f.ref, at)
        if c == C_CLASS:
            v = struct.unpack_from("<I", d, at)[0]
            if v == 0:
                return None
            if v & 0x80000000:
                return f"import:{v & 0x7FFFFFFF}"
            return f"#{v - 1}"
        if c == C_ARRAY:
            i = struct.unpack_from("<I", d, at)[0]
            if i >= len(self.arrays):
                raise EbxError(f"array {i} of {len(self.arrays)}")
            off, count, t = self.arrays[i]
            member = self.x.types[t].fields[0]
            stride = self.member_size(member)
            base = self.arrays_at + off
            return [self.value(member, base + k * stride, stride) for k in range(count)]
        if c in (C_CSTRING, C_FILEREF):
            v = self.cstring(struct.unpack_from("<I", d, at)[0])
            return v if c == C_CSTRING else {"fileref": v}
        if c == C_ENUM:
            v = struct.unpack_from("<I", d, at)[0]
            for e in self.x.types[f.ref].fields:
                if e.offset == v:
                    return e.name
            return v
        if c == C_BOOL:
            b = d[at]
            return bool(b) if b in (0, 1) else {"u8": b}
        if c in _INTS:
            return struct.unpack_from(_INTS[c], d, at)[0]
        if c == C_F32:
            raw = d[at:at + 4]
            v = struct.unpack("<f", raw)[0]
            return short_f32(v) if math.isfinite(v) and struct.pack("<f", v) == raw else {"f32": raw.hex()}
        if c == C_F64:
            raw = d[at:at + 8]
            v = struct.unpack("<d", raw)[0]
            return v if math.isfinite(v) else {"f64": raw.hex()}
        if c in (C_GUID, C_SHA1):
            return d[at:at + _SIZES[c]].hex()
        if c == C_RESREF:
            return f"{struct.unpack_from('<Q', d, at)[0]:#018x}"
        return {"raw": d[at:at + size].hex()}  # a type code not read yet: kept as bytes

    def member_size(self, member: Field) -> int:
        c = code_of(member.flags)
        return self.x.types[member.ref].size if c == C_STRUCT else _SIZES.get(c, 0) or self.x.types[member.ref].size

    def struct(self, ti: int, base: int) -> dict:
        out: dict = {}
        self.fill(ti, base, out)
        return out

    def fill(self, ti: int, base: int, out: dict) -> None:
        t = self.x.types[ti]
        ends = sorted({f.offset for f in t.fields if code_of(f.flags) != C_INHERITED} | {t.size})
        for f in t.fields:
            c = code_of(f.flags)
            if c == C_INHERITED:
                self.fill(f.ref, base, out)
                continue
            nxt = next(e for e in ends if e > f.offset) if f.offset < t.size else f.offset
            size = _SIZES.get(c) or (self.x.types[f.ref].size if c == C_STRUCT else nxt - f.offset)
            out[key_of(t, f, out)] = self.value(f, base + f.offset, size)


def read(d: bytes) -> Ebx:
    if len(d) < HEADER or struct.unpack_from("<I", d, 0)[0] != MAGIC:
        raise EbxError("not an EBX version 2 file (magic 0x0FB2D1CE)")
    str_at, to_end, n_imp = struct.unpack_from("<3I", d, 4)
    n_runs, n_exp, n_inst, n_types, n_fields, name_len = struct.unpack_from("<6H", d, 0x10)
    str_len, n_arrays, pay_len = struct.unpack_from("<3I", d, 0x1C)
    if str_at + to_end != len(d):
        raise EbxError(f"strings at {str_at:#x} + {to_end:#x} is not the file size {len(d):#x}")
    p = HEADER
    imports = [(d[p + 32 * i:p + 32 * i + 16], d[p + 32 * i + 16:p + 32 * i + 32]) for i in range(n_imp)]
    p += 32 * n_imp
    names = {name_hash(s): s for s in d[p:p + name_len].decode("utf-8").split("\0") if s}
    p += name_len

    def nm(h: int) -> str:
        if h not in names:
            raise EbxError(f"name hash {h:#010x} not in the name table")
        return names[h]

    flds = []
    for _ in range(n_fields):
        h, flags, ref, off, off2 = struct.unpack_from("<IHHII", d, p)
        flds.append(Field(nm(h), flags, ref, off, off2))
        p += 16
    types = []
    for _ in range(n_types):
        h, first, count, align, flags, size, size2 = struct.unpack_from("<IIBBHHH", d, p)
        types.append(Type(nm(h), align, flags, size, size2, flds[first:first + count], first))
        p += 16
    runs = [struct.unpack_from("<HH", d, p + 4 * i) for i in range(n_runs)]
    p = _align(p + 4 * n_runs, 16)
    arrays = [struct.unpack_from("<3I", d, p + 12 * i) for i in range(n_arrays)]
    if len({ti for ti, _ in runs}) != n_inst:  # (0x14: the number of types the instances are of)
        raise EbxError(f"instances of {len({ti for ti, _ in runs})} types, the header says {n_inst}")
    x = Ebx(d[0x28:0x38], imports, types, [], [tuple(r) for r in runs])
    strings = d[str_at:str_at + str_len]
    pay = str_at + str_len
    r = _Reader(d, x, strings, pay + pay_len, arrays)
    exported = sum(n for _, n in runs[:n_exp])  # the first n_exp runs are the exported instances
    pos = 0
    k = 0
    for ti, count in runs:
        t = types[ti]
        for _ in range(count):
            guid = None
            start = data_start(pos, t.align, k < exported)
            if k < exported:
                g = guid_at(pos, t.align)
                guid = d[pay + g:pay + g + 16]
            x.instances.append(Instance(ti, guid, r.struct(ti, pay + start - 8)))
            pos = start - 8 + t.size
            k += 1
    return x


def data_start(end: int, align: int, exported: bool) -> int:
    """Where an instance's data starts, what is before it ending at `end`. The fields' offsets count an
    8-byte header: a type aligned to 8 or more has it in the file (zeros, aligned for the type), one
    aligned to 4 does not (its data starts at the aligned place). An exported instance has its
    16-byte GUID first."""
    a = max(align, 1)
    if exported:
        end = guid_at(end, align) + 16
    return _align(end, a) + (8 if a >= 8 else 0)


def guid_at(end: int, align: int) -> int:
    """Where an exported instance's GUID is: aligned for the instance's type."""
    return _align(end, max(align, 1))


def end_of(start: int, size: int) -> int:
    return start - 8 + size


class _Writer:
    def __init__(self, x: Ebx):
        self.x = x
        self.strings = bytearray()
        self.string_at: dict[bytes, int] = {}
        self.arrays: list[tuple[int, int, int]] = []  # array 0, the empty one, comes with the first array met
        self.array_data = bytearray()

    def cstring(self, v) -> int:
        if v is None:
            return 0xFFFFFFFF
        raw = bytes.fromhex(v["bytes"]) if isinstance(v, dict) else v.encode("utf-8")
        if raw not in self.string_at:
            self.string_at[raw] = len(self.strings)
            self.strings += raw + b"\0"
        return self.string_at[raw]

    def member_size(self, member: Field) -> int:
        c = code_of(member.flags)
        return self.x.types[member.ref].size if c == C_STRUCT else _SIZES.get(c, 0) or self.x.types[member.ref].size

    def member_align(self, member: Field) -> int:
        c = code_of(member.flags)
        if c == C_STRUCT:
            return max(self.x.types[member.ref].align, 1)
        return min(_SIZES.get(c, 4), 8) if c not in (C_GUID, C_SHA1) else 4  # a GUID array's elements: 4 (seen)

    def array(self, f: Field, items: list) -> int:
        if not self.arrays:
            self.arrays.append((0, 0, f.ref))
        if not items:
            return 0
        t = self.x.types[f.ref]
        member = t.fields[0]
        stride = self.member_size(member)
        # Arrays inside the elements first (the file lists an array after the ones in it), then this one.
        elements = bytearray(stride * len(items))
        for k, v in enumerate(items):
            self.value(member, v, elements, k * stride, stride)
        index = len(self.arrays)
        off = _align(len(self.array_data) + 4, self.member_align(member))
        self.arrays.append((off, len(items), f.ref))
        self.array_data += bytes(off - len(self.array_data))
        self.array_data[off - 4:off] = struct.pack("<I", len(items))
        self.array_data += elements
        return index

    def value(self, f: Field, v, buf: bytearray, at: int, size: int) -> None:
        c = code_of(f.flags)
        if c == C_STRUCT:
            self.fill(f.ref, v or {}, buf, at)
        elif c == C_CLASS:
            if v is None:
                n = 0
            elif isinstance(v, str) and v.startswith("import:") and 0 <= int(v[7:]) < len(self.x.imports):
                n = 0x80000000 | int(v[7:])
            elif isinstance(v, str) and v.startswith("#") and 0 <= int(v[1:]) < len(self.x.instances):
                n = int(v[1:]) + 1
            else:
                raise EbxError(f"{f.name}: a reference is null, '#N' (an instance of this file) or 'import:N' "
                               f"(one of its {len(self.x.imports)} imports), not {v!r}")
            struct.pack_into("<I", buf, at, n)
        elif c == C_ARRAY:
            struct.pack_into("<I", buf, at, self.array(f, list(v or [])))
        elif c == C_CSTRING:
            struct.pack_into("<I", buf, at, self.cstring(v))
        elif c == C_FILEREF:
            struct.pack_into("<I", buf, at, self.cstring(v["fileref"] if isinstance(v, dict) and "fileref" in v else v))
        elif c == C_ENUM:
            if isinstance(v, str):
                match = [e.offset for e in self.x.types[f.ref].fields if e.name == v]
                if not match:
                    raise EbxError(f"{f.name}: {v} is not a value of {self.x.types[f.ref].name}")
                v = match[0]
            struct.pack_into("<I", buf, at, int(v or 0))
        elif c == C_BOOL:
            buf[at] = v["u8"] if isinstance(v, dict) else int(bool(v))
        elif c in _INTS:
            struct.pack_into(_INTS[c], buf, at, int(v or 0))
        elif c == C_F32:
            buf[at:at + 4] = bytes.fromhex(v["f32"]) if isinstance(v, dict) else struct.pack("<f", float(v or 0))
        elif c == C_F64:
            buf[at:at + 8] = bytes.fromhex(v["f64"]) if isinstance(v, dict) else struct.pack("<d", float(v or 0))
        elif c in (C_GUID, C_SHA1):
            raw = bytes.fromhex(v) if v else bytes(_SIZES[c])
            if len(raw) != _SIZES[c]:
                raise EbxError(f"{f.name}: {len(raw)} bytes, not {_SIZES[c]}")
            buf[at:at + len(raw)] = raw
        elif c == C_RESREF:
            struct.pack_into("<Q", buf, at, int(v, 16) if isinstance(v, str) else int(v or 0))
        else:
            raw = bytes.fromhex(v["raw"]) if isinstance(v, dict) else b""
            buf[at:at + len(raw)] = raw[:size]

    def fill(self, ti: int, values: dict, buf: bytearray, base: int, taken: set | None = None) -> None:
        t = self.x.types[ti]
        top = taken is None  # the object itself, not a parent part of it
        taken = set() if taken is None else taken
        ends = sorted({f.offset for f in t.fields if code_of(f.flags) != C_INHERITED} | {t.size})
        for f in t.fields:
            c = code_of(f.flags)
            if c == C_INHERITED:
                self.fill(f.ref, values, buf, base, taken)
                continue
            nxt = next(e for e in ends if e > f.offset) if f.offset < t.size else f.offset
            size = _SIZES.get(c) or (self.x.types[f.ref].size if c == C_STRUCT else nxt - f.offset)
            key = key_of(t, f, taken)
            taken.add(key)
            if key in values:
                self.value(f, values[key], buf, base + f.offset, size)
        if top:
            unknown = [k for k in values if k not in taken]
            if unknown:
                raise EbxError(f"{t.name} has no field {', '.join(unknown)}")


def _type_table(x: Ebx) -> tuple[bytes, bytes, bytes]:
    # The order the file's writer met them: a type after its parent, then per field the field's own
    # type (with its fields) before the field's name.
    names: dict[str, None] = {}
    seen: set[int] = set()

    def visit(ti: int) -> None:
        if ti in seen or ti >= len(x.types):
            return
        seen.add(ti)
        t = x.types[ti]
        for f in t.fields:
            if code_of(f.flags) == C_INHERITED:
                visit(f.ref)
        names.setdefault(t.name)
        for f in t.fields:
            if code_of(f.flags) in (C_INHERITED, C_STRUCT, C_ARRAY, C_ENUM):
                visit(f.ref)
            names.setdefault(f.name)

    for ti in range(len(x.types)):
        visit(ti)
    name_bytes = b"".join(n.encode("utf-8") + b"\0" for n in names)
    name_bytes += bytes(_align(len(name_bytes), 16) - len(name_bytes))
    fields = bytearray()
    types = bytearray()
    n = 0
    for t in x.types:
        types += struct.pack("<IIBBHHH", name_hash(t.name), n, len(t.fields), t.align, t.flags, t.size, t.size2)
        for f in t.fields:
            fields += struct.pack("<IHHII", name_hash(f.name), f.flags, f.ref, f.offset, f.offset2)
        n += len(t.fields)
    return name_bytes, bytes(fields), bytes(types)


def runs_of(instances: list[Instance]) -> list[tuple[int, int]]:
    runs: list[list[int]] = []
    prev = None
    for i in instances:
        if runs and runs[-1][0] == i.type and runs[-1][1] < 0xFFFF and (i.guid is None) == (prev.guid is None):
            runs[-1][1] += 1
        else:
            runs.append([i.type, 1])
        prev = i
    return [tuple(r) for r in runs]


def write(x: Ebx) -> bytes:
    n_exp = 0
    for i in x.instances:
        if i.guid is None:
            break
        n_exp += 1
    if any(i.guid is not None for i in x.instances[n_exp:]):
        raise EbxError("exported instances (with a GUID) must come first")
    runs = x.runs or runs_of(x.instances)
    n_exp_runs, covered = 0, 0
    while covered < n_exp and n_exp_runs < len(runs):
        covered += runs[n_exp_runs][1]
        n_exp_runs += 1
    if covered != n_exp:
        raise EbxError(f"the {n_exp} exported instances do not end where an instance run does")
    w = _Writer(x)
    payload = bytearray()
    pos = 0
    for k, inst in enumerate(x.instances):
        t = x.types[inst.type]
        start = data_start(pos, t.align, k < n_exp)
        base = start - 8
        need = base + t.size
        if len(payload) < need:
            payload += bytes(need - len(payload))
        if inst.guid is not None:
            g = guid_at(pos, t.align)
            payload[g:g + 16] = inst.guid
        w.fill(inst.type, inst.values, payload, base)
        # the header bytes it does not store: put back what was there (a GUID or the previous data)
        pos = need
    payload += bytes(_align(len(payload) + 1, 16) - len(payload))  # never ends right at the data's end
    strings = bytes(w.strings) + bytes(_align(len(w.strings), 16) - len(w.strings))
    arrays = bytes(w.array_data) + bytes(_align(len(w.array_data), 16) - len(w.array_data))
    names, fields, types = _type_table(x)
    head = bytearray(HEADER)
    body = bytearray()
    for fg, ig in x.imports:
        body += fg + ig
    body += names + fields + types
    for ti, count in runs:
        body += struct.pack("<HH", ti, count)
    body += bytes(_align(HEADER + len(body), 16) - HEADER - len(body))
    for a in w.arrays:
        body += struct.pack("<3I", *a)
    body += bytes(_align(HEADER + len(body), 16) - HEADER - len(body))
    str_at = HEADER + len(body)
    struct.pack_into("<4I", head, 0, MAGIC, str_at, len(strings) + len(payload) + len(arrays), len(x.imports))
    struct.pack_into("<6H", head, 0x10, len(runs), n_exp_runs, len({ti for ti, _ in runs}), len(x.types), len(fields) // 16, len(names))
    struct.pack_into("<3I", head, 0x1C, len(strings), len(w.arrays), len(payload))
    head[0x28:0x38] = x.guid
    return bytes(head + body + strings + payload + arrays)


# ---- the text form -------------------------------------------------------------------------------
# One YAML document: the file GUID, its imports, its own type table (names, flags, sizes, offsets in
# the file and in memory: what a lossless write needs, and what a reader checks a type against), and
# the instances in file order, each with its type, its GUID if exported, and its fields by name
# (parents' first; a name a parent already has is "Type.name"). References: null, "#N" (instance N
# of this file) or "import:N" (imports[N]). Values the reader could not type exactly are kept as
# {f32: hex}, {u8: n}, {bytes: hex} or {raw: hex}.

class Hex(int):
    """An int written in hex in the YAML (flags, offsets, sizes)."""


def to_dict(x: Ebx) -> dict:
    counts: dict[str, int] = {}
    for t in x.types:
        counts[t.name] = counts.get(t.name, 0) + 1
    return {
        "ebx": 2,
        "guid": x.guid.hex(),
        "imports": [[f.hex(), i.hex()] for f, i in x.imports],
        "instances": [
            {"type": x.types[i.type].name if counts[x.types[i.type].name] == 1 else i.type,
             **({"guid": i.guid.hex()} if i.guid is not None else {}),
             "values": i.values}
            for i in x.instances
        ],
        **({"runs": [list(r) for r in x.runs]} if x.runs != runs_of(x.instances) else {}),
        "types": [
            {"name": t.name, "align": t.align, "flags": Hex(t.flags), "size": Hex(t.size), "size2": Hex(t.size2),
             "fields": [[f.name, Hex(f.flags), f.ref, Hex(f.offset), Hex(f.offset2)] for f in t.fields]}
            for t in x.types
        ],
    }


def from_dict(y: dict) -> Ebx:
    if y.get("ebx") != 2:
        raise EbxError("not an EBX YAML document (ebx: 2)")
    types = [Type(t["name"], t["align"], t["flags"], t["size"], t["size2"], [Field(*f) for f in t["fields"]]) for t in y["types"]]
    by_name: dict[str, int] = {}
    for k, t in enumerate(types):
        by_name.setdefault(t.name, k)
    insts = []
    for i in y["instances"]:
        ti = i["type"] if isinstance(i["type"], int) else by_name.get(i["type"])
        if ti is None:
            raise EbxError(f"instance type {i['type']} is not in the type table")
        insts.append(Instance(ti, bytes.fromhex(i["guid"]) if i.get("guid") else None, i.get("values") or {}))
    x = Ebx(bytes.fromhex(y["guid"]), [(bytes.fromhex(a), bytes.fromhex(b)) for a, b in y.get("imports", [])], types, insts, [])
    x.runs = [tuple(r) for r in y["runs"]] if "runs" in y else runs_of(insts)
    return x


def _yaml():
    import yaml

    class Dumper(getattr(yaml, "CSafeDumper", yaml.SafeDumper)):  # libyaml when there: ~10x faster
        pass

    def short_list(d, v):  # a type table row, a vector or a GUID pair on one line
        flow = len(v) <= 6 and all(not isinstance(e, (dict, list)) for e in v)
        return d.represent_sequence("tag:yaml.org,2002:seq", v, flow_style=flow)

    def short_dict(d, v):  # a vector or a small struct of numbers on one line
        flow = len(v) <= 4 and all(isinstance(e, (int, float)) and not isinstance(e, bool) for e in v.values())
        return d.represent_mapping("tag:yaml.org,2002:map", v.items(), flow_style=flow)

    Dumper.add_representer(Hex, lambda d, v: d.represent_scalar("tag:yaml.org,2002:int", hex(v)))
    Dumper.add_representer(list, short_list)
    Dumper.add_representer(dict, short_dict)
    return yaml, Dumper


def to_yaml(x: Ebx) -> str:
    yaml, Dumper = _yaml()
    return yaml.dump(to_dict(x), Dumper=Dumper, sort_keys=False, allow_unicode=True, width=120)


def from_yaml(text: str) -> Ebx:
    import yaml
    return from_dict(yaml.load(text, Loader=getattr(yaml, "CSafeLoader", yaml.SafeLoader)))


def round_trip(d: bytes) -> tuple[bool, str]:
    """read, to YAML, back, write: (same bytes?, where they first differ)."""
    o = write(from_yaml(to_yaml(read(d))))
    if o == d:
        return True, ""
    n = min(len(o), len(d))
    first = next((i for i in range(n) if o[i] != d[i]), n)
    return False, f"{len(d)} bytes, written {len(o)}; first difference at {first:#x}"


# ---- the file's types against the game's -----------------------------------------------------------
# GridNode's reflected dump (gridnode-reflected.json) has every type the running game knows. A file's
# own type table must agree with it, or the game reads the data wrong: type codes, struct and enum
# types by name, enum values. Seen on the user's files and not errors: a few fields and one type that
# exist in the data but not in the running game (pipeline or debug only), and value types whose flags
# have 0x8000 in the game. The file's second offsets are not the runtime offsets.

def load_dump(path: str) -> dict:
    import json
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    return {"by_name": {t["name"]: t for t in d.get("types", []) if t.get("name")},
            "by_ti": {t["ti"]: t for t in d.get("types", [])}}


def check_types(x: Ebx, dump: dict) -> tuple[list[str], list[str]]:
    """(errors, warnings) for the file's type table against the game's types."""
    errors: list[str] = []
    warnings: list[str] = []
    by_name, by_ti = dump["by_name"], dump["by_ti"]

    def name_of(f: Field) -> str:
        c = code_of(f.flags)
        if c in (C_STRUCT, C_ENUM):
            return x.types[f.ref].name
        if c == C_ARRAY:
            m = x.types[f.ref].fields[0] if x.types[f.ref].fields else None
            return "array of " + (name_of(m) if m and code_of(m.flags) in (C_STRUCT, C_ENUM, C_ARRAY) else str(m and code_of(m.flags)))
        return ""

    def game_name(g: dict) -> str:
        c = (g["flags"] >> 4) & 0x1F
        if c in (C_STRUCT, C_ENUM):
            return g["name"]
        if c == C_ARRAY:
            e = by_ti.get(g.get("element") or g.get("elementType") or "")
            return "array of " + (game_name(e) if e and ((e["flags"] >> 4) & 0x1F) in (C_STRUCT, C_ENUM, C_ARRAY) else
                                  str(e and (e["flags"] >> 4) & 0x1F))
        return ""

    for t in x.types:
        if t.name == "array":
            continue
        g = by_name.get(t.name)
        if g is None:
            warnings.append(f"{t.name}: not a type of the running game (data only the pipeline or a debug build uses?)")
            continue
        if code_of(t.flags) != (g["flags"] >> 4) & 0x1F:
            errors.append(f"{t.name}: type code {code_of(t.flags)} here, {(g['flags'] >> 4) & 0x1F} in the game")
            continue
        if g.get("category") == "Enum":
            game = {v["name"]: v["value"] for v in g.get("values", [])}
            for f in t.fields:
                if f.name not in game:
                    errors.append(f"{t.name}.{f.name}: not a value of the game's enum")
                elif game[f.name] != f.offset:
                    errors.append(f"{t.name}.{f.name} = {f.offset} here, {game[f.name]} in the game")
            continue
        game_fields = {f["name"]: f for f in g.get("fields", [])}
        for f in t.fields:
            if code_of(f.flags) == C_INHERITED:
                parent = by_ti.get(g.get("super", ""))
                if parent is not None and parent["name"] != x.types[f.ref].name:
                    errors.append(f"{t.name}: parent {x.types[f.ref].name} here, {parent['name']} in the game")
                continue
            gf = game_fields.get(f.name)
            if gf is None:
                warnings.append(f"{t.name}.{f.name}: not a field of the game's type (data only the pipeline uses?)")
                continue
            gt = by_ti.get(gf["type"])
            if gt is None:
                continue
            if (gt["flags"] >> 4) & 0x1F != code_of(f.flags):
                errors.append(f"{t.name}.{f.name}: type code {code_of(f.flags)} here, {(gt['flags'] >> 4) & 0x1F} ({gt['name']}) in the game")
            elif code_of(f.flags) in (C_STRUCT, C_ENUM) and name_of(f) != game_name(gt):
                errors.append(f"{t.name}.{f.name}: {name_of(f)} here, {game_name(gt)} in the game")
    return errors, warnings
