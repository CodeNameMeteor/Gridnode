"""gridnode.py assets: the game's data files, read where they are.

  assets check --data DIR                      every catalogue entry parses, every bundle lists, the
                                               compression codecs found are printed (plan step 1's test)
  assets list --data DIR [--bundle B] [--type ebx|res|chunks] [--limit N]
  assets extract NAME --data DIR [--out DIR]   one ebx / res entry (or a chunk by GUID), decompressed

DIR is the game's Data folder (gridnode.log: "startup: game data from ..."), or the GRIDNODE_DATA
environment variable. Extracted files go to bin/assets/ (git-ignored): game data is never committed.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections import Counter

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
    print(f"catalogues: {len(s.catalogues)}, {entries} entries, {patches} patch entries "
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
    if problems or unknown:
        print(f"check: {len(problems)} superbundle(s) did not read, {len(unknown)} unknown codec(s)", file=sys.stderr)
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
    a = ap.parse_args(argv)
    try:
        return {"check": cmd_check, "list": cmd_list, "extract": cmd_extract}[a.cmd](a)
    except StoreError as e:
        print(f"assets: {e}", file=sys.stderr)
        return 1
