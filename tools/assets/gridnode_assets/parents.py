"""Which bundles a sublevel bundle needs loaded first: its parents, from the level files.

A level is a tree of sublevels: a SubWorldReferenceObjectData in one sublevel's files names a child
sublevel's bundle (BundleName) and the bundles to load before it (PreloadedBundleNames). A child's objects
use assets its parent holds, so a bundle loaded at runtime without its parent breaks when the parent
streams out (docs/research/zone-streaming.md). GridNode reads the table this writes (bundle_load).
"""

from __future__ import annotations

from typing import Iterable

from .ebx import Ebx

KIND = "SubWorldReferenceObjectData"


def bundle_key(name: str) -> str:
    """A bundle name as GridNode compares it: lower case, '/' only, without the platform folder."""
    n = name.replace("\\", "/").strip("/").lower()
    return n[len("win32/"):] if n.startswith("win32/") else n


def parents_of(files: Iterable[tuple[str, Ebx]]) -> dict[str, dict]:
    """files: (bundle the file is in, its model). -> {child: {"parent": bundle, "preload": [bundles]}}.
    A child named by two bundles keeps the first (logged by the caller as a clash)."""
    out: dict[str, dict] = {}
    for bundle, x in files:
        for inst in x.instances:
            if x.types[inst.type].name != KIND:
                continue
            child = inst.values.get("BundleName")
            if not isinstance(child, str) or not child:
                continue
            preload = [bundle_key(p) for p in (inst.values.get("PreloadedBundleNames") or []) if isinstance(p, str) and p]
            key = bundle_key(child)
            if key not in out:
                out[key] = {"parent": bundle_key(bundle), "preload": preload}
    return out


def chain(table: dict[str, dict], name: str, limit: int = 32) -> list[str]:
    """What to load before `name`, outermost first: its parents up the tree and their preloads (no repeats)."""
    order: list[str] = []
    key = bundle_key(name)
    seen = {key}
    for _ in range(limit):
        row = table.get(key)
        if not row:
            break
        step = [p for p in row["preload"] + [row["parent"]] if p not in seen]
        for p in step:
            seen.add(p)
        order = step + order
        key = row["parent"]
    return order


BLUEPRINTS = ("SpatialPrefabBlueprint", "ObjectBlueprint", "PrefabBlueprint")


def prefab_bundle(bundles: list[str], table: dict[str, dict]) -> str:
    """The bundle to load for a blueprint found in `bundles`: one GridNode can load as a sublevel (a child
    in the table) and with the fewest parents to load first; '' when it is in none."""
    known = [bundle_key(b) for b in bundles if bundle_key(b) in table]
    return min(known, key=lambda b: (len(chain(table, b)), b)) if known else ""


def write_prefabs(prefabs: dict[str, str], path: str) -> None:
    """One line a blueprint: asset<TAB>bundle (the layout editor's list of what it can place)."""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# GridNode prefabs (gridnode.py assets bundle-parents): asset, the bundle to load. From the game's files: do not commit.\n")
        for asset in sorted(prefabs):
            f.write(f"{asset}\t{prefabs[asset]}\n")


def write_table(table: dict[str, dict], path: str) -> None:
    """One line a child: child<TAB>parent<TAB>preload;preload (GridNode's bundles.cpp reads this)."""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# GridNode bundle parents (gridnode.py assets bundle-parents): child, parent, preloads. From the game's files: do not commit.\n")
        for child in sorted(table):
            row = table[child]
            f.write(f"{child}\t{row['parent']}\t{';'.join(row['preload'])}\n")
