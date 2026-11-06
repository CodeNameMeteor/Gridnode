# Tools

Everything under `tools/`, what it is for and how to run it. Python 3 with PyYAML is enough for all of the
Python tools; nothing here needs the game running except where it says so. Paths are from the repository root.

## `gridnode.py`: the database and the release

`python tools/gen/gridnode.py --help` lists every command. The ones you will use:

| Command | What it does | Needs |
|---|---|---|
| `validate [--strict] [--reflected FILE]` | Checks `db/annotations/` (and, with a dump, offsets and sizes against it). Run after every annotation change. | nothing |
| `setup --dump FILE` | Installs your reflected dump into `db/reflected/` and generates `types.hpp`. First build only. | your dump |
| `rust [--check]` | Regenerates the Rust bindings from `api.h`. `--check` fails if they are out of date (CI does this). Run `cargo fmt` after. | nothing |
| `docs [--build]` | Builds this site, strict, as CI does. With a dump: the full engine reference, locally. | `pip install -r docs/requirements.txt` |
| `package [--build DIR] [--examples]`, `package --player` | The release zips: developer build and players' build. | a build folder |
| `new-mod NAME [--rust]`, `pack-mod DLL` | A mod from the template; a mod's zip for others. | a release zip |
| `fingerprint IMAGE [--add NAME]` | Fingerprint bytes for a function, **from a game image dump** (never from a disassembly listing). | an image dump (menu: Game image) |
| `verify-image FILE` | Checks the annotations against a dump of the game. | an image dump |
| `compare GRIDNODE_JSON GHIDRA_JSON` | Compares our reflected dump with FrostbiteTypeDumper's. | both dumps |
| `ghidra-export`, `ghidra-import`, `ghidra-relay` | Annotations to and from Ghidra (below). | Ghidra |
| `annotations-diff OTHER` | Compares two annotation sets (folders or `git:REF`). | nothing |
| `hash NAME...`, `unhash HASH...` | Frostbite name hashes, and names for hashes. | optionally the game exe |

Tests: `cd tools/gen && python3 -m unittest tests.test_gen`.

## Recordings

`tools/recording/summarize.py`: reads a Recorder export (`recordings/*.jsonl`, tens of MB: never open one
directly). The common calls are in [Recorder](recording.md); `--help` has the rest. Add `--ghidra OUT --exe DUMP`
to make a GridNodeSync import from what a recording saw.

`tools/events/build_names.py`: builds `event-names.txt`, names for the event and property hashes the Recorder
shows as hex. Every input is optional (an EBX XML dump, your reflected dump, the game exe, recordings); the
header of the script has the command. The output comes from the game: do not commit it.

## Ghidra

| File | Use |
|---|---|
| `tools/ghidra/GridNodeSync.java` | Annotations into Ghidra and names out (`import FILE` / `export FILE`). Copy into a script folder; [Contributing](contributing.md#ghidra-gridnodesync). |
| `tools/ghidra/FrostbiteTypeDumper.java` | Dumps Frostbite's type info from Ghidra, to compare with ours (`gridnode.py compare`); [Dump format](dump-format.md). |
| `tools/ghidra/FrostbiteVTableLengthFinder.java`, `vtable_finder.py` | The exact length of every vtable ([vtables](research/vtables.md)). The Python one works on an image file: `python tools/ghidra/vtable_finder.py --exe DUMP --export out.json`. |

## Meshes (custom models)

`tools/mesh/frosty_fbx.py TEMPLATE.fbx BLENDER.fbx` makes a Blender export importable by Frosty;
`fbxbin.py` is its binary FBX reader and writer. [Custom meshes](custom-meshes.md) walks through it.

## Other

| Tool | Use |
|---|---|
| `tools/offline_dump/offline_dump.cpp` | Runs the reflection walker on a game image saved as a PE file, off the game ([Dump format](dump-format.md)). Developer tool. |
| `tools/gen/compare_dumps.py` | What `gridnode.py compare` runs. |
| `tools/gen/mecmp_coverage.py ENGINE_H --reflected FILE` | Maintainer check that every value in MECMP's `engine.h` is in the database. Needs the MECMP source. |

## In game (developer build)

INSERT opens the menu: inspector (entities, types, settings, messages), console, Recorder (F8 record / pause,
Home marker), function tracer, field diff, Game image (dump the game for fingerprints), free camera and the
Layout editor (paused). The `probe` example mod maps objects and pointer paths ([Contributing](contributing.md)).
