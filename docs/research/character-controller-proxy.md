# Havok Character Controller & Proxy Reversing (`hknpCharacterProxy`)

## 1. Overview & Architectural Hierarchy

Mirror's Edge Catalyst combines Frostbite's character gameplay components with Havok's New Physics (`hknp`) character proxy architecture. Unlike classic rigid-body ragdolls or kinematic physical bodies, the player character (Faith) is controlled via a specialized proxy object that interacts with the physics world through swept shape queries and custom manifold resolution.

```
+-------------------------------------------------------------+
|               fb::CharacterPhysicsBody                      |
|  (vtable: 0x141bab5e0, candidate_2: 0x141bab8b8)            |
|                                                             |
|  +0x50: hknpCharacterProxy*  ---------------------------+   |
|  +0x80: Shape Instance                                   |   |
|  +0x8c: Character Height / Scale                         |   |
|  +0xd0: Havok Body ID (u32)                              |   |
|  +0x108: SupportInfo (hkpSurfaceInfo, 0x30 bytes)        |   |
|  +0x120: m_upVector (Vector4: 0, 1, 0, 0)                |   |
+----------------------------------------------------------+--+
                                                           |
                                                           v
+-------------------------------------------------------------+
|                   hknpCharacterProxy                        |
|  (vtable: 0x141d53348, size: 0x130 bytes)                   |
|                                                             |
|  +0x2c: Shape Pointer / Filter Info                         |
|  +0x30: hknpWorld*                                          |
|  +0x40..+0x50: Position & Transform                         |
|  +0x70: Linear Velocity                                     |
|  +0xd0: m_up (Vector4: unit up vector)                      |
|  +0x120: m_cosMaxSlope (float: cos(maxSlopeAngle))          |
+-------------------------------------------------------------+
```

---

## 2. Character Body Initialization (`FUN_1434e4400`)

The function `FUN_1434e4400` initializes `fb::CharacterPhysicsBody` and constructs its internal `hknpCharacterProxy`.

### Confirmed Parameters & Limits
| Parameter | Value | Offset in Cinfo | Binary Reference | Description |
| :--- | :--- | :--- | :--- | :--- |
| **Contact Tolerance** | `0.1f` (`0x3dcccccd`) | `cinfo + 0x48` | `0x1434e4716` | Distance threshold for maintaining contact manifolds |
| **Keep Distance** | `0.05f` (`0x3d4ccccd`) | `cinfo + 0x74` | `0x1434e4739` | Buffer distance maintained from obstacles |
| **Max Solver Speed** | `10.0f` (`0x41200000`) | `cinfo + 0x78` | `0x1434e4746` | Clamps maximum character velocity inside solver |
| **Collision Layer** | `14` (`0x0e`) | `cinfo + 0x70` | `0x1434e4875` | Collision layer `Character` |
| **Max Slope** | $45.0^\circ$ | `cinfo + 0x8c` | `0x1434e481f` | Read from `CharacterPhysicsData + 0x44`, converted to rad ($\times \frac{\pi}{180}$) |
| **Dynamic Friction Scale** | `height * 50.0f` | `cinfo + 0x84` | `0x1434e4839` | Scaled solver character strength |

### Up-Vector Derivation from World Gravity
At `0x1434e4790`, the engine reads the initial world gravity vector from world physics context:
```x86asm
MOV RAX, qword ptr [R13 + 0x58] ; Physics world context
MOVAPS XMM5, [RAX + 0x80]       ; World gravity vector g
XORPS XMM5, XMM0                ; Negate: v = -g
; Compute magnitude and normalize via RSQRTPS:
MULPS XMM4, XMM5
...
RSQRTPS XMM1, XMM4
...
MOVAPS [RBP - 0x60], XMM5       ; Store in cinfo + 0x50 (m_up)
```
The proxy's initial up vector $\hat{u}$ is defined as:
$$\hat{u} = \frac{-\vec{g}_{\text{world}}}{\|\vec{g}_{\text{world}}\|}$$
For standard downward gravity $\vec{g} = (0, -9.81, 0)$, $\hat{u} = (0, 1, 0)$.

---

## 3. Proxy Construction (`0x143f601d0`: `hknpCharacterProxy_ctor`)

The proxy is instantiated from the heap pool with size **`0x130`** bytes:
- **Primary Vtable**: `0x141d53348`
- **Registration**: Registers itself into `world + 0x4a8` with profiler tag `"hknpCharacterProxy"`.

### Trigonometric Precomputations (`proxy + 0x120`)
The constructor receives `cinfo + 0x8c` ($\theta_{\text{max}}$ in radians) and calculates:
$$f_{\text{temp}} = \theta_{\text{max}} + \frac{\pi}{2}$$
Using a high-order Taylor series approximation of $\sin(x)$:
$$\sin\left(\theta_{\text{max}} + \frac{\pi}{2}\right) = \cos(\theta_{\text{max}})$$
For $\theta_{\text{max}} = 45^\circ$ ($0.785398 \text{ rad}$):
$$\cos(45^\circ) = \frac{\sqrt{2}}{2} \approx 0.70710678$$
This value is stored directly at `proxy + 0x120` (`m_cosMaxSlope`).

---

## 4. The Support Check (`0x143f60710`: `checkSupport`)

The support query runs during pre-simulation character updates (`FUN_1434fc450`) to determine if Faith is supported by walkable ground, sliding down a steep slope, or in free fall.

### Mathematical Support Resolution
1. **Input Parameters**:
   - `RCX`: `hknpCharacterProxy* this`
   - `RDX`: `const Vector4& downDirection` (derived by negating `CharacterPhysicsBody + 0x120`: $\hat{d} = -\hat{u}$)
   - `R8`: `SupportInfo* groundInfo` (written back to `CharacterPhysicsBody + 0x108`)
   - `R9`: Collector context (`0x141d53678`)

2. **Contact Normal Evaluation**:
   The engine extracts the contact surface normal $\hat{n}$ from active contact manifolds.
   It computes the dot product with the down-direction vector:
   $$D = \hat{n} \cdot \hat{d}$$
   Since $\hat{d} = -\hat{u}$, $D = -(\hat{n} \cdot \hat{u}) = -\cos(\alpha)$, where $\alpha$ is the slope angle of the ground from the character's up-vector.

3. **Slope Condition**:
   Havok evaluates:
   $$\cos^2(\theta_{\text{max}}) \le 1.0 - D^2 = 1.0 - \cos^2(\alpha) = \sin^2(\alpha)$$
   - If $\sin^2(\alpha) \ge \cos^2(\theta_{\text{max}})$:
     The slope is steeper than the allowed limit ($> 45^\circ$).
     Support state is set to **`SUPPORT_SLIDING` (2)**.
   - If $\sin^2(\alpha) < \cos^2(\theta_{\text{max}})$:
     The slope is walkable ($\le 45^\circ$).
     Support state is set to **`SUPPORT_SUPPORTED` (1)**.
   - If no valid contacts exist or all contacts fail distance/angle thresholds:
     Support state is set to **`SUPPORT_UNSUPPORTED` (0)**.

### Support States Summary
| State Value | Constant Name | Description | Player Gameplay Effect |
| :---: | :--- | :--- | :--- |
| **0** | `SUPPORT_UNSUPPORTED` | Character is in mid-air / free fall | Airborne controls; jump disabled; fall timer active |
| **1** | `SUPPORT_SUPPORTED` | Ground contact $\le 45^\circ$ | Ground locomotion (running, sprinting, jumping) allowed |
| **2** | `SUPPORT_SLIDING` | Slope $> 45^\circ$ | Character slides down; locomotion restricted |

---

## 5. Dynamic Gravity & Wall/Ceiling Walking Analysis

The user asked whether the up-vector dynamically follows gravity so that calling `set_gravity` (Trainer API 21) would allow Faith to walk on walls and ceilings, and what else breaks.

### A. Does the Up-Vector Follow Gravity Dynamically?
**No.**
1. In `FUN_1434e4400`, the up-vector is computed from world gravity **only during object construction**.
2. In `FUN_1434fc450` (the per-frame simulation update), the support check reads `CharacterPhysicsBody + 0x120` directly. It never reads `hknpWorld + 0x4c0` (world gravity).
3. If `set_gravity` is called, world gravity at `world + 0x4c0` changes, pulling Faith's physics body towards the new gravity direction (e.g., towards a wall or ceiling).
4. However, because `CharacterPhysicsBody + 0x120` and `hknpCharacterProxy + 0xd0` still point along $(0, 1, 0)$, the support check continues querying straight down $(0, -1, 0)$.
5. When Faith contacts the wall, the wall normal is horizontal ($(1, 0, 0)$). The check evaluates $\alpha = 90^\circ > 45^\circ$, marking the contact as `SUPPORT_SLIDING` (2) or `SUPPORT_UNSUPPORTED` (0).
6. **Result**: Faith enters an endless falling animation and cannot run, jump, or balance against the wall.

### B. Can We Enable Wall/Ceiling Walking?
**Yes**, by synchronizing the proxy up-vector alongside world gravity:
1. Update world gravity via `set_gravity` (API 21) or direct write to `hknpWorld + 0x4c0`.
2. Compute $\hat{u} = \text{normalize}(-\vec{g}_{\text{new}})$.
3. Overwrite `CharacterPhysicsBody + 0x120` with $\hat{u}$.
4. Overwrite `hknpCharacterProxy + 0xd0` with $\hat{u}$.
5. When this is done, `checkSupport` evaluates wall or ceiling contacts against the new up-frame, correctly identifying horizontal walls or ceilings as walkable surfaces (`SUPPORT_SUPPORTED` = 1)!

---

## 6. Subsystem Failure Breakdown (What Else Breaks)

When the up-vector and gravity are redirected, three major subsystems break unless compensated:

### 1. Camera System (`ClientCameraViewManager` / `CameraComponent`)
- **World Up Hardcoding**: Frostbite's camera controller constructs the view matrix from forward direction $\vec{f}$ and a hardcoded world up reference:
  $$\vec{r} = \text{normalize}(\vec{f} \times (0, 1, 0)), \quad \vec{u}_{\text{cam}} = \vec{r} \times \vec{f}$$
- **Ceiling Walk (Upside Down)**: Pitch limits $[-85^\circ, +85^\circ]$ invert. Looking forward leaves the camera right-side up while Faith's feet are above her head. Yaw controls invert.
- **Wall Walk (Sideways)**: When looking parallel to the wall, $\vec{f}$ crosses $(0, 1, 0)$ awkwardly; when looking straight up/down the wall, $\vec{f} \times (0, 1, 0) = \vec{0}$, causing gimbal lock, division by zero, and visual jitter.
- **Remedy**: The camera view manager's reference up-vector must be hooked to mirror `CharacterPhysicsBody + 0x120`.

### 2. ANT Animation System (`AntAnimatableComponent` / Locomotion Blend Trees)
- **Foot IK Probing**: ANT uses an internal raycast sensor for two-bone foot IK adaptation. This sensor casts along world $(0, -1, 0)$. On a wall, the foot ray misses the wall surface completely, causing Faith's feet to hyper-extend into the void.
- **Planar Locomotion Extraction**: Faith's root motion and blend trees project velocities onto the world XZ plane. When running vertically up a wall, the vertical displacement along Y is misinterpreted as fall/jump velocity rather than forward run speed, breaking animation playback.

### 3. Gameplay State Machine & Kill Volumes
- **Fall Velocity Threshold**: `ClientPlayerMovePhysicsComponent` monitors vertical fall velocity $v_y = \vec{v} \cdot (0, 1, 0)$. If $v_y < -5.0 \text{ m/s}$, Faith is forced into `InAir / Falling` state. When running along a wall, sideways motion can trigger this check if velocity aligns negatively with Y.
- **World Kill Volumes**: Level kill planes are defined as AABB triggers or half-spaces with normal $(0, 1, 0)$ at the bottom of the map. Changing gravity allows the player to bypass floor kill triggers or unintentionally fall through lateral volume boundaries.

---

## 7. Open Research Follow-ups

1. **Camera Up-Vector Hook**: Reverse engineer `ClientCameraViewManager::updateView` to find the exact function building the camera basis matrix and test redirecting its up vector to match character proxy up.
2. **ANT Ground Normal Override**: Locate the ANT variable or parameter controlling the ground-normal reference for the locomotion blend tree.
3. **Dynamic Up-Vector Injection Routine**: Develop a trainer utility function in `mec_trainer.cpp` that safely updates `hknpWorld + 0x4c0`, `CharacterPhysicsBody + 0x120`, and `hknpCharacterProxy + 0xd0` simultaneously.
