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

`tools/ghidra/GridNodeSync.java` keeps Ghidra and the annotations in step, both ways.

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
