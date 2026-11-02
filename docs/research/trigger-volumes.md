# Trigger volumes: shapes and what they set off

Research 2026-11-01, for drawing the level's triggers in the trainer (Volumes tab) and sorting them
by what they do. From the executable in Ghidra and probes in game (Fly Trap and Old Friends
cutscene triggers, the safehouse trigger, the Back in the Game checkpoint). Mirror's Edge Catalyst
PC (EA app build), image base 0x140000000. Confidence: **confirmed** (seen in game), **read** (read
from the code, not tested).

## Summary

- An area trigger keeps a **geometry cache**: the shapes it tests Faith against each tick, with
  their transforms. Drawing that cache draws the trigger as the game sees it (**confirmed**: the
  shapes sat where the four triggers above fire).
- What a trigger is for is not in the trigger: its `OnEnter` / `OnLeave` events go through its
  bus's **event connections** to logic and to **prefab references**, whose blueprint names say it
  (`Gameplay/Logic/PF_SafeSpawnControl`, `Animations/Prefabs/Missions/Flytrap/...`,
  `ME_Sound/Music/...`). Following the links two steps through logic in the same bus and sorting by
  those names gives usable categories (**confirmed** for the four triggers).
- Blueprint names of the trigger's own bus came out empty (bus +0x38); not needed so far.

## Classes

| Class | Notes |
|---|---|
| `PamServerAreaTriggerEntity` (0x1890 bytes, vtable 0x141c4b168) | The game's own trigger: about 1,000 to 1,450 live in the city. **Not** a subclass of `ServerAreaTriggerEntity` in the type list (asking for the base class finds none of them). Its vf15 (0x14395bc50) builds two geometry caches with `FUN_14330bac0` from the data's `Geometry` links (0x31fb3b9f = djb2 "Geometry"): +0x1b8 (the one tested) and +0x70. |
| `ServerAreaTriggerEntity` (vtable 0x141b58bb0) | The engine's: cache at +0x1a8, tested in its vf25 (0x1433c8230) by `FUN_14330d320`. None seen live in the city. |
| `PamServerKillVolumeEntity`, `CameraEnterAreaTriggerEntity`, `PamServerMissionCheckpointEntity` | No cache at either offset; not drawn yet. |

Both have the bus's interface at +0x20 (**the bus is 8 bytes before it**) and the data
(`PamAreaTriggerData`) at +0x28.

## The geometry cache

Read from `FUN_14330d320` (the inside test), **confirmed** for kinds 1 to 3:

| Offset | What |
|---|---|
| +0x00, +0x04 | x, z of a bounding circle (local space for kinds 2 and 3) |
| +0x08 | shapes: `BaseShapeData*[]`, count at -4 |
| +0x10 | transforms: `LinearTransform[]` (0x40 each), count at -4; **world to local** (the test applies them to Faith's position) |
| +0x18 | the circle's radius squared |
| +0x20 | kind: 1 shapes in world space; 2 one transform (none: the bus's, from `FUN_14314df80`); 3 one transform per shape; 4 looked up each test (`FUN_143124f50`) |

The shapes are the data objects themselves, tested by `FUN_143427220` by type:

| Shape | Layout |
|---|---|
| `SphereData` | Position +0x20, Radius +0x18 |
| `AABBData` | Position +0x20, HalfExtents +0x30 |
| `OBBData` | Transform +0x20 (its own placement), HalfExtents +0x60 |
| `VolumeVectorShapeData` | Points +0x18 (Vec3, 0x10 apart, count at -4): an outline in x / z (`FUN_1434274b0`); Height +0x28 up from the lowest point, 0 = unbounded in y |

Not seen yet: kind 2 with no transform (the bus's transform), kind 4.

## Event connections

`EntityBus_fireEvent` (0x14314e4a0): bus +0x68 -> +0x120 (+0x148 when bus +0x3c bit 28) holds
{begin, end} of pointers to 0x20-byte records, sorted by source and event:

| Offset | What |
|---|---|
| +0x00 | source: the sending entity's **data** (not the entity) |
| +0x08 | target (data: logic, a prefab reference, an interface descriptor...) |
| +0x10 | event id (djb2-xor: `OnEnter`, `OnLeave`, `Out`) |
| +0x14 | the target's event id |
| +0x18 | kind (3, 4, 5 seen) |

A prefab reference (`LogicPrefabReferenceObjectData`, `SpatialPrefabReferenceObjectData`) names its
blueprint through its `Blueprint` field's `Name`. A sublevel's bus can hold thousands of records
(3,168 at the safehouse).

## What the four triggers reach

| Trigger | Reaches (first steps) |
|---|---|
| Back in the Game checkpoint | `PF_SafeSpawnControl`, `MissionMusic_Progression_PF`, `PF_SetTimeOfDay`; then `GoldMissionMarker`, `FadeToWhite`, an elevator, AI spawn |
| Safehouse | `CityAlert/CityAlertManagerModifier` (three inputs); a second trigger: `VO/Greetings/IcarusGreetings` |
| Fly Trap cutscene | a `TimelineEntityData` (Play, two Stops); nearby: `PF_Anim_Flytrap_Npc_IcarusFound`, `GoldMissionMarker`, mission music |

## Dashes

A Dash's start volume is a `PamServerAreaTriggerEntity` made when the Dash is opened (about 800 area
triggers are created then; recording `dash`, 2026-11-01). Entering it sends `Start` to the
`PamServerNamedChallengeEntity` (call stack through the area trigger's enter code, 0x14395c8a8 /
0x1433cad1b). The trigger is inside the Dash's blueprint: its `OnEnter` goes to an
`InterfaceDescriptorData` (the blueprint's interface), and the link to the challenge is in the
parent bus (the bus interface's +0x8), from the source EntityBus_deliverEvent gives it (bus +0x60,
low two bits masked, the reference that placed the blueprint), with the interface's event. Following that hop puts the start under "Dashes and challenges" (**confirmed**
in game, High Roller Avenue).

## Crash on quitting during a Dash

Seen twice (2026-11-01): quitting the game with a Dash open crashes the server thread at
0x143164d50 (reading +0x28 of a null world, `g_physicsQueryManager`) in the mission clean-up
`FUN_1439e08f0`, from `levelWorld_tickFrame`. Every frame is the game's; not yet tried without
GridNode, so whether GridNode's shutdown plays a part is open.

## Teleports (fall catchers)

The Old Friends skip runners use is an area trigger about 120 m across under the roofs (centre
(28.2, 30.2, 80.5)), RunOnce, whose `OnEnter` goes straight to a `PamTeleportEntityData` `Teleport`
(2026-11-02, the trainer's per-trigger log). Falling into it puts Faith at the teleport's
`NewPositionTransform` (TeleportEntityData +0x20; `CheckBelowGround` +0x18), in the next area. The
Volumes tab has a Teleports category and draws a line to where each one sends her (as authored: a
link may set the transform at run time).

## Key checkpoints (story checkpoints)

A trigger high above the city at (609.5, 580.5, 211.3), 24 m across, RunOnce (recording
`checkpoint`, 2026-11-02): entering it sends `Save` through a `FullEntityBus::FullEntityBusBridge`
to a `PamServerMissionCheckpointEntity`, and in the same tick 8 progression flags are set, the game
is saved (`CoreWriteSaveGameDoneMessage`), `PamTelemetryStoryCheckpointMessage` is sent and the next
mission trigger is enabled. Nothing checks earlier checkpoints: entering it overwrites the save.

The trigger is inside a sublevel: its output leaves through the blueprint's interface into the
parent bus, where a placed blueprint (a reference whose peer is a bridge) takes it on. A bridge's
onEvent (vtable 0x141aea788 +0x20, 0x14314a5d0) fires the event in the bridged bus (bridge +8) from
that bus's interface descriptor (bus +0x68 -> +0xb8 -> +0x28). The peer of a target is found as
EntityBus_deliverEvent does: the bus data's sorted targets (+0xd0, +0xf8 when bus +0x3c bit 28) give
the index into the bus's peer table (+0x40). The Volumes tab follows both ways now.
