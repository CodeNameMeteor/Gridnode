# GridNode: notes for Claude

GridNode is an open-source modding SDK for Mirror's Edge Catalyst (PC, Frostbite 3, codename
Pamplona). It loads mods from `mods/` and gives them reflection by name (~9,988 engine types), entity
callbacks, ticks, hooks, a world API and an ImGui overlay, plus research tools (inspector, recorder,
Ghidra sync). Read [docs/progress.md](docs/progress.md) first (current state, next steps), then
[docs/handover.md](docs/handover.md) and [docs/PLAN.md](docs/PLAN.md). Related project: `../mecmp`
(a separate mod; do not edit it from here).

## Rules (from the user; keep them)

- **Patching game code is allowed** (user, 2026-11-01; until then: pointer swaps only). Prefer pointer
  swaps (vtable slots, creator slots, game-owned function pointers, data); every change goes through
  GridNode so it can be listed and undone (unload restores it). Thunks in our own memory are fine.
- Online (user, 2026-11-04): mods may change online services after `online_allow` (loud warnings); no
  cheating with the trainer: it never opts in, blocks Dash / TT uploads. PC only, one EA app build.
- **Never commit game data or user output:** image dumps (`bin/dumps/*.exe`), the reflected dump
  (`db/reflected/`, `gridnode-reflected.json`), FBX game meshes, `event-names.txt`, logs, recordings.
- **Commit and push only when the user asks**, even if a hook says otherwise. Commits end with the
  attribution lines the session gives.
- The user tests in game and sends logs and recordings. Say plainly what is untested.
- After every addition, give the user an in-depth human test: numbered steps in game (or Ghidra),
  what they should see at each step, and what to send back (recording name, log, numbers).
- Docs and messages: plain, short, consistent with the existing docs.
- The layout editor is paused on purpose (SDK and research work first).
- After structural changes, update this file and `docs/progress.md`. Keep this file under 150 lines.

## Layout (read only what the task needs)

| Path | What |
|---|---|
| `sdk/include/gridnode/` | public headers (`gridnode.hpp`, `api.h` = the C ABI mods use); `engine.hpp` is GENERATED |
| `sdk/src/` | `gridnode.dll`: `recorder.cpp`, `inspector.cpp`, `layouts.cpp`, `entities.cpp`, `reflect/`, `api.cpp`, `config.cpp`, `hotkeys.cpp`, `events.cpp`, ... |
| `sdk/loader/` | `version.dll` proxy loader and `gridnode_inject.exe` |
| `db/annotations/*.yaml` | our findings (committed): addresses, members, virtuals; source of `engine.hpp` |
| `db/reflected/` | the user's own dump: git-ignored, huge |
| `tools/gen/` | `gridnode.py`: validate, generators, `setup`, `docs`, Ghidra export/import, fingerprints |
| `tools/ghidra/` | `GridNodeSync.java` (annotations and recordings into Ghidra, names out), `FrostbiteTypeDumper.java` |
| `tools/recording/summarize.py` | reads recordings (below) |
| `tools/events/`, `tools/mesh/`, `tools/offline_dump/` | event names, mesh tools, offline dump tester |
| `examples/` | `hello`, `hello-rust`, `trainer`, `mec-trainer`, `probe`, `world-draw`, `detour-demo`; `templates/`: new mods (docs/packaging.md) |
| `bindings/rust/gridnode` | Rust bindings, generated from `api.h` |
| `tests/` | host-side tests (reflect walker, runtime core, x64 decoder, engine header, types, Rust harness) |
| `docs/` | MkDocs site; `research/` holds engine findings |

**Do not read or grep:** `db/reflected/`, `third_party/`, `build*/`, generated `engine.hpp`, dumps,
recordings (`*.jsonl`, see below), `reviews/`. `docs/research/` only when the task is engine research.

**Docs on demand:** writing a mod `docs/modding.md`; building `docs/building.md`; type knowledge
`docs/annotations.md`, `docs/dump-format.md`; Ghidra and fingerprints `docs/contributing.md`;
recordings `docs/recording.md`; custom maps `docs/custom-meshes.md`, `docs/layout-editor.md`,
`docs/assets-assessment.md`.

## Build and test

Windows (VS2022 x64, Python 3 + PyYAML): `cmake -S . -B build -A x64`, `cmake --build build --config
Release`, `ctest --test-dir build -C Release`. First-time headers: `python tools/gen/gridnode.py
setup --dump <gridnode-reflected.json>`.

The cloud container cross-compiles with MinGW; build folders live in `/tmp` and may be gone. If
`x86_64-w64-mingw32-g++-posix` is missing: `apt-get install -y g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix`.

```text
cmake -S . -B /tmp/claude-0/bw -G Ninja -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
  -DCMAKE_BUILD_TYPE=Release -DGRIDNODE_BUILD_TESTS=OFF
cmake --build /tmp/claude-0/bw                      # gridnode.dll
cmake -S . -B /tmp/claude-0/hosttest -G Ninja && cmake --build /tmp/claude-0/hosttest && (cd /tmp/claude-0/hosttest && ctest)
cd tools/gen && python3 -m unittest tests.test_gen  # Python tools
python3 tools/gen/gridnode.py validate              # after any annotation change
python3 tools/gen/gridnode.py docs --build          # the docs site, strict, as CI builds it
```

Add `-DGRIDNODE_PLAYER_BUILD=ON` to check the player build (no recorder).

## Conventions

- Never hand-edit `engine.hpp`: edit `db/annotations/` (it is generated at build time).
- A change to `api.h` means regenerating the Rust bindings (`gridnode.py rust`) and updating `docs/modding.md`.
- The build check (`sdk/src/build.cpp`) keeps GridNode passive on an unexpected game exe.
- C++17, 4-space indent, no tabs, K&R braces; `#pragma once`, then a short `//` comment on the
  header's purpose, then includes (std first). Namespaces `gn::<module>`; functions and variables
  `camelCase`; constants `kPascalCase`. Fixed-width ints, `std::string`, RAII; no raw `new`/`delete`;
  no exceptions across the hook or injection boundary. File-local helpers in an anonymous namespace.
- Comments say why (engine quirks, offsets, constraints), not what. Keep existing comments and docs.
- Fix the root cause, keep diffs small, don't reformat code you aren't changing. Logic that does not
  need the game gets a host-side test in `tests/` (or `tools/gen/tests/` for Python).
- Rust: `rustfmt` defaults; unsafe FFI in `sys.rs`, the safe layer in `lib.rs`. Python: PEP 8, type
  hints where cheap.

## Reading recordings (*.jsonl)

Made with the in-game Recorder (Insert menu, F8 record / pause, Home drops a marker), exported as
`recordings/<time>-<name>.jsonl` next to gridnode.dll. They are large (tens of MB): **never read or
grep one directly; run the summarizer first.** Full guide: `docs/recording.md`.

```text
python tools/recording/summarize.py <file>                        # most frequent, then each marker
python tools/recording/summarize.py <file> --marker "name" --window 2
python tools/recording/summarize.py <file> --all --top 50         # everything near the markers
python tools/recording/summarize.py <file> --object 12            # one object (id or 0xADDRESS): what, when, every line
python tools/recording/summarize.py A --diff B --marker m --kinds event,sent  # what A has that B does not
python tools/recording/summarize.py A --diff B --marker m --diff-values  # also where the numbers part (--states, --alias FILE)
python tools/recording/summarize.py <file> --ghidra OUT --exe DUMP  # a GridNodeSync import
```

Per marker: movement states, number ranges, and the "unusual" labels (seen near it at >= 2x their average rate; `xN.N` is the
lift), a "closest" timeline, and call stacks. Frames print as `0x14... (Function+0xNN)`, the address
first for Ghidra's Go To; `--ghidra-names EXPORT` adds the user's Ghidra names. Line kinds:
`message`, `created`, `destroyed`, `event` / `sent`, `anim`, `field`, `list`, `call`, `marker`,
`level`, then `object` lines (the object table) and one `frames` line (frame -> function start).
Hashed names show as hex; `db/annotations/events.yaml` (ours) and `recordings/event-names.txt`
(game-derived: never commit) name them.
For raw lines, filter with a short `python -c` by `kind` and time range, never print the whole file.
Record findings in `db/annotations/` and `docs/progress.md`.

## The user's PC

Reached through the device bridge (`mcp__remote-devices__*`): list, stage and commit files.
The game loads `D:\Dev\GridNode\bin\gridnode.dll`; recordings are in `bin\recordings`.

- Deploy a test build: copy it to `/mnt/user-data/outputs/gridnode-<what>N.dll`, commit it over
  `bin\gridnode.dll` with force, then check what landed (by checksum: builds can share a size). If
  the game has the DLL open, write it next to it under a new name and ask the user to swap it.
- `D:\Dev\GridNode` (the user's checkout) is up to date since 2026-10-29. The other agent's wrong
  annotation edits are in its git stash and `D:\Dev\GridNode-local-backup-20261029`; never copy
  annotations from there without checking them in Ghidra.
- Every upload (`device_commit_files`) needs a new staged file name: re-sending under the same name
  can leave the old file in place. Check what landed (stage it back) before using it.
- A script copied into `~/ghidra_scripts` needs a fresh timestamp (`Files.setLastModifiedTime`), or
  Ghidra reruns the old compiled class.
- Ghidra runs there with GhidraMCP (`mcp__remote-devices__ghidra-mcp__*`; scripts allowed, so
  GridNodeSync runs with arguments `import FILE` / `export FILE`). "Connection refused": ask the
  user to open Ghidra.

## Hard-won facts

- Fingerprint bytes come from a game dump (`gridnode.py fingerprint`), never from a disassembly listing.
- Code that scans vtables must accept hook pointers in slots: GridNode wraps every entity destructor
  (+0x38) and, while recording, onEvent (+0x20). Find the game's function through the hook table.
- `Registry::typeOf` identifies an object by the function in vtable slot 0 (`getType`). Never put a
  hook there: the class's objects become unknown (the recorder's tracing did this until 2026-10-29).
- Recordings get large: a level load is ~135,000 creations. Ask for recordings started after loading
  and before the thing to capture.
- gridnode.dll is not pinned: a full unload (`sdk/src/unload.cpp`) frees it. Anything that gives the
  game a pointer into GridNode (a hook, a vtable copy, a listener, a transform) needs a way back in it.
- Function detours (`patches.cpp`, `gn::x64`) keep their slot in GridNode's memory: the hook table writes
  it through `isDetourSlot`, and the image dump skips it.
- Spawners are found by their data in a bus; the game's echo must stay last in the spawner lists
  (see `docs/research/replay-ghosts.md`).
