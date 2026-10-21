# Recording: fast travel to a safehouse (2026-10-20)

First recording made with the Recorder (`recordings/20261020-200704-tptosafehouse.jsonl`, not in
the repository): 7.2 s, 5776 events, Levels/SP/SP_MainCity. The map was opened, a safehouse picked,
and Faith arrived there.

## What happened, in order

| t (s) | What | Notes |
|---|---|---|
| 1.21 | `PamSocialPlayOnShowMapMessage`, `PamplonaGamePausedMessage` | the map opens and pauses the game |
| 2.88 | `PamSocialPlayOnHideMapMessage`, `PamplonaGamePausedMessage` | fast travel confirmed, the map closes |
| 2.89 | server: `ServerInputDeactivateInputRestrictionMessage`, `PamProgressionFlagUpdatedMessage` | from the event graph (stack through `EntityBus_fireEvent`) |
| 2.89 | client: profile options saved, `CoreWriteSaveGameDoneMessage` | a save |
| 3.42 | server: `ServerStaticModelSpawnMessage` | a blueprint spawned by `entityLoader_resolveBlueprintThenSpawn`, in reply to the event |
| 3.43 | client: `PamTelemetryTeleportMessage` | **from (-143.0, 29.9, 823.0) to (-423.0, 10.1, 799.0)** |
| 3.49–4.4 | `SubLevel*`, `ZoneStreamer*`, `BundleManagerBundleUnloadMessage` | the old area streams out, the safehouse area in |
| 3.63 | server: `ServerControllableSpawnDoneMessage` (a new `PamServerCharacterEntity`) | Faith is **respawned** at the safehouse by `ServerCharacterSpawnEntity_update` |

So fast travel is not a transform change: the server event graph is triggered, the level streams,
and Faith's character is spawned again at the destination's character spawn.

## How the event graph was triggered

The server stacks of 2.89 s all run through:

- `ServerEventSync_onReachedClient` (0x143364ba0). It handles `EventSyncReachedClientMessage`
  (type id 0x458f8b20 = djb2 of the name), which the client sends when it reaches an event sync
  point (here: confirming fast travel). It finds the sync entity from the message's ids and fires
  its **"Out"** event (0xb87df4b = djb2("Out")).
- `EntityBus_fireEvent` (0x14314e4a0): walks the bus's event connections for that source and event
  id and delivers each one. `EntityBus_deliverEvent` (0x14314eac0) → `EntityBusPeer_sendEvent`
  (0x143134460) → the target's vtable +0x20 (its onEvent).

Event ids, like message type ids, are djb2-xor hashes of names. The reflected type and field names
already resolve some (e.g. "Out").

## Message layouts learned

- `PamTelemetryTeleportMessage` (client, 0x70 bytes): +0x38 ClientConnection*, +0x50 from (Vec3),
  +0x60 to (Vec3).
- `ServerStaticModelSpawnMessage` (0xa0): +0x30 the static model entity, +0x60.. a LinearTransform.
- `ServerControllableSpawnDoneMessage`: +0x30 the character entity.
- `ClientSpawnSpawnedOrUnSpawnedMessage`: +0x30 the ClientCharacterSpawnEntity, +0x38 1 = spawned.

## The event chain (second recording, with entity events)

`recordings/20261020-201905-safehousetp.jsonl`: 43,455 events, 14,570 of them events between
entities (onEvent wrapped in 1,354 entity vtables). On the server thread, after the map closes:

| t (s) | Event → receiver | |
|---|---|---|
| 0.448 | `EventSyncReachedClientMessage` | the client confirmed fast travel |
| 0.448 | **Teleport** → FullEntityBusBridge (a blueprint's interface) | |
| 0.448 | b728297b → ServerCharacterProxyEntity, PamServerHumanHealthComponent, PamServerCharacterEntity | Faith prepared (name unknown) |
| 0.448 | **Activate** → ServerInputRestrictionEntity | input locked (`ServerInputDeactivateInputRestrictionMessage`) |
| 0.448 | 74f05cea → PamServerStartPointEntity | the destination's start point |
| 0.479 | Deactivate / Activate → PamUIInputControlEntity, **Unpause** → ZoneStreamerControlEntity ×3 | streaming resumes |
| 0.982 | `EventSyncReachedClientMessage` (the next sync point) | |
| 0.982 | **Teleport** → **PamServerTeleportEntity** | the move itself |
| 0.989 | `PamTelemetryTeleportMessage` | from / to |
| 0.982 | d27bc17e → ServerBlueprintEntity, Impulse → EventSplitterEntity, Reset → PamServerFlowEntity | the rest of the graph |

`PamServerTeleportEntity_onEvent` (0x1433845f0) handles "Teleport" (0x94ba5a24): it teleports the
player (`ServerTeleportEntity_teleportPlayer`) to its `TeleportEntityData.NewPositionTransform`
(+0x20; `CheckBelowGround` +0x18). So the game teleports with an entity that holds the
destination, triggered by an event; GridNode could do the same by sending "Teleport" to such an
entity, or by spawning one with its own transform.

Unnamed event ids (b728297b, 74f05cea, 27d4023a, d27bc17e, ...) are names not in the reflection
data; a list of event names from the game's data (Frosty) would name them
(`recordings/event-names.txt` is read by the Recorder).

## Teleporting without the reload (2026-10-21)

Teleporting Faith while switching the view back to her (the layout editor's first teleport turned
the free camera off, then teleported) reloaded the area around her: copies placed with the editor
were destroyed with it and placed again later. Teleporting her while the free camera stays on, and
turning it off afterwards, did not (confirmed in game: a copy placed before a ~340 m teleport was
still there after the free camera was turned off). So the reload comes from the view and the
teleport changing together, not from Faith moving. Still to check: a copy far from where she
lands. The editor's T now teleports with the camera kept; Shift + T is the old way.
