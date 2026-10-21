# Code review: GridNode, full repository

Date: 2026-10-21. Branch reviewed: `claude/inspiring-pascal-z6xm3q` (identical to `main` at `56a8959`).
This review changes no code. The only file this PR adds is this one.

## Summary

| Severity | Count | Meaning |
|---|---|---|
| High | 1 | Can corrupt game memory or crash in normal use. Fix before relying on the affected API. |
| Medium | 8 | Real defect or hazard that needs specific conditions, a mod that misuses an API, or a hot-path cost. |
| Low | 18 | Robustness, maintainability, test gaps, or minor races. |

Each finding gives the location (`file:line`), what happens, why it matters, and a suggested direction.
Line numbers are from the reviewed commit.

The most important point: **entity pointers outlive the entities they point to.** The inspector never
learns that an entity was destroyed, and several paths (`entities_near`, layout moves, inspector
edits) still write through those pointers. See H1.

---

## High

### H1. Destroyed entities stay tracked and get written to

- `sdk/src/inspector.cpp:5-7`, `70-99`: entries are removed only by `prune()`, which uses the
  vtable heuristic "the object still has the type it was created as".
- `sdk/src/inspector.cpp:1102-1103`, `sdk/src/ui.cpp:406`, `sdk/src/ui.cpp:448`: `prune()` runs only
  inside `inspector::draw()`, which runs only from `drawMain()`, which runs only when the menu is open.
  With the menu closed, the list grows for the whole session until it reaches the 400,000 cap
  (`inspector.cpp:56-58`), and new entities are then dropped silently.
- `sdk/src/world.cpp:524-538` (`entitiesNear`): calls `fn` for every tracked pointer whose transform
  reads. A freed object can still pass that check. `sdk/include/gridnode/api.h:358` documents the
  callback as "every **live** entity", which the code does not guarantee.
- `sdk/src/layouts.cpp:250-264` (`runQueue`) and `sdk/src/world.cpp:540-558` (`entitySetTransform`):
  a queued move checks only that the vtable still matches, then calls `SpatialEntity_setTransform`.
  If the object was freed and the vtable word survived, the game function runs on freed memory.
- `sdk/src/inspector.cpp:150` and `:200`: edits capture the raw address and write it later on the
  client thread, with no liveness check at write time.

**Why it matters:** a stale pointer write is heap corruption. The vtable heuristic is not a
liveness test: allocators often overwrite the first word of a freed block, and a reused block can
hold the same class again. The 400,000 cap makes the leak bounded but still large.

**Suggested direction:** prune on a tick (not a UI draw), filter `entitiesNear` and `tracked()` by
liveness, and re-validate type and liveness inside the posted lambda on the client thread. The
durable fix is a destroy hook on the engine's entity removal path, so entries are removed rather
than inferred from memory.

---

## Medium

### M1. Entity creation dispatch re-enters a shared lock

- `sdk/src/entities.cpp:65-67`: `factoryDetour` holds `g_lock` shared while it calls each mod listener.
- `sdk/src/entities.cpp:135`: `onCreated` takes `g_lock` exclusive.
- `sdk/src/entities.cpp:154`: `refreshCreators` takes it exclusive about once a second
  (`sdk/src/runtime.cpp:232-233`).

A listener that calls `on_created`, or creates another entity of a hooked class, re-locks the
same `std::shared_mutex` on the same thread. Recursive shared locking is undefined behaviour for
`std::shared_mutex`, and a waiting writer can block the nested reader, which deadlocks the game
thread.

**Suggested direction:** copy the listener list under the lock and call listeners after releasing
it, or defer `onCreated` calls made during dispatch.

### M2. Exceptions can cross into engine frames

- `sdk/src/ticks.cpp:28-44`: the server and client post-update detours call `messagesServerTick`,
  `layouts::serverTick`, `worldClientTick`, `messagesClientTick`, `faithClientTick`, and
  `layouts::clientTick` with no `try`. Only `Dispatcher::dispatch` catches, and only around
  listeners and tasks (`sdk/src/dispatch.cpp:34-53`).
- Other mod-facing entry points have no isolation either: `sdk/src/ui.cpp:412-425` (`callMods`),
  `sdk/src/world.cpp:117-119` (`callLevel`), and `sdk/src/entities.cpp:66-67` (`factoryDetour`).

An exception that escapes a detour unwinds through game frames compiled without that in mind. A
throwing mod UI or level callback can take down the game inside `Present`.

**Suggested direction:** wrap each detour body and each callback invocation in `try`/`catch`,
log through `GN_LOG`, and keep the game running. Make the policy the same everywhere.

### M3. Hook slots are not restricted to image data

- `sdk/src/mem.cpp:60-69` (`patchPointer`): accepts any 8-byte-aligned address. It calls
  `VirtualProtect(PAGE_READWRITE)` on the whole page, which drops execute permission for the window
  of the write.
- `sdk/include/gridnode/hook_table.hpp` / `sdk/src/hook_table.cpp:22-26`: checks alignment and
  readability only. With `expected == 0`, it checks only that the *current* contents are a game
  function.
- `sdk/src/api.cpp:106-117` (`apiHookSlot`): mods may pass any VA.

The `mem.cpp` comment says the function swaps "one pointer in read-only image data". Nothing
enforces that. A typo in a slot VA silently rewrites an unrelated page and changes its protection.

**Suggested direction:** require the slot to lie in a non-code section of the image (see
`mem::imageSections()`), and reject everything else with `GN_ERR_SLOT`.

### M4. Layout values bypass finiteness checks on the body and group paths

- `sdk/src/layouts.cpp:370-372` (`parseVec`) and `:404-419` (`load`): `sscanf "%f"` accepts `nan`
  and `inf` on conforming C runtimes. Nothing rejects them.
- `sdk/src/layouts.cpp:216-220` (`normalize`): `n > 1e-6f` is false for NaN, so NaN rows pass through
  unchanged. `moveBodies` (`:224-248`) then calls `RigidBody` `setTransform` with them (`:242`).
- `sdk/src/layouts.cpp:518-530` (`writeInstance`): `if (n < 1e-6f) return;` is false for NaN, so NaN
  is written into compound-shape instance data (`:529`).

`entitySetTransform` (`world.cpp:541`) does reject non-finite values, so the paths disagree.

**Suggested direction:** validate each parsed edit once in `load()` (finite and orthonormal, or
reject the edit with a log line), so every path sees clean data.

### M5. Game-state writers skip the realm-thread and finiteness checks

- `sdk/src/faith.cpp:215-227` (`keyPress`) and `:331-342` (`actionPress`, via `applyActions` at
  `:273-292`): write the player's input state on the caller's thread.
- `sdk/src/faith.cpp:392-403` (`movementSet`): writes speed, state and ground fields with no finite
  check and no thread check.
- `sdk/src/faith.cpp:408-417` (`positionSet`), `:486-490` (`timeOfDaySet`), and `:508-514`
  (`tuningSet`): write game floats with no finite check. `tuningSet` accepts NaN and infinity.

`teleport` (`world.cpp:283-288`) and `entitySetTransform` (`world.cpp:540-551`) do check both. The
same contract should apply to all of these, or the API documentation should say these functions
are unchecked.

**Suggested direction:** reject non-finite inputs in each setter. Either post the write to the
owning realm's thread or document the caller-thread contract.

### M6. Asset refcounts and releases act on unverified pointers

- `sdk/src/assets.cpp:43-55` (`assetFind`): walks the partition's list to an asset pointer with no
  vtable or type check, then `_InterlockedIncrement`s `asset + 8`.
- `sdk/src/assets.cpp:60-64` (`assetRelease`): passes any pointer a mod gives it to the game's
  `DataContainer_release`.

A stale or invalid pointer corrupts memory. A double release underflows the count and can free an
asset that is still in use.

**Suggested direction:** check the vtable or reflected type before touching the count. For
`assetRelease`, track handed-out assets (a small set) and refuse pointers that were not returned by
`assetFind`.

### M7. Layout spawning trusts game-supplied pointers and counts

- `sdk/src/layouts.cpp:704-707` (`memberType`) and `:743-746` (`makeStandalone`): the data pointer
  is only null-checked before `ServerEntityCreationInfo_ctor` and `spawnIntoBus` construct game
  objects from it (`:711-737`).
- `sdk/src/layouts.cpp:558-563` (`memberTransform`): the element count is read from game memory and
  used to bound a write at `arr + copy * 0x40` (written at `:622-626`).
- `sdk/src/layouts.cpp:567-577` (`renderObjectOf`): indexes a list at `copy * 8` with no bound from
  the list's length.

**Suggested direction:** check each pointer with `registry().typeOf` before construction. Bound
every game-supplied index by its container's count, and refuse the operation otherwise.

### M8. Per-tick and per-frame paths make many kernel calls

- `sdk/src/world.cpp:265-281` (`entityTransform`): three `ReadProcessMemory` calls per entity.
  `entitiesNear` (`:524-538`) calls it for every tracked entity, and each call first copies the
  whole tracked list (`sdk/src/inspector.cpp:1086-1092`). With a few hundred thousand tracked
  entities, one call is several hundred thousand syscalls. A mod that calls `entities_near` every
  tick will stall the game.
- `sdk/src/layouts.cpp:1322-1328` (`firstViewManager`) is called every frame from
  `onCameraUpdated` (`:1330-1343`) and from `cursorRay` (`:1517`), using `mem::readOr` (kernel)
  for image globals. `mem::readFast` (`mem.hpp`) is the in-image path and would avoid the syscall.
- `sdk/src/layouts.cpp:786-838` (`findGroupProp`) scans the tracked list once per group and again for
  each client group. `applyGroupEdits` (`:841-868`) calls it per edit on the server thread at level
  start.

**Suggested direction:** keep a spatial index for `entitiesNear` (or at least avoid the copy),
use `readFast` for image-resident data, and build the group lookup once per level instead of per
edit.

---

## Low

### L1. Queued moves survive a level change
`sdk/src/layouts.cpp:1239-1270` (`onLevel`) clears edits and the group bus but not `g_serverQ` or
`g_clientQ` (`:165`). A move queued just before the level changed is applied to whatever object
reuses that address, if its type matches (`runQueue`, `:258`). Clear both queues in `onLevel`.

### L2. A normal object at a group-prop edit is recorded as a group prop
`sdk/src/layouts.cpp:1116-1119` matches `ed.kind == "GroupProp"` by position for any picked object,
then sets `s.spawned` from that match. A moved normal object that lands within 5 cm of a group-prop
edit gets saved under the wrong kind. Match on kind first, or key group props separately.

### L3. Detached threads run outside the realm threads
`sdk/src/layouts.cpp:979` and `:1284` run `applyToExisting` on detached threads. It reads entity
transforms and iterates `inspector::tracked()` concurrently with the game, which the file header's
rule about realm threads does not allow. The work is read-only and posts to the realm queues, so
the practical risk is torn reads, not corruption. Move it onto the server thread's queue.

### L4. The layout editor does file I/O and scans on the render thread
`sdk/src/layouts.cpp:1585-1589` lists layout files every second. `unloadLayout` (`:923-959`) and
`loadLayout` (`:962-980`) walk every tracked entity with kernel reads, from a button handler on the
render thread. Expect frame hitches on large levels.

### L5. Failed mods stay loaded and active
`sdk/src/mods.cpp:57-79`: a mod whose `gn_mod_load` returns failure stays loaded and pinned. Any
hook, listener, or callback it registered before failing keeps running. The comment at `:67` states
the design; the failure path should at least disable what the mod registered, or unload it.

### L6. Dispatcher data races
`sdk/src/dispatch.cpp:55-57` erases from `listeners_` outside the lock that `listenerCount()` takes
(`:18-21`). `ticks_` is a plain `uint64_t` written on the tick thread and read by the UI
(`sdk/src/ui.cpp:401-402`). Both are formally data races. Make `ticks_` atomic, and do the erase
under the lock.

### L7. Mod listener errors cannot name the mod
`sdk/src/api.cpp:89` and `:94` register every tick listener with owner `"mod"`, and the tick API
takes no mod name, so "tick listener threw" never says which mod threw it. Pass the name through
the API, as the other registration calls do.

### L8. Creator hook maintenance repeats every second
`sdk/src/entities.cpp:108-126` and `:145-162` retry failed slot hooks and log each failure on every
`refreshCreators` call (about once a second). `boundCreatorCount` and `refreshCreators` each rescan
every type (`sdk/src/entities.cpp:97-105`, `:164-170`; `sdk/src/runtime.cpp:232-234`). The fixed
pool of 2,048 (`entities.cpp:45`) silently stops hooking when full, with one log line per second.
Cache failures and cap the retries.

### L9. Unsynchronized `g_real` in the version proxy
`sdk/loader/version_proxy.cpp:32-50` writes `g_real` without synchronization. `loadCore` (`:62`) and
the forwarders (`:75-80`, which can each call `realVersion`) can race. `LoadLibrary` is reference
counted, so the result is the same module, but the access is still a data race. Use `std::call_once`
or a `static` local.

### L10. The injector ignores a failed path build and leaks on early returns
`sdk/loader/inject.cpp:198`: `wcscat_s` return value is not checked. A long executable path leaves
the DLL path empty, and the user sees "gridnode.dll not found" for the wrong reason. `inject`
(`:153-172`) returns early without releasing the remote buffer or the process handle. Both are minor,
but the first produces a misleading message.

### L11. The crash handler can deadlock on the log mutex
`sdk/src/crash.cpp:40-69` calls `GN_LOG`, which takes the log mutex (`sdk/src/log.cpp:85`). A fault
raised while the faulting thread already holds that mutex deadlocks. The handler is first-chance and
process-wide, so it also logs faults the game handles itself, counting toward the three logged. Use a
lock-free path or skip logging when the log lock is already held by this thread.

### L12. `readCString` misses strings near unmapped pages
`sdk/src/reflect/walker.cpp:21-35` reads 64-byte blocks. If a block crosses into unreadable memory,
it returns an empty string even when the NUL is inside the readable part. Read to the page boundary,
or read byte-by-byte near the edge.

### L13. The inspector's typed integer write trusts the reflected size
`sdk/src/inspector.cpp:186-205`: `size` comes from reflection. A size above 8 makes
`mem::write(at, &w, size)` read past the 8-byte local, and a size of 0 is replaced with 4 whatever
the type. Clamp to the type's real width.

### L14. Rust mods abort the host on panic
`bindings/rust/gridnode/src/lib.rs:52-60` (`gn_mod`), `:95-100` (`tick_tramp`), and the other
`extern "C"` trampolines let a panic unwind out of an `extern "C"` function, which aborts the game.
Wrap each user closure in `std::panic::catch_unwind` and log the failure.

### L15. The PE reader zero-fills truncated reads silently
`tools/gen/gridnode_gen/pe_image.py:28-33` pads short reads with zeros and reports nothing. A
truncated dump produces zeros where pointers belong, with no error. Raise when the file is shorter
than the section claims.

### L16. Layouts hard-code game addresses and offsets outside the fingerprint
`sdk/src/layouts.cpp:570-576` (`+0xc8`, `+0x20`, `+0x210`) and `:582-605` (`0x141a4e8dc`, the
update-buffer offsets) are not in `db/annotations` and not covered by the build fingerprint, which
gates everything else. A game patch would break these silently. Move them into annotations, and
fingerprint the functions they call.

### L17. Magic numbers in the standalone-copy path
`sdk/src/layouts.cpp:760-763` hides a group copy by moving it 15,000 m down. That is a magic value
that can overlap real level geometry. Use a named constant, or disable collision and render through
the game's own visibility path.

### L18. Large mixed-responsibility files
`sdk/src/layouts.cpp` (1,666 lines) combines the file format, physics bodies, render submission, the
camera hook, and the editor UI. `sdk/src/inspector.cpp` (1,149 lines) does the same for tracking,
UI, and console. Splitting them would make the findings above easier to fix and test.

Test coverage also has gaps; see "Test coverage" below.

---

## Positive notes

- The memory model is consistent. Game memory is read with fault tolerance, and the one
  always-mapped path (`readFast`) is limited to the image.
- `build::check` gates the whole runtime on a fingerprint of the game build, so a different game
  version fails closed (`sdk/src/runtime.cpp:191-196`).
- Hook chaining (`hook_table.cpp`) refuses a slot that someone else changed, and publishes the
  original before the swap.
- The version proxy and injector are careful about loader-lock and about reporting failures.
- The layout file format is strict about bad lines (`layouts.cpp:388`, `:418`), and save and load
  validate names (`validName`).
- YAML is read with `safe_load` (`tools/gen/gridnode_gen/annotations.py:260`).
- CI runs the reflection walker under ASan and UBSan.

---

## Test coverage

Covered (and passing here, see below): the type registry, the hook table, the dispatcher, and the
walker, plus the generator's 30 unit tests.

Not covered by any test:
- layout parsing, `validName`, `parseVec`, `reframe`, and the save/load round-trip (L1, L2, M4)
- the entity creator slot pool and its listener attachment (M1, L8)
- inspector pruning and liveness (H1)
- the mod loader, including failure paths (L5)
- faith's action and key bit logic (M5)
- anything under `#ifdef WIN32` (the game code), which the Linux CI cannot run

The threaded code (realm threads, the render thread, and the detached threads) has no sanitizer or
thread-sanitizer coverage. Most of the Medium findings are thread-discipline problems that a test
on Linux could not catch today.

---

## Verification performed

- `tests/reflect_walker_test.cpp` built with `-fsanitize=address,undefined -Werror` and run: all
  checks passed.
- `tests/runtime_core_test.cpp` built with `-Wall -Wextra` and run: all checks passed.
- `python3 -m unittest discover -s tools/gen/tests -t tools/gen`: 30 tests, OK.
- The Windows DLLs were not built, because the cross toolchain is not available in this environment.
  Every Windows-only finding above comes from reading the code, not from running it. Each cited line
  was read in this review.

## Not reviewed in depth

- `sdk/src/inspector.cpp` lines 215–1080 (the drawing code), `sdk/src/settings.cpp` 80–289,
  `sdk/src/messages.cpp` 1–200 and 260+, `sdk/src/ui.cpp` 60–360 (input and mouse handling),
  the reflection walker's internals beyond the sampled parts, and `sdk/include/gridnode/mod.hpp`
  beyond the first 120 lines.
- `tools/gen/gridnode_gen/` other than `pe_image.py`, `cli.py`, and the subprocess call sites
  (`emit_*`, `ghidra_sync.py`, `annotations.py` validation logic).
- `tools/ghidra/*.java` (about 4,300 lines), `examples/*`, `docs/*` content, and `db/annotations/*`
  content (only the structure was checked).
- `bindings/rust/gridnode/src/sys.rs`, which is generated.

A follow-up pass on those areas would be worth doing, and would most likely add Low-severity items
rather than change the High or Medium ones.
