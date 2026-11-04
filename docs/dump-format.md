# The reflected dump (`gridnode-reflected` JSON)

Mirror's Edge Catalyst keeps Frostbite's reflection data in memory: every data class, value type,
enum and array type the engine knows, with field names, offsets and types. GridNode reads it from
**your own copy of the game** and writes it to a JSON file. Nothing from the game is shipped in this
repository; everyone generates their own dump.

## Making a dump

1. Start the game with a developer build of GridNode: run `gridnode_inject.exe` from GridNode's
   folder ([Getting started](getting-started.md)), or load it with the game through `version.dll`.
   A player build does not write the dump unless `[dump] mode=always`.
2. Wait for the main menu. `gridnode.log` says `dump: written to ...`.
3. The file is `gridnode-reflected.json` next to `gridnode.dll`. Copy it to `db/reflected/` in your
   GridNode checkout.

It takes a few seconds and runs on GridNode's own thread; the game keeps running.

`gridnode.ini` (next to `gridnode.dll`, optional):

```ini
[dump]
mode=auto                      ; auto: only when the file does not exist yet | always | never
file=gridnode-reflected.json   ; relative to gridnode.dll, or an absolute path
```

GridNode only writes the dump after its build check has passed (see `sdk/src/build.cpp`).

## Format (version 1)

```jsonc
{
  "format": "gridnode-reflected", "version": 1,
  "generator": "gridnode 0.1.0", "created": "2026-10-19T15:00:00Z",
  "image": { "name": "MirrorsEdgeCatalyst.exe", "base": "0x140000000", "size": "0x727b000", "timestamp": "0x5762e3b7" },
  "stats": { "listed": 9988, "accessors": ..., "vtablesFromClassInfo": ..., "vtablesFromDataScan": ...,
             "vtableCandidatesAmbiguous": ..., "creators": ..., "shortClassInfos": ..., "unknownCategory": 0, "unreadable": 0 },
  "typeCount": 9988,
  "types": [ /* sorted by "ti" */ ]
}
```

All addresses are **image VAs** at base `0x140000000` (the same numbers as in Ghidra), written as
hex strings. Addresses outside the image (heap objects such as default instances) are left out,
because they change every session.

Each type:

| Key | Present for | Meaning |
|---|---|---|
| `ti` | all | runtime TypeInfo (`ClassInfo` for classes). The type's identity. |
| `data` | all | static TypeInfoData record |
| `name` | all | raw name (often empty for arrays) |
| `displayName` | all | `name`, or `Array<Element>` for unnamed arrays |
| `module` | all | Frostbite module (`Core`, `Entity`, `Pamplona`, ...) |
| `category` | all | `Primitive`, `ValueType`, `Class`, `Array`, `FixedArray`, `Enum`, ... (from the flags) |
| `flags`, `size`, `alignment`, `runtimeId` | all | raw TypeInfoData values; runtime id from TypeInfo+0x10 |
| `super` | classes | super class `ti` |
| `element` | arrays | element type `ti` |
| `classId`, `lastSubClassId` | classes | id range used by `isKindOf` |
| `create` | data classes | create function |
| `accessors` | classes | functions that return this TypeInfo (`lea rax,[ti]; ret`) |
| `getType` | classes with a vtable | the accessor in vtable slot 0 |
| `vtables`, `vtableConfirmed` | classes | `true`: taken from ClassInfo+0x50. `false`: found by scanning image data for pointers to the accessor; several entries mean subclasses without their own reflection share it |
| `creators` | runtime classes | `{creator, realm, data}`: the entity creator (its vtable slot +8 is the factory), realm 0 client / 1 server, and the data class it creates from |
| `fields` | classes, value types | `{name, offset, flags, type}`; flags `0x48` = array field, `0x2000` = settings field |
| `values` | enums | `{name, value}` |

## Checking a dump against Ghidra

`tools/gen/compare_dumps.py` compares a GridNode dump with a FrostbiteTypeDumper `fb_types.json`
type by type:

```
python tools/gen/compare_dumps.py gridnode-reflected.json fb_types.json
```

Layout and identity (names, sizes, fields, super classes, ...) must match exactly. getType, vtables
and creators are found differently by the two tools (Ghidra also uses the create functions), so
differences there are reported but only fail with `--strict`.

`gn_offline_dump <dump.exe> <out.json>` runs the same walker on a PE memory dump of the game (for
example a Scylla dump) on any OS. It is a development tool for testing the walker without launching
the game.
