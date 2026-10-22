# City alert (KSEC alerts): research for syncing them between players

Research 2026-10-22, from a GridNode recording of one full alert (camera → response → KSEC →
helicopter → safehouse) and the executable in Ghidra. Mirror's Edge Catalyst PC (EA app build), image
base 0x140000000. Addresses are image VAs; "+0x.." are offsets into the object named.
Confidence: **confirmed** (seen in the recording and in the code), **read** (read from the code, not
tested), **guess** (inferred, to check).

## Summary

One server entity, **`PamServerCityAlertManager`**, owns the whole alert. Its state (active, level
0–4, timers, counters) changes in two places: its **event handler** (0x143958810) and its **update**
(0x14395e400). Cameras, AI and the safehouse talk to it with events; it talks back through output
properties and output events, and it reports each state change with telemetry messages.

For syncing:

- **Detect** on the player where it happens: the telemetry messages (Activated, LevelChanged,
  Deactivated with a reason), or the manager's own state (+0x175 active, +0x134 level).
- **Apply** on the others: drive the same manager, either with its runtime inputs
  (`ForceActivateCityAlert` / `ForceDeactivateCityAlert`, read in its update every tick) or with
  its own events (`0x6bb5e497` / `0x6bb5e496` / `0x6bb5e491` raise the level to at least 2 / 3 / 4;
  `0x798161be` ends it).
- Mind: the manager is **re-created** when an alert ends (its blueprint streams out and in), the
  alert is centred on **one** player (the server's first player), and hotspots (KSEC response
  zones) are picked near that player.

## Types

| Type | Realm | Size | TypeInfo | Vtable | Creator | Data |
|---|---|---|---|---|---|---|
| `PamServerCityAlertManager` | server | 0x180 | 0x142857720 | 0x141c4b5f8 | 0x1425794a0 | `PamCityAlertManagerData` (TypeInfo 0x14287d740) |
| `PamCityAlertTelemetryEventEntity` | server | 0x98 | 0x142856260 | 0x141c456d8 | 0x142577f68 | `PamCityAlertTelemetryEventEntityData` (0x14287dce0) |
| `PamServerSafeSpawnTrackerEntity` | server | 0x1eb0 | 0x1428592c0 | 0x141c509c0 | 0x14257a700 | `PamSafeSpawnTrackerEntityData` (0x1428813d0) |
| `PamServerSafeSpawnLockerEntity` | server | 0x38 | 0x142859360 | 0x141c50908 | 0x14257a6d0 | `PamSafeSpawnLockerEntityData` (0x142881370): `BlockReturnTeleport` +0x18 |
| `PamServerHotspotEntity` (KSEC response zones) | server | 0x140 | 0x142857400 | 0x141c4b6b0 | 0x142579590 | 0x14287d6e0 |
| `PamServerAICoordinator` | server | 0x618 | 0x142854d20 | 0x141c424f0 | 0x142577b60 | 0x14286c670 |

The manager is found like any entity of a class: the server world's per-class list (ClassInfo
lists; the game does it with 0x143164d50(TypeInfo, g_physicsQueryManager), as for the hotspots).
GridNode's `docs/research/zone-streaming.md` describes the lists.

## `PamCityAlertManagerData` (0x80 bytes): tuning, the same for every player

| Offset | Field | Type | Used for |
|---|---|---|---|
| +0x18 | Realm | enum | |
| +0x1c | NoticedByHumanAITimeThreshold | f32 | level ≥ 2 after this long noticed (update) |
| +0x20 | NoticedByHelicopterTimeThreshold | f32 | |
| +0x24 | NoticedByDroneTimeThreshold | f32 | |
| +0x28 | NoticedByCameraTimeThreshold | f32 | camera notice → alert (event `0x74615ef5`) |
| +0x2c | NoticedByHumanAICooldown | f32 | update: notice expiry |
| +0x30 | NoticedByHelicopterCooldown | f32 | |
| +0x34 | NoticedByDroneCooldown | f32 | |
| +0x38 | NoticedByCameraCooldown | f32 | |
| +0x3c | NoticedBySecHubCooldown | f32 | |
| +0x40 | DefaultCooldown | f32 | |
| +0x44 | NoticedBySecHubTime | f32 | |
| +0x48 | MinimumCityAlertTime | f32 | the alert lasts at least this long |
| +0x4c | ActivationTimeUntilLevel4 | f32 | level 3 → 4 after this long |
| +0x50 | TimeNoticedByCameraInLevel1UntilDroneIsSpawned | f32 | |
| +0x54 | SpawnDroneWhenInactiveAndSensedByAITime | f32 | |
| +0x58 | MaximumCityAlertLevel | i32 | |
| +0x5c | AIsToKillToTriggerLevel4 | i32 | kills ≥ this → level 4 |
| +0x60 | HotSpotsToActivateToGoToLevel3 | i32 | hotspots ≥ this at level 2 → level 3 |
| +0x64 | HelicopterAllowedToSpawnAtLevel | i32 | level ≥ this → output event `0xc3c0bae9` (helicopter) |
| +0x68 | DroneAllowedToSpawnAtLevel | i32 | level ≥ this → output event `0xcef736cc` (drone) |
| +0x6c | TimeAllowedBetweenCameraDestruction | f32 | two cameras destroyed within this → `0xd1a8e78d` |
| +0x70 | CityAlertRadius | f32 | |
| +0x74..+0x7b | NoticedByHumanAI, NoticedByHelicopterAI, NoticedByDroneAI, ForceActivateCityAlert, ForceDeactivateCityAlert, PauseCityAlert, HelicopterAvailable, DroneAvailable | bool | defaults of the runtime inputs below |

## The manager at runtime (0x180 bytes) — read from the code

Inputs (property values, `TypedValue`s read with `TypedValue_getValuePtr`; each 0x10 bytes). The
mapping to names is **guess** from how the update uses them, to confirm:

| Offset | Probably | Type | How the update uses it |
|---|---|---|---|
| +0x78 | MaximumCityAlertLevel | i32 | the level cap; 0 forces the alert off |
| +0x88 | CityAlertRadius | f32 | the player inside this radius of the alert centre keeps it going |
| +0x98 / +0xa8 / +0xb8 | NoticedByHumanAI / Helicopter / Drone | bool | |
| +0xc8 | **ForceActivateCityAlert** | bool | true → active this tick |
| +0xd8 | **ForceDeactivateCityAlert** | bool | true → deactivated (and kills are not counted) |
| +0xe8 | DroneAvailable | bool | needed for the drone event |
| +0xf8 | HelicopterAvailable | bool | needed for the helicopter event |
| +0x108 | **PauseCityAlert** | bool | freezes everything (notices are snapshotted) |

Outputs (property writers, set with 0x142a430f0(writer, value*, 1)):

| Offset | Value | Type |
|---|---|---|
| +0x40 | active (from +0x175) | bool |
| +0x48 | "nothing notices the player any more" | bool |
| +0x50 | a camera sees the player | bool |
| +0x58 | time left (FLT_MAX when off) | f32 |
| +0x60 | level (from +0x134) | i32 |
| +0x68 | hotspots activated (from +0x140) | i32 |
| +0x70 | alert centre (from +0x120): the player's position when level 2 is reached | vec3 |

State:

| Offset | What |
|---|---|
| +0x120 | alert centre (vec4) |
| +0x130 | cameras currently seeing the player (count) |
| +0x134 | **level** 0..4 |
| +0x138 | hotspot counter |
| +0x13c | **AI killed** during this alert |
| +0x140 | hotspots activated |
| +0x144 / +0x148 / +0x14c | noticed by human AI / drone / helicopter: time (−1 none) |
| +0x150 | camera notice time |
| +0x154 | sec-hub notice time |
| +0x158 | activation time |
| +0x15c, +0x168, +0x16c | timers (level 2, level 3, drone) |
| +0x160 | last camera destroyed time |
| +0x164 | camera seeing since |
| +0x170..+0x173 | noticed by human / heli / drone / sec hub (flags) |
| +0x174 | within MinimumCityAlertTime of activation |
| +0x175 | **active** |
| +0x176..+0x17b | pause snapshot and misc |

Times are game time from 0x1432f5540(*(g_serverGameContext + 0x28)).

## Functions

| VA | What |
|---|---|
| 0x143958810 | **onEvent** (vtable +0x20): the input events below |
| 0x14395e400 | **update**: decides active / inactive and the level each tick; calls 0x14395f330 |
| 0x14395d9c0 | **setLevel**(manager, level): clamps to the max level, records the alert centre when going from < 2 to ≥ 2 (output +0x70), writes +0x134 and output +0x60 |
| 0x14395e210 | **deactivate**(manager): level 0, timers −1, +0x175 = 0, outputs reset |
| 0x14395f330 | **hotspots**(manager, …, wasActive): activates the hotspot nearest the player (KSEC response), counts them (+0x140); deactivates all when the alert ends |
| 0x143956830 | camera destroyed twice quickly: nearest hotspot to 100 |
| 0x14395a520 | time left |
| 0x14395b230 / 0x14395b2e0 | "elapsed since t ≥ / < threshold" |
| 0x143965730 | **the player** the alert follows: g_serverGameContext +0x68 (player manager) → 0x1433503c0 → +0x1478 (its character). One player only |
| 0x14314e8e0 | fires an output event (entity, bus, data, hash) |
| 0x142a430f0 | writes an output property |

## Events

Input events (to the manager; onEvent 0x143958810), each an **event id** (a djb2-xor hash of the
name). Names unknown except where shown:

| Id | When (recording) | What it does (read) |
|---|---|---|
| `0xb81ad36f` | a camera starts seeing the player (4.91 s, 36.45 s) | cameras seeing += 1; on the first, "camera seeing" output on and its time noted |
| `0x74615ef5` | that camera stops seeing (14.58 s, 36.55 s) | cameras seeing −= 1; at 0 the output goes off; the notice time decides activation (NoticedByCameraTimeThreshold) |
| `0x1270058a` | sec hub notices | sec-hub flag and time (ignored when paused) |
| `0x1ceed46b` | each AI killed (18.08, 27.48, 30.28, 33.22) | kills += 1 (not when forced off or paused) |
| `0x6bb5e497` | with each kill | **level at least 2** (only when active) |
| `0x6bb5e496` | | **level at least 3** |
| `0x6bb5e491` | | **level at least 4** |
| `0x664887be` | | **level + 1** (only when active) |
| `0x60ec67df` | a camera destroyed | two within TimeAllowedBetweenCameraDestruction → nearest hotspot, output `0xd1a8e78d` |
| `0x798161be` | the safehouse (44.08) | **deactivate** (0x14395e210) |

Output events (fired by the manager): `0xc3c0bae9` helicopter (seen at 18.08 with the helicopter
spawn), `0xcef736cc` drone, `0xd1a8e78d` cameras destroyed.

Around the manager in the recording: `PamServerSafeSpawnTrackerEntity` gets **Lock** at the start
(4.95) and **Unlock** at the end (44.08); `PamCityAlertTelemetryEventEntity` gets
**`PlayerTriggeredSafeHouse`** (`0x18c33d9a`) at the safehouse, then `PamServerAICoordinator`
`0x0f594832` and `ServerBlueprintSpawnEntity` **Kill** (KSEC and the helicopter despawn).

## Telemetry messages (category PamTelemetry; realm 255)

| Message | TypeInfo | Size | Payload (from the recording) |
|---|---|---|---|
| `PamTelemetryCityAlertActivatedMessage` | 0x1428774a0 | 0x50 | |
| `PamTelemetryCityAlertLevelChangedMessage` | 0x142877440 | 0x50 | +0x48 level (u32: 1, then 2); +0x4c 0 then 70 (guess: score) |
| `PamTelemetryCityAlertDeactivatedMessage` | 0x142877460 | 0x60 | +0x48 level (2); +0x4c 39 (the alert's seconds); +0x50 35, +0x54 50 (unknown); +0x58 **reason** |
| `PamTelemetryAIKilledMessage` | 0x142877500 | 0x70 | one per kill |

All carry a `ClientConnection*` at +0x38 (the player). Reason (`PamTelemetryCityAlertDeactivateReason`,
TypeInfo 0x142877480): 0 EnemiesOutrun, 1 EnteredSafeHouse, 2 Died. They are sent from the
telemetry path (stack 0x14347c6d6 → 0x143479a1d → 0x14345e8fa).

## Timeline of the recording (seconds)

| t | |
|---|---|
| 2.34 | marker 1 (camera) |
| 4.91 | `0xb81ad36f` → manager; safe spawn Lock; Activated; LevelChanged 1 |
| 11.98 | KSEC AI created (PamAIStateInvestigate, fire managers) |
| 14.58 | `0x74615ef5` → manager |
| 18.08 | first kill: `0x1ceed46b`, `0x6bb5e497`; LevelChanged 2; helicopter (`0xc3c0bae9`, ServerVehicleSpawnEntity Spawn, PamAIStateHeliPatrol) |
| 27.48, 30.28, 33.22 | kills |
| 36.45 / 36.55 | camera sees again / stops |
| 44.08 | safehouse: `PlayerTriggeredSafeHouse`, `0x798161be`, Unlock, KSEC and helicopter killed |
| 44.15–44.22 | the manager's blueprint streams out and in: a **new** `PamServerCityAlertManager` |
| 44.66 | Deactivated, reason 1 |

## Syncing: what it takes

1. **One owner of the alert.** Decide who decides (a host, or "whoever was noticed"), and send
   state, not raw events: active, level, the alert centre, and the reason it ended.
2. **Apply with the manager's own inputs.** To start: set the ForceActivate input (+0xc8) or post
   `0x6bb5e497` (level ≥ 2) once it is active; to raise: `0x6bb5e496` / `0x6bb5e491` /
   `0x664887be`; to end: `0x798161be` (or ForceDeactivate, +0xd8, for a tick). Posting events goes
   through EntityBus_postEvent (GridNode's `entity.yaml`); a property input is a TypedValue, so
   writing it needs the property-set path, not a plain write (to confirm).
3. **Look the manager up each time** (the server world's PamServerCityAlertManager list): it is
   re-created after each alert.
4. **One player.** The alert, its centre (+0x120) and its hotspots follow the server's first
   player (0x143965730). For KSEC to respond to the right runner, the host's game needs to know
   about the others (or each player runs their own and only start/level/end are synced).
5. **No echoes.** A remote start makes the local game send its own Activated: mark alerts applied
   from the network and do not send them back.
6. **Ends.** Sync the end with its reason (safehouse / outrun / died). The safehouse also sends
   `PlayerTriggeredSafeHouse` to the telemetry entity and unlocks the safe-spawn tracker; ending
   through the manager (`0x798161be`) leaves those to the game.
7. **Tuning.** `PamCityAlertManagerData` comes from the game data, so it is the same for everyone
   unless a mod changes it.

## Blueprints

The recording does not name blueprints. What it shows: the manager lives in a server blueprint
that streams out and back in when the alert ends (44.15 s: `ServerBlueprintEntity` StreamOut and
StreamIn, then a new manager and its `PamCountdownEntity` and `WidgetReferenceEntity`), and KSEC
and the helicopter come from `ServerBlueprintSpawnEntity` / `ServerVehicleSpawnEntity`, killed at
the end. To find the blueprints in Frosty: search the city's EBX for `PamCityAlertManagerData`,
`PamServerHotspotEntityData` and `PamSafeSpawnTrackerEntityData`; the blueprints holding them, and
the connections into the manager's events and properties, show who sends each event.

## To find next (Ghidra)

- The names of the event ids (hash the strings of the EBX dump with djb2-xor; GridNode's
  `tools/events/build_names.py` does this).
- Who sends `0xb81ad36f` / `0x74615ef5` (stack 0x143172ca5 / 0x143172c48 via 0x142a4298b: an
  event fired from a property change) and `0x798161be` (0x1433cad1b).
- Confirm the input TypedValue names (+0x78..+0x108) by finding where the manager binds its
  properties (its init, vtable +0x78).
- Where the Activated / Deactivated telemetry is sent from the manager.
