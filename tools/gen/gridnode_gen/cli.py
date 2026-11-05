"""gridnode: command line for the GridNode type database.

  gridnode validate [--reflected FILE]        check the annotations (and against a dump)
  gridnode engine --out FILE                  write gridnode/engine.hpp (annotations only)
  gridnode types --reflected FILE --out FILE  write gridnode/generated/types.hpp
  gridnode setup --dump FILE                  install your dump and generate types.hpp
  gridnode verify-image FILE                  check the annotations against a PE dump of the game
  gridnode fingerprint IMAGE [--write] [--mecmp DIR] [--add NAME]
                                              fingerprint bytes from a dump of the game (GridNode and MECMP)
  gridnode compare GRIDNODE_JSON GHIDRA_JSON  compare with a FrostbiteTypeDumper dump
  gridnode rust [--out FILE] [--check]       Rust bindings (bindings/rust/gridnode/src/sys.rs) from api.h
  gridnode docs [--reflected FILE] [--build]   the docs site (with a dump: the full reference, local)
  gridnode ghidra-export [--out FILE]          annotations for GridNodeSync.java to apply in Ghidra
  gridnode ghidra-import FILE [--who NAME]     what was named in Ghidra (yours or a friend's), to review
  gridnode ghidra-relay FILE [--who NAME]      a friend's Ghidra export, to import into your own Ghidra
  gridnode annotations-diff OTHER [--base B]   compare two annotation sets (folders or git:REF)
  gridnode hash NAME...                       Frostbite name hash of each name
  gridnode unhash HASH... [--image EXE]       names for hashes (or a file of hashes, one per line)
  gridnode package [--build DIR] [--examples] a release zip (game files, SDK, mod templates)
gridnode package --player --build DIR       the players' zip (player build, injector, MEC trainer)
  gridnode new-mod NAME [--rust] [--sdk DIR]  a mod from the template (templates/gridnode_mod.py new)
  gridnode pack-mod DLL [--file F ...]        a mod's zip for others (templates/gridnode_mod.py pack)
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

    fp = sub.add_parser("fingerprint", help="check / write fingerprint bytes from a dump of the game (GridNode menu -> Game image)")
    fp.add_argument("image", help="the dump (bin/dumps/MirrorsEdgeCatalyst-*.exe)")
    fp.add_argument("--write", action="store_true", help="put the image's bytes into the entries that differ")
    fp.add_argument("--mecmp", help="also MECMP's src/game/fingerprint.cpp, in this MECMP checkout")
    fp.add_argument("--add", metavar="NAME", help="add a code entry for this function / code site (or address)")
    fp.add_argument("--skip", type=lambda v: int(v, 0), default=0, help="with --add: bytes to skip at the start")
    fp.add_argument("--length", type=int, default=16, help="with --add: how many bytes (1..32, default 16)")
    fp.add_argument("--required", action="store_true", help="with --add: a mismatch stops GridNode (default: optional)")
    fp.add_argument("--note", default="", help="with --add: a note")

    c = sub.add_parser("compare", help="compare a GridNode dump with a FrostbiteTypeDumper dump")
    c.add_argument("gridnode")
    c.add_argument("ghidra")
    c.add_argument("--strict", action="store_true")

    rs = sub.add_parser("rust", help="write the Rust bindings from sdk/include/gridnode/api.h")
    rs.add_argument("--out", default=os.path.join(ROOT, "bindings", "rust", "gridnode", "src", "sys.rs"))
    rs.add_argument("--check", action="store_true", help="fail if the file is not up to date (CI)")

    d = sub.add_parser("docs", help="the docs site; with --reflected, the full reflected reference (local only)")
    d.add_argument("--reflected", help="your dump: adds the reflected reference (never publish that build)")
    d.add_argument("--out", default=os.path.join(ROOT, "build", "docs"), help="output folder (default: build/docs)")
    d.add_argument("--build", action="store_true", help="run mkdocs build (site in OUT/site)")
    d.add_argument("--serve", action="store_true", help="run mkdocs serve")

    ge = sub.add_parser("ghidra-export", help="annotations as JSON for GridNodeSync.java (import into Ghidra)")
    ge.add_argument("--out", default=os.path.join(ROOT, "build", "gridnode-ghidra.json"))
    ge.add_argument("--reflected", default=DEFAULT_DUMP, help="dump for class sizes, base classes and vtables (optional)")
    ge.add_argument("--class", dest="classes", action="append", default=[], metavar="NAME",
                    help="also a struct and vtable type for this class (repeat; needs the dump)")
    gi = sub.add_parser("ghidra-import", help="GridNodeSync.java's export -> annotations to review")
    gi.add_argument("file")
    gi.add_argument("--out", default=os.path.join(ROOT, "db", "ghidra-inbox.yaml"))
    gi.add_argument("--who", help="whose Ghidra it came from (a friend's name): added to sources and notes")
    gr = sub.add_parser("ghidra-relay", help="a GridNodeSync export -> a file GridNodeSync imports into another Ghidra")
    gr.add_argument("file")
    gr.add_argument("--who", help="whose Ghidra it came from (shown in the comments)")
    gr.add_argument("--out", default=os.path.join(ROOT, "build", "gridnode-ghidra-relay.json"))
    ad = sub.add_parser("annotations-diff", help="compare two annotation sets")
    ad.add_argument("other", help="annotation folder, or git:REF (e.g. git:origin/main)")
    ad.add_argument("--base", help="annotation folder or git:REF to compare against (default: --annotations)")
    ad.add_argument("--inbox", help="also write the other set's new and changed entries to this YAML file")

    h = sub.add_parser("hash", help="Frostbite name hash (h = 5381; h = h*33 ^ c) of each name")
    h.add_argument("names", nargs="+")

    u = sub.add_parser("unhash", help="look hashes up in GridNode's dictionary")
    u.add_argument("hashes", nargs="+", help="hex (0x...) or decimal (Frosty shows signed ints), or @file with one per line")
    u.add_argument("--reflected", default=DEFAULT_DUMP, help="dump whose names go in the dictionary")
    u.add_argument("--image", help="also every string in this PE dump of the game")
    u.add_argument("--words", action="append", default=[], help="extra word list (one name per line)")
    u.add_argument("--unknown", action="store_true", help="print only the hashes that were not found")

    pk = sub.add_parser("package", help="a release zip from a Windows build (docs/packaging.md)")
    pk.add_argument("--build", default=os.path.join(ROOT, "build"), help="the Windows build folder (default: build)")
    pk.add_argument("--out", default=os.path.join(ROOT, "build", "release"), help="where the zip goes (default: build/release)")
    pk.add_argument("--examples", action="store_true", help="the example mods in game/mods too")
    pk.add_argument("--repo", default="https://github.com/CodeNameMeteor/Gridnode", help="named in README.txt")
    pk.add_argument("--player", action="store_true", help="the players' zip: --build is a player build; no SDK")
    pk.add_argument("--mods", help="with --player: the folder with mec_trainer.dll (default: the build's mods)")
    nm = sub.add_parser("new-mod", help="a new mod from the template (templates/gridnode_mod.py new)")
    nm.add_argument("rest", nargs=argparse.REMAINDER)
    pm = sub.add_parser("pack-mod", help="a mod's zip for others (templates/gridnode_mod.py pack)")
    pm.add_argument("rest", nargs=argparse.REMAINDER)

    a = ap.parse_args(argv)

    if a.cmd in ("new-mod", "pack-mod"):
        import importlib.util
        spec = importlib.util.spec_from_file_location("gridnode_mod", os.path.join(ROOT, "templates", "gridnode_mod.py"))
        gm = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gm)
        return gm.main(["new" if a.cmd == "new-mod" else "pack"] + a.rest)

    if a.cmd == "package":
        from . import package
        if a.player:
            z = package.build_player(ROOT, os.path.abspath(a.build), os.path.abspath(a.out), a.repo, os.path.abspath(a.mods) if a.mods else None)
        else:
            z = package.build(ROOT, os.path.abspath(a.build), os.path.abspath(a.out), a.examples, a.repo)
        print(f"release: {z}")
        return 0

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

    if a.cmd == "docs":
        from . import emit_docs
        dump = _find(a.reflected, "dump") if a.reflected else None
        db, r, p = _load(a.annotations, dump, quiet=True)
        if p.errors:
            return 1
        pages = emit_docs.write_site(ROOT, a.out, db, r)
        print(f"docs: {len(pages)} generated pages in {a.out}" + (" (with the reflected reference: local only)" if r else ""))
        if a.build or a.serve:
            import subprocess
            cmd = [sys.executable, "-m", "mkdocs", "serve" if a.serve else "build", "-f", os.path.join(a.out, "mkdocs.yml")]
            if a.build:
                cmd += ["-d", os.path.join(a.out, "site")]
            return subprocess.call(cmd)
        return 0

    if a.cmd == "ghidra-export":
        import json
        from . import ghidra_sync
        dump = _find(a.reflected, "dump") if a.reflected and os.path.exists(_resolve(a.reflected)) else None
        db, r, p = _load(a.annotations, dump, quiet=True)
        if p.errors:
            return 1
        missing = [n for n in a.classes if n not in db.classes and (r is None or n not in r.by_name)]
        if missing:
            print(f"ghidra-export: unknown class(es) {', '.join(missing)}" + ("" if r else " (no dump: only annotated classes)"))
            return 1
        doc = ghidra_sync.to_ghidra(db, r, a.classes)
        _write(a.out, json.dumps(doc, indent=1) + "\n")
        print(f"ghidra-export: {len(doc['structs'])} structs, {len(doc['functions'])} functions, {len(doc['labels'])} labels "
              f"-> {a.out}\n  In Ghidra: Script Manager -> GridNode -> GridNodeSync, choose Import, pick this file.")
        return 0

    if a.cmd == "ghidra-import":
        import json
        import tempfile
        from . import ghidra_sync
        db, _, p = _load(a.annotations, None, quiet=True)
        if p.errors:
            return 1
        with open(_find(a.file, "GridNodeSync export"), encoding="utf-8") as f:
            text, counts = ghidra_sync.from_ghidra(json.load(f), db, who=a.who)
        # The result must load next to the existing annotations before it is written.
        with tempfile.TemporaryDirectory() as d:
            for fn in os.listdir(a.annotations):
                if fn.endswith(".yaml"):
                    shutil.copy(os.path.join(a.annotations, fn), d)
            with open(os.path.join(d, "zz-ghidra-inbox.yaml"), "w", encoding="utf-8") as f:
                f.write(text)
            _, p2 = annotations.load(d)
            if p2.errors:
                for e in p2.errors:
                    print("error:", e, file=sys.stderr)
                raise UserError("the imported findings do not load; nothing written")
        _write(a.out, text)
        print(f"ghidra-import: {counts['functions']} functions, {counts['globals']} globals, {counts['members']} members, "
              f"{counts['renamed']} named differently -> {a.out}\n  Review them and move each into its annotation file.")
        return 0

    if a.cmd == "ghidra-relay":
        import json
        from . import ghidra_sync
        with open(_find(a.file, "GridNodeSync export"), encoding="utf-8") as f:
            doc = ghidra_sync.relay(json.load(f), who=a.who)
        _write(a.out, json.dumps(doc, indent=1) + "\n")
        print(f"ghidra-relay: {len(doc['functions'])} functions, {len(doc['labels'])} labels -> {a.out}\n"
              "  In your Ghidra: GridNodeSync, choose Import, pick this file. Names you gave yourself are kept.")
        return 0

    if a.cmd == "annotations-diff":
        from . import annotations_diff

        def side(spec: str) -> tuple[str, str]:
            if spec.startswith("git:"):
                ref = spec[4:]
                sub_dir = os.path.relpath(os.path.abspath(a.annotations), ROOT).replace(os.sep, "/")
                return annotations_diff.checkout(ref, ROOT, sub_dir), ref
            path = spec if os.path.isdir(spec) else os.path.join(ROOT, spec)
            if not os.path.isdir(path):
                raise UserError(f"annotation folder not found: {spec}")
            return path, spec

        base_dir, base_label = side(a.base) if a.base else (a.annotations, "repo")
        other_dir, other_label = side(a.other)
        d = annotations_diff.diff(base_dir, other_dir, base_label, other_label)
        print(annotations_diff.report(d), end="")
        if a.inbox:
            _write(a.inbox, annotations_diff.inbox(d))
            print(f"inbox: {len(d.added) + len(d.renamed) + len(d.changed)} entries -> {a.inbox}")
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
              f"{len(db.functions)} functions, {len(db.hooks)} hooks, {len(db.events)} events ({n} items): "
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

    if a.cmd == "fingerprint":
        from . import fingerprint_gen, pe_image
        db, _, p = _load(a.annotations, None, quiet=True)
        if p.errors:
            return 1
        img = pe_image.PeImage(_find(a.image, "image"))
        fp_yaml = os.path.join(a.annotations, "fingerprint.yaml")
        if a.add:
            try:
                line = fingerprint_gen.add_gridnode(fp_yaml, db, img, a.add, a.skip, a.length, a.required, a.note)
            except ValueError as e:
                raise UserError(str(e))
            print(f"added to {fp_yaml}:\n{line}", end="")
            db, _, p = _load(a.annotations, None, quiet=True)
            if p.errors:
                return 1
        bad = 0
        res = fingerprint_gen.check_gridnode(db, img)
        print(fingerprint_gen.report(res, "GridNode fingerprint.yaml"), end="")
        bad += sum(1 for r in res if not r.ok and r.required)
        if a.write:
            n = fingerprint_gen.write_gridnode(fp_yaml, res)
            print(f"  {n} code entr{'y' if n == 1 else 'ies'} rewritten from the image" + (" (check their notes)" if n else ""))
        if a.mecmp:
            try:
                mres = fingerprint_gen.check_mecmp(a.mecmp, img)
            except ValueError as e:
                raise UserError(str(e))
            print(fingerprint_gen.report(mres, "MECMP fingerprint.cpp"), end="")
            bad += sum(1 for r in mres if not r.ok and r.required)
            if a.write:
                n = fingerprint_gen.write_mecmp(a.mecmp, mres)
                print(f"  {n} code entr{'y' if n == 1 else 'ies'} rewritten from the image" + (" (check the comments above them)" if n else ""))
        return 1 if bad and not a.write else 0

    if a.cmd == "compare":
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        sys.path.insert(0, here)
        import compare_dumps
        return compare_dumps.main([_find(a.gridnode, "GridNode dump"), _find(a.ghidra, "Ghidra dump")] +
                                  (["--strict"] if a.strict else []))
    return 2
