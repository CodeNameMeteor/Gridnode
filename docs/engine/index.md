# Engine notes

How the parts of Catalyst's Frostbite fit together, as far as GridNode knows. Addresses are PC image
VAs (Ghidra). Each claim links to its annotation, where the confidence and source are recorded.

## Entities, components and data

- **Data and instances.** A level is built from data (`EntityData` subclasses, from the game's EBX
  assets) and the entities made from it. Data is shared: every entity made from the same asset reads
  the same object, so writing data changes all of them (several characters share one
  `PamCharacterEntityData`). Per-entity state lives in the entity.
- **Realms.** Single player still runs a server and a client world, on two threads. Faith exists
  twice: `PamServerCharacterEntity` (owns movement, health, teleport) and `PamClientCharacterEntity`
  (animation, camera, input). Engine functions must run on their realm's thread.
- **Creation.** Entities are made by creators, one per class and realm. GridNode hooks the creator's
  factory slot to tell mods about every new entity ([entity](../annotated/entity.md),
  [spawn](../annotated/spawn.md)).
- **Components.** A character's components (health, camera, ANT input, player entry...) are found
  with its `getComponent` virtual ([character](../annotated/character.md)). An entity's own state is
  mostly in its components: a KSEC's object did not change at all while he was hit, his
  `PamServerAIHealthComponent` did (the Field diff window).
- **Transforms and the component collection.** The object at an entity's +0x38 is both its
  transform space (read by `SpatialEntity_setTransform`; the transform is selected by two bytes in
  it) and its component collection: a u8 slot count at +8, 0x20-byte slots from +0x10, a slot's
  component at +0x10 ([entity](../annotated/entity.md), `ComponentEntity_destroyComponents`).
- **Properties.** Components connect to data bus properties by name hash (djb2-xor, the hash Frosty
  calls Fnv1). The health component's `IsImmortal` is one: GridNode's god mode sets it.

## Engine systems

- **Settings**: `SettingsManager` keeps every settings object by type; the console clones a settings
  object when it changes one ([console](../annotated/console.md)).
- **Console**: `Console_executeCommand` runs any command, including the ones the menus hide.
- **Messages**: three message managers (core, client, server); listeners register per category
  ([messages](../annotated/messages.md)).
- **Physics** (Havok's hknp): each realm has a physics world (the client's is what Faith collides
  with), and the client has a second one for effects. Ray casts go through the server world, as the
  game's ray cast entities do. The game makes its worlds with room for 16,384 bodies and crashes
  when they fill; GridNode raises that to 65,536. The city's bodies are static: about 2,600 per
  world and almost none dynamic, so gravity and pushes have little to act on; ragdolls are dynamic
  bodies in the effect world ([physics](../annotated/physics.md)).
- **Progression** (the save): every flag the game knows is defined in `PamProgressionData` (flag
  groups, missions with their completed and available flags, five groups the game generates); the
  server's progression manager keeps each flag's value in a hash map by the flag's `NameHash`, and
  one setter changes them, telling missions and sending `PamProgressionFlagUpdatedMessage`. The
  active mission is the first flag of the generated flags ([gamestate](../annotated/gamestate.md)).
- **Health**: a server health component's health is at +0x20 (f32) and its max at +0x24. Faith has
  100; a KSEC 3, one per hit ([gamestate](../annotated/gamestate.md)).

## Faith

[Characters](characters.md) covers Faith's objects, her movement state and what it takes to move,
jump or protect her. [Input](input.md) covers how keys become actions.
