# Characters: Spawning, Outfits, Bone Poses, and Vocals

## 1. Architectural Overview

Mirror's Edge Catalyst represents characters using a distributed Frostbite entity architecture split across server and client worlds:

```
                  SERVER REALM                                CLIENT REALM
     +-------------------------------------+     +-------------------------------------+
     |    ServerCharacterSpawnEntity       |     |      ClientCharacterSpawnEntity     |
     |   (PamServerCharacterSpawnEntity)   |     |                                     |
     +-------------------------------------+     +-------------------------------------+
                       |                                            ^
             spawn (0x143390830)                                    | Network
                       v                                            | Replication
     +-------------------------------------+                        |
     |     PamServerCharacterEntity        | -----------------------+
     |   - CharacterCustomizationComponent |
     |   - ServerReplayPlayerComponent     |
     +-------------------------------------+
                       |
                       +--------------------------------------------+
                                                                    v
                                                 +-------------------------------------+
                                                 |      PamClientCharacterEntity       |
                                                 |  +0x158: Spatial Transform          |
                                                 |  +0x334: ghostShader (bool)         |
                                                 |  Components:                        |
                                                 |   - PamClientReplayPlayerComponent  |
                                                 |   - ClientAntAnimatableComponent    |
                                                 |   - ClientCharacterCameraComponent  |
                                                 +-------------------------------------+
```

This research synthesizes the four essential pillars needed for a comprehensive character API:
1. **Character Spawning & Safe Teardown**
2. **Outfits & Visual Customization**
3. **Bone Pose Injection & Replay Puppeteering**
4. **Vocals & 3D Spatial Audio Attachment**

---

## 2. Pillar 1: Character Spawning & Teardown

### A. Spawn Mechanism (`ServerCharacterSpawnEntity::spawn`)
Spawning occurs on the server game thread via `ServerCharacterSpawnEntity::spawn` (vtable slot `+0xd0`, `0x143390830`):
- **Signature**: `void* spawn(ServerCharacterSpawnEntity* spawner, SpawnInfo* info)`
- **`SpawnInfo` Layout** (size `0x70` bytes, 16-byte aligned):
  | Offset | Field | Type | Description |
  | :--- | :--- | :--- | :--- |
  | `+0x00` | `transform` | `LinearTransform` | 4x4 matrix: right (`+0x0`), up (`+0x10`), forward (`+0x20`), trans (`+0x30`) |
  | `+0x40` | `player` | `void*` | `ServerPlayer*` or `null` for AI and replay ghosts |
  | `+0x48` | `existing` | `void*` | Reused character entity, or `null` to create new |
  | `+0x50` | `unk50` | `void*` | Context pointer (typically `null`) |
  | `+0x58` | `unk58` | `int32_t` | `-1` |
  | `+0x5c` | `busId` | `int32_t` | `-1` (engine allocates next bus ID via `counter & 0x7f`) |
  | `+0x60` | `unk60` | `uint8_t` | `1` |
  | `+0x61` | `useTransform`| `uint8_t` | `1` = spawn at `info->transform`; `0` = spawn at spawner's local position |

### B. Internal Blueprint Instantiation (`ServerCharacterSpawnEntity_createCharacter`, `0x143390a30`)
1. **Blueprint Resolution**:
   - If `player != null`: Uses `player + 0x540` (`characterBlueprint`).
   - If `player == null`: Calls `spawner->vtable->vf_0x138`. If null, falls back to `spawnerData->m_blueprint` (`CharacterSpawnReferenceObjectData + 0x18`).
2. **Entity Bus**:
   - Instantiates a dedicated `EntityBus` via `fb_EntityBus_create(owner, parentBus, spawnerData, busId, ...)`.
3. **Creation Info**:
   - Populates `ServerEntityCreationInfo` (`0x140` bytes) via `0x14335cab0`:
     - `ci.bus = newBus`
     - `ci.kind = 2` (Character kind)
     - `ci.existing = info->existing`
     - `ci.transform = info->transform`
4. **Instantiation**:
   - Calls `fb_Blueprint_instantiate(&result, &ci, blueprint, ...)` (`0x14335e390`).
   - Appends created character pointer to `spawner + 0x148` / `+0x150` (`vector<ServerCharacterEntity*>`).
   - Writes back pointer: `character + 0x138 = spawner`.
5. **Client Replication**:
   - The server character replicates across the network. The client receives `PamClientCharacterEntity` via network replication (`0x14347d65d`), and notifies listeners via event `CharacterSpawned` (`0xa29c98e6`).

### C. Safe Teardown (`ServerCharacterSpawnEntity::unspawn`, `0x14338e4c0`)
MECMP previously suffered from ghost accumulation because "ghosts were never destroyed". The native unspawn path must be executed on the server thread:
- **Call**: `spawner->vtable->vf_0x108(spawner, character, destroy = true, 0)` (`0x14338e4c0`).
- **Execution Flow**:
  1. Checks if a dummy player was created for the character (`character->player + 0x1398 != 0`); if so, unlinks and destroys it (`0x143351330`, `0x14334c2d0`).
  2. Calls `ServerCharacterSpawnEntity_removeSpawned` (`0x14338e5d0`):
     - Removes the character from the spawner's vector (`spawner + 0x148`..`+0x150`).
     - Clears character back-pointer: `character + 0x138 = null`.
     - Calls `Entity_requestDestroy(character, character->bus)` (`0x143167700`) if `destroy == true`.
     - Clears flag bit 9 (`flags &= ~0x200`).
     - If the spawner's spawned vector is now empty, fires event `0xb6d9fdc4` ("AllUnspawned") and unregisters message listener `0x1286cf79`.

---

## 3. Pillar 2: Outfits & Visual Customization

### A. The Customization Pipeline (`PamServerCharacterEntity::applyCustomization`, `0x14336cf00`)
Visual looks (outfits, clothes, armor, hair, gloves) are applied to `PamServerCharacterEntity` via vtable slot `+0x340`:
- **Argument**: `CustomizeRequest* request` (size `0x20` bytes):
  | Offset | Field | Type | Description |
  | :--- | :--- | :--- | :--- |
  | `+0x00` | `character` | `void*` | `PamServerCharacterEntity*` target |
  | `+0x08` | `player` | `void*` | `null` to bypass unlock progression checks and apply directly |
  | `+0x10` | `customization`| `CustomizeCharacterData*` | Pointer to asset defining visual meshes and materials |
  | `+0x18` | `skip` | `uint8_t` | `0` = execute customization; non-zero = no-op |

### B. Component Application (`ServerCharacterEntity_applyCustomizeData`, `0x14336c860`)
1. Queries the character for `fb::TI_ServerCharacterCustomizationComponent` via `character->getComponent` (vtable slot `+0x108`, `0x1431c6eb0`).
2. Reads `CustomizeCharacterData`:
   - `+0x18`: `VisualUnlockGroup` array (e.g. Head, Torso, Legs, Hands, Feet).
   - `+0x20`: Unlock asset list (`VisualUnlockAsset*`).
   - `+0x28`: Fade/transition time (`float`).
   - `+0x30`, `+0x31`: Override flags.
3. Assembles selected visual unlocks into an active unlock set.
4. Invokes `FUN_143377950(customizationComponent, unlockSet, flags)` to bind meshes.
5. Flags replication dirty: `NetState_setDirty(customizationComponent + 0x1410, 0x20)`.
6. On the client, `ClientCharacterCustomizationComponent` streams and replaces skeletal sub-meshes and material instances.

### C. Runner Ghost Shader (`PamClientCharacterEntity + 0x334`)
- When creating a ghost runner projection:
  - `PamClientCharacterEntity + 0x334` stores `bool ghostShader`.
  - When set to `true`, the character material pipeline switches from standard deferred PBR shading (`SS_Character_02`) to the semi-transparent blue projection shader (`PamUseRunnersGhostShader`).
  - This can be toggled per-character dynamically without reloading assets.

---

## 4. Pillar 3: Driving Bone Poses (ANT & Replay Puppeteering)

### A. Component Architecture (`PamClientReplayPlayerComponent`, `0x141c48a28`)
Spawned characters that serve as ghosts or puppets contain `PamClientReplayPlayerComponent` (size `0x310` bytes).
Key member offsets:
| Offset | Name | Type | Description |
| :--- | :--- | :--- | :--- |
| `+0x180` | `poseBufferInput` | `BoneTransform*` | Array of incoming keyframe bone transforms |
| `+0x1d8` | `poseBufferActive`| `BoneTransform*` | Active target skeletal pose buffer |
| `+0x1c8` | `character` | `PamClientCharacterEntity*` | Owner client character entity |
| `+0x200` | `boneCount` | `uint32_t` | Number of animated bones (typically 65 to 90) |
| `+0x2e0` | `timeScale` | `float` | Playback speed multiplier |
| `+0x2e4` | `poseValid` | `uint8_t` | Flag: pose contains valid data |
| `+0x2e5` | `poseAccepting` | `uint8_t` | Flag: component is active and accepting poses |
| `+0x2e6` | `skipRoot` | `uint8_t` | Flag: bypass automatic root motion placement |

### B. Bone Transform Format (Stride `0x20` / 32 bytes)
Each bone in `poseBufferInput` (`+0x180`) consists of two 128-bit SIMD vectors:
```cpp
struct BoneTransform {
    float translation[4]; // x, y, z, w (translation vector, w is uniform scale/flag)
    float rotation[4];    // x, y, z, w (unit quaternion: x, y, z, w)
};
```

### C. The Root Placement Drift Bug & Resolution (`skipRoot` at `+0x2e6`)
In `PamClientReplayPlayerComponent_update` (`0x1439501a0`):
1. **When `skipRoot == 0` (Default)**:
   The engine automatically computes the world transform of the puppet from Bone 0 (Root) and Bone 1 (`AITrajectory`):
   $$\vec{P}_{\text{world}} = \vec{t}_0 + w_0 \cdot \mathbf{R}(q_0) \vec{t}_1$$
   $$\mathbf{R}_{\text{world}} = q_0 \otimes q_1$$
   It then calls `SpatialEntity_setTransform` (`0x142fd8f20`) on `character + 0x158`.
   - **Why MECMP Suffered Altitude Drift**: Because Bone 1 stores coordinates in world trajectory space, any infinitesimal rotation error in Bone 0 is multiplied by the distance from the origin ($d = \|\vec{t}_1\|$). At 200 meters from world origin, a $0.05^\circ$ pitch error shifts Faith vertically by:
     $$\Delta h \approx 200 \times \sin(0.05^\circ) \approx 0.174 \text{ meters}$$
     over time accumulating massive vertical desyncs.
2. **When `skipRoot == 1`**:
   The engine bypasses the automatic root transformation call.
   - The mod explicitly sets the character's world position and rotation via `SpatialEntity_setTransform` or direct write to `character + 0x158`.
   - The replay component copies only relative skeletal rotations for Bone 1 through `boneCount - 1`.
   - **Result**: Zero altitude drift, zero position jitter, and completely stable playback!

---

## 5. Pillar 4: Vocals & 3D Spatial Audio Attachment

### A. The VoiceOver System (`*0x1423e3150`)
Character vocalizations, dialogues, and mission voice lines are managed by `VoiceOverSystem`:
- **Global Instance**: Read via `*(void**)0x1423e3150`.
- **Submitting Conversations**: `VoiceOverSystem_submitConversation` (`0x1430477f0`):
  ```cpp
  void VoiceOverSystem_submitConversation(
      void* voSystem,
      void* conversationInfo,
      uint16_t realm,       // EntityBus_getLevel(bus) + 0x90
      uint32_t* outHandle   // Returned handle for active speech tracking
  );
  ```
- **Binding Voice to Character**: `VoiceOverSystem_setSources` (`0x1430467f0`):
  ```cpp
  void VoiceOverSystem_setSources(
      void* voSystem,
      void* conversationInfo,
      const void** sourceEntities, // Array of PamClientCharacterEntity*
      int32_t startIndex,
      uint32_t count
  );
  ```
  This binds the conversation's audio emitter directly to the character's transform holder (`character + 0x158`). As the character moves, walks, or jumps, the voice moves dynamically in full 3D binaural / surround space.

- **Stopping Conversations**: `VoiceOverSystem_cancelConversation` (`0x143042720`):
  Cancels active speech by handle and clears pending queues at `voSystem + 0x17df0`.

### B. One-Shot Character Barks & SFX (`SoundSystem_createVoice`)
For short vocal barks (e.g. KSEC combat callouts, exertion grunts, pain cries):
- Call `SoundSystem_createVoice` (vtable slot `+0x40`, `0x142fe2ee0`).
- Update emitter position each frame via `Voice_setTransform` (slot `+0x68`, `0x14301dce0`) using `character + 0x158`.
- Trigger vocal sound asset via `Voice_event` (slot `+0x0`, `0x14301d7e0`).

---

## 6. Open Research Follow-ups

1. **Standalone Spawner Entity Factory**:
   - Rather than borrowing a live `PamServerCharacterSpawnEntity` from the city, research creating an isolated spawner using `ServerEntityCreationInfo_ctor` (`0x14335cab0`) with a synthesized `CharacterSpawnReferenceObjectData` asset to guarantee zero interaction with game spawners.
2. **`CustomizeCharacterData` Asset Enumeration**:
   - Reverse engineer the asset loader table to catalog all built-in `CustomizeCharacterData` assets (e.g. Faith default, Prisoner outfit, Runner outfits, KSEC Enforcer, Sentinel, Protector armor).
3. **ANT Direct Pose Model Hook**:
   - Investigate whether bone transforms can be written directly to `ClientAntAnimatableComponent` without requiring `PamClientReplayPlayerComponent`, enabling bone posing on standard AI and NPC characters that lack replay components.
