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
  with its `getComponent` virtual ([character](../annotated/character.md)).
- **Transforms.** An entity's transform space is at +0x38; the transform is selected by two bytes in
  it ([entity](../annotated/entity.md)).
- **Properties.** Components connect to data bus properties by name hash (djb2-xor, the hash Frosty
  calls Fnv1). The health component's `IsImmortal` is one: GridNode's god mode sets it.

## Engine systems

- **Settings**: `SettingsManager` keeps every settings object by type; the console clones a settings
  object when it changes one ([console](../annotated/console.md)).
- **Console**: `Console_executeCommand` runs any command, including the ones the menus hide.
- **Messages**: three message managers (core, client, server); listeners register per category
  ([messages](../annotated/messages.md)).
- **Physics**: ray casts go through the server world's physics, as the game's ray cast entities do
  ([physics](../annotated/physics.md)).

## Faith

[Characters](characters.md) covers Faith's objects, her movement state and what it takes to move,
jump or protect her. [Input](input.md) covers how keys become actions.
