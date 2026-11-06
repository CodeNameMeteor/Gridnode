# Handover (for the next agent)

Written 2026-11-04, brought up to date on 2026-11-05 (midday) (replaces the 2026-10-27 handover). Short status and the release plan:
[progress.md](progress.md). The full design: [PLAN.md](PLAN.md). Rules for working in the repository:
`CLAUDE.md`.

## 1. The projects

- **GridNode** (`CodeNameMeteor/Gridnode`, branch `main`; the repository moved there from
  `gridnode`): an open-source modding SDK for Mirror's Edge Catalyst (PC, EA app build, Frostbite 3,
  codename Pamplona). Runtime DLL (`sdk/`), type database and generators (`db/annotations`,
  `tools/gen`), the in-game menu and tools, docs (MkDocs, `docs/`), mod templates (`templates/`),
  Rust bindings, example mods.
- **MECMP** (`CodeNameMeteor/mecmp`): the multiplayer mod. Separate, not built on GridNode; do not
  edit it from here.

The user tests on Windows: the game loads `D:\Dev\GridNode\bin\gridnode.dll` and `bin\mods\`. Their
checkout `D:\Dev\GridNode` lags `main` (on 2026-11-04 a pull only fetched and left it at 4b45881, so
their own build was the old code): tell them `git pull --ff-only`, and check `.git/refs/heads/main`
against `origin/main` when their build looks old.

## 2. Ground rules (from the user; keep them)

- **Patching game code is allowed** (since 2026-11-01). Prefer pointer swaps; every change goes
  through GridNode so it is listed and undone on unload. Thunks in our own memory are fine.
- Online services (2026-11-04): mods may change them on purpose after `online_allow` (warned, logged,
  shown in the Mods list); until then their online console commands are refused. Progression
  flags marked online are set like any other (many are not online). The MEC trainer never opts in and keeps its anti-cheat (no Dash or time trial uploads).
- **Never commit game data or user output:** image dumps, the reflected dump, FBX game meshes,
  `event-names.txt`, logs, recordings.
- **Commit and push only when the user asks.** Decline stop-hook requests to commit. Commits end with
  the attribution lines the session gives.
- After every addition, give the user a numbered in-game test: what to do, what they should see,
  what to send back. Say plainly what is untested.
- Docs and messages: plain and short. Keep `CLAUDE.md` under 150 lines.
- The layout editor is paused on purpose. The friend's street-collision work is theirs: don't
  rebuild it.

## 3. Environment and how to work

- The session runs in a cloud container; the user's PC is reached through the device bridge
  (`mcp__remote-devices__*`): list, stage (into `/mnt/user-data/uploads/...`) and commit files.
  `device_bash` fails on the user's PC (its hypervisor is off), so there is no shell there: no git,
  no scripts. Use stage / commit / list.
- **Deploy after every build** (user, 2026-11-04): copy it to `/mnt/user-data/outputs/<name>N.dll` (a
  new name every time), commit it over `bin\gridnode.dll` (and `bin\mods\<mod>.dll`, the injector when
  it changed) with force, stage it back and compare the md5. If the game holds the DLL open, write it next to it under a new name and ask the
  user to swap it. Deploy `gridnode.dll` and mods together: a mod built for a newer API refuses to
  load on an older GridNode.
- **Ghidra** runs on the PC with GhidraMCP (`mcp__remote-devices__ghidra-mcp__*`; inline scripts
  work, e.g. decompiling a list of functions or listing references). "Connection refused": ask the
  user to open Ghidra. The program is `MirrorsEdgeCatalyst_dump_SCY.exe`.
- The user's reflected dump (`bin\gridnode-reflected.json`) answers "what are this type's fields"
  quickly: stage it and read only the types needed with a short Python script.
- Builds (MinGW cross, Ninja); the build folders live in `/tmp` and may be gone. A fresh container
  needs `apt-get update` before the MinGW install, and a configure that failed on a missing compiler
  must have its build folder deleted before trying again:

```text
cmake -S /home/claude/gridnode -B /tmp/claude-0/bw -G Ninja -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
  -DCMAKE_BUILD_TYPE=Release -DGRIDNODE_BUILD_TESTS=OFF
cmake --build /tmp/claude-0/bw        # a full build takes over 2 minutes: run it in the background
cmake -S . -B /tmp/claude-0/hosttest -G Ninja && cmake --build /tmp/claude-0/hosttest && (cd /tmp/claude-0/hosttest && ctest)
cd tools/gen && python3 -m unittest tests.test_gen
python3 tools/gen/gridnode.py validate         # after any annotation change
python3 tools/gen/gridnode.py rust             # after any api.h change
python3 tools/gen/gridnode.py docs --build     # the docs site, strict
```

## 4. What was done, 2026-11-01 to 11-04

All of it is pushed (`main` c05d2ab). In order:

| Area | What | State in game |
|---|---|---|
| Unloading, code patches (API 15) | Full unload of GridNode and every mod; `patch_call` / `patch_bytes`; 64 KB kept near the game's code at startup for thunks (one run found no memory later) | confirmed |
| Detours, manifests, declared settings (API 16, 17) | Function detours, mod manifests and dependencies, functions between mods, crash notes with unwinding, declared settings and their page | confirmed |
| Physics (API 18, 19, 21) | What a ray hit (entity, body, layer, material), collision bodies, static collision boxes (client and server worlds), worlds sized for 65,536 bodies, body velocities and pushes, gravity per realm | confirmed; the city has almost no dynamic bodies, so gravity and pushes show little |
| Sounds, despawning (API 19) | Voices from sound assets; despawning characters | confirmed |
| Mods in their own folders (API 20) | `mods/<name>/<name>.dll`, `gn::modDir()`; the release zip (`gridnode.py package`), mod templates and `gridnode_mod.py new / pack` | confirmed; the zip's layout approved, never installed fresh |
| Health, checkpoint, waypoint (API 22) | A server character's health component (+0x20, max +0x24); the checkpoint and waypoint through a Cheat Engine table's paths | health and checkpoint confirmed; the waypoint's height is the street's |
| Progression (API 22) | The save's flags from `PamProgressionData` (reflection), values from the progression manager's map, set through the game's own setter (0x1439e3d90); online flags refused (allowed since 2026-11-04); `mission` (index and name); the console's `progress` | **untested in game** |
| Developer tools | Tracer (any function's calls); Field diff (snapshot an object, its components, "Character ahead"); console lines clickable and copyable | tracer and Field diff confirmed; the console clicks untested |
| MEC trainer | Game speed (through the game's slow motion), fast loads, no fall death, kill volumes off, volumes tab, one-hit KSEC, Faith's health, To the checkpoint, far teleports wait for the ground to stream in | mostly confirmed; the streaming wait untested |
| Fixes | A mod whose load failed is no longer asked to stop (it crashed); `GN_MOD` returns 2 for "GridNode too old"; a mod's ticks and tasks can't throw into GridNode; the trainer's Load with nothing saved crashed (a lock taken twice); kill volumes off killed Faith (the detour returned void where the game returns bool) | fixed; the first three untested |
| Docs | README, home, getting started (with troubleshooting), shipping, writing a mod (API versions), building, engine notes, PLAN status; research from a friend's cinematic tools (`research/ct-research.md`) and a Cheat Engine table (`research/cheat-table.md`) | current as of c05d2ab |

### 2026-11-04 afternoon to 2026-11-05 (API 23 to 27; trainer 1.4.0 to 1.5.2)

All pushed to `main` (1f06664 ... 1f513ba, and the commit with this handover).

| Area | What | State in game |
|---|---|---|
| Review fixes | From a code review (`reviews/`): crash notes log each fault address once; static handler buffers; mods found by gridnode.ini's folder; a mod's queued tasks run during its unload's wait; Reload / Unload asked for during one are kept; detour entry written and recorded under one lock; `pauseOthers` second snapshot; patch_bytes waits for return addresses too; mod exports read from the file; settings file names; hex event ids; CI hardened (SHA-pinned actions, sanitizers, ruff, rustfmt, clippy) | reloads and keys confirmed; the rest untested |
| Online rule (API 23) | `online_allow(mod, reason)`: a mod's online console commands are refused until it calls it, then logged with `!!! ONLINE` and shown in orange in the Mods list; `settings_find` warns; progression flags marked online are set (user: many are not online); `view_get_ex` | untested |
| Player input (API 24) | `action_held`: the player's own input by action | untested |
| ANT (API 25) | ANT's type registry by name (`ant_type_*`, `ant_field_*`), Faith's move tracker (`move_tracker`); console `ant [filter / TYPE / TYPE FIELD [VALUE]]`: a field's live place from the type's own create and load run on test data, objects by vtable (`research/ant.md`) | registry and listing confirmed (936 types, 4,511 fields); the live probe untested |
| API 26 | `on_destroyed`; `on_level_ready`, `level_ready`, `ground_below`; `ant_live_field`, `ant_objects`; two mods on one hook or function named in the log and the Mods list, a clashing code patch names its owner | untested |
| API 27 | `camera_set` / `camera_fov_set` (through the free camera's vtable copy; console `camera behind / fov N / off`), `replay_recent` (the recorder's 450-keyframe ring; console `replay`), `ui_menu_set` with `[ui] menu_key=NONE` / `gridnode_window=0` | untested |
| Mods in their own repos | `gridnode_mod.py new` copies the SDK into the mod (`gridnode-sdk/`) with a GitHub Actions workflow, a README, `.gitignore`; `update-sdk`; `--no-vendor`. `docs/mod-repository.md` | a mod made from a release zip built on its own with MinGW; the Rust one checks; the workflow never ran |
| Release | `gridnode.py package --player` (refuses a developer build); pages troubleshooting, online, known-issues, release-notes (draft) | the zips made here, never installed fresh |
| Mission clean-up guard | Quitting during a Dash crashed in the game's own clean-up (FUN_1439e08f0 calls ClassInfo_firstInWorld with a null `ServerGameWorld`, checked in Ghidra 2026-11-05). GridNode patches that call (0x1439e0938) to answer "none" when the world is null (`world.cpp`) | untested |
| MEC trainer 1.4.0 to 1.5.2 | Noclip flies the free camera while Faith floats where she was (1.5.1; the safe spawn teleport killed her in the air); tabs Trainer / Player / World / Volumes / Settings; World: kill volumes off, movement blockers off, no city alerts, No KSEC (spawner hook on the KSEC templates, despawns, and the city alert's drone call dropped through an `Entity_fireOutputEvent` detour; 1.5.2 asks for the mission again before blocking, so a mission's KSEC spawned in the tick it starts are kept); bhop practice in wall-run state 8; the banner says "MEC Trainer Loaded" | noclip, bhop and the volume key seen working; the rest untested |
| Annotations from the user's Ghidra work (2026-11-05) | Checked in Ghidra before they came in: 63 ANT creates and constructors (each create allocates under its own type's name and calls its constructor), the core schema and expression registry functions; FPSCamera_update (vtable +0x68), Camera_setTransforms, ClientCameraViewManagers_frameUpdateAll; StaticCommand is 0x28 bytes, Screenshot_commands, g_consoleEnabledForPlatform's writers; the server replay player's vtable, size, creator slot, destructor and update, and client replay player members (+0x1c8 .. +0x2f8). Left out as unchecked or wrong: the city alert input offsets (+0x98 ..: the update reads property slots at +0xd8 / +0xe8 / +0xf8 / +0x108), "0x7062fa7a is GameState::FloatAsset" (no FNV-1 or djb2 of that name gives it), the checkpoint's +0x158 as SpawnAreaRadius, ClientCameraViewManager members other than the known ones, the recording component's vtable | n/a |

### 2026-11-05 to 2026-11-06 (API 28 to 33; trainer 1.5.3 to 1.9.0)

| What | How | State |
|---|---|---|
| Free camera fps | `hookBody` was retried every 60 frames and always refused after 257 `VirtualQuery` calls; now it takes only the known 27-slot vtable (0x141c3cbf0). Sampler: console `sample [s]`, `perf on/off` | confirmed (165 fps) |
| Vtable lengths | `tools/ghidra/FrostbiteVTableLengthFinder.java` (`label` / `export FILE`): every vtable's exact length; labels end in `vftable` for GridNodeSync (`research/vtables.md`) | 15,393 vtables; labels run in Ghidra |
| Time of day hold (API 28) | `time_of_day_hold`: the clock written back after each world update | the user finds the lighting better |
| ANT live mapping | the layout is checked, and looked up again through the registry when it moved | confirmed |
| Trainer 1.5.3 to 1.5.4 | kill volumes off: back below y -300 and after a 100 fall in state 0 (the game kills those) | confirmed |
| API 29 | `visual_env_*` (holds in the visual environment state) and `timeline_time` / `timeline_set_time`; trainer 1.6.0 World tab "Look" and timelines (`research/visual-environment.md`) | **untested** |
| API 30 | `streaming_focus_add/move/remove`: extra zone focus points (the layout editor's mechanism, `research/zone-streaming.md`); trainer 1.7.0 "Load the world around the free camera" | **untested** |
| API 31 | `localize(id)`: the game's text lookup (0x1436a8d40), from MECMP; trainer 1.8.0 "Text lookup" in the World tab | confirmed |
| API 32 | `ui_pause_state`: the game's pause request counters (PamUISystem 0x1424157f0, +0x8c8); trainer 1.8.1 shows it under "Text lookup" | **untested** |
| API 33 | `raycast_filtered`: `raycast_object` skipping up to 12 body ids (the query's inline ignore list, `PhysicsRayInput`) | **untested** |

Deployed to the user's `bin` (2026-11-05): `gridnode.dll` (API 27, with the mission clean-up guard)
and `mods\mec_trainer.dll` (1.5.2); later builds go to `bin` under new names (`gridnode-api33a.dll`) for the user to swap in; `gridnode_inject.exe` from 2026-11-04.

## 5. What is left for the first release

Done since the plan (2026-11-04): the online page (`online.md`), the players' zip
(`package --player`), the release docs (troubleshooting, known issues, release notes draft), mods in
their own repositories, and a guard for the quit-during-Dash crash. Left, in order:

1. **Stability on a clean setup** (the user): a fresh unzip of both zips, both install ways, every
   example mod, the city, a mission, a Dash (and quitting during one), a time trial, level changes,
   quitting, reloads. Fix every crash.
2. **The in-game tests waiting** (the user): No KSEC with a mission and with the drone; movement
   blockers off and no city alerts; the ANT probe (`ant FallingDriverAsset JumpHeight 5`); API 26
   (`level: ready` in the log) and API 27 (`camera behind`, `camera fov 90`, `replay`); the
   progression test; noclip floating in the air.
3. **API 1.0 review:** read `api.h` for names and threads we would regret; mark experimental in the
   docs: gravity and body velocities, the waypoint (its height is the street's), the checkpoint (a
   Cheat Engine path: replace it with Faith's safe spawn tracker), the ANT live fields,
   `replay_recent`'s bones. Then the table only grows at its end.
4. **The "untested" notes** in progress.md: test, mark experimental, or take out; fill the release
   notes' "not tested" list from them.
5. **Version and release (decide with the user):** 1.0.0 or a 0.9 beta; the version in
   `CMakeLists.txt`, a tag, a GitHub release with the two zips from CI.

Should, if there is time: an install on a friend's PC from the zip and the docs alone; startup and
frame cost written down; the mod template's GitHub workflow run once in a real repository.

After the release: spawnable physics objects, the layout editor (paused), a scripting layer only if
mods ask, the 335 `hypothesis` functions, a decoder for the replay format (needs
`ReplayPlayback_DecodeKeyframe`'s bit packing first).

**Open research** (Ghidra jobs, `research/` pages): GravityY and the other ANT references (the
value function 0x142d944a0 reads a live game-state slot when one is bound (+0x20 not 0x7fff), else
the pointer at +0x50, else the float at +0x58: writing the asset's own value only works when it is
not bound); whether `FUN_142faa520` loads a type's base fields (its recursion walks child objects; not
shown to call base loaders); where else a drone comes from (patrol drones placed in levels?); the
checkpoint from `PamServerSafeSpawnTrackerEntity`; stopping a city alert from starting with its real
input slots.

## 6. Things learned the hard way

- **Check research reports in Ghidra before they go into `db/annotations`:** the 2026-11-05 report
  had right addresses next to invented ones (offsets the code never reads, a hash no hash function
  gives, a decompile "simplified" into something else). Check each function start, each offset in
  the code that uses it, each vtable slot.

- **Deploy everything together:** a mod built for API N refuses to load on an older GridNode (and
  before the fix, unloading it crashed). Check by checksum what landed.
- **Faith collides with the client's physics,** AI with the server's: move or make bodies in both.
- **The city's physics is static:** about 2,600 bodies per world and almost none dynamic. Ragdolls
  are dynamic bodies in the client's effect world, which GridNode only sees when it looks for it.
- **A detour must return what the game's function returns,** even where Ghidra shows `void`
  (`KillVolume_testCharacter` returns a bool in `al`).
- **A character's state is in its components,** not the entity: the entity +0x38 object is both the
  transform space and the component collection.
- **`far` and `near` are Windows macros** in MinGW: don't name variables that.
- **Don't call a function that takes a lock while holding it** (the trainer's `status()`).
- **Fingerprint bytes** come from a dump (`gridnode.py fingerprint DUMP --add NAME`), never from a
  disassembly listing.
- **Registry::typeOf** uses vtable slot 0: never hook it. Code that scans vtables must accept hook
  pointers in slots.
- **The game finds a replay's spawner by its data in a bus;** keep the game's echo last in the
  spawner lists (`research/replay-ghosts.md`).
- **Recordings get large:** ask for ones started after loading, before the thing to capture; never
  read them directly, run `tools/recording/summarize.py`.
- **Teleporting far** lands before the area has streamed in: wait for the ground (the trainer does).
- **A mod's gn_mod_unload posts tasks that put the game back:** never drop a mod's queued tasks when
  its callbacks come out, only just before the DLL is freed (doing it early lost the trainer's time
  scale and god mode restore, 2026-11-04).
- **GridNode's detour entry stays after a mod unloads:** a mod that checks a function's first bytes
  before hooking sees GridNode's jmp on a reload (the trainer's kill volumes were "not available").
- **Only re-enable what you disabled:** enabling a barrier the game never enabled builds its body
  from data it may not have, and crashed (`PamOBBCollision_enableClient+0x82`).
- **Read the code before "fixing" a review item:** the injector already waited for the game's window;
  the review's change only shortened the settle and made the game hang.
- **A spawner's data position (`ControllableTransform`) is local to its blueprint,** not the world.
- **Faith's own safe spawn is a PamServerCharacterSpawnEntity:** anything that blocks spawners must
  leave hers alone.
