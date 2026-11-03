# Focused review: `sdk/src/layouts.cpp` and `examples/mec-trainer/volumes.cpp`

Date: 2026-11-03. Reviewed: `origin/main` at `5f9c89b` (API 17). Review only: no code was changed.
Line numbers are from that commit.

## Summary

| Area | High | Medium | Low | Notes |
|---|---|---|---|---|
| `layouts.cpp` (6,072 lines) | 0 | 3 | 9 | Several stale-pointer uses and one unchecked game call. Most earlier fixes hold. |
| `volumes.cpp` (888 lines, example mod) | 0 | 3 | 3 | Read-only with respect to the game, but its scans and caches cost a lot on the client tick. |

No finding here can corrupt memory in a common path. The Medium findings are edge cases that need
a group, bus, sublevel or barrier to stream out or change at the wrong moment. Several are one-line
liveness checks away from being fixed.

## How the review was done

- `layouts.cpp` was read in full from the start through the free camera, the copy and undo code, the
  object library and the host interface. The barrier and magrope section and the outline and handle
  code were checked by grep and by reading their entry points. Gaps are listed at the end.
- Every game-function call and every write to game memory was listed by grep and checked for a
  liveness, type or bounds check.
- `volumes.cpp` was read in full up to the render section, which was read only at its entry point.
- Earlier fixes from `reviews/2026-10-21-code-review-response.md` were re-checked where they touch
  these files: the queue clearing, the copy and undo liveness checks, the bounded group member lookup,
  the usable-transform checks, and the server-thread posting in place of detached threads.

## Layouts: findings

### LY-1 (Medium): group lookup cache used for writes after it may be stale

`findGroupProp` (1099) keeps the server group and client group lists for up to one second
(`serversCache`, `clientByDataCache`, 1110–1126). The cached pointers are used from line 1131 on
without an `inspector::alive` check. The result drives `moveGroupProp` (851), which writes the group's
instance data (`writeInstance`, 873–874) and the member's transform row, and `makeStandalone` (1049).

If the group streams out inside that second, these writes go to freed memory. `writeInstance` and
`mem::write` are fault-tolerant, so this writes into whatever now owns the address rather than
faulting.

**Suggested fix:** check `inspector::alive` for the server and client entities right before they are
used (after the cache), or cache the entity list and re-check each entry.

### LY-2 (Medium): bus and data pointers used to spawn game objects after they may be freed

`makeStandalone` (1049) takes the group's bus from `busOf` (974), which reads a map GridNode fills
when a group is created and clears only when a level starts. It checks only that the bus is
non-null. It then calls `spawnServer(data, sbus, to)` (1062). The client half is spawned later, on
the client thread (lambda at 1074), with `data` and `cbus` captured at post time. Nothing checks
that the bus or the data is still alive when that lambda runs.

A sublevel that streams out between the post and the spawn would give the game constructor
(`ClientEntityCreationInfo_ctor`, `ClientEntity_spawnIntoBus`) a freed bus or data object.

**Suggested fix:** check the bus's owning sublevel (or the group entity) for liveness on the realm
thread before calling the constructors, and check `data` with `registry().typeOf` at that point too.

### LY-3 (Medium): barrier placement calls game functions on an unchecked owner

`placeBarrier` (4117) finds `owner` in the physics manager's owner table at
`owners + ((id >> 16) & 0xff) * 8` (4127). It checks only that the result is non-null. It then passes
`owner` as the `this` argument of `RigidBody_transformSource` (4130) and
`RigidBody_setTransformNative` (4131).

Nothing checks that the table has that many entries, or that the owner has the class those functions
expect. The sibling code, `forEachBody` (258–281), checks the body with `registry().typeOf` and
`isKindOf`, so this call site is out of line with the rest of the file.

**Suggested fix:** bound the index by the table's length, and check the owner's type (or its vtable)
before the calls, as `forEachBody` does for bodies.

### LY-4 (Low–Medium): sublevel list validated by type only, then used to stream sublevels

`subLevels` (1422–1429) returns cached sublevel pointers after a check with `registry().typeOf` only
(1426–1429). It does not check `inspector::alive`. `streamIn` (1484) then calls the game's sublevel
`onEvent` (`g_subOrig`) on the cached pointer. A sublevel freed in the last second is still called.

**Suggested fix:** use `inspector::alive` in the cached branch, as the uncached branch already does.

### LY-5 (Low–Medium): pick can select a dead physics entity

`entityOfBody` (1943–1952) returns the first tracked physics entity whose body list matches, with no
`inspector::alive` check. A freed entity whose component memory still matches can be picked. The edit
is then recorded against it. The later moves are safe (`runQueue` checks liveness), but the selection
and the saved layout are wrong.

**Suggested fix:** add `inspector::alive(p, t)` to the loop condition.

### LY-6 (Low–Medium, cost): whole-list copies on the render thread

- `scanBarriers` (4012) copies the entire tracked list (4022) once a second while
  "Show barriers and magropes" is on (`drawBarriers`, 5803). The copy can hold 400,000 entries.
- `drawLibrary` (3150) copies the whole library each frame it is drawn (`lib = g_lib`, 3175) while the
  Library header is open.

Both run on the render thread.

**Suggested fix:** filter the list while walking it (as `inspector::trackedOf` does) instead of
copying it, and keep a per-frame copy only of what is drawn.

### LY-7 (Low): sublevel pin hooks only the first vtable

`hookSubLevels` (1397) installs the `onEvent` wrapper on `t->vtables[0]` only (1403). An object whose
vtable is another `ServerSubLevelEntity` vtable (a subclass, or a second instance of the same class
with a different vtable) is not pinned, so its stream-out is not refused.

**Suggested fix:** wrap every entry of `t->vtables`, or confirm that only one vtable exists in the
build and check that when the hook is installed.

### LY-8 (Low): factory call without a type check

`fillGroupUpdate` (around 760) calls the factory's vtable slot `+0x18` on `factory`
(`block + 0xb8`). It checks only that the function address is in the image (`mem::inImage(ffn)`), not
that `factory` has the expected class.

**Suggested fix:** check `factory` with `registry().typeOf` before reading its vtable.

### LY-9 (Low): buffer sizes assumed in the free camera

- `camOutput` (3469) writes two `gn_transform` values into the `out` buffer the game passes in
  (3475–3476). The size of that buffer is assumed (MECMP's observed layout), not checked.
- `camFrameUpdate` and the body hook copy `kCamSlots = 160` (3411) and 96 vtable entries from the
  game's classes (3513). A class with more virtual functions than that would call past the copy.

These match the game build this was written for. The build check does not cover them, so a game
update would fail silently here.

**Suggested fix:** check the class's slot count from its reflected type or its vtable size, and skip
the swap when the count is larger than the copy.

### LY-10 (Low): layout and library files are not escaped

`save` (430) writes `source`, `mesh`, `kind` and `level` without escaping. A value with a newline, or
with ` | ` in a library entry, corrupts the file. `saveLibrary` (2950) has the same problem. The
library UI replaces newlines in names (3159–3165), but nothing protects the other fields.

Names come from the game (mesh and source names) and from the user (layout and library names), so
this can happen with a hand-edited or unusual asset name.

**Suggested fix:** reject or escape those characters when a file is written.

### LY-11 (Low): game reads under the layout lock on every model creation

`onCreatedModel` (378) calls `levelName()`, which reads game memory, while it holds `g_m`. It runs on
the game's creation thread for each static model created while a layout is loaded. The read costs a
few kernel calls per creation.

**Suggested fix:** read the level name once per level change (in `onLevel`), and compare the cached
string.

## Layouts: checked and holds

- The move queues are cleared when a level starts (`onLevel`, 3280), and every
  queued move re-checks liveness when it runs (`runQueue`, 350).
- Copies re-check `templateAlive` at spawn time (`spawnCopy`), so a stale clipboard template is refused.
- `memberTransform` and `renderObjectOf` bound their indexes by the game's own counts.
- `usableTransform` guards every transform the game is given (layout load, body moves, instances).
- Zone focus registration and the streamer flags check liveness on the streamer's own thread.
- The camera and the body vtable swaps are restored in `release`, and the swap uses an 8-byte
  aligned store, so a running thread sees either the old or the new pointer.
- The earlier detached threads are gone: load and apply run through `server().post`.
- Undo removes an edit by looking it up again (`editOf`) rather than by a saved index.
- The barrier and magrope transform writes check liveness and type first.

## Volumes: findings

`examples/mec-trainer/volumes.cpp` is sample code. Its findings matter to anyone who copies it.

### V-1 (Medium, cost): four full scans every quarter second

`clientTick` (574) calls `gn::entitiesOfClass` for three classes (597) and once more for
`OBBCollisionEntity` (636). Every 0.25 s that is four passes over the tracked list. Each pass is
`inspector::trackedOf` (`world.cpp` 550–561), which holds the tracked-list lock for the whole walk.
Entity creation on the game's threads waits on that lock.

**Suggested fix:** scan once a second, or give the engine an index by class. At the very least, scan
only when the level or the tracked count changes.

### V-2 (Medium, memory): every bus's connections are cached without a cap

`connectionsOf` (243–262) reads every connection of a bus into a vector and keeps it in `g_conns`
until a level change. The guard allows up to 4,000,000 records. Each `Conn` is 24 bytes, so a large
root bus can use about 96 MB. The first query on the client thread also pays the full read.

**Suggested fix:** cap the cache, or keep only the connections for the buses that the nearest triggers
actually reach.

### V-3 (Medium, cost): unbounded scan of a bus's connections

`classify` falls back to scanning all connections of a bus for each event step that
found no direct match (375). The step count is bounded (depth 4), but the scan is not, so one trigger
can cost a full pass over a big bus. `classify` runs on the client tick, a few triggers per tick.

**Suggested fix:** index each bus's connections by event id when it is read, and look the event up
instead of scanning.

### V-4 (Low): the research log bypasses the per-tick budget

The research path in `clientTick` (716) classifies every unclassified volume in one tick, ignoring the
24-per-tick budget (706). Pressing the log key on a level with many triggers causes one long client
tick.

**Suggested fix:** reuse the budget, or move the work to a later tick.

### V-5 (Low): unloading the trainer keeps the caches until the next tick

`unload` (570–572) sets `g_on` only. The caches are cleared by the next `clientTick`, and only if that
tick runs again. If the trainer is unloaded and the level keeps going, `g_conns` (V-2) stays resident.

**Suggested fix:** clear the caches in `unload` itself, on the client thread.

### V-6 (Low): names and links cached by raw pointer

`g_names` (268–272) and `g_conns` are keyed by raw pointers. When memory is reused for another
object, the cached name or links are stale until the level changes.

**Suggested fix:** key by pointer and class, or clear the affected entries when an object is seen
dead.

## Volumes: checked and holds

- Read only: a grep for game calls and writes found none. Every game read goes through the
  fault-tolerant `gn::read` or `gn::api().read`.
- Recursion is bounded: `classify` and `describe` stop at depth 4 and at 120 log lines.
- Pointer arithmetic on game-supplied counts is clamped (`std::min(..., 32)`, `256`, `4000000`).

## Not reviewed in depth

- `layouts.cpp` 3700–3820 (the rest of the free camera input), 4040–4117 (barrier scan details),
  4142–4192 (magrope placement details), 4192–5033 (barrier and magrope edits, read by grep only),
  5033–5565 (key binds and the selection outline), 5600–5803 (handles), 5830–5971 (barrier drawing
  details), and 6008–6072 (`centreRay` and `release` details).
- `volumes.cpp` 725–888 (the render tab and the on-screen list).

These parts make up about 40% of `layouts.cpp` and about 15% of `volumes.cpp`. The grep checks above
covered their game calls and writes.
