"""Markdown for the docs site (MkDocs): the annotated engine pages (our findings, published) and,
when a dump is given, the full reflected reference (built locally only: reflected data is the
game's and is never published).

Layout written under OUT/docs:
  annotated/index.md, annotated/<file>.md  one page per db/annotations/<file>.yaml
  reference/index.md, reference/<module>.md  one page per engine module, a section per type
and OUT/mkdocs.yml: the repository's mkdocs.yml with those pages added to the navigation.
"""

from __future__ import annotations

import os
import re
import shutil
from collections import defaultdict

from .annotations import Database, Meta, Signature

CONF_BADGE = {"confirmed": "confirmed", "inferred": "inferred", "hypothesis": "hypothesis"}


def _esc(s: str) -> str:
    return str(s).replace("|", "\\|").replace("\n", " ")


def _sig(sig: Signature | None) -> str:
    if sig is None:
        return ""
    params = ", ".join(f"{p.type}{(' ' + p.name) if p.name else ''}" for p in sig.params)
    return f"{sig.returns}({params})"


def _hex(v) -> str:
    return f"0x{v:x}" if isinstance(v, int) else str(v)


def _badge(m: Meta) -> str:
    src = f" · {', '.join(m.sources)}" if m.sources else ""
    return f"`{CONF_BADGE.get(m.confidence, m.confidence)}`{src}"


def _anchor(name: str) -> str:
    return re.sub(r"[^a-z0-9_-]", "", name.lower())


# ---- annotated pages --------------------------------------------------------------------------
def annotated_pages(db: Database, reflected=None) -> dict[str, str]:
    """{relative path: markdown}. Everything here is GridNode's own finding: safe to publish.
    With a dump, class pages link to the class's reference section."""
    by_file: dict[str, dict[str, list]] = defaultdict(lambda: defaultdict(list))
    for kind, table in (("classes", db.classes), ("structs", db.structs), ("globals", db.globals),
                        ("functions", db.functions), ("hooks", db.hooks), ("code", db.code), ("constants", db.constants),
                        ("events", db.events)):
        for item in table.values():
            by_file[os.path.basename(item.meta.file)][kind].append(item)

    pages: dict[str, str] = {}
    index = ["# Annotated engine", "",
             "GridNode's own findings about the engine: runtime class members, virtual functions, globals,",
             "functions and hooks that reflection does not provide. Generated from `db/annotations/*.yaml`",
             "([format](../annotations.md)). Each item shows its confidence and where it came from.", "",
             "| File | Classes | Structs | Globals | Functions | Hooks |", "|---|---|---|---|---|---|"]
    for fn in sorted(by_file):
        k = by_file[fn]
        stem = fn.rsplit(".", 1)[0]
        index.append(f"| [{stem}]({stem}.md) | {len(k['classes'])} | {len(k['structs'])} | {len(k['globals'])} | "
                     f"{len(k['functions'])} | {len(k['hooks'])} |")
        pages[f"annotated/{stem}.md"] = _annotated_file(stem, k, reflected)
    pages["annotated/index.md"] = "\n".join(index) + "\n"
    return pages


def _ref_link(name: str, reflected) -> str:
    if reflected is None:
        return ""
    t = reflected.by_name.get(name)
    if not t:
        return ""
    module = _module_page(t)
    return f" · [reflected]({'../reference/' + module + '.md'}#{_anchor(t['_ident'])})"


def _annotated_file(stem: str, k: dict, reflected) -> str:
    o = [f"# {stem}", "", f"From `db/annotations/{stem}.yaml`.", ""]
    for c in sorted(k["classes"], key=lambda c: c.name):
        o += [f"## {c.name}", "", f"Class · realm `{c.realm}` · {_badge(c.meta)}{_ref_link(c.name, reflected)}"]
        if c.vtable:
            o[-1] += f" · vtable `{_hex(c.vtable)}`"
        if c.meta.note:
            o += ["", c.meta.note]
        if c.members:
            o += ["", "| Offset | Member | Type | Confidence | Note |", "|---|---|---|---|---|"]
            o += [f"| `{_hex(m.offset)}` | `{m.name}` | `{_esc(m.type)}` | {_badge(m.meta)} | {_esc(m.meta.note)} |"
                  for m in sorted(c.members, key=lambda m: m.offset)]
        if c.virtuals:
            o += ["", "| Slot | Virtual | Function | Signature | Note |", "|---|---|---|---|---|"]
            o += [f"| `{_hex(v.offset)}` | `{v.name}` | `{_hex(v.va) if v.va else ''}` | `{_esc(_sig(v.signature))}` | {_esc(v.meta.note)} |"
                  for v in sorted(c.virtuals, key=lambda v: v.offset)]
        if c.properties:
            o += ["", "| Property | Hash | Type | Note |", "|---|---|---|---|"]
            o += [f"| `{p.name}` | `{_hex(p.hash)}` | `{p.type or ''}` | {_esc(p.meta.note)} |" for p in c.properties]
        o.append("")
    for s in sorted(k["structs"], key=lambda s: s.name):
        o += [f"## {s.name}", "", f"Struct · size `{_hex(s.size)}`{' (opaque)' if s.opaque else ''} · {_badge(s.meta)}"]
        if s.meta.note:
            o += ["", s.meta.note]
        if s.members:
            o += ["", "| Offset | Member | Type | Note |", "|---|---|---|---|"]
            o += [f"| `{_hex(m.offset)}` | `{m.name}` | `{_esc(m.type)}` | {_esc(m.meta.note)} |" for m in sorted(s.members, key=lambda m: m.offset)]
        o.append("")
    tables = [
        ("Globals", k["globals"], ["Name", "Address", "Type", "Confidence", "Note"],
         lambda g: [f"`{g.name}`", f"`{_hex(g.va)}`", f"`{_esc(g.type)}`", _badge(g.meta), _esc(g.meta.note)]),
        ("Functions", k["functions"], ["Name", "Address", "Signature", "Confidence", "Note"],
         lambda f: [f"`{f.name}`", f"`{_hex(f.va)}`", f"`{_esc(_sig(f.signature))}`", _badge(f.meta), _esc(f.meta.note)]),
        ("Hooks", k["hooks"], ["Name", "Kind", "Slot", "Original", "Note"],
         lambda h: [f"`{h.name}`", h.kind, f"`{_hex(h.slot)}`", f"`{_hex(h.original)}`", _esc(h.meta.note)]),
        ("Code sites", k["code"], ["Name", "Address", "Note"], lambda c: [f"`{c.name}`", f"`{_hex(c.va)}`", _esc(c.meta.note)]),
        ("Constants", k["constants"], ["Name", "Value", "Note"], lambda c: [f"`{c.name}`", f"`{_hex(c.value)}`", _esc(c.meta.note)]),
        ("Events", k["events"], ["Event", "Hash", "Sent to", "Confidence", "Note"],
         lambda e: [f"`{e.name}`", f"`{_hex(e.hash)}`", ", ".join(f"`{c}`" for c in e.sent_to), _badge(e.meta), _esc(e.meta.note)]),
    ]
    for title, items, head, row in tables:
        if not items:
            continue
        o += [f"## {title}", "", "| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
        o += ["| " + " | ".join(row(x)) + " |" for x in sorted(items, key=lambda x: x.name)]
        o.append("")
    return "\n".join(o) + "\n"


# ---- reflected reference (local only) ------------------------------------------------------------
def _module_page(t: dict) -> str:
    return re.sub(r"[^A-Za-z0-9_-]", "_", t.get("module") or "Unknown")


def reference_pages(reflected, db: Database | None = None) -> dict[str, str]:
    by_module: dict[str, list[dict]] = defaultdict(list)
    for t in reflected.types:
        by_module[_module_page(t)].append(t)
    pages: dict[str, str] = {}
    idx = ["# Reflected reference", "",
           "Every type the engine's reflection describes, from **your own dump** "
           f"(`{os.path.basename(reflected.path) or 'dump'}`, {len(reflected.types)} types). This part of the site",
           "is built locally and never published.", "",
           "| Module | Types |", "|---|---|"]
    for mod in sorted(by_module, key=str.lower):
        idx.append(f"| [{mod}]({mod}.md) | {len(by_module[mod])} |")
        pages[f"reference/{mod}.md"] = _module(mod, by_module[mod], reflected, db)
    pages["reference/index.md"] = "\n".join(idx) + "\n"
    return pages


def _type_link(ti: int, reflected) -> str:
    t = reflected.get(ti) if ti else None
    if not t:
        return f"`{_hex(ti)}`" if ti else ""
    return f"[{t['displayName']}]({_module_page(t)}.md#{_anchor(t['_ident'])})"


def _module(mod: str, types: list[dict], reflected, db) -> str:
    o = [f"# {mod}", ""]
    for t in sorted(types, key=lambda t: (t["displayName"].lower(), t["_ti"])):
        o += [f"## {t['displayName']} {{ #{_anchor(t['_ident'])} }}", ""]
        info = [f"{t['category']}", f"size `{_hex(t.get('size', 0))}`", f"TypeInfo `{t['ti']}`"]
        if t.get("_super"):
            info.append("base " + _type_link(t["_super"], reflected))
        o.append(" · ".join(info))
        if db is not None and (t["displayName"] in db.classes or t["displayName"] in db.structs):
            src = (db.classes.get(t["displayName"]) or db.structs.get(t["displayName"])).meta.file
            o += ["", f"Annotated: [{os.path.basename(src).rsplit('.', 1)[0]}](../annotated/"
                      f"{os.path.basename(src).rsplit('.', 1)[0]}.md#{_anchor(t['displayName'])})"]
        fields = t.get("fields") or []
        if fields:
            o += ["", "| Offset | Field | Type |", "|---|---|---|"]
            o += [f"| `{_hex(f['offset'])}` | `{f['name']}` | {_type_link(f['_type'], reflected)} |"
                  for f in sorted(fields, key=lambda f: (f["offset"], f["name"]))]
        values = t.get("values") or []
        if values:
            o += ["", "| Value | Name |", "|---|---|"]
            o += [f"| {v['value']} | `{v['name']}` |" for v in values]
        o.append("")
    return "\n".join(o) + "\n"


# ---- the site ----------------------------------------------------------------------------------
def write_site(root: str, out: str, db: Database, reflected=None) -> list[str]:
    """Copies docs/ and mkdocs.yml from the repository into OUT, adds the generated pages and their
    navigation entries. Returns the generated page paths."""
    src_docs = os.path.join(root, "docs")
    dst_docs = os.path.join(out, "docs")
    if os.path.isdir(dst_docs):
        shutil.rmtree(dst_docs)
    shutil.copytree(src_docs, dst_docs, ignore=shutil.ignore_patterns("XML Dump", "*.json"))
    pages = annotated_pages(db, reflected)
    if reflected is not None:
        pages.update(reference_pages(reflected, db))
    for rel, text in pages.items():
        path = os.path.join(dst_docs, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    with open(os.path.join(root, "mkdocs.yml"), encoding="utf-8") as f:
        conf = f.read()
    ann = sorted(p for p in pages if p.startswith("annotated/") and not p.endswith("index.md"))
    nav = ["  - Annotated engine:", "      - annotated/index.md"] + [f"      - annotated/{os.path.basename(p)}" for p in ann]
    if reflected is not None:
        ref = sorted((p for p in pages if p.startswith("reference/") and not p.endswith("index.md")), key=str.lower)
        nav += ["  - Reflected reference (local):", "      - reference/index.md"] + [f"      - {p}" for p in ref]
    conf = conf.replace("  # @generated-nav", "\n".join(nav))
    with open(os.path.join(out, "mkdocs.yml"), "w", encoding="utf-8", newline="\n") as f:
        f.write(conf)
    return sorted(pages)
