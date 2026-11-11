# Havok World Internals, Compound Shapes & Body Flags

Reverse engineering research for Havok Physics 2014/2015 (`hknp`) integration in Mirror's Edge Catalyst (*Frostbite 3*).

> [!NOTE]
> This is purely research from Ghidra decompilation and binary disassembly for now, to be double-checked and verified in game during runtime execution.

---

## 1. `hknpWorld` Layout, Allocation & Capacity Fields

### 1.1 Allocation & Construction
The Havok simulation world is allocated and constructed during `PhysicsWorld_create` (`0x1434e4da0`):
- **Allocation (`0x1434d0ff0`)**:
  Calls `hkMemoryRouter::alignedAlloc(0x5a0, 0x10)` (`0x143cd0cf0`).
  - Allocation size: **`0x5a0` bytes** (1440 bytes).
  - Alignment: **`16` bytes** (`0x10`).
- **Constructor (`0x143f3e820`)**:
  - Primary vtable: **`0x141d51418`** (`hknpWorld::vftable`).
  - Sets up internal managers, broadphase, island manager, constraint solver, and motion systems.

### 1.2 Layout & Key Member Offsets

| Offset | Type | Member Name | Description |
|---|---|---|---|
| `+0x00` | `void*` | `__vftable` | Primary vtable (`0x141d51418`). |
| `+0x10` | `hknpBodyManager` | `m_bodyManager` | Embedded body manager object (size `0x90`, initialized in `0x143f5da50`). |
| `+0x20` | `hknpBody*` | `m_bodies` | Array of bodies (stride **`0x90`** / 144 bytes). |
| `+0x28` | `uint32_t` | `m_bodyCapacity` | Total allocated slot capacity (e.g. 16,384 + 1 from settings). |
| `+0x2c` | `uint32_t` | `m_bodyCapacityMask` | Capacity bitmask / flags (typically `0x3fffffff`). |
| `+0x68` | `uint32_t` | `m_numBodiesUsed` | Live active body count (read by GridNode's `freePlaces` to calculate available slots). |
| `+0x70` | `uint32_t` | `m_numActiveBodies` | Count of awake bodies in the active simulation island. |
| `+0xe0` | `hknpMotion*` | `m_motions` | Array of motions (stride **`0x80`** / 128 bytes), indexed by `body->m_motionId`. |
| `+0x150`| `void*` | `m_filterOwner` | Collision filter owner (`FilterOwner_Filter` at `+0x4d0`). |
| `+0x4c0`| `hkVector4` | `m_gravity` | World gravity vector (X, Y, Z, W). Read by solver at every step (`0x143f474f0`). |

---

## 2. Static Compound Shapes & 6-Byte AABB Tree

Static geometry groups (e.g. static mesh collections, buildings, complex props) use `hknpStaticCompoundShape` to bundle hundreds of primitive child shapes into a single rigid body with a hierarchical compressed bounding volume tree.

### 2.1 `hknpStaticCompoundShape` Layout
- `+0x60`: `m_instances` (pointer to array of `hknpShapeInstance`, stride **`0x80`** / 128 bytes).
- `+0xc0`: `m_tree` (embedded `hkcdStaticAabbTree`).

### 2.2 `hknpShapeInstance` Layout (`0x80` bytes)
Built into a 64-byte `LinearTransform` by `hknpShapeInstance_buildTransform` (`0x143f50740`):
- `+0x00`: Rotation row/column 0 (`Vector4`).
- `+0x10`: Rotation row/column 1 (`Vector4`).
- `+0x20`: Rotation row/column 2 (`Vector4`).
- `+0x30`: Translation ($\vec{t} \in \mathbb{R}^3$, $w=1.0$).
- `+0x40`: Scale ($s_x, s_y, s_z, \dots$).
- `+0x50`: Child shape pointer (`hknpShape*`).
- `+0x60`..`+0x7f`: Flags, material ID, filter info, and runtime cached properties.

### 2.3 6-Byte AABB Node Format & Decompression (`0x143e99090`)
The AABB tree at `compound + 0xc0` compresses bounding boxes down to **6 bytes per node** to minimize cache misses during broadphase and ray traversal:
- `tree + 0x10`: `m_nodes` (pointer to 6-byte node buffer).
- `tree + 0x18`: `m_numNodes` (`uint32_t`).
- `tree + 0x20`: `m_domainMin` (`Vector4`, domain bounding box min).
- `tree + 0x30`: `m_domainMax` (`Vector4`, domain bounding box max).

#### Node Byte Layout
Each 6-byte node represents a box quantized within its parent's domain:
- **Byte 0**: Quantized X-axis bounds (high nibble: min, low nibble: max).
- **Byte 1**: Quantized Y-axis bounds (high nibble: min, low nibble: max).
- **Byte 2**: Quantized Z-axis bounds (high nibble: min, low nibble: max).
- **Byte 3**:
  - `Bit 7` (`0x80`): Node type flag:
    - `1`: **Inner node** (branch).
    - `0`: **Leaf node**.
  - `Bits 0..6` (`0x7f`): High 7 bits of the right child offset.
- **Bytes 4..5** (`uint16_t`):
  - **For inner nodes**: Low 16 bits of right child offset. Left child is always at index $i + 1$. Right child is at index:
    $$\text{rightChild} = i + \left( ((b_3 \ \& \ \text{0x7f}) \ll 16) \ | \ u_{16} \right) \times 2$$
  - **For leaf nodes**: `int16_t` index into the compound shape's `m_instances` array (`compound + 0x60`).

#### Mathematical Decompression Proof
At instruction `0x143e99233`, the decompressor executes:
```assembly
mulps xmm6, cs:0x1425a8e10
```
Inspection of `0x1425a8e10` (`HavokAabb6Step`) reveals four 32-bit floats:
$$\text{float}(0.0044247787) \equiv \frac{1}{226.0}$$
Because $15^2 = 225$, squaring the 4-bit nibbles $[0..15] \to [0..225]$ covers $[0..225/226]$ of the parent domain with quadratic precision clustering towards the boundaries:
- For each axis $k \in \{X, Y, Z\}$:
  $$\text{step}_k = (\text{parentMax}_k - \text{parentMin}_k) \times \frac{1}{226.0}$$
  $$\text{childMin}_k = \text{parentMin}_k + \left( \text{byte}_k \gg 4 \right)^2 \times \text{step}_k$$
  $$\text{childMax}_k = \text{parentMax}_k - \left( \text{byte}_k \ \& \ \text{0x0f} \right)^2 \times \text{step}_k$$

---

## 3. `GroupRigidBody` Architecture (`0x1434eda80`)

In Frostbite 3, static mesh groups share a single Havok rigid body (`GroupRigidBody`) containing an `hknpStaticCompoundShape`.
`0x1434eda80` is **`GroupRigidBody_getInstanceTransform`**:
```cpp
void GroupRigidBody_getInstanceTransform(GroupRigidBody* self, LinearTransform* outTransform, uint32_t partIndex)
{
    // Part-to-instance mapping table at +0x110
    int16_t instanceIndex = *(int16_t*)((uintptr_t)self + 0x110 + partIndex * 4);
    
    // Static compound shape pointer at +0x58
    hknpStaticCompoundShape* compound = *(hknpStaticCompoundShape**)((uintptr_t)self + 0x58);
    
    // Instance array at +0x60, stride 0x80 (128 bytes)
    hknpShapeInstance* instance = &compound->m_instances[instanceIndex];
    
    // Multiply rotation matrix columns by scale vector (0x143f50740)
    hknpShapeInstance_buildTransform(instance, outTransform);
}
```

---

## 4. `hknpBody` Flags Breakdown (`body + 0x58`)

The 32-bit bitfield at `body + 0x58` tracks the body's simulation type, awake state, and broadphase dirty flags:

```
Bit:   31 ... 11   10   9   8   7   6   5   4   3   2   1   0
Flags: [Unused]   DIRTY .   .   .   .   .   .  ACT KEY DYN STA
```

| Bit | Hex Mask | Flag Name | Set Location | Cleared Location | Meaning |
|---|---|---|---|---|---|
| **0** | `0x0001` | `IS_STATIC` | `0x143f5e54a` | `0x143f3c70c` | Static body (motion ID = 0, zero mass, never moved by solver). |
| **1** | `0x0002` | `IS_DYNAMIC` | `0x143f5e6e3` | `0x143f3c8dc` | Dynamic body (simulated by solver with non-zero mass and velocities). |
| **2** | `0x0004` | `IS_KEYFRAMED` | `0x143f3c41a` | `0x143f3c680` | Keyframed kinematic body (infinite mass, moved directly by transform). |
| **3** | `0x0008` | `IS_ACTIVE` | `0x143f5f076` | `0x143f5f21a` | Awake in active simulation island. Cleared when body falls asleep. When asleep, `body + 0x74` is reset to `0x7fffffff`. |
| **4** | `0x0010` | `IS_INTERNAL` | `hknpBodyCinfo` | -- | Internal solver body. |
| **5** | `0x0020` | `IS_ADDED` | `0x143f41880` | `0x143f423e0` | Body is registered into broadphase. |
| **10**| `0x0400` | `TRANSFORM_DIRTY` / `AABB_DIRTY` | `0x143f3ce7f`, `0x143f41c7f`, `0x143f423a2` | broadphase step (`0x143f474f0`) | Transform or velocity changed; broadphase must recompute bounding box. |

### Immovable Body Optimization
In collision queries, Frostbite frequently checks `test byte ptr [body + 0x58], 5` (e.g. at `0x143f429fe` and `0x143f620ee`).
Testing mask `0x05` checks:
$$\text{Bit 0} \lor \text{Bit 2} = \text{IS\_STATIC} \lor \text{IS\_KEYFRAMED}$$
Bodies with either bit set have infinite effective mass, allowing solver optimizations to skip momentum calculations.

---

## 5. Summary of Confirmations

| Feature | Offset / Address | Confirmed Detail |
|---|---|---|
| `hknpWorld` Size | `0x5a0` | Allocated via `hkMemoryRouter::alignedAlloc(0x5a0, 0x10)` at `0x1434d0ff0`. |
| `hknpWorld` Vtable | `0x141d51418` | Stored in `rax` during constructor `0x143f3e820`. |
| Live Body Count | `world + 0x68` | `m_numBodiesUsed`, read by `freePlaces`. |
| Total Slot Capacity | `world + 0x28` | Max capacity (`m_bodyCapacity`). |
| Capacity Mask | `world + 0x2c` | `m_bodyCapacityMask` (`0x3fffffff`). |
| Motion Stride | `0x80` (128 bytes) | `world->m_motions[motionId]` at `world + 0xe0`. |
| Body Stride | `0x90` (144 bytes) | `world->m_bodies[bodyId]` at `world + 0x20`. |
| AABB Compression Step | `0x1425a8e10` | Four `0.0044247787f` floats $\equiv 1/226.0$. |
| Node Size | 6 bytes | Nibble-packed X, Y, Z min/max, 15-bit child offset, 1-bit inner flag. |
| `GroupRigidBody` getter | `0x1434eda80` | `GroupRigidBody_getInstanceTransform` reading part map `+0x110` and compound `+0x58`. |
| Transform builder | `0x143f50740` | `hknpShapeInstance_buildTransform` multiplying rotation rows by scale `+0x40`. |
