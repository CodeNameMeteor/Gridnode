# Open research

Large pieces of work nobody has done yet, each with where to start. They suit another model (or person) with Ghidra
open on the game image. Pick one, work it through, and put what you find in `db/annotations/` (with a source,
a confidence and a note), a page under `docs/research/`, and the status here. [Contributing](../contributing.md) has
the rules; [Tools](../tools.md) the tools. Say what you checked in Ghidra and what only in your head, and do not
trust an address or an offset from another model until you have read it in the code: wrong claims in earlier
references are listed in [Havok reference](havok-reference.md).

Nothing here is needed for the first release.

## 1. Standalone collision, all the way

Boxes made by the game's own factory work in the client world (`boxBackendNative`, API 34;
[progress](../progress.md)). Open:

- **No dependence on a live barrier.** Each box copies a barrier's setup and keeps a pointer to it (the game reads it
  when Faith touches the body: `0x143913840`, called from the character probe `0x142f450c0`). A parent made for the
  purpose, or a body type that needs none, would remove the barrier hook and the 3 second wait after a level load.
- **Other shapes** (sphere, capsule, convex hull, mesh): `PhysicsEntityFactory` `0x143516450` already switches on the
  description's type (1 a box, 2 probably a sphere, 3 a capsule, 7 a prebuilt shape). A mod API for them, and for a mesh from a model.
- **The server world** (AI, server ray casts) still uses lent records. Does the server factory work the same way?
- **Fingerprints** for the new functions (bytes from a game dump, `gridnode.py fingerprint --add`).
- **Dynamic bodies** (pushable props): mass, motion creation, who simulates them, and how that interacts with the
  client and server worlds.

## 2. Havok character controller

Claimed but not checked ([Havok reference](havok-reference.md)): `hknpCharacterProxy` (vtable `0x141d53348`), the
support check `0x143f60710`, the 45 degree slope limit, contact tolerance 0.1 and keep distance 0.05, and the up
vector taken from gravity (`0x1434e4400`). Read them. The payoff is real: if the up vector follows gravity, `set_gravity`
(API 21) lets Faith walk on walls and ceilings; find out what else breaks (camera, ANT, the fall state).

## 3. Characters: spawn, outfits, poses, voices

What MECMP does for its ghosts and GridNode does not expose ([replay ghosts](replay-ghosts.md),
[progress](../progress.md)): spawning a character, outfits, driving bone poses, vocals. Each piece needs its own
reading of the spawner, the outfit and ANT code, and a safe teardown. A "spawn a character" API would help most mods.

## 4. Sound

The sound instance's real `setParameter` and velocity entry points. The earlier report named two vtable slots
(`0x141acfad0` +0x18, +0x78) that are something else (checked 2026-11-06). Find the real ones (Wwise calls),
then `sound_set_parameter` and 3D velocity become possible.

## 5. UI and pause

`ui_pause_state` (API 32) reads the game's pause counters: 0 in play, 1 with the pause menu open; the full-screen
counter stayed 0. Find which screens add to which counter (`UIPauseController_request` `0x143b16210`, callers
`0x143b389b0`, `0x143b40460`, `0x143b41900`, and the pause entity's events `0x0dd8ddf7` / `0xc44b5c3c`), what the map
and the other menus do, and what the "time value" in `UIPauseController_applyToClient` is. Then a reliable
"the game is paused" for mods, and a way to pause and resume the game from a mod.

## 6. Streaming

Does the game already stream the world around the free camera? `zone-streaming.md` and API 30 (streaming focus
points) were written without an answer: the trainer's test showed nothing either way. Instrument the zone streamer's
focus list while the camera flies far away, and decide whether the API is needed.

## 7. Map markers and presence

Friend markers and the Runner Tag icon (`PresenceInfoManager_assignSlot` `0x143a405e0`, slots of 0x50 bytes). Needs a
hook on the pool update and a real online id; only online mods would use it, so it is the careful kind of work
([online play](../online.md)).

## 8. Dash and time trial events

The messages for a Dash starting, finishing and being cancelled are known (category `0x54776fc6`; types in
[progress](../progress.md)). A typed wrapper (`onDash`, with the time and the route) and a check of every field.

## 9. The visual environment and timelines

API 29 reads and writes the visual environment (sun, exposure, depth of field) and timelines. The sun sliders work;
the rest is untested. Cover the other entries, and find the render hook that lets a mod override a value for a frame
without fighting the game. Document the field list in `visual-environment.md`.

## 10. Other builds of the game

GridNode stays passive on any build but the EA app's. A signature-based resolver (fingerprint, then find the
function) instead of fixed addresses would let it support a Steam or updated build. Start with
`sdk/src/build.cpp` and `db/annotations/fingerprint.yaml`; the question is how many of the ~850 annotated functions
can be found by their bytes.

## 11. Custom maps and assets

The Layout editor is paused. The save format works; what is missing is a reliable way to add new objects (meshes,
materials) to a level. [Custom meshes](../custom-meshes.md) and [the Frosty pipeline](frosty-unity-pipeline.md) say
what is known.

## 12. Havok world facts to confirm

`hknpWorld` size `0x5a0` and its count and capacity fields (`+0x28`, `+0x2c`; our code reads `+0x68`), the
compound shape instance array and tree, and what the body flags at `+0x58` mean. Small, but each is a claim in
[Havok reference](havok-reference.md) with no read behind it.
