# Building

## The SDK

Windows, Visual Studio 2022 (x64):

```text
cmake -S . -B build -A x64
cmake --build build --config Release
```

Linux, cross-compiling with MinGW-w64:

```text
cmake -S . -B build -G Ninja -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
  -DCMAKE_BUILD_TYPE=Release -DGRIDNODE_BUILD_TESTS=OFF
cmake --build build
```

The build needs Python 3 with PyYAML (`pip install pyyaml`): `gridnode/engine.hpp` is generated from
`db/annotations` at build time.

Outputs: `gridnode.dll`, `version.dll`, `gridnode_inject.exe`, and the example mods in `build/mods/`.

Options (`-D...=ON/OFF` when configuring):

| Option | Default | |
|---|---|---|
| `GRIDNODE_PLAYER_BUILD` | OFF | the player build: the mod loader without the developer tools ([Shipping a mod](shipping.md)) |
| `GRIDNODE_BUILD_EXAMPLES` | ON | the example mods |
| `GRIDNODE_BUILD_TESTS` | ON | the host-side tests (`ctest`; off when cross-compiling) |

The host-side tests (reflection walker, runtime core, the x64 decoder, the engine header, types,
the Rust harness) build on Linux or Windows without the game:

```text
cmake -S . -B build-host -G Ninja && cmake --build build-host && (cd build-host && ctest)
cd tools/gen && python3 -m unittest tests.test_gen     # the Python tools
```

A release zip from a build: `python tools/gen/gridnode.py package --build build --examples`
([Releases and mod templates](packaging.md)).

## Your type headers

After the first in-game dump:

```text
python tools/gen/gridnode.py setup --dump path/to/gridnode-reflected.json
```

This copies the dump to `db/reflected/` (git-ignored) and writes
`sdk/include/gridnode/generated/types.hpp`: a C++ struct for every engine class and value type, with
GridNode's annotations merged in.

## The tools

`tools/gen/gridnode.py`:

| Command | |
|---|---|
| `validate [--reflected FILE] [--strict]` | check the annotations (and against a dump) |
| `engine --out FILE` | `gridnode/engine.hpp` from the annotations |
| `types --reflected FILE --out FILE` | `types.hpp` from a dump |
| `setup --dump FILE` | install your dump and generate `types.hpp` |
| `rust [--check]` | the Rust bindings from `api.h` |
| `docs [--reflected FILE] [--out DIR] [--build]` | this site; with a dump, the full reference (local) |
| `hash NAME...` / `unhash HASH...` | Frostbite name hashes (Frosty shows them as signed ints) |
| `verify-image FILE` | check the annotations against a memory dump of the game |
| `fingerprint DUMP [--write] [--add NAME] [--mecmp DIR]` | fingerprint bytes from a dump of the game (see contributing) |
| `annotations-diff OTHER [--base B] [--inbox FILE]` | compare two annotation sets (folders or `git:REF`) |
| `ghidra-export` / `ghidra-import FILE [--who NAME]` / `ghidra-relay FILE [--who NAME]` | GridNodeSync files (see contributing) |
| `compare GRIDNODE_JSON GHIDRA_JSON` | compare GridNode's dump with a FrostbiteTypeDumper dump |
| `package [--build DIR] [--examples]` | a release zip ([Releases and mod templates](packaging.md)) |
| `new-mod NAME [--rust] [--sdk DIR]` / `pack-mod DLL [--file F ...]` | a mod from the template, and a mod's zip for others |

## These docs

```text
pip install mkdocs-material
python tools/gen/gridnode.py docs --build                       # the published site, in build/docs/site
python tools/gen/gridnode.py docs --reflected db/reflected/gridnode-reflected.json --build
                                                                # plus the reflected reference (local only)
```

`docs --serve` runs a local server instead of building.

## Rust mods

```text
cargo build --release --target x86_64-pc-windows-msvc --manifest-path examples/hello-rust/Cargo.toml
```

`bindings/rust/gridnode/src/sys.rs` is generated (`gridnode.py rust`); CI fails when it is out of date.
