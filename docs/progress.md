# Progress / handoff

Update at the end of each session. Keep it short. The full handover for a new agent is
[handover.md](handover.md).

## Current task
(none) Last session ended 2026-10-23. All work pushed: GridNode `main` 14507f0; MECMP PRs #102 to #106
merged.

## Done recently
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
Proposed order, not yet started (see handover.md §5):
1. Run GridNodeSync's import / export in Ghidra once (the Ghidra connection can run scripts now).
2. Recordings into Ghidra: `summarize.py`-side export of a recording as a GridNodeSync import
   (create missing functions, bookmark call sites with what came through, comment hit counts).
3. Pointer names in recordings: entity destruction + a pointer table (type, data, created/destroyed).
4. Watch a list (a vector of entity pointers), not only fields.
5. Runtime class structs and vtable types in Ghidra (spawners, replay entities, characters).
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
