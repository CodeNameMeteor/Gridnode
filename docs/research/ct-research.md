# The cinematic tools' research: what GridNode can use

A contributor shared the research behind their cinematic tools for Mirror's Edge Catalyst (47
`Research_*.md` files, 2026-10-22 to 2026-11-03, about 2,300 game addresses). They name game build
0x5762E3B7, the build GridNode checks for, and the addresses match ours where both name one (8 of 8).
Four were also checked in Ghidra on 2026-11-03 and read as the research says: the world description
read at 0x1434E51F2, `hknpWorld::createBody` 0x143F41880, and the fall check's compare at
0x142CE8632 .. 0x142CE864C.

Nothing here is in `db/annotations/` yet: each address goes in when the feature that uses it is built
and the function is checked in Ghidra. The sections are the research's own; "proven in game" means
their tools ran it.

## Most useful first

| # | What | The research's addresses | Status there | For GridNode |
|---|---|---|---|---|
| 1 | **Havok body buffer full crash.** The game makes its worlds with room for 16,384 bodies and 8,192 moving ones, and its "buffer full" handlers do nothing, so Havok writes past the buffer. | World made by 0x1434E4DA0(world, desc): desc +0x64 bodies, +0x68 motions, +0x6C constraints, +0x40 broadphase, +0x50 gravity. Handlers 0x1434E54B9 .. 0x1434E5530. | Proven in game (raised to 65,536). | A stability fix in GridNode for every mod that makes or keeps bodies (layout copies, API 18). Also the world's body count for a "physics: N of M bodies" line. |
| 2 | **Making collision boxes.** Static box bodies in the client world, made, moved and destroyed safely. | BuildConfig 0x143F2E490, box from half extents 0x143F2F140 (radius 0.01), convex from points 0x143F2E510, capsule 0x143F6B310, `hknpBodyCinfo` 0x143F38A70, `createBody` 0x143F41880, `addBodies` 0x143F41A80, `destroyBodies` 0x143F423E0, `setBodyTransform` 0x143F3A6B0. The game's flush 0x1434F4C80 (the only place records die); hit to game ids 0x1435177B0. | Proven in game, after a crash: a body must carry a live static body's user data (+0x88), re-checked right after each flush. | A `body_add_box` / `body_remove` API, through the flush, with the donor rules. The safer alternative is the game's own `OBBCollisionEntity` (chain 0x143335BC0 → 0x143333310 → 0x1435169E0 → 0x143516450), which registers itself. |
| 3 | **Collision layers.** `RigidBodyCollisionLayer` 0 .. 26 (Static 1, Dynamic 2, Player 3, AI 4, Character 14, CameraCollision 21, ...); the low bits of a body's filter info. Ray filter 9 is the game's sweep filter. | Enum from the type info; hknpBody +0x5C filter, +0x6C material. | Read from the type info. | Report the layer in `raycast_object` hits; filter rays by layer. |
| 4 | **Game speed.** `GameTimeSettings.TimeScale` (+0x48). The game's `SlowMotionEntity` writes it every frame while easing and writes 1.0 when it goes (its update 0x1433362A0, destructor 0x143332D10), so a plain slider is overwritten. Above 1x the game manages at most frame rate / 30. | Main loop 0x142A2D470 calls 0x1432F4E90(sim timer, scale) once a frame. | Proven in game. | The trainer's speed slider: multiply on top of the game's own slow motion instead of fighting it, and say the 1x+ limit. |
| 5 | **No fall death, kill volumes off.** The fall check is Ant's `RayCastCollisionProcessorAsset` (vtable 0x141A62EE0): `HasFallenPastThreshold = -FallThreshold (+0x40, default 10 m) > distance`, run by 0x142CE8200. Kill volumes' inside test 0x143970D30. | As left. | Proven in game. | Trainer toggles: no fall death (raise +0x40 for the call), and "this kill volume off" from the Volumes tab. |
| 6 | **Removing an entity.** Every removal path ends in the destroy request 0x143167700(entity, owner): returns if +0x18 bit 10 is set, else the owner's vtable slot 2 queues it. | Spawner "Unspawn" loop 0x143383F48; AI coordinator slot 24 0x143933580. | Read in code. | Compare with `docs/research/destroying-entities.md`; a `entity_destroy` call. |
| 7 | **Sounds.** The sound system `[0x1423DD580]` slot 8 makes a voice from a sound asset; a voice's slot 0 fires an event, slot 13 places it; the listener `[0x1423E29B8] + 0x30`. | `SoundEntity` vtable 0x141AD0338 (handler slot 4 0x14301A170). | Proven (their replay plays sounds). | A "play this sound here" API for mods. |
| 8 | **Local lights.** `PBRAnalyticLightEntity` and its sphere, spot, tube and rectangle classes; `UpdateLight` is slot 23 (0x14376A030); the data's fields. | Vtables 0x141BFF0A0 .. 0x141BFF3D0. | Proven (their Lights tab). | Lights in the inspector; a light API later. |
| 9 | **Time of day.** `TimeOfDayManagerEntity` (vtable 0x141C48868), its clock `TimeOfDayClockEntity.Time`, and why a jump lags (Enlighten's pipeline). | Several. | Proven. | A time of day API, if mods want one. |
| 10 | **Faith's size.** Ant's pose record keeps a uniform scale; one on her root bone scales her model about her feet. Her collision can't be resized with what's known. | Pose math 0x142C1D5B0, pose copy 0x142FD9D50. | Read in code, not tried. | Research only. |

Also in the research, for later: the game's job system and the hang it can get into (0x142FB9250),
missions' start, end and abort, the car swarm and its road network, crowds, weather and snow, shadows
and screen-space reflections, the KrugerSec types and their AI, and how the game's data patch is laid out.

## Built (2026-11-03)

- 1: GridNode's `physicsStart` (world.cpp) hooks `PhysicsWorld_create`.
- 4 and 5: the trainer's game speed slider and "No fall death".
- 3 is waiting on one Ghidra trace: our ray hit gives the game's body id, and the filter info is on
  Havok's body (hknpBody +0x5c), which the hit doesn't lead to yet.

## Build order (proposed)

1. The body buffer fix (1), and the hit's collision layer (3): small, and they make API 18 safer.
2. The trainer's game speed (4) and no fall death (5).
3. Collision boxes (2), through `OBBCollisionEntity` if it can be made cleanly, else raw bodies with
   the donor rules.
4. Sounds (7) and the destroy request (6) as mod API.
