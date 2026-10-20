"""GridNodeSync: annotations <-> Ghidra (tools/ghidra/GridNodeSync.java).

  to_ghidra(db, reflected)   -> the JSON the script imports: structs for annotated classes and
                                structs, names and comments for functions, virtuals, globals, code
                                sites, vtables and hook slots.
  from_ghidra(doc, db)       -> annotation YAML for what was named in Ghidra and GridNode does not
                                know yet (functions, labels, struct members), marked
                                confidence: inferred, sources: [ghidra], for someone to review and
                                move into the right annotation file.

Types cross as GridNode's type language (u8..u64, i8..i64, f32, f64, bool, char, void, Name*,
T[N]); the script maps them to Ghidra types and back.
"""

from __future__ import annotations

import re

from .annotations import Database
from .ctypes_ import TypeError_, parse_signature, parse_type

FORMAT_IN = "gridnode-ghidra"          # GridNode -> Ghidra
FORMAT_OUT = "gridnode-ghidra-export"  # Ghidra -> GridNode
VERSION = 1


def _meta_comment(m, extra: str = "") -> str:
    parts = [f"GridNode [{m.confidence}]"]
    if m.sources:
        parts[0] += f" ({', '.join(m.sources)})"
    if extra:
        parts.append(extra)
    if m.note:
        parts.append(m.note)
    return "\n".join(parts)


def _sig_text(sig) -> str:
    if sig is None:
        return ""
    params = ", ".join(f"{p.type}{(' ' + p.name) if p.name else ''}" for p in sig.params)
    return f"{sig.returns}({params})"


def to_ghidra(db: Database, reflected=None) -> dict:
    structs = []
    for c in db.classes.values():
        size = c.size
        if size is None and reflected is not None:
            t = reflected.by_name.get(c.name)
            size = t.get("size") if t else None
        if not c.members and size is None:
            continue
        structs.append({
            "name": c.name, "kind": "class",
            "size": size or (max((m.offset + 8 for m in c.members), default=8)),
            "comment": _meta_comment(c.meta, f"realm {c.realm}"),
            "members": [{"offset": m.offset, "name": m.name, "type": str(m.type), "comment": _meta_comment(m.meta)}
                        for m in sorted(c.members, key=lambda m: m.offset)],
        })
    for s in db.structs.values():
        structs.append({
            "name": s.name, "kind": "struct", "size": s.size, "comment": _meta_comment(s.meta),
            "members": [{"offset": m.offset, "name": m.name, "type": str(m.type), "comment": _meta_comment(m.meta)}
                        for m in sorted(s.members, key=lambda m: m.offset)],
        })

    functions = [{"name": f.name, "va": f.va, "signature": _sig_text(f.signature),
                  "comment": _meta_comment(f.meta, _sig_text(f.signature))} for f in db.functions.values()]
    labels = []
    for c in db.classes.values():
        if c.vtable:
            labels.append({"name": f"{c.name}::vftable", "va": c.vtable, "type": "",
                           "comment": _meta_comment(c.meta, f"vtable of {c.name}")})
        for v in c.virtuals:
            if v.va:
                functions.append({"name": f"{c.name}::{v.name}", "va": v.va, "signature": _sig_text(v.signature),
                                  "comment": _meta_comment(v.meta, f"{c.name} vtable +{v.offset:#x}: {_sig_text(v.signature)}")})
    for g in db.globals.values():
        labels.append({"name": g.name, "va": g.va, "type": str(g.type), "comment": _meta_comment(g.meta)})
    for cs in db.code.values():
        labels.append({"name": cs.name, "va": cs.va, "type": "", "comment": _meta_comment(cs.meta)})
    for h in db.hooks.values():
        labels.append({"name": f"{h.name}_slot", "va": h.slot, "type": "void*",
                       "comment": _meta_comment(h.meta, f"GridNode hook ({h.kind}); holds {h.original:#x} in this build")})
    functions.sort(key=lambda f: f["va"])
    labels.sort(key=lambda x: x["va"])
    structs.sort(key=lambda s: s["name"])
    return {"format": FORMAT_IN, "version": VERSION, "structs": structs, "functions": functions, "labels": labels}


# ---- Ghidra -> annotations -------------------------------------------------------------------------
_GHIDRA_TYPES = {
    "void": "void", "bool": "bool", "char": "char", "uchar": "u8", "byte": "u8", "undefined1": "u8", "undefined": "u8",
    "short": "i16", "ushort": "u16", "word": "u16", "undefined2": "u16",
    "int": "i32", "uint": "u32", "dword": "u32", "undefined4": "u32", "long": "i32", "ulong": "u32",
    "longlong": "i64", "ulonglong": "u64", "qword": "u64", "undefined8": "u64",
    "float": "f32", "double": "f64",
    "int8_t": "i8", "uint8_t": "u8", "int16_t": "i16", "uint16_t": "u16", "int32_t": "i32", "uint32_t": "u32",
    "int64_t": "i64", "uint64_t": "u64",
}


def ghidra_type(text: str, known: set[str]) -> str:
    """A Ghidra type name in GridNode's type language: primitives map, known names stay, anything
    else becomes void* (a pointer) or u8[n] (unknown by value is not representable)."""
    t = text.strip().replace("const ", "")
    t = re.sub(r"\s+", "", t)
    m = re.match(r"^(.*?)(\*+)$", t)
    stars = ""
    if m:
        t, stars = m.group(1), m.group(2)
    t = t.split("::")[-1] if t.startswith("fb::") else t
    base = _GHIDRA_TYPES.get(t.lower(), t if t in known else None)
    if base is None:
        return "void*" if stars else "u64"
    if base == "void" and not stars:
        return "void"
    return base + stars


def _sig_from_ghidra(proto: str, known: set[str]) -> str | None:
    """'undefined8 FUN(longlong param_1, int param_2)' -> 'u64(i64 param_1, i32 param_2)'."""
    m = re.match(r"^\s*(.+?)\s+[\w:~<>]+\s*\((.*)\)\s*$", proto or "")
    if not m:
        return None
    ret = ghidra_type(m.group(1).replace("__thiscall", "").replace("__fastcall", "").strip(), known)
    params = []
    body = m.group(2).strip()
    if body and body != "void" and body != "...":
        for i, part in enumerate(x.strip() for x in body.split(",")):
            if part == "...":
                continue
            pm = re.match(r"^(.*?)([A-Za-z_]\w*)$", part)
            ptype, pname = (pm.group(1), pm.group(2)) if pm and pm.group(1).strip() else (part, f"a{i}")
            params.append(f"{ghidra_type(ptype, known)} {pname}")
    text = f"{ret}({', '.join(params)})"
    try:
        parse_signature(text)
    except TypeError_:
        return None
    return text


def _ident(name: str) -> str:
    n = re.sub(r"[^A-Za-z0-9_]", "_", name.replace("::", "_"))
    return n if not n[:1].isdigit() else "_" + n


def from_ghidra(doc: dict, db: Database) -> tuple[str, dict]:
    """YAML text for the new findings in a GridNodeSync export, and counts."""
    if doc.get("format") != FORMAT_OUT:
        raise ValueError(f"not a GridNodeSync export (format {doc.get('format')!r})")
    if doc.get("version") != VERSION:
        raise ValueError(f"unsupported GridNodeSync export version {doc.get('version')}")
    known_types = set(db.classes) | set(db.structs)
    known_vas = {f.va: f.name for f in db.functions.values()}
    for c in db.classes.values():
        for v in c.virtuals:
            if v.va:
                known_vas.setdefault(v.va, f"{c.name}::{v.name}")
    known_vas.update({g.va: g.name for g in db.globals.values()})
    known_vas.update({c.va: c.name for c in db.code.values()})
    for c in db.classes.values():
        if c.vtable:
            known_vas.setdefault(c.vtable, f"{c.name}::vftable")
    for h in db.hooks.values():
        known_vas.setdefault(h.original, h.name)   # the function a hook replaces
        known_vas.setdefault(h.slot, f"{h.name}_slot")
    # Every name an annotation already uses (symbols, hooks, types, constants) is taken.
    taken = (set(db.functions) | set(db.globals) | set(db.code) | set(db.hooks) | set(db.classes) | set(db.structs)
             | set(db.constants))

    funcs, globs, renamed, members = [], [], [], {}
    for f in doc.get("functions", []):
        va, name = int(f["va"]), f["name"]
        if va in known_vas:
            if known_vas[va].split("::")[-1] != name.split("::")[-1]:
                renamed.append((va, known_vas[va], name))
            continue
        ident = _ident(name)
        if ident in taken:
            continue
        taken.add(ident)
        sig = _sig_from_ghidra(f.get("prototype", ""), known_types) if f.get("signatureUserDefined") else None
        note = f"Named in Ghidra as {name}." + ("" if sig else " Signature not known yet.")
        entry = {"name": ident, "va": va}
        if sig:
            entry["signature"] = sig
        entry["note"] = note
        funcs.append(entry)
    function_vas = {int(f["va"]) for f in doc.get("functions", [])}
    for g in doc.get("labels", []):
        va, name = int(g["va"]), g["name"]
        if va in function_vas:  # a label at a function's entry (older exports): the function covers it
            continue
        if va in known_vas:
            if known_vas[va].split("::")[-1] != name.split("::")[-1]:
                renamed.append((va, known_vas[va], name))
            continue
        ident = _ident(name)
        if ident in taken:
            continue
        taken.add(ident)
        globs.append({"name": ident, "va": va, "type": ghidra_type(g.get("type") or "void*", known_types),
                      "note": f"Labelled in Ghidra as {name}."})
    for s in doc.get("structs", []):
        ann = db.classes.get(s["name"]) or db.structs.get(s["name"])
        have = {m.offset for m in ann.members} if ann else set()
        for m in s.get("members", []):
            if m["offset"] in have or re.match(r"^(field|padding|undefined)_?(0x)?[0-9a-f]*$", m["name"], re.I):
                continue
            members.setdefault(s["name"], []).append(
                {"offset": int(m["offset"]), "name": _ident(m["name"]), "type": ghidra_type(m.get("type", "u8"), known_types)})

    o = ["# Findings named in Ghidra that GridNode did not know (gridnode.py ghidra-import).",
         "# Review each one, then move it into the annotation file it belongs to (and delete it here).",
         "defaults: { confidence: inferred, sources: [ghidra] }", ""]

    def item(d: dict, indent="  - ") -> list[str]:
        keys = list(d)
        lines = [f"{indent}{keys[0]}: {_yaml(d[keys[0]])}"]
        lines += [f"{' ' * len(indent)}{k}: {_yaml(d[k])}" for k in keys[1:]]
        return lines

    if funcs:
        o.append("functions:")
        for f in sorted(funcs, key=lambda f: f["va"]):
            o += item(f)
    if globs:
        o.append("globals:")
        for g in sorted(globs, key=lambda g: g["va"]):
            o += item(g)
    struct_members = {k: v for k, v in members.items() if k in db.structs}
    class_members = {k: v for k, v in members.items() if k in db.classes}
    if class_members or struct_members:
        o.append("# Members named in Ghidra on GridNode's structs (add them to the class / struct in its own file):")
        for name, ms in sorted({**class_members, **struct_members}.items()):
            o.append(f"#   {name}:")
            for m in sorted(ms, key=lambda m: m["offset"]):
                o.append(f"#     - {{ offset: {m['offset']:#x}, name: {m['name']}, type: \"{m['type']}\" }}")
    if renamed:
        o.append("# Named differently in Ghidra (GridNode's name kept):")
        for va, ours, theirs in sorted(renamed):
            o.append(f"#   {va:#x}: GridNode {ours}, Ghidra {theirs}")
    counts = {"functions": len(funcs), "globals": len(globs), "members": sum(len(v) for v in members.values()),
              "renamed": len(renamed)}
    return "\n".join(o) + "\n", counts


def _yaml(v) -> str:
    if isinstance(v, int) and not isinstance(v, bool):
        return f"{v:#x}"
    s = str(v)
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
