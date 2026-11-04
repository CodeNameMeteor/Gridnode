# Handover (for the next agent)

Written 2026-11-04 (replaces the 2026-10-27 handover). Short status and the release plan:
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
checkout `D:\Dev\GridNode` was 27 commits behind on 2026-11-04 (at 4b45881) and needs `git pull`.

## 2. Ground rules (from the user; keep them)

- **Patching game code is allowed** (since 2026-11-01). Prefer pointer swaps; every change goes
  through GridNode so it is listed and undone on unload. Thunks in our own memory are fine.
- Leave online services and Beat Revival alone, except the MEC trainer's anti-cheat (it blocks Dash
  and time trial uploads while loaded). Progression flags that sync to online stats are refused.
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
- **Deploying a test build:** copy it to `/mnt/user-data/outputs/<name>N.dll` (a new name every
  time), commit it over `bin\gridnode.dll` (and `bin\mods\<mod>.dll`) with force, stage it back and
  compare the md5. If the game holds the DLL open, write it next to it under a new name and ask the
  user to swap it. Deploy `gridnode.dll` and mods together: a mod built for a newer API refuses to
  load on an older GridNode.
- **Ghidra** runs on the PC with GhidraMCP (`mcp__remote-devices__ghidra-mcp__*`; inline scripts
  work, e.g. decompiling a list of functions or listing references). "Connection refused": ask the
  user to open Ghidra. The program is `MirrorsEdgeCatalyst_dump_SCY.exe`.
- The user's reflected dump (`bin\gridnode-reflected.json`) answers "what are this type's fields"
  quickly: stage it and read only the types needed with a short Python script.
- Builds (MinGW cross, Ninja); the build folders live in `/tmp` and may be gone:

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
| Progression (API 22) | The save's flags from `PamProgressionData` (reflection), values from the progression manager's map, set through the game's own setter (0x1439e3d90); online flags refused; `mission` (index and name); the console's `progress` | **untested in game** |
| Developer tools | Tracer (any function's calls); Field diff (snapshot an object, its components, "Character ahead"); console lines clickable and copyable | tracer and Field diff confirmed; the console clicks untested |
| MEC trainer | Game speed (through the game's slow motion), fast loads, no fall death, kill volumes off, volumes tab, one-hit KSEC, Faith's health, To the checkpoint, far teleports wait for the ground to stream in | mostly confirmed; the streaming wait untested |
| Fixes | A mod whose load failed is no longer asked to stop (it crashed); `GN_MOD` returns 2 for "GridNode too old"; a mod's ticks and tasks can't throw into GridNode; the trainer's Load with nothing saved crashed (a lock taken twice); kill volumes off killed Faith (the detour returned void where the game returns bool) | fixed; the first three untested |
| Docs | README, home, getting started (with troubleshooting), shipping, writing a mod (API versions), building, engine notes, PLAN status; research from a friend's cinematic tools (`research/ct-research.md`) and a Cheat Engine table (`research/cheat-table.md`) | current as of c05d2ab |

Deployed to the user's `bin` (2026-11-04): `gridnode.dll` and `mods\mec_trainer.dll` from d96c362 plus
the console change; `mods\world_draw.dll` is older (API 21, it still loads). `bin\gridnode-api22b.dll`
is a leftover the user can delete.

## 5. What is planned: the first release

In order (details in [progress.md](progress.md), "Current task"):

1. **Stability on a clean setup:** a fresh unzip, both install ways, every example mod, the city, a
   mission, a Dash, a time trial, level changes, quitting, reloads. Includes the open crash: quitting
   during a Dash crashed twice with GridNode injected (`0x143164d50`, a null world in
   `FUN_1439e08f0`'s mission clean-up).
2. **Online safety audit** and a docs page on it.
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

Waiting on the user: the progression test (`progress`, `progress <filter>`, setting a flag, the
mission name), the console clicks, the trainer's streaming wait, mod reloads after the unload fix,
and the two decisions above.

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
