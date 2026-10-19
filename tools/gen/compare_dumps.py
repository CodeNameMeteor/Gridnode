#!/usr/bin/env python3
"""Compare a GridNode reflected dump with a FrostbiteTypeDumper (Ghidra) dump.

    python tools/gen/compare_dumps.py gridnode-reflected.json fb_types.json

Types are matched by their runtime TypeInfo address. Everything the two dumps both describe is
compared: name, category, size, alignment, flags, module, runtime id, super class, array element,
fields (name, offset, flags, type), enum values, class id range, getType and vtables, creators.

Exit code 0 when every "core" property (layout and identity) matches, 1 otherwise. Differences in
getType / vtables / creators are reported but only fail with --strict, because the two tools find
those in different ways (see docs/dump-format.md).
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict

CORE = ("name", "category", "size", "alignment", "flags", "module", "runtimeId", "super", "element",
        "fields", "values", "classId", "lastSubClassId")
METHODS = ("getType", "vtables", "creators")


def norm_addr(v):
    if v in (None, "", 0):
        return None
    return int(v, 16) if isinstance(v, str) else int(v)


def load_gridnode(path):
    d = json.load(open(path, encoding="utf-8"))
    if d.get("format") != "gridnode-reflected":
        sys.exit(f"{path}: not a gridnode-reflected file")
    out = {}
    for t in d["types"]:
        r = {
            "name": t.get("displayName") or t.get("name", ""),
            "category": t["category"],
            "size": t["size"],
            "alignment": t["alignment"],
            "flags": t["flags"],
            "module": t.get("module", ""),
            "runtimeId": t.get("runtimeId"),
            "super": norm_addr(t.get("super")),
            "element": norm_addr(t.get("element")),
            "fields": tuple((f["name"], f["offset"], f["flags"], norm_addr(f.get("type"))) for f in t.get("fields", [])),
            "values": tuple((v["name"], v["value"]) for v in t.get("values", [])),
            "classId": t.get("classId"),
            "lastSubClassId": t.get("lastSubClassId"),
            "getType": norm_addr(t.get("getType")),
            "vtables": tuple(sorted(norm_addr(v) for v in t.get("vtables", []))),
            "creators": tuple(sorted((norm_addr(c["creator"]), c["realm"], norm_addr(c["data"])) for c in t.get("creators", []))),
        }
        out[norm_addr(t["ti"])] = r
    return d, out


def load_ghidra(path):
    d = json.load(open(path, encoding="utf-8"))
    ident_ti = {t.get("ident"): norm_addr(t["ti"]) for t in d["types"]}
    out = {}
    for t in d["types"]:
        cat = t["category"]
        fields, values = (), ()
        if cat == "Enum":
            values = tuple((f["name"], f.get("value", 0)) for f in t.get("fields", []))
        else:
            fields = tuple((f["name"], f["offset"], f["flags"], norm_addr(f.get("typeTi"))) for f in t.get("fields", []))
        creators = tuple(sorted((norm_addr(c.get("creator")), c.get("realm"), ident_ti.get(c.get("dataIdent")))
                                for c in t.get("createdFrom", [])))
        out[norm_addr(t["ti"])] = {
            "name": t.get("name", ""),
            "category": cat,
            "size": t["size"],
            "alignment": t["alignment"],
            "flags": t["flags"],
            "module": t.get("module", ""),
            "runtimeId": t.get("runtimeId"),
            "super": norm_addr(t.get("superTi")),
            "element": norm_addr(t.get("elementTi")),
            "fields": fields,
            "values": values,
            "classId": t.get("classId"),
            "lastSubClassId": t.get("lastSubClassId"),
            "getType": norm_addr(t.get("getType")),
            "vtables": tuple(sorted(norm_addr(v) for v in t.get("vftables", []))),
            "creators": creators,
        }
    return d, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gridnode")
    ap.add_argument("ghidra")
    ap.add_argument("--strict", action="store_true", help="also fail on getType / vtable / creator differences")
    ap.add_argument("--examples", type=int, default=5, help="examples to print per property")
    a = ap.parse_args()

    gd, g = load_gridnode(a.gridnode)
    hd, h = load_ghidra(a.ghidra)

    print(f"gridnode: {len(g)} types   ghidra: {len(h)} types")
    only_g = sorted(set(g) - set(h))
    only_h = sorted(set(h) - set(g))
    if only_g:
        print(f"  only in gridnode: {len(only_g)}, e.g. " + ", ".join(f"{hex(x)} {g[x]['name']}" for x in only_g[:a.examples]))
    if only_h:
        print(f"  only in ghidra:   {len(only_h)}, e.g. " + ", ".join(f"{hex(x)} {h[x]['name']}" for x in only_h[:a.examples]))

    diffs = defaultdict(list)
    extra_vtables = []  # gridnode lists every vtable candidate; ghidra's are a subset: not a failure
    for ti in sorted(set(g) & set(h)):
        for k in CORE + METHODS:
            gv, hv = g[ti][k], h[ti][k]
            if k == "name" and not hv:
                continue
            if gv != hv:
                if k == "vtables" and set(hv) < set(gv):
                    extra_vtables.append((ti, sorted(set(gv) - set(hv))))
                    continue
                diffs[k].append((ti, gv, hv))

    common = len(set(g) & set(h))
    failed = bool(only_g or only_h)
    for k in CORE + METHODS:
        n = len(diffs[k])
        status = "ok" if n == 0 else f"{n} differ"
        print(f"{k:15s} {common - n:6d}/{common} match  {status}")
        for ti, gv, hv in diffs[k][:a.examples]:
            gs, hs = str(gv), str(hv)
            if len(gs) > 160:
                gs = gs[:160] + "..."
            if len(hs) > 160:
                hs = hs[:160] + "..."
            print(f"    {hex(ti)} {h[ti]['name'] or g[ti]['name']}\n      gridnode: {gs}\n      ghidra:   {hs}")
        if n and (k in CORE or a.strict):
            failed = True

    if extra_vtables:
        print(f"extra vtable candidates (gridnode lists more vtables sharing the type's getType): {len(extra_vtables)}")
        for ti, extra in extra_vtables[:a.examples]:
            print(f"    {hex(ti)} {h[ti]['name'] or g[ti]['name']}: " + ", ".join(hex(x) for x in extra))

    cats_g = Counter(t["category"] for t in g.values())
    print("categories:", dict(sorted(cats_g.items())))
    print("RESULT:", "FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
