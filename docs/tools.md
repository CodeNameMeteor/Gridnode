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

## Assets (the game's data files)

`gridnode.py assets ...` reads the game's data where it is installed (`--data` the `Data` folder, as
`gridnode.log` says, or `GRIDNODE_DATA`) and writes only to `bin/assets/` (git-ignored: game data is never
committed). Extracting needs `pip install lz4`.

| Command | What it does |
|---|---|
| `check` | Every catalogue and bundle read; the compression codecs found. |
| `list [--bundle B] [--type ebx\|res\|chunks]` | The entries of the bundles. |
| `extract NAME` | One entry, decompressed, into `bin/assets/<kind>/`. |
| `ebx NAME` (or a `.ebx` file) `[--out DIR\|-]` | An EBX file as YAML, into `bin/assets/yaml/`; says whether it writes back to the same bytes. |
| `ebx-check [--superbundle S] [--bundle B] [--limit N] [--dump JSON]` | Every EBX read, turned into YAML, read back and written: the same bytes? Lists the files that are not, and with the reflected dump, where a file's types disagree with the game's. |
| `deps NAME [--into BUNDLE] [--all]` | Everything an EBX needs: the EBX it imports (and theirs), the resources its fields name, the chunks those name, each with the bundles that hold it. `--into`: only what that bundle lacks (what placing the object in that area would have to add). EBX file GUIDs are kept in `bin/assets/index/`. |
| `fbmod-info FILE.fbmod [--data DIR] [--extract DIR]` | A Frosty mod: its details, what it changes and the bundles it adds to (names with `--data`). `--extract`: its assets into a folder, EBX also as YAML. Reads every format seen (FROSTY 1, 3, 5; FBMODV1 and V2 headers). |
| `fbmod-build SPEC.yaml [--out FILE.fbmod] [--data DIR] [--dump JSON]` | A Frosty mod (FROSTY version 5, as current Frosty writes) from EBX or YAML files; `gridnode.py assets --help` shows the SPEC. Types are checked against the game's first; into `bin/assets/mods/`. |
| `ebx-build FILE.yaml [--out FILE.ebx] [--dump JSON]` | A YAML file written as EBX (into `bin/assets/built/`). Its types are checked against the game's first (the dump: `--dump`, or `db/reflected/` after `setup`); an unknown field, a reference to nothing or a type that disagrees stops it. |

The YAML keeps everything the file has (its GUID, imports, its own type table with offsets, every
instance in order), so it writes back to the same bytes. In it: fields by name (a parent's first; a name a
parent already has is `Type.name`); references are `null`, `'#N'` (instance N of this file) or `'import:N'`;
enums by name; floats as the shortest decimal that is the same float32. To change an asset: `ebx` it, edit the
values, `ebx-build` it. Warnings about fields "not of the game's type" are normal for a few (data only the
pipeline or a debug build uses).

To make a Frosty mod from a change: `ebx` the asset, edit the YAML, list it in a SPEC, `fbmod-build`, then
import the `.fbmod` in Frosty's Mod Manager. An EBX that Frosty wrote reads and builds with the same values,
but not the same bytes: GridNode writes the game's own layout. Format notes: the header of
`tools/assets/gridnode_assets/ebx.py`. Tests: `python3 -m unittest discover -s tools/assets/tests -t tools/assets`.

## Meshes (custom models)

`tools/mesh/frosty_fbx.py TEMPLATE.fbx BLENDER.fbx` makes a Blender export importable by Frosty;
`fbxbin.py` is its binary FBX reader and writer. [Custom meshes](custom-meshes.md) walks through it.

## Other

| Tool | Use |
|---|---|
| `tools/offline_dump/offline_dump.cpp` | Runs the reflection walker on a game image saved as a PE file, off the game ([Dump format](dump-format.md)). Developer tool. |
| `tools/gen/compare_dumps.py` | What `gridnode.py compare` runs. |
| `tools/gen/mecmp_coverage.py ENGINE_H --reflected FILE` | Maintainer check that every value in MECMP's `engine.h` is in the database. Needs the MECMP source. |

## Research to pick up

[Open research](research/open-research.md) lists the large pieces nobody has done yet, with where to start.

## In game (developer build)

INSERT opens the menu: inspector (entities, types, settings, messages), console, Recorder (F8 record / pause,
Home marker), function tracer, field diff, Game image (dump the game for fingerprints), free camera and the
Layout editor (paused). The `probe` example mod maps objects and pointer paths ([Contributing](contributing.md)).
