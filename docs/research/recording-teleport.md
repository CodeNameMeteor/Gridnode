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

## What the Recorder should add next

Events between entities: recording each delivery (target type, event id and its name, source) would
show the whole graph from "Out" to the spawn. That needs a hook on entity onEvent (vtable +0x20, a
pointer swap per entity class) or on the bus functions above.
