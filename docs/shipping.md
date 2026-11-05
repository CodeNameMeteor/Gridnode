# Shipping a mod

Players need GridNode to run your mod, but not GridNode's developer tools. Ship the **player build**
of `gridnode.dll`: the mod loader, the API, the overlay for your mod's windows, and saved layouts
(custom maps), without the inspector, console window, tracer, field diff, Layout editor, free camera,
Recorder or the type dump.

## Get the player build

- From a release: the players' zip, `GridNode-player-<version>.zip` (it has the injector and the MEC
  trainer too).
- From CI: the `gridnode-player-x64-mingw` artifact (`gridnode.dll`, `version.dll`).
- Or build it: add `-DGRIDNODE_PLAYER_BUILD=ON` when configuring ([Building](building.md)):

  ```sh
  cmake -S . -B build-player -DGRIDNODE_PLAYER_BUILD=ON -DGRIDNODE_BUILD_EXAMPLES=OFF ...
  cmake --build build-player --target gridnode version_proxy
  ```

A player build logs `startup: player build` in `gridnode.log`. Its GridNode menu (INSERT) shows the
level, the loaded mods and the mods' own windows only. It does not write `gridnode-reflected.json`
(players do not need it); `[dump] mode=always` in `gridnode.ini` turns it back on.

## What to put in the download

Your mod's own zip is made by `gridnode_mod.py pack` ([Releases and mod templates](packaging.md#shipping-a-mod)):
`mods/<name>/<name>.dll` and its files, unzipped next to `gridnode.dll`. Players install GridNode
once from its release zip and add mods to it. Shipping GridNode with your mod instead:

```text
YourMod/                        anywhere: the player injects with gridnode_inject.exe
  gridnode.dll                  the player build
  gridnode_inject.exe
  gridnode.ini                  optional (see below)
  mods/
    your_mod/
      your_mod.dll
      ...                       its files (gn::modDir() finds them)
  layouts/                      only if you ship a custom map
    Levels_SP_SP_MainCity_SP_MainCity/
      your_map.yaml
      last.txt                  the layout to load: "your_map"
    settings.txt                autoload=1
  start-with-the-game/
    version.dll                 optional: with gridnode.dll and mods/ in the game folder, GridNode
                                loads with the game every time
```

- **Mods** are `mods/<name>.dll` or `mods/<name>/<name>.dll`, loaded at startup ([Writing a mod](modding.md)).
  A mod refuses to load on an older GridNode than it was built for, and the log says so: say which
  GridNode it needs.
- **Custom maps** are layouts made with the Layout editor (developer build): copy the level's
  folder from your `layouts/`. With `autoload=1` in `layouts/settings.txt` and the layout's name
  in the level's `last.txt`, it loads when the level starts. Teleport points
  (`teleports.txt`) can go with it.
- `version.dll` replaces nothing of the game's: Windows' own `version.dll` lives in System32, and
  GridNode's forwards every call to it.

## gridnode.ini for players

Everything has a default; ship one only to change something:

```ini
[ui]
enabled=1        ; the overlay (needed for your mod's windows)
menu_key=INSERT  ; or another key, if your mod uses INSERT; NONE: only your mod's own key opens it
gridnode_window=1 ; 0: the menu shows only the mods' windows (your mod alone)
[mods]
enabled=1
folder=mods
```

## Developer builds without some tools

The same tools can be switched off in a developer build without rebuilding:

```ini
[ui]
inspector=0
[tools]
editor=0      ; Layout editor, free camera and their keys (saved layouts still load)
recorder=0
console=0
dev_info=0    ; the menu's World, Game image and Hooks sections, Dear ImGui metrics
```
