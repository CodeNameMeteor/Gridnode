# ANT: its type registry and the move tracker (research, 2026-11-05)

ANT is the animation and movement system Faith's parkour is built on (the Pamplona drivers, processors
and the stateflow graph). It has its own reflection, separate from Frostbite's TypeInfos. Starting
point: the user's ANT reverse-engineering handbook (2026-11-05); every address below was checked in
Ghidra the same day. Annotations: `db/annotations/ant_runtime.yaml` (registry, pipeline, factories,
subsystems) and `ant.yaml` (the asset definitions).

## The type registry

- `g_antTypeRegistry` (0x1423bd9b0) points at the registry object (returned by 0x142c2f840).
- **Name map** at +0x00 (begin) / +0x08 (end): sorted 16-byte entries `{u32 hash, pad, Schema*}`.
  The hash is FNV-1 (`h = h * 0x1000193 ^ c`, seed 0x811c9dc5; `ant_hashString_fnv1` 0x143fbc460,
  mode 0). `ant_TypeRegistry_getTypeByName` (0x142c2d4b0) binary-searches it.
- **Id map** at +0x28, keyed by each layout's CRC-32.
- A **Schema** is 0x10 bytes: `{layout, factory}`.

`ant_TypeRegistry_registerType` (0x142c352b0) → `ant_registerSchema` (0x142c0c3c0), which prepends
`__guid` (-3), `__name` (-2) and `__base` (-1) to the declared fields, → `ant_createClassLayout`
(0x142fb0d60) → `ant_buildClassLayout` (0x142fb0f00) and its field pass (0x142fb1880).

### A layout

| Offset | Header |
|---|---|
| +0x00 | i32 lowest field index (-3) |
| +0x04 | i32 highest field index |
| +0x08 | data size |
| +0x0c | alignment |
| +0x10 | string table offset (from the layout) |
| +0x14 | string table size |
| +0x18 | pack flag (fields placed by alignment, not in declaration order) |
| +0x1c | CRC-32 of the layout: ANT's type id (`ant_computeSchemaCrc32` 0x142fb0800) |

Then one 0x20-byte record per index, from the lowest:

| Offset | Record |
|---|---|
| +0x00 | the field type's id |
| +0x04 | element size (0x10 for a reference, flags & 1) |
| +0x08 | offset in the type's data |
| +0x0c | name offset in the string table (0: no name) |
| +0x10 | count (array length) |
| +0x12 | flags: 1 reference; 2 / 4 / 8 at least 4 / 8 / 16 aligned |
| +0x14 | alignment |
| +0x18 | the field type's layout |

The string table starts with a 0, then the type's name, then the field names. The descriptor the
definitions pass (`ant_initFieldDescriptor` 0x142faf9e0, 0x20 bytes) is `{index, count, name, type,
flags}`; the handbook's "alignment" and "offset" are the count and the flags.

Most driver and processor fields are references to value assets (the field type is a pointer-like
layout), not plain floats: their value is in the asset they point at.

**GridNode:** `gridnode/ant_types.hpp` reads all of this (host-tested), `sdk/src/ant.cpp` keeps the
registry once read; mods get it through `ant_type_*` / `ant_field_*` (API 25) and the console has
`ant [filter]` and `ant TypeName`. Checked in game 2026-11-05: 936 types, 4,511 fields.

**A layout's offsets are not places in the live C++ object.** FallThreshold is at 0x94 in
RayCastCollisionProcessorAsset's layout but +0x40 in the live asset; Timespan at 0xac in
PositionPredictionProcessorAsset's but +0x10 live. The differences disagree (0x54 and 0x9c), so the
layout describes ANT's asset data (base fields first), not the object the game runs with, and
`instance_offset` is -1.

**Where a field lives (the console's probe, 2026-11-05, untested in game).** A Schema's factory leads
to the type's function table: +0x00 makes an object (`FallingDriverAsset_create` 0x142efd9a0: no
arguments, 0x140 bytes, `ant_DriverAsset_ctor`), +0x08 loads it: `load(?, object, view, resolver)`
(0x142eee0b0 for FallingDriverAsset). The load reads each own field through 0x142fb0110 (view +0x10
the layout, view +0x1c a u16: the data's offset from the view; it returns data + the record's
offset) and either copies it to a fixed place (`JumpHeight` +0xe8, `AirControlSpeed` +0xc0, bool
`CompensateForHighSpeed` +0xd8) or calls `resolver->vtable[1](resolver, object, fieldData, dest,
typeHash, 0)`: a reference, a pointer to another asset (`GravityY` +0xb0, hash 0x7062fa7a; `FallMetrics`
+0xe0). Base fields are loaded by the base's own loader, not this one.

`ant TYPE FIELD` uses this: it makes one object with the type's create, runs its load on test data
(zeros, then each field set alone: the bytes that change are the field's place; a resolver call names
a reference's place), finds the live objects by the vtable create set (a scan of the game's
read-write memory, up to 256 objects, 15 s), and prints the field in each. `ant TYPE FIELD VALUE`
writes 1-byte (bool) and 4-byte (float, or int when the type name says so) values into every object.
References cannot be set yet: the print shows the pointer, the target's vtable and its first bytes,
which is the research for that (GravityY is one). Limits: own fields only; 8-byte fields are left
unplaced (often pointers into the asset); a type whose loader reads through its data could crash
(the log names the type first); objects that share the base vtable (FallingDriverAsset uses
DriverAsset's) may include other types; drivers may copy a value when a move starts.

## The move tracker

`PamClientMoveTrackerComponent` (vtable 0x141c3d368, 0x200 bytes) reads six ANT values every tick,
named by its data's `AntBindings` (`PamMoveTrackerAntBinding`, TypeInfo 0x142884938, 0x78 bytes:
CurrentMove, DamageType, MovePosition, MoveNormal, MovingSpeedXZ, IsInJumpState). Its init (vf37,
0x143917410) finds Faith's ClientAntAnimatableEntity (+0x78) and binds them; the reader
(0x144795f00) writes them to +0x90 (move), +0x94 (damage type), +0xa0 / +0xb0 (position, normal),
+0xc0 (speed), +0xc4 (in a jump); the update (0x1439189e0) publishes the move at +0x60 (and +0x150),
the speed at +0x64, the distance moved this tick at +0x68 and the jump flag at +0x6c, and counts
each move it enters (+0x170, moves 1..31, sent to telemetry every 30 s).

**GridNode:** `move_tracker` (API 25); the MEC trainer shows it on its Player tab. **Not yet checked
in game**, including whether CurrentMove is the same number as `gn_movement.state` (1 ground,
2 jumping, 8 wall run, 25 swing).

## The handbook, corrected

- `CharacterStateType` (0 OnGround, 1 Jumping, 2 InAir, ...) is a Frostbite enum, not the value
  `gn_movement.state` reads (1 ground, 2 jumping, 8 wall run, 25 swing; recorded in game).
- `PamMoveTrackerAntBinding` is at TypeInfo 0x142884938 and has six fields (0x78 bytes): there is
  no `MoveConditions`.
- Constructors: only Wallrun (0x142ee4ba0), Walking (0x142ee4a60) and Vault (0x142ee4880) are
  function starts. Coil's is the shared `ant_EventDriverAsset_ctor` (0x142cc19a0), Springboard's
  the base `ant_DriverAsset_ctor` (0x142ee12b0); the others fall inside other functions.
- MagropeSwing's and MagropePullUp's create functions (0x142efeb80, 0x142efeac0) are not
  function starts.
- Springboard and Falling share a type id in the table (0x6ef2ac2d), as do LedgeAlign and
  FollowSpline (0x75512aed): the type id is each layout's CRC, so one of each pair is a copy error.
- `ant_initLocomotionTagsSubsystem` and `ant_initCollisionGeometrySubsystem` are unnamed in Ghidra
  (0x142d2a990, 0x142af7f50); their names come from the strings they use.
