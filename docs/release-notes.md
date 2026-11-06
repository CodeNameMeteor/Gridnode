# Release notes

## First release (draft: version to decide, 1.0.0 or a 0.9 beta)

The first public GridNode: a modding SDK for Mirror's Edge Catalyst (PC, the EA app build), API 31.

### Downloads

| Zip | For |
|---|---|
| `GridNode-player-<version>.zip` | Players: the player build of GridNode (the loader, the API, mods' windows), the injector, `version.dll` and the MEC trainer. |
| `GridNode-<version>.zip` | Mod makers: the developer build (inspector, console, Recorder, tracer, field diff, free camera, Layout editor), the example mods and the SDK (`sdk/`: headers, Dear ImGui, the Rust crate, mod templates). |

Install: unzip anywhere, start the game, run `gridnode_inject.exe` ([Getting started](getting-started.md)).
Making a mod: [Your mod in its own repository](mod-repository.md).

### What a mod gets

- **Mods** in C, C++ or Rust, each in its own folder with its files; reload and unload while the game
  runs; manifests, dependencies, functions shared between mods; declared settings and hotkeys.
- **Reflection** by name over the engine's 9,988 types; our own annotations of addresses and
  offsets (`engine.hpp`).
- **Entities:** creation and destruction callbacks, components, transforms, entities near a point or
  of a class, events between entities, the game's own teleport, despawning characters.
- **Threads:** server and client ticks, `runOnServer` / `runOnClient`; the level started and the level
  ready (Faith over loaded ground).
- **Hooks:** chained pointer hooks, function detours and code patches, all listed and undone on
  unload; two mods on one place are named in the log and the Mods list.
- **Physics:** ray casts (what was hit), collision bodies, static boxes; body velocities and gravity
  (experimental).
- **Faith:** movement state, speed, health, input actions whatever the binding, god mode, movement
  tuning by name, movement events, landing prediction, her ANT move tracker and her last 15 seconds.
- **Look:** the time of day, set or held against the game moving it on; the visual environment and
  timelines (experimental).
- **The camera:** GridNode's free camera, or a mod's own camera and field of view.
- **Game state:** levels, every settings class, the console, engine messages, sounds, progression
  flags, missions; online play changed only on purpose ([GridNode and online play](online.md)).
- **ANT** (the movement and animation system): its types and fields by name; live fields (research).
- **Overlay:** a shared Dear ImGui overlay; a mod can open it with its own key and be the only
  window in it.

### The MEC trainer

Noclip (Faith floats while the free camera flies), god mode, game speed, saved positions, fast
loads, movement tuning; World: kill volumes off, movement blockers off, no city alerts and no KSEC in
free roam; trigger volumes shown; bunny hop practice with early / late feedback. Dash and time trial
uploads are blocked while it is loaded.

### Not tested in game before this release

To be filled from `docs/progress.md` when the release is cut: the clean-setup pass decides what is
tested, marked experimental, or taken out. See [Known issues](known-issues.md).
