# GridNode

An open-source SDK for modding **Mirror's Edge Catalyst** (PC). GridNode lets mods find, read,
write, call and hook the game's engine classes (Frostbite 3) without reversing them first, and
documents everything it knows about the engine.

The full plan is in [docs/PLAN.md](docs/PLAN.md).

> Status: **M1 in progress.** The runtime loads into the game, checks the build, and writes the
> engine's reflection data to a JSON file. Entity, hook and threading APIs come next.

## What's here

| Path | What |
|---|---|
| `sdk/include/gridnode/` | Public headers (`gridnode.hpp`) |
| `sdk/src/` | `gridnode.dll`: memory access, build check, log, reflection walker, type dump |
| `sdk/loader/` | `version.dll` proxy loader and `gridnode_inject.exe` |
| `db/` | Type database: `annotations/` (our findings, committed), `reflected/` (your own dump, git-ignored) |
| `tools/ghidra/` | `FrostbiteTypeDumper.java` Ghidra script |
| `tools/gen/` | Database tools (`compare_dumps.py`) |
| `tools/offline_dump/` | `gn_offline_dump`: the walker run on a PE memory dump, for testing |
| `tests/` | Host-side tests |
| `docs/` | Plan and reference docs |

## Building

Windows, Visual Studio 2022 (x64):

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Linux, cross-compiling with MinGW-w64:

```
cmake -S . -B build -G Ninja -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
  -DCMAKE_BUILD_TYPE=Release -DGRIDNODE_BUILD_TESTS=OFF
cmake --build build
```

On Linux without the cross compiler only the portable parts (walker, tests, `gn_offline_dump`) build.

## Running

1. Copy `gridnode.dll` and `version.dll` into the game folder (or run `gridnode_inject.exe` next to
   `gridnode.dll` after starting the game).
2. Start the game. `gridnode.log` (next to `gridnode.dll`) reports the build check and writes
   `gridnode-reflected.json` the first time. See [docs/dump-format.md](docs/dump-format.md).

If the game is in `Program Files`, Windows may redirect the log to
`%LOCALAPPDATA%\VirtualStore\Program Files\...`; GridNode also falls back to `%LOCALAPPDATA%\GridNode`.

## Rules GridNode follows

- Game code is never patched. Hooks only swap pointers in vtables and creator tables.
- Nothing is done until the build check passes; on any other build GridNode stays passive.
- Nothing from the game is committed: each user generates their own reflected data.
- Online services, Denuvo and the EA app are left alone.

## Licence

MIT, see [LICENSE](LICENSE).
