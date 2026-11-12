# Zone streaming (why far-away placing and keeping copies loaded fail)

Findings of 2026-10-21 (EA app build), from in-game logs and the executable (Ghidra, then a
disassembly of the dump when Ghidra was closed). Confirmed in game on 2026-10-21 (3 streamers found; copies kept and a library entry placed 1.1 km
from its source).

## What the logs showed

- The city's districts are sublevels such as `Levels/SP/SP_MainCity/CentralCityBuildings_ZS`. Such a
  sublevel never streams out as a whole: a **ZoneStreamer** loads and unloads *zones* inside it
  around Faith. Dropping the sublevel's StreamOut (GridNode's pin) keeps nothing, and a StreamIn on
  it loads nothing. When Faith teleports away, the zone holding a copy's source unloads, and the
  copy with it (its owner list is the zone's).
- What the editor sees is where Faith is, not where the free camera is: placing far away worked
  only while Faith was still near the source.
- The same mesh can be in two places: the district's detailed object and a `Backdrops/...`
  sublevel's distant version. A copy re-made from the backdrop one looked wrong ("kind of loaded").

## ZoneStreamerSettings.PinVisitedZones does nothing

`ZoneStreamerSettings` (TypeInfo `0x14289fba0`; `TestZoneHeights` +0x28, `PinVisitedZones` +0x29)
is registered at `0x143c7ed30` and kept in the global `0x1425a3fa8` (getter `0x143c7fe40`). Only
three places use that global: the registration, the getter, and `0x143c82069`. The getter's two
callers (`0x143c854ed`, `0x143c866b4`) and `0x143c82069` read +0x28 only. Nothing reads +0x29:
the option is left over and has no effect in this build (confirmed in game: ticking it changed
nothing).

## Focus points (worked out 2026-10-21 in Ghidra)

A ZoneStreamerEntity's **focus manager** is at +0x280. It keeps records of 0x18 bytes at +0x08
(end +0x10, count +0x48): `{u64, focus, -1, flags}`, flags bit 0 = removed.

- `ZoneStreamer_update` (0x143c81900) calls 0x143c81850 each frame: for every record not removed,
  `focus->vtable[0](focus, record[0], out)`, where `out` (0x20 bytes, the streamer's +0x2a8) gets a
  position (float4) and a direction (float4). 0x143c82060 then finds each position's zone, and
  0x143c7f570 drops removed records.
- `ZoneStreamerFocus_add` (0x143c7e7c0) appends `{a2, focus, -1, 0}`. `ZoneStreamerFocus_remove`
  (0x143c81390) marks the matching record removed, and writes out of bounds when none matches.
- `ZoneStreamerFocusEntity` (0x58 bytes; creator 0x143c7f0c0) is the game's own focus: its
  interface is at +0x40 (vtable 0x141cfe528, slot 0 = 0x143c7f9a0 reads its `FocusPoint`), the
  streamer at +0x30 and its focus manager at +0x48; Enable (event 0x8ba423e4) adds it, Disable
  removes it.
- `ZoneStreamerSettings.PinVisitedZones` is never read (see above).

**What GridNode does** (layout editor): it keeps up to 8 focus objects of its own, in static memory
(a record may point at one until the streamer drops it), each a vtable with that one function. Every
2 s it places them at the sources of the level's copies and, for 45 s, where a library entry is being
placed from, and registers them with every live zone streamer on its own thread (the add above; to
remove, it marks its own record once found). A streamer whose grid does not hold a point finds no zone
for it. Library entries save their source's position for this.

The streamers are not made through a creator GridNode hooks, so they are found as the game's focus
entities find them (0x143c7d800): each game world (`GameWorlds`, 0x1424a9b98: [0] client,
[1] server) has an index (+0x28) into every ClassInfo's per-world entity lists, whose first node is
at ClassInfo + (index + 6) * 0x10 (0x143164d50); a ZoneStreamerEntity's node is at +0x268, and a
node's first field is the next node.

## Loading a bundle at runtime (game-tested 2026-11-12)

Worked out with the `bundle-load` claim-lab experiment (`.workflow/runs/bundle-load/`), from a mod, mid-level:

- Streaming runs on the **server thread**. Districts stream in as sublevels (`Levels/SP/SP_MainCity/Anchor`,
  `AnchorArea3`, ...) through `SubLevelManager_requestBundle` (0x1432f38f0) with parent compartment 0 (request
  +0x18). Zones stream as many small bundles (`Ground_ZS_ZS_15`, `CentralCityProps_ZS_ZS_445`) whose request carries
  their region's compartment, so they go with it.
- A mod can do the same: a copy of a sublevel request with another name, on the server thread, loaded
  `AnchorArea3` (about 9 MB) in 0.16 s with Faith elsewhere. While loading, the game holds one extra reference and
  drops it about 2 s after the bundle is ready.
- A group's state goes 0, 1, then 3 (ready); 4 was never seen. Releasing the last reference
  (`BundleHandle_release`, 0x1432f4880) unloads it: the group object is reused for another bundle a moment later.
  Releasing while it loads (state 1) also works.
- A group's +0x0c is the sublevel manager's own id, **not** a resource group: `getCompartment(+0x0c)` is null. The
  resource group id comes from `ResourceManager_registerBundleGroup` (0x142abc370), called with the bundle's name
  during the load; it returns the id (13 for `AnchorArea3`, kind 1). `getCompartment(id)` then gives the
  compartment, and `Partition_findByName` finds the bundle's assets in it (`objects/props/missionitem/magrope/
  pf_magrope_pullup`, `levels/sp/sp_maincity/anchorarea3`); they are not in compartment 0.
- The handles the game copies and drops every frame (thousands a minute, count 2 to 2) are noise: log only a
  count of 1 or 0.

## Spawning a prefab from a loaded bundle (game-tested 2026-11-12)

From `.workflow/runs/prefab-spawn/` (claims c1, c4-c11) and the claim-lab experiment: `pf_magrope_pullup` from
`AnchorArea3`, loaded by the lab with Faith in another district, spawned 3 m in front of her, could be used, and
stayed after teleporting away and back.

- Blueprint: `Partition_primaryInstance(Partition_findByName(getCompartment(resource group id), name))` (a
  `SpatialPrefabBlueprint` here).
- Server thread, as the engine's own blueprint spawner does (0x1438fce60): `ServerEntityCreationInfo_ctor(ci, 0,
  bus, transform)`; ci +0x28 = 2, ci +0x108 = `[[g_serverGameContext] +0x80]`, ci +0xe8 = an empty 0x20-byte entity
  list; `Blueprint_instantiate(result[2], ci, blueprint, null, 0)` (a null reference object works); release
  result[1] (`EntityBus_release`); `serverSpawned_initAll(list, 0)` (0x1433602d0). result[0] is 0 for a prefab: the
  entities are in the list.
- Client thread, the same with `ClientEntityCreationInfo_ctor`, ci +0x108 = `[[g_clientGameContext] +0x88]`,
  `ClientEntity_spawnBlueprint(result[3], ...)`, release result[2], `clientSpawned_initAll(list)` (0x1431c5b40).
- `Blueprint_instantiate` returns `result` in rax and its callers use it: a detour must return it.
- The bus: creation info +0x10 is the bus itself: +0x00 owner (owner +0x38 = the level), +0x08 parent bus, +0x10
  transform, +0x28 first child, +0x30 refcount, +0x38 data (`FUN_14313ffe0`). Faith's server character +0x20 is
  not a bus (0x737). Both the bus the game last used and the top of its parent chain (7 to 9 levels up) worked.
- A checkpoint reload or a level reload removes the spawned prefab with everything else. After a level reload the
  lab's bundle handle was stale: the level unload had freed the group, and releasing it read freed memory
  (refs 526426303). A bundle API must drop its handles at level end without releasing them.

Removing it (game-tested 2026-11-12): for every entity in the spawn's list (list +0x08 = first chunk; entries
from chunk +0x10; bit 0 of chunk +0 marks the last chunk, whose u32 count is at +0x08; a full chunk ends at +0x1f0
with the next chunk at +0x08), `Entity_requestDestroy(entity, bus)` on the entity's realm thread. The function
does `jmp [[[bus]] +0x10]` with (owner, entity): the bus holds its owner at +0x00, and the owner (vtable +0x10 =
0x143125180) sets entity +0x18 bit 10 and queues it. The bus is at entity +0x20 for logic entities and at +0x28
for spatial ones (static models, the distance culling entity, characters); reading the other offset crashed. All
21 entities of the magrope went, and it vanished.

A Load within about 2 s of the last Release reuses the group the game still holds (state 3 at once), and
`ResourceManager_registerBundleGroup` is not called again: keep each name's resource group id from earlier calls.

In the SDK since API 38 (`sdk/src/bundles.cpp`): `bundle_load`, `prefab_spawn`, `prefab_destroy`, `bundle_release`. It
builds its own request (`BundleRequest_init` as 0x143363c00 does: +0x2a = 0xfe, +0x24 = 0, +0x28 = 1, the heap info
at +0x2c left at its default) instead of copying the game's, finds the asset as `asset_find` does (every resource
group), and spawns into the root bus above Faith's (her bus is at +0x28). A release reaches the game 1.5 s later, after the
prefabs' entities are gone; a load of the same name before then takes the handle back. Loading again just after a
release found the group half unloaded, and a spawn from it crashed in an entity transform update (2026-11-12).
