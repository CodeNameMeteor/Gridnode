# Destroying an entity

Findings from making the layout editor's **Remove this copy** destroy the copy instead of parking it
15 km below the level (2026-10-21, EA app build). Confirmed in game: copies destroyed, then the
area unloaded and the level ended without a crash.

## The sequence

An entity GridNode made (a copy of a static model, server and client halves) is taken down on the
game thread of its realm, in this order:

| Step | What | Why |
|---|---|---|
| 1 | Unlink it from its owner list: node at entity +0x08 `{prev node, next node}` | The part of the level it was made in (an EntityOwner) destroys every entity in that list when it unloads. The destructor does not unlink it. |
| 2 | vtable +0x88, deinit (`SpatialEntity_deinit` 0x1432e3b80) with `{int realm, u8 0}` | Takes its components out of the world, the client half out of the client context's list, unregisters it from its bus, clears the initialised flag (0x8). |
| 3 | vtable +0x30, destroy (`SpatialEntity_destroy` 0x1432e3c10) | Sets the destroyed flag (0x2000000) and destroys its components (each: +0x30, then +0x38 with 1). |
| 4 | vtable +0x38 with 1: destructor and free | The base destructor (`Entity_dtor` 0x143117610) releases the entity's reference to its bus. |

GridNode checks that the two slots hold those functions (and fingerprints both), that both neighbours
of the owner link point back at it, and that it is not already being destroyed.

## What went wrong without each step

- **Without the deinit**: the copy's physics part stayed in the spatial grid; the next query
  (0x1436d1dd0) called a virtual on the freed object. A crash GridNode's handler caught.
- **Without the unlink**: the game closed silently (no crash line, no "level ended") when the area
  unloaded or the game quit, as the owner destroyed the freed copy again.

## EntityBus_release is not the way

`EntityBus_release` (0x1431547b0) drops a reference to a bus and frees it at zero. It does not
destroy the bus's entities: each entity holds a reference to its bus and releases it in its own
destructor, so a bus is freed once its entities are gone. Releasing the bus of a live entity frees
it under that entity (and a copy shares its bus with the part of the level it comes from).

## Flags at +0x18 seen on a copy

| Before | After deinit |
|---|---|
| server 0x0888101f, client 0x08aa101e | server 0x00081003, client 0x000a1002 |

Cleared: 0x4 (on its bus), 0x8 (initialised), 0x10, 0x800000 (in the world), 0x08000000.
