# Progress / handoff

Update at the end of each session. Keep it short. The full handover for a new agent is
[handover.md](handover.md).

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
1. **Class instance lists.** The synthesis says every ClassInfo keeps four instance lists at
   `+0x60 / +0x70 / +0x80 / +0x90` with the node at entity `+0x40`. TimelineEntity's constructor
   (0x14368d590) zeroes `+0x30..+0x48` after the base constructor (0x143160100), so the node cannot be at +0x40
   there. Find the code that links an entity into its class's list (the base constructor, or an
   `addToClassList` call) and the real node offset, the list head and how a walk ends. Payoff: every
   door, vehicle or trigger of a class, including ones made before GridNode started.
2. **ZoneFocusAdd / Remove** (0x143c7e7c0, 0x143c81390). They take the streamer and two pointers, and store
   24-byte records (+0x00, +0x08, +0x10 = -1, flag in +0x14). Find the callers: what the two pointers
   are (a position and a radius? a focus object and its owner?) and where the streamer is kept. Payoff: stream
   the world around the free camera or a far teleport.
3. **The visual environment update.** Find the function that blends the state (0x143703d60 per the
   synthesis, unchecked) and its callers; say which one runs once per frame on which thread, and where the
   render reads the state. Payoff: a hook point to write held values before and after, as the cinematic tools do.
4. **Dynamic lights.** SphereLightEntity (vtable 0x141bff178) and SpotLightEntity (0x141bff240): how to make one
   at runtime (creator, entity data, the world to add it to), the update at 0x14376a030, and the light data
   fields. Payoff: a `light_create` API.
5. **Audio listeners.** `0x143018a10` (add) and `0x14301d4b0` (remove), the nearest-listener resolver
   `0x142fe50b0`. Payoff: sound that follows the free camera and Faith at once.
6. **The long-fall kill.** Faith dies after falling uncontrolled for a while even with kill volumes off and
   FallThreshold raised: find what kills her (state 0, distance or time) and where. Payoff: a clean
   `no fall death` without the trainer's teleport back.
7. **Swarms and traffic.** `CrowdBoidsComponent` agent lists (+0x70 / +0x78, 0x20 bytes an agent): confirm, and
   find a density or speed control.
8. **The 335 `hypothesis` functions** left in the annotations: check them area by area.

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
