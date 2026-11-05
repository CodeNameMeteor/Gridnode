# GridNode

An open-source SDK for modding **Mirror's Edge Catalyst** (PC, EA app build). GridNode lets mods
find, read, write, call and hook the game's engine classes (Frostbite 3) by name, without reversing
them first, and documents everything it knows about the engine.

> **Status:** getting ready for the first release (API 27). The plan is in
> [docs/progress.md](docs/progress.md); the full design in [docs/PLAN.md](docs/PLAN.md). The docs site
> builds with `python tools/gen/gridnode.py docs --build` ([docs/building.md](docs/building.md#these-docs));
> CI publishes it as the `docs-site` artifact.

## What GridNode gives a mod

| | |
|---|---|
| Mods | C, C++ (a plain C ABI) and Rust; a mod is `mods/<name>.dll` or `mods/<name>/<name>.dll` with its files; reload and unload while the game runs; manifests, dependencies and functions shared between mods |
| Reflection | 9,988 engine types by name: fields, enums, sizes, `typeOf(object)` |
| Entities | creation callbacks per class, components, transforms, entities near a point or of a class, events between entities, the game's own teleport, despawning characters |
| Threads | server and client ticks, `runOnServer` / `runOnClient` |
| Hooks | chained pointer hooks (vtable and creator slots), function detours and call patches; every change is listed and undone on unload |
| Physics | ray casts (what was hit: entity, body, layer, material), collision bodies, static collision boxes, body velocities, gravity |
| Faith | movement state, speed, wall counts, camera, health, input actions whatever the binding, god mode, movement tuning by name, movement events |
| Game state | levels, settings (every settings class by name), the console, engine messages, sounds, the checkpoint, the save's progression flags and missions |
| Overlay | a shared Dear ImGui overlay (INSERT): mods' windows, drawing in the world, declared settings and hotkeys |

The developer build adds tools for research: an inspector (entities, types, settings, messages), the
console, the Recorder (what the game does, as a file to summarize), a function tracer, a field diff
window, the Layout editor (paused), a game image dump, and Ghidra sync (`tools/ghidra/GridNodeSync.java`).

## Using it

1. Get a release zip (CI's `gridnode-release` artifact, or `gridnode.py package`) and unzip it
   anywhere ([docs/packaging.md](docs/packaging.md)).
2. Start the game, then run `gridnode_inject.exe` from that folder. To load GridNode with the game
   every time instead, copy `start-with-the-game/version.dll`, `gridnode.dll` and `mods/` into the
   game's folder.
3. Mods go in `mods/`. In game, **INSERT** opens the GridNode menu. `gridnode.log` (next to
   `gridnode.dll`) says what loaded.

[docs/getting-started.md](docs/getting-started.md) has the details; [docs/modding.md](docs/modding.md)
is the API; [docs/shipping.md](docs/shipping.md) is what players of your mod need.

## Making a mod (in your own repository)

A mod needs only the SDK from a release zip, not this repository:

```text
python <GridNode release>/sdk/templates/gridnode_mod.py new my_mod     (--rust for Rust)
```

makes `my_mod/` with the mod, a copy of the SDK (`gridnode-sdk/`), a GitHub Actions workflow and a
README: commit it and it builds anywhere. [docs/mod-repository.md](docs/mod-repository.md) covers
building, testing in game, showing only your mod's window and shipping it.

## What's here

| Path | What |
|---|---|
| `sdk/include/gridnode/` | Public headers: `api.h` (the C ABI), `mod.hpp` (C++ helpers), `engine.hpp` (generated from the annotations) |
| `sdk/src/` | `gridnode.dll`: reflection, the mod loader, hooks, the world API, the overlay and the developer tools |
| `sdk/loader/` | `version.dll` proxy loader and `gridnode_inject.exe` |
| `db/annotations/` | Our findings (committed): addresses, members, virtual functions, hooks; `db/reflected/` is your own dump (git-ignored) |
| `tools/gen/` | `gridnode.py`: validate, generate headers and docs, set up your dump, fingerprints, Ghidra import / export, the release zip |
| `tools/ghidra/` | `GridNodeSync.java` (annotations and recordings into Ghidra, names out) and `FrostbiteTypeDumper.java` |
| `tools/recording/` | `summarize.py`: reads the Recorder's files |
| `tools/events/`, `tools/mesh/`, `tools/offline_dump/` | event names, mesh tools, the walker run on a memory dump |
| `examples/` | Example mods: `hello`, `hello-rust`, `trainer`, `mec-trainer` (the MEC trainer), `world-draw`, `detour-demo`, `probe` |
| `templates/` | Mod templates (C++, Rust) and `gridnode_mod.py` (new / pack / update-sdk) |
| `bindings/rust/gridnode` | Rust bindings (generated from `api.h`) and a safe layer |
| `third_party/imgui` | Dear ImGui 1.91.9 (MIT, [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)) |
| `tests/` | Host-side tests |
| `docs/` | The docs site; `docs/research/` holds engine findings |

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

Building needs Python 3 with PyYAML (`pip install pyyaml`): `engine.hpp` is generated from
`db/annotations` at build time. `-DGRIDNODE_PLAYER_BUILD=ON` builds the player build (the mod loader
without the developer tools). More in [docs/building.md](docs/building.md).

After your first in-game start, `python tools/gen/gridnode.py setup --dump path/to/gridnode-reflected.json`
turns your own reflected dump into C++ headers for every engine type ([docs/annotations.md](docs/annotations.md)).

## Rules GridNode follows

- Every change to the game goes through GridNode, which lists it and undoes it on unload. Pointer
  swaps are preferred (vtable and creator slots); code is patched where no pointer reaches.
- Nothing is done until the build check passes; on any other game build GridNode stays passive.
- Nothing from the game is committed or shipped: each user generates their own reflected data.
- Online services, Beat Revival, Denuvo and the EA app are left alone. Settings and console commands
  that touch online play are marked and ask first; progression flags that go to online stats can't
  be set; the MEC trainer blocks Dash and time trial uploads while it is loaded.

## Licence

MIT, see [LICENSE](LICENSE).
