# Response to the code review of 2026-10-21

The review (PR #1, `reviews/2026-10-21-code-review.md`) was made on `main` at `56a8959`. The fixes
below are on top of the 39 later commits (the layout editor's outline, handles, multiple selection
and library, the Recorder's events and animation flags), so line numbers in the review no longer
match; the findings still applied. Each was checked against the current code before fixing.

## High

**H1, destroyed entities stay tracked and get written to: fixed.**
- Pruning runs on GridNode's own thread all the time (every 15 ms, 4,000 entries a pass, reads
  only), not from the Inspector window. It was narrower than the review said: `prune()` ran only
  while the Inspector window itself was open. Logs showed 140,000 entries after a minute of play.
- Addresses found dead are remembered (bounded) until a new entity is created there.
  `inspector::alive(ptr, type)` answers from that and the type check, and every write through a
  tracked pointer asks it at the time of the write: queued moves (`runQueue`),
  `entity_set_transform`, `entities_near`, the layout editor's copies, templates and group
  lookups, and the Inspector's edits (re-checked on the client thread before writing).
- Since then (2026-10-21): every entity class's deleting destructor (vtable +0x38, 1,061 vtables) is
  wrapped, and the entity is marked dead before it goes, so death is known rather than inferred.
  Pruning stays as the backstop.

## Medium

| | Status |
|---|---|
| M1 shared lock re-entered | Fixed: listeners are copied under the lock and called after it is released. |
| M2 exceptions into engine frames | Fixed: every tick step, the creation listeners, level listeners, mod UI callbacks, the menu, the editor and recorder frames, and GridNode's own detours (events, animation flags, sublevels) catch and log. |
| M3 hook slots anywhere | Fixed: `mem::patchableSlot` accepts only an aligned slot in the image outside its code sections (every reflected vtable is in a data section, checked); `hook_slot` refuses others with `GN_ERR_SLOT`. The write keeps the page executable when it was (in this game every image page is), so a thread running code there no longer faults. The overlay's swap chain slots (in `dxgi.dll`) go through `patchModulePointer`: a slot in another loaded module, outside its code sections. (The first version of this fix refused them too, and the menu did not open.) |
| M4 nan / inf in layouts | Fixed: `load()` drops edits with values that are not usable (logged); `queueMove`, `writeInstance` and the body moves refuse them too; `normalize` no longer passes NaN. |
| M5 unchecked setters | Fixed: movement, position, time of day and tuning refuse non-finite values; the API header now states the calling-thread contract. |
| M6 asset refcounts | Fixed: `asset_find` only counts a reflected DataContainer; `asset_release` only releases what `asset_find` handed out (and refuses a second release). |
| M7 game pointers and counts | Fixed: a member's data must be a StaticModelEntityData before anything is spawned from it; per-copy indexes are bounded by the member's InstanceCount; render objects are checked to be render objects before the game is called with them. |
| M8 kernel calls on hot paths | Improved: `entities_near` checks the type before reading memory; the camera manager global is read without a kernel call; the group lookup is built once a second, not per edit. A spatial index is still to do. |

## Low

| | Status |
|---|---|
| L1 queued moves across a level change | Fixed: the queues are cleared when a level starts. |
| L2 normal object recorded as a group prop | Fixed: edits match by their own kind; standalone copies are recognised from GridNode's own record of them. |
| L3 detached threads | Fixed: `applyToExisting` runs on the server thread. |
| L4 render-thread I/O | Improved: Load and Unload run on the server thread; the layout list is read every 3 s. |
| L5 failed mods stay active | Open: hooks cannot be removed from a chain yet. |
| L6 dispatcher races | Fixed: `ticks_` is atomic, removals happen under the lock. |
| L7 tick listeners named "mod" | Open: needs an API change (the tick calls take no mod name). |
| L8 creator retries | Fixed: a slot that could not be hooked is not tried again, and is logged once. |
| L9 `g_real` race | Fixed: published with a compare-exchange; the loser drops its reference. |
| L10 injector | Fixed: a too-long path says so; early returns give back the remote buffer and the handle. |
| L11 crash handler and the log lock | Fixed: it uses `log::writeTry`, which skips a line rather than wait. (It still logs first-chance faults the game may handle.) |
| L12 `readCString` near unmapped pages | Not a defect: it already falls back to reading byte by byte when a 64-byte block fails (`walker.cpp`, the same in the reviewed commit). |
| L13 integer width from reflection | Fixed: the width comes from the type's name; the write never reads past the local. |
| L14 Rust panics | Fixed: every trampoline and the mod's main run under `catch_unwind` and log. |
| L15 truncated PE files | Fixed: a section past the end of the file is an error; zeros past a section's raw data (its virtual size) are still real. |
| L16 hard-coded addresses | Fixed for the layout editor and the Recorder: render-object offsets and vtables, the LOD scale, the sublevel bus and stream events are annotations now; the functions they depend on (`RenderObject_buildUpdate`, `RenderWorld_submitUpdate`, `RigidMeshEntity_setTransform`, `ServerSubLevelEntity_onEvent`, `WriteBoolGameState_write`, `TypedValue_getValuePtr`, `ServerWriteAntGameStateEntity_onEvent`) are fingerprinted, and the features that use them turn off when they do not match. The sublevel hook also requires the slot to hold that function. |
| L17 the 15 km park depth | Fixed: removed copies are destroyed as the engine takes an entity down (unlinked from its owner list, then deinit, destroy, delete; confirmed in game, including the level's own teardown afterwards). Parking (`kParkDepth`) is left only as the fallback for a game build whose destroy functions do not match. |
| L18 large files | Open. |

## Found while fixing

- `types_compile` failed to build: the type generator wrote `offsetof` checks for members it had
  only written as comments (overlapping ones: `InputDevices.keyboardKeys`). It now checks only
  members it wrote. Two signatures used C names (`int`, `const char*`) the generator does not know;
  they are `i32` and `const i8*` now. All five tests pass.
- `InputDevices` in `faith.yaml` annotates `keyboardBytes` as 256 bytes from +0x1a78, which runs
  into `keyboardKeys` at +0x1acc: one of the two is wrong and should be checked in Ghidra.

## Verification

- `gridnode.dll`, the mods, `version.dll` and the injector build (MinGW, no warnings).
- Linux tests: `reflect_walker`, `runtime_core`, `types_compile`, `engine_header`, `gridnode_gen`:
  5 of 5 pass. `gridnode validate`: 0 errors, 0 warnings. `gridnode rust --check`: up to date.
  The Rust crate checks (`cargo check`).
- Not yet run in the game.
