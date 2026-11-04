#!/usr/bin/env python3
"""GridNode mods: start one from a template, and pack one for others to install.

  python gridnode_mod.py new NAME [--rust] [--out DIR] [--sdk DIR] [--author A] [--description D]
  python gridnode_mod.py pack DLL [--id ID] [--version V] [--file F ...] [--out DIR]

new: a folder NAME with a mod that builds (CMake for C++, Cargo for Rust) against the SDK: by
default the `sdk` folder this script is in (a GridNode release), else --sdk.
pack: NAME-VERSION.zip holding mods/NAME/NAME.dll and the files given; unzipped next to
gridnode.dll (GridNode's folder), GridNode loads it from mods/NAME/ (the mod finds its files with
gn::modDir()).

Shipped in a release as sdk/templates/gridnode_mod.py; in the repository as templates/.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))


def _sdk_default() -> str | None:
    sdk = os.path.dirname(HERE)  # sdk/templates/gridnode_mod.py -> sdk
    return sdk if os.path.isfile(os.path.join(sdk, "include", "gridnode", "api.h")) else None


def _names(name: str):
    """The mod's file name (letters, digits, _), its id (kebab case) and a title."""
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]{0,47}", name):
        raise SystemExit(f"error: a mod name is a letter then letters, digits, _ or - (at most 48): {name!r}")
    file = name.replace("-", "_").lower()
    ident = name.replace("_", "-").lower()
    title = " ".join(w.capitalize() for w in re.split(r"[-_]", name) if w)
    return file, ident, title


def new(args) -> int:
    file, ident, title = _names(args.name)
    sdk = os.path.abspath(args.sdk) if args.sdk else _sdk_default()
    if not sdk or not os.path.isfile(os.path.join(sdk, "include", "gridnode", "api.h")):
        raise SystemExit("error: no GridNode SDK: pass --sdk <a release's sdk folder> (gridnode.py package makes one)")
    src = os.path.join(HERE, "mod-rust" if args.rust else "mod-cpp")
    out = os.path.abspath(os.path.join(args.out, file))
    if os.path.exists(out):
        raise SystemExit(f"error: {out} exists")
    subst = {
        "@NAME@": file,
        "@ID@": ident,
        "@TITLE@": title,
        "@AUTHOR@": args.author,
        "@DESCRIPTION@": args.description.replace('"', "'"),
        "@SDK@": sdk.replace("\\", "/"),
        "@SDK_RUST@": os.path.join(sdk, "rust", "gridnode").replace("\\", "/"),
    }
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        for f in files:
            target = os.path.join(out, rel, f.replace("@NAME@", file))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(os.path.join(root, f), encoding="utf-8") as fh:
                text = fh.read()
            for k, v in subst.items():
                text = text.replace(k, v)
            with open(target, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
    print(f"{'Rust' if args.rust else 'C++'} mod {ident} in {out}")
    if args.rust:
        print("  build: cargo build --release --target x86_64-pc-windows-msvc")
    else:
        print("  build: cmake -S . -B build -A x64 && cmake --build build --config Release")
    print(f"  test:  copy the DLL into <game>\\mods\\{file}\\ (or mods\\); pack: gridnode_mod.py pack <the DLL>")
    return 0


def pack(args) -> int:
    dll = os.path.abspath(args.dll)
    if not os.path.isfile(dll) or not dll.lower().endswith(".dll"):
        raise SystemExit(f"error: not a DLL: {args.dll}")
    name = os.path.splitext(os.path.basename(dll))[0]
    ident = args.id or name.replace("_", "-")
    os.makedirs(args.out, exist_ok=True)
    zpath = os.path.abspath(os.path.join(args.out, f"{ident}-{args.version}.zip"))
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(dll, f"mods/{name}/{name}.dll")
        for f in args.file:
            if os.path.isdir(f):
                for root, _dirs, files in os.walk(f):
                    for g in files:
                        p = os.path.join(root, g)
                        z.write(p, f"mods/{name}/{os.path.relpath(p, os.path.dirname(os.path.abspath(f)))}".replace("\\", "/"))
            elif os.path.isfile(f):
                z.write(f, f"mods/{name}/{os.path.basename(f)}")
            else:
                raise SystemExit(f"error: not found: {f}")
    print(f"{zpath}: unzip next to gridnode.dll (GridNode's folder); GridNode loads mods/{name}/{name}.dll")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gridnode_mod", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("new", help="a new mod from the template")
    n.add_argument("name")
    n.add_argument("--rust", action="store_true", help="a Rust mod (Cargo) instead of C++ (CMake)")
    n.add_argument("--out", default=".", help="where its folder goes (default: here)")
    n.add_argument("--sdk", help="the SDK folder (default: the one this script is in)")
    n.add_argument("--author", default="me")
    n.add_argument("--description", default="A GridNode mod.")
    p = sub.add_parser("pack", help="a zip others unzip next to gridnode.dll")
    p.add_argument("dll")
    p.add_argument("--id", help="its id, for the zip's name (default: the DLL's name with - for _)")
    p.add_argument("--version", default="0.1.0")
    p.add_argument("--file", action="append", default=[], help="a file or folder the mod brings (repeat)")
    p.add_argument("--out", default=".", help="where the zip goes (default: here)")
    a = ap.parse_args(argv)
    return new(a) if a.cmd == "new" else pack(a)


if __name__ == "__main__":
    sys.exit(main())
