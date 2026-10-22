"""Compare two sets of annotations (gridnode.py annotations-diff).

Both sides are read as YAML (no validation), so a set that does not load yet can still be compared.
Entries are matched by section and name; functions, globals and code sites also by address, so a
symbol renamed at the same address shows as a rename and not as one removed and one added.

  diff(base_dir, other_dir) -> Diff     what differs, entry by entry
  report(d)                 -> str      readable text
  inbox(d)                  -> str      YAML with the other side's new and changed entries, to review
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from dataclasses import dataclass, field

import yaml

SYMBOL_SECTIONS = ("functions", "globals", "code", "hooks", "constants")
TYPE_SECTIONS = ("classes", "structs")
META_KEYS = ("note", "confidence", "sources")
ADDRESS_KEYS = {"functions": "va", "globals": "va", "code": "va", "hooks": "slot"}


@dataclass
class Entry:
    section: str
    name: str
    file: str
    data: dict
    defaults: dict


@dataclass
class Change:
    section: str
    name: str
    base: Entry | None
    other: Entry | None
    what: list[str] = field(default_factory=list)   # what differs, one line each


@dataclass
class Diff:
    base_label: str
    other_label: str
    added: list[Change] = field(default_factory=list)     # only in the other set
    removed: list[Change] = field(default_factory=list)   # only in the base set
    renamed: list[Change] = field(default_factory=list)   # same address, other name
    changed: list[Change] = field(default_factory=list)
    moved: list[Change] = field(default_factory=list)     # same entry, another file
    errors: list[str] = field(default_factory=list)


def load_raw(directory: str) -> tuple[dict[tuple[str, str], Entry], list[str]]:
    entries: dict[tuple[str, str], Entry] = {}
    errors: list[str] = []
    for fn in sorted(f for f in os.listdir(directory) if f.endswith((".yaml", ".yml"))):
        try:
            with open(os.path.join(directory, fn), encoding="utf-8") as f:
                doc = yaml.safe_load(f) or {}
        except yaml.YAMLError as e:
            errors.append(f"{fn}: YAML error: {e}")
            continue
        if not isinstance(doc, dict):
            errors.append(f"{fn}: top level is not a mapping")
            continue
        defaults = doc.get("defaults") or {}
        for section in SYMBOL_SECTIONS + TYPE_SECTIONS:
            for item in doc.get(section) or []:
                if not isinstance(item, dict) or not isinstance(item.get("name"), str):
                    continue
                key = (section, item["name"])
                if key in entries:
                    errors.append(f"{fn}: {section} {item['name']} also in {entries[key].file}")
                    continue
                entries[key] = Entry(section, item["name"], fn, item, defaults)
    return entries, errors


def checkout(ref: str, repo: str, subdir: str) -> str:
    """The annotations at a git ref, in a temporary folder."""
    out = tempfile.mkdtemp(prefix="gridnode-ann-")
    names = subprocess.run(["git", "-C", repo, "ls-tree", "--name-only", f"{ref}:{subdir}"], check=True,
                           capture_output=True, text=True).stdout.split()
    for n in names:
        if n.endswith((".yaml", ".yml")):
            text = subprocess.run(["git", "-C", repo, "show", f"{ref}:{subdir}/{n}"], check=True, capture_output=True).stdout
            with open(os.path.join(out, n), "wb") as f:
                f.write(text)
    return out


def _fmt(v) -> str:
    if isinstance(v, bool):
        return str(v).lower()
    if isinstance(v, int):
        return f"{v:#x}"
    if isinstance(v, list):
        return "[" + ", ".join(_fmt(x) for x in v) + "]"
    return repr(v) if isinstance(v, str) else str(v)


def _meta(e: Entry, key: str):
    if key in e.data:
        return e.data[key]
    if key in e.defaults:
        return e.defaults[key]
    return "confirmed" if key == "confidence" else ([] if key == "sources" else None)


def _compare_fields(a: Entry, b: Entry, skip=("name",), prefix="") -> list[str]:
    out = []
    for k in sorted(set(a.data) | set(b.data)):
        if k in skip or k in META_KEYS:
            continue
        x, y = a.data.get(k), b.data.get(k)
        if x != y:
            out.append(f"{prefix}{k}: {_fmt(x)} -> {_fmt(y)}")
    for k in META_KEYS:
        x, y = _meta(a, k), _meta(b, k)
        if x != y:
            if k == "note":
                out.append(f"{prefix}note changed")
            else:
                out.append(f"{prefix}{k}: {_fmt(x)} -> {_fmt(y)}")
    return out


def _compare_list(a: list, b: list, key: str, what: str) -> list[str]:
    """Members (by offset), virtuals (by offset) or properties (by name)."""
    am = {m.get(key): m for m in a or [] if isinstance(m, dict)}
    bm = {m.get(key): m for m in b or [] if isinstance(m, dict)}
    out = []
    for k in sorted(set(am) | set(bm), key=lambda v: (isinstance(v, str), v)):
        label = f"{what} {_fmt(k)}"
        if k not in bm:
            out.append(f"{label} {am[k].get('name', '')}: only in base")
        elif k not in am:
            out.append(f"{label} {bm[k].get('name', '')}: only in other ({_fmt(bm[k].get('type', bm[k].get('signature', '')))})")
        elif am[k] != bm[k]:
            for f in sorted(set(am[k]) | set(bm[k])):
                if f != key and am[k].get(f) != bm[k].get(f):
                    out.append(f"{label} {am[k].get('name', bm[k].get('name', ''))}: {f} {_fmt(am[k].get(f))} -> {_fmt(bm[k].get(f))}")
    return out


def _compare(a: Entry, b: Entry) -> list[str]:
    if a.section not in TYPE_SECTIONS:
        return _compare_fields(a, b)
    out = _compare_fields(a, b, skip=("name", "members", "virtuals", "properties"))
    out += _compare_list(a.data.get("members"), b.data.get("members"), "offset", "member")
    out += _compare_list(a.data.get("virtuals"), b.data.get("virtuals"), "offset", "virtual")
    out += _compare_list(a.data.get("properties"), b.data.get("properties"), "name", "property")
    return out


def diff(base_dir: str, other_dir: str, base_label="base", other_label="other") -> Diff:
    a, ea = load_raw(base_dir)
    b, eb = load_raw(other_dir)
    d = Diff(base_label, other_label, errors=[f"{base_label}: {e}" for e in ea] + [f"{other_label}: {e}" for e in eb])
    only_a = {k: v for k, v in a.items() if k not in b}
    only_b = {k: v for k, v in b.items() if k not in a}
    # Same address, another name: a rename.
    by_addr = {}
    for k, e in only_a.items():
        ak = ADDRESS_KEYS.get(e.section)
        if ak and isinstance(e.data.get(ak), int):
            by_addr[(e.section, e.data[ak])] = k
    for k, e in list(only_b.items()):
        ak = ADDRESS_KEYS.get(e.section)
        hit = by_addr.get((e.section, e.data.get(ak))) if ak else None
        if hit and hit in only_a:
            old = only_a.pop(hit)
            del only_b[k]
            c = Change(e.section, e.name, old, e, [f"was {old.name}"] + _compare_fields(old, e, skip=("name",)))
            d.renamed.append(c)
    d.removed = [Change(k[0], k[1], e, None) for k, e in sorted(only_a.items())]
    d.added = [Change(k[0], k[1], None, e) for k, e in sorted(only_b.items())]
    for k in sorted(set(a) & set(b)):
        what = _compare(a[k], b[k])
        if what:
            d.changed.append(Change(k[0], k[1], a[k], b[k], what))
        if a[k].file != b[k].file:
            d.moved.append(Change(k[0], k[1], a[k], b[k], [f"{a[k].file} -> {b[k].file}"]))
    return d


def _where(e: Entry) -> str:
    ak = ADDRESS_KEYS.get(e.section)
    at = f" @ {e.data[ak]:#x}" if ak and isinstance(e.data.get(ak), int) else ""
    return f"{at}  ({e.file})"


def report(d: Diff) -> str:
    o = [f"annotations: {d.base_label} -> {d.other_label}",
         f"  {len(d.added)} only in {d.other_label}, {len(d.removed)} only in {d.base_label}, "
         f"{len(d.renamed)} renamed, {len(d.changed)} changed, {len(d.moved)} in another file", ""]
    for e in d.errors:
        o.append(f"problem: {e}")
    if d.errors:
        o.append("")

    def section(title: str, items: list[Change], side: str, details=True):
        if not items:
            return
        o.append(f"== {title} ({len(items)})")
        for c in items:
            e = c.other if side == "other" else c.base
            o.append(f"  {c.section[:-1] if c.section != 'code' else 'code'} {c.name}{_where(e)}")
            if details:
                o.extend(f"      {w}" for w in c.what)
        o.append("")

    section(f"Only in {d.other_label}", d.added, "other", details=False)
    section(f"Only in {d.base_label}", d.removed, "base", details=False)
    section("Renamed (same address)", d.renamed, "other")
    section("Changed", d.changed, "other")
    section("In another file", d.moved, "other")
    if not (d.added or d.removed or d.renamed or d.changed or d.moved):
        o.append("No differences.")
    return "\n".join(o) + "\n"


class _HexDumper(yaml.SafeDumper):
    pass


_HexDumper.add_representer(int, lambda dumper, v: dumper.represent_scalar(
    "tag:yaml.org,2002:int", f"{v:#x}" if v >= 0x100 else str(v)))
_HexDumper.add_representer(bool, yaml.SafeDumper.represent_bool)


def inbox(d: Diff) -> str:
    """The other side's new, renamed and changed entries as one YAML file to review. Each keeps its
    own confidence and sources (its file's defaults filled in)."""
    sections: dict[str, list] = {}
    for c in d.added + d.renamed + d.changed:
        e = c.other
        item = dict(e.data)
        for k in ("confidence", "sources"):
            if k not in item and k in e.defaults:
                item[k] = e.defaults[k]
        why = "new" if c in d.added else ("renamed: " if c in d.renamed else "changed: ") + "; ".join(c.what[:4])
        item["note"] = (str(item.get("note", "")) + f" [{d.other_label} {e.file}: {why}]").strip()
        sections.setdefault(e.section, []).append(item)
    head = (f"# {d.other_label}'s annotations that differ from {d.base_label} (gridnode.py annotations-diff --inbox).\n"
            "# Review each one, then move it into the annotation file it belongs to (replacing the old entry)\n"
            "# and delete it here. This file does not load next to the annotations as it is: names repeat.\n")
    body = yaml.dump({k: sections[k] for k in sorted(sections)}, Dumper=_HexDumper, sort_keys=False, allow_unicode=True,
                     width=120)
    return head + body
