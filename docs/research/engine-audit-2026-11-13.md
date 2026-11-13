# Engine audit of 2026-11-13: what was checked

Two Gemini reports on GridNode and the game, checked before anything entered `db/annotations/` or the
SDK. The first (`gridnode_engine_audit_and_opportunities.md`) was checked by four agents in Ghidra; their
tables are on the user's PC (`scratch\audit-2026-11-13\*-verified.md`). This page covers the second,
"offset-free hooking and reflection", checked by hand in Ghidra on 2026-11-13. "Confirmed" here means read
in Ghidra; nothing below is game-tested unless it says so.

## Taken into GridNode (API 39)

| Claim | Verdict | What GridNode does with it |
|---|---|---|
| `0x142105510` ("JumpTuningRoot") is the data registry's object table | confirmed | A sorted vector of `{u64 key, DataContainer*}`, key = `slot << 40 \| classId << 16 \| index` (DataRegistry_register's search and insert). Now `DataRegistry_Entries`. The trainer's jump tuning finds Faith's `PamCharacterBlueprint` there by its vtable instead of entry 164 (`+0xa48`). |
| Data registry listeners: `DataRegistry_addListener` 0x143134420, remove 0x143138c60 | confirmed | `{ctx, fn}` pairs in an rb-tree at registry + 0x88; `fn(ctx, &objects, slot, loaded)` called under the lock 0x1423f7390 after a load and before an unload drops the objects (SubLevelManager's listener 0x1432ff6a0 has that shape). `on_data` uses it; `data_of_class` reads the table. |
| `Speed_DriveGlobal` (0x1423dd6f8) is an audio object | confirmed | Written only by the audio init 0x142fe1fd0; all 32 readers are audio code. Note in `faith.yaml`: why the old speed path matched only 77% of the time. |

## Checked, not taken yet

| Claim | Verdict | Note |
|---|---|---|
| `ServerSubLevelManager` points at the pointer slot 0x1424a9f60 (a bug in bundles.cpp) | wrong for `main` | `world.yaml` has had `ServerSubLevelManager = 0x14253d680` (the object, game-tested 2026-11-12) since the bundle work; the report read an older checkout. |
| Updater registration into the 9 update passes: `0x143172680(passSlot, updater)`, key `djb2(name at +0x18) << 32 \| priority (+0x24)`, deferred vector at pass + 0x58 under the pass lock | confirmed (registration) | A way to tick without vtable swaps. Not used: GridNode's ticks work; the updater's vtable layout (+0x08 pre, +0x10 post) and unregistering are not read yet. |
| Deferred body destroy sets bit 0x10 in `owner + 0xb88[id]` (0x1434f53e0) and queues the id at +0x1c8 | confirmed | Not used for `physicsQueriesSafe`: the crash it guards (2026-11-06) was in a query reading an entity's freed physics component, which a hit filter cannot prevent. |
| `PhysicsWorld_create` called only from `PhysicsManager` vf6; `PhysicsWorld_flush` from three places in the physics manager | confirmed (callers) | Replacing the two detours with a per-instance `PhysicsManager` vtable copy is possible; not done (both detours are game-tested). |
| Entity creators in per-realm lists at 0x1423fc1f0 | plausible (written by `EntityCreator_ctor`, `EntityCreators_bindRealm`) | Node layout not read. See also the single creation function (0x143167120) in the first report. |
| Freeze-world / override camera flag 0x142401cb8 | plausible (read in `ClientWorld_tickFrame`, written in two client functions) | Pos / rotation / fov layout after it not read. |
| Entity property read / write by hash (`0x1431462a0`) | confirmed (first report) | Needs an entity's own bus peer and data pointer: the bus is at +0x20 or +0x28 by entity kind and the data only through the bus's key table. Left until that is read. |

## Not checked

The rest of the report: the TypeInfo hash maps, `SettingsManager_getByType`, the camera callback list at
`ClientCameraViewManager + 0x1b90`, EntityBus cross-thread queue, the WeakPtr trees, the DX11 swap chain
path, ANT schema layout, Havok's singleton list, the virtual file system, synthetic input, the
SubLevelManager state machine, the VisualEnvironment state stack, Lua. Hypotheses until read in Ghidra.
