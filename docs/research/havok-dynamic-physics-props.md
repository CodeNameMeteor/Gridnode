# Havok Dynamic Physics Simulation & Movable Props: Complete Reverse Engineering

## Executive Summary

Mirror's Edge Catalyst executes its physical simulation using a heavily customized build of **Havok 2013 (`hknp`)**. Previously, custom collision geometry in mods (e.g. MECMP and GridNode standalone collision boxes in [`sdk/src/bodies.cpp`](https://github.com/CodeNameMeteor/Gridnode/blob/main/sdk/src/bodies.cpp)) was restricted to **static immovable geometry** (`m_motionId = 0`, `MOTION_STATIC`).

This research report documents the reverse engineering of Havok's **dynamic rigid body simulation pipeline**, revealing how to instantiate movable props that react to gravity, bounce against static architecture, collide with Faith's character controller (`CharacterPhysicsEntity` / `hknpCharacterProxy`), receive physical impulses (punches, kicks, explosions), and fire contact callbacks when collisions occur.

---

## 1. Havok 2013 (`hknp`) Architectural Hierarchy & Core Strides

Havok 2013 decouples the collision geometry (`hknpBody`) from the integration dynamics (`hknpMotion`). The `hknpWorld` maintains parallel flat arrays indexed by dense integer handles:

```
+---------------------------------------------------------------------------------------+
|                                      hknpWorld                                        |
+---------------------------------------------------------------------------------------+
|  +0x10: m_bodyPool        (Body allocation bitmap & free list)                        |
|  +0x20: m_bodies          (Array of hknpBody records; STRIDE = 0x90 / 144 bytes)     |
|  +0xd0: m_motionPool      (Motion allocation bitmap & free list)                      |
|  +0xe0: m_motions         (Array of hknpMotion records; STRIDE = 0x80 / 128 bytes)   |
|  +0x398: m_simulation     (Simulation islands manager & active body scheduler)        |
|  +0x4e0: m_motionProperties (Motion properties library: damping, gravity factors)     |
|  +0x540: m_eventManager   (Collision event dispatcher & signal slots)                 |
+---------------------------------------------------------------------------------------+
```

### The Three Fundamental Strides:
1. **`hknpBody` (Stride `0x90` = 144 bytes)**: Located at `*(hknpWorld + 0x20) + bodyId * 0x90`. Contains the body's world transform, reference to collision shape (`hknpShape*`), collision filter flags, and its linked motion ID.
2. **`hknpMotion` (Stride `0x80` = 128 bytes)**: Located at `*(hknpWorld + 0xe0) + motionId * 0x80`. Contains the dynamic simulation state: center of mass, orientation quaternion, 16-bit half-float inverse inertia/mass tensor, world linear velocity, and local principal angular velocity.
3. **`hknpMotionProperties`**: Stored in a lookup table at `*(hknpWorld + 0x4e0)`. Governs sleep thresholds, stabilization heuristics, velocity damping, and gravity scaling factors.

---

## 2. Memory Layout of `hknpBody` (144 bytes / `0x90`)

A decompiled inspection of `hknpBody::initialize` (`0x143f38340`) and `hknpWorld_createBody` (`0x143f41880`) reveals the complete field layout:

| Offset | Type | Field Name | Description |
|---|---|---|---|
| `0x00` | `float[4]` | `transform.right` | World orientation right vector $(R_x, R_y, R_z, 0)$ |
| `0x10` | `float[4]` | `transform.up` | World orientation up vector $(U_x, U_y, U_z, 0)$ |
| `0x20` | `float[4]` | `transform.forward` | World orientation forward vector $(F_x, F_y, F_z, 0)$ |
| `0x30` | `float[4]` | `transform.trans` | World position translation vector $(T_x, T_y, T_z, 1)$ |
| `0x50` | `void*` | `m_shape` | Pointer to `hknpShape` (Box, Sphere, Capsule, Convex) |
| `0x58` | `uint32_t` | `m_flags` | Bitfield: `0x1` = Static, `0x2` = Dynamic, `0x4` = HasMass, `0x8` = Active |
| `0x5c` | `uint32_t` | `m_collisionFilter` | Collision layer / mask (matches Frostbite collision layers) |
| `0x60` | `uint32_t` | `m_bodyId` | Dense index in `m_bodies` |
| `0x64` | `uint32_t` | `m_nextAttachedBodyId` | Linked list index of next body sharing this motion |
| `0x68` | `uint32_t` | `m_motionId` | Index into `m_motions` (`0` for static immovable bodies) |
| `0x6e` | `uint8_t` | `m_motionType` | `1` = Static, `2` = Dynamic, `5` = Keyframed |
| `0x70` | `int32_t` | `m_simulationIslandId` | Active simulation island (`-1` when deactivated/sleeping) |
| `0x7c` | `int16_t` | `m_maxLinearVelocity` | Scaled speed limit clamp |
| `0x88` | `uint64_t` | `m_userData` | Pointer to Frostbite `PhysicsEntity` or packed owner ID |

---

## 3. Memory Layout of `hknpMotion` (128 bytes / `0x80`)

Decompiled from `hknpWorld_allocateMotion` (`0x143f43540`), `hknpMotion_initialize` (`0x143f76890`), and `hknpWorld_setBodyVelocity` (`0x143f3af60`):

| Offset | Type | Field Name | Description |
|---|---|---|---|
| `0x00` | `float[4]` | `m_centerOfMass` | Center of mass position in world space $(C_x, C_y, C_z, 0)$ |
| `0x10` | `float[4]` | `m_orientation` | Orientation quaternion $(q_x, q_y, q_z, q_w)$ of principal inertia frame |
| `0x20` | `uint16_t` | `m_invInertia[0]` | 16-bit half-float inverse inertia $I_{xx}^{-1}$ |
| `0x22` | `uint16_t` | `m_invInertia[1]` | 16-bit half-float inverse inertia $I_{yy}^{-1}$ |
| `0x24` | `uint16_t` | `m_invInertia[2]` | 16-bit half-float inverse inertia $I_{zz}^{-1}$ |
| `0x26` | `uint16_t` | `m_invMass` | 16-bit half-float inverse mass $m^{-1}$ |
| `0x28` | `uint32_t` | `m_firstAttachedBodyId` | Head of linked list of bodies attached to this motion |
| `0x38` | `uint16_t` | `m_lookAheadDistance` | CCD collision lookahead distance |
| `0x3a` | `uint16_t` | `m_dampingFactor` | Half-float velocity damping coefficient |
| `0x40` | `float[4]` | `m_linearVelocity` | World-space linear velocity vector $(v_x, v_y, v_z, 0)$ |
| `0x50` | `float[4]` | `m_angularVelocity` | Angular velocity $(\omega_x, \omega_y, \omega_z, 0)$ in local principal inertia frame |

---

## 4. Mass Properties & Inertia Tensor Calculation (`0x1434cf0d0`)

When instantiating a dynamic body in Frostbite's `PhysicsEntityFactory_createBody` (`0x143516450`), passing `motionType = 2` automatically triggers `FUN_1434cf0d0`:

1. **Shape Mass Property Extraction**: `FUN_1434d12f0` computes the volumetric shape inertia matrix.
2. **Principal Axis Diagonalization**: The inertia tensor is diagonalized into principal moments of inertia $I_{xx}, I_{yy}, I_{zz}$ and an orientation quaternion $\mathbf{q}_{\text{inertia}}$.
3. **Half-Float Inverse Storage**: Inverse values are computed via `rcpps` and converted to 16-bit half-floats:
   $$\text{invMass} = \frac{1}{M}, \quad I_{xx}^{-1} = \frac{1}{I_{xx}}, \quad I_{yy}^{-1} = \frac{1}{I_{yy}}, \quad I_{zz}^{-1} = \frac{1}{I_{zz}}$$
4. **Motion Registration**: `hknpWorld_allocateMotion` (`0x143f43540`) claims a motion index, sets initial velocities to zero, and attaches it to the body via `FUN_143f44fb0`.

---

## 5. Direct Impulse Application & Velocity Manipulation API

Ghidra decompilation of Havok's simulation primitives reveals the exact mathematical equations implemented by the engine:

### A. Linear Impulse (`0x143f3b450`)
```c
void hknpWorld_applyLinearImpulse(hknpWorld* world, uint32_t bodyId, const float* impulse)
{
    hknpBody* body = &world->m_bodies[bodyId];
    if (!(body->m_flags & 2)) return; // Must be dynamic
    if ((body->m_flags & 9) == 0) hknpWorld_activateBody(world, bodyId); // Wake up

    hknpMotion* motion = &world->m_motions[body->m_motionId];
    float invMass = halfToFloat(motion->m_invMass);

    motion->m_linearVelocity.x += invMass * impulse[0];
    motion->m_linearVelocity.y += invMass * impulse[1];
    motion->m_linearVelocity.z += invMass * impulse[2];

    hknpWorld_markMotionModified(world, bodyId);
}
```

### B. Angular Impulse (`0x143f3b4f0`)
Computes $\Delta \boldsymbol{\omega} = \mathbf{I}^{-1} \cdot (\mathbf{q}^{-1} \cdot \mathbf{L}_{\text{world}} \cdot \mathbf{q})$:
```c
void hknpWorld_applyAngularImpulse(hknpWorld* world, uint32_t bodyId, const float* impulse)
{
    hknpBody* body = &world->m_bodies[bodyId];
    if (!(body->m_flags & 2)) return;
    if ((body->m_flags & 9) == 0) hknpWorld_activateBody(world, bodyId);

    hknpMotion* motion = &world->m_motions[body->m_motionId];
    
    // Rotate world torque impulse into principal inertia frame
    Vec3 L_local = quatRotateInverse(motion->m_orientation, impulse);

    // Multiply by diagonal inverse inertia
    motion->m_angularVelocity.x += halfToFloat(motion->m_invInertia[0]) * L_local.x;
    motion->m_angularVelocity.y += halfToFloat(motion->m_invInertia[1]) * L_local.y;
    motion->m_angularVelocity.z += halfToFloat(motion->m_invInertia[2]) * L_local.z;

    hknpWorld_markMotionModified(world, bodyId);
}
```

### C. Point Impulse at Contact Point (`0x143f3b610`)
Applies linear force and resulting off-center torque simultaneously:
$$\mathbf{r} = \mathbf{P}_{\text{contact}} - \mathbf{C}_{\text{mass}}$$
$$\Delta \mathbf{v} = m^{-1} \mathbf{J}, \quad \Delta \boldsymbol{\omega} = \mathbf{I}^{-1} (\mathbf{r} \times \mathbf{J})$$

---

## 6. The Complete Havok 2013 Command Protocol (`0x143f7bc40`)

Havok maintains a command queue (`hknpApiCommandProcessor`) capable of receiving commands either synchronously or queued across worker threads:

| Opcode | Command Name | Arguments | Description |
|---|---|---|---|
| `0x00` | `createBody` | `id, cinfo` | Instantiates a body structure in the world pool |
| `0x01` | `addBody` | `id, flags, lastInBatch` | Links body into spatial broadphase |
| `0x02` | `destroyBody` | `id` | Frees body back to pool |
| `0x03` | `removeBody` | `id, lastInBatch` | Unlinks body from broadphase |
| `0x06` | `setBodyTransform` | `id, transform, activation` | Teleports body to new 4x4 matrix |
| `0x07` | `setBodyPosition` | `id, position, activation` | Updates translation row |
| `0x08` | `setBodyOrientation`| `id, orientation, pivot` | Sets orientation quaternion |
| `0x09` | `setBodyVelocity` | `id, linVel, angVel` | Overwrites linear and angular velocity |
| `0x0a` | `setBodyLinearVelocity` | `id, linVel` | Sets linear velocity only |
| `0x0b` | `setBodyAngularVelocity` | `id, angVel` | Sets angular velocity only |
| `0x0c` | `applyLinearImpulse` | `id, impulse` | Adds linear velocity $\Delta v = m^{-1} J$ |
| `0x0d` | `applyAngularImpulse` | `id, impulse` | Adds angular velocity $\Delta \omega = I^{-1} L$ |
| `0x0e` | `applyPointImpulse` | `id, impulse, position` | Computes $r \times J$ and applies both |
| `0x0f` | `setPointVelocity` | `id, velocity, position`| Matches velocity at a contact point |
| `0x11` | `setBodyMotion` | `id, motionId` | Links body to a motion record |
| `0x12` | `setBodyMass` | `id, mass` | Recomputes mass and inertia scalars |
| `0x15` | `setBodyMotionProperties` | `id, propertiesId` | Assigns damping and sleep settings |
| `0x16` | `setBodyMaterial` | `id, materialId` | Replaces friction / restitution material |
| `0x18` | `activateBody` | `id, activeFlag` | Awakens simulation island (`0x143f43fa0`) |
| `0x19` | `setBodyCollisionFilterInfo` | `id, filterInfo` | Overwrites collision mask |
| `0x25` | `setGravity` | `gravityVec4` | Modifies global gravity vector |
| `0x26` | `setMaterialFriction` | `materialId, dynamicFriction`| Modifies material friction coefficient |
| `0x27` | `setMaterialRestitution` | `materialId, restitution` | Modifies material restitution (bounciness) |

---

## 7. Collision Contact Events & Signal Listeners

Frostbite hooks into Havok 2013 collision events using `HavokRigidBodyHook_registerSignals` (`0x1434f9580`). The events are dispatched via `hkSignal2<hknpEventHandlerInput const&, hknpEvent const&>`:

### Registered Event Signals:
- **Type 1 (`manifoldStatusSignalBase`)**: Contact point creation, persistence, or destruction.
- **Type 2 (`manifoldProcessSignalBase`)**: Active contact solver constraint execution.
- **Type 5 (`contactImpulseSignalBase`)**: Collision impulse transmission (`hknpContactImpulseEvent`).
- **Type 6 (`contactImpulseClippedSignalBase`)**: Excessive impact impulses clamped by restitution limits.
- **Type 10 (`bodyActivationSignalBase`)**: Body transitions between active and sleeping.

### Unpacking Contact Impulse Events (`0x143f3ddf0`):
When a contact event fires, the payload contains:
- `bodyIdA` (`event + 0x08`) & `bodyIdB` (`event + 0x0c`).
- `status` (`event + 0x20`): `0 = NONE`, `1 = STARTED` (initial impact), `2 = FINISHED` (separation), `3 = CONTINUED` (sliding/resting).
- `impulses` (`event + 0x30`): 4-component vector of normal and friction impulses.
- `hknpContactImpulseEvent_unpack` (`0x143f3ddf0`) decompresses the manifold to extract:
  - World contact normal unit vector $\mathbf{N}$.
  - Up to 4 contact manifold positions $\mathbf{P}_0, \mathbf{P}_1, \mathbf{P}_2, \mathbf{P}_3$ in world space.

---

## 8. Mod Developer Guide: Spawning Movable Props in GridNode

To create a movable dynamic box, sphere, or custom physical prop that reacts to gravity and Faith's physical body:

```cpp
// 1. Allocate Shape
float halfExtents[4] = { 0.5f, 0.5f, 0.5f, 0.0f }; // 1m x 1m x 1m cube
void* config = hknpConvexShape_BuildConfig_ctor(alloca(0x30));
void* shape = hknpConvexShape_createFromHalfExtents(halfExtents, 0.01f, config);

// 2. Allocate and Configure Dynamic Motion
uint32_t motionId = hknpWorld_allocateMotion(hkWorld, /*defaultProperties*/ 0);
alignas(16) float massProps[16];
float massKg = 25.0f; // 25 kg prop
hknpMotion_computeInertia(hkWorld, shape, massKg, massProps);

// 3. Populate Body Creation Info
alignas(16) uint8_t cinfo[0x60] = {};
hknpBodyCinfo_ctor(cinfo);
*reinterpret_cast<void**>(cinfo + 0x00) = shape;
*reinterpret_cast<uint32_t*>(cinfo + 0x0c) = motionId; // Dynamic motion ID!
cinfo[0x10] = donor.quality;    // e.g. DYNAMIC_QUALITY (0x02)
*reinterpret_cast<uint16_t*>(cinfo + 0x12) = donor.material; // Material with restitution/friction
*reinterpret_cast<uint32_t*>(cinfo + 0x14) = donor.filter;   // Collision layer
*reinterpret_cast<uint32_t*>(cinfo + 0x1c) = 0x02;           // MOTION_DYNAMIC flag

// 4. Initial Spawn Transform
float initialPos[4] = { spawnX, spawnY + 2.0f, spawnZ, 1.0f }; // 2m in air
float initialQuat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
memcpy(cinfo + 0x30, initialPos, 16);
memcpy(cinfo + 0x40, initialQuat, 16);

// 5. Create & Add Body to Simulation
uint32_t bodyId = 0;
hknpWorld_createBody(hkWorld, &bodyId, cinfo, /*addImmediately*/ 0);
*reinterpret_cast<uint64_t*>(havokBody(hkWorld, bodyId) + 0x88) = donor.userData;
hknpWorld_addBodies(hkWorld, &bodyId, 1, 0);

// The prop will immediately fall under gravity and interact with all world geometry!
```
