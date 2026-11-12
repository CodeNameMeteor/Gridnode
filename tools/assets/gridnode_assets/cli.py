"""gridnode.py assets: the game's data files, read where they are.

  assets check --data DIR                      every catalogue entry parses, every bundle lists, the
                                               compression codecs found are printed (plan step 1's test)
  assets list --data DIR [--bundle B] [--type ebx|res|chunks] [--limit N]
  assets extract NAME --data DIR [--out DIR]   one ebx / res entry (or a chunk by GUID), decompressed
  assets ebx NAME|FILE.ebx [--data DIR] [--out DIR|-]
                                               an EBX file as YAML (lossless: written back it is the same)
  assets ebx-check --data DIR [--superbundle S] [--bundle B] [--limit N] [--dump JSON]
                                               every EBX read, to YAML, back and written: same bytes?
                                               (and, with the reflected dump, its types against the game's)
  assets ebx-build FILE.yaml [--out FILE.ebx] [--dump JSON]
                                               a YAML file written as EBX, its types checked against the game's

DIR is the game's Data folder (gridnode.log: "startup: game data from ..."), or the GRIDNODE_DATA
environment variable. Extracted files go to bin/assets/ (git-ignored): game data is never committed.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections import Counter

from . import ebx
from .store import Store, StoreError, bundles_of, read_toc

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT = os.path.join(ROOT, "bin", "assets")


def _store(a) -> Store:
    data = a.data or os.environ.get("GRIDNODE_DATA")
    if not data:
        raise StoreError("give the game's Data folder: --data DIR (or set GRIDNODE_DATA)")
    return Store(data)


def _all_bundles(s: Store, problems: list[str] | None = None):
    for toc_path, sb_path in s.superbundles():
        try:
            toc = read_toc(toc_path)
            if sb_path:
                yield from bundles_of(sb_path, toc)
        except StoreError as e:
            if problems is None:
                raise
            problems.append(str(e))


def cmd_check(a) -> int:
    s = _store(a)
    entries = sum(len(c.entries) for c in s.catalogues)
    patches = sum(len(c.patches) for c in s.catalogues)
    layouts = Counter(c.counts_layout for c in s.catalogues)
    for p in s.problems:
        print("catalogue problem:", p, file=sys.stderr)
    print(f"catalogues: {len(s.catalogues)} read, {len(s.problems)} not, {entries} entries, {patches} patch entries "
          f"(counts after the magic: {dict(layouts)}), {len(s.by_sha1)} distinct SHA-1s")
    problems: list[str] = []
    nb = 0
    kinds: Counter = Counter()
    missing = 0
    for b in _all_bundles(s, problems):
        nb += 1
        for e in b.entries:
            kinds[e.kind] += 1
            missing += e.sha1 not in s.by_sha1 if e.sha1 else 0
    print(f"superbundles: {len(s.superbundles())}, bundles: {nb}, entries: {dict(kinds)}, not in any catalogue: {missing}")
    codecs = s.codecs(a.sample)
    print(f"payload blocks by codec{' (first ' + str(a.sample) + ' entries)' if a.sample else ''}: {dict(codecs)}")
    unknown = [k for k in codecs if k.startswith("unknown")]
    for p in problems[:20]:
        print("problem:", p, file=sys.stderr)
    if problems or unknown or s.problems:
        print(f"check: {len(s.problems)} catalogue(s) and {len(problems)} superbundle(s) did not read, {len(unknown)} unknown codec(s)",
              file=sys.stderr)
        return 1
    print("check: passed")
    return 0


def cmd_list(a) -> int:
    s = _store(a)
    shown = 0
    for b in _all_bundles(s):
        if a.bundle and a.bundle.lower() not in b.name.lower():
            continue
        for e in b.entries:
            if a.type and e.kind != a.type:
                continue
            extra = f"  resType {e.res_type:#010x}" if e.kind == "res" else ""
            print(f"{e.kind:6} {e.name}  {e.size} bytes  [{b.superbundle}: {b.name}]{extra}")
            shown += 1
            if a.limit and shown >= a.limit:
                return 0
    return 0


def cmd_extract(a) -> int:
    s = _store(a)
    want = a.name.lower()
    for b in _all_bundles(s):
        for e in b.entries:
            if e.name.lower() != want or not e.sha1:
                continue
            data = s.read(e.sha1)
            out_dir = a.out or OUT
            path = os.path.join(out_dir, e.kind, e.name.replace("/", os.sep) + "." + ("ebx" if e.kind == "ebx" else e.kind.rstrip("s")))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            print(f"{e.kind} {e.name}: {len(data)} bytes -> {path}")
            return 0
    print(f"extract: {a.name} is in no bundle", file=sys.stderr)
    return 1


def _ebx_bytes(a) -> tuple[str, bytes]:
    if a.name.lower().endswith(".ebx") and os.path.isfile(a.name):
        with open(a.name, "rb") as f:
            return os.path.splitext(os.path.basename(a.name))[0], f.read()
    s = _store(a)
    want = a.name.lower()
    for b in _all_bundles(s):
        for e in b.entries:
            if e.kind == "ebx" and e.name.lower() == want and e.sha1:
                return e.name, s.read(e.sha1)
    raise StoreError(f"{a.name} is no EBX in any bundle (or .ebx file)")


def cmd_ebx(a) -> int:
    name, data = _ebx_bytes(a)
    text = ebx.to_yaml(ebx.read(data))
    if a.out == "-":
        sys.stdout.write(text)
        return 0
    path = os.path.join(a.out or os.path.join(OUT, "yaml"), name.replace("/", os.sep) + ".yaml")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    same, why = ebx.round_trip(data)
    print(f"ebx {name}: {len(data)} bytes -> {path} ({'written back the same' if same else 'NOT the same written back: ' + why})")
    return 0 if same else 1


DUMP = os.path.join(ROOT, "db", "reflected", "gridnode-reflected.json")


def _dump(a):
    path = a.dump or (DUMP if os.path.isfile(DUMP) else "")
    return ebx.load_dump(path) if path else None


def cmd_ebx_build(a) -> int:
    with open(a.file, encoding="utf-8") as f:
        x = ebx.from_yaml(f.read())
    dump = _dump(a)
    errors: list[str] = []
    if dump is None:
        print("ebx-build: no reflected dump (--dump, or gridnode.py setup): types not checked against the game's", file=sys.stderr)
    else:
        errors, warnings = ebx.check_types(x, dump)
        for w in warnings:
            print("warning:", w, file=sys.stderr)
        for e in errors:
            print("error:", e, file=sys.stderr)
    if errors:
        print(f"ebx-build: {len(errors)} error(s): not written", file=sys.stderr)
        return 1
    data = ebx.write(x)
    out = a.out or os.path.join(OUT, "built", os.path.splitext(os.path.basename(a.file))[0] + ".ebx")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    print(f"ebx-build: {len(x.instances)} instances, {len(data)} bytes -> {out}")
    return 0


def cmd_ebx_check(a) -> int:
    s = _store(a)
    dump = _dump(a)
    type_errors: Counter = Counter()
    seen: set[bytes] = set()
    passed = failed = absent = 0
    reasons: Counter = Counter()
    examples: dict[str, str] = {}
    raw_codes: Counter = Counter()
    for b in _all_bundles(s):
        if a.superbundle and a.superbundle.lower() not in b.superbundle.lower():
            continue
        if a.bundle and a.bundle.lower() not in b.name.lower():
            continue
        for e in b.entries:
            if e.kind != "ebx" or not e.sha1 or e.sha1 in seen:
                continue
            seen.add(e.sha1)
            where = s.by_sha1.get(e.sha1)
            if where is not None and not os.path.isfile(where.archive_path()):
                absent += 1  # a copy of the install with some archives left out
                continue
            try:
                data = s.read(e.sha1)
                x = ebx.read(data)
                for t in x.types:
                    for fl in t.fields:
                        c = ebx.code_of(fl.flags)
                        if c not in ebx._SIZES and c not in (ebx.C_INHERITED, ebx.C_STRUCT):
                            raw_codes[c] += 1
                same, why = ebx.round_trip(data)
                if dump is not None:
                    for err in ebx.check_types(x, dump)[0]:
                        type_errors[err] += 1
            except Exception as ex:  # noqa: BLE001 - every failure is counted and shown
                same, why = False, f"{type(ex).__name__}: {ex}"
            if same:
                passed += 1
            else:
                failed += 1
                key = why.split(";")[-1].strip() if "difference" in why else why[:120]
                key = "differs" if "difference" in why else key
                reasons[key] += 1
                examples.setdefault(key, f"{e.name}: {why}")
            if a.limit and passed + failed >= a.limit:
                break
        if a.limit and passed + failed >= a.limit:
            break
    print(f"ebx-check: {passed + failed} EBX files, {passed} written back the same, {failed} not"
          + (f" ({absent} in archives not in this folder)" if absent else ""))
    if raw_codes:
        print(f"type codes kept as raw bytes (fields per code): {dict(raw_codes)}")
    for k, n in reasons.most_common(20):
        print(f"  {n:6}  {examples[k]}")
    if dump is not None:
        print(f"types against the game's: {sum(type_errors.values())} disagreement(s) in {len(type_errors)} place(s)")
        for k, n in type_errors.most_common(20):
            print(f"  {n:6}  {k}")
    return 0 if failed == 0 and passed and not type_errors else 1


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="gridnode.py assets", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check", help="read every catalogue and bundle; print the codecs found")
    c.add_argument("--data")
    c.add_argument("--sample", type=int, default=0, help="look at the codecs of only the first N entries (0: all)")
    li = sub.add_parser("list", help="the entries of the bundles")
    li.add_argument("--data")
    li.add_argument("--bundle", help="only bundles whose name contains this")
    li.add_argument("--type", choices=["ebx", "res", "chunks"])
    li.add_argument("--limit", type=int, default=0)
    ex = sub.add_parser("extract", help="one entry, decompressed, into bin/assets")
    ex.add_argument("name")
    ex.add_argument("--data")
    ex.add_argument("--out")
    eb = sub.add_parser("ebx", help="an EBX file as YAML, into bin/assets/yaml")
    eb.add_argument("name", help="the EBX's name in its bundle, or a .ebx file")
    eb.add_argument("--data")
    eb.add_argument("--out", help="a folder, or - for the screen")
    ec = sub.add_parser("ebx-check", help="every EBX written back from its YAML: the same bytes?")
    ec.add_argument("--data")
    ec.add_argument("--superbundle", help="only superbundles whose name contains this")
    ec.add_argument("--bundle", help="only bundles whose name contains this")
    ec.add_argument("--limit", type=int, default=0)
    ec.add_argument("--dump", help="the reflected dump (default: db/reflected/gridnode-reflected.json when there)")
    bu = sub.add_parser("ebx-build", help="a YAML file written as EBX, into bin/assets/built")
    bu.add_argument("file")
    bu.add_argument("--out")
    bu.add_argument("--dump", help="the reflected dump (default: db/reflected/gridnode-reflected.json when there)")
    a = ap.parse_args(argv)
    try:
        return {"check": cmd_check, "list": cmd_list, "extract": cmd_extract, "ebx": cmd_ebx,
                "ebx-check": cmd_ebx_check, "ebx-build": cmd_ebx_build}[a.cmd](a)
    except (StoreError, ebx.EbxError) as e:
        print(f"assets: {e}", file=sys.stderr)
        return 1
