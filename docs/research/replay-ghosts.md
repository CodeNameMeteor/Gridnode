# Replay ghosts (how a mission spawns its own ghost)

Research 2026-10-22, from a GridNode recording of the start of the mission "Benefactor"
(20261022-144709-benefactor.jsonl), which shows a ghost runner. For MECMP's remote-player
ghosts. Ghidra was not open at the end: the function addresses below come from the recorded call
stacks and still need opening.

## What happens

| t (s) | Server | Client |
|---|---|---|
| 3.70 | the mission's sublevel streams in: a `PamServerReplayEntity` (data `PamReplayEntityData`) at (594.9, 70.4, 217.7), and a `PamReplayAssetContainer` | 3.73: the `PamClientReplayEntity` (same data) and its asset container |
| 12.16 | event **StartReplay** (`0x2734a476`) to the server replay entity; at once a **`PamServerCharacterEntity`** is created at the replay entity's position, from its own `PamCharacterEntityData` (not the mission soldiers') | 12.17: a `PamClientCharacterEntity` arrives by network replication; the client replay entity gets **CharacterSpawned** (`0xa29c98e6`) |
| 22.19 | StartReplay again: a new server character, **at the same address** as the first | 22.20: a new client character and CharacterSpawned again |

So the replay entity spawns its own ghost character on StartReplay, without any spawner
entity, and the client side follows by the normal network replication (no matching by time). The
second StartReplay giving the same address suggests the replay entity takes its previous character
down before spawning the next: a despawn path the game uses itself (to confirm in Ghidra).

## Call paths (image VAs, innermost first)

- Server character: 0x143167367 (generic creation) ← 0x143361456 ← 0x14335e494 ← 0x14335e3f1 ←
  **0x143390d94** ← **0x1433908a1** (a character spawn routine) ← **0x14396e965** ←
  **0x14396d798** (the replay entity's event handling) ← 0x14314e759 (EntityBus_fireEvent) ←
  0x14314a619 ← 0x14314e759 ← 0x1433cad1b ← 0x1439595d3 ← 0x14395c8a8 (who sent StartReplay).
- Client character: 0x143167367 ← 0x1431c6482 ← 0x1431c4994 ← 0x1431bf98a ← 0x14346912d ←
  0x143234f21 ← 0x14347d65d (network replication) ...; CharacterSpawned to the client replay:
  ... ← 0x143234f51 ← 0x14347d65d.

## Types (reflected)

| Type | Size | TypeInfo | Vtable | Creator |
|---|---|---|---|---|
| PamServerReplayEntity | 0x80 | 0x142859680 | 0x141c4ffe0 | 0x14257a5a8 (server) |
| PamClientReplayEntity | 0x58 | 0x142856990 | 0x141c48960 | 0x142578890 (client) |
| PamReplayAssetContainer | 0x38 | 0x142856850 | 0x141c48f08 | 0x142578980 / 0x142578950 |
| PamServerReplayPlayerComponent | 0x1c8 | 0x142858f00 | 0x141c500a8 | (a component of the character) |
| PamClientReplayPlayerComponent | 0x310 | 0x142856800 | 0x141c48a28 | |

Data:

- `PamReplayEntityData` (0x70, TypeInfo 0x14287c600): `StartTrim` +0x18, `OverrideTransform`
  +0x20, `EndTrim` +0x60, `MapHack` +0x64.
- `PamReplayAssetContainerData` (TypeInfo 0x14287c660): `ReplayAsset` +0x18 → `PamReplayAsset`
  (`PamReplayResource` +0x18): the recorded run.
- `PamReplayPlayerComponentData` (TypeInfo 0x14286b890): `RecordingBones` +0x70,
  `TranslationBones` +0x78, `SkeletonAsset` +0x80, `TimeScale` +0x88.
- Messages: `PamReplayStopReplayMessage`, `PamReplayReplayDoneMessage`,
  `PamReplayStartPreviewMessage` / `StopPreviewMessage`, `PamReplayRecordingStarted/Stopped`.

## How the replay entity spawns and despawns (Ghidra, 2026-10-22)

`PamServerReplayEntity`'s onEvent (vtable +0x20, **0x14396d750**) handles two events:

- **StartReplay** (`0x2734a476`) → **0x14396e8b0**: finds the entity linked to the replay entity in
  its blueprint that was made from a **`CharacterSpawnReferenceObjectData`** (a lookup with its bus
  +0x20, its data +0x28 and the link id `0xa695a747`: 0x143115190, then 0x14311c850). That is a
  character spawner (`ServerCharacterSpawnEntity` / `PamServerCharacterSpawnEntity`). If the spawner
  has no last-spawned character (`ServerSpawnEntity_getLastSpawnedCharacter`), it calls the
  spawner's **vtable +0xd0** (spawn) with a spawn info: transform (unset), player 0, existing 0,
  0, −1, busId −1, +0x60 = 1, **useTransform 0** (the ghost appears at the spawner's own place).
  This is the same call and the same struct as MECMP's `SpawnInfo`.
- **StopReplay** (`0xd6a2182e`): under the entity's lock (+0x50) sets a flag (+0x4c), registers the
  entity with its class's updater (0x14396ccd0; updater vtable 0x141c4ffb0, created on first use as
  "PamServerReplayEntity"), and sends the player a message (probably `PamReplayStopReplayMessage`,
  with the 16 bytes at +0x3c) through 0x1433b6f90 / 0x1433d56a0. On the next update
  (**0x1439724d0**) the flag leads to **0x1439741d0**: the same spawner lookup, its last spawned
  character, and the spawner's **vtable +0x108 (spawner, character, 1, 0)**: the unspawn.

The unspawn, **0x14338e4c0** (vtable +0x108 of both `ServerCharacterSpawnEntity` 0x141b4e0c8 and
`PamServerCharacterSpawnEntity` 0x141c4b388):

1. if the character's player (+0x250) has its +0x1398 flag set (a player made for the character),
   releases it (0x143351330) and removes it from the server player manager (0x14334c2d0);
2. **0x14338e5d0(spawner, character, 1, 0)**: out of the spawner's spawned list (+0x148/+0x150)
   and destroyed;
3. when the list is empty, unregisters the spawner's message listener (`0x1286cf79`).

The replay entity's init is vtable +0x78 (0x143971380) and its deinit +0x88 (0x143971120: leaves
the updater, then the base deinit 0x143168770).

## For MECMP

MECMP already spawns ghosts the same way the mission does (spawner vtable +0xd0 with the same
`SpawnInfo`); the differences are which spawner and what happens after:

1. **Despawn: the spawner's vtable +0x108 (spawner, character, 1, 0)**, on the server thread. It is
   what the game itself does to take a replay ghost away, and it also tidies the spawner's list and
   the character's player. MECMP's "ghosts are never destroyed" can use it.
2. **A spawner of its own.** The mission's ghost comes from a character spawner linked to its replay
   entity, not the city-ghost spawner, so the city's "Your Echo" character is left alone and the
   list does not need reordering. A spawner can be made like any entity from a
   `CharacterSpawnReferenceObjectData` and a bus (GridNode does this for prop copies).
3. **The client side.** The client character arrives by replication, and the client replay entity
   is told `CharacterSpawned` (`0xa29c98e6`): a definite signal, where MECMP matches by time.
4. **Placement.** The mission passes useTransform 0 (the spawner's own place); MECMP's
   useTransform 1 with its own transform is the same call with the other choice.
5. **Driving it.** The character has the replay player component MECMP already writes poses into.

## The map's echo (recording 2026-10-23, "open map")

Opening the map plays an echo too, the same way: 0.39 s in, **StartReplay** (`0x2734a476`) reaches a
`PamServerReplayEntity` through a network message from the client (stack 0x14314e759 ← 0x143364d03
← 0x142a395b9 message dispatch), and the client replay entity gets **CharacterSpawned**
(`0xa29c98e6`, from 0x1431c7faa) at once. No character entity was created in the recording: the
replay entity found its spawner's last character and reused it (it only spawns when there is
none). In MECMP's log for the same action, its ghost's client component stopped accepting poses at
that moment ("rejects the pose", the bone-count flag +0x2e4 cleared), which fits the map's replay
reaching a character spawner MECMP's ghosts came from (the city-ghost spawner). MECMP's own spawner
keeps its ghosts out of it; to confirm with that build. Still open: which spawner this replay entity
is linked to, and what the client replay entity does on CharacterSpawned (0x1431c7faa).

Still to find:

- 0x14338e5d0 in full (what destroying the character involves), and whether +0x108 must run on the
  server thread (the replay entity calls it from its updater, which runs there).
- What the client replay entity does on CharacterSpawned. (The server replay entity handles only
  StartReplay and StopReplay.) In game: the Recorder's **Send an event** can send StopReplay
  (`0xd6a2182e`) to a live `PamServerReplayEntity` during the mission to watch the unspawn.
- Whether a replay entity can run without a `PamReplayResource` (MECMP supplies the poses itself).
