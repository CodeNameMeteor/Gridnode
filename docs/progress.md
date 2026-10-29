# Progress / handoff

Update at the end of each session. Keep it short. The full handover for a new agent is
[handover.md](handover.md).

## Current task
2026-10-29: handover steps 1 to 4 done. Test build `gridnode-watch4.dll` deployed to
`D:\Dev\GridNode\bin\gridnode.dll`: destroyed lines, objects, list watches and the tracing fix are
confirmed in game; the class picker and every-frame watches are not yet.
**GridNodeSync confirmed in Ghidra 12.1.4** (2026-10-29, run through the MCP with script arguments):
export (632 functions, 30 labels; backup in `bin/ghidra-test/my-names-before.json`), annotations import
(45 structs, 124 names, 539 of the user's kept, 573 comments, 0 problems), a recording's import (515
comments, 606 bookmarks on the call instructions); a second import changes nothing; the export reads
back with `ghidra-import`. The user's `~/ghidra_scripts/GridNodeSync.java` is the new one (old as `.bak`).

## Done recently
- **Recordings into Ghidra** (2026-10-29): `summarize.py REC --ghidra OUT --exe DUMP` writes a GridNodeSync
  import: `createFunctions` (unwind-table starts, `PeImage.function_start`, chained entries followed),
  `bookmarks` (call before each return address, category "GridNode recording"), `comments` (plate block
  `GridNode [recording] (<file>)`, its own block key). GridNodeSync reads those, and takes script
  arguments `import FILE` / `export FILE` (no dialogs). Confirmed in Ghidra (above). The Scylla image's exception directory
  is intact (RVA 0x2630000, 0x17e974 bytes).
- **Recorder: destroyed entities and the object table** (2026-10-29): `destroyed` lines from the
  destructor hooks (entities.cpp calls `recorder::onDestroyed`); every pointer an event names gets an
  object id (`obj`), exported as `object` lines (type, data, blueprint, created / destroyed / first seen;
  a reused address gets a new id). `summarize.py --object ID|0xPTR` tells one object's story.
- **Recorder: class picker** (2026-10-29): "..." beside every class field (trace, watch, send) lists
  classes alive now / in this recording / all, searchable, base classes on hover. Not yet tested in game.
- **First recording with the new build** (`20261029-161758-test-ghost`): 38,570 `destroyed` lines and
  72,110 objects. Ghost server characters each live from `ServerControllableSpawnDoneMessage` to
  `ServerControllableUnspawnDoneMessage` (+0x30 is the character); StopReplay at marker 1, StartReplay
  and CharacterSpawned at marker 2. Ghost characters' creations have no bus blueprint name.
- **List watches confirmed in game** (`20261029-162532-ghost-test`): the ghost spawner
  (`PamServerCharacterSpawnEntity`) holds one character at a time; each StopReplay / StartReplay cycle
  removes it and adds a new one about 2 to 3 s apart. Client spawners were missed: **tracing a class
  put a thunk in vtable slot 0 (`getType`), so `typeOf` no longer knew its objects** (the inspector
  dropped them, watches skipped them). Fixed: tracing skips slot 0. **Confirmed** (`20261029-163034-test-test`):
  client spawners recorded, server and client lists change in step; client characters come from
  `ClientSpawnEntity_createFromNetwork`. A ghost living ~0.1 s (server #10617) was missed by the server
  list: lists are read 10 times a second, so changes shorter than that can slip by (creations and
  destructions still show them).
- **Watches read every frame** (2026-10-29, the user: for reverse engineering anything matters): fields
  and lists are read every frame by default; a *readings a second* slider lowers it, and the menu
  shows how long the last reading took. Not yet tested in game.
- `CLAUDE.md` at the root: rules, build, the user's PC.
- **Recorder: list watches** (2026-10-29): watch type *list* (vector begin/end at the offset): `list`
  lines with `added`, `removed`, `reordered`, `items`. Preset *Add the spawners' lists*
  (ServerSpawnEntity +0x148, ClientSpawnEntity +0xb0; the client class name is from the annotations,
  unchecked against the reflection).
- **Fingerprints from the game** (2026-10-23): menu → *Game image* → *Dump the game image* writes
  `bin/dumps/*.exe` (hooked slots restored to the game's pointers). `gridnode.py fingerprint DUMP
  [--write] [--add NAME] [--mecmp DIR]` checks / rewrites GridNode's and MECMP's fingerprint bytes.
  Checked against the first dump: all of GridNode's 38 code entries and MECMP's 19 checks match; 12
  more entries added from the image.
- **Recorder: trace a class's virtual calls** (2026-10-23): thunks with unwind info on every vtable
  slot (passing over slots already hooked), rate-limited, stacks unwound from the caller. Confirmed
  in game on `ClientCharacterSpawnEntity` (52 slots). Export kind `call`; `summarize.py` names it.
- **Tools** (2026-10-22): `annotations-diff` (two annotation sets, folders or `git:REF`, `--inbox`),
  `ghidra-import --who`, `ghidra-relay` (a friend's Ghidra export into your Ghidra), GridNodeSync
  exports your comments and keeps one comment block per source. GridNodeSync's Java changes are
  **not yet run in Ghidra**.
- **Research** (2026-10-22/23): replay ghosts (StartReplay / StopReplay through a linked character
  spawner), the map's echo and the Your Echo screen take the **last** character of the city-ghost
  spawner's client list (directly and through `Entity_resolveLinkedEntity`), the city alert, the
  startup sequence. Spawner and replay functions annotated in `db/annotations/spawn.yaml`.
- **MECMP** (PRs #102 to #106): ghosts despawned through the spawner's unspawn; client objects
  claimed by owner and bus value; the game's echo kept last in the server and client spawner lists
  (map and Your Echo fixed, tested); own spawner experimental and off (`own_spawner=0`); ghosts shown
  again when the map or a menu closes; MSVC C4702 fixed.
- **Nollz's PR #2** (merged): GridNode chains onto another mod's hook in either load order.
- **Layout editor** (paused on purpose): barriers and magropes shown, moved, saved and copied;
  magrope trigger volumes sized by their transform (`Target` drawn as a point, meaning unconfirmed).

## Next steps
1. Sort `ghidra-import`'s inbox from the user's Ghidra (111 functions, 34 names differ).
2. In game: the class picker, and every-frame watches (their cost shows as *last reading*; does a
   ~0.1 s ghost now show in the server list?).
3. Runtime class structs and vtable types in Ghidra (spawners, replay entities, characters).
Later: layout editor (occluders in the overlay, copying a whole group prop, confirm magrope `Target`).

## Decisions and gotchas
- Never patch game code: pointer swaps only. Leave online services and Beat Revival alone. PC only.
- Never commit game data or user dumps (dumps, FBX game meshes, event-names.txt, logs, recordings).
- Commit / push only when asked. Deploy test builds under a unique file name first.
- A fingerprint's bytes come from a dump, never from a disassembly listing (one instruction has
  several encodings; a guess turned MECMP's own spawner off on the right game).
- The game finds spawners by their data in a bus: a second spawner with the same data and bus (MECMP's
  "own spawner") is found instead of the game's one. Keep the game's character last instead.
- The u32 at a character's bus +0x58 is the same for every ghost of a spawner (low bits of the
  spawner's data pointer): not a per-character id.
- A dump made with hooks in place shows hook pointers in the slots; the dump button restores them,
  and the checker reports a pointer outside the game as "hooked when dumped".
- The user's local checkout `D:\Dev\GridNode` is far behind `main` with uncommitted annotation edits
  from another agent (most wrong; the repo already has the corrected versions). Steps to clean it up
  were given 2026-10-22 (back up, `git restore`, remove the 4 untracked YAML files, pull).
