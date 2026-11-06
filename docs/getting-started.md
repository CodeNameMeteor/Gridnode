# Getting started

GridNode runs on the PC game from the EA app (one build: GridNode checks it at startup and stays
passive on any other).

## Install

1. Get a zip from the GitHub Releases page: `GridNode-player-<version>.zip` to play with mods, or
   `GridNode-<version>.zip` to make them (it adds the developer tools, the examples and the SDK).
   Or make one with `gridnode.py package` ([Releases](packaging.md)), or build GridNode yourself
   ([Building](building.md)).
2. Unzip it anywhere: a folder of its own, not the game's folder.
3. Start the game, then run `gridnode_inject.exe` from that folder. It loads `gridnode.dll` from its
   own folder into the running game.
4. Mods go in `mods/` next to `gridnode.dll`: `mods/<name>.dll`, or `mods/<name>/<name>.dll` with
   the files the mod brings.

**Loading with the game every time** instead of injecting: copy `start-with-the-game/version.dll`,
`gridnode.dll` and `mods/` into the game's folder, next to `MirrorsEdgeCatalyst.exe`. `version.dll`
is a proxy Windows loads with the game; it loads `gridnode.dll` from the same folder. Delete
`version.dll` to stop.

**Uninstall:** open the menu (INSERT) and use Unload, or just close the game; delete the unzipped folder
(and `version.dll`, `gridnode.dll` and `mods/` from the game's folder if you copied them there). Injected,
GridNode writes only next to its own `gridnode.dll`, never to the game's files.

## First start

`gridnode.log` (next to `gridnode.dll`) shows what happened:

```text
GridNode 1.0.0 loading into MirrorsEdgeCatalyst.exe (base 0x140000000)
startup: engine type list ready (9988 types)
startup: build check passed
types: 9988 (9988 listed), ...
dump: written to ...\gridnode-reflected.json
mods: mec_trainer.dll loaded (mec-trainer 1.10.2), reloadable
mods: 1 of 1 loaded
ready (2164 hooks installed)
```

- **build check**: GridNode only does anything on the build it knows. On any other build it logs
  "this is not the game build GridNode supports" and stays passive.
- **dump**: a developer build writes `gridnode-reflected.json` the first time, the engine's
  reflection data from your own copy of the game ([format](dump-format.md)).
  `python tools/gen/gridnode.py setup --dump ...` turns it into C++ headers and, with
  `gridnode.py docs --reflected ...`, into the full reference. A player build does not write it.
- The previous start's log is kept as `gridnode.prev.log`.

If GridNode's folder is under `Program Files`, Windows may redirect the log to
`%LOCALAPPDATA%\VirtualStore\...`; GridNode also falls back to `%LOCALAPPDATA%\GridNode`.

## In game

**INSERT** opens the GridNode menu:

- **Mods**: every mod in `mods/`, loaded or not, with Load, Reload, Unload (and reload when its DLL
  changes). The mods' own windows and settings.
- **Unload**: every mod and GridNode itself, the game put back as it was.
- **Tool windows** (developer build; the checkboxes at the top of the menu):
    - **Inspector**: every live entity by class, its reflected fields (editable); types; settings
      (online settings are locked until you allow changes); engine messages.
    - **Console**: every console command, with history (up / down) and completion (Tab).
      `progress` lists the save's progression flags; click one to set it.
    - **Recorder**: records what the game does, for `tools/recording/summarize.py` ([Recorder](recording.md)).
    - **Tracer**: every call to a game function, with its arguments and caller.
    - **Field diff**: snapshot an object (or one of its components) and see which fields change.
    - **Layout editor**: paused for now ([Layout editor](layout-editor.md)).
- **World**, **Game image**, **Hooks** (developer build): Faith's position, an image dump of the game
  for Ghidra, and every hook and patch.

Settings are in `gridnode.ini` next to `gridnode.dll` (all optional):

```ini
[dump]
mode=auto       ; auto: when the file is missing; always; never
[mods]
enabled=1
folder=mods     ; relative to gridnode.dll, or absolute
[ui]
enabled=1       ; the overlay (mods' windows need it)
menu_key=INSERT
scale=1.0
inspector=1
[tools]         ; developer tools (a player build has none of them)
editor=1
recorder=1
console=1
dev_info=1      ; the menu's World, Game image and Hooks sections, Dear ImGui metrics
```

## When something goes wrong

[Troubleshooting](troubleshooting.md) has the full list. In short:

- **Nothing happens:** look at `gridnode.log`. No log at all means GridNode was not loaded (run the
  injector after the game has started, or check `version.dll` is next to the game's exe).
- **"not the game build GridNode supports":** the game is another build (not the EA app's, or
  updated). GridNode does nothing on it, by design.
- **"returned 2 from gn_mod_load: it was built for a newer GridNode":** the mod needs a newer
  GridNode than this one.
- **A crash:** the log ends with a `crash:` block: the exception, the address (a module and offset,
  `(a mod)` when it is in a mod), the registers and the call stack. Send that block, with
  `gridnode.prev.log` if the game was restarted since, to whoever made the mod (or open an issue for
  GridNode itself).

## Next

[Writing a mod](modding.md) covers the API; [Your mod in its own repository](mod-repository.md) covers
starting, building and shipping a mod outside this repository; [Releases and mod templates](packaging.md) covers the zips; [Shipping a mod](shipping.md) covers what players need. `examples/` has complete
mods: `hello` (reflection, ticks, creation, messages), `hello-rust`, `trainer` (save / load position,
a window), `mec-trainer` (the MEC trainer: movement, tuning, noclip, god mode, game speed, volumes),
`world-draw` (drawing in the world, and test buttons for the physics, sound and despawn API),
`detour-demo` (function detours and functions between mods) and `probe` (pointer chain research).
