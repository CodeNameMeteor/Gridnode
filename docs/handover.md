# Handover (for the next agent)

Written 2026-11-04, brought up to date on 2026-11-05 (replaces the 2026-10-27 handover). Short status and the release plan:
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

### The afternoon of 2026-11-04 (API 23, 24; trainer 1.4.0)

Pushed: 1f06664, 9608164, 31afe2d, 1add893, 59efbcb, and the KSEC commit after them.

| Area | What | State in game |
|---|---|---|
| Review fixes | From a code review (`reviews/`): crash notes log each fault address once (handled faults used up a cap of 3); static handler buffers; mods found by gridnode.ini's folder; a mod's queued tasks run during its unload's wait and only leftovers are dropped before the free; Reload / Unload asked for during one are kept; detour entry written and recorded under one lock; `pauseOthers` second snapshot; patch_bytes waits for return addresses too; mod exports read from the file (`pe_file.cpp`); settings file names; hex event ids; CI hardened (SHA-pinned actions, sanitizers, ruff, rustfmt, clippy) | reloads and keys confirmed after the task fix; the rest untested |
| Online rule (API 23) | `online_allow(mod, reason)`: a mod's online console commands are refused until it calls it, then logged with `!!! ONLINE` and shown in orange in the Mods list; `settings_find` warns. Progression flags marked online are set by default (user: many are not online). The trainer never opts in. `view_get_ex` (sized `gn_view`); `gn_ray_hit` is fixed | untested |
| Player input (API 24) | `action_held`: the player's own input by action (the live EntryInputState) | untested |
| Free camera keys | Top right; mods add rows (`gn::ui::freecamRow`, `freecamSeparator`) | untested |
| Injector | The 3 s settle after the game's window is back (a review change had cut it to 0.5 s; the game hung when its window was not opened) | untested |
| MEC trainer 1.4.0 | Noclip flies the free camera, Faith waits at her safe spawn and lands at the camera, last ground -300; tabs Trainer / Player (Faith's health and position, movement tuning) / World / Volumes / Settings (keys, HUD, unload); World: kill volumes off, movement blockers off (each barrier's own disable / enable, both realms), no city alerts (the manager's deactivate event, outside missions), no KSEC (below); bhop practice in the middle of the screen for wall run jumps (state 8; ON TIME / EARLY / LATE / MISSED from the player's own jump); key 3 shows trigger volumes; the banner only says "MEC Trainer Loaded". Removed: no fall death, one-hit KSEC, no stumble, To the checkpoint | noclip, safe spawn, bhop and the volume key seen working in the user's log; the rest untested |
| No KSEC (free roam) | KSEC are spawners whose template is `Characters/SpawnTemplates/Enemy/...` (from the trainer's character log: team is 2 for everyone). A slot hook on the spawner's spawn (vtable +0xd0) returns null for them while no mission runs, and the ones out are despawned; Faith's own spawner is never touched (`research/ksec-spawning.md`) | **untested in game** |
| ANT (API 25) | ANT's type registry by name (936 types, 4,511 fields, read in game), `ant_type_*` / `ant_field_*`, Faith's move tracker (`move_tracker`, trainer's Player tab); console `ant [filter | TYPE | TYPE FIELD [VALUE]]`: a field's live place from the type's own create and load run on test data, objects by vtable (`research/ant.md`) | registry and listing confirmed; the probe untested |
| API 26 | `on_destroyed` (in GridNode's destructor wrapper); `on_level_ready`, `level_ready`, `ground_below`; `ant_live_field`, `ant_objects`; mods on one hook or function named in the log and the Mods list, a clashing code patch names its owner | untested |
| API 27 | `camera_set` / `camera_fov_set` (a mod's camera, through the free camera's vtable copy), `replay_recent` (Faith's last 450 keyframes), `ui_menu_set` with `menu_key=NONE` / `gridnode_window=0`; mod template copies the SDK into the mod with CI (`update-sdk`); `package --player`; pages mod-repository, troubleshooting, online, known-issues, release-notes | untested in game; the template build checked with MinGW |
| MEC trainer 1.5.1 | Noclip: Faith floats where she was (the safe spawn teleport killed her in the air). No KSEC: also drops the city alert's drone call (`Entity_fireOutputEvent` detour; the drone is a `PamServerDroneActorEntity`, not a character) | untested |

Deployed to the user's `bin` (2026-11-05): `gridnode.dll` (API 27) and `mods\mec_trainer.dll`
(1.5.1); `gridnode_inject.exe` from 2026-11-04 (the 3 s settle back).

## 5. What is planned: the first release

In order (details in [progress.md](progress.md), "Current task"):

1. **Stability on a clean setup:** a fresh unzip, both install ways, every example mod, the city, a
   mission, a Dash, a time trial, level changes, quitting, reloads. (Quitting during a Dash crashed
   twice with GridNode injected, `0x143164d50`; the user believes it is fixed, 2026-11-05: check
   it in that pass.)
2. **Online safety audit** and a docs page on it. The rule is now enforced in code (`online_allow`,
   API 23) and described in modding.md ("Online services"); what is left is the list and the page.
3. **The release's zips** (decide with the user): a player zip (the player build, injector,
   `version.dll`, the MEC trainer) and an SDK zip (developer build, headers, templates, examples).
   `gridnode.py package` makes one zip today; add `--player`.
4. **Docs for the release:** known issues and release notes (the rest was brought up to date on
   2026-11-04).
5. **API 1.0:** review `api.h`, mark the experimental parts (gravity, body velocities, waypoint,
   checkpoint), then the table only grows at its end.
6. **Version and release** (decide with the user: 1.0.0 or a 0.9 beta): a tag and a GitHub release
   from CI's zips.

Then, if there is time: the example mods in the release, the 29 "untested" notes in progress.md, an
install on a friend's PC, startup and frame cost written down.

After the release: spawnable physics objects (GridNode-made dynamic bodies, so gravity and pushes do
something), the layout editor, a scripting layer only if mods ask (Lua was skipped on 2026-10-20),
the 335 `hypothesis` functions.

Waiting on the user: the No KSEC test (free roam, a runner nearby, a mission with KSEC), movement
blockers off and no city alerts in game, the bhop practice's early / late results, the progression
test (`progress`, setting a flag that is marked online), the console clicks, whether the 16:09 run on
2026-11-04 (log ended after "level: ended") was a quit or a crash, and the two decisions above.

Open research: whether a mission spawns its KSEC before the mission index is set (No KSEC would stop
those); stopping an alert from starting instead of ending it (the manager's pause / force-off inputs,
`research/city-alert.md`).

## 6. Things learned the hard way

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
