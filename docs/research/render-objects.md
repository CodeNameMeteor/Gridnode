# Render objects and the selection outline

Findings from building the layout editor's selection outline (2026-10-20 and 21). The outline's
fit was confirmed in game on 2026-10-21.

## What draws a model: RigidMeshEntity (or CompositeMeshEntity)

A static model is drawn by a render object, a `RigidMeshEntity` (vtable `0x141bf72c8` in the
EA app build). Its set-transform, `0x14376c060` (vtable +0x10), shows its layout:

| Offset | Holds |
|---|---|
| +0x10 | world transform, four float4 rows: right, up, forward, translation |
| +0x50 | world bounding sphere (centre xyz, radius), computed from the box below |
| +0x60 | `**(object + 0x60) + 0x20` points at the mesh's local box: min xyz, pad, max xyz |
| +0x88 | culling handle |

Some standalone models are drawn by a `CompositeMeshEntity` instead (vtable `0x141c012c0`, made
by `0x14377f750`; the creator `0x143183670` picks one or the other from a flag at +0x41 of its
arguments). It is built on the same base (`0x14375f930`) and shares the set-transform, so the
layout above is the same.

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
  level down. When there is none to be found (copies of large models), the outline uses the client
  entity's own local box: `ClientStaticModelEntity` caches it at +0x100 (min) and +0x110 (max),
  valid when bit 0 of +0x142 is clear, computed by its bounds function (vtable +0xb8,
  0x1431833a0: out +0x00 world transform, +0x40 / +0x50 box), which GridNode asks once on the
  client thread when it is not computed yet.

## What draws a group copy (2026-10-21)

A group member's block makes each copy's render object in 0x1431841f0 (from 0x143185940; taken
down in 0x143184c30): the RigidMeshEntity (from the block's +0xb8 factory), its render-world update,
an entry in the visibility system at 0x142563a30 (bounding sphere; handle at the object's +0x84),
and an entry in the **mesh streaming** manager at 0x142563410 (named "rigidMeshEntity": mesh index,
radius, position; handle at the object's +0x78).

### Moving one copy in place (solved 2026-10-22)

The object's set-transform (vtable +0x10, 0x14376c060) moves it and also its culling sphere (+0x50,
and its visibility entry through 0x14357b970) and its streaming entry (0x143555450, with the value
at 0x1419fd590, which is 1). Nothing else needs updating for those.

What made a moved copy vanish was the **render update**. The builder (0x143769330) writes only the
transform (+0x00), the mesh box (+0x40) and one float (+0x60). The rest comes from the caller, and
the group's own re-place (0x14318fbc0) leaves several fields unset. When the group makes the
object, it sets:

| Offset | What | From |
|---|---|---|
| +0x70 | object + 8 | |
| +0x78 | 16-byte hash of the mesh asset and the placement | 0x143b722e0(out, asset - 0x10 if `*(u16*)(asset+10) & 0x100`, else null; world transform) |
| +0x88 | u16 0 | |
| +0x8a | u16 | the block's factory (+0xb8), vtable +0x18 (factory, bool: member data +0x38 is a RigidMeshAsset) |
| +0x8c | per-view mask | member data +0x18, 12 bytes a copy {a, b, c}: a 0/1/2 → 1/3/2; c 0/1/2 → 4/0xc/8; b 0/1/2 → 0x20/0x30/0x10 (all zero, or no entry: 0x25) |
| +0x8d | 6 (bits 2 and 4) | |
| +0x90 | the object's +0x8c (its render-world id) | |

Sent with a zero mask, the copy is drawn in no view. With these filled (the hash kept from the
original placement), a 50 m building moved in place stays drawn (confirmed in game). Only the drawn
mesh moves; the collision (the group's compound shape) is a separate step.

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
