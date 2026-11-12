# Volumes and placeable objects: what the game's data uses

From `gridnode.py assets usage --kind-of SpatialEntityData` on the user's install (2026-11-12): 92 types placed
in the world, over all 43,661 EBX files. Numbers are files / instances. Fields are the type's own (the common
ones, `Transform`, `Enabled`, `Flags` and the component counts, are left out). For planning the general volume
editor (user, 2026-11-12): place any volume, blocker, dash part, mission part or teleport, then move and resize it.

## Boxes: transform + `HalfExtents` (one editor covers them all)

| Type | Files / instances | Own fields |
|---|---|---|
| `PamOBBCollisionEntityData` (movement blockers) | 374 / 2,468 | `HalfExtents`, `PhysicsBodies`, `CollisionLayer`, `Material`, `CantStand`, `CantClimb`, `CantVault`, `CantWallrun` |
| `OBBCollisionEntityData` | 66 / 236 | the same without the Cant* flags |
| `PamMovementExclusionEntityData` | 206 / 910 | `HalfExtents`, `ExcludeVault`, `ExcludeHeaveUp`, `ExcludeHang`, `ExcludeWallrun`, `ExcludeMagrope` |
| `PamCustomArcEntityData` | 81 / 172 | `HalfExtents`, `ForwardSpeed`, `JumpHeight`, `ArcType`, `Bidirectional` |
| `PamUncontrollableSlideEntityData` | 81 / 206 | `HalfExtents` |
| `PamCustomJumpOrFallEntityData` | 30 / 42 | `HalfExtents`, ... |
| `PamSoftLandingEntityData` | 4 / 4 | `HalfExtents`, `Radius`, `Guidance` |

The last five share `PamFindableMovementVolumeEntityData` (its `Enabled` shows as
`PamFindableMovementVolumeEntityData.Enabled`).

## Other shapes

| Type | Files / instances | Shape and fields |
|---|---|---|
| `PamAreaTriggerData` (triggers; kill volumes are server entities on these) | 276 / 1,107 | `GeometryTransform`, `Include`, `Delay`, `RunOnce`, `TriggerOnlyOnLeave`, ... (the geometry is not a field: to find) |
| `PamSphereCollisionEntityData` | 6 / 7 | `Position`, `Radius`, Cant* flags |
| `PamMagropeEntityData` | 49 / 171 | `MagropeType`, `Radius`, `VerticalStretch`, `GraphicsOffset`, `Triggers` |
| `PamGameSplineEntityData` (ladders, pipes, ziplines, swings, balance) | 218 / 608 | `LocalPoints`, `Normals`, `SplineType`, `SplineRadius`, `AssistSpeed`, exit angles, ... |
| `CharacterLookAtTriggerEntityData` | 25 / 36 | |

## Dash, challenges, missions, spawns

| Type | Files / instances | Example file |
|---|---|---|
| `PamCheckpointEntityData` | 1 / 1 | `gameplay/social/timetrial/pf_timetrialcheckpoint` |
| `PamChallengeMarkerEntityData` | 2 / 2 | `gameplay/social/reachthis/pf_reachthismarker` (`CheckpointOrder`, `Time`, `HasDuration`) |
| `PamNamedChallengeEntityData` | 3 / 3 | `gameplay/social/pf_hackablebillboard` |
| `PamSpawnLocationEntityData` | 35 / 302 | mission logic layers |
| `PamMissionCheckpointEntityData` | 1 / 1 | |
| `AlternateSpawnEntityData` | 142 / 588 | |

A dash or mission is a prefab: these entities plus logic entities joined by event and property connections. The
prefab YAML (`gridnode.py assets ebx NAME`) shows the connections; the next step is reading a dash prefab and the
time-trial checkpoint prefab to find the smallest set that makes one work.

The full list (all 92 types, with examples and every field) is in the user's `bin/assets/usage.yaml` (game data:
not committed).
