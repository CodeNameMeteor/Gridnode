# Getting started

## Install

1. Build GridNode ([Building](building.md)) or download a CI build.
2. Copy `gridnode.dll` and `version.dll` into the game folder, next to `MirrorsEdgeCatalyst.exe`.
   `version.dll` is a proxy that loads GridNode when the game starts. Alternatively, start the game
   and run `gridnode_inject.exe` next to `gridnode.dll`.
3. Make a `mods` folder next to `gridnode.dll` and put mods in it (`trainer.dll`, ...).
4. Start the game.

## First start

`gridnode.log` (next to `gridnode.dll`) shows what happened:

```text
GridNode 0.1.0 loading into MirrorsEdgeCatalyst.exe (base 0x140000000)
startup: build check passed
types: 9988 (9988 listed), ...
dump: written to ...\gridnode-reflected.json
mods: trainer.dll loaded
ready (1094 hooks installed)
```

- **build check**: GridNode only does anything on the build it knows. On any other build it says so
  and stays passive.
- **dump**: the first start writes `gridnode-reflected.json`, the engine's reflection data from your
  own copy of the game ([format](dump-format.md)). `python tools/gen/gridnode.py setup --dump ...`
  turns it into C++ headers and, with `gridnode.py docs --reflected ...`, into the full reference.

If the game is in `Program Files`, Windows may redirect the log to
`%LOCALAPPDATA%\VirtualStore\...`; GridNode also falls back to `%LOCALAPPDATA%\GridNode`.

## In game

**INSERT** opens the GridNode menu: loaded mods, the world (Faith's position on client and server),
hooks, and the inspector:

- **Entities**: every live entity by class; its reflected fields, editable.
- **Types**: every reflected type.
- **Settings**: every settings object; changes apply at once. Online settings are locked until you
  allow changes.
- **Messages**: record the engine's messages and look inside them.
- **Console**: every console command, with history (up / down) and completion (Tab).

Settings for the overlay are in `gridnode.ini`:

```ini
[ui]
enabled=1
menu_key=INSERT
scale=1.0
inspector=1
```

## Next

[Writing a mod](modding.md) covers the API. `examples/` has complete mods: `hello` (reflection,
ticks, creation, messages), `trainer` (save / load position, a window), `mec-trainer` (Faith's
movement, tuning, noclip, god mode), `hello-rust`.
