# Progress / handoff

Update at the end of each session. Keep it short. The full handover for a new agent is
[handover.md](handover.md).

## Current task
2026-10-29: handover steps 1 to 4 done, then frame and event names. Test build `gridnode-watchlock8.dll` deployed to
`D:\Dev\GridNode\bin\gridnode.dll`: destroyed lines, objects, list watches and the tracing fix are
confirmed in game, as are the class picker and the faster watches.
**GridNodeSync confirmed in Ghidra 12.1.4** (2026-10-29, run through the MCP with script arguments):
export (632 functions, 30 labels; backup in `bin/ghidra-test/my-names-before.json`), annotations import
(45 structs, 124 names, 539 of the user's kept, 573 comments, 0 problems), a recording's import (515
comments, 606 bookmarks on the call instructions); a second import changes nothing; the export reads
back with `ghidra-import`. The user's `~/ghidra_scripts/GridNodeSync.java` is the new one (old as `.bak`).

## Done recently
- **Review fixes, third round** (2026-11-03, untested in game). The "still in its code" check also
  reads registers (first second only) and logs long pauses; optional dependencies never block a load;
  `events::ensureHooked` keeps and repeats its result; pruning slows to every 250 ms once destructors
  are seen; `trackedOf` scans in blocks of 8,192; `view()` returns a copy taken as the frame starts;
  settings files are written outside their lock; replaced event and creation listener lists are freed
  after 10 s; camera and body vtable copies check the real size (copies now 512 / 256 entries); a
  read-only thunk page is reused with the threads paused; the crash handler names modules from a table
  made outside it; the decoder refuses 0x67 in moved bytes; manifest strings and lists are bounded;
  the injector names its copies `gridnode.copy-N.dll` and removes old ones; a queued console callback
  of an unloaded mod is skipped.
- **Declared settings (API 17, M9 item 4)** (2026-11-03, untested in game). `setting_add` and friends,
  `gn::setting::addBool / addInt / addFloat / addChoice / addText`: kept in the mod's ini with their
  description as a comment, clamped, and drawn by GridNode on a page per mod (menu, Mod settings;
  `settings_page(false)` hides it), so a mod with no UI code gets one. Portable parts in
  `gridnode/config.hpp` (tested); host side `modsettings.cpp`. The detour demo declares five.
- **Review fixes** (2026-11-03, untested in game). Before a mod or gridnode.dll is freed, every other
  thread is paused and checked for running in its code or having an address in it on its stack
  (`quiet.cpp`); still there after a few seconds: it stays in memory, inert (a mod: Unload tries
  again). GridNode's part of the destructor and recorder ANT hooks runs under `guardHook` (no
  exception into engine frames). The dead-entity set evicts its oldest quarter instead of clearing
  (a freed entity could pass as alive again); a reused address is cleared before the 400,000 cap.
  Code writes that fit no aligned 8 or 16 bytes are made with the other threads paused and none
  about to run them. Creation listeners: 64 per creator, extra ones logged. Chaining over another
  module's code in a slot is logged with the module.
  Second pass: functions between mods work from `gn_mod_load` (the mod being started is known by
  its code; the trainer's `godMode` was "not offered"); the recorder's ANT value cache is split into
  32 locks; `exportAll` checks its name itself; the landing hooks check the writers' functions
  (`AntFloatWriter_write`, `AntBoolWriter_write`, `PositionPrediction_update` in `ant.yaml`);
  `inspector::trackedOf(class)` instead of copying every entity; a queued hotkey press of an unloaded
  mod is skipped; `fbxbin.py` checks counts against the file. Left: the 11 vtables `faith.cpp` checks
  on its tuning paths are not annotated (compared only, never called: a mismatch turns the tuning off).
  Third pass (user's test, 2026-11-03): the "still in its code" check scanned whole stacks and a stale
  trainer address in a waiting thread kept it in memory for good; stacks are now unwound with the
  modules' unwind data (read by hand while threads are paused), scanning only past code with none.
  A GridNode kept in memory lets go of its instance mutex, and `gridnode_inject` then loads a copy
  (`gridnode.copy-2.dll`); mod copies in `mods\.loaded` skip names still mapped.
- **M9, first three items (API 16)** (2026-11-03, untested in game). Function detours (`hook_function`:
  the entry's instructions moved to a trampoline by `gn::x64`, a jump at the entry written with the
  other threads stopped, the detours chained in the hook table through a slot of ours; checked on the
  662 functions named in Ghidra: 658 take a detour, 4 are too short, instruction lengths agree with
  objdump). Mod manifests (`gn_mod_info`, `GN_MOD_INFO[_DEPS]`): ids, versions, hard and optional
  dependencies, the load order from them (`gridnode/mod_order.hpp`), functions between mods
  (`mod_export` / `mod_import` / `mod_version`); a mod others need cannot be unloaded on its own.
  Crash notes: the stack unwound with the modules' unwind data, game frames named from the
  annotations (`meta::functionsByAddress`) or as `FUN_<va>`, the thread named (server, client,
  render), the mods on the stack. Example: `examples/detour-demo`. The trainer has a manifest
  (`mec-trainer` 1.3.0) and offers `godMode`.
- **Code patches (API 15)** (2026-11-02, confirmed in game with the trainer's best-time guard). `patch_call` (a call or jmp rel32 sent
  to a detour through a thunk near the image) and `patch_bytes`; each listed in the menu's Hooks
  section, undone when its mod (the code that made it) is unloaded or at a full unload, and the
  original bytes go into image dumps. The MEC trainer uses it: a new best Dash time is not saved
  while it is loaded (`NamedChallengeStats_onFinished`, call site 0x143a8f522, runs; any best it
  wrote, its pending changes and its flag are put back).
- **Unloading from the API and per mod** (2026-11-02, API 15, untested in game). `mod_unload`
  (`gn::unloadSelf`, Rust `unload_self`): the calling mod (found by its code address) is unloaded and
  stays unloaded; `gridnode_unload` (`gn::unloadGridNode`, Rust `unload_gridnode`): the full unload.
  The menu's Mods list has Unload and Load for mods with `GN_MOD_UNLOAD`.
- **Soft anti-cheat in the MEC trainer** (2026-11-02, confirmed in game: a new best was neither saved
  nor uploaded, also with Beat Revival connected). While the trainer is loaded,
  the gateway backend's upload requests for Dash times (`UploadNamedChallengeStatsRequest`), time
  trial times (`UploadStatsRequest`), ghosts (`UploadReplayRequestMessageBase`,
  `SetPlayerGhostRequest`) are dropped: both message listeners that lead to the backend are wrapped
  (hooks `GatewayBackendListener_onMessage`, `GatewayRouteListener_onMessage` in
  `db/annotations/online.yaml`; out again when the trainer unloads). A line at the top of the screen
  says the trainer is loaded and whether uploads are blocked; it is not an option.
- **Full unload of GridNode and every mod** (2026-11-01, untested in game). The menu's Unload section
  ([modding.md](modding.md#unloading-gridnode-completely), `sdk/src/unload.cpp`): mods unloaded (their
  `gn_mod_unload` puts their changes back), GridNode's own changes back (recording, free camera and its
  vtable copies, held keys and actions, god mode, layout copies and magropes, message listeners), every
  hook out, the overlay closed and Present / ResizeBuffers / the window procedure back, then
  `gridnode.dll` freed (it is no longer pinned; the loader's reference is the only one). A mod without
  `gn_mod_unload` or a hook over GridNode's keeps it in memory, inactive. The MEC trainer now has
  `GN_MOD_UNLOAD` (god mode, noclip, free camera, tuning, time scale, post-processing, fast loads back),
  so it is reloadable too.
- **Trigger volumes in the MEC trainer** (2026-11-01; shapes confirmed in game, the tab untested).
  A Volumes tab draws the level's area triggers from their own geometry caches, sorted into
  categories (cutscenes, checkpoints and spawns, mission, city alert, AI, music and voice, other) by
  the blueprint names their events reach; ticks per category, a search box, a distance slider and
  labels on the nearest. Research: [trigger-volumes.md](research/trigger-volumes.md). API 14
  `entities_of_class` (logic entities, not only spatial ones; Rust `entities_of_class`) came out of
  it. The user checked the categories in game and found nothing wrong. Not drawn yet: kill volumes,
  camera triggers, mission checkpoints (no geometry cache at the area trigger's offsets). Dash start
  volumes show under "Dashes and challenges" (confirmed, 2026-11-01: the link crosses the Dash
  blueprint's interface into the parent bus). Open: quitting during a Dash crashed twice (game code,
  0x143164d50); check once without GridNode (research doc).
- **The free camera for mods (API 13) and in the MEC trainer** (2026-11-01, untested in game). The layout
  editor's free camera now runs without the editor tool, and a mod's is a plain camera even with the editor on (the first version showed the editor's keys and picking; fixed 2026-11-01):
  `freecam_set` / `freecam_get` / `freecam_teleport` (to its crosshair, or to the camera). The trainer
  has a Free camera section and hotkeys 6 (fly), 7 (teleport to the crosshair), 8 (to the camera), and
  "keep flying after a teleport". New plan from the user (2026-11-01), in order: volume
  visualisation by type (triggers, checkpoints, cutscene starts...; tick what to show), full unload
  of mods and GridNode (everything restored), a soft anti-cheat in the trainer (block time trial and
  Dash uploads, an on-screen mark), and patching support in the SDK. The rules now allow patching
  and the trainer's upload block (CLAUDE.md).
- **Landing prediction when GridNode loads late** (2026-10-30, untested). A friend's log: auto-bhop fell
  back to the height method because Faith's animated entity was not found ("0
  ClientAntAnimatableEntity tracked": GridNode came in after the level loaded, so no creations were
  seen). Her +0xd0 is now walked back to the entity by its type (up to 0x100 bytes), which needs no
  tracked list.
- **Recorder: after the fact, and on an event; mods send events (API 12)** (2026-10-30, untested in
  game). "Keep only the last N seconds" records all the time and keeps a window; End exports it
  (`<time>-last<N>s`, a `saved` marker, `rolling_seconds` in the header). "Start recording on this
  event" arms an event id; the first delivery starts recording with a marker. `event_send` /
  `event_send_class` (`gn::sendEvent`, `gn::sendEventToClass`; Rust `send_event`,
  `send_event_to_class`): the Recorder's "send" moved to `events.cpp` (`events::send`), shared.
  Next: entity properties need research first (record `PropertyValue_set`, 0x142a464e0), then
  spawning.
- **The 425 `hypothesis` functions read in the decompiler** (2026-10-30, in the user's Ghidra). Every
  one now has a signature from the decompiler (414; the rest had one): integers used as addresses
  become `void*`, named types Ghidra already had stay (`SettingsManager*`, `Message*`...), a
  `Class_method`'s first pointer is `self`. 90 are now `inferred`, each note saying what showed it:
  the ctor sets that class's vtable, the function checks `TI_<Class>`, its strings name it (two words
  of the name, or the class), or (spawn, physics, entity, players, camera, replay: read one by one)
  what it calls and does. 335 stay `hypothesis` ("the name is not checked yet"), mostly resources
  (96), render (58), levels (46), reflection (32), startup (31). Imported with GridNodeSync: 684
  prototypes, none of the user's own prototypes or names changed (exports before and after compared,
  `bin/ghidra-test/my-names-*-hyp.json`).
- **Reloading mods while the game runs** (2026-10-30, confirmed in game by the user: manual reload, "when changed", hello and world-draw). A mod exporting
  `gn_mod_unload` (`GN_MOD_UNLOAD()`, Rust `gn_mod_unload!`) is loaded from a copy in
  `mods\.loaded\` and can be reloaded from the menu (Mods: Reload, or "when changed"). Unloading
  calls it, then each part of GridNode drops the callbacks whose code is in the mod's image (ticks at
  the next tick: `Dispatcher::removeListeners`; hooks: `HookTable::remove`, which re-links a chain
  around a hook in the middle; both host-tested), waits for frames and ticks, frees it, loads the new
  copy. `hello` and `world-draw` are reloadable.
- **API 11: events between entities for mods** (2026-10-30, confirmed in game: 1354 vtables wrapped, 0 failed; `hello` got StreamIn / StreamOut; the Recorder's `events-1` has 9,984 named event lines with stacks through the shared wrappers; "send" reached 1 PamServerCityAlertManager). `on_event(mod, event,
  class, fn)`: a name or "0x<hash>", a class (with subclasses), either open. The onEvent wrappers
  moved from the Recorder to `events.cpp`, shared: the Recorder is a tap on them (so the player build
  has events too), and the event name table moved with them (`event_name`). Recorder fix:
  "send event to class" refused every entity once events were wrapped (it required the slot to hold
  game code). `hello` logs the first 40 StreamIn / StreamOut events. Rust: `on_event`, `event_hash`,
  `event_name`. The movement events' portable namespace is now `gn::moveevents` (it clashed with
  `gn::movement` in mod.hpp).
- **API 10: drawing in the world for mods** (2026-10-30, untested in game). `view_get` gives the camera
  as drawn (the layout editor's `viewBasis` and FOV, the free camera while it flies) and the screen
  size; `gridnode/projection.hpp` the editor's projection math (host-tested); `gn::ui::line3d`,
  `box3d`, `marker`, `text3d`, `toScreen` draw with it on the background draw list. New example
  `examples/world-draw`: a box around Faith, the entities near her labelled with their types and
  distance (options kept in `mods/world-draw.ini`). Rust: `view()`, `to_screen()`.
- **API 9: mod settings, hotkeys, movement events** (2026-10-30, untested in game).
  `config_get` / `config_set`: per-mod `key=value` files in the mods folder (`config.cpp`, written a
  second after the last change and at exit; parsing in `config_core.cpp`, host-tested).
  `hotkey_add` / `hotkey_info` / `hotkey_rebind` / `hotkey_set`: keys read every frame on the render
  thread, run on the client thread, kept as `key.<id>` in the mod's own settings and rebound in the
  mod's own window with `gn::ui::hotkeyButton` / `hotkeyRow` (clashes with other mods and GridNode's
  menu, F7, F8 and Home shown) (`hotkeys.cpp`). First test: a GridNode **Hotkeys** window, where
  clicking a key did nothing (it waited on GetAsyncKeyState's unreliable "pressed since" bit); the
  user wanted the keys in each mod instead (a mod's players need no GridNode tool), and the wait now
  reads keys going down. `on_movement`: landed / airborne / state events from a reading per client tick
  (`faith.cpp`, the diff host-tested). Rust bindings regenerated with wrappers. The MEC trainer uses
  all three: its keys are hotkeys (plus "auto bunny hop on / off", no default key) in a Keys tab, its settings
  are kept in `mods/mec-trainer.ini` (not god mode, noclip, frozen time), and its HUD shows the bhop
  chain (from movement events) with an option to log every event.
- **Bhop research: the game's own landing prediction** (2026-10-30, Ghidra; untested in game). The
  movement state and in-air byte are a mirror of ANT's graph (`PamMovement_readAntState`), and the
  jump button goes into ANT as held / not held, so a press in the air is spent there: the press must
  land on exactly the landing tick, which prediction from heights cannot always hit. ANT's
  `PositionPredictionProcessorAsset` (vtable 0x141aabb98, update +0x20) computes, once per jump or
  fall, the time to the hit from the trajectory (`ant_predictTrajectoryHit`, x60). The Recorder's
  Faith's movement watch now wraps that update (pointer swap) and records the prediction
  (`gridnode-predict1.dll`, confirmed in game: `predict-test`, 1,498 predictions in 50 s). The
  prediction is recomputed every tick (~33 ms) while she is in the air, not once per jump. On the
  tick before a landing it read 0.74-1.54 (60ths of a second), two ticks before 1.96-2.91, on the
  landing tick 0.06-0.44, in 23 of 29 landings (17 of 19 from the wall-run state); in the other 6 it
  dropped near 0 a few ticks early and hovered (the hit height moving too: an edge, slope or the
  wall). So "press on the first tick it drops below ~1.8" picks the landing tick ~9 times in 10, at
  any speed. Annotated in `ant.yaml`, `input.yaml`.
  **API 8: `landing_prediction`** (`sdk/src/landing.cpp`, `gn::landingPrediction`, Rust
  `landing_prediction`): wraps the prediction's update and the float / bool game-state writers' write
  (pointer swaps, chained with the Recorder's) on the first call and keeps Faith's. The trainer's
  auto-bhop presses on the first tick it drops below 1.8 (slider), the heights as the fallback.
  First test (`landing1`, `auto-bhop`): the prediction was never used: Faith's +0xd0 held vtable
  0x141b06828, not ClientAntAnimatableEntity's 0x141b06770 (an object inside the entity; the game has
  no type info to say where), so it fell back to the heights (63 hits, 10 misses over both logs).
  And the Recorder recorded no predictions: its own "slot holds the game's function" check refused
  the landing module's wrapper. Now Faith's animated entity is the ClientAntAnimatableEntity on her
  bus (+0x20), searched on the caller's thread (never in the wrappers on the animation threads),
  and the Recorder lets the hook table chain. Second test (`landing2`, `auto-bhop-2`): still no
  prediction (not on her bus either) and still none recorded: the Recorder's ANT writer hook refused a
  slot that did not hold game code, i.e. held the landing module's wrapper (fixed: the hook table
  checks). Heights alone: 54 hits, 13 misses. Now Faith's entity is also the tracked one her +0xd0
  points into, and the log gives the evidence when none fits. Third test (`landing3`, `auto-bhop-3`):
  works. Faith's +0xd0 points 0x30 into her ClientAntAnimatableEntity. 11 presses by the prediction,
  9 hits; in the recording every press landed exactly one tick later (the right tick). One miss was a
  predicted soft landing: she went to state 1 although jump came on time. Soft landings cannot be
  bhopped (user, 2026-10-30): the trainer now skips them (logged, not counted). The other (pressed at 1.71, outside the recording) ended a chain whose press values
  crept up 1.35 -> 1.71, so 1.8 may be near the upper edge: watch it.
  Fourth test (`auto-bhop-4`, long chains and cboosts; 84 presses, 76 hits, 7 misses, 5 soft landings
  skipped): every hit was pressed at 0.52-1.65 with the hit 0.11-0.35 m below her. Four misses were
  pressed at 1.73-1.81 and landed two ticks later (a tick early); three had the hit above her
  (+0.08 to +0.19 m: rising toward a ledge, one landed 19 ticks later). The trainer now presses below
  1.7 and only with the hit below her. With the slider at 1.66 (user, 2026-10-30): chains without
  end, 92 hits in 93 presses, the miss pressed at 1.68. Default now 1.66. Arming one hop (F) turns
  auto off. The HUD speed is measured from her position: `gn_movement.speed` (body motion +0x438)
  read 0.00 most of the time; what that field is needs a look. The trainer's HUD can also show the
  game's landing prediction (a bar to the landing, green in the bhop window, red for a soft landing).
  Mag rope swings bhop the same way (`speed-2`): state 25 lasts through the swing jump until the
  landing, a landing with jump held goes 25 -> 0 -> 25, without it 0 -> 1; the prediction runs there
  (1.55 and 1.10 on the tick before). Auto-bhop now takes wall runs (8) and swings (25), each a
  checkbox; a hit is the press's state again. Untested in game.
- **Bhops** (2026-10-30, two recordings and the trainer's log): a successful bhop lands with
  `FootPlantLandSoft` and `FootPlantWall` at once (still in the wall state) and its jump fires no
  `Jump` event (not a ground jump); a vertical-speed-like ANT value goes from -8.8 straight to +4.9.
  The failed one jumped 0.17-0.33 s after landing; several of Faith's ANT flags flip about 0.17 s
  after touchdown there (likely the switch to grounded: the window is shorter). The trainer's
  auto-bhop never fired: on the server tick (30/s) Faith falls ~0.29 m between checks, so state 8
  always ended 0.15-0.25 m above the ground by its ray cast (tolerance 0.10).
  **Faith's movement** in the Recorder (off by default; confirmed in game, ~25 lines a recording):
  state, in air, wall counters and the jump action every frame, as field lines when they change.
  It showed (`bhop-hit` / `bhop-miss`, 2026-10-30): state 8 is the wall run and lasts through the
  jump off the wall until the landing; the landing tick decides: with jump held then, she stays in
  state 8 and leaves the ground 37 ms later (the bhop); without it she is state 1 in that same tick.
  So the jump must be down on the landing tick (pressed the tick before). The "on ground" byte
  (ground +0x17, `gn_movement.on_ground`) is 1 in the AIR: notes and comments corrected, names kept.
  The trainer's auto-bhop now presses jump on the server tick before the landing (its fall over the
  last tick predicts the next), and logs hit / missed. First test (`mec_trainer-bhop1.dll`, log
  10:28-10:30): 22 hits, 13 misses; the landing registers while the ray still reads ~0.20 m, every
  press at least ~1 tick of fall before that height hit, every one later than ~0.65 tick missed (up
  to 3 ticks early still hit). Second test (`mec_trainer-bhop2.dll`, lead in ticks): 81 hits, 24
  misses, chains of 8-10; presses 2.1-2.2 ticks early missed too (the press is released before the
  landing), so the window is ~33-67 ms; and at a low frame rate (while recording) presses came after
  she had landed: the server's copy of her position lags the client's. Now the prediction runs on the
  client tick from the client character (the server ray only gives the ground height), timed in ms
  (lead 50 ms), and a hit is judged at the landing (in air drops to 0) instead of "state 8 and wall
  count 0", which counted chained hits a tick early. Third test (`bhop3`, lead 20 ms best): the
  client ticks at 30/s too (33.3 ms); every hit landed one tick after its press, every miss two
  ticks after: the press must come on the last tick before the landing. The ray-based height is good
  to ~0.1 m only (half a tick at 6 m/s). Chained hops on flat ground are exactly 20 ticks apart, and
  a hit can land into state 0 for two ticks before state 8 again; a cboost landing (14.8 m/s) went to
  state 24. Now (`mec_trainer-bhop4.dll`, untested): each client tick predicts her height one tick on
  and presses when it reaches the last landing's client height (in a chain) or the ray's ground +
  0.15 m (first hop); Bias slider. The Recorder's movement watch also records "jump held by mods"
  (`gridnode-movewatch3.dll`).
- **Recording diff** (2026-10-30): `summarize.py A --diff B` lists what happens only (or far more,
  per second) in A, only in B, A's lines in order from the marker, and their stacks; `--marker` /
  `--diff-marker` / `"*"`, `--kinds`, `--values`, `--min-lift`. Tried on the door recordings: kick vs
  slow open shows `DoorKick` on the door's bus, the `Kick` sound and a voice-over trigger, in order.
  Old recordings exported without Clear share their start (slow-door holds all of kick-door); the
  diff says so. Untested by the user.
- **PamServerProjectionSpawnerEntity** (2026-10-30): the `ProjectionSpawner_*` functions are this class
  (0x90 bytes, vtable 0x141c7e920 holds the download listener); annotated with its members (replay
  name, flags, lock, recording and size) in `replay.yaml`. The challenge and replay markers ask it for
  a replay.
- **Ghidra inbox sorted** (2026-10-29, checked in the user's Ghidra): all 111 new names are in the
  annotations. 110 `ant_define*` functions (checked: each passes its own type's name; notes list the
  fields) in `ant.yaml`, `inferred`; 26 replay functions (playback, keyframe ring, recorder setup,
  the server entity that loads a downloaded replay into a ghost) in `replay.yaml`, read in the
  decompiler and consistent with `ReplayRecorder`'s members; engine start/stop and the arena allocator.
  Of the 34 names that differ, GridNode's are kept (the meaning matches); the two that read
  differently note the other name (`ServerGameWorld` is the same global as `g_physicsQueryManager`).
  `ghidra-import` no longer lists GridNodeSync's own `vtbl` and copied base members as findings.
- **More prototypes** (2026-10-29): signatures from the decompiler for the 28 checked functions that
  had none; 268 prototypes now go into Ghidra (from 115). The 421 `hypothesis` functions still have none.
- **Prototypes and recording classes in Ghidra** (2026-10-29, run on the user's Ghidra; untested by the
  user): `ghidra-export` gives each function with a known signature its prototype (115 applied, 1 of
  the user's kept); entries that share an address are merged into one (5 did: they swapped names and
  comments on every import). `summarize.py --ghidra` adds structs and vtable types for the classes a
  recording traced (`--classes traced|all|none`, `--reflected`); a later import never moves `this` to
  a subclass. Comment blocks keep a fixed order, so a second import changes nothing (0 comments).
  Spawner signatures in `spawn.yaml` now take the spawner class instead of `void*`.
- **Class structs and vtable types in Ghidra** (2026-10-29, run on the user's Ghidra): `ghidra-export`
  gives each class its vtable (annotated, else the dump's), depth and base classes' members (`--class
  NAME` for more); GridNodeSync builds `<Class>_vtbl` (slots named and typed from what they point to),
  types the vtables in the listing, puts `vtbl` at +0, and types `this` of the classes' virtuals (17
  vtables, 266 functions; 52 now read `this->vtbl->...`). It found that the client spawner notes had
  the vtable slots 0x20 off (onCharacterSpawned +0xd8, removeSpawned +0xe0; checked in the image and
  traces); fixed, and `ClientSpawnEntity` / `ClientCharacterSpawnEntity` are annotated classes now.
- **Confirmed in game** (`20261029-171715-test-names-test`): the class picker (user), event names
  written by the recorder, the `frames` line (all frames named with no dump), every-frame lists (server
  add 2 ms after StartReplay, client 3 ms after CharacterSpawned). Watch timing is now kept per
  recording (menu, header `watch_readings`); it read 0 before because watches only run while recording.
  Every-frame list watches cost 2.1 ms a frame (3.7 worst) with ~1,100 spawners.
- **Cheaper watches, picked up at creation** (2026-10-29, untested in game): one read per empty list
  (begin and end together), entries only when present, the re-check only on a change; no liveness
  check per reading (destroyed entities leave the watch set from the destructor hook); entities of
  watched classes join at creation, so a level load no longer hides a spawner's first character. In game
  (`watchfast-test`): 0.82 ms average (from 2.1), 3.7 worst; ~45 spawners filled during a reload
  still showed as "first". Then (untested): the reading holds the watch lock and reads entities seen
  created with plain loads (their destructor hook waits on the lock); scanned ones keep safe reads;
  while recording, a created entity's values at creation are its baseline, so fills before the next
  frame show as additions. **Confirmed** (`watchlock-test2`): 0.19 ms average, 3.0 ms worst (a reload
  frame); every spawner filled during two reloads shows its characters as additions (no non-empty
  first readings), no lines from destroyed spawners, no hitch at reloads. Reloads fill the 250,000-line
  buffer: untick created / destroyed when recording across one.
- **Frames named by function, events from the annotations** (2026-10-29): summaries print each frame
  as `0x143234f21 (ClientSpawnEntity_createFromNetwork+0x51)` (address first, for Go To), `(Class::vf+0xNN
  +0x..)` or `(FUN_<start>+0x..)`. Starts from the recording's new `frames` line, `callers`, or a dump
  (`--exe`, default newest `../dumps/*.exe`); `--ghidra-names` adds the user's Ghidra names. New
  `events:` annotation section (`events.yaml`, hashes checked): into `engine.hpp` (`hash::names[]`), which
  the Recorder uses in game; `summarize.py` names them in older recordings. The in-game part is untested.
- **Recordings into Ghidra** (2026-10-29): `summarize.py REC --ghidra OUT --exe DUMP` writes a GridNodeSync
  import: `createFunctions` (unwind-table starts, `PeImage.function_start`, chained entries followed),
  `bookmarks` (call before each return address, category "GridNode recording"), `comments` (plate block
  `GridNode [recording] (<file>)`, its own block key). GridNodeSync reads those, and takes script
  arguments `import FILE` / `export FILE` (no dialogs). Confirmed in Ghidra (above). The Scylla image's exception directory
  is intact (RVA 0x2630000, 0x17e974 bytes).
- **Recorder: destroyed entities and the object table** (2026-10-29): `destroyed` lines from the
  destructor hooks (entities.cpp calls `recorder::onDestroyed`); every pointer an event names gets an
  object id (`obj`), exported as `object` lines (type, data, blueprint, created / destroyed / first seen;
  a reused address gets a new id). `summarize.py --object ID|0xPTR` tells one object's story.
- **Recorder: class picker** (2026-10-29): "..." beside every class field (trace, watch, send) lists
  classes alive now / in this recording / all, searchable, base classes on hover. Not yet tested in game.
- **First recording with the new build** (`20261029-161758-test-ghost`): 38,570 `destroyed` lines and
  72,110 objects. Ghost server characters each live from `ServerControllableSpawnDoneMessage` to
  `ServerControllableUnspawnDoneMessage` (+0x30 is the character); StopReplay at marker 1, StartReplay
  and CharacterSpawned at marker 2. Ghost characters' creations have no bus blueprint name.
- **List watches confirmed in game** (`20261029-162532-ghost-test`): the ghost spawner
  (`PamServerCharacterSpawnEntity`) holds one character at a time; each StopReplay / StartReplay cycle
  removes it and adds a new one about 2 to 3 s apart. Client spawners were missed: **tracing a class
  put a thunk in vtable slot 0 (`getType`), so `typeOf` no longer knew its objects** (the inspector
  dropped them, watches skipped them). Fixed: tracing skips slot 0. **Confirmed** (`20261029-163034-test-test`):
  client spawners recorded, server and client lists change in step; client characters come from
  `ClientSpawnEntity_createFromNetwork`. A ghost living ~0.1 s (server #10617) was missed by the server
  list: lists are read 10 times a second, so changes shorter than that can slip by (creations and
  destructions still show them).
- **Watches read every frame** (2026-10-29, the user: for reverse engineering anything matters): fields
  and lists are read every frame by default; a *readings a second* slider lowers it, and the menu
  shows how long the last reading took. Not yet tested in game.
- `CLAUDE.md` at the root: rules, build, the user's PC.
- **Recorder: list watches** (2026-10-29): watch type *list* (vector begin/end at the offset): `list`
  lines with `added`, `removed`, `reordered`, `items`. Preset *Add the spawners' lists*
  (ServerSpawnEntity +0x148, ClientSpawnEntity +0xb0; the client class name is from the annotations,
  unchecked against the reflection).
- **Fingerprints from the game** (2026-10-23): menu → *Game image* → *Dump the game image* writes
  `bin/dumps/*.exe` (hooked slots restored to the game's pointers). `gridnode.py fingerprint DUMP
  [--write] [--add NAME] [--mecmp DIR]` checks / rewrites GridNode's and MECMP's fingerprint bytes.
  Checked against the first dump: all of GridNode's 38 code entries and MECMP's 19 checks match; 12
  more entries added from the image.
- **Recorder: trace a class's virtual calls** (2026-10-23): thunks with unwind info on every vtable
  slot (passing over slots already hooked), rate-limited, stacks unwound from the caller. Confirmed
  in game on `ClientCharacterSpawnEntity` (52 slots). Export kind `call`; `summarize.py` names it.
- **Tools** (2026-10-22): `annotations-diff` (two annotation sets, folders or `git:REF`, `--inbox`),
  `ghidra-import --who`, `ghidra-relay` (a friend's Ghidra export into your Ghidra), GridNodeSync
  exports your comments and keeps one comment block per source. GridNodeSync's Java changes are
  **not yet run in Ghidra**.
- **Research** (2026-10-22/23): replay ghosts (StartReplay / StopReplay through a linked character
  spawner), the map's echo and the Your Echo screen take the **last** character of the city-ghost
  spawner's client list (directly and through `Entity_resolveLinkedEntity`), the city alert, the
  startup sequence. Spawner and replay functions annotated in `db/annotations/spawn.yaml`.
- **MECMP** (PRs #102 to #106): ghosts despawned through the spawner's unspawn; client objects
  claimed by owner and bus value; the game's echo kept last in the server and client spawner lists
  (map and Your Echo fixed, tested); own spawner experimental and off (`own_spawner=0`); ghosts shown
  again when the map or a menu closes; MSVC C4702 fixed.
- **Nollz's PR #2** (merged): GridNode chains onto another mod's hook in either load order.
- **Layout editor** (paused on purpose): barriers and magropes shown, moved, saved and copied;
  magrope trigger volumes sized by their transform (`Target` drawn as a point, meaning unconfirmed).

## Volumes tab, 2026-11-02 (picking and trails untested in game)
Picking (0 toggles, 9 logs the trigger in the middle of the screen, a "Largest shown" size filter),
a per-trigger Log of every link it follows, a Teleports category with each destination drawn (Old
Friends' fall catcher), and trails that follow events out of a blueprint (source bus +0x60) and into
placed blueprints (FullEntityBusBridge), so story checkpoints sort as checkpoints. Patches: the
thunk for patch_call is placed with VirtualQuery above or below the image (one run found no memory
at fixed steps), and every way a patch can fail is logged. See research/trigger-volumes.md.

## Checked in game (2026-11-02)
Confirmed: the landing late-load fix (auto-bhop), the Keys tab's click-to-rebind, the speed field
(the trainer's HUD speed was jittery tick to tick: now measured over a quarter second, untested).
The crash when quitting during a Dash (0x143164d50, a null world in the mission clean-up
`FUN_1439e08f0`) happens only with GridNode injected; not explained yet.

## Next steps
1. The 335 `hypothesis` functions left (all have signatures now): read them one by one, area by area
   (resources, render, levels, reflection, startup first).
2. In game: the class picker, and every-frame watches (their cost shows as *last reading*; does a
   ~0.1 s ghost now show in the server list?).
3. Check `gn_movement.speed`, now the length of her velocity at movement ground +0xd0 (x, z at
   +0xd0, +0xd8): `speed (game)` should follow `speed (measured)`. The speed probe matched it in
   82-86% of samples in two runs (`speed-3`, `speed-4`); the old trainer path
   ([[[0x1423e29b8] + 0x2378] + 0x10] + 0x438, a Cheat Engine path past the end of its object;
   properly slot +0x1e8 of the object at 0x1423dd6f8) 77%; our old path (Faith +0x28 -> +0x1f0 ->
   +0x10 -> +0x438) never (+0x1f0 held garbage or a MathEntity). The first pick, movement state
   +0x1bc, matched in `speed-3` only and read 0 throughout `speed-4`: one run is not enough. Writing
   the speed (the trainer's position load) did not keep it with +0x1bc; it now scales her velocity
   and writes the old trainer's field too (untested).
Later: layout editor (occluders in the overlay, copying a whole group prop, confirm magrope `Target`).
Later (user, 2026-10-30): the trainer spawning volumes such as triggers. Leads found in Ghidra, not
looked into: `HavokVisualDebugger` and `HavokCaptureToFile` are reflected settings fields in two
settings classes (one also has `SavePoint`, `DisableCutscenes`, `ShowTriggerDebugText`); the Havok
Visual Debugger is Havok 2013.3's (path in the exe), which streams to Havok's own viewer app over the
network rather than drawing in game. Also present: `PhysicsDebugSettings`, `DebugRenderSettings`,
`EnableDebugRendering`, `DebugRenderer`, `hkpTriggerVolume`, `WithinPlayerTriggerVolumeCondition`.
`ShowTriggerDebugText` and the debug-render settings may show triggers in game, if retail kept the
renderer: try them in the console first. Our own `gn::ui::box3d` can draw trigger boxes either way.

## Decisions and gotchas
- Patching game code is allowed since 2026-11-01 (prefer pointer swaps; everything undoable through GridNode).
  Leave online services and Beat Revival alone, except the trainer's upload block. PC only.
- Never commit game data or user dumps (dumps, FBX game meshes, event-names.txt, logs, recordings).
- Commit / push only when asked. Deploy test builds under a unique file name first.
- A fingerprint's bytes come from a dump, never from a disassembly listing (one instruction has
  several encodings; a guess turned MECMP's own spawner off on the right game).
- The game finds spawners by their data in a bus: a second spawner with the same data and bus (MECMP's
  "own spawner") is found instead of the game's one. Keep the game's character last instead.
- The u32 at a character's bus +0x58 is the same for every ghost of a spawner (low bits of the
  spawner's data pointer): not a per-character id.
- A dump made with hooks in place shows hook pointers in the slots; the dump button restores them,
  and the checker reports a pointer outside the game as "hooked when dumped".
- The user's local checkout `D:\Dev\GridNode` is far behind `main` with uncommitted annotation edits
  from another agent (most wrong; the repo already has the corrected versions). Steps to clean it up
  were given 2026-10-22 (back up, `git restore`, remove the 4 untracked YAML files, pull).
