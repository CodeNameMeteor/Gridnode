# Handover (for the next agent)

Written 2026-10-27. Short status: [progress.md](progress.md). The full plan: [PLAN.md](PLAN.md).

## 1. The projects

- **GridNode** (`CodeNameMeteor/gridnode`, branch `main`): an open-source modding SDK for Mirror's
  Edge Catalyst (PC, Frostbite 3, codename Pamplona). Runtime DLL (`sdk/`), type database and
  generators (`db/annotations`, `tools/gen`), the in-game menu (inspector, console, layout editor,
  recorder), docs (MkDocs, `docs/`), tools (`tools/recording`, `tools/ghidra`, `tools/mesh`).
- **MECMP** (`CodeNameMeteor/mecmp`): the multiplayer mod (other players as ghosts). It is a separate
  mod, not built on GridNode; both load into the game together (Nollz's PR #2 made the hooks chain
  in either order). Contributors open PRs there too (yamixdev, TheNollz).

The user works on Windows; their game folder setup is `D:\Dev\GridNode\bin` (the game loads
`gridnode.dll` from there) and `D:\Dev\MECMP\bin` (`mecmp.dll`, `mecmp.ini`, `mecmp.log`).

## 2. Ground rules (from the user; keep them)

- **Patching game code is allowed** since 2026-11-01 (the user; before: pointer swaps only). Prefer
  pointer swaps (vtable slots, creator slots, data); every change goes through GridNode so unload can
  undo it. Writing our own thunks in our own memory is fine.
- Leave online services and Beat Revival alone. PC only.
- **Never commit game data or user dumps:** image dumps (`bin/dumps/*.exe`), FBX game meshes,
  `event-names.txt`, logs, recordings, the reflected dump.
- **Commit and push only when asked.** Commits end with the attribution lines the session gives.
- Deploy test builds to the user's `bin` folder; stage them under a unique name first
  (`/mnt/user-data/outputs/gridnode-<what>N.dll`), then copy over `gridnode.dll` with force. If the
  game has the DLL open, write it next to it under a new name and tell the user to swap it.
  Check the file that landed (size) before saying it is there: once a failed build's old DLL went out.
- Docs and messages: plain, short, consistent with the existing docs. The user tests in game and
  sends logs and recordings; say clearly what is untested.
- The layout editor is paused on purpose (the user wants SDK and research work first, then back to
  the editor near the end).

## 3. Environment and how to work

- The session runs in a cloud container. The user's PC is reached through the device bridge
  (`mcp__remote-devices__*`): list, stage (copy to `/mnt/user-data/uploads/...`) and commit files.
  `device_bash` does not work in `D:\Dev\GridNode\bin`; use stage / commit / list.
- **Ghidra** runs on the user's PC with the GhidraMCP plugin (tools `mcp__remote-devices__ghidra__*`
  and, newer, `ghidra-mcp__*` that can also run scripts, create functions and set bookmarks). If a
  call says "connection refused", ask the user to open Ghidra / enable the plugin.
- GitHub: `gh` works over REST only (`gh api repos/...`; GraphQL is blocked). PRs are created with
  `gh api repos/OWNER/REPO/pulls --input body.json`.
- MECMP clone at `/home/claude/mecmp`: remote `origin` = CodeNameMeteor/mecmp, `reglitched` = a fork;
  the local `main` tracks the fork, so work from `origin/main` (new branch per PR).
- Builds (MinGW cross, Ninja). The build folders live in `/tmp` and may be gone; recreate them:

```text
# GridNode (in /tmp/claude-0/bw)
cmake -S /home/claude/gridnode -B /tmp/claude-0/bw -G Ninja -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
  -DCMAKE_BUILD_TYPE=Release -DGRIDNODE_BUILD_TESTS=OFF
cmake --build /tmp/claude-0/bw            # gridnode.dll; a full build can take over 2 minutes
# GridNode host tests (Linux): cmake -S . -B /tmp/claude-0/hosttest -G Ninja && ctest
# Python tests: cd tools/gen && python3 -m unittest tests.test_gen
# MECMP (in /tmp/claude-0/mbuild), same toolchain flags; for the user's solo tests add
#   -DMECMP_DEBUG_KEYS=ON -DMECMP_VERBOSE_LOG=ON  (F7/F9/F12, solo test join=127.0.0.1)
cmake --build /tmp/claude-0/mbuild --target mecmp
```

  The GridNode build generates `engine.hpp` from `db/annotations` (needs PyYAML).
- `python3 tools/gen/gridnode.py validate` after any annotation change.

## 4. Where things stand (2026-10-27)

All work is pushed (GridNode `main` 14507f0). MECMP PRs #102 to #106 are merged. Open elsewhere:
GridNode PR #1 (an old code review), MECMP #112 (yamixdev) and #95 (Nollz, WIP PVP), not ours.

Recent features (details in progress.md and the docs):

| Feature | Where | State |
|---|---|---|
| Game image dump (menu → Game image) | `sdk/src/mem.cpp` `dumpImage`, `sdk/src/ui.cpp` | confirmed in game |
| `gridnode.py fingerprint` (GridNode + `--mecmp`) | `tools/gen/gridnode_gen/fingerprint_gen.py` | checked against a real dump |
| Recorder: trace a class's virtual calls | `sdk/src/recorder.cpp` (`traceClass`, `writeThunk`, `callerStack`) | confirmed in game, stacks too |
| Recorder: watch fields, send an event, callers | `sdk/src/recorder.cpp` | built 2026-10-22, lightly used |
| `annotations-diff`, `ghidra-import --who`, `ghidra-relay` | `tools/gen/gridnode_gen/` | tested in Python |
| GridNodeSync comment blocks and comment export | `tools/ghidra/GridNodeSync.java` | **never compiled / run in Ghidra** |
| Barriers and magropes in the layout editor (save, copy) | `sdk/src/layouts.cpp` | moving confirmed; save/copy untested |

Research notes worth reading first: `docs/research/replay-ghosts.md` (ghost spawning, the map's echo),
`city-alert.md`, `startup-sequence.md`, `barriers-and-magropes.md`, `render-objects.md`.
Annotations for the spawner / replay work: `db/annotations/spawn.yaml` (end of the functions list).

## 5. What to do next (proposed to the user 2026-10-27, not started)

1. **Run GridNodeSync in Ghidra** (import and export once) and fix what breaks: the 2026-10-22
   changes (`setComment` blocks, `blockKey`, comment export) were written without a Ghidra to compile
   them. Check with a friend relay too (`gridnode.py ghidra-relay`).
2. **Recordings into Ghidra:** a command (`summarize.py --ghidra OUT` or `gridnode.py
   recording-to-ghidra`) that writes a GridNodeSync import from a recording: create functions at the
   unwind-table starts of frames Ghidra has no function for (e.g. `0x1431c7faa`, `0x142d46e1b` were
   return addresses outside any function), bookmark call sites with what came through (event /
   message / traced call), and comment functions with hit counts and the recording's name.
   GridNodeSync would need a "create function" and "bookmark" entry kind.
3. **Pointer names in recordings:** record entity destruction (the destructor hooks exist, see
   `research/destroying-entities.md`) and keep a table of pointers seen (type, data, created and
   destroyed times) so exports and summaries name objects.
4. **Watch a list:** like field watches, for a vector of entity pointers (begin/end at an offset),
   recording adds, removes and order changes with each entry's type. MECMP had a temporary version
   (`trace:` lines in its verbose log) for spawner lists.
5. **Runtime class structs in Ghidra** for the classes in use (spawners, replay entities,
   characters), from reflected sizes plus annotated members, with vtable types.

## 6. Things learned the hard way

- **Fingerprint bytes** must come from a dump (`gridnode.py fingerprint`), never be typed from a
  Ghidra listing: an instruction has several encodings.
- **The game finds a replay's spawner by its data in a bus**, and connections into a spawned blueprint
  resolve through the spawner's *last* spawned character (`Entity_resolveLinkedEntity` → vtable
  +0x50). A second spawner with the same data and bus gets found instead of the game's. MECMP keeps
  the game's echo last in the server and client lists instead.
- **Bus +0x58** of a character is the same for every ghost of a spawner; not a per-character id.
- **Hooks already in a slot:** GridNode wraps every entity destructor slot and onEvent (+0x20) of
  1,354 vtables while recording; code that scans vtables must accept hook pointers (any executable
  memory), and look up the game's function through the hook table (`chain.front().previous`).
- **Stack capture through our own thunks** needs unwinding from the caller (the thunk passes the
  caller's rsp); `RtlCaptureStackBackTrace` inside the logger found nothing.
- **Recordings get large fast:** a level load adds ~135,000 creation events. Ask the user to start
  recording after loading, and to start it *before* the thing to capture (a ghost spawn, opening the
  map).
- **The user's local checkout** `D:\Dev\GridNode` is far behind `main` and has uncommitted annotation
  edits from another agent, mostly wrong (wrong action names, signatures, an address off by 0x1c);
  the repo already holds corrected versions. Cleanup steps were given on 2026-10-22. Do not copy
  annotations from that folder without checking them in Ghidra.
- **MECMP:** `mecmp.ini` is only written when missing, so a new option's default lives in code.
  MECMP's own ghost spawner exists but is off (`own_spawner=0`) for the reason above.
