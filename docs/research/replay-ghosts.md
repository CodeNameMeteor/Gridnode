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

## For MECMP

MECMP spawns a remote player's ghost from the level's city-ghost spawner (a one-off enable), finds
its client objects by time (anything created within 1.5 s), and has no despawn. The replay entity
does the same job on its own:

1. **Spawn without the city-ghost spawner.** A replay entity spawns its character when it gets
   StartReplay. A replay entity can be made like any entity from its data and a bus (GridNode's
   layout editor does this for copies), or an existing one can be told to start.
2. **The client follows the server.** The client character comes by replication, and the client
   replay entity gets CharacterSpawned: a definite signal instead of a time window.
3. **Despawn.** The replay entity replaces its character on a second StartReplay, and the game has
   StopReplay / ReplayDone: the replay entity's own teardown is the safe despawn to look for.
4. **Driving it.** The character has the same replay player component MECMP already writes poses
   into (PamServerReplayPlayerComponent), so streaming poses should work as it does now.

To find in Ghidra:

- The replay entity's onEvent (vtable 0x141c4ffe0 +0x20) and the functions 0x14396d798,
  0x14396e965, 0x1433908a1, 0x143390d94: what StartReplay does, where the character data and blueprint
  come from (the ghost's `PamCharacterEntityData` is not in `PamReplayEntityData`'s fields), and
  what happens to the previous character.
- The event ids for stopping and what the client replay does on CharacterSpawned. Candidates to
  look for in the onEvent switch (djb2-xor): StopReplay `0xd6a2182e`, PauseReplay `0x9e54be24`,
  ResumeReplay `0xffd01cef`, RestartReplay `0xe8c6a041`, ReplayDone `0x66afe476`. The Recorder's
  **Send an event** can try them on a live `PamServerReplayEntity` during the mission.
- Whether a replay entity can run without a `PamReplayResource` (MECMP supplies the poses itself).
