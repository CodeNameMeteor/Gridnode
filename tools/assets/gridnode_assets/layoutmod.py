"""A layout editor layout written as a Frosty mod (`assets layout-fbmod`): the objects GridNode places at runtime put
into the level's own data, so they are there without GridNode once the mod is installed.

As a real map mod does it (TheNollz's Map Expansion 0.8, read 2026-11-14): each new object is an instance of the
game's own data type appended to the WorldPartData `Objects` of the zone layer it stands in
(`levels/sp/sp_maincity/<zone>/layer0_<zone>_layer`); a placed prefab is a reference object (Blueprint: an import
of the prefab, BlueprintTransform in world space), a movement blocker a PamOBBCollisionEntityData with its
RigidBodyData. A prefab whose files the zone does not load is made available by adding its bundle (and that
bundle's parents) to the zone's PreloadedBundleNames in the SubWorldReferenceObjectData that streams the zone in,
as GridNode loads it at runtime (bundle_load).

Done here: PlacedPrefab and PlacedBlocker. Not yet: moves of the level's objects (the instance has to be found by
position, and one inside a shared prefab cannot move alone), copies, magrope and barrier copies, custom dashes.
"""

from __future__ import annotations

import json
import math
import os
from dataclasses import dataclass, field

from . import ebx, parents
from .ebx import Ebx, Instance, Type, Field
from .store import Store, bundles_of, read_toc

ROD_FOR = {  # the reference object a blueprint is placed with
    "SpatialPrefabBlueprint": "SpatialPrefabReferenceObjectData",
    "ObjectBlueprint": "ObjectReferenceObjectData",
    "EffectBlueprint": "EffectReferenceObjectData",
    "PrefabBlueprint": "ReferenceObjectData",
}
DONE = ("PlacedPrefab", "PlacedBlocker")
REF_CODES = (ebx.C_INHERITED, ebx.C_STRUCT, ebx.C_ARRAY, ebx.C_ENUM)  # field kinds whose ref is a type index
MARGIN = 10.0  # metres: a point this close to a layer's box counts as in it


class LayoutModError(ValueError):
    pass


# ---- the layout file -----------------------------------------------------------------------------------------
def load_layout(path: str) -> tuple[str, list[dict]]:
    import yaml
    with open(path, encoding="utf-8") as f:
        y = yaml.safe_load(f) or {}
    level = str(y.get("level") or "")
    if not level:
        raise LayoutModError(f"{path}: no level")
    return level, list(y.get("edits") or [])


def transform_of(e: dict) -> dict:
    """An edit's placement as an EBX LinearTransform value."""
    def v(key):
        x = e.get(key)
        if not isinstance(x, list) or len(x) != 3:
            raise LayoutModError(f"an edit's {key} is not [x, y, z]")
        return {"x": float(x[0]), "y": float(x[1]), "z": float(x[2])}
    return {"right": v("right"), "up": v("up"), "forward": v("forward"), "trans": v("position")}


# ---- the level's layers --------------------------------------------------------------------------------------
@dataclass
class Layer:
    name: str
    bundle: str
    sha1: bytes
    points: list = field(default_factory=list)  # a sample of its objects' positions
    lo: tuple = (0.0, 0.0, 0.0)
    hi: tuple = (0.0, 0.0, 0.0)
    types: list = field(default_factory=list)


@dataclass
class SubWorldRef:
    file: str      # the EBX holding it
    sha1: bytes
    bundle: str    # the bundle that file is in
    child: str     # the bundle it streams in (key form)
    preload: list  # its PreloadedBundleNames (key form)


def _positions(x: Ebx) -> list[tuple[float, float, float]]:
    out = []
    for inst in x.instances:
        for key in ("BlueprintTransform", "Transform"):
            t = inst.values.get(key)
            if isinstance(t, dict) and isinstance(t.get("trans"), dict):
                p = t["trans"]
                out.append((float(p.get("x", 0)), float(p.get("y", 0)), float(p.get("z", 0))))
                break
    return out


class LevelIndex:
    """The level's zone layers (where their objects are) and the SubWorldReferenceObjectData that stream its
    sublevels in, from the game's files; cached by SHA-1 in bin/assets/index (a game update changes them)."""

    def __init__(self, store: Store, level: str, cache_dir: str | None = None):
        self.store = store
        self.level = level
        self.dir = parents.bundle_key(os.path.dirname(level)) + "/"  # levels/sp/sp_maincity/
        self.layers: list[Layer] = []
        self.refs: list[SubWorldRef] = []
        self.ebx: dict[str, tuple[bytes, set]] = {}  # every EBX in the install: name -> (sha1, bundle keys)
        cache_path = os.path.join(cache_dir, "layers-" + self.dir.strip("/").replace("/", "_") + ".json") if cache_dir else ""
        cache: dict = {}
        if cache_path and os.path.isfile(cache_path):
            with open(cache_path, encoding="utf-8") as f:
                cache = json.load(f)
        changed = False
        level_files: list[tuple[str, bytes, str]] = []
        for toc, sb in store.superbundles():
            if not sb:
                continue
            for b in bundles_of(sb, read_toc(toc)):
                bkey = parents.bundle_key(b.name)
                for e in b.entries:
                    if e.kind != "ebx" or not e.sha1:
                        continue
                    name = e.name.lower()
                    sha, where = self.ebx.setdefault(name, (e.sha1, set()))
                    where.add(bkey)
                    if bkey.startswith(self.dir) and (name.startswith(self.dir) and ("/layer" in name or name == bkey)):
                        level_files.append((name, e.sha1, bkey))
        seen = set()
        for name, sha, bkey in level_files:
            if (name, bkey) in seen:
                continue
            seen.add((name, bkey))
            row = cache.get(sha.hex())
            if row is None:
                try:
                    x = ebx.read(store.read(sha))
                except Exception:  # noqa: BLE001 - one unreadable file is skipped, not the whole index
                    continue
                first = x.types[x.instances[0].type].name if x.instances else ""
                pts = _positions(x)
                step = max(1, len(pts) // 200)
                refs = []
                for inst in x.instances:
                    if x.types[inst.type].name == parents.KIND:
                        child = inst.values.get("BundleName")
                        if isinstance(child, str) and child:
                            refs.append([parents.bundle_key(child), [parents.bundle_key(p) for p in inst.values.get("PreloadedBundleNames") or [] if isinstance(p, str)]])
                row = {"first": first, "points": pts[::step], "types": sorted({x.types[i.type].name for i in x.instances}), "refs": refs}
                cache[sha.hex()] = row
                changed = True
            if row["first"] == "WorldPartData" and row["points"]:
                pts = [tuple(p) for p in row["points"]]
                lo = tuple(min(p[k] for p in pts) for k in range(3))
                hi = tuple(max(p[k] for p in pts) for k in range(3))
                self.layers.append(Layer(name, bkey, sha, pts, lo, hi, row["types"]))
            for child, preload in row["refs"]:
                self.refs.append(SubWorldRef(name, sha, bkey, child, preload))
        if cache_path and changed:
            os.makedirs(cache_dir, exist_ok=True)
            with open(cache_path, "w", encoding="utf-8") as f:
                json.dump(cache, f)
        self.table = {r.child: {"parent": r.bundle, "preload": r.preload} for r in self.refs}

    def layer_named(self, name: str) -> Layer:
        n = name.lower()
        for layer in self.layers:
            if layer.name == n or layer.name.endswith("/" + n):
                return layer
        raise LayoutModError(f"no zone layer {name} in {self.dir}")

    def layer_for(self, p: tuple[float, float, float]) -> Layer:
        """The zone layer a point stands in: the smallest layer box holding it (with a margin), else the layer
        with the nearest object."""
        if not self.layers:
            raise LayoutModError(f"no zone layers found in {self.dir}")
        inside = [lay for lay in self.layers if all(lay.lo[k] - MARGIN <= p[k] <= lay.hi[k] + MARGIN for k in range(3))]
        if inside:
            return min(inside, key=lambda lay: math.prod(max(1.0, lay.hi[k] - lay.lo[k]) for k in range(3)))
        return min(self.layers, key=lambda lay: min(math.dist(p, q) for q in lay.points))

    def loaded_with(self, bundle: str) -> set[str]:
        """The bundles in memory while `bundle` is: it, its parents and their preloads."""
        return {bundle, *parents.chain(self.table, bundle)}

    def read(self, name: str) -> Ebx:
        hit = self.ebx.get(name.lower())
        if not hit:
            raise LayoutModError(f"{name} is no EBX in this install")
        return ebx.read(self.store.read(hit[0]))


# ---- adding to an EBX ----------------------------------------------------------------------------------------
def type_index(x: Ebx, name: str) -> int:
    for k, t in enumerate(x.types):
        if t.name == name:
            return k
    return -1


def copy_type(dst: Ebx, src: Ebx, k: int, memo: dict[int, int] | None = None) -> int:
    """src.types[k] (and the types its fields name) into dst's table; a named type dst already has is shared."""
    memo = {} if memo is None else memo
    if k in memo:
        return memo[k]
    t = src.types[k]
    if t.name != "array":
        have = type_index(dst, t.name)
        if have >= 0:
            memo[k] = have
            return have
    memo[k] = len(dst.types)  # placeholder before the fields (a type can name itself)
    nt = Type(t.name, t.align, t.flags, t.size, t.size2, [])
    dst.types.append(nt)
    for f in t.fields:
        ref = f.ref
        if ebx.code_of(f.flags) in REF_CODES:
            ref = copy_type(dst, src, f.ref, memo)
        nt.fields.append(Field(f.name, f.flags, ref, f.offset, f.offset2))
    if t.name == "array":  # an array type equal to one dst has: use that one
        for j, o in enumerate(dst.types[:-1]):
            if o.name == "array" and [(f.name, f.flags, f.ref) for f in o.fields] == [(f.name, f.flags, f.ref) for f in nt.fields]:
                dst.types.pop()
                memo[k] = j
                return j
    return memo[k]


def add_import(x: Ebx, file_guid: bytes, inst_guid: bytes) -> str:
    pair = (file_guid, inst_guid)
    if pair not in x.imports:
        x.imports.append(pair)
    return f"import:{x.imports.index(pair)}"


def template_of(index: LevelIndex, layer: Ebx, layer_name: str, type_name: str) -> tuple[Ebx, Instance]:
    """An instance of type_name to copy: from the layer itself, else from another layer that has one."""
    for inst in layer.instances:
        if layer.types[inst.type].name == type_name:
            return layer, inst
    for other in index.layers:
        if other.name != layer_name and type_name in other.types:
            x = index.read(other.name)
            for inst in x.instances:
                if x.types[inst.type].name == type_name:
                    return x, inst
    raise LayoutModError(f"no {type_name} in any zone layer to copy from")


def _clone_values(v):
    return json.loads(json.dumps(v))


def append_object(layer: Ebx, src: Ebx, template: Instance, values: dict) -> int:
    """A new instance (no GUID: not exported, as a map mod's) of template's type with values over a copy of its
    own, appended to the layer and to its WorldPartData's Objects. Returns its index."""
    k = copy_type(layer, src, template.type) if src is not layer else template.type
    v = _clone_values(template.values)
    v.update(values)
    layer.instances.append(Instance(k, None, v))
    idx = len(layer.instances) - 1
    root = layer.instances[0]
    if layer.types[root.type].name != "WorldPartData":
        raise LayoutModError("the layer's first instance is not its WorldPartData")
    root.values.setdefault("Objects", []).append(f"#{idx}")
    return idx


def place_prefab(index: LevelIndex, layer: Ebx, layer_name: str, prefab: Ebx, at: dict) -> str:
    root = prefab.instances[0] if prefab.instances else None
    kind = prefab.types[root.type].name if root else ""
    rod = ROD_FOR.get(kind)
    if not rod or root.guid is None:
        raise LayoutModError(f"not a blueprint GridNode can place ({kind or 'empty'})")
    src, tmpl = template_of(index, layer, layer_name, rod)
    blueprint = add_import(layer, prefab.guid, root.guid)
    values = {"Blueprint": blueprint, "BlueprintTransform": at}
    for key in ("ObjectVariation",):
        if key in tmpl.values:
            values[key] = None
    if "Excluded" in tmpl.values:
        values["Excluded"] = False
    append_object(layer, src, tmpl, values)
    return rod


def place_blocker(index: LevelIndex, layer: Ebx, layer_name: str, at: dict, size: list, cant: int) -> None:
    src, tmpl = template_of(index, layer, layer_name, "PamOBBCollisionEntityData")
    body_ref = (tmpl.values.get("PhysicsBodies") or [None])[0]
    if not (isinstance(body_ref, str) and body_ref.startswith("#")):
        raise LayoutModError("the barrier to copy has no RigidBodyData of its own")
    body = src.instances[int(body_ref[1:])]
    bi = copy_type(layer, src, body.type) if src is not layer else body.type
    layer.instances.append(Instance(bi, None, _clone_values(body.values)))
    body_idx = len(layer.instances) - 1
    half = {"x": float(size[0]) / 2, "y": float(size[1]) / 2, "z": float(size[2]) / 2}
    append_object(layer, src, tmpl, {
        "Transform": at, "HalfExtents": half, "PhysicsBodies": [f"#{body_idx}"], "Enabled": True,
        "CantStand": bool(cant & 1), "CantClimb": bool(cant & 2), "CantVault": bool(cant & 4), "CantWallrun": bool(cant & 8),
    })


def add_preloads(index: LevelIndex, edited: dict[str, Ebx], child: str, bundles: list[str]) -> str:
    """Adds bundles to the PreloadedBundleNames of the SubWorldReferenceObjectData that streams `child` in."""
    ref = next((r for r in index.refs if r.child == child), None)
    if not ref:
        raise LayoutModError(f"nothing streams {child} in: its prefabs' bundles cannot be added to it")
    x = edited.get(ref.file) or index.read(ref.file)
    found = False
    for inst in x.instances:
        if x.types[inst.type].name == parents.KIND and parents.bundle_key(str(inst.values.get("BundleName") or "")) == child:
            found = True
            have = inst.values.setdefault("PreloadedBundleNames", [])
            keys = {parents.bundle_key(p) for p in have if isinstance(p, str)}
            for b in bundles:
                if b not in keys:
                    have.append(b)
                    keys.add(b)
    if not found:
        raise LayoutModError(f"{ref.file} has no {parents.KIND} for {child}: its prefabs' bundles were not added")
    edited[ref.file] = x
    return ref.file


# ---- the whole layout ----------------------------------------------------------------------------------------
@dataclass
class Report:
    placed: list = field(default_factory=list)   # (kind, name, layer)
    skipped: dict = field(default_factory=dict)  # kind -> count
    preloads: list = field(default_factory=list)  # (zone bundle, [bundles], file)
    problems: list = field(default_factory=list)


def export(index: LevelIndex, edits: list[dict], layer_name: str = "") -> tuple[dict[str, Ebx], Report]:
    rep = Report()
    edited: dict[str, Ebx] = {}
    need: dict[str, list[str]] = {}  # zone bundle -> bundles to preload
    for e in edits:
        kind = str(e.get("kind") or "")
        if kind not in DONE:
            rep.skipped[kind] = rep.skipped.get(kind, 0) + 1
            continue
        try:
            at = transform_of(e)
            p = (at["trans"]["x"], at["trans"]["y"], at["trans"]["z"])
            lay = index.layer_named(layer_name) if layer_name else index.layer_for(p)
            x = edited.get(lay.name) or index.read(lay.name)
            edited[lay.name] = x
            if kind == "PlacedPrefab":
                asset = str(e.get("asset") or "")
                prefab = index.read(asset)
                rod = place_prefab(index, x, lay.name, prefab, at)
                rep.placed.append((rod, asset, lay.name))
                where = index.ebx.get(asset.lower(), (b"", set()))[1]
                if not where & index.loaded_with(lay.bundle):
                    b = parents.bundle_key(str(e.get("bundle") or "")) or parents.prefab_bundle(sorted(where), index.table)
                    if not b:
                        rep.problems.append(f"{asset}: no bundle to load it with; it will be missing in {lay.bundle}")
                        continue
                    lst = need.setdefault(lay.bundle, [])
                    for nb in [*parents.chain(index.table, b), b]:
                        if nb not in lst and nb not in index.loaded_with(lay.bundle):
                            lst.append(nb)
            else:
                place_blocker(index, x, lay.name, at, e.get("size") or [2.0, 3.0, 0.5], int(e.get("cant") or 0))
                rep.placed.append(("PamOBBCollisionEntityData", "blocker", lay.name))
        except Exception as err:  # noqa: BLE001 - one edit that cannot go in is reported, the rest still do
            rep.problems.append(f"{kind} at {e.get('position')}: {err}")
    for zone, bundles in need.items():
        if bundles:
            try:
                f = add_preloads(index, edited, zone, bundles)
                rep.preloads.append((zone, bundles, f))
            except LayoutModError as err:
                rep.problems.append(str(err))
    return edited, rep

