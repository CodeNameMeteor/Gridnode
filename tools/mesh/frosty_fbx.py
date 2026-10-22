#!/usr/bin/env python3
"""Make a Blender FBX export importable by Frosty, using Frosty's own export as the template.

Frosty imports a mesh from an FBX laid out the way it exports one: a node per LOD
(`<mesh>_lod0`, `_lod1`, ...) holding one mesh object per section, named after the section, with
per-vertex normals, binormals, tangents and UVs, triangles, and a material layer. Blender's export
differs: it renames same-named objects (`Crate.001`), keeps object moves and scales as transforms,
stores normals and UVs per polygon corner, and adds its scene's camera, light and cube. Frosty then
imports an empty or misplaced mesh (it is invisible).

This tool keeps the template (Frosty's export of the mesh being replaced) as it is and puts the
Blender geometry into it, section by section:

- each section is matched by its LOD node's name and its own name (Blender's `.001` suffixes
  ignored), or, when the Blender file has a single mesh, that mesh goes into every section;
- the Blender objects' transforms (moves, turns, scales made in Blender) are applied to the
  vertices; `--no-move` leaves out the translation;
- polygons are triangulated, corners with the same position, normal, tangent and UV share a
  vertex, and normals, binormals, tangents and UVs are written per vertex as Frosty writes them;
- names, the hierarchy, materials and every other part of the template stay as they were.

    python tools/mesh/frosty_fbx.py cratetableplastic01_Mesh_original.fbx my_crate_blender.fbx

writes `my_crate_blender_frosty.fbx`; import that in Frosty (open the mesh, Import in its toolbar).
Export from Blender as binary FBX with Geometry > Tangent Space ticked.
"""
from __future__ import annotations

import argparse
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbxbin as F  # noqa: E402

SEP = "\x00\x01"


# ---- scene ---------------------------------------------------------------------------------------
class Scene:
    def __init__(self, fbx: F.Fbx):
        self.fbx = fbx
        self.objects: dict[int, F.Node] = {}
        for n in fbx.section("Objects").kids:
            if n.props and chr(n.props[0][0]) == "L":
                self.objects[n.value(0)] = n
        self.parents: dict[int, list[int]] = {}
        self.children: dict[int, list[int]] = {}
        for c in fbx.section("Connections").children("C"):
            if c.value(0) != "OO":
                continue
            child, parent = c.value(1), c.value(2)
            self.parents.setdefault(child, []).append(parent)
            self.children.setdefault(parent, []).append(child)

    @staticmethod
    def name(n: F.Node) -> str:
        return n.value(1).split(SEP)[0]

    def models(self, kind: str | None = None) -> list[tuple[int, F.Node]]:
        return [(i, n) for i, n in self.objects.items() if n.name == "Model" and (kind is None or n.value(2) == kind)]

    def parent_model(self, oid: int) -> int | None:
        for p in self.parents.get(oid, []):
            if p in self.objects and self.objects[p].name == "Model":
                return p
        return None

    def kids_of(self, oid: int, kind: str) -> list[tuple[int, F.Node]]:
        out = []
        for c in self.children.get(oid, []):
            n = self.objects.get(c)
            if n is not None and n.name == "Model" and n.value(2) == kind:
                out.append((c, n))
        return out

    def geometry_of(self, model: int) -> tuple[int, F.Node] | None:
        for c in self.children.get(model, []):
            n = self.objects.get(c)
            if n is not None and n.name == "Geometry":
                return c, n
        return None


def props70(n: F.Node) -> dict[str, list]:
    out: dict[str, list] = {}
    p70 = n.child("Properties70")
    if p70:
        for p in p70.children("P"):
            out[p.value(0)] = [p.value(i) for i in range(4, len(p.props))]
    return out


# ---- transforms ----------------------------------------------------------------------------------
def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def ident():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def rot_xyz(deg):
    x, y, z = (math.radians(v) for v in deg)
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    rx = [[1, 0, 0, 0], [0, cx, -sx, 0], [0, sx, cx, 0], [0, 0, 0, 1]]
    ry = [[cy, 0, sy, 0], [0, 1, 0, 0], [-sy, 0, cy, 0], [0, 0, 0, 1]]
    rz = [[cz, -sz, 0, 0], [sz, cz, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    return matmul(rz, matmul(ry, rx))  # FBX's default order XYZ: X first


def local_matrix(n: F.Node, warn) -> list[list[float]]:
    p = props70(n)
    t = p.get("Lcl Translation", [0.0, 0.0, 0.0])
    r = p.get("Lcl Rotation", [0.0, 0.0, 0.0])
    s = p.get("Lcl Scaling", [1.0, 1.0, 1.0])
    pre = p.get("PreRotation", [0.0, 0.0, 0.0])
    order = p.get("RotationOrder", [0])[0] if "RotationOrder" in p else 0
    if order not in (0, None):
        warn(f"{Scene.name(n)}: rotation order {order} is not XYZ; turns may come out wrong")
    for k in ("RotationPivot", "ScalingPivot", "RotationOffset", "ScalingOffset"):
        if any(abs(v) > 1e-6 for v in p.get(k, [0.0, 0.0, 0.0])):
            warn(f"{Scene.name(n)}: {k} is not zero and is ignored; apply transforms in Blender (Ctrl+A) if it looks wrong")
    m = ident()
    m[0][3], m[1][3], m[2][3] = t
    sm = ident()
    sm[0][0], sm[1][1], sm[2][2] = s
    return matmul(m, matmul(rot_xyz(pre), matmul(rot_xyz(r), sm)))


def world_matrix(scene: Scene, oid: int, warn) -> list[list[float]]:
    m = local_matrix(scene.objects[oid], warn)
    p = scene.parent_model(oid)
    while p is not None:
        m = matmul(local_matrix(scene.objects[p], warn), m)
        p = scene.parent_model(p)
    return m


def inverse3(m):
    a = [[m[i][j] for j in range(3)] for i in range(3)]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
           + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    if abs(det) < 1e-12:
        raise ValueError("an object has zero scale")
    inv = [[0.0] * 3 for _ in range(3)]
    for i in range(3):
        for j in range(3):
            r = [x for x in range(3) if x != j]
            c = [x for x in range(3) if x != i]
            minor = a[r[0]][c[0]] * a[r[1]][c[1]] - a[r[0]][c[1]] * a[r[1]][c[0]]
            inv[i][j] = ((-1) ** (i + j)) * minor / det
    return inv, det


def apply_point(m, p):
    return tuple(m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] for i in range(3))


def apply_dir(m3, v):
    r = [m3[i][0] * v[0] + m3[i][1] * v[1] + m3[i][2] * v[2] for i in range(3)]
    n = math.sqrt(sum(x * x for x in r)) or 1.0
    return tuple(x / n for x in r)


# ---- geometry ------------------------------------------------------------------------------------
class Layer:
    """One layer element (normals, UVs...): its value for a polygon corner."""

    def __init__(self, node: F.Node, data_name: str, width: int):
        self.mapping = node.child("MappingInformationType").value()
        self.reference = node.child("ReferenceInformationType").value()
        self.data = node.child(data_name).value()
        idx = node.child(data_name + "Index") or node.child(data_name.rstrip("s") + "Index")
        self.index = idx.value() if idx else None
        self.width = width

    def get(self, corner: int, vertex: int, polygon: int):
        i = {"ByPolygonVertex": corner, "ByVertice": vertex, "ByVertex": vertex, "ByPolygon": polygon, "AllSame": 0}.get(self.mapping)
        if i is None:
            raise ValueError(f"layer mapping {self.mapping} is not supported")
        if self.reference == "IndexToDirect" and self.index is not None:
            i = self.index[i]
        w = self.width
        return tuple(self.data[i * w:(i + 1) * w])


def find_layer(geom: F.Node, element: str, data_name: str, width: int, name: str | None = None) -> Layer | None:
    nodes = geom.children(element)
    if name is not None:
        named = [n for n in nodes if n.child("Name") and n.child("Name").value() == name]
        nodes = named or nodes
    return Layer(nodes[0], data_name, width) if nodes else None


def build(geom: F.Node, matrix, uv_name: str | None, label: str):
    """Triangles with per-vertex position, normal, binormal, tangent and UV (Frosty's layout)."""
    pos = geom.child("Vertices").value()
    pvi = geom.child("PolygonVertexIndex").value()
    normals = find_layer(geom, "LayerElementNormal", "Normals", 3)
    binormals = find_layer(geom, "LayerElementBinormal", "Binormals", 3)
    tangents = find_layer(geom, "LayerElementTangent", "Tangents", 3)
    uvs = find_layer(geom, "LayerElementUV", "UV", 2, uv_name)
    if normals is None:
        raise ValueError(f"{label}: no normals in the Blender export")
    if tangents is None or binormals is None:
        raise ValueError(f"{label}: no tangents; export from Blender with Geometry > Tangent Space ticked")
    if uvs is None:
        raise ValueError(f"{label}: no UV map; add one in Blender (UV Editing) and export again")
    lin = [[matrix[i][j] for j in range(3)] for i in range(3)]
    inv, det = inverse3(matrix)
    normal_m = [[inv[j][i] for j in range(3)] for i in range(3)]  # inverse transpose
    flip = det < 0  # a mirrored object: keep the triangles facing out

    verts: list[tuple] = []
    seen: dict[tuple, int] = {}
    tris: list[tuple[int, int, int]] = []
    corner, polygon, start = 0, 0, 0
    for k, raw in enumerate(pvi):
        if raw >= 0:
            continue
        poly = list(range(start, k + 1))
        ids = []
        for c in poly:
            v = pvi[c] if pvi[c] >= 0 else ~pvi[c]
            p = apply_point(matrix, pos[v * 3:v * 3 + 3])
            n = apply_dir(normal_m, normals.get(c, v, polygon))
            t = apply_dir(lin, tangents.get(c, v, polygon))
            b = apply_dir(lin, binormals.get(c, v, polygon))
            uv = uvs.get(c, v, polygon)
            key = tuple(round(x, 6) for x in (*p, *n, *t, *b, *uv))
            if key not in seen:
                seen[key] = len(verts)
                verts.append((p, n, b, t, uv))
            ids.append(seen[key])
        for i in range(1, len(ids) - 1):  # a fan; Blender's "Triangulate Faces" makes this a no-op
            tri = (ids[0], ids[i], ids[i + 1])
            tris.append((tri[0], tri[2], tri[1]) if flip else tri)
        start = k + 1
        polygon += 1
    return verts, tris


def fill(template_geom: F.Node, verts, tris, warn, label: str) -> F.Node:
    """The template's geometry node with its arrays replaced (everything else as it was)."""
    g = template_geom.clone()
    pvi, edges, edge_seen = [], [], set()
    for t in tris:
        base = len(pvi)
        pvi.extend([t[0], t[1], ~t[2]])
        for e in range(3):
            a, b = t[e], t[(e + 1) % 3]
            key = (min(a, b), max(a, b))
            if key not in edge_seen:
                edge_seen.add(key)
                edges.append(base + e)

    def put(node: F.Node | None, name: str, values, kind: str = "d"):
        if node is None:
            return
        target = node.child(name)
        if target is None:
            warn(f"{label}: template layer {node.name} has no {name}; left as it was")
            return
        target.props[0] = F.array(kind, values)

    put(g, "Vertices", [x for v in verts for x in v[0]])
    put(g, "PolygonVertexIndex", pvi, "i")
    put(g, "Edges", edges, "i")
    for layer in g.kids:
        mapping = layer.child("MappingInformationType")
        if mapping is not None and layer.name != "LayerElementMaterial":
            if mapping.value() not in ("ByVertice", "ByVertex", "ByEdge"):
                warn(f"{label}: template {layer.name} is {mapping.value()}; written per vertex anyway")
                mapping.props[0] = F.string("ByVertice")
            if layer.child("ReferenceInformationType") is not None:
                layer.child("ReferenceInformationType").props[0] = F.string("Direct")
            for idx in [k for k in layer.kids if k.name.endswith("Index")]:
                layer.kids.remove(idx)
        if layer.name == "LayerElementNormal":
            put(layer, "Normals", [x for v in verts for x in v[1]])
        elif layer.name == "LayerElementBinormal":
            put(layer, "Binormals", [x for v in verts for x in v[2]])
        elif layer.name == "LayerElementTangent":
            put(layer, "Tangents", [x for v in verts for x in v[3]])
        elif layer.name == "LayerElementUV":
            put(layer, "UV", [x for v in verts for x in v[4]])
        elif layer.name == "LayerElementSmoothing":
            put(layer, "Smoothing", [1] * len(edges), "i")
        elif layer.name == "LayerElementColor":
            warn(f"{label}: the template has vertex colours; written white")
            put(layer, "Colors", [1.0] * (4 * len(verts)))
        elif layer.name == "LayerElementMaterial":
            mats = layer.child("Materials")
            if mats is not None and mapping is not None and mapping.value() == "ByPolygon":
                warn(f"{label}: the template's section has several materials; every triangle gets the first")
                mats.props[0] = F.array("i", [0] * len(tris))
    return g


def unit_scale(fbx: F.Fbx) -> float:
    """Centimetres per unit in the file (FBX's UnitScaleFactor: 1 = cm, 100 = m)."""
    try:
        v = props70(fbx.section("GlobalSettings")).get("UnitScaleFactor", [1.0])[0]
        return float(v) if v else 1.0
    except ValueError:
        return 1.0


def bbox(points):
    if not points:
        return (0, 0, 0), (0, 0, 0)
    return tuple(min(p[i] for p in points) for i in range(3)), tuple(max(p[i] for p in points) for i in range(3))


def strip_suffix(name: str) -> str:
    return re.sub(r"\.\d{3}$", "", name)


# ---- main ----------------------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("template", help="Frosty's export of the mesh you are replacing (.fbx)")
    ap.add_argument("blender", help="your Blender export (binary .fbx, Tangent Space ticked)")
    ap.add_argument("-o", "--out", help="output file (default: <blender>_frosty.fbx)")
    ap.add_argument("--mesh", help="the Blender object to use for every section (when matching by name does not work)")
    ap.add_argument("--no-move", action="store_true", help="leave out the objects' translation (keep scale and rotation)")
    a = ap.parse_args(argv)

    warnings: list[str] = []
    warn = warnings.append
    tmpl, blend = F.read(a.template), F.read(a.blender)
    ts, bs = Scene(tmpl), Scene(blend)

    # The template's sections: (LOD node name, section model, its geometry).
    sections = []
    for lid, lod in ts.models("Null"):
        if ts.parent_model(lid) is not None:
            continue
        for sid, sec in ts.kids_of(lid, "Mesh"):
            g = ts.geometry_of(sid)
            if g:
                sections.append((ts.name(lod), ts.name(sec), g[0], g[1]))
    if not sections:
        print("The template has no LOD nodes with mesh sections: is it an export from Frosty's mesh editor?", file=sys.stderr)
        return 1

    # Each file's own unit (Frosty can export in mm, cm or m; Blender writes cm by default): Blender's
    # positions are converted to the template's.
    units = unit_scale(blend) / unit_scale(tmpl)
    if abs(units - 1.0) > 1e-9:
        print(f"Units: the template is in {unit_scale(tmpl):g} cm per unit, the Blender file in {unit_scale(blend):g}: "
              f"Blender's positions times {units:g}")
    blender_meshes = [(i, n) for i, n in bs.models("Mesh") if bs.geometry_of(i)]
    chosen = None
    if a.mesh:
        chosen = next(((i, n) for i, n in blender_meshes if bs.name(n) == a.mesh), None)
        if chosen is None:
            print(f"No mesh object named {a.mesh!r} in the Blender file; it has: {', '.join(bs.name(n) for _, n in blender_meshes)}", file=sys.stderr)
            return 1

    def match(lod_name: str, sec_name: str, count_in_lod: int):
        if chosen:
            return chosen
        lod = next(((i, n) for i, n in bs.models("Null") if bs.name(n) == lod_name), None)
        if lod is not None:
            kids = [(i, n) for i, n in bs.kids_of(lod[0], "Mesh") if bs.geometry_of(i)]
            same = [k for k in kids if strip_suffix(bs.name(k[1])) == sec_name]
            if same:
                return same[0]
            if len(kids) == 1 and count_in_lod == 1:
                return kids[0]
        if len(blender_meshes) == 1:
            return blender_meshes[0]
        return None

    per_lod: dict[str, int] = {}
    for lod_name, *_ in sections:
        per_lod[lod_name] = per_lod.get(lod_name, 0) + 1

    objects = tmpl.section("Objects")
    t_points, n_points = [], []
    for lod_name, sec_name, gid, tgeom in sections:
        label = f"{lod_name}/{sec_name}"
        m = match(lod_name, sec_name, per_lod[lod_name])
        if m is None:
            names = ", ".join(bs.name(n) for _, n in blender_meshes)
            print(f"{label}: no matching object in the Blender file (it has: {names}). Keep the names from Frosty's export, "
                  f"or pick one with --mesh NAME.", file=sys.stderr)
            return 1
        mid, mnode = m
        matrix = world_matrix(bs, mid, warn)
        if a.no_move:
            matrix[0][3] = matrix[1][3] = matrix[2][3] = 0.0
        matrix = [[matrix[i][j] * (units if i < 3 else 1.0) for j in range(4)] for i in range(4)]
        bgeom = bs.geometry_of(mid)[1]
        uv_layer = tgeom.child("LayerElementUV")
        uv_name = uv_layer.child("Name").value() if uv_layer and uv_layer.child("Name") else None
        verts, tris = build(bgeom, matrix, uv_name, label)
        old = tgeom.child("Vertices").value()
        t_points += [tuple(old[i:i + 3]) for i in range(0, len(old), 3)]
        n_points += [v[0] for v in verts]
        objects.kids[objects.kids.index(tgeom)] = fill(tgeom, verts, tris, warn, label)
        print(f"{label}: from Blender's {bs.name(mnode)!r}: {len(verts)} vertices, {len(tris)} triangles "
              f"(the original had {len(old) // 3} vertices)")

    out = a.out or os.path.splitext(a.blender)[0] + "_frosty.fbx"
    F.write(tmpl, out)
    (tmn, tmx), (nmn, nmx) = bbox(t_points), bbox(n_points)
    print(f"Size: original {tuple(round(tmx[i] - tmn[i], 3) for i in range(3))}, yours {tuple(round(nmx[i] - nmn[i], 3) for i in range(3))}")
    tc = [(tmn[i] + tmx[i]) / 2 for i in range(3)]
    nc = [(nmn[i] + nmx[i]) / 2 for i in range(3)]
    off = math.sqrt(sum((tc[i] - nc[i]) ** 2 for i in range(3)))
    if off > 1.0 and not a.no_move:
        warn(f"your mesh's middle is {off:.1f} units from the original's: it was moved in Blender. "
             f"If you did not mean to move it, run again with --no-move.")
    for w in dict.fromkeys(warnings):
        print("note:", w)
    print(f"Wrote {out}: import it in Frosty (open the mesh, Import in its toolbar).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
