# Havok (`hknp`) reference, checked

A reference written by another model (2026-11-06), checked against our annotations and Ghidra. This page keeps what
holds, marks what is wrong and lists what is still open for whoever researches it next. Layouts we rely on are in
`db/annotations/physics.yaml` (the source of truth); this page does not repeat their notes.

## Holds (matches our annotations, confirmed in Ghidra)

| Claim | Where it is annotated |
|---|---|
| `PhysicsWorld +0xe0` = the `hknpWorld`; `+0xb84` = 1 for the client's effect world | `PhysicsWorld_HavokWorld`, `PhysicsWorld_EffectFlag` |
| `hknpWorld +0x20` body array (0x90 each), `+0xe0` motion array (0x80 each), `+0x150` filter owner, `+0x4c0` gravity | `HavokWorld_Bodies`, `_Motions`, `_FilterOwner`, `_Gravity` |
| Body: `+0x30` position, `+0x40` orientation, `+0x68` motion id (0 = static), `+0x88` user data | `HavokWorld_Bodies` note |
| User data: low 16 bits = record index (`PhysicsWorld +0x130`), bits 16..23 = owner table index (manager `+0xf0`), high 32 bits = `(kind & 0xf) << 4` | seen in the body factory `0x143516450` |
| Motion: `+0x10` orientation, `+0x40` linear velocity, `+0x50` angular | `HavokWorld_Motions` |
| Body capacity 16,384 from the description `+0x64`, and the 65,536 fix | `PhysicsWorldDesc_BodyCapacity` |
| Layers: 1 Static, 2 Dynamic, 3 Player, 4 AI, 14 Character, 21 CameraCollision | `gn_ray_hit_ex.layer`, trainer log |
| Three worlds: client main, client effect, server | `bodies.cpp` |
| Box shape `0x143f2f140` with a 0.01 skin (our `hknpConvexShape_createFromHalfExtents`); capsule `0x143f6b310` (used by the body factory for type 3) | physics.yaml, `0x143516450` |
| `0x1434fc450` (called from a vtable slot) calls `0x143f60710` (the support check); `0x1434e4400` calls `0x143f601d0` (the proxy constructor) | callers/callees in Ghidra |

## Wrong, or mixed up

- **The `hknpBody` struct mixes in the creation info.** `+0x10 quality`, `+0x12 material`, `+0x14 filter` are the
  body *creation info* (`hknpBodyCinfo`) layout. In the live body the filter is at **`+0x5c`**, the material at
  **`+0x6c`** and the quality at **`+0x6e`**, flags at `+0x58` (our `bodies.cpp` census and `makeBody` use both).
- **"Frostbite" chain at the top.** `0x142401ca0` is `ClientGameWorld`, not a "ClientPhysicsContext", and
  `PhysicsManager +0xf0` is the *owner table* (indexed by bits 16..23 of a body id), not "per world index".
  Worlds come from `GameWorlds[realm]` (`0x1424a9b98`); the manager is `world +0x50`, the factory list `+0x58`.
- **Body id** (the game object's id, not Havok's): "low 24 bits index, high 8 serial" is wrong. Read in `0x1434f6810` (2026-11-06): `id = (typeTag << 24) | (world's byte at +0xb84 << 16) | recordIndex`, the record index being the slot in the physics world's table at `+0x130`. The standalone boxes got ids like `0x01000637`, as this says. (Havok's own body index in `+0x60` equals its place in the array.) The "owner" the manager's table (`+0xf0`) holds is the `PhysicsWorld` itself, picked by the effect flag.
- **Body count fields** (`+0x28`, `+0x2c`): our `freePlaces` reads the used count at `hknpWorld +0x68`.

## Learned since, from the standalone boxes (API 34)

- A game physics object's id (the low 32 bits of a body's user data) is `(owner index << 16) | record index`: the owner
  table is `manager +0xf0`, the records `owner +0x130`. A body is made, placed and enabled by the barrier's calls
  (`PamOBBCollision_enableClient`), not by Havok calls alone; see `progress.md`, "API 34".
- Standalone boxes work in game (2026-11-06: ids `0x01000637...`, made at the next flush). A barrier's body setup is an array of `RigidBodyData` in its entity data, with the count at `ptr - 4`. The reflection puts it at
  data `+0x70` (`PhysicsBodies`); the client enable reads `+0x78`. The first in-game test found nothing at `+0x78`
  for 289 client barriers; with `+0x70` tried first the factory made the box.
- Not Havok, but found on the way: `PamUISystem +0x8c8` is a pointer to the pause controller (Ghidra's struct had it
  inline).

## Second pass: claims read by another model, and what I checked

A second reference (2026-11-06) says it read the following in Ghidra. Checked by me: the **ray filter 9** (`Raycast_query`
sets `PhysicsRayInput +8 = 9` for rays and sphere sweeps; confirmed) and the **id format** (above). Not rechecked, so
still claims: the character proxy numbers (contact tolerance 0.1, keep distance 0.05, slope limit from
`CharacterPhysicsData +0x44` default 45 degrees, up vector = normalised minus gravity at `cinfo +0x50`, in
`0x1434e4400`), and the compound shape tree (the node formula in `0x143e99090`, `HavokAabb6Step` = 1/226). Their
formula `min_child = min_parent + (b >> 4)^2 * step`, `max_child = max_parent - (b & 15)^2 * step` with
`step = (parent.max - parent.min) / 226` is the one already in the annotations.

## Not checked (open for research)

- `hknpCharacterProxy` (vtable `0x141d53348`; its slots still need a proper read) and the numbers listed above. A gravity trick (Faith walking on
  a tilted gravity) would be the test; `set_gravity` (API 21) already changes the value.
- `hknpWorld` vtable `0x141d51418` and size `0x5a0`, its count and capacity fields (`+0x28`, `+0x2c`: ours reads the used count at `+0x68`); `hknpStaticCompoundShape` instance array (`+0x60`, stride 0x80) and tree (`+0xc0`).
- `GroupRigidBody` at `0x1434eda80` (it is a 117-byte function: probably a small constructor or getter, not the class).

Sources: `havok_physics_engine_reversing_reference.md` and `havok-reference.md` (attached to the session, not committed).
