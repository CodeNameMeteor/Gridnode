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
| Reflection | 9,988 engine types by name: fields, enums, sizes, `typeOf(object)` |
| Entities | creation callbacks per class, components, transforms, the game's own teleport |
| Threads | server and client ticks, `runOnServer` / `runOnClient` |
| Hooks | chained pointer hooks (vtable and creator slots); game code is never patched |
| World | Faith on client and server, level events, ray casts |
| Faith | movement state, wall counters, speed, camera; jump and any input action, whatever the binding; god mode; movement tuning by name |
| Engine systems | settings (every settings class, by name), the console (every command), engine messages |
| Overlay | a shared Dear ImGui overlay (INSERT), an inspector for entities, types, settings and messages |
| Languages | C and C++ (a plain C ABI) and Rust |

## Rules GridNode follows

- Game code is never patched. Hooks only swap pointers in vtables and creator tables.
- Nothing runs until the build check passes; on any other build GridNode stays passive.
- Nothing from the game is published: each user generates their own reflected data, and the full
  reflected reference in these docs is built locally from it.
- Online services, Denuvo and the EA app are left alone. Settings and console commands that touch
  online play are marked and ask before changing.
