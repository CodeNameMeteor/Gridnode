# Render objects and the selection outline

Findings from building the layout editor's selection outline (2026-10-20 and 21). The outline's
fit was confirmed in game on 2026-10-21.

## What draws a model: RigidMeshEntity

A static model is drawn by a render object, a `RigidMeshEntity` (vtable `0x141bf72c8` in the
EA app build). Its set-transform, `0x14376c060` (vtable +0x10), shows its layout:

| Offset | Holds |
|---|---|
| +0x10 | world transform, four float4 rows: right, up, forward, translation |
| +0x50 | world bounding sphere (centre xyz, radius), computed from the box below |
| +0x60 | `**(object + 0x60) + 0x20` points at the mesh's local box: min xyz, pad, max xyz |
| +0x88 | culling handle |

The local box is the mesh's own, the one the renderer culls with, so it fits what is drawn.
Drawn in the object's own transform, it also has the right axes.

The box and the transform both have to come from the render object that draws *this* object.
The first versions took the first render object found near the entity's memory. That was often a
neighbour's, which gave the right place but the wrong size (a 1.24 × 1.12 m wall panel's box on a
row of slats). GridNode now accepts a render object only if its translation is within 0.75 m of
the selection.

## Finding it

- **Group props** (most props in a level): each member has a block in the client group's array
  (`ClientStaticModelGroupEntity` +0xe0, count at +0xec, 0x130 bytes each), which keeps a list of
  render objects, one per copy, at block +0xc8.
  When that list is empty, the game asks the block's physics component (block +0x20, vtable
  +0x210, with the copy's first physics part) for it, as `0x14318fbc0` does. That is a call, so
  GridNode makes it on the client thread.
- **Standalone models and copies**: a render object the client entity points to, directly or one
  level down. This path is less certain; a selection without a render object falls back to the
  collision box.

Read from the group's render objects, the transforms match the physics instance transforms
exactly (same rows, same translation), so group-prop placement read from Havok is what is drawn.

## The collision box, for comparison

A group's collision is one `hknpStaticCompoundShape` (the `GroupRigidBody` at +0x58). Its
instance tree at +0xc0 is a Havok static tree of 6-byte compressed nodes (box update
`0x143e99090`):

- nodes at tree +0x10, count at +0x18; the root's box at +0x20 (min) and +0x30 (max);
- a node's bytes 0..2 place its box inside its parent's, per axis:
  min = parent min + (b >> 4)² × step, max = parent max − (b & 15)² × step, where
  step = parent size × the constant at `0x1425a8e10`;
- byte 3 bit 7 marks an inner node: the left child is the next node, the right child is
  ((byte 3 & 0x7f) << 16 | u16 at +4) × 2 nodes on; a leaf's u16 at +4 is its instance.

The collision box is in world space and can be much larger than the mesh (a railing's collision
was a 5.8 × 3.7 m slab). The outline uses it only until the render object is found.

## Models that point away from their pivot

Many models are built extending from a corner, often towards −X and −Z (the railing's box runs
from x −1.28 to −0.04). Axes drawn from the pivot then point away from the object, which looks
wrong though it is not. The outline draws the axes from the box's middle and marks the pivot with
a dot.
