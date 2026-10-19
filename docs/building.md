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
