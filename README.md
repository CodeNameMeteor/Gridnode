# GridNode

An open-source SDK for modding **Mirror's Edge Catalyst** (PC). GridNode lets mods find, read,
write, call and hook the game's engine classes (Frostbite 3) without reversing them first, and
documents everything it knows about the engine.

The full plan is in [docs/PLAN.md](docs/PLAN.md).

> Status: **M6 done** (docs, GridNodeSync, Frosty assessment). M7 (custom maps at runtime) in progress. Docs: `python tools/gen/gridnode.py docs --build`
> (see [docs/building.md](docs/building.md#these-docs)); CI publishes the site as the `docs-site` artifact. GridNode loads mods from `mods/` and gives them reflection by name
> (9,988 engine types), entity-creation callbacks, server and client ticks, chained pointer hooks,
> a world API (local characters, transforms, teleport, components, level events) and a Dear ImGui
> overlay mods can draw into. INSERT opens the GridNode menu and its inspector. See
> [docs/modding.md](docs/modding.md), `examples/hello` and `examples/trainer`.

## What's here

| Path | What |
|---|---|
| `sdk/include/gridnode/` | Public headers (`gridnode.hpp`) |
| `sdk/src/` | `gridnode.dll`: memory access, build check, log, reflection walker, type dump |
| `sdk/loader/` | `version.dll` proxy loader and `gridnode_inject.exe` |
| `db/` | Type database: `annotations/` (our findings, committed), `reflected/` (your own dump, git-ignored) |
| `tools/ghidra/` | `FrostbiteTypeDumper.java` Ghidra script |
| `tools/gen/` | `gridnode.py`: validate the database, generate headers, set up your dump ([docs/annotations.md](docs/annotations.md)) |
| `tools/offline_dump/` | `gn_offline_dump`: the walker run on a PE memory dump, for testing |
| `examples/` | Example mods: `hello` (reflection, ticks, creation), `trainer` (save/load position, speed, an ImGui window) |
| `bindings/rust/gridnode` | Rust bindings (generated from `api.h`) and a safe layer; `examples/hello-rust` |
| `templates/` | Mod templates (C++, Rust) and `gridnode_mod.py` (new / pack); releases: `gridnode.py package` ([docs/packaging.md](docs/packaging.md)) |
| `third_party/imgui` | Dear ImGui 1.91.9 (MIT, [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) |
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

Building needs Python 3 with PyYAML (`pip install pyyaml`): `gridnode/engine.hpp` is generated from
`db/annotations` at build time. On Linux without the cross compiler only the portable parts
(walker, generator, tests, `gn_offline_dump`) build.

## Setting up your type headers

After your first in-game dump:

```
python tools/gen/gridnode.py setup --dump path/to/gridnode-reflected.json
```

This copies the dump to `db/reflected/` (git-ignored) and writes
`sdk/include/gridnode/generated/types.hpp`: C++ structs for every engine class and value type,
with GridNode's annotations merged in. CMake regenerates and compile-checks it whenever the dump
is present.

## Running

1. Copy `gridnode.dll` and `version.dll` into the game folder (or run `gridnode_inject.exe` next to
   `gridnode.dll` after starting the game).
2. Put mods (e.g. `build/mods/trainer.dll`) in a `mods` folder next to `gridnode.dll`.
3. Start the game. `gridnode.log` (next to `gridnode.dll`) reports the build check, writes
   `gridnode-reflected.json` the first time ([docs/dump-format.md](docs/dump-format.md)), and
   lists the mods it loaded.

If the game is in `Program Files`, Windows may redirect the log to
`%LOCALAPPDATA%\VirtualStore\Program Files\...`; GridNode also falls back to `%LOCALAPPDATA%\GridNode`.

In game, **INSERT** opens the GridNode menu (settings: `[ui]` in `gridnode.ini`, see
[docs/modding.md](docs/modding.md#ui-dear-imgui)).

## Rules GridNode follows

- Hooks swap pointers in vtables and creator tables (and the overlay swaps two slots of DXGI's swap
  chain vtable, as MECMP does). Game code is patched only where no pointer reaches (a direct call),
  through GridNode, which lists every patch and undoes it on unload.
- Nothing is done until the build check passes; on any other build GridNode stays passive.
- Nothing from the game is committed: each user generates their own reflected data.
- Online services, Denuvo and the EA app are left alone.

## Licence

MIT, see [LICENSE](LICENSE).
