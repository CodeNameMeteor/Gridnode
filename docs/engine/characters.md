# Characters

## Finding Faith

| | |
|---|---|
| Client | `LocalClientCharacter()` (0x1439655f0), also the global at 0x142106a68 |
| Server | the server player manager's players (+0x268), `ServerPlayer +0x1478`, checked by the character's player back pointer (+0x250); fallbacks while she respawns |

GridNode does this for mods: `gn::localClientCharacter()`, `gn::localServerCharacter()`.

## What hangs off her

On the client (`PamClientCharacterEntity`, [character](../annotated/character.md)):

| Offset | |
|---|---|
| +0x30 | her data, `PamCharacterEntityData` (shared) |
| +0xb0 | the movement owner: +0x48 -> +0 is the movement object below |
| +0x198 | `PamClientHumanHealthComponent` |
| +0x218 | `PamCharacterBlueprint` |
| +0x238 | her `ClientPlayer` (which points back at +0x14a0) |
| +0x250 | `PamClientCharacterCameraComponent` (+0x70: camera forward) |
| +0x370.. | the component list: impact material, customization, player entry, capsule collision, ANT driven, ANT input, camera, player info, ANT query, health, replay recording, move tracker, effects |

The server side has the same shape with server components (ANT driven, move tracker, player info,
entity interaction, ANT query, health).

## Movement

The movement object (not reflected; [faith](../annotated/faith.md)) has two blocks:

| | |
|---|---|
| ground block | +0x4 last ground Y, +0x17 on ground |
| state block | +0x18 walls used, +0x38 wall climbs, +0x3c wall runs, +0x90 movement state |

- **Last ground Y** is where falls are measured from. A long fall is a death, not damage, so god mode
  does not cover it; keeping last ground Y at her height does (MEC_Trainer's no-stumble).
- **Movement state 8** is set when she jumps. After a jump off a wall run she touches the ground for
  a few frames still in state 8; a jump then keeps her speed (the wall run bunny hop).
- **Speed**: character +0x28 -> +0x1f0 -> +0x10 -> +0x438.

`gn::movement()` / `gn::setMovement()` read and write all of it.

## Movement tuning

Run speed, friction, jump heights, wall run and wall climb lengths, coil, slide and mag rope values
belong to the animation system's (ANT) assets, which reflection does not describe. GridNode reaches
them by fixed paths, checks every object on the way by its vtable, and only writes a value after it
has seen the game's default there (`gn::setTuning("MaxRunSpeed", ...)`). The values are shared and
stay until set back.

## Moving her

| | |
|---|---|
| `gn::teleport` | the game's own teleport (server thread). Moves her, her components and the camera |
| `gn::setPosition` | writes the position the animation system holds (client). The camera stays free, so it suits noclip |

## Health

The server health component connects `IsImmortal` (hash 0xa1b7b692) at +0xc0 when it is made
(0x14391e410 -> 0x14474f280). Setting that bool makes her immortal, as the game's own logic would;
it is per component, so a respawn needs it again. `gn::godMode(true)` does this.
