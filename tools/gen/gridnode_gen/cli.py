"""gridnode: command line for the GridNode type database.

  gridnode validate [--reflected FILE]        check the annotations (and against a dump)
  gridnode engine --out FILE                  write gridnode/engine.hpp (annotations only)
  gridnode types --reflected FILE --out FILE  write gridnode/generated/types.hpp
  gridnode setup --dump FILE                  install your dump and generate types.hpp
  gridnode verify-image FILE                  check the annotations against a PE dump of the game
  gridnode compare GRIDNODE_JSON GHIDRA_JSON  compare with a FrostbiteTypeDumper dump
  gridnode rust [--out FILE] [--check]       Rust bindings (bindings/rust/gridnode/src/sys.rs) from api.h
  gridnode hash NAME...                       Frostbite name hash of each name
  gridnode unhash HASH... [--image EXE]       names for hashes (or a file of hashes, one per line)
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys

from . import __version__, annotations, emit_engine, emit_types, reflected, validate

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
ANNOTATIONS = os.path.join(ROOT, "db", "annotations")
DEFAULT_DUMP = os.path.join(ROOT, "db", "reflected", "gridnode-reflected.json")
TYPES_OUT = os.path.join(ROOT, "sdk", "include", "gridnode", "generated", "types.hpp")


class UserError(Exception):
    pass


def _find(path: str | None, what: str) -> str | None:
    """A path as given (relative to the current folder), else relative to the repository root, so
    the commands work from any folder."""
    if path is None:
        return None
    for candidate in (path, os.path.join(ROOT, path)):
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)
    raise UserError(f"{what} not found: {path}\n  (looked in {os.getcwd()} and in the repository root {ROOT})")


def _resolve(path: str) -> str:
    return path if os.path.exists(path) else os.path.join(ROOT, path)


def _load(ann_dir: str, reflected_path: str | None, quiet=False):
    db, p = annotations.load(ann_dir)
    r = reflected.Reflected.load(reflected_path) if reflected_path else None
    p.extend(validate.check(db, r))
    if not quiet or p.errors:
        for e in p.errors:
            print("error:", e, file=sys.stderr)
        for w in p.warnings:
            print("warning:", w, file=sys.stderr)
    return db, r, p


def _write(path: str, text: str):
    """Writes only when the content changed, so builds do not redo work."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return False
    except FileNotFoundError:
        pass
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)
    return True


def main(argv=None) -> int:
    try:
        return _main(argv)
    except UserError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


def _main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gridnode", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--annotations", default=ANNOTATIONS, help="annotation folder (default: db/annotations)")
    ap.add_argument("--version", action="version", version=f"gridnode-gen {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    v = sub.add_parser("validate", help="check the annotations")
    v.add_argument("--reflected", help="also check against this dump")
    v.add_argument("--strict", action="store_true", help="warnings count as errors")

    e = sub.add_parser("engine", help="write engine.hpp")
    e.add_argument("--out", required=True)

    t = sub.add_parser("types", help="write types.hpp from a dump")
    t.add_argument("--reflected", default=DEFAULT_DUMP)
    t.add_argument("--out", default=TYPES_OUT)

    s = sub.add_parser("setup", help="install your dump into db/reflected and generate types.hpp")
    s.add_argument("--dump", required=True, help="gridnode-reflected.json written by gridnode.dll")

    vi = sub.add_parser("verify-image", help="check the annotations against a PE dump of the game")
    vi.add_argument("image")

    c = sub.add_parser("compare", help="compare a GridNode dump with a FrostbiteTypeDumper dump")
    c.add_argument("gridnode")
    c.add_argument("ghidra")
    c.add_argument("--strict", action="store_true")

    rs = sub.add_parser("rust", help="write the Rust bindings from sdk/include/gridnode/api.h")
    rs.add_argument("--out", default=os.path.join(ROOT, "bindings", "rust", "gridnode", "src", "sys.rs"))
    rs.add_argument("--check", action="store_true", help="fail if the file is not up to date (CI)")

    h = sub.add_parser("hash", help="Frostbite name hash (h = 5381; h = h*33 ^ c) of each name")
    h.add_argument("names", nargs="+")

    u = sub.add_parser("unhash", help="look hashes up in GridNode's dictionary")
    u.add_argument("hashes", nargs="+", help="hex (0x...) or decimal (Frosty shows signed ints), or @file with one per line")
    u.add_argument("--reflected", default=DEFAULT_DUMP, help="dump whose names go in the dictionary")
    u.add_argument("--image", help="also every string in this PE dump of the game")
    u.add_argument("--words", action="append", default=[], help="extra word list (one name per line)")
    u.add_argument("--unknown", action="store_true", help="print only the hashes that were not found")

    a = ap.parse_args(argv)

    if a.cmd == "rust":
        from . import emit_rust
        with open(os.path.join(ROOT, "sdk", "include", "gridnode", "api.h"), encoding="utf-8") as f:
            text = emit_rust.emit(f.read())
        if a.check:
            try:
                with open(a.out, encoding="utf-8") as f:
                    same = f.read() == text
            except FileNotFoundError:
                same = False
            print(f"{a.out}: {'up to date' if same else 'OUT OF DATE (run gridnode.py rust)'}")
            return 0 if same else 1
        changed = _write(a.out, text)
        print(f"sys.rs {'written' if changed else 'unchanged'}: {a.out}")
        return 0

    if a.cmd == "hash":
        from .hashes import name_hash
        for n in a.names:
            hv = name_hash(n)
            signed = hv - (1 << 32) if hv & 0x80000000 else hv
            print(f"0x{hv:08x}  {signed:>11}  {n}")
        return 0

    if a.cmd == "unhash":
        from . import hashes
        values: list[str] = []
        for x in a.hashes:
            if x.startswith("@"):
                with open(_find(x[1:], "hash list"), encoding="utf-8") as f:
                    values += [ln.split("#")[0].strip() for ln in f if ln.split("#")[0].strip()]
            else:
                values.append(x)
        dump = _find(a.reflected, "dump") if a.reflected and os.path.exists(_resolve(a.reflected)) else None
        db, r, _ = _load(a.annotations, dump, quiet=True)
        image = None
        if a.image:
            with open(_find(a.image, "image"), "rb") as f:
                image = f.read()
        d = hashes.dictionary(reflected=r, db=db, image=image, word_files=[_find(w, "word list") for w in a.words])
        found = 0
        for v in values:
            hv = hashes.parse_hash(v)
            name = d.get(hv)
            found += name is not None
            if name is None or not a.unknown:
                print(f"0x{hv:08x}  {name if name is not None else '?'}")
        print(f"{found} of {len(values)} found ({len(d)} names in the dictionary)", file=sys.stderr)
        return 0

    if a.cmd == "validate":
        db, r, p = _load(a.annotations, _find(a.reflected, "dump"))
        n = len(db.classes) + len(db.structs) + len(db.globals) + len(db.functions) + len(db.hooks)
        print(f"{len(db.files)} files, {len(db.classes)} classes, {len(db.structs)} structs, {len(db.globals)} globals, "
              f"{len(db.functions)} functions, {len(db.hooks)} hooks ({n} items): "
              f"{len(p.errors)} errors, {len(p.warnings)} warnings" + (" (checked against the dump)" if r else ""))
        return 1 if p.errors or (a.strict and p.warnings) else 0

    if a.cmd == "engine":
        db, _, p = _load(a.annotations, None, quiet=True)
        if p.errors:
            return 1
        changed = _write(a.out, emit_engine.emit(db))
        print(f"engine.hpp {'written' if changed else 'unchanged'}: {a.out}")
        return 0

    if a.cmd in ("types", "setup"):
        dump = _find(a.reflected if a.cmd == "types" else a.dump,
                     "dump (make one with gridnode.dll, see docs/dump-format.md)")
        db, r, p = _load(a.annotations, dump)
        if p.errors:
            print("fix the errors above first", file=sys.stderr)
            return 1
        if a.cmd == "setup":
            if os.path.abspath(dump) != os.path.abspath(DEFAULT_DUMP):
                os.makedirs(os.path.dirname(DEFAULT_DUMP), exist_ok=True)
                shutil.copyfile(dump, DEFAULT_DUMP)
                print(f"dump installed: {DEFAULT_DUMP}")
            out = TYPES_OUT
        else:
            out = a.out
        changed = _write(out, emit_types.emit(db, r))
        print(f"types.hpp {'written' if changed else 'unchanged'}: {out} ({len(r.types)} types)")
        return 0

    if a.cmd == "verify-image":
        from . import pe_image, verify_image
        db, _, p = _load(a.annotations, None, quiet=True)
        if p.errors:
            return 1
        img = pe_image.PeImage(_find(a.image, "image"))
        q, n = verify_image.check(db, img)
        for e_ in q.errors:
            print("error:", e_, file=sys.stderr)
        for w in q.warnings:
            print("warning:", w, file=sys.stderr)
        print(f"{n} checks against {os.path.basename(a.image)} (timestamp {img.timestamp:#x}): "
              f"{len(q.errors)} errors, {len(q.warnings)} warnings")
        return 1 if q.errors else 0

    if a.cmd == "compare":
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        sys.path.insert(0, here)
        import compare_dumps
        return compare_dumps.main([_find(a.gridnode, "GridNode dump"), _find(a.ghidra, "Ghidra dump")] +
                                  (["--strict"] if a.strict else []))
    return 2
