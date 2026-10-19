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

## World

```cpp
void* faith = gn::localServerCharacter();    // ServerCharacterEntity (what teleport moves)
void* faithC = gn::localClientCharacter();   // PamClientCharacterEntity
gn::Vec3 p;
gn::positionOf(faithC, p);                   // any spatial entity, any thread (fault tolerant)
gn::Transform t;
gn::transformOf(faith, t);                   // the full LinearTransform
gn::runOnServer([=] { gn::teleport(faith, t); });  // the game's own teleport: server thread only
void* rec = gn::component(faithC, gn::Type::find("PamClientReplayRecordingComponent"));
gn::onLevel([](const char* level, bool loaded) { /* "Levels/SP/..." started or ended */ });
std::string level = gn::levelName();
```

| Function | Thread | Notes |
|---|---|---|
| `localClientCharacter` | any | On the client thread it asks the game; elsewhere it is the value from the last client tick. |
| `localServerCharacter` | any | Reads only: the server player manager, checked against the character's back pointer. |
| `transformOf` / `positionOf` | any | `false` if the entity has no readable transform. |
| `teleport` | server | `GN_ERR_THREAD` elsewhere. Clears velocity and replicates to the client like the game's teleport triggers. |
| `component` | the entity's realm | Characters only (client or server). |
| `onLevel` | GridNode's thread | Within ~0.1 s of the server world changing; called at once for a running level. |

Entity pointers are only good while the entity lives (most until their level ends). Keep them
in a per-level variable cleared by `onLevel`, or look them up again each time.

## UI (Dear ImGui)

GridNode draws a Dear ImGui overlay on the game. **INSERT** opens its menu: the GridNode window
(level, mods, the inspector), and every mod's interactive windows. While the menu is open the
mouse and keyboard belong to the overlay; key releases still reach the game.

```cpp
#include <gridnode/ui.hpp>   // includes imgui.h

GN_MOD("my-mod") {
    gn::ui::onDraw([](bool menuOpen) {
        if (!menuOpen) return;            // or draw a HUD here (a window with ImGuiWindowFlags_NoInputs)
        ImGui::Begin("My mod");
        if (ImGui::Button("Teleport home")) gn::runOnServer([] { /* ... */ });
        ImGui::End();
    });
    return true;
}
```

- Link the `gn_imgui` CMake target: GridNode's own copy of Dear ImGui (`third_party/imgui`,
  1.91.9). `ui.hpp` checks the version and data layout once; a mod built against another
  ImGui gets no UI (logged) rather than a corrupted frame.
- Callbacks run on the **render thread**, every frame. Never call engine functions there: post
  work with `gn::runOnServer` / `gn::runOnClient` and share state with atomics or a mutex
  (`examples/trainer` shows the pattern).
- A mod that leaves a window open (a missing `End`) is cleaned up after its callback.
- Window titles are global: prefix them with your mod's name if they could clash.

`gridnode.ini`:

```ini
[ui]
enabled=1        ; 0: no overlay (mods' onDraw returns false)
menu_key=INSERT  ; INSERT, HOME, END, DELETE, PAGEUP, PAGEDOWN, PAUSE, F1..F12, NUM0..NUM9, a letter, or VK_xx
scale=1.0        ; text size
inspector=1      ; the inspector (tracks every entity the game creates)
```

MECMP also opens its menu with INSERT: if you run both, give GridNode another `menu_key`.

### Inspector

GridNode menu → **Inspector**:

- **Entities**: every live entity created since GridNode started, by class, with positions.
  (With the injector, entities created before injection are not listed; with `version.dll` all are.)
- **Types**: all 9,988 reflected types, with fields, enum values, base classes, vtable and the
  default instance.
- **Shortcuts**: Faith (client and server), the worlds, and any address.
- The object view decodes reflected fields (and GridNode's annotated members, in blue), follows
  pointers and arrays, and edits booleans, numbers and enums. Edits are written on the client
  thread. Remember that data objects (`...Data`) are shared by every entity made from them.

## Settings and console

```cpp
gn::settings("GameTimeSettings").set("TimeScale", 0.5f);   // any field of any settings class
gn::console("list", [](const char* out) { gn::log("%s", out); });  // client thread
```

- `gn::settings(className)`: the engine's settings object (135 classes, all 3,431 fields, not
  only the 141 the player's console exposes). Look it up each time you use it: setting a value
  through the console replaces the object with a changed copy.
- `gn::console(line[, fn])`: runs a console line the way the engine runs its config files, so every
  registered command works. It runs on the client thread; `fn` receives the output there.
  The engine prints its own messages (its `list`, unknown commands, argument errors) to a log the
  shipping game does not show, so GridNode answers `list [filter]` itself and reports unknown
  commands; otherwise the output is the command's result (a setting without a value prints it).
- Some settings are only read when the game starts or a level loads: changing them later may do
  nothing until then.
- Online play: settings classes and command groups that reach the network or EA's services
  (`Network`, `Online`, `Origin`, `Server`, `PamSocialPlay`, `PamStats`, ...) are marked.
  `gn::onlineWarning(name)` says why. GridNode does not block them, but logs every change a mod
  makes to them; the menu asks before changing them. Keep them alone unless you know what they do:
  they can affect Beat Revival and your online profile.

In game: GridNode menu → **Console** (every command, with search; Up / Down recall earlier lines,
Tab completes a command name; online ones in orange ask for confirmation) and Inspector → **Settings** (every settings object; online ones need "Allow changes").

## Messages

```cpp
gn::onMessage("ClientCharacterLocalPlayerSetMessage", [](const void* m, const gn_message_info& info) {
    void* character = nullptr;
    gn::read(static_cast<const char*>(m) + 0x30, character);   // members start after the 0x30-byte header
});
gn::onMessage("ServerPlayer", [](const void*, const gn_message_info& info) { gn::log("%s", info.type_name); });
```

- The engine's systems talk through three message managers (core, client, server). GridNode
  registers its own listener with them, as the game's code does: nothing is hooked, and listening
  is safe for every category.
- Subscribe to a message type (668 of them) or a whole category (120, e.g. `ServerPlayer`,
  `ClientPlayer`, `UI`). Inspector → **Messages** lists them all, and with **Record** shows every
  message as it is sent, with a copy of its bytes.
- The callback runs on the thread that sent the message, and the message only lives during the
  call: copy what you need. Most members after the header are not reflected; the inspector's copy
  shows them (pointers to known objects are links).
- Messages an entity sends to its own components (e.g. `CharacterToComponentsOnTeleportedMessage`
  from the game's teleport) do not go through the managers and are not seen here.
- Sending messages is planned for M8.

## Name hashes

Frostbite keeps many names only as a 32-bit hash: property and event ids in blueprint connections
(`PropertyConnection.SourceFieldId` / `TargetFieldId`, `EventConnection.SourceEvent` /
`TargetEvent`) and names in EBX. The hash is `h = 5381; h = (h * 33) ^ c` (Frosty calls it Fnv1).
A hash cannot be reversed, only looked up:

```
python tools/gen/gridnode.py hash Visible ControllableTransform
python tools/gen/gridnode.py unhash 0x7c46ea68 -2024647575 @hashes.txt --image MirrorsEdgeCatalyst_dump_SCY.exe
```

The dictionary holds every reflected type, field and enum value name, GridNode's annotations, every
string in the game image (with `--image`) and your own word lists (`--words`). Frosty shows ids as
signed numbers; both forms are accepted.

## Logging

`gn::log(fmt, ...)` writes one line to `gridnode.log`, prefixed with the mod's name.
