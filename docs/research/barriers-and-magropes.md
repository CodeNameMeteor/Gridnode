# Barriers and magropes (research, started 2026-10-22)

Goal: place invisible barriers (with Catalyst's "can't stand / climb / vault / wallrun" flags) and
magropes from the layout editor. Findings so far, from the reflected types and the executable.

## Types

| Class | Realms | Base | Creators |
|---|---|---|---|
| `PamOBBCollisionEntity` (0xa0 bytes, vtable 0x141c4e1a0) | client and server | `OBBCollisionEntity` < `Entity` (not a SpatialEntity) | client 0x142579a90, server 0x142579a60 |
| `PamClientMagropeEntity` (0x68 bytes, vtable 0x141c619e0) | client only | `SpatialEntity` | 0x14257c648 |
| `PamClientMagropeRenderEntity` (draws the rope) | client | `PamClientRopeRenderEntity` | 0x141c410f8 (vtable) |

Data:

- `PamOBBCollisionEntityData` (0xa0; TypeInfo 0x142881550): from `OBBCollisionEntityData` /
  `SpatialEntityData`: `Transform` +0x20, `HalfExtents` +0x60, `PhysicsBodies` +0x70,
  `CollisionLayer` +0x78, `Material` +0x7c, `Enabled` +0x80; its own `CantStand` +0x90,
  `CantClimb` +0x91, `CantVault` +0x92, `CantWallrun` +0x93.
- `PamMagropeEntityData` (0x90; TypeInfo 0x142884ce0): `Transform` +0x20, `GraphicsOffset` +0x60,
  `MagropeType` +0x70, `Radius` +0x74, `VerticalStretch` +0x78, `Triggers` +0x80, `Enabled` +0x88.

## A barrier's collision is built when it is enabled

The barrier's init (vtable +0x78, 0x143335bc0; the client one is 0x143335c20) reads its `Enabled`
property and, when set, enables it (0x143333310; client 0x143333590). That reads the data's
`HalfExtents` (+0x60) and `PhysicsBodies` (+0x70), creates a physics body (0x1434ee7e0) and keeps
its id at entity +0x90, then sets the body's transform and settings (0x1434f6d90, 0x1434f8930,
0x1434f8980, 0x1434efca0). So the box comes from `HalfExtents` at runtime: a barrier made from a
copy of a loaded barrier's data, with other `HalfExtents` and flags, should collide as edited.

To find in Ghidra: where the enable takes the barrier's transform (0x143165bc0 is called before the
body is placed), how a barrier is moved after it is made (no SpatialEntity_setTransform), and how
it is disabled and taken down: its vtable +0x30 is the empty stub and +0x88 is its own deinit
(0x143335b30), so GridNode's destroy sequence (written for spatial entities) does not apply yet.

## Plan

1. Show the barriers and magropes near the camera in the editor (boxes and circles drawn like the
   selection outline), from the live entities of these classes.
2. Copy one: spawn its class from a copy of its data (GridNode's own memory, kept for the copy's
   life), with the transform, size and flags edited in the editor.
3. Save them in layouts with their fields, and remove them (their own deinit and teardown).
