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
| `PamAreaTriggerData` (triggers; kill volumes are server entities on these) | 276 / 1,107 | `GeometryTransform`, `Include`, `Delay`, `RunOnce`, `TriggerOnlyOnLeave`, ... (the geometry is a link, below) |
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

## What the prefabs show (2026-11-12, from their YAML)

**Teleport** (`gameplay/logic/pf_teleportplayer`, 21 entities): a `PamTeleportEntityData` whose two links point at a
`LocatorEntityData` (where to) and a `CharacterProxyData` (who: the player); its `Teleport` event does the move.
The prefab's interface event `Teleport` starts a sequence: input locked (`InputRestrictionEntityData` Activate), a UI
fade (`PamUIInputControlEntityData`), the destination's area streamed in (`ObjectReferenceObjectData` StreamIn),
then the teleport, input unlocked; the locator's transform is the interface's `Transform` property. A teleport
volume is then a trigger area whose enter event fires this prefab's `Teleport`, with the locator at the target.

**Dashes are named challenges, not UGC** (user, 2026-11-12). `gameplay/social/namedchallenges` (in `globals`) is a
`PamNamedChallengesAsset`: 62 `PamNamedChallengeData` (Sid such as `ID_NC_THEVIEW02`, star thresholds in
centiseconds, stats codes `ch_rrt_*` for "runner's route" = dash, preview texture, flag) and 7 challenge types. The
routes themselves (start, checkpoints, finish) are not in it. `PamTimeTrial*` / `ReachThis` prefabs are the UGC
system.

`levels/sp/sp_maincity/sp_maincity/namedchallenges/namedchallengedatabase_win32` is not the route either: it is the
map index, one `PamNamedChallengeDatabaseAsset` with 40 `Entries` (22 `RunnersRoute` = dashes, 18
`HackableBillboard`). Each entry has the map `Transform` (where the start sits in the world), `ChallengeDataGuid` (the
`PamNamedChallengeData` above), `ContentId`, `Sid`, the star thresholds in seconds (`ID_NC_THEVIEW02`: 46 / 38 / 35,
cheater 21), the stats codes and `HideInMap`. No type in the dump has a checkpoint list (`PamNamedChallengeData`,
`PamNamedChallengeTypeData` and `PamNamedChallengeTransformOverride` checked), so a dash's checkpoints and finish are
level logic. A dash has only a start and a finish (user, 2026-11-12), no checkpoints.

`gameplay/social/pf_runnersroute` (135 instances) is one dash. Its parts:

- `PamNamedChallengeEntityData` (#5) with events `Start`, `Finish`, `Restart`, `Trigger`; it drives a
  `PamStopWatchEntityData` whose time goes back into it, and it enables a 50 m `SphereData` trigger (the
  "press to start" area near the marker).
- A logic prefab (#13, import 5) fires `OnEnterStart` (through an event gate to `Start`) and `OnEnterFinished` (to
  `Finish`). Its geometry is not inside the dash prefab: three object links leave through the interface,
  `StartGeometry`, `StopGeometry` (the finish) and one not yet named (`0x32107415`). So the layer that places the
  dash links its own start and finish volumes in.
- The interface also takes `ChallengeSelector` (the link that picks the `PamNamedChallengeData`), `FinishTransform`
  (camera at the end), `Activated`, and exposes `Trigger`.

A new dash therefore needs a `PamNamedChallengeData`, a database entry, the placed `pf_runnersroute`, and two
volumes (shape plus area) linked as `StartGeometry` and `StopGeometry`.

In game (user, 2026-11-12): a sphere trigger near the start is always on; using it shows the leaderboard; choosing to
play turns the start and finish volumes on; entering the start starts the timer at the top; entering the finish stops
it and shows the leaderboard again.

Each dash has its own layer in `levels/sp/sp_maincity/mainlogic/mainlogic` (`assets used-by
gameplay/social/pf_runnersroute`: 21 layers plus `mainlogic_networkregistry_win32`). `layer17_ct_socialrunnerroute01`
is a `WorldPartData` holding the placed `pf_runnersroute`, `objects/props/runnerobject/dasharrows/pf_dasharrow_start`
beside it, 10 `pf_dasharrow_medium` along the way (scaled and turned to point along the route), `pf_dasharrow_end`
at the far end, and an `OBBData` (half extents 3.7 x 2.0 x
5.8 m) beside the prefab that no object in the layer uses. The layer has no connections at all; the links are in
`levels/sp/sp_maincity/mainlogic/mainlogic` (a `SubWorldData` with 52 `WorldPartReferenceObjectData`, one per
layer), which links objects of different layers by import:

- `pf_runnersroute` `StartGeometry` -> the layer's `OBBData` (the start volume is a bare shape: transform plus half
  extents, no trigger of its own; the trigger is inside the dash prefab).
- `pf_runnersroute` `StopGeometry` -> `pf_dasharrow_end`'s `EndVolume` (the finish volume is inside the end arrow).
- `pf_runnersroute` `ChallengeSelector` -> an object in another file (file `5201a4e6...`, instance `da619d7a...`; not
  a `ChallengeDataGuid` of the database: still to name).
- `pf_runnersroute` `Activated` -> `Enabled` of every arrow (they show only while the dash is on), and two more
  properties into the end arrow (hashes `0x13f3c4fc` -> `0x4db752b4`, `0x90100ae4` -> `0xc2490554`: unnamed).

So a custom dash in a mod is: the dash prefab, a start `OBBData`, the arrows with the end arrow last, and these
links in the level's `SubWorldData` (or in our own blueprint that holds all of them).

**Trigger shapes** are separate instances: `VolumeVectorShapeData` (a closed polygon of `Points` with a `Height`,
`IsClosed`), `BFWaypointsShapeData` (open paths) and `SphereData` (`Radius`, `Position`). A trigger does not hold
its shape in a field: the blueprint's `LinkConnections` has a link from the trigger to the shape instance, with
`SourceFieldId` = hash of `Geometry` (838548383) and `TargetFieldId` 0 (the whole object). Seen in
`me_sound/vo/logic/ambientanimconversation` (a `PamAreaTriggerData` linked to a 15 m `SphereData`). The hash is
Frostbite's name hash (5381, `h = h*33 ^ c`, 32-bit). A placed trigger therefore needs three things: the trigger
data, a shape instance, and that link; resizing it means changing the shape (or `GeometryTransform`).

The full list (all 92 types, with examples and every field) is in the user's `bin/assets/usage.yaml` (game data:
not committed).
