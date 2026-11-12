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
  assets deps NAME --data DIR [--into BUNDLE]   everything an EBX needs (imports, resources, chunks) and
                                               the bundles that hold each; --into: what a bundle lacks
  assets usage --data DIR [--kind-of TYPE] [--like TEXT] [--dump JSON] [--out FILE.yaml]
                                               every type instances are of, in every EBX: how many, in how
                                               many files, examples, its fields (a catalogue to plan from)
  assets fbmod-info FILE.fbmod [--data DIR] [--extract DIR]
                                               a Frosty mod: details, what it changes, in which bundles
  assets fbmod-build SPEC.yaml [--out FILE.fbmod] [--data DIR] [--dump JSON]
                                               a Frosty mod (version 5) from EBX / YAML files (SPEC: below)
  assets ebx-build FILE.yaml [--out FILE.ebx] [--dump JSON]
                                               a YAML file written as EBX, its types checked against the game's

SPEC for fbmod-build (paths relative to it):
  title: My mod
  author: me
  category: Gameplay
  version: "1.0"
  description: What it does
  ebx:
    - name: gameplay/movement/climbing/ladders/pf_climbladder_768_nobrackets   # the asset it replaces
      file: ladder.yaml                                                        # or a .ebx file
      bundles: []          # only for an asset added to bundles that do not have it (names, as `list` shows)

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


def cmd_used_by(a) -> int:
    from .deps import Index, used_by
    s = _store(a)
    names = used_by(Index(s, os.path.join(OUT, "index")), a.name)
    for n in names:
        print(n)
    print(f"used-by {a.name}: {len(names)} EBX files import it")
    return 0


def cmd_deps(a) -> int:
    from .deps import Index, needs
    s = _store(a)
    index = Index(s, os.path.join(OUT, "index"))
    items, missing = needs(index, a.name)
    into = a.into.lower() if a.into else ""
    lacking = 0
    for it in items:
        held = sorted(it.bundles)
        has = any(into in b.lower() for b in held) if into else True
        lacking += not has
        if into and has and not a.all:
            continue
        mark = "" if it.here else "  (data not here)"
        need = f"  <- {it.via}" if it.via else ""
        where = held[0] + (f" (+{len(held) - 1} more)" if len(held) > 1 else "") if held else "no bundle"
        print(f"{it.kind:6} {it.name}{(' ' + it.detail) if it.detail else ''}  [{where}]{need}{mark}")
    counts = {k: sum(1 for i in items if i.kind == k) for k in ("ebx", "res", "chunks")}
    print(f"deps {a.name}: {counts['ebx']} ebx, {counts['res']} res, {counts['chunks']} chunks"
          + (f"; {lacking} not in a bundle matching '{a.into}'" if into else ""))
    for m in missing[:30]:
        print("not followed:", m, file=sys.stderr)
    if len(missing) > 30:
        print(f"not followed: {len(missing) - 30} more", file=sys.stderr)
    return 0


def _bundle_names(a) -> dict[int, str]:
    if not (a.data or os.environ.get("GRIDNODE_DATA")):
        return {}
    names = {}
    for b in _all_bundles(_store(a)):
        names[ebx.name_hash(b.name.lower())] = b.name
    return names


def cmd_fbmod_info(a) -> int:
    from . import fbmod
    with open(a.file, "rb") as f:
        d = f.read()
    if not d.startswith(fbmod.MAGIC):
        v = fbmod.read_legacy(d)
        print(f"{v.get('magic')}: {v.get('title')} {v.get('version')} by {v.get('author')} (an old format: read only; "
              f"its data is in {os.path.splitext(os.path.basename(a.file))[0]}_01.archive)")
        for r in v.get("resources", []):
            print(f"  {r.get('type', '?'):9} {r.get('name', '')}  {r.get('uncompressedSize', '')} bytes")
        for act in v.get("actions", []):
            print(f"  {act.get('type')} resource {act.get('resourceId')} in {act.get('bundle')}")
        return 0
    m = fbmod.read(d)
    names = _bundle_names(a)
    print(f"{m.title} {m.mod_version} by {m.author} ({m.category}): FROSTY version {m.version}, game version {m.game_version}")
    if m.description:
        print("  " + m.description.strip().replace("\n", "\n  "))
    for r in m.resources:
        if r.type == fbmod.EMBEDDED and r.index < 0:
            continue
        where = ", ".join(names.get(b, f"{b:#010x}") for b in r.bundles)
        extra = f" resType {r.res_type:#010x} rid {r.res_rid:#018x}" if r.type == fbmod.RES else ""
        print(f"  {fbmod.KINDS[r.type]:8} {r.name}  {r.size} bytes{extra}" + (f"  added to: {where}" if where else ""))
        if a.extract and r.index >= 0:
            data = m.payload(r)
            path = os.path.join(a.extract, fbmod.KINDS[r.type], r.name.replace("/", os.sep) + "." + fbmod.KINDS[r.type])
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            if r.type == fbmod.EBX:
                with open(path[:-4] + ".yaml", "w", encoding="utf-8") as f:
                    f.write(ebx.to_yaml(ebx.read(data)))
    if a.extract:
        print(f"extracted into {a.extract}")
    return 0


def cmd_fbmod_build(a) -> int:
    import yaml

    from . import fbmod
    with open(a.spec, encoding="utf-8") as f:
        spec = yaml.safe_load(f) or {}
    here = os.path.dirname(os.path.abspath(a.spec))
    m = fbmod.new_mod(str(spec.get("title", "")), str(spec.get("author", "")), str(spec.get("category", "")),
                      str(spec.get("version", "1.0")), str(spec.get("description", "")), str(spec.get("link", "")))
    if not m.title:
        raise StoreError(f"{a.spec}: give the mod a title")
    dump = _dump(a)
    known: dict[str, str] = {}
    if a.data or os.environ.get("GRIDNODE_DATA"):
        for b in _all_bundles(_store(a)):
            for e in b.entries:
                if e.kind == "ebx":
                    known.setdefault(e.name.lower(), b.name)
    errors = 0
    for item in spec.get("ebx") or []:
        name, path = str(item.get("name", "")), os.path.join(here, str(item.get("file", "")))
        if not name or not os.path.isfile(path):
            raise StoreError(f"{a.spec}: an ebx entry needs a name and a file that exists ({name or '?'}: {path})")
        if path.lower().endswith((".yaml", ".yml")):
            with open(path, encoding="utf-8") as f:
                x = ebx.from_yaml(f.read())
        else:
            with open(path, "rb") as f:
                x = ebx.read(f.read())
        if dump is not None:
            errs, _ = ebx.check_types(x, dump)
            for e in errs:
                print(f"error: {name}: {e}", file=sys.stderr)
            errors += len(errs)
        bundles = [str(b) for b in item.get("bundles") or []]
        if known and name.lower() not in known and not bundles:
            print(f"warning: {name} is in no bundle of the game: a new asset needs `bundles` to be loaded", file=sys.stderr)
        fbmod.add_ebx(m, name, ebx.write(x), bundles)
    if errors:
        print(f"fbmod-build: {errors} error(s): not written", file=sys.stderr)
        return 1
    out = a.out or os.path.join(OUT, "mods", "".join(c if c.isalnum() or c in " -_" else "_" for c in m.title) + ".fbmod")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    data = fbmod.write(m)
    with open(out, "wb") as f:
        f.write(data)
    print(f"fbmod-build: {sum(1 for r in m.resources if r.index >= 0)} asset(s), {len(data)} bytes -> {out}")
    return 0


_PRIMS = {1: "DbObject", 6: "String", 7: "CString", 9: "FileRef", 10: "Boolean", 11: "Int8", 12: "Uint8", 13: "Int16",
          14: "Uint16", 15: "Int32", 16: "Uint32", 17: "Int64", 18: "Uint64", 19: "Float32", 20: "Float64", 21: "Guid",
          22: "SHA1", 23: "ResourceRef", 25: "TypeRef", 26: "BoxedValueRef"}


def _field_type(x, f) -> str:
    c = ebx.code_of(f.flags)
    if c in (ebx.C_STRUCT, ebx.C_ENUM):
        return x.types[f.ref].name
    if c == ebx.C_CLASS:
        return "ref"
    if c == ebx.C_ARRAY:
        m = x.types[f.ref].fields[0] if x.types[f.ref].fields else None
        return "array of " + (_field_type(x, m) if m else "?")
    return _PRIMS.get(c, f"code {c}")


def _fields_of(x, ti: int) -> dict:
    out: dict = {}
    t = x.types[ti]
    for f in t.fields:
        if ebx.code_of(f.flags) == ebx.C_INHERITED:
            out.update(_fields_of(x, f.ref))
        else:
            out[ebx.key_of(t, f, out)] = _field_type(x, f)
    return out


def cmd_bundle_parents(a) -> int:
    from .parents import BLUEPRINTS, KIND, bundle_key, chain, parents_of, prefab_bundle, shares_of, write_prefabs, write_table
    s = _store(a)
    levels: list[tuple[str, ebx.Ebx]] = []  # (bundle, model) of the level files naming sublevels
    where: dict[str, list[str]] = {}       # blueprint asset -> the bundles it is in
    read: dict[bytes, tuple[str, str]] = {}  # sha1 -> (what it is, its name): a file in several bundles is read once
    contents: dict[str, set[str]] = {}       # bundle -> its EBX names
    for b in _all_bundles(s):
        mine = contents.setdefault(bundle_key(b.name), set())
        for e in b.entries:
            if e.kind == "ebx":
                mine.add(e.name.lower())
            if e.kind != "ebx" or not e.sha1 or not s.has(e.sha1):
                continue
            got = read.get(e.sha1)
            if got is None:
                got = ("", e.name)
                try:
                    data = s.read(e.sha1)
                    head = ebx.read(data, values=False)
                    names = {t.name for t in head.types}
                    if e.name.lower().startswith("levels/") and KIND in names:
                        levels.append((b.name, ebx.read(data)))
                    if head.instances and head.types[head.instances[0].type].name in BLUEPRINTS:
                        got = ("blueprint", e.name)
                except Exception:  # noqa: BLE001 - ebx-check is the place to look at files that do not read
                    pass
                read[e.sha1] = got
            if got[0] == "blueprint":
                where.setdefault(got[1].lower(), []).append(b.name)
    table = parents_of(levels)
    tops = {row["parent"] for row in table.values()} - set(table)  # the levels themselves: always loaded
    shares = shares_of(contents, sorted(table), tops)
    path = a.out or os.path.join(OUT, "bundle-parents.txt")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    write_table(table, path, shares)
    sizes = {b: len(f) for b, f in contents.items()}
    prefabs = {asset: prefab_bundle(bundles, table, sizes) for asset, bundles in where.items()}
    prefabs = {k: v for k, v in prefabs.items() if v}
    ppath = os.path.join(os.path.dirname(os.path.abspath(path)), "prefabs.txt")
    write_prefabs(prefabs, ppath)
    for name in a.show or []:
        k = bundle_key(name)
        print(f"{k} needs first: {', '.join(chain(table, name)) or '(nothing known)'}")
        print(f"{k} shares files with {len(shares.get(k, []))} bundles: {', '.join(shares.get(k, [])[:40])}")
    print(f"bundle-parents: {len(table)} sublevel bundles -> {path}")
    print(f"prefabs: {len(prefabs)} of {len(where)} blueprints are in a sublevel bundle -> {ppath}")
    return 0


def cmd_usage(a) -> int:
    import yaml
    s = _store(a)
    dump = _dump(a)
    parents: dict[str, list[str]] = {}

    def chain(name: str) -> list[str]:
        if name in parents:
            return parents[name]
        out, t = [], dump["by_name"].get(name) if dump else None
        while t is not None and len(out) < 64:
            out.append(t["name"])
            t = dump["by_ti"].get(t.get("super", ""))
        parents[name] = out
        return out

    want_kind = a.kind_of.lower() if a.kind_of else ""
    like = a.like.lower() if a.like else ""
    if want_kind and dump is None:
        raise StoreError("--kind-of needs the reflected dump (--dump, or gridnode.py setup)")
    seen: set[bytes] = set()
    found: dict[str, dict] = {}
    files = absent = bad = 0
    for b in _all_bundles(s):
        for e in b.entries:
            if e.kind != "ebx" or not e.sha1 or e.sha1 in seen:
                continue
            seen.add(e.sha1)
            if not s.has(e.sha1):
                absent += 1
                continue
            try:
                x = ebx.read(s.read(e.sha1), values=False)
            except Exception:  # noqa: BLE001 - counted; ebx-check is the place to look at them
                bad += 1
                continue
            files += 1
            here: set[str] = set()
            for inst in x.instances:
                name = x.types[inst.type].name
                if like and like not in name.lower():
                    continue
                if want_kind and want_kind not in (p.lower() for p in chain(name)):
                    continue
                u = found.get(name)
                if u is None:
                    u = found[name] = {"instances": 0, "files": 0, "superbundles": set(), "examples": [],
                                       "fields": _fields_of(x, inst.type)}
                u["instances"] += 1
                if name not in here:
                    here.add(name)
                    u["files"] += 1
                    u["superbundles"].add(b.superbundle)
                    if len(u["examples"]) < a.examples:
                        u["examples"].append(e.name)
    out = []
    for name, u in sorted(found.items(), key=lambda kv: (-kv[1]["files"], kv[0])):
        row = {"type": name}
        if dump is not None:
            row["parents"] = chain(name)[1:]
        row.update(instances=u["instances"], files=u["files"], superbundles=sorted(u["superbundles"]), examples=u["examples"],
                   fields=u["fields"])
        out.append(row)
    path = a.out or os.path.join(OUT, "usage.yaml")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        yaml.safe_dump(out, f, sort_keys=False, width=140, allow_unicode=True)
    for row in out[:a.top]:
        print(f"{row['files']:6} files {row['instances']:7} instances  {row['type']}")
    print(f"usage: {len(out)} types in {files} EBX files ({absent} in archives not here, {bad} not read) -> {path}")
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
            data = b""
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
                # grouped by where they differ (the section) or by the error
                key = why[why.rfind("(") + 1:-1] if "difference" in why else why[:120]
                if a.keep:  # the file itself, to look at (git-ignored: game data)
                    path = os.path.join(a.keep, e.name.replace("/", os.sep) + ".ebx")
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with open(path, "wb") as f:
                        f.write(data)
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
    ec.add_argument("--keep", help="a folder: the files that do not write back the same are copied there")
    de = sub.add_parser("deps", help="everything an EBX needs, and the bundles that hold it")
    de.add_argument("name")
    de.add_argument("--data")
    de.add_argument("--into", help="a bundle (part of its name): list only what it lacks")
    de.add_argument("--all", action="store_true", help="with --into: list everything, not only what it lacks")
    ub = sub.add_parser("used-by", help="the EBX files that import an EBX (where a prefab is placed)")
    ub.add_argument("name")
    ub.add_argument("--data")
    bp = sub.add_parser("bundle-parents", help="each sublevel bundle's parents, for GridNode's bundle_load (bin/assets/bundle-parents.txt)")
    bp.add_argument("--data")
    bp.add_argument("--out")
    bp.add_argument("--show", nargs="*", help="print what these bundles need first")
    us = sub.add_parser("usage", help="which types the game's EBX instances are of: counts, examples, fields")
    us.add_argument("--data")
    us.add_argument("--kind-of", help="only this class and its subclasses (needs the dump), e.g. SpatialEntityData")
    us.add_argument("--like", help="only types whose name contains this, e.g. Volume")
    us.add_argument("--dump", help="the reflected dump (default: db/reflected/gridnode-reflected.json when there)")
    us.add_argument("--out", help="the catalogue (default: bin/assets/usage.yaml)")
    us.add_argument("--examples", type=int, default=3)
    us.add_argument("--top", type=int, default=40, help="how many to print (all are in the file)")
    fi = sub.add_parser("fbmod-info", help="a Frosty mod: details and what it changes")
    fi.add_argument("file")
    fi.add_argument("--data", help="the game's Data folder: bundle names instead of hashes")
    fi.add_argument("--extract", help="a folder: write its assets there (EBX also as YAML)")
    fb = sub.add_parser("fbmod-build", help="a Frosty mod from EBX / YAML files")
    fb.add_argument("spec")
    fb.add_argument("--out")
    fb.add_argument("--data", help="the game's Data folder: warns about assets in no bundle")
    fb.add_argument("--dump", help="the reflected dump (default: db/reflected/gridnode-reflected.json when there)")
    bu = sub.add_parser("ebx-build", help="a YAML file written as EBX, into bin/assets/built")
    bu.add_argument("file")
    bu.add_argument("--out")
    bu.add_argument("--dump", help="the reflected dump (default: db/reflected/gridnode-reflected.json when there)")
    a = ap.parse_args(argv)
    try:
        return {"check": cmd_check, "list": cmd_list, "extract": cmd_extract, "ebx": cmd_ebx,
                "ebx-check": cmd_ebx_check, "ebx-build": cmd_ebx_build,
                "deps": cmd_deps, "used-by": cmd_used_by, "bundle-parents": cmd_bundle_parents, "usage": cmd_usage, "fbmod-info": cmd_fbmod_info, "fbmod-build": cmd_fbmod_build}[a.cmd](a)
    except (StoreError, ebx.EbxError, ValueError) as e:
        print(f"assets: {e}", file=sys.stderr)
        return 1
