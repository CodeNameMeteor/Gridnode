# GridNode

GridNode is an open-source SDK for modding **Mirror's Edge Catalyst** on PC (Frostbite 3). It lets
mods find, read, write, call and hook the game's engine classes by name, and it documents what is
known about the engine.

- **For mod authors:** start with [Getting started](getting-started.md), then [Writing a mod](modding.md).
- **For reverse engineers:** the [Annotated engine](annotated/index.md) pages list every runtime member,
  virtual function, global and hook GridNode knows, with its confidence and source. How to add your
  own findings is in [Contributing findings](contributing.md).
- **Engine notes** explain how the parts fit together: [characters](engine/characters.md),
  [input](engine/input.md).

## What GridNode gives a mod

| | |
|---|---|
| Mods | C, C++ (a plain C ABI) and Rust; `mods/<name>.dll` or `mods/<name>/<name>.dll`; reload and unload while the game runs; manifests, dependencies, functions shared between mods |
| Reflection | 9,988 engine types by name: fields, enums, sizes, `typeOf(object)` |
| Entities | creation callbacks per class, components, transforms, entities near a point or of a class, events between entities, the game's own teleport, despawning characters |
| Threads | server and client ticks, `runOnServer` / `runOnClient` |
| Hooks | chained pointer hooks (vtable and creator slots), function detours and call patches, all undone on unload |
| Physics | ray casts (entity, body, layer, material), collision bodies, static collision boxes, body velocities, gravity |
| Faith | movement state, wall counters, speed, camera, health; jump and any input action, whatever the binding; god mode; movement tuning by name; movement events |
| Game state | levels, settings (every settings class, by name), the console (every command), engine messages, sounds, the checkpoint, the save's progression flags and missions |
| Overlay | a shared Dear ImGui overlay (INSERT): mods' windows, drawing in the world, declared settings and hotkeys |

The developer build adds research tools: the inspector (entities, types, settings, messages), the
console, the [Recorder](recording.md), a function tracer, a field diff window, the
[Layout editor](layout-editor.md) (paused for now) and a game image dump.

## Rules GridNode follows

- Every change to the game goes through GridNode, which lists it and undoes it on unload. Pointer
  swaps are preferred (vtable and creator slots); code is patched where no pointer reaches.
- Nothing runs until the build check passes; on any other build GridNode stays passive.
- Nothing from the game is published: each user generates their own reflected data, and the full
  reflected reference in these docs is built locally from it.
- GridNode itself leaves online services, Beat Revival, Denuvo and the EA app alone. Settings and
  console commands that touch online play are marked and ask before changing. A mod may run online console
  commands only after saying so (`online_allow`): it is then shown in orange in the Mods
  list, and every online command it runs is logged. The MEC trainer never does, and blocks Dash and
  time trial uploads while it is loaded.
