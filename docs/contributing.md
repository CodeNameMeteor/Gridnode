# Contributing findings

Everything GridNode knows about the engine beyond reflection lives in `db/annotations/*.yaml`: runtime
class members, virtual functions, globals, functions, hooks, constants. The SDK headers, the build
check and the [Annotated engine](annotated/index.md) pages are all generated from it. The format is
in [Annotation format](annotations.md).

## What makes a good finding

- **Where it came from** (`sources`): `gridnode` (found with GridNode or its probe), `ghidra`,
  `mecmp`, `mec-trainer`, `research`...
- **How sure** (`confidence`):
    - `confirmed`: checked in game, or the code leaves no doubt;
    - `inferred`: from the code or a pattern, not seen working;
    - `hypothesis`: a guess worth writing down.
- **A note** on how it was found and how to check it again: the function that reads it, the probe
  that showed it.
- **Addresses** are image VAs as Ghidra shows them (`0x14...`), never runtime addresses.

## Finding things with the probe

`examples/probe` is a research mod. It walks pointer chains and names every object on the way, maps
an object's members, searches pointer paths from Faith to an address, compares memory while a key is
held, and watches the input actions. Results go to `gridnode.log`. Most findings in `faith.yaml` and
`input.yaml` came from it.

## Before sending a change

```text
python tools/gen/gridnode.py validate --strict
python tools/gen/gridnode.py validate --reflected db/reflected/gridnode-reflected.json
```

The second checks offsets and sizes against your dump. Never commit the dump or anything copied
from the game: annotations hold our own findings only.

## Ghidra (GridNodeSync)

`tools/ghidra/GridNodeSync.java` keeps Ghidra and the annotations in step, both ways. It needs
Ghidra 12 (run on 12.1.4): copy it into one of your script folders (Script Manager → Manage
Script Directories).

**Annotations into Ghidra.**

```text
python tools/gen/gridnode.py ghidra-export      # build/gridnode-ghidra.json (sizes from your dump if present)
```

In Ghidra: Script Manager → GridNode → GridNodeSync → *Import*, pick the file. Every annotated class
and struct becomes a struct in `/GridNode` with its members; functions, virtual functions
(`Class::name`), globals, code sites, vtables (`Class::vftable`) and hook slots are named, with
GridNode's confidence, source and note as a comment. Names you gave yourself are never changed, and
your own comments are kept above GridNode's.

**Your Ghidra names into annotations.**

Run GridNodeSync → *Export* and save the file, then:

```text
python tools/gen/gridnode.py ghidra-import ghidra-names.json     # db/ghidra-inbox.yaml
```

The inbox holds the functions and labels you named that GridNode does not know (with your prototype
when you set one), the members you named in `/GridNode` structs, and names that differ from GridNode's.
Everything is `inferred` from `ghidra` until checked. Move each item into the annotation file it
belongs to.

Export before importing again if you edited the `/GridNode` structs in Ghidra: an import replaces them.

**A friend's Ghidra.** They run GridNodeSync → *Export* in their Ghidra and send you the file. Either
review their findings as annotations, or put their names straight into your Ghidra:

```text
python tools/gen/gridnode.py ghidra-import alex-names.json --who Alex   # db/ghidra-inbox.yaml, sources [ghidra, alex]
python tools/gen/gridnode.py ghidra-relay alex-names.json --who Alex    # build/gridnode-ghidra-relay.json
```

Import the relay file with GridNodeSync → *Import*. Their names go where you have none of your own
(yours are kept; a name an earlier GridNode import set is replaced, so import GridNode's file again
after a relay if you prefer its names), and their comments go in a `GridNode [relay] (Alex's Ghidra)` block that a later
relay from them replaces; your own text, GridNode's block and other friends' blocks stay. Exports now
carry the comments you wrote (plate comments of functions, plate or end-of-line comments of labels).

**Without the dialogs.** GridNodeSync also takes script arguments, `import FILE` or `export FILE`,
for a headless Ghidra or a run through the Ghidra MCP plugin (which needs `GHIDRA_MCP_ALLOW_SCRIPTS=1`).

**Recordings into Ghidra.** `summarize.py --ghidra` turns a recording into a file GridNodeSync imports
the same way (see [Recording](recording.md#into-ghidra)): functions made where Ghidra has none, a
bookmark at each call site, and a comment per function with what went through it.

**Two sets of annotations.** To see how another copy differs from the repository (a fork, another
checkout, what another tool wrote):

```text
python tools/gen/gridnode.py annotations-diff D:/other/db/annotations                 # against db/annotations
python tools/gen/gridnode.py annotations-diff git:origin/main --base D:/mine/annotations
python tools/gen/gridnode.py annotations-diff OTHER --inbox db/annotations-inbox.yaml  # their new and changed entries
```

Entries are matched by section and name, and functions, globals, code sites and hooks also by address
(a rename at the same address shows as one). Classes and structs are compared member by member,
virtual by virtual. The inbox keeps each entry's confidence and sources and notes what changed.

## Fingerprints from the game

Fingerprint bytes come from the game, not from a disassembly listing: an instruction can be encoded
several ways (`mov rbx,r9` is `49 8B D9` or `4C 89 CB`), and a listing does not say which one the game uses.

1. In game: GridNode menu → **Game image** → **Dump the game image**. It writes
   `bin/dumps/MirrorsEdgeCatalyst-<time>.exe`, the running game's code and data (about 115 MB). It
   is game data: never share or commit it.
2. Check, and put the image's bytes into the entries that differ:

```text
python tools/gen/gridnode.py fingerprint bin/dumps/MirrorsEdgeCatalyst-....exe                  # check
python tools/gen/gridnode.py fingerprint DUMP --write                                           # fix fingerprint.yaml
python tools/gen/gridnode.py fingerprint DUMP --add Console_executeCommand --length 16          # a new entry
python tools/gen/gridnode.py fingerprint DUMP --mecmp D:/Dev/MECMP [--write]                    # MECMP's fingerprint.cpp too
```

Slots (vtable entries, hooks) are checked as well. After `--write`, read the notes and comments next
to the rewritten entries: they may still describe the old bytes.
