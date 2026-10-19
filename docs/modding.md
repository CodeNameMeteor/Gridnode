# Writing a mod

A GridNode mod is a DLL in the `mods/` folder next to `gridnode.dll`. GridNode loads every DLL
there (in name order) once the game has started, the build check has passed and the engine's
types are known, and calls its `gn_mod_load`.

```cpp
#include <gridnode/engine.hpp>   // addresses and offsets from db/annotations (optional)
#include <gridnode/mod.hpp>      // the API

GN_MOD("my-mod") {
    gn::log("hello");
    gn::onClientTick([](float dt) { /* every client tick, on the client game thread */ });
    return true;                 // false: the mod failed to load (it stays loaded but GridNode says so)
}
```

See `examples/hello/hello.cpp` for a complete mod. Build it with the repository's CMake (it lands
in `build/mods/`), or in your own project: add `sdk/include` and the generated
`build/generated` to the include path. No library to link: the API is a table of function
pointers handed to `gn_mod_load` (`gridnode/api.h`), so a mod can be built with MSVC, MinGW or
clang, independently of how `gridnode.dll` was built.

## Threads

The game updates its server and client worlds on two separate threads, even in single player.
Engine functions must be called on the thread of their realm.

| | |
|---|---|
| `gn::onServerTick(fn)` | every server simulation tick, after the world update (`ServerGameWorld::postUpdate`) |
| `gn::onClientTick(fn)` | every client tick, after the world update (`ClientGameWorld::postUpdate`) |
| `gn::runOnServer(fn)` / `gn::runOnClient(fn)` | once, at the next tick of that realm (call from any thread) |
| `gn::isServerThread()` / `gn::isClientThread()` | |

Both ticks run in the main menu and in levels; there may be several per frame when the game
catches up. `dt` is the tick's time step. `gn_mod_load` itself runs on GridNode's own thread:
do not call engine functions there; post them with `runOnServer` / `runOnClient`.

A tick callback that throws a C++ exception is logged and removed.

## Reflection

Every engine class, value type and enum the game reflects (9,988 types) is available by name:

```cpp
gn::Type t = gn::Type::find("PamCharacterEntityData");
gn::Type::of(object);                       // an object's type (from its vtable; calls nothing)
t.isKindOf(gn::Type::find("EntityData"));   // class id ranges, as the engine does it
gn_field f; t.field("Visible", f);          // offset, size, type, declaring class

gn::Object o = gn::Object::of(data);
bool visible; o.get("Visible", visible);   // checks the field's size against bool
o.set("Visible", true);                     // fault-tolerant write
gn::Object blueprint = o.ref("Blueprint");  // follow a class field (a pointer)
```

Reflection describes data objects (`...Data`, assets, settings) and value types fully. Runtime
classes (entities, components) are reflected without fields; what is known about them is in
`db/annotations` and therefore in `gridnode/engine.hpp` (`gn::engine::cls::<Class>::off::...`,
`::vf::...`) and in the generated `gridnode/generated/types.hpp`.

Data objects are shared: every entity made from the same asset points at the same data
(`ComponentEntity+0x30`). Writing a data field changes it for all of them.

## Entity creation

```cpp
gn::onCreated("PamClientCharacterEntity", [](void* entity, void* creationInfo) { ... });
```

Called for every entity of that class or a subclass the game creates from then on, right after
its constructor, on the thread that created it (server entities on the server thread, client
entities on the client thread). It works for every class the engine has an entity creator for
(1,103 of them); GridNode hooks the creators' factory slots.

## Hooks

```cpp
using UpdateFn = void (*)(void* self, void* info);
static UpdateFn original;
static void detour(void* self, void* info) { original(self, info); /* ... */ }

gn::hookSlot(gn::engine::hook::PamUISystem_update.slot,
             gn::engine::hook::PamUISystem_update.original, &detour, &original);
```

A hook swaps one pointer: a vtable slot, a creator slot, or a function pointer the game keeps in
data. Game code is never patched (the executable is protected; MECMP has run this way since its
first release). GridNode checks the slot holds the expected function before swapping, and
chains hooks: if two mods hook the same slot, the second one's `original` is the first one's
detour. Always call `original`. Hooks stay for the life of the process.

A vtable hook affects every object of that class (and of subclasses that inherit the slot):
filter inside the detour if you only care about some objects.

## Logging

`gn::log(fmt, ...)` writes one line to `gridnode.log`, prefixed with the mod's name.
