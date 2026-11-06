# Releases, mod templates and shipping a mod

## A GridNode release

```
python tools/gen/gridnode.py package --build build --examples
```

makes `build/release/GridNode-<version>.zip` from a Windows build (MSVC `build\Release` or the MinGW
build folder). The version is the CMake project's, with the commit: `1.0.0+ccd4784` (`.dirty` when
the tree has changes). CI makes one on every push (artifact `gridnode-release`).

The zip's folder goes anywhere, not in the game's folder: start the game and run
`gridnode_inject.exe`, which loads `gridnode.dll` from its own folder. Only to have GridNode load
with the game every time do `version.dll`, `gridnode.dll` and `mods/` go in the game folder.

| In the zip | What |
|---|---|
| `gridnode.dll`, `gridnode_inject.exe`, `mods/` | GridNode, the injector, and the mods folder (with `--examples`: hello, MEC trainer, World draw, detour demo, each in its own folder) |
| `start-with-the-game/version.dll` | Only to load GridNode at the game's start: copied into the game folder with `gridnode.dll` and `mods/` |
| `sdk/include/gridnode/` | The API headers and `engine.hpp` (our annotations: addresses and offsets) |
| `sdk/imgui/` | Dear ImGui, the copy GridNode draws with: a mod that draws builds it in |
| `sdk/rust/gridnode/` | The Rust crate |
| `sdk/templates/` | `gridnode_mod.py` and the C++ and Rust mod templates |
| `sdk/VERSION` | The release, copied with the SDK into a mod's `gridnode-sdk/` |
| `README.txt`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `VERSION` | |

Nothing of the game's goes in: no reflected dump, no `types.hpp` (made from it), no game files.

### The players' zip

```
python tools/gen/gridnode.py package --player --build build-player --mods build/mods
```

makes `GridNode-player-<version>.zip` from a player build (`-DGRIDNODE_PLAYER_BUILD=ON`; a developer
build is refused): `gridnode.dll`, `gridnode_inject.exe`, `start-with-the-game/version.dll`, the MEC
trainer in `mods/mec_trainer/` (from `--mods`, default the build's own `mods/`) and a README for
players. No SDK.

## Starting a mod

```
python sdk/templates/gridnode_mod.py new my_mod              C++ (CMake)
python sdk/templates/gridnode_mod.py new my_mod --rust       Rust (Cargo)
```

makes a folder `my_mod` with a mod and a copy of that SDK in `my_mod/gridnode-sdk/` (`--sdk` for
another SDK, `--no-vendor` to use it where it is): a window opened with its own key, a setting and a
client tick in C++; a client tick in Rust. The folder is a repository of its own, with a GitHub
Actions workflow and a README: [Your mod in its own repository](mod-repository.md). In this
repository the same command is `gridnode.py new-mod my_mod --sdk <a release's sdk>`.

```
cmake -S my_mod -B my_mod/build -A x64 && cmake --build my_mod/build --config Release
cargo build --release                                        (in the Rust mod's folder, on Windows)
python my_mod/gridnode-sdk/templates/gridnode_mod.py update-sdk --sdk <a newer release's sdk> --mod my_mod
```

## Shipping a mod

```
python sdk/templates/gridnode_mod.py pack my_mod.dll --version 1.0.0 --file data/
```

makes `my-mod-1.0.0.zip` holding `mods/my_mod/my_mod.dll` and its files. Unzipped next to
`gridnode.dll` (GridNode's folder, or the game's when it starts with `version.dll`), GridNode loads it
from `mods/my_mod/`:

- a mod in its own folder is `mods/<name>/<name>.dll`; other DLLs there are its own (found when it
  loads), not mods;
- `gn::modDir()` (Rust `gridnode::mod_dir()`, API 20) is that folder, for its files: a mod runs from
  a copy in `mods/.loaded` so it can be reloaded, so its module path is not where it was installed;
- its settings stay in `mods/<id>.ini`.

A mod built for a newer GridNode than the one installed refuses to load (`gn_mod_load` checks the
API version), so ship it saying which GridNode it needs.
