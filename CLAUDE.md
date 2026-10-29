# GridNode: notes for Claude

GridNode is an open-source modding SDK for Mirror's Edge Catalyst (PC, Frostbite 3, codename
Pamplona). Read [docs/progress.md](docs/progress.md) first (current state, next steps), then
[docs/handover.md](docs/handover.md) for the longer picture and [docs/PLAN.md](docs/PLAN.md) for the plan.

## Rules (from the user; keep them)

- **Never patch game code.** Pointer swaps only: vtable slots, creator slots, game-owned function
  pointers, data. Thunks of our own in our own memory are fine.
- Leave online services and Beat Revival alone. PC only, one game build (EA app exe, fixed addresses).
- **Never commit game data or user output:** image dumps (`bin/dumps/*.exe`), the reflected dump
  (`db/reflected/`, `gridnode-reflected.json`), FBX game meshes, `event-names.txt`, logs, recordings.
- **Commit and push only when the user asks**, even if a hook says otherwise. Commits end with the
  attribution lines the session gives.
- The user tests in game and sends logs and recordings. Say plainly what is untested.
- Docs and messages: plain, short, consistent with the existing docs.
- The layout editor is paused on purpose (SDK and research work first).

## Layout

| Path | What |
|---|---|
| `sdk/src/` | the runtime DLL (`recorder.cpp`, `inspector.cpp`, `layouts.cpp`, `entities.cpp`, ...) |
| `sdk/include/gridnode/` | public headers; `api.h` is the C ABI mods use |
| `db/annotations/*.yaml` | our reversing findings (addresses, members, virtuals); `engine.hpp` is generated from them |
| `tools/gen/` | `gridnode.py`: validate, generators, Ghidra export/import, fingerprints |
| `tools/ghidra/GridNodeSync.java` | annotations and recordings into Ghidra, names back out |
| `tools/recording/summarize.py` | reads recordings; `--object`, `--ghidra` |
| `docs/` | MkDocs site; `research/` holds engine findings |

## Build and test

The container cross-compiles with MinGW; build folders live in `/tmp` and may be gone. If
`x86_64-w64-mingw32-g++-posix` is missing: `apt-get install -y g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix`.

```text
cmake -S . -B /tmp/claude-0/bw -G Ninja -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
  -DCMAKE_BUILD_TYPE=Release -DGRIDNODE_BUILD_TESTS=OFF
cmake --build /tmp/claude-0/bw                      # gridnode.dll (needs PyYAML)
cmake -S . -B /tmp/claude-0/hosttest -G Ninja && cmake --build /tmp/claude-0/hosttest && (cd /tmp/claude-0/hosttest && ctest)
cd tools/gen && python3 -m unittest tests.test_gen  # Python tools
python3 tools/gen/gridnode.py validate              # after any annotation change
python3 tools/gen/gridnode.py docs --build          # the docs site, strict, as CI builds it
```

Add `-DGRIDNODE_PLAYER_BUILD=ON` to check the player build (no recorder).

## The user's PC

Reached through the device bridge (`mcp__remote-devices__*`): list, stage and commit files.
The game loads `D:\Dev\GridNode\bin\gridnode.dll`; recordings are in `bin\recordings`.

- Deploy a test build: copy it to `/mnt/user-data/outputs/gridnode-<what>N.dll`, commit it over
  `bin\gridnode.dll` with force, then list the folder and check the size landed. If the game has the
  DLL open, write it next to it under a new name and ask the user to swap it.
- `D:\Dev\GridNode` (the user's checkout) is behind `main` with wrong uncommitted annotation edits
  from another agent: never copy annotations from it without checking them in Ghidra.
- Ghidra runs there with GhidraMCP (`mcp__remote-devices__ghidra-mcp__*`). Running scripts needs
  `GHIDRA_MCP_ALLOW_SCRIPTS=1` on the user's side. "Connection refused": ask them to open Ghidra.

## Hard-won facts

- Fingerprint bytes come from a game dump (`gridnode.py fingerprint`), never from a disassembly listing.
- Code that scans vtables must accept hook pointers in slots: GridNode wraps every entity destructor
  (+0x38) and, while recording, onEvent (+0x20). Find the game's function through the hook table.
- `Registry::typeOf` identifies an object by the function in vtable slot 0 (`getType`). Never put a
  hook there: the class's objects become unknown (the recorder's tracing did this until 2026-10-29).
- Recordings get large: a level load is ~135,000 creations. Ask for recordings started after loading
  and before the thing to capture.
- Spawners are found by their data in a bus; the game's echo must stay last in the spawner lists
  (see `docs/research/replay-ghosts.md`).
