# Release notes

## GridNode 1.0.0 (first release)

The first public GridNode: a modding SDK for Mirror's Edge Catalyst (PC, the EA app build), API 34.

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
- **Physics:** ray casts (what was hit, and past chosen bodies), collision bodies, static boxes (optionally
  made by the game's own physics factory, experimental); body velocities and gravity (experimental).
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

### Experimental in this release

Young: tested lightly in game or not at all, and likely to change. Expect changes before API 35.

- Standalone collision boxes (`boxBackendNative`, API 34): work in the client world, including after a level
  reload; the server world's boxes still borrow a level body's record.
- Streaming focus points (API 30; the game may already stream around the camera), the visual environment and
  timelines (API 29; the sun sliders work, the rest is untested), `raycastFiltered` (API 33; works), ANT live objects
  and Faith's last 15 seconds.

Everything else in the API was used by the MEC trainer or the examples. See [Known issues](known-issues.md) and
[Open research](research/open-research.md) for what is not done.
