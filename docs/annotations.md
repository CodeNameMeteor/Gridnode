# Annotations (`db/annotations/*.yaml`)

Annotations are GridNode's own findings about the engine: what reflection does not say. Runtime
class members, virtual functions, globals, functions, hook points, structs the engine never
reflects, and the build fingerprint. They are committed to the repository. The reflected data
(names, fields, sizes of every reflected type) is not: each user generates it from their own game
(see [dump-format.md](dump-format.md)).

Everything is checked:

| Check | Command |
|---|---|
| Syntax, types, names, hashes, hook alignment | `python tools/gen/gridnode.py validate` |
| Against your dump: class exists, vtable, realm, sizes, member ranges, fingerprint types | `python tools/gen/gridnode.py validate --reflected db/reflected/gridnode-reflected.json` |
| Against the game image: every hook slot and virtual holds the stated function, every fingerprint byte matches, every function is in code | `python tools/gen/gridnode.py verify-image <memory dump>.exe` |

## File layout

One file per subsystem (`world.yaml`, `spawn.yaml`, `replay.yaml`, ...). Each file is a mapping
with any of these sections:

```yaml
defaults: { confidence: confirmed, sources: [mecmp, research] }   # for every item in this file
classes: [...]      # reflected runtime classes: members, virtuals, properties
structs: [...]      # structs the engine does not reflect
globals: [...]
functions: [...]
hooks: [...]        # pointers GridNode may swap
code: [...]         # addresses of instructions (load/store sites)
constants: [...]
fingerprint: {...}  # the build check (fingerprint.yaml)
```

Every item may carry `confidence` (`confirmed`: read in the code or tested in game; `inferred`:
strong evidence; `hypothesis`: a guess worth testing), `sources` (where it comes from, e.g.
`mecmp`, `research`, `dumper`, `ghidra`) and `note`. All addresses are image VAs
(base `0x140000000`), as in Ghidra.

### Types

Members, globals and signatures use a small type language:

| Write | Means |
|---|---|
| `u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool void` | primitives |
| `LinearTransform`, `PamClientCharacterEntity` | a reflected type (by its name) or a struct from `structs:` |
| `T*`, `T**`, `const T*` | pointers |
| `f32[4]`, `u8[0x10]` | fixed arrays |

A signature is `ret(type name, type name, ...)`; names are optional. For virtuals, `this` is
implicit. A function may leave its signature out while it is not known (a name from Ghidra, say):
it then gets an address in `engine.hpp` but no function pointer type, so nothing can call it by
mistake.

### classes

```yaml
classes:
  - name: PamClientCharacterEntity      # must be a reflected Class
    realm: client                        # client | server | any (checked against its creators)
    vtable: 0x141c3c760                  # checked against the dump's vtables
    members:                             # only what reflection does not describe
      - { offset: 0x334, name: ghostShader, type: bool, note: "..." }
    virtuals:                            # offset = byte offset in the vtable
      - { offset: 0x3d8, name: setVisible, va: 0x143229480, signature: "void(bool visible)" }
    properties:                          # names handled through the entity's property hash
      - { name: Visible, hash: 0x35bc69ab, type: bool }   # hash is checked: djb2-xor of the name
```

Rules: a member must fit in the class; a member that falls inside a base class's range produces a
warning (annotate it on the base class); a member must not repeat a reflected field. Annotations
never copy reflected field lists.

### structs

```yaml
structs:
  - name: SpawnInfo
    size: 0x70
    align: 16            # optional (default 8)
    opaque: true         # optional: only the listed members are known
    members:
      - { offset: 0x00, name: transform, type: LinearTransform }
```

Struct names must not clash with reflected type names.

### hooks

```yaml
hooks:
  - name: ServerGameWorld_postUpdate
    kind: vtable-slot          # vtable-slot | creator-slot | pointer
    vtable: 0x141b42d00        # either vtable + offset ...
    offset: 0x28
    # slot: 0x141b42d28        # ... or the slot address itself
    original: 0x143341ee0      # what the slot holds in this build
    signature: "void(void* world, void* updateInfo)"
```

GridNode only ever swaps these pointers; code is never patched. Slots must be 8-byte aligned so
the swap is atomic.

### fingerprint

`fingerprint.yaml` lists what `gridnode.dll` compares before it touches anything: TypeInfo
addresses, vtable slots (by hook name or `Class.virtual`), and the first bytes of functions
(position-independent bytes only). `required: false` makes a check a warning.

## Generated headers

| Header | From | Contents |
|---|---|---|
| `gridnode/engine.hpp` | annotations only | `gn::engine::global`, `function`, `code`, `hook`, `cls::<Class>::off/vf/va/vtable`, `st::<Struct>`, `hash`, `constant`, `fingerprint`. Built with every GridNode build. |
| `gridnode/generated/types.hpp` | your dump + annotations | `gn::fb::` structs for every reflected class and value type with annotated members merged in, enums, annotation structs, `gn::fn::` and `gn::vfn::` function pointer types. Every struct has `static_assert`s for its size and annotated member offsets. |

```
python tools/gen/gridnode.py setup --dump path/to/gridnode-reflected.json
```

installs your dump in `db/reflected/` and writes `sdk/include/gridnode/generated/types.hpp`.
CMake also generates and compile-checks `types.hpp` whenever `db/reflected/gridnode-reflected.json`
exists (`types_compile_test`).
