# Progress / handoff

Update at the end of each session. Keep it short. The full handover for a new agent is
[handover.md](handover.md).

## Layout editor: crash when undoing a copy (2026-11-06; fix confirmed by the user)
Log: the third copy destroyed, then a crash in the same server tick, in the trainer's ground ray cast
(`FUN_1434de340` -> `PhysicsOwner_objectOfHit`: the owner's table pointer was ASCII, freed memory). Cause
(from `PhysicsWorld_flush`'s notes): the game releases a physics record only in the next flush, but
`destroyEntity` frees the physics component at once, so a query between the two reads a freed component.
Which copy crashes depends on where the ray goes (two earlier destroys were fine). Fix: after a
server entity is destroyed, server ray casts (`raycast`, `raycastObject`) answer unavailable until the
server world's next flush, a second at most (`physicsStaleNote` / `physicsQueriesSafe`, bodies.cpp). A
mod's ground check fails for one or two frames after an undo. Other server queries (the editor's own
`entities_near`, body functions) are not guarded. The destroy runs after the flush in our post-update;
doing it just before the flush, as the engine's own unload does, would be cleaner (hook point: bodies.cpp's
`flushHook`).

## MECMP compared with the API (2026-11-06)
Another model compared `D:\Dev\MECMP` with API 29. Taken: **API 31 `localize`** (0x1436a8d40). Not taken, and why:
- Dash and time trial events: `on_message` already delivers them (category 0x54776fc6; types started 0xc164590c,
  finished 0xd71d027f with the time at +0x58 ms, cancelled 0x9a9a27c6, time trial finished 0x9b35497c); a mod
  reads the fields itself (MECMP's `runs.h` has the layout). A typed wrapper is possible later.
- Map markers (`PresenceInfoManager_assignSlot` 0x143a405e0, slots of 0x50 bytes, the pool update slot
  0x141c74bc8): needs a hook on the pool update and a real online id (the icon is the Runner Tag), so it only
  suits online mods. Not exposed.
- Character spawn, outfits, bone poses, vocals, gamepad and pause hooks: large, each needs its own research and
  game testing (see `research/replay-ghosts.md`); MECMP keeps them. Pause also needs the UISystem instance.

## The ReGlitched branch (2026-11-06)
Another model read `mecmp-reglitched-physics`. Taken: **API 33 `raycast_filtered`** (the ignore list in `PhysicsRayInput`
was already annotated). Already in GridNode: the third-person body (`ClientCharacter_GetViewMode`, the 27-slot body
hook), boxes (`box_add`, which borrow a static body), sounds and rays. Checked in Ghidra and **wrong in the report**: the
voice vtable (0x141acfad0) +0x18 is not `setParameter` (it registers the voice with the sound manager) and +0x78 takes no
vector (it clears the 3D state's velocity fields), so no `sound_set_velocity` / `sound_set_parameter`. Not taken:
standalone Havok boxes (`PhysicsEntityFactory_create` 0x1435169e0, `RigidBody_transformSource` 0x1434f6d90 exist in Ghidra;
creating a body without a borrowed one is the one real gain, but a wrong call crashes the physics world: needs its own
research and an in-game test), the audio graph cloning and PCM injection (MECMP's voice chat), the conversation system
(`VoiceOverSystem_*` are annotated, not exposed).

## Second MECMP comparison (2026-11-06)
Another model went through MECMP's other files. Taken: **API 32 `ui_pause_state`** (the UI system global 0x1424157f0,
checked in Ghidra: the getter 0x143269ae0 is `mov rax, [rip+..]; ret`). Paused means requests A >= 1 and either B >= 1
or a time value below 1e-6 (`UIPauseController_applyToClient`). Not taken yet, all from MECMP and unchecked here:
- **Safe save reload**: `PamplonaRestartGameMessage` (category 0x0dc84585, type 0x04aea817, vtable 0x141c8eee0, 0x38 bytes)
  with "ResumeSaveGame" at +0x30, queued on the client manager. Ghidra shows the helpers exist (poolAlloc 0x142a770c0,
  Message_ctor 0x142a37120, MessageManager_queueMessage 0x142a3ad30); reloads the checkpoint, so it needs an in-game test.
- **Damage and death**: health component vtable slots 0x100 (apply damage) and 0xf0 (set health, death at <= 0).
- **AI targets and teams**: a `ServerPlayer` made for a character, team at +0x139c, `AIPlayerEnableAsTargetMessage`.
- **Press-F triggers**: `PamServerSpatialEntryInputTriggerEntity`, fire through trigger vtable slot 0x88.
- **Outfits and map pins**: see the first comparison above.

## API 30: streaming focus points (2026-11-06; untested in game)
- `streaming_focus_add/move/remove` (`layouts.cpp`, `focusApi*`): up to 4 points a mod owns, added to the streamers'
  focus lists ahead of the layout editor's own, merged when within 60 m, synced every 2 s and 250 ms after a move.
  Released on unload (`forgetStreamingFocus`). Needs the known streamer (`focusKnown()`).
- Trainer 1.7.1: World tab has Hazards first; the sun sliders and the streaming box sit in a closed "Look and streaming" header; the timeline buttons are gone (the API stays).
  "Load the world around the free camera": Open: does a far district really stream in,
  and how fast does it unload again?

## API 29: the visual environment and timelines (2026-11-06; untested in game)
- **`visual_env_get/set/release`**: holds values in the visual environment state (MAIN +0x50 -> +0x90 ->
  +0x1c0 -> +0x30, state +0x85470: sun, tonemap, colour correction, sky, wind, depth of field, vignette,
  film grain), written back after each server and client world update like the clock hold. Offsets
  are the cinematic tools'; only the state offset was checked in Ghidra (two functions use +0x85470). The
  update itself is still not hooked: if a held value flickers, hook it (research/visual-environment.md).
- **`timeline_time` / `timeline_set_time`**: calls the game's `Timeline_SetTime` (0x143691120, checked in
  Ghidra: time +0x98, force skips the external-time check).
- Trainer 1.6.0: World tab "Look" sliders and a timeline list / seek, for testing.
- **Left out on purpose** (the synthesis doc, 2026-11-06): class instance lists (`ClassInfo` +0x60 and the
  node at entity +0x40: TimelineEntity's constructor zeroes +0x30..0x48 after the base constructor, so
  the node is not at +0x40 there; unverified), zone streaming foci (`ZoneFocusAdd` takes two pointers,
  not one; what they are is unknown), lights, audio listeners, weather, ropes, AI. See
  "Research for another model" below. The fall threshold needs no API: `ant_live_field` already sets
  `RayCastCollisionProcessorAsset.FallThreshold`.

## Time of day hold, ANT guard, vtables (2026-11-06)
- **API 28 `time_of_day_hold`** (user: lighting is better). The cinematic tools' VisualsManager (shared 2026-11-06)
  writes the same clock as `time_of_day_set` (`[[[g]+8]+0x28]+0x30`) but rewrites it before and after the
  visual environment's update, so the lighting never reads a clock the game moved on. A hold now writes it
  back after each server and each client world update (`timeOfDayTick`, ticks.cpp); the trainer's freeze
  uses it. The visual environment update itself is not hooked (see API 29 above).
- **Body hook** takes exactly the 27-slot vtable at 0x141c3cbf0 (`research/vtables.md`); seen working in game
  (slot 15).
- **ANT live mapping** checks the layout is still what was read before running a type's loader (a crash
  in FallingDriverAsset's loader on 2026-11-06: the layout's memory had changed). When it has moved, the
  layout is looked up again through the registry (hash -> schema -> layout) and used if name and id match.
- Trainer 1.5.4: kill volumes off rescues below y -300 (was -50) and a long fall (state 0, 100 or more
  below the last ground; the game kills those itself). User: both work.
- ANT live mapping: a stale layout is looked up again through the registry (user: works).

## Free camera speed (2026-11-05)
The free camera and `camera behind` cost about 20 fps (165 to 145). Found with the sampler: the update
pass waited on the client thread, which re-ran `hookBody()` every 60 frames; it always refused
("more than 256 entries") after 257 `VirtualQuery` calls, as `vtableSlots` counted executable pages and
every page of the image is executable. Now `vtableSlots` counts pointers into PE code sections, and a
refused vtable is not scanned again. User confirmed the free camera is much better (165 fps); the
third-person body install and `camera behind` fps after the fix are untested. New: console `sample [seconds]`
(sampler.cpp: samples the three busiest threads, logs top functions and the innermost game function on
the stack; compare two runs by address in Ghidra) and console `perf on|off` (a `ui: perf` log line every 5 s per camera mode; off by default). The body hook
no longer measures the vtable (Frostbite vtables run back to back with no RTTI pointer, so a run of
code pointers never ends): it copies 512 slots; `vtableSlots` (camera only) is `countVtableSlots`, host-tested.

## Current task: getting GridNode ready for its first release (plan, 2026-11-04)
Everything up to API 22 is committed (d96c362). The plan below is in order: the "must" items before
a release, "should" before if there is time, the rest after. Open decisions for the user are marked
**(decide)**.

**Must**
1. **Stability on a clean setup.** Unzip the release zip somewhere new (not `D:\Dev\GridNode\bin`),
   inject with `gridnode_inject.exe`, then once more with `version.dll`; with no mods, then each
   example mod. Load the city, the map, a mission, a Dash, a time trial, the menus, a level change,
   quit. Reload and unload each mod a few times. Fix every crash. Quitting during a Dash
   crashed twice with GridNode injected (`0x143164d50`, a null world in `FUN_1439e08f0`'s mission
   clean-up: the game's own code, guarded by GridNode since 2026-11-05): check it in this pass.
2. **Online safety audit.** One list of everything that could reach EA's services or Beat Revival,
   checked in code: the console's online commands (confirmation; mods: refused without
   `online_allow`, enforced in `consoleRunFor` since 2026-11-04), settings marked online (mods
   warned), the trainer's upload block, progression flags marked online (set, logged: user's call,
   many are not online), nothing else sending. A short docs page "GridNode and online play" (modding.md
   has the rule now: "Online services").
3. **What the release is (decide).** Proposed: two zips. *GridNode* for players: the player build
   (no inspector, console, editor, recorder), `gridnode_inject.exe`, `version.dll`, `mods/`. *GridNode
   SDK* for modders: the developer build, `sdk/` (headers, ImGui, Rust crate, templates) and the
   example mods. `gridnode.py package` makes one zip today; add `--player`.
4. **Docs that match the release.** README (its status still says M6/M7 and lists old examples),
   getting started (unzip anywhere and inject; the game folder only for `version.dll`), shipping a
   mod (`mods/<name>/`, `gridnode_mod.py pack`), a troubleshooting page (where the log is, "build
   check failed" = another game build, what a `crash:` block means, how to report one), known issues,
   release notes.
5. **API 1.0.** From the release on the table only grows at its end, and a mod built for API N loads
   on any GridNode with API N or newer (GN_MOD returns 2 otherwise, and the log says so). Before
   freezing: review `api.h` for names and threads we would regret, and mark what is experimental in
   the docs (gravity and body velocities: the city has almost no dynamic bodies; waypoint: street
   height; checkpoint: the table's path, not understood).
6. **Version and how to release (decide).** 1.0.0, or 0.9 as a beta first. A tag, a GitHub release
   from CI's zips, the release notes.

**Should**
7. **The example mods in the release.** Players get the MEC trainer (and hello?). World draw, probe
   and the detour demo are test harnesses: source in the SDK zip only.
8. **The "untested" list.** 29 notes in this file say untested: test, mark experimental, or take out.
9. **A friend's PC.** One install by someone else from the zip and the docs alone.
10. **Cost.** Startup (about 2 s, 2,164 hooks) and frame time with and without GridNode, written down.

**After the release:** spawnable physics objects, the layout editor (paused on purpose), a scripting layer only if
mods ask for one (Lua was skipped on 2026-10-20, PLAN.md), the 335 `hypothesis` functions.

## Research for another model (Ghidra, 2026-11-06)
Each is self-contained: the answer goes into `db/annotations/` and a research page, with the addresses
checked in the decompiler. Claims from the cinematic tools' synthesis are leads, not facts.
1. ~~Class instance lists~~ answered by another model (2026-11-06), partly checked: only spatial entities
   (SpatialEntity and below) have the node at entity +0x40 and a ClassInfo list; `Entity` is 0x30 bytes
   and `TimelineEntity` derives from it directly (no node, its +0x30..0x50 are property connections),
   so its ClassInfo +0x60..+0x78 are 0. A class walk would work for spatial classes only; the list
   head, the node's prev / next order and the end of a walk are still unchecked. `entities_of_class`
   (GridNode's own table of created entities) already finds timelines.
2. ~~ZoneFocusAdd / Remove~~ answered, and already in `research/zone-streaming.md` (2026-10-21): `(focus
   manager = streamer +0x280, key = 0, focus interface)`; the interface's slot 0 `(self, key, out)` writes
   position and direction. The caller bytes in `ZoneStreamerFocusEntity`'s Enable handler were re-read
   2026-11-06 and match. The layout editor keeps 8 focus objects of its own; exposing them as an API
   (`streaming_focus_*`) is the open step.
3. ~~The visual environment update~~ answered by another model, **checked in Ghidra 2026-11-06**: the blend is 0x143703d60,
   called per active context from 0x143705b30 (under the VE mutex); the per-frame hook point is slot 21
   (0x141BFACA8) of the world renderer's vtable (0x141BFAC00), on the render thread, with `worldRenderer +
   0x85470` as the destination. Writing there, before and after the blend, would replace the post-update
   write-back (no flicker). Checked: slot 21 holds 0x14373e5d0, which passes `worldRenderer + 0x85470` to the blend and other calls
   (six uses); 0x143705b30 locks the mutex and loops over the contexts at +0x2358 / +0x2360, as said. Thread not checked.
4. ~~Dynamic lights~~ answered, **partly checked in Ghidra**: creator 0x14375e000 `(ctx, ...)` (allocates 0xf0, constructor
   0x14375f020(mem, ctx + 0x10, *(ctx + 0xd0)), links with 0x14375c4a0; the light data comes from the context,
   not a parameter; its one caller is 0x143766ec0). Vtables are PBRSphereLightEntity / PBRSpotLightEntity.
   update 0x14376a030 sets the dirty byte at entity +0xa2 and queues the entity to the light
   manager (0x14256a3e0): checked. Colour, radius and the other fields: unchecked. Fields: flags +0x80, colour and intensity +0xd8..0xe0, radius and attenuation +0x220.
   Not enough for a `light_create` yet: the light entity data and the world to add it to are missing.
5. ~~Audio listeners~~ answered, **checked in Ghidra 2026-11-06, all as said**: sound manager at 0x1423e29b8 (listener array +0x30 / +0x38);
   add `(mgr, sourceEntity, viewId)` 0x143018a10 (listener object 0x80, entity +0x70, view +0x78, no-op if
   present); remove `(mgr, sourceEntity)` 0x14301d4b0; nearest-listener 0x142fe50b0 takes the listeners'
   transforms via vtable slot 1. Listeners are matched by the entity their vtable slot 6 returns. A second listener on the free camera is
   possible (an entity with a transform is needed). Not built.
6. ~~The long-fall kill~~ answered, **unchecked**: state 0 with `last_ground_y - y >= 100` (or the air time
   running out) starts the death. Cleaner than the trainer's teleport: keep `last_ground_y` at her y while in
   state 0 (`GN_MOVE_LAST_GROUND_Y`). The air-time limit is not found, so this may only cover the distance.
   Not built: the teleport works (confirmed).
7. ~~Swarms and traffic~~ **checked in Ghidra 2026-11-06; the other model had parts wrong.** The class is
   `CrowdPlacementBoidsEntity` (entity +0x28 bus, +0x30 its data); its per-frame step is 0x143b9b7e0 (called
   from 0x143b9bc10 with the crowd component and its dt). In it:
   - **Agent buffer** (component +0x70 begin, +0x78 end): two `float4` per agent, 0x20 bytes, indexed by
     entity +0x44 (first slot) + i, count at entity +0x48. The *even* float4 is the previous position, the
     *odd* one the current (the other model had it the other way). The step copies odd to even, then writes the
     new position from the source record to odd.
   - **Source records**: 0x24 bytes each, from the swarm manager (entity +0x98), index entity +0xa0 + i:
     position x, y, z at +0x00, heading as a half-float in degrees at +0x0c. `y <= -10042` means the agent is
     not spawned (both float4s are set to its position).
   - **Render buffer**: one `float4` per agent (slot entity +0x44 + i): previous position and a half-float
     heading in radians in the last 16 bits.
   - **Density**: a `Float32` property (hash 0xbdd8feb6, its default is the data's +0x78) read every step as a
     percentage; the spawner's reserve (spawner +0x34) is set to `count * (100 - value) / 100`, clamped 0..1,
     and the respawn (0x143b9cbe0) pops a free agent (the pool at spawner +0x40 / +0x48) only while the pool
     is bigger than the reserve. So 100 = every agent active, 0 = none spawn. Writing the data's +0x78 should
     therefore be a density control (untested; it needs a `CrowdPlacementBoidsEntity`'s data from
     `entities_of_class`). The respawn also needs the time at the context +0x14 to pass the spawner's
     +0x30 and rolls a chance per spawn point (0x10-byte records at spawner +0x60 / +0x68).
   - Still unchecked: the agent cap (`SwarmEntityData +0x84`), speed (the behaviour classes 0x141CCEF20 /
     0x141CCEED8 / 0x141CCEEA0 / 0x141CCEE60 and settings 0x141CD2CA8 are only vtables here), and how many
     of these entities a level has.
8. The 377 `hypothesis` functions (the count was 335 above; the other model counted 377): resources 98, render 64,
   levels 47, reflection 36, startup 32, input 20, 13 other files 80. Still to check, area by area.

From item 2 (checked): the game's `ZoneStreamerFocus_remove` writes out of bounds when the record is missing (index 0xffffffff). GridNode never
calls it: `focusApply` sets the removed flag (bit 0) on a record it found first.

## Done earlier
Everything before 2026-11-06 (the Volumes tab, what was checked in game 2026-11-02, the long list of
additions up to API 27) is in [progress-archive.md](progress-archive.md).

## Older next steps (research; after the release plan above)
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
  Online services (user, 2026-11-04): mods may change them on purpose after `online_allow` (warned,
  logged, shown in the Mods list); the trainer never does, and keeps its upload block. PC only.
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
