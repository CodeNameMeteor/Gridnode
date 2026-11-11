# Known issues

Kept up to date for each release. [Troubleshooting](troubleshooting.md) covers what to do when
something goes wrong; report anything not listed here with `gridnode.log`.

## The game

- **One build only:** the PC game from the EA app. GridNode stays passive on any other build (Steam
  or an updated one may differ) and says so in its log.
- **One player:** the API and the trainer assume the local player (single player). MECMP, the
  multiplayer mod, is a separate project.

## GridNode

- **Quitting during a Dash** crashed twice (`0x143164d50`): the game's own mission clean-up looks
  in the server world after it is gone. GridNode guards that call since 2026-11-05 (it answers
  "none" then); not yet confirmed in game.
- **Teleporting far** lands before the area has streamed in. Wait for `gn::onLevelReady` after a
  level loads, and for `gn::groundBelow` after a far teleport (the trainer does).
- **Physics in the city is static:** almost no body is dynamic, so `body_set_velocity`, `body_push`
  and gravity changes rarely have anything to act on (experimental).
- **The waypoint** is at street height, not at the marker's; **the checkpoint** reads a table that is
  not fully understood (experimental).
- **ANT live fields** (`ant_live_field`, `ant_objects`, the console's `ant TYPE FIELD`) are research
  tools: a type's own fields only, references cannot be set, a type's loader runs on test data the
  first time.
- **`replay_recent`**: the bones are as ANT's pose has them; which bone is which past the root and
  the trajectory is not mapped yet.
- **Experimental APIs, not yet tested in game or only lightly:** standalone collision boxes (`boxBackendNative`,
  API 34; a box made this way copies a barrier's setup and could crash if that barrier is destroyed first), streaming
  focus points (API 30; the game may already stream around the camera), timelines and the visual environment hooks
  (API 29), `raycastFiltered` (API 33). Each is marked experimental in [Modding](modding.md).
- **The Layout editor** (custom maps) is paused: saved layouts load, the editor itself is not being
  worked on.

## The MEC trainer

- With the trainer loaded, **Dash and time trial times are not uploaded** (on purpose:
  [GridNode and online play](online.md)).
- **No KSEC** works in free roam only; a mission's KSEC are left alone. The hunter drone is stopped
  through the city alert; a drone that comes another way is logged.
- **Movement blockers off** only re-enables barriers it disabled; barriers the level turns on later
  are found within a second.

## Not tested in game yet

Features marked "untested" in `docs/progress.md` were built from the game's code but not yet tried
in game; they are listed in the release notes of the release they came in.

API 36 (class hooks by name, any entity's components, the data folder, type views) and the fixes of
2026-11-11 (the near-image reserve, detour writes, thread listing) have not run in game yet
([progress](progress.md) has the test).
