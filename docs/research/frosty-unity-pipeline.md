# Notes: Frosty–Unity Pipeline (FUP)

A work-in-progress Frosty Editor plugin with a Unity half. It exports a level layer to Unity as a
scene, then writes edits back into the game data (EBX and Havok resources). Reviewed 2026-10-20 for
what it shows about objects, meshes and collision in Catalyst.

**Licence: GPL-3.0.** GridNode is MIT, so none of its code is copied. These notes record facts about
the game, checked against GridNode's own reflected type dump where possible.

## How it works

1. **Frosty → Unity:** walks a layer's EBX objects and writes a JSON tree of objects with their
   transforms, mesh names, and the Havok resource and index of their collision. Unity rebuilds the
   scene from FBX exports of the meshes.
2. **Unity → Frosty:** reads the JSON back. It patches transforms in the EBX
   (`BlueprintTransform`, or `Transform` for `SpatialEntityData` types), patches Havok instance
   transforms in the binary physics resources, and adds new references.

It edits data files on disk. GridNode edits the running game, so the two are complementary.

## Findings that matter to GridNode

### Most level props are in StaticModelGroups, not separate entities

`StaticModelGroupEntityData` (`ServerStaticModelGroupEntity` / `ClientStaticModelGroupEntity` at
runtime; both are physics entities) holds `MemberDatas`, a list of `StaticModelGroupMemberData`:

| Field (offset) | Meaning |
|---|---|
| `MemberType` (+0x30) | the member's entity data (`GamePhysicsEntityData`, usually a static model) |
| `MeshAsset` (+0x38) | its mesh |
| `InstanceCount` (+0x40) | how many copies |
| `InstanceTransforms` (+0x08) | one `LinearTransform` per copy, **often empty** |
| `PhysicsPartRange`, `PhysicsPartCountPerInstance` (+0x48, +0x50) | which collision parts belong to which copy |
| `NetworkIdRange`, `NetworkIdCountPerInstance` | per-copy network ids |

When `InstanceTransforms` is empty (most members, per FUP), the copy's placement lives only in the
Havok collision data. The rendered position must come from the same data at runtime.

### One collision body for a whole group: `hknpStaticCompoundShape`

A group's collision is a `StaticModelGroupPhysicsComponentData` → `PhysicsBodies` → `RigidBodyData`
→ `HavokAsset.Resource`. That resource holds an `hknpStaticCompoundShape` with one *instance* per
copy. This is the `GroupRigidBody` GridNode's ray hit on many props. Each instance stores:

- a 3×4 transform, each row a float4 (w is other data): rotation rows, then translation;
- a scale float4;
- the child shape pointer and ten u32s of flags and data.

Shapes with flag `0x10` are mirrored in X before their transform is applied (a negative
determinant). The compound shape also has a BVH tree and a **quantization domain**, a min/max AABB
around all its instances (FUP calls these `Unknown1`/`Unknown2`). An instance outside the domain is
never collision-tested, so collision moved far from where it was authored silently disappears.
FUP's on-disk fix zeroes the BVH tree and widens the domain.

For GridNode this means:

- Moving a grouped prop at runtime means moving its **instance** inside the group's compound shape,
  and its render instance. Moving a separate `RigidBody` (what GridNode does now) doesn't apply.
- After moving an instance the BVH and domain need refreshing, or the collision will be missed.
  Havok's runtime may have a call for this, which would save rebuilding them by hand.
- Removing a grouped prop may be easiest by disabling its instance; compound-shape instances have
  enable flags in Havok.
- A ray hit's body is the group body, and the hit's shape key should name the instance (the hit's
  `part`, filled when `PhysicsRayInput.wants` has bit 2). The member's `PhysicsPartRange` maps that
  back to a member and copy.

### Stable ids for level objects

Every entity data has `DataBusPeer.Flags` (+0x10, u32), and FUP keys objects by it (it writes a
24-bit hash into it for objects it adds). Objects inside a blueprint also have `IndexInBlueprint`.
FUP renumbers it after removing objects because gaps crash the engine. A layout file could
key an edit by (blueprint/partition, `Flags`) instead of by creation position. That survives two
identical props at the same place, and props that a previous edit moved.

### Hiding instead of deleting

FUP removes a prop by moving it to y = −15000 (EBX and Havok alike). It never deletes it, because
deletion breaks indices that other data refers to. GridNode can use the same trick at runtime:
moving a prop far below the map removes it without destroying anything.

### Placement fields differ by type

- References (`ObjectReferenceObjectData`, `SpatialPrefabReferenceObjectData`) are placed by
  `BlueprintTransform`.
- `SpatialEntityData`-derived data (OBB barriers, magropes, the static model group's anchor) are
  placed by `Transform`. They have no `BlueprintTransform`, and FUP throws if it asks for one.

### Gameplay objects worth spawning

- **`PamOBBCollisionEntityData`** (an invisible box barrier): `Transform`, `HalfExtents`, `Enabled`,
  `CollisionLayer = CharacterLayer`, Pam flags `CantStand` / `CantClimb` / `CantVault` /
  `CantWallrun` (+0x90..0x93). It needs a companion `RigidBodyData` in `PhysicsBodies`
  (`MotionType_Invalid`, `CharacterLayer`).
- **`PamMagropeEntityData`** (the grapple / pull-up rope): `Transform`, `MagropeType` (+0x70),
  `Radius` (+0x74), `VerticalStretch` (+0x78), `Triggers` (+0x80), `Enabled` (+0x88),
  `GraphicsOffset` (+0x60).

These two make a good first target for M7 step 3 (spawning). They are self-contained, have no mesh,
and matter for parkour layouts.

### Coordinates

Frostbite is right-handed (Y up) and Unity left-handed. FUP mirrors one axis on the way in and out,
and wraps angles to [−180, 180]. The Blender add-on (step 6) needs the same care, since Blender is
right-handed with Z up.

## What to do with this

1. **Grouped props:** read the `GroupRigidBody`'s compound shape at runtime (instances, domain,
   tree). Map a hit's shape key to an instance, move it, then refresh the tree. Also find the
   matching render instance on `ClientStaticModelGroupEntity`.
2. **Layout keys:** add (partition, `Flags`) as an edit key, alongside the position.
3. **Removal:** use "hide at −15000" for both separate and grouped props.
4. **Spawning:** start with `PamOBBCollisionEntityData` and `PamMagropeEntityData`.
