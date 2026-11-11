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

## Confirmed from Ghidra decompilation (still to be double checked in game)

The character proxy numbers, formulas, and gravity analysis have now been verified in Ghidra and mathematically proven (see [character-controller-proxy.md](character-controller-proxy.md) and `db/annotations/physics.yaml`):
- **Character proxy creation (`0x1434e4400`) & `hknpCharacterProxy_ctor` (`0x143f601d0`)**:
  - `CharacterPhysicsBody` (vtable `0x141bab5e0`), proxy at `+0x50`, support info at `+0x108`, up-vector at `+0x120`.
  - `hknpCharacterProxy` (vtable `0x141d53348`, size `0x130`).
  - Contact tolerance `0.1f` (`0x3dcccccd` at `cinfo + 0x48`).
  - Keep distance `0.05f` (`0x3d4ccccd` at `cinfo + 0x74`).
  - Max solver speed `10.0f` (`0x41200000` at `cinfo + 0x78`).
  - Collision layer `14` (`Character` layer at `cinfo + 0x70`).
  - Max slope angle read from `CharacterPhysicsData + 0x44` (default 45 degrees, converted to radians).
  - Proxy precomputes $\cos(\theta_{\text{max}}) \approx 0.70710678$ at `proxy + 0x120` using $\sin(\theta + \pi/2)$.
  - Up-vector derived from world gravity at `world + 0x80`: $\hat{u} = \text{normalize}(-\vec{g}_{\text{world}})$, stored at `cinfo + 0x50` and written to `proxy + 0xd0` and `body + 0x120`.
- **Support check (`0x143f60710`) & caller (`0x1434fc450`)**:
  - `0x1434fc450` reads `body + 0x120`, negates it to obtain downward direction $\hat{d} = -\hat{u}$, and calls `checkSupport(proxy, downDir, supportInfo, collector)`.
  - Evaluates contact normal $\hat{n}$: if $\cos^2(\theta_{\text{max}}) \le 1 - (\hat{n} \cdot \hat{d})^2 = \sin^2(\alpha)$, the slope is sliding (state 2). If $\le 45^\circ$, ground is supported (state 1). No contacts = unsupported (state 0).
- **The gravity trick & dynamic behavior**:
  - The proxy up-vector does **not** follow gravity dynamically. Calling `set_gravity` (API 21) updates `hknpWorld + 0x4c0`, but leaves `body + 0x120` and `proxy + 0xd0` pointing $(0, 1, 0)$. Contacts with walls evaluate as sliding/unsupported.
  - To walk on walls/ceilings, a mod must simultaneously update `world + 0x4c0`, `body + 0x120`, and `proxy + 0xd0`.
  - What breaks: Camera orientation hardcodes world Y-up (causing inversion and gimbal lock singularities); ANT two-bone foot IK casts downward along $(0, -1, 0)$ and misses the wall; movement controller fall threshold $v_y < -5.0 \text{ m/s}$ triggers fall states during vertical wall runs.

- **`hknpWorld` allocation (`0x1434d0ff0`), vtable & size**:
  - Size is exactly `0x5a0` (1440 bytes), allocated aligned to `0x10` via `hkMemoryRouter::alignedAlloc` (`0x143cd0cf0`).
  - Primary vtable is `0x141d51418` (`hknpWorld::vftable`).
  - `+0x10`: `m_bodyManager` (embedded object, size `0x90`, initialized in `0x143f5da50`).
  - `+0x20`: `m_bodies` (`hknpBody*` array, stride `0x90` / 144 bytes).
  - `+0x28`: `m_bodyCapacity` (total allocated slots, e.g. 16,384 + 1).
  - `+0x2c`: `m_bodyCapacityMask` (`0x3fffffff`).
  - `+0x68`: `m_numBodiesUsed` (live/active body count, read by GridNode's `freePlaces`).
  - `+0xe0`: `m_motions` (`hknpMotion*` array, stride `0x80` / 128 bytes).
  - `+0x4c0`: `m_gravity` (`hkVector4`).
- **Static compound shape, `GroupRigidBody` & 6-byte AABB tree**:
  - `GroupRigidBody` (`0x1434eda80`): `GroupRigidBody_getInstanceTransform` takes `(GroupRigidBody* self, LinearTransform* out, uint32_t partIndex)`. It reads part instance index from `self + 0x110 + partIndex * 4`, retrieves `hknpStaticCompoundShape*` from `self + 0x58`, and delegates to `hknpShapeInstance_buildTransform` (`0x143f50740`).
  - `hknpShapeInstance` at `compound + 0x60`: stride is `0x80` (128 bytes). `0x143f50740` multiplies rotation matrix columns (`+0x00`, `+0x10`, `+0x20`) by scale (`+0x40`) and translation (`+0x30`) to output a 64-byte `LinearTransform`.
  - AABB tree at `compound + 0xc0`: 6-byte compressed nodes (`m_nodes` at `+0x10`, count at `+0x18`, root min at `+0x20`, root max at `+0x30`).
  - Node formula confirmed in `0x143e99090`: instruction `mulps xmm6, [0x1425a8e10]` where `0x1425a8e10` holds four floats equal to $0.0044247787 \equiv \frac{1}{226}$. Formula: $\text{step} = (\text{parent.max} - \text{parent.min}) / 226$; $\text{child.min} = \text{parent.min} + (b \gg 4)^2 \times \text{step}$; $\text{child.max} = \text{parent.max} - (b \ \& \ 15)^2 \times \text{step}$. Byte 3 bit 7 = inner node flag (right child offset $= (((b_3 \ \& \ \text{0x7f}) \ll 16) \ | \ u_{16}) \times 2$); for leaf nodes, bytes 4..5 are `int16_t` instance index into `m_instances`.
- **`hknpBody` flags bitfield at `+0x58`**:
  - Bit 0 (`0x0001`): `IS_STATIC` (motion ID 0, set in `0x143f5e54a`).
  - Bit 1 (`0x0002`): `IS_DYNAMIC` (simulated rigid body, set in `0x143f5e6e3`).
  - Bit 2 (`0x0004`): `IS_KEYFRAMED` (kinematic body moved by game code, set in `0x143f3c41a` when mass = 0, cleared in `0x143f3c680`).
  - Bit 3 (`0x0008`): `IS_ACTIVE` (awake in simulation island, set in `0x143f5f076`, cleared in `0x143f5f21a` when asleep).
  - Bit 10 (`0x0400`): `TRANSFORM_DIRTY` / `AABB_DIRTY` (set in `0x143f3ce7f`, `0x143f41c7f`, `0x143f423a2` to trigger broadphase AABB update).
  - Immovable body test: Frostbite checks `test byte ptr [body + 0x58], 5` (testing Static `0x1` | Keyframed `0x4`).

## Not checked (open for research)

All previously unverified claims regarding `hknpWorld` size `0x5a0`, capacity fields, static compound shape 6-byte AABB trees, `GroupRigidBody`, and body flags have now been decompiled and confirmed. Detailed technical documentation lives in [havok-world-internals.md](havok-world-internals.md).

Sources: `havok_physics_engine_reversing_reference.md` and `havok-reference.md` (attached to the session, not committed).
