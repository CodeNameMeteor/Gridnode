# Open Research & Reverse-Engineering Master Tracking

Master tracking index for advanced reverse-engineering research on Mirror's Edge Catalyst (Frostbite 3, build `0x5762E3B7`, Image Base `0x140000000`).

> [!NOTE]
> All findings summarized here are derived from Ghidra decompilation, reflected type definitions (`gridnode-reflected.json`), and binary verification. All findings represent reverse-engineering research to be confirmed during runtime execution in game.

---

## Task board (open research tasks and their status)

Agents and people post research tasks here, update the status as the work moves, and **remove the row when
the work is implemented** (in the SDK/API or its annotations). The dashboard below stays as the log of where each
topic's research lives; when a task is removed, make sure the dashboard (or the topic's research document) records
where the findings are.

Status flow: `planned` (brief written) > `researching` (claims being collected) > `verifying` (checking claims in
Ghidra) > `testing` (claim-lab experiment in game) > `proven` (claims confirmed in game, SDK/API change next) >
`implementing` (SDK/API change being built and tested in game). Implemented: row removed.

| Task | Topic | Status | Next step | Research doc / annotations | Updated |
|---|---|---|---|---|---|

---

## Master Open Research Tracking Dashboard

| # | Topic | Status | Research Document | Database Annotations | Key Technical Facts Reversed | Active Open Research Frontiers |
|---|---|---|---|---|---|---|
| **1** | **Standalone Collision** | Reversed & Documented | [`standalone-collision.md`](standalone-collision.md) | `physics.yaml` | `DummyBarrierEntity` (`+0x30`, vtable `0x141c4e258`) eliminates barrier borrowing, 3s delay, and reload crash (`0x143913840`). Shapes 1, 2, 3, 7 via `0x143516450`. | Dynamic bodies (motion type 2, inertia tensors), contact listener callbacks, compound barrier placement. |
| **2** | **Havok Character Controller** | Reversed & Documented | [`character-controller-proxy.md`](character-controller-proxy.md) | `physics.yaml` | `CharacterPhysicsBody` (`0x141bab5e0`), `hknpCharacterProxy` (`0x141d53348`), support query `0x143f60710` ($\cos(	heta_{\text{max}}) \approx 0.7071$). Dynamic up-vector sync enables wall/ceiling walking. | Camera view matrix decoupling (`ClientCameraViewManager`), ANT ground-normal override, trainer up-vector sync hook. |
| **3** | **Characters: Spawning, Outfits, Poses, Voices** | Reversed & Documented | [`characters-spawning-outfits-poses-voices.md`](characters-spawning-outfits-poses-voices.md) | `character.yaml`, `spawn.yaml`, `audio.yaml` | `ServerCharacterSpawnEntity::spawn` (`0x143390830`), clean teardown (`0x14338e4c0`). Outfits via `0x14336cf00`. Bone pose injection via `0x1439501a0` (`skipRoot` bypasses altitude drift). Vocals via `0x1430477f0`. | Isolated standalone spawner instantiation without level spawners, `CustomizeCharacterData` asset catalog, direct ANT pose injection. |
| **4** | **Sound Parameters & Velocity** | Reversed & Documented | [`sound-parameters-velocity.md`](sound-parameters-velocity.md) | `audio.yaml` | Slot `+0x18` (`0x14301da40`) is `Voice::setParameter` (key in `EDX`, float in `XMM2`). Slot `+0x78` clear velocity, slot `+0x80` update velocity (`0x143021f60`). 3D velocity vector at `*(float[4]*)(*(voice + 0x28) + 0x40)`. | 3D cone directivity curves, music/ambience state switches (`0x24ffcb4b`), real-time sound bus ducking / master gain. |
| **5** | **UI & Pause System** | Reversed & Documented | [`ui-pause-system.md`](ui-pause-system.md) | `ui.yaml` | `UIPauseController_request` (`0x143b16210`) driven by `PamClientUIPauseEntity` (`0x141cbd4a8`). Pause formula: $\text{isPaused} = (A \ge 1) \land (s < 10^{-6} \lor B \ge 1)$. Instant pause via `request(ctrl, 1, 1)`. | Direct timescale override (`subview + 0x3c`), input routing during mod pause, screen counter matrix in game. |
| **6** | **Streaming: Focus Points** | Reversed & Documented | [`freecam-streaming-and-focus-points.md`](freecam-streaming-and-focus-points.md) | `levels.yaml` | Vanilla game does NOT stream around freecam (`ZoneStreamerFocus_add` `0x143c7e7c0` only called for Faith). API 30 essential. Focus manager at `streamer + 0x280`. OOB crash fix: set `rec + 0x14 \|= 1`. | Sublevel pre-streaming integration (`SubLevelManager_drainPendingSubWorlds`), zone search radius parameter tuning. |
| **7** | **Map Markers & Presence** | Reversed & Documented | [`map-markers-and-presence.md`](map-markers-and-presence.md) | `ui.yaml`, `online.yaml` | `PresenceInfoManagerPool` (`0x141c74bb8`), `PamClientPresenceInfoManagerEntity` (`0x141c74be8`). Slot layout (`0x50` bytes, `worldPos` at `+0x30` requires 16-byte alignment). Priority 1000 locks slot. Hook at `0x141c74bc8`. | Offline custom tag texture injection, 3D pin/beacon ray recoloring, tooltip rich text dynamic player stats. |
| **8** | **Dash & Time Trial Events** | Reversed & Documented | [`dash-and-time-trial-events.md`](dash-and-time-trial-events.md) | `messages.yaml`, `ui.yaml` | Social play category `0x54776fc6` (35 messages mapped). Finish payload `0xd71d027f`: `runCs = (newRecord \|\| !hadRecord) ? val : val + delta`. UGC time trials `0x9b35497c` separated from ReachThis via `CheckpointOrder != 0`. | Intermediate checkpoint split timing events, trajectory replay upload format (`0x955f5cbd`), dynamic UGC checkpoint injection. |
| **9** | **Visual Environment & Timelines** | Reversed & Documented | [`visual-environment.md`](visual-environment.md) | `render.yaml`, `entity.yaml`, `faith.yaml` | Destination buffer `worldRenderer + 0x85470` (`0xb30` bytes, 8 parameter blocks). Flicker eliminated via Render Hook on **Slot 21** (`0x141bfaca8`). `TimelineEntity` (`0x141beab28`, size `0x490`), scrubbing via `Timeline_SetTime` (`0x143691120`, `force = true`). | Custom 3D LUT texture injection (`0x143797400`), Camera View Manager autofocus raycast coupling, dynamic timeline track injection. |
| **10** | **Other Builds of the Game** | Deprioritized / Skipped | N/A | `fingerprint.yaml` | Skipped per project directive: retail EA App build (`0x5762E3B7`) is the sole target build; multi-build fingerprinting deferred. | Cross-build signature scanning if Steam or updated builds are targeted in the future. |
| **11** | **Custom Maps, Meshes & Assets** | Reversed & Documented | [`custom-maps-assets-pipeline.md`](custom-maps-assets-pipeline.md) | `resources.yaml`, `render.yaml`, `spawn.yaml`, `levels.yaml` | Dynamic partition injection into `compartment + 0x158` (DJB2-XOR hash). Hardware-instanced static model groups (`StaticModelGroupEntityData` `0xb0`, member placement `0x14318fbc0`, placement hash `0x143b722e0`). Material parameter overrides. Root bus anchoring (`EntityBus_getLevel` `0x14314c5e0`). | Synthetic DX11 vertex buffer generation from raw OBJ/GLTF, dynamic Enlighten radiosity bypass, Havok compound runtime triangle insertion. |
| **12** | **Havok World Internals & Compounds** | Reversed & Documented | [`havok-world-internals.md`](havok-world-internals.md) | `physics.yaml`, `havok-reference.md` | `hknpWorld` allocation (`0x1434d0ff0`, size `0x5a0`, vtable `0x141d51418`). Static compound shape instance array `+0x60` (stride `0x80`), 6-byte AABB tree decompressed via step formula with constant `0x1425a8e10` ($1/226.0$). `GroupRigidBody_getInstanceTransform` (`0x1434eda80`). | Broadphase dynamic AABB rebuilding under rapid kinematic mutation, runtime body slot capacity exhaustion, part-to-instance mesh alignment. |
| **13** | **Missions, Objectives & Custom Checkpoints** | Reversed & Documented | [`missions-objectives-checkpoints.md`](missions-objectives-checkpoints.md) | `levels.yaml`, `events.yaml`, `messages.yaml` | `PamServerMissionManagerEntity` (`0x141c7add0`), `PamClientMissionEntity` (`0x141c6b180`), `ServerObjectiveEntity` (`0x141b4eaa8`), `PamServerCheckpointEntity` (`0x141c7e750`). Events `ShowObjective` (`0xd0488c09`), `HideObjective` (`0x23ccf4aa`), `Success` (`0x7f3e7106`), `OnSuccess` (`0xbb06a967`). Checkpoint respawn at `*DAT_14257c9d8 + 0xe0`. Reload via `ServerLevel_reloadLastCheckpoint` (`0x143341d40`). | Dynamic blueprint sequence stitching for custom campaign chapters, offline save game injection for objective state, custom audio voice line synchronization during objective milestones. |

---

## Executive Progress Summary Metrics

- **Total Research Categories Tracked**: 13
- **Fully Reverse-Engineered & Documented**: 12 / 13 (**92.3%**)
- **Deprioritized / Out-of-Scope**: 1 / 13 (Option 10: Other builds)
- **Total Database Annotated Symbols**: 1,085 items (38 classes, 50 structs, 70 globals, 907 functions, 20 hooks, 10 events across 27 files)
- **Strict Database Validation Status**: `gridnode.py validate --strict` passes with **0 errors, 0 warnings**
- **Follow-Up Deep-Dive Research Documents**: 13 dedicated Markdown reports in `docs/research/`
- **Current Operational Status**: Pure reverse-engineering research derived from Ghidra and reflected metadata; pending runtime confirmation during in-game testing.

---

## 1. Standalone collision, all the way

Boxes made by the game's own factory work in the client world (`boxBackendNative`, API 34; [progress](../progress.md)).

Follow-up research lives in [standalone-collision.md](standalone-collision.md) (with annotations in `db/annotations/physics.yaml`). This is purely research from Ghidra disassembly for now and will be confirmed in the future when tested in game:

### 1.1 Key Reverse-Engineering Breakthroughs
- **No dependence on a live barrier & level reload crash fix**: The crash at `FUN_143913840 + 0x5` was caused by dereferencing freed barrier data when Faith touches a box after a level reload. `SurfaceTouch_classifyBody` (`0x143913840`) reads the parent registered in `PhysicsManager` via `PhysicsManager_objectOfId` (`0x1434ee250`). A static pinned dummy barrier entity (`DummyBarrierEntity` at `+0x30` with native vtable `0x141c4e258` and traversal flags at `data + 0x90`..`0x93`) completely eliminates barrier borrowing, removing both the 3-second load delay and the reload crash.
- **Other primitive collision shapes**: `PhysicsEntityFactory_createBody` (`0x143516450`) creates Box (type 1, half-extents at `+0x20`), Sphere (type 2, `0x143f6c460`, radius at `+0x20`), Capsule (type 3, `0x143f6b310`, radius at `+0x20` and height at `+0x24`), and Prebuilt shapes (type 7, pointer at `+0x20`).
- **Server world collision parity**: Verified in `PamOBBCollision_enable` (`0x143333310`). `ServerGameWorld` (`GameWorlds[1]`) uses the exact same `PhysicsEntityFactory` and creation routine with realm = 1.
- **Dynamic body simulation & movable props**: Fully reversed in [`havok-dynamic-physics-props.md`](havok-dynamic-physics-props.md). Dynamic motion records (`hknpMotion`, stride `0x80`, array at `world + 0xe0`), mass/inertia derivation (`0x1434cf0d0`), impulse application (`hknpWorld_applyLinearImpulse` `0x143f3b450`, `hknpWorld_applyAngularImpulse` `0x143f3b4f0`, `hknpWorld_applyPointImpulse` `0x143f3b610`), velocity overrides (`hknpWorld_setBodyVelocity` `0x143f3af60`), and simulation island activation (`0x143f43fa0`).
- **Collision contact listener callbacks & events**: Fully reversed in [`havok-dynamic-physics-props.md`](havok-dynamic-physics-props.md). Signal dispatch via `hknpEventManager` (`world + 0x540`, `0x143f7d0b0`) using event signals (1 = `manifoldStatus`, 2 = `manifoldProcess`, 5 = `contactImpulse`, 6 = `contactImpulseClipped`, 10 = `bodyActivation`). Frostbite bridge `HavokRigidBodyHook_registerSignals` (`0x1434f9580`) and contact manifold decompression `hknpContactImpulseEvent_unpack` (`0x143f3ddf0`) extracting contact normal and 4 contact points.

### 1.2 Areas Needing Even More Research
- **Compound OBB barrier placement**: Programmatically spawning multi-box static barriers via `PamOBBCollisionEntity` without requiring manual OBB entity data cloning.
- **Cloth and soft-body modifiers**: Reversing `hknpSoftContactModifier` and `hknpMassChangerModifier` for non-rigid deformation effects.

### 1.3 In-Game Verification Test Plan
1. Spawn a native static collision box via API 34 at Faith's coordinates.
2. Confirm Faith can stand, jump, and wall-climb on the box without falling through.
3. Trigger a level reload (e.g. reload last checkpoint). Verify that touching the box post-reload causes no crash (`0xC0000005` at `0x143913845`).

---

## 2. Havok character controller

Follow-up research lives in [character-controller-proxy.md](character-controller-proxy.md) (with annotations in `db/annotations/physics.yaml`). This is purely research from Ghidra decompilation and mathematical analysis for now and will be confirmed in the future when tested in game:

### 2.1 Key Reverse-Engineering Breakthroughs
- **Object hierarchy & constants**: `CharacterPhysicsBody` (vtable `0x141bab5e0`, proxy at `+0x50`, support info at `+0x108`, up-vector at `+0x120`) and `hknpCharacterProxy` (vtable `0x141d53348`, size `0x130`, normalized up-vector at `+0xd0`, $\cos(	heta_{\text{max}})$ at `+0x120`). Constants confirmed: `contactTolerance = 0.1f` (`cinfo + 0x48`), `keepDistance = 0.05f` (`cinfo + 0x74`), `maxCharacterSpeedForSolver = 10.0f` (`cinfo + 0x78`), `layer = 14` (`Character`), and `maxSlope` from `CharacterPhysicsData + 0x44` (default $45^\circ$).
- **Up-vector derivation**: Derived at initialization (`0x1434e4790`) as $\hat{u} = \text{normalize}(-\vec{g}_{\text{world}})$. For normal gravity $(0, -9.81, 0)$, $\hat{u} = (0, 1, 0)$.
- **Support check (`0x143f60710`)**: Precomputes $\cos(	heta_{\text{max}}) \approx 0.70710678$ at `proxy + 0x120`. `checkSupport` evaluates contact normal $\hat{n}$ against down-direction $\hat{d} = -\hat{u}$. Condition $\cos^2(	heta_{\text{max}}) \le 1 - (\hat{n} \cdot \hat{d})^2 = \sin^2(\alpha)$ identifies sliding slopes ($> 45^\circ$, state 2) vs walkable ground ($\le 45^\circ$, state 1); state 0 is midair/freefall.
- **Dynamic gravity & wall/ceiling walking**: The up-vector does **NOT** dynamically follow gravity during simulation. Calling `set_gravity` (API 21) updates `hknpWorld + 0x4c0` but leaves `body + 0x120` and `proxy + 0xd0` pointing up $(0, 1, 0)$, causing wall contacts ($\alpha = 90^\circ$) to evaluate as sliding/falling. Synchronizing `body + 0x120` and `proxy + 0xd0` with $-\text{normalize}(\vec{g}_{\text{new}})$ allows Faith's proxy to evaluate walls or ceilings as valid ground (`SUPPORT_SUPPORTED` = 1).
- **Subsystem failures (what breaks)**:
  - *Camera*: Hardcodes world Y-up in view matrix construction. On ceilings, controls invert; on walls, looking vertical creates gimbal lock.
  - *ANT Animation*: Two-bone foot IK sensor casts downward along world $(0, -1, 0)$, missing vertical walls.
  - *Fall state & kill volumes*: Movement controller checks vertical velocity $v_y = \vec{v} \cdot (0, 1, 0) < -5.0 \text{ m/s}$ to force falling states.

### 2.2 Camera View Matrix Decoupling & 6-DOF Freedom (Completed Frontier 1)
Follow-up deep-dive research lives in [camera-view-matrix-6dof.md](camera-view-matrix-6dof.md) (with annotations in `db/annotations/camera.yaml`). This is purely research from Ghidra decompilation for now and will be confirmed in the future when tested in game:
- **Root cause of view flipping identified**: Every frame, `ClientCameraViewManager_frameUpdate` (`0x14327ba00`) calls `0x14327e850`, resetting `manager->worldBasisMatrix` (`manager + 0x10f0`) to an exact identity matrix with up = $(0, 1, 0)$. In `FPSCamera_computeTransforms` (`0x14327fb80`), the raw head-bone transform $\mathbf{T}_{\text{head}}$ from provider `FPSCamera + 0x150` is multiplied by `manager + 0x10f0`. When Faith walks on walls or ceilings, her physical proxy up is perpendicular or opposite to world Y, but the view matrix projects against $(0, 1, 0)$, inducing pitch clamps ($[-89^\circ, +89^\circ]$) and violent $180^\circ$ roll snapping.
- **Full camera hierarchy & types reversed**: `CameraObject` (`0xb0` base), `FPSCamera` (`0x190`, vtable `0x141b18e58`, type 5), `FreeCamera` (`0x210`, vtable `0x141b18c78`, type 3, ancestor of `DemoRecorderCamera`).
- **6-DOF roll support**: Input concept IDs confirmed from `InputConceptId::toString` (`0x14468ca20`): `0x78` (Pitch), `0x79` (Yaw), and `0x7a` (`ConceptFreeCameraRotateZ` / Roll). GridNode's per-object vtable hook on slots `+0x68` (`camFrameUpdate`) and `+0x70` (`camOutput`) bypasses the Frostbite pitch clamp and allows continuous arbitrary roll angles $\phi \in [-180^\circ, 180^\circ]$.
- **Decoupling architectures**:
  1. *GridNode `camera_set` override*: Replaces active camera vtable slots `+0x68` and `+0x70` with arbitrary 6-DOF matrices (full roll, no pitch clamp).
  2. *Surface-aligned `worldBasisMatrix` injection*: Injecting surface normal $\hat{n}$ into `manager + 0x10f0` allows Faith to run on walls with first-person perspective tracking her feet.
  3. *Property dispatch `0x846f73f0` (`OverrideTransform`)*: Handled by `ClientCameraComponent` (`0x141b0a298`).

### 2.3 Areas Needing Even More Research
- **ANT Foot IK Surface Raycasts**: Reverse engineer the downward IK raycaster in ANT animation logic to project along proxy $-\hat{u}$ rather than global $(0, -1, 0)$.
- **Dynamic In-Game Surface Normal Tracking**: Test automatic real-time lerping of `manager + 0x10f0` when transitioning between vertical wall-runs and flat ground.
- **Frustum Near/Far Culling at Inverted Pitch**: Verify whether Frostbite's GBuffer frustum culling (`0x14352d4c0`) handles inverted up-vectors without near-plane geometry clipping.

### 2.3 In-Game Verification Test Plan
1. Call `set_gravity(0, 0, -9.81)` while simultaneously writing $(0, 0, 1)$ to `CharacterPhysicsBody + 0x120` and `hknpCharacterProxy + 0xd0`.
2. Approach a south-facing vertical wall and verify Faith enters grounded walking state (`SUPPORT_SUPPORTED` = 1) rather than sliding off.

---

## 3. Characters: spawn, outfits, poses, voices

Follow-up research lives in [characters-spawning-outfits-poses-voices.md](characters-spawning-outfits-poses-voices.md) (with annotations in `db/annotations/character.yaml`, `db/annotations/spawn.yaml`, and `db/annotations/audio.yaml`). This is purely research from Ghidra decompilation and MECMP reversing for now and will be confirmed in the future when tested in game:

### 3.1 Key Reverse-Engineering Breakthroughs
- **Pillar 1 (Spawning & Safe Teardown)**: Spawning runs on server thread via `ServerCharacterSpawnEntity::spawn` (`0x143390830`), passing `SpawnInfo` (`0x70` bytes). Replicated client side as `PamClientCharacterEntity` with event `CharacterSpawned` (`0xa29c98e6`). Safe teardown via spawner vtable slot `+0x108` (`0x14338e4c0`), calling `ServerCharacterSpawnEntity_removeSpawned` (`0x14338e5d0`) and `Entity_requestDestroy(character, character->bus)` (`0x143167700`) to eliminate ghost leaks.
- **Pillar 2 (Outfits & Customization)**: `PamServerCharacterEntity::applyCustomization` (`0x14336cf00`) calls `ServerCharacterEntity_applyCustomizeData` (`0x14336c860`), querying `fb::TI_ServerCharacterCustomizationComponent` (`0x1431c6eb0`) to bind `CustomizeCharacterData` visual unlocks. Ghost shader toggled via `PamClientCharacterEntity + 0x334` (`bool ghostShader`).
- **Pillar 3 (Bone Pose Injection & The Root Drift Bug)**: Puppets evaluated in `PamClientReplayPlayerComponent_update` (`0x1439501a0`). Setting `skipRoot` (`+0x2e6`) to 1 bypasses automatic trajectory reconstruction from Bone 0/1, eliminating the severe altitude drift bug during replay playback.
- **Pillar 4 (Vocals & 3D Spatial Audio)**: `VoiceOverSystem` (`*0x1423e3150`) submits speech lines via `VoiceOverSystem_submitConversation` (`0x1430477f0`) and dynamically binds emitters to character transforms via `VoiceOverSystem_setSources` (`0x1430467f0`). One-shot barks use `SoundSystem_createVoice` (`0x142fe2ee0`) and `Voice_setTransform` (`0x14301dce0`).

### 3.2 Areas Needing Even More Research
- **Isolated spawner creation**: Develop a mechanism to instantiate a standalone `PamServerCharacterSpawnEntity` using `ServerEntityCreationInfo_ctor` (`0x14335cab0`) with synthetic `CharacterSpawnReferenceObjectData`.
- **Visual unlock cataloging**: Dump and index all available `CustomizeCharacterData` assets (Runner outfits, KSEC Protector/Enforcer/Sentinel armor sets).
- **Direct ANT animatable pose injection**: Trace whether bone matrices can be pushed directly to `ClientAntAnimatableComponent` to pose characters that lack `PamClientReplayPlayerComponent`.

### 3.3 In-Game Verification Test Plan
1. Spawn a puppet character at Faith's position using `ServerCharacterSpawnEntity::spawn`.
2. Push keyframe rotations to `PamClientReplayPlayerComponent` with `skipRoot = 1` and verify the model animates without vertical elevation drift.
3. Despawn the puppet via `ServerCharacterSpawnEntity_unspawn` and confirm the entity is destroyed cleanly with zero memory leaks.

---

## 4. Sound parameters and velocity

Follow-up research lives in [sound-parameters-velocity.md](sound-parameters-velocity.md) (with annotations in `db/annotations/audio.yaml`). This is purely research from Ghidra disassembly for now and will be confirmed in the future when tested in game:

### 4.1 Key Reverse-Engineering Breakthroughs
- **Voice parameter setter verified**: Vtable slot `+0x18` (`0x14301da40`) is `Voice::setParameter`. In x64 ABI, it receives parameter key in `EDX` and float value in `XMM2`, passing them directly to graph setter `0x1430060b0` before marking the voice dirty via `0x143018bb0`. Proven by caller `SoundEntity::updateProperties` (`0x14303d214`--`0x14303d231`).
- **Velocity methods**: Slot `+0x78` (`0x14301dd30`) is `Voice::clearVelocity`, while slot `+0x80` (`0x143021f60`) is `Voice::updateVelocity` calling `State3D_update` (`0x143021cc8`).
- **Direct linear velocity override**: `*(float[4]*)(*(uintptr_t*)(voice + 0x28) + 0x40)` holds the 3D linear velocity vector $\vec{v} \in \mathbb{R}^3$ (in $\text{m/s}$) in `State3D` for accurate Doppler shifts.

### 4.2 Areas Needing Even More Research
- **Emitter cone directivity**: Reverse-engineer `SoundEntityData` angular attenuation cones (inner angle, outer angle, outer gain attenuation).
- **Music & ambience state transitions**: Map `SoundStateEntity` and `MusicEventMessage` (`0x24ffcb4b`) to dynamically trigger combat music, ambient stingers, and district theme changes.
- **Sound bus ducking & master volume**: Locate master audio bus mixing controls to implement mod ducking during custom vocal playback.

### 4.3 In-Game Verification Test Plan
1. Create a voice from a sound asset at Faith's position via `SoundSystem_createVoice` (`0x142fe2ee0`).
2. Write high velocity $(50.0, 0.0, 0.0)$ to `*(voice + 0x28) + 0x40` and verify realistic Doppler pitch shifting as Faith moves past the emitter.

---

## 5. UI and pause

Follow-up research lives in [ui-pause-system.md](ui-pause-system.md) (with annotations in `db/annotations/ui.yaml`). This is purely research from Ghidra disassembly for now and will be confirmed in the future when tested in game:

### 5.1 Key Reverse-Engineering Breakthroughs
- **Pause counters & driving entity**: `UIPauseController_request` (`0x143b16210`) has only 3 callers in the binary, all in `PamClientUIPauseEntity` (`0x141cbd4a8`). It responds to events `0x0dd8ddf7` (Pause ON) and `0xc44b5c3c` (Pause OFF), reading boolean `PostponePauseUntilScreenIsFadedIn` at `data + 0x18`. If postpone is true (1), it only increments `pauseRequestsA` (soft pause); if postpone is false (0), it increments both `pauseRequestsA` and `pauseRequestsB` (hard pause).
- **The exact pause condition**: `UIPauseController_applyToClient` (`0x143b16ae0`) and `UIPauseController_isPaused` (`0x143b12830`) read the player subview's simulation timescale factor $s$ (`PlayerView_getTimescale` at `0x1432a8710`):
  $$\text{isPaused} = (A \ge 1) \land (s < 10^{-6} \lor B \ge 1)$$
  When the Pause Menu opens, $A=1, B=0$. Once the screen finishes fading in, the animation graph sets $s = 0.0 < 10^{-6}$, pausing the simulation.
- **Mod pause/resume API**: A mod can pause the game immediately by calling `UIPauseController_request(ctrl, 1, 1)` ($B=1$ guarantees instant pause regardless of timescale), and resume via `request(ctrl, 0, 1)`.

### 5.2 Areas Needing Even More Research
- **Direct timescale manipulation (bullet time / slow-mo)**: `PlayerView_getTimescale` (`0x1432a8710`) reads `subview + 0x3c`, `+0x64`, and `+0x8c`. Reverse-engineer which Frostbite components write to these offsets to build a dedicated `set_timescale(float)` API.
- **Input routing during mod pause**: Determine whether `PamUISystem` captures mouse/keyboard input when paused by a mod without an active native menu screen.
- **Screen counter matrix in game**: Test in-game counter values across all screens (Map, Progression, Runner Tag, Death) to verify whether any base game screen sets $B \ge 1$.

### 5.3 In-Game Verification Test Plan
1. Call `UIPauseController_request(ctrl, 1, 1)` from the trainer while Faith is mid-air.
2. Confirm the game freezes instantly with zero fade-in delay, and resumes cleanly upon calling `request(ctrl, 0, 1)`.

---

## 6. Streaming: Zone Streamer Focus Points

Follow-up research lives in [freecam-streaming-and-focus-points.md](freecam-streaming-and-focus-points.md) (with annotations in `db/annotations/levels.yaml`). This is purely research from Ghidra decompilation and GridNode layout engine reversing for now and will be confirmed in the future when tested in game:

### 6.1 Key Reverse-Engineering Breakthroughs
- **Free camera streaming verdict**: The game does **NOT** stream the world around the free camera. `ZoneStreamerFocus_add` (`0x143c7e7c0`) has only two callers in the entire executable, both in `ZoneStreamerFocusEntity` parented to Faith. The camera subsystem has zero focus integration. Flying far from Faith leaves distant districts unstreamed.
- **API 30 is essential**: `streamingFocusAdd`, `streamingFocusMove`, and `streamingFocusRemove` are required for freecam flight, cinematic sweeps, and remote layout placement. Registering a synthetic `ZoneFocus` record at `streamer + 0x280` enables dynamic streaming around camera coordinates.
- **Focus manager architecture**: `ZoneStreamerEntity` (vtable `0x141cfe4c0`, size `0x710`), focus manager at `streamer + 0x280` (`Records` at `+0x08`, `Positions` at `+0x2a8`, `FocusCount` at `+0x2c8`). Zone load `ZoneStreamer_loadZone` (`0x143c810d0`), unload `ZoneStreamer_unloadZone` (`0x143c81240`). Avoids vanilla out-of-bounds write crash in `ZoneStreamerFocus_remove` (`0x143c81390`) by directly flagging `rec + 0x14 |= 1`.

### 6.2 Areas Needing Even More Research
- **Sublevel pre-streaming integration**: Trace `SubLevelManager_drainPendingSubWorlds` (`0x1432eaba0`) or dispatch `Event_StreamIn` (`0xd27bc17e`) to `ServerSubLevelEntity` when free camera approaches an unloaded district boundary.
- **Zone search tolerance tuning**: Reverse-engineer `0x143c7fb40` to evaluate whether dynamically expanding the zone search radius parameter allows earlier pre-loading during high-speed camera movement.

### 6.3 In-Game Verification Test Plan
1. Fly free camera 1 km away from Faith into an unstreamed district.
2. Register a focus point at camera position using `streamingFocusAdd`.
3. Confirm geometry, LODs, and textures stream in around the camera, and unload cleanly when calling `streamingFocusRemove`.

---

## 7. Map markers and presence

Follow-up research lives in [map-markers-and-presence.md](map-markers-and-presence.md) (with annotations in `db/annotations/ui.yaml` and `db/annotations/online.yaml`). This is purely research from Ghidra decompilation and MECMP reversing for now and will be confirmed in the future when tested in game:

### 7.1 Key Reverse-Engineering Breakthroughs
- **Presence architecture**: Managed by `PresenceInfoManagerPool` (vtable `0x141c74bb8`) updating `PamClientPresenceInfoManagerEntity` (vtable `0x141c74be8`, factory `0x143a35310`).
- **Slot allocation & eviction (`0x143a405e0`)**: Array at `manager + 0x50` / `+0x58`. `0x50`-byte slot structure: `shown` (`+0x00`), `priority` (`+0x04`), `OnlineId` (`+0x08`, 0x28 bytes), `worldPos` (`+0x30`, Vec4, 16-byte aligned), `blueprintInstance` (`+0x40`), `entityHandle` (`+0x48`). Mods pass priority `1000` to prevent eviction by native friend queries (priority `0`).
- **Prefab spawning (`PF_PresenceInfo`)**: Lazily spawned via `ClientEntity_spawnBlueprint` on first use of a slot, binding `PamClientPresenceInfoEntity` (vtable `0x141c74ac0`, ctor `0x143a38c10`) with property connectors driving `PamUIMapMarkerEntity` on map and radar.
- **Hook point**: `PresenceInfoManagerPool_update` at slot `0x141c74bc8` (`0x143a4bb30`), executed per-frame on client main thread. Intercepting before update enables injecting positions and asserting `shown = 1`.

### 7.2 Areas Needing Even More Research
- **Offline custom tag injection**: Investigate bypassing online gateway authentication to render custom user icons or textures in the Scaleform UI marker without EA Nucleus server connectivity.
- **Marker 3D pin customization**: Decompile `PamUIMapMarkerEntity` to determine if 3D in-world pin geometry, beacon light rays, or minimap radar blips can be dynamically recolored or swapped per team/role.
- **Tooltip rich text formatting**: Test extending property connector `+0x30` (`TI_LocalizedString`) to display dynamic player statistics (speed, ping, district location) within map tooltips.

### 7.3 In-Game Verification Test Plan
1. Hook `PresenceInfoManagerPool_update` and assign a slot with priority 1000 at a known landmark coordinate.
2. Open in-game world map (M / TAB) and verify a customized marker appears at the landmark with the correct tooltip title.

---

## 8. Dash, Time Trial Events & Ghost / Echo Replay Pipeline

Follow-up research lives in [dash-and-time-trial-events.md](dash-and-time-trial-events.md) and [ghost-echo-replay-system.md](ghost-echo-replay-system.md) (with annotations in `db/annotations/messages.yaml`, `db/annotations/replay.yaml`, and `db/annotations/ui.yaml`). This is purely research from Ghidra decompilation and MECMP reversing for now and will be confirmed in the future when tested in game:

### 8.1 Key Reverse-Engineering Breakthroughs
- **Social play messaging infrastructure**: Category `0x54776fc6` (`kCategorySocialPlay`). Base class `PamSocialPlayMessage` (ctor `0x143474ae0`, vtable `0x141b94668`). Catalog of 35 distinct social play messages mapped with constructors and vtables.
- **Named challenge finish payload (`0xd71d027f`)**: Handled by `PamSocialPlayNamedChallengeFinishedMessage` (ctor `0x144733a30`, vtable `0x141c38eb0`). Offsets: `+0x48 ContentId`, `+0x4c Index`, `+0x50 Delta`, `+0x54 HadRecord`, `+0x58 Value`, `+0x60 NewRecord`. Exact formula:
  $$\text{runCs} = (\text{newRecord} \lor \neg\text{hadRecord}) \mathbin{?} \text{value} : \text{value} + \text{delta}$$
- **UGC Time Trials vs ReachThis markers**: Player time trials fire `PamSocialPlayChallengeFinishedMessage` (`0x9b35497c`, ctor `0x144733550`, vtable `0x141c38cd0`, `+0x48 TimeTrialGuid`, `+0x60 Time`). Timed runs are distinguished from Beat L.E. markers by `PamChallengeMarkerEntityData + 0x68 CheckpointOrder != 0`.
- **Entity metadata & route title**: `PamServerNamedChallengeEntity` (vtable `0x141c7ec70`) holds `PamNamedChallengeData*` at `+0x80`. Route title resolved via `localize` (`0x1436a8d40`) passing `Sid`. UGC user titles resolved via `PamServerChallengeMarkerEntity` (`+0xb8 Name`).
- **Ghost / Replay 30 Hz dual ring-buffer recording**: Driven by `PamClientReplayRecordingComponent` (`0x141c46d78`, updater `0x14394dac0`) sampling raw ANT bone transforms (`*(updater + 0x28)`) at 30 Hz into `ReplayRecorder` (`0xf0` bytes, 450 frames / ~15s capacity).
- **Bitstream compression (Format Version 5)**: `ReplaySetup_WriteHeader` (`0x1439516b0`) embeds DJB2 checksum keyed by bone IDs, animated track flags, and quantization bit lengths (10 and 11). `ReplayKeyframe_Write` (`0x143951450`) emits I-frames every 30 frames or when displacement $> 5.0\text{ m}$, and P-frames with 10-bit quantized delta position (clamped $\pm 5.0\text{ m}$) and 11-bit quantized quaternion. Also emits downsampled 10 Hz 3D coordinate trajectory for map HUD trails.
- **Playback & root transform placement**: `PamClientReplayPlayerComponent` (vtable `0x141c48a28`, update `0x1439501a0`) decodes frames via `ReplayPlayback_DecodeKeyframe` (`0x14394e390`), ticks interpolation (LERP position + SLERP quaternion) in `ReplayPlayback_Tick` (`0x1439729f0`), and places puppet in world via `FUN_142fd8f20` using Bone 0 (root) and Bone 1 (`AITrajectory`).
- **Holographic shader & echo customization**: Rendered via forward shader `Shaders/SpecialUseCase/GhostProjectionVSFAsset` (`0x141c48540`, ctor `0x143944740`), `PamUseRunnersGhostShader` (`0x141c80098`), and `PamGhostInRvColorBurnPass` (`0x141c800c8`). Visual skins driven by `PamEchoCustomizationData` (`0x141c91648`, size `0x28`) and `PamClientEchoCustomizationManagerEntity` (`0x141c73c18`).
- **Pamplona REST & gateway backend endpoints**: Uploads use `PamplonaAuthenticated.setRunnersRouteReplay` (`0x143a0c890`) and `PamSocialPlayNamedChallengeUploadReplayMessage` (`0x955f5cbd`). Player ghost metadata queries use `Pamplona.getPlayerGhosts` (`0x1439f98d0`) parsed by `FUN_1439b6950` into `PamEchoGhostRecord` (`0x18` bytes: personaId, timestampValue, variation).

### 8.2 Areas Needing Even More Research
- **Intermediate checkpoint split timing events**: Investigate whether checkpoint triggers fire `PamSocialPlayFireEvent` (`0x36154959`) or sub-bus entity events to track live split deltas during an active run.
- **Dynamic UGC checkpoint injection**: Evaluate programmatic invocation of `PamServerChallengeMarkerEntity_create` to spawn custom user time trials without backend server round-trips.

### 8.3 In-Game Verification Test Plan
1. Start and complete an official Dash (e.g. "The View").
2. Verify that GridNode's `onDash` callback fires with exact centisecond time, new record boolean, route name, and content ID matching the HUD summary.

---

## 9. The visual environment and timelines

Follow-up research lives in [visual-environment.md](visual-environment.md) (with annotations in `db/annotations/render.yaml`, `db/annotations/entity.yaml`, and `db/annotations/faith.yaml`). This is purely research from Ghidra decompilation and runtime reversing for now and will be confirmed in the future when tested in game:

### 9.1 Key Reverse-Engineering Breakthroughs
- **Visual environment architecture & state layout**: Destination buffer is `worldRenderer + 0x85470` (`0xb30` / 2,864 bytes). Complete catalog of 8 functional parameter blocks documented with exact byte offsets: Outdoor Light (`+0x000`), Tonemap (`+0x1d0`), Color Grading (`+0x330`), Sky & Clouds (`+0x390`), Global Wind (`+0x6e0`), Depth of Field (`+0xa50`), Vignette (`+0xac0`), and Film Grain (`+0xb00`).
- **Eliminating write-back flicker (The Render Hook)**: Post-update simulation writes flicker because `VisualEnvironmentManager_update` (`0x143705b30`) and `VisualEnvironmentContext_blend` (`0x143703d60`) recompute layer curves each frame. Hooking **Slot 21** (`0x141bfaca8`, original `0x14373e5d0`) of `WorldRenderer` vtable (`0x141bfac00`) executes synchronously on the Render Thread immediately prior to constant buffer upload (`0x1436fab20` / `0x1436f80c0`), guaranteeing 100% flicker-free overrides.
- **TimelineEntity architecture (`0x141beab28`)**: Reflected class (size `0x490`, factory `0x14368c060`). Controlled via `Timeline_SetTime` (`0x143691120`). Passing `force = true` overrides the external time connection guard at `timeline + 0x60`, forcing immediate keyframe evaluation and event dispatch.

### 9.2 Areas Needing Even More Research
- **Custom 3D LUT texture injection**: Investigate direct memory patching into the color grading LUT unit (`worldRenderer + 0xde10` / `FUN_143797400`) to supply synthetic 3D color lookup tables without `.ebx` mod bundles.
- **Camera View Manager autofocus coupling**: Reverse engineer `ClientCameraViewManager::updateView` to test coupling depth of field focal distance directly to real-time player camera raycasts.
- **Dynamic timeline track synthesis**: Determine runtime stability of dynamically instantiating and inserting `DiceTransformTrackData` tracks into live `TimelineEntity` track groups.

### 9.3 In-Game Verification Test Plan
1. Install Render Hook on Slot 21 of `WorldRenderer` (`0x141bfaca8`).
2. Write custom `sunColor` (neon blue) and `filmGrainEnable = 1` into `worldRenderer + 0x85470`.
3. Confirm scene lighting immediately recolors with zero frame-to-frame flicker or fight-back from native environment volumes.

---

## 10. Other builds of the game

> [!IMPORTANT]
> **Status**: *Deprioritized / Out-of-Scope*. Per project guidance, the retail EA App build (`0x5762E3B7`) is the sole build supported by GridNode. Signature-based fingerprinting for alternate or updated builds is deferred.

---

## 11. Custom maps, meshes, and asset spawning pipeline

Follow-up research lives in [custom-maps-assets-pipeline.md](custom-maps-assets-pipeline.md) and [custom-mesh-loading-dx11.md](custom-mesh-loading-dx11.md) (with annotations in `db/annotations/resources.yaml`, `render.yaml`, `spawn.yaml`, and `levels.yaml`). This is purely research from Ghidra decompilation and type reflection for now and will be confirmed in the future when tested in game:

### 11.1 Key Reverse-Engineering Breakthroughs
- **Asset discovery, partition lookup & dynamic partition injection**: `ResourceGroups` table at `0x14237deb8` (max `0xbbd` / 3,005 entries); `getCompartment(id)` (`0x142abad00`) reads `ResourceGroups[id] + 0x68`. `Partition_findByName` (`0x142aeb310`) and `PartitionMap_find` (`0x142aeb940`) calculate case-insensitive lowercase DJB2-XOR hash ($H_{i+1} = (H_i \times 33) \oplus c$, initial $5381$) and search bucket hash map at `compartment + 0x158`. Partition layout: `+0x08` Guid (16 bytes), `+0x18` name, `+0x20` instances array (`DataContainer**`), `+0x28` instance count. In-memory partition injection: custom synthetic partitions (`0x30` bytes) can be directly linked into `compartment + 0x158` bucket chains under mutex lock (`FUN_142a9f970` / `FUN_142aa1840`) without requiring offline `.ebx` or `.sb` bundle packaging.
- **Hardware-instanced multi-mesh groups (`StaticModelGroup`)**: `StaticModelGroupEntityData` (`0xb0` bytes, vtable `0x141b73f40`) holds `MemberDatas` array at `+0x90`. Each `StaticModelGroupMemberData` (`0x68` bytes) holds `InstanceTransforms` array at `+0x08` (`LinearTransform`), `MeshAsset*` at `+0x38`, `InstanceCount` at `+0x40`. `ClientStaticModelGroupEntity` (vtable `0x141afbe90`, size `0x108`) and `ServerStaticModelGroupEntity` (vtable `0x141b5e3a0`, size `0xc0`). `ClientStaticModelGroup_placeMember` (`0x14318fbc0`) multiplies member transforms by group world transform and submits updates to `RenderWorld` (`GameWorlds[0] + 0x48`). `StaticModelGroup_placementHash` (`0x143b722e0`) quantizes position, rotation, and scale into a 16-byte placement hash at `+0x78` for GPU hardware instancing batching.
- **Material parameter & texture overrides without shader recompilation**: `RigidMeshAsset` (`0x78` bytes, vtable `0x141bbdab0`) holds `Materials` array at `+0x60` (`MeshMaterial*`). `MeshMaterial` (`0x70` bytes, vtable `0x141bbdae0`) holds `ShaderInstance` at `+0x10` (`SurfaceShaderInstanceDataStruct*`, size `0x28` bytes). `VectorParameters` at `+0x10` (stride `0x20`), `TextureParameters` at `+0x20` (stride `0x10`). Cloned materials can override colors, emissive accents, and texture maps dynamically.
- **Synthetic DirectX 11 Ingestion & GPU Buffer Creation**: `MeshLod_createGpuBuffers` (`0x14354fe00`) creates D3D11 vertex and index buffers via render device slot 21 (`0xa8`) directly from raw CPU memory arrays and asserts ready flag `0x20000000` in `lod->statusFlags` (`+0x50`). Index buffer offset (`lod->indexOffset` at `+0x5c`) defines the exact vertex buffer byte size, enabling contiguous single-buffer memory layouts `[Vertices || Indices]`.
- **Dynamic InputLayout & VertexDeclaration Caching**: `GraphicsPlatform_createOrGetVertexDeclaration` (`0x1435b8f30`) dynamically resolves, generates, and caches `ID3D11InputLayout` wrappers in an `eastl::map` at `*g_platformInfo + 0xb0` from an 84-byte `GeometryDeclarationDesc` (16 element descriptors: `{usage, format, offset, stream}`). Works for any arbitrary vertex format at runtime without requiring pre-compiled shader permutations.
- **Lightmap & Enlighten Radiosity Bypass**: Setting `RigidMeshAsset.EnlightenType = 0` (`EnlightenType_Dynamic`) or `1` (`EnlightenType_LightProbe`) and `StreamingEnable = false` (`0`) enables full real-time lighting (diffuse, specular, normal maps, shadows) sampled dynamically from ambient cubes and spherical harmonics probes, completely eliminating the need for offline UV lightmap charts or radiosity baking.
- **Sublevel streaming anchoring & Level Root Bus**: `EntityBus_getLevel` (`0x14314c5e0`) retrieves `*(Level**)(*(uintptr_t*)bus + 0x38)`. Client Level root bus can be retrieved from `*(ClientGameWorld + 0x20)`. Spawning into child buses created under the level root bus (via `ClientEntity_spawnIntoBus` `0x1431c44b0` / `fb_EntityBus_create` `0x143146e90`) makes custom map geometry immune to sublevel stream-out unloads.
- **Four custom collision strategies**: Prop borrowing (Phase 1), standalone Havok boxes / hulls (API 34, `PhysicsEntityFactory_createBody` `0x143516450` via `DummyBarrierEntity`), static compound part injection (`hknpShapeInstance` in `GroupRigidBody`), and precompiled Havok assets in `.fbmod`.
- **Layout persistence & editor enhancements**: Stable keying by `(partitionGuid, dataBusPeerFlags)` or `IndexInBlueprint` rather than spatial coordinates; non-destructive removal by parking at $y = -15,000$ (`kParkDepth`).

### 11.2 Areas Needing Even More Research
- **Havok static compound shape runtime triangle insertion**: Determining whether inserting dynamic triangle collections into `GroupRigidBody` (`0x1434eda80`) at runtime causes broadphase tree degradation or requires periodic rebalancing during gameplay.
- **Custom texture resource streaming bypass**: Direct creation of `ID3D11ShaderResourceView` texture objects for user-imported PNG/DDS textures via `GraphicsPlatform` texture factories (`0x143532a70` / `0x1435f7340`) without requiring offline `.res` texture compilation.

### 11.3 In-Game Verification Test Plan
1. Dynamically inject a synthetic partition containing a custom `RigidMeshAsset` and `MeshSetResource` into `compartment + 0x158`.
2. Call `MeshLod_createGpuBuffers` (`0x14354fe00`) with an in-memory 24-byte-stride vertex buffer (cube/ramp) and 16-bit index buffer.
3. Spawn a `ClientStaticModelGroupEntity` with 10 instances arranged in a grid anchored to the Level Root Bus.
4. Confirm all 10 instances render with GPU hardware instancing, receive dynamic lighting, and remain resident when crossing district streaming boundaries.

---

## 12. Havok world facts to confirm

Follow-up research lives in [havok-world-internals.md](havok-world-internals.md) (with annotations in `db/annotations/physics.yaml` and reference updates in [havok-reference.md](havok-reference.md)). This is purely research from Ghidra decompilation for now and will be confirmed in the future when tested in game:

### 12.1 Key Reverse-Engineering Breakthroughs
- **`hknpWorld` allocation (`0x1434d0ff0`), size & capacity fields**: Allocated via `hkMemoryRouter::alignedAlloc(0x5a0, 0x10)` at `0x1434d0ff0` (size `0x5a0` = 1440 bytes). Primary vtable `0x141d51418` (`hknpWorld::vftable`), constructor at `0x143f3e820`. Stride of `hknpBody` in `world + 0x20` is `0x90` (144 bytes). Stride of `hknpMotion` in `world + 0xe0` is `0x80` (128 bytes). `world + 0x28`: `m_bodyCapacity` (total allocated slots, e.g. 16,384 + 1). `world + 0x2c`: `m_bodyCapacityMask` (`0x3fffffff`). `world + 0x68`: `m_numBodiesUsed` (live active body count, which GridNode's `freePlaces` reads). `world + 0x4c0`: `m_gravity` (`hkVector4`).
- **Static compound shape & 6-byte AABB tree**: `hknpStaticCompoundShape`: instance array at `+0x60` (`hknpShapeInstance`, stride `0x80` = 128 bytes); compressed AABB tree at `+0xc0`. Node formula confirmed in `0x143e99090`: `mulps xmm6, [0x1425a8e10]` where `0x1425a8e10` holds four floats equal to $0.0044247787 \equiv \frac{1}{226.0}$. Step per axis: $\text{step} = (\text{parent.max} - \text{parent.min}) / 226$. Child bounds: $\text{min} = \text{parent.min} + (b \gg 4)^2 \times \text{step}$, $\text{max} = \text{parent.max} - (b \ \& \ 15)^2 \times \text{step}$. Node layout: 6 bytes per node. Byte 3 bit 7 = inner node flag (right child offset $= (((b_3 \ \& \ \text{0x7f}) \ll 16) \ | \ u_{16}) \times 2$); leaf node bytes 4..5 are `int16_t` instance index into `m_instances`.
- **`GroupRigidBody` (`0x1434eda80`)**: `GroupRigidBody_getInstanceTransform` takes `(GroupRigidBody* self, LinearTransform* out, uint32_t partIndex)`. It reads part instance index from `self + 0x110 + partIndex * 4`, retrieves `hknpStaticCompoundShape*` from `self + 0x58`, and delegates to `hknpShapeInstance_buildTransform` (`0x143f50740`), which multiplies rotation columns by scale `+0x40`.
- **`hknpBody` flags at `+0x58`**: Bit 0 (`0x0001`): `IS_STATIC` (motion ID 0). Bit 1 (`0x0002`): `IS_DYNAMIC` (simulated dynamic body). Bit 2 (`0x0004`): `IS_KEYFRAMED` (kinematic body moved by game code). Bit 3 (`0x0008`): `IS_ACTIVE` (awake in active simulation island; cleared when asleep). Bit 10 (`0x0400`): `TRANSFORM_DIRTY` / `AABB_DIRTY` (triggers broadphase AABB recomputation). Immovable body test: Frostbite checks `test byte ptr [body + 0x58], 5` (testing Static `0x1` | Keyframed `0x4`).

### 12.2 Areas Needing Even More Research
- **Dynamic broadphase AABB rebuilding & rebalancing**: Trace how broadphase dirty flags (`0x400` in `body + 0x58`) are batched during `hknpWorld::stepDeltaTime` (`0x143f474f0`) to determine if rapid kinematic updates cause broadphase tree degradation.
- **Runtime body slot exhaustion handling**: Confirm whether allocating bodies beyond capacity `0x28` triggers silent failure or memory corruption given the empty stub buffer-full handlers connected in `PhysicsWorld_create`.
- **Part-to-instance mesh alignment testing**: In-game validation of `GroupRigidBody_getInstanceTransform` against complex multi-part building meshes to ensure zero drift between Havok compound collision parts and Frostbite visual meshes.

### 12.3 In-Game Verification Test Plan
1. Read `world + 0x68` (`m_numBodiesUsed`) and compare against total bodies allocated.
2. Interrogate a multi-part building `GroupRigidBody`, query `GroupRigidBody_getInstanceTransform` for part 0, and verify it perfectly coincides with visual mesh instance transform.

---

## 13. Missions, objectives & custom checkpoint scripting

Follow-up research lives in [missions-objectives-checkpoints.md](missions-objectives-checkpoints.md) (with annotations in `db/annotations/levels.yaml`, `events.yaml`, and `messages.yaml`). This is purely research from Ghidra decompilation for now and will be confirmed in the future when tested in game:

### 13.1 Key Reverse-Engineering Breakthroughs
- **Mission Lifecycle Architecture**: `PamServerMissionManagerEntity` (vtable `0x141c7add0`) manages mission transitions via event dispatcher `vf4` (`0x143a74bf0`): `StartMission` (`0x68fb16fc`), `RestartCurrentMission` (`0xf258e0db`), `GetLastCheckpointTransform` (`0xc5b7322f`), `AbandonMission` (`0x41ee0da7`), and `CompleteMission` (`0x37bb9bfc`).
- **Client Replication & Presence Hook**: `PamClientMissionEntity` (vtable `0x141c6b180`, size `0x78`) deserializes state bits (`+0x58` active, `+0x59` paused, `+0x5a` in district, `+0x5b` completed) in `0x143a17370`, broadcasts `PamProgressionMissionUpdatedMessage` (`0x3c8db41e`, category `0xae19b9ac`), and drives the rich presence string `"OnAMissionInDistrict"`.
- **Objectives Subsystem & Verified Events**: `ServerObjectiveEntity` (vtable `0x141b4eaa8`, size `0x100`) event handler `vf4` (`0x143383810`) handles verified event hashes: `ShowObjective` (`0xd0488c09`), `HideObjective` (`0x23ccf4aa`), `Success` (`0x7f3e7106`), and completion broadcast `OnSuccess` (`0xbb06a967`). `ClientObjectiveEntity` (vtable `0x141b2a308`, size `0xf8`) controls HUD waypoint visibility via flag bit 7 (`0x80`) at `+0x18`.
- **Checkpoint Placement & Respawn Pipeline**: `PamServerCheckpointEntity` (vtable `0x141c7e750`, size `0x150`) binds spawn transform (`0x87525469` at `+0x80`), sequence index (`0x0cc9279b` at `+0x70`), and trigger orientation (`0xae496a36` at `+0xd0`). Faith's active reload position lives at `*DAT_14257c9d8 + 0xe0`. Writing directly to `*DAT_14257c9d8 + 0xe0` allows mods to set arbitrary custom checkpoints anywhere in the world. Level reloads are triggered via `ServerLevel_reloadLastCheckpoint` (`0x143341d40`), executing `ServerRoundResetMessage` (`0x4bb8de5c`) and `ServerLevelRestartMessage` (`0xf8429a0e`).

### 13.2 Areas Needing Even More Research
- **Dynamic Blueprint Sequence Stitching**: Injecting custom multi-stage objective graphs into level blueprint entity buses without editing retail Frostbite EBX files.
- **Offline Save Game Progression Injection**: Reverse engineering the binary schema of `UserProgress.sav` to persist custom mission unlocks across game restarts.
- **Custom Vocal Dialogue Synchronization**: Triggering custom spatial audio voice barks when intermediate objective milestones are reached.

### 13.3 In-Game Verification Test Plan
1. Move Faith to an arbitrary non-standard vantage point (e.g. high crane) and write her transform to `*DAT_14257c9d8 + 0xe0`.
2. Fall out of bounds or trigger death. Confirm Faith respawns directly on the crane.
3. Call `ServerLevel_reloadLastCheckpoint` (`0x143341d40`). Confirm level reloads cleanly without hanging.
4. Dispatch `ShowObjective` (`0xd0488c09`) followed by `Success` (`0x7f3e7106`). Verify HUD objective marker display and success completion banner.
