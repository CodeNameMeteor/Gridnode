# Sound: Parameters and 3D Velocity (`Voice` and `State3D`)

**Engine:** Frostbite 3 Audio System (Internal Graph Engine, not Wwise)  
**Target Binary:** `MirrorsEdgeCatalyst_dump_SCY.exe` (Image Base `0x140000000`, Build `0x5762E3B7`)  
**Status:** Researched (verified against Ghidra disassembly & decompiler on 2026-11-06; to be confirmed in game)  
**Confidence:** Inferred (read directly from code; in-game runtime confirmation pending)

---

## 1. Executive Summary & Resolution of Earlier Confusion

Earlier notes (2026-11-06) reported that `Voice` vtable (`0x141acfad0`) slot `+0x18` was "not setParameter" and slot `+0x78` "takes no vector".

Disassembly of both the caller [`FUN_14303d190`](file:///D:/Dev/GridNode/docs/research/sound-parameters-velocity.md#L45) and the internal graph dispatcher [`FUN_1430060b0`](file:///D:/Dev/GridNode/docs/research/sound-parameters-velocity.md#L72) proves that:
1. **Slot `+0x18` (`0x14301da40`) IS `Voice::setParameter`:**
   In x64 Windows ABI, caller [`0x14303d190`](file:///D:/Dev/GridNode/docs/research/sound-parameters-velocity.md#L45) passes:
   - `RCX`: `Voice*`
   - `EDX`: `uint32_t paramKey` (DJB2 hash of the graph parameter name)
   - `XMM2`: `float value`
   `Voice::setParameter` (`0x14301da40`) loads `self->graphInstance` from `[RCX + 0x18]` into `RCX`, leaves `EDX` and `XMM2` completely untouched in their registers, and calls [`FUN_1430060b0(graphInstance, paramKey, value)`](file:///D:/Dev/GridNode/docs/research/sound-parameters-velocity.md#L72). Ghidra's decompiler in isolation omitted `param_2` and `param_3` because the wrapper didn't modify them.
2. **Slot `+0x78` (`0x14301dd30`) is `Voice::clearVelocity`:**
   It zeroes the velocity and smoothing buffers in `State3D` (`[state3d + 0x40]`, `[state3d + 0x50]`, `[state3d + 0x70]`).
3. **Slot `+0x80` (`0x143021f60`) is `Voice::updateVelocity`:**
   Takes `(Voice* rcx, const float* newPos, float dt)`. Calls [`State3D_update`](file:///D:/Dev/GridNode/docs/research/sound-parameters-velocity.md#L108) (`0x143021cc8`), which differentiates $\vec{v} = \frac{\Delta \vec{x}}{\Delta t}$, updates scalar speed $\|v\|$, and stores linear velocity at `State3D + 0x40`.
4. **Direct Velocity Override:**
   A mod can directly write $\vec{v} = (v_x, v_y, v_z, 0)$ into `*(float[4]*)(*(uintptr_t*)(voice + 0x28) + 0x40)` to impart instantaneous 3D audio velocity and Doppler shift.

---

## 2. The `Voice` Object Layout & Vtable (`0x141acfad0`)

Allocated by `SoundSystem::createVoice` (`0x142fe2ee0` $\to$ `0x143019860`, size `0x48` bytes):

| Offset | Type | Description |
|---|---|---|
| `+0x00` | `void*` | Vtable (`0x141acfad0`, 25 slots, `0xc8` bytes) |
| `+0x08` | `void*` | Graph node memory block |
| `+0x18` | `void*` | `SoundGraphInstance` (Audio Graph runtime context) |
| `+0x19` | `uint8_t` | Voice state flags (written by slot 19 `0x14301d7e8`) |
| `+0x28` | `State3D*` | 3D spatial state (`0xd0` bytes, null for flat 2D sounds) |
| `+0x38` | `void*` | Sound patch data pointer |
| `+0x40` | `uint8_t` | Playback status bitfield (bit 0 = active/dirty) |

### Virtual Method Table (`0x141acfad0`)

| Slot | Offset | Address | Name / Signature | Description |
|---|---|---|---|---|
| 0 | `+0x00` | `0x14301d5f0` | `sendEvent(uint32_t nameHash)` | Fires an AudioGraph event by hash |
| 1 | `+0x08` | `0x14301b1b0` | `isParameterActive(uint32_t key)` | Checks parameter activity flag |
| 2 | `+0x10` | `0x14301da80` | `setAssetParameter(uint32_t key, void* asset)` | Forwards to `FUN_143006180` |
| 3 | `+0x18` | `0x14301da40` | `setParameter(uint32_t key, float value)` | Forwards to `FUN_1430060b0` and marks dirty |
| 8 | `+0x40` | `0x14301d760` | `sendDefaultEvent(int index)` | Triggers default asset event 0..3 |
| 9 | `+0x48` | `0x14301d360` | `release()` | Releases voice and stops audio |
| 11 | `+0x58` | `0x14301b220` | `isPlaying()` | Returns `true` if voice is actively playing |
| 12 | `+0x60` | `0x14301dc70` | `substateUpdate()` | Calls `FUN_14301db40` on `State3D` |
| 13 | `+0x68` | `0x14301dc30` | `setTransform(const gn_transform* t)` | Copies $4 \times 4$ matrix into `State3D + 0x00` |
| 15 | `+0x78` | `0x14301dd30` | `clearVelocity()` | Zeroes velocity fields in `State3D` |
| 16 | `+0x80` | `0x143021f60` | `updateVelocity(const float* pos, float dt)` | Updates position, differentiates velocity |
| 18 | `+0x90` | `0x14301d7c0` | `setDistanceScale(float scale)` | Writes to `State3D + 0xb0` |

---

## 3. Disassembly Proofs

### Proof 1: `SoundEntity` Calling `Voice::setParameter`
In [`SoundEntity::updateProperties`](file:///D:/Dev/GridNode/docs/research/sound-parameters-velocity.md) (`0x14303d190` lines `14303d214`–`14303d231`):
```assembly
14303d214  LEA    RDX, [0x1427b0cb8]
14303d21b  MOV    RCX, RBX
14303d21e  CALL   0x142a424b0          ; returns RAX = pointer to float property value
14303d223  MOV    RCX, qword ptr [RDI + 0x58] ; RCX = Voice* (SoundEntity->field_0x88)
14303d227  MOV    EDX, dword ptr [RBX + 0x20] ; EDX = parameter key hash (uint32_t)
14303d22a  MOV    R8, qword ptr [RCX]         ; R8 = Voice vtable (0x141acfad0)
14303d22d  MOVSS  XMM2, dword ptr [RAX]       ; XMM2 = float parameter value
14303d231  CALL   qword ptr [R8 + 0x18]       ; CALLS Voice::setParameter (slot 3)
```

### Proof 2: `Voice::setParameter` Passthrough to Graph
At `0x14301da40`:
```assembly
14301da40  PUSH   RBX
14301da41  SUB    RSP, 0x20
14301da45  MOV    RBX, RCX                   ; RBX = Voice*
14301da48  MOV    RCX, qword ptr [RCX + 0x18] ; RCX = SoundGraphInstance*
14301da4c  TEST   RCX, RCX
14301da4f  JZ     exit_false
14301da51  CALL   0x1430060b0                ; FUN_1430060b0(RCX, EDX, XMM2)
14301da56  TEST   AL, AL
14301da58  JZ     exit_false
14301da5a  MOV    RCX, qword ptr [0x1423e29b8] ; SoundManager*
14301da61  MOV    RDX, RBX                   ; Voice*
14301da64  CALL   0x143018bb0                ; Mark voice dirty in sound manager
14301da69  MOV    AL, 1
14301da6b  ADD    RSP, 0x20
14301da6f  POP    RBX
14301da70  RET
```

### Proof 3: Writing Value in `FUN_1430060b0`
In `FUN_1430060b0`, `param_2` (EDX) is matched against the graph's parameter key entries:
```c
if ((uVar5 != 0) && (*(int *)(uVar5 + 0x14) == param_2)) {
    pfVar6 = (float *)((ulonglong)*(ushort *)(uVar5 + 0x18) * 0x10 + param_1[1]);
    *pfVar6 = param_3; // Overwrites the graph float input directly from XMM2!
    return 1;
}
```

---

## 4. `State3D` Layout (`0xd0` bytes) & 3D Velocity

Located at `*(uintptr_t*)(voice + 0x28)`.

| Offset | Type | Description |
|---|---|---|
| `+0x00` | `float[4]` | Right vector $\{R_x, R_y, R_z, 0\}$ |
| `+0x10` | `float[4]` | Up vector $\{U_x, U_y, U_z, 0\}$ |
| `+0x20` | `float[4]` | Forward vector $\{F_x, F_y, F_z, 0\}$ |
| `+0x30` | `float[4]` | Translation / Position $\{P_x, P_y, P_z, 1\}$ |
| `+0x40` | `float[4]` | **Linear Velocity vector $\{V_x, V_y, V_z, 0\}$ (in $\text{m/s}$)** |
| `+0x50` | `float[4]` | Previous frame position $\{P'_x, P'_y, P'_z, 1\}$ |
| `+0x60` | `float[4]` | Velocity history sample 1 |
| `+0x70` | `float[4]` | Velocity history sample 2 |
| `+0x80` | `float[4]` | Filtered velocity |
| `+0x8c` | `float` | Scalar speed ($\|v\|$) |
| `+0x90` | `void*` | `SoundPatchAsset*` |
| `+0xb0` | `float` | Max distance / volume attenuation scale (default `1.0f`) |
| `+0xbc` | `float` | Frame accumulator $\Delta t$ |

---

## 5. SDK Implementation Blueprint

Exposing `sound_set_parameter` and `sound_set_velocity`:

```cpp
// Set a float graph parameter on an active sound voice
int soundSetParameter(uint32_t soundHandle, const char* paramName, float value) {
    if (!paramName || !*paramName) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;
    
    const uint32_t key = nameHash(paramName);
    std::lock_guard<std::mutex> lock(g_m);
    for (const Voice& v : g_voices) {
        if (v.handle == soundHandle) {
            // Slot 3 (+0x18): void (*setParam)(uintptr_t, uint32_t, float)
            typedef void (*SetParamFn)(uintptr_t voice, uint32_t key, float val);
            const auto fn = slot<SetParamFn>(v.voice, 0x18);
            if (!fn) return GN_ERR_UNAVAILABLE;
            fn(v.voice, key, value);
            return GN_OK;
        }
    }
    return GN_ERR_NOT_FOUND;
}

// Set instantaneous 3D velocity on an active sound voice
int soundSetVelocity(uint32_t soundHandle, const float* vel) {
    if (!vel || !finite3(vel)) return GN_ERR_ARGUMENT;
    if (!isClientThread()) return GN_ERR_THREAD;

    std::lock_guard<std::mutex> lock(g_m);
    for (const Voice& v : g_voices) {
        if (v.handle == soundHandle) {
            const uintptr_t state3d = mem::readOr<uintptr_t>(v.voice + 0x28);
            if (!state3d) return GN_ERR_UNAVAILABLE; // 2D flat sound
            float* vDst = reinterpret_cast<float*>(state3d + 0x40);
            vDst[0] = vel[0];
            vDst[1] = vel[1];
            vDst[2] = vel[2];
            vDst[3] = 0.0f;
            return GN_OK;
        }
    }
    return GN_ERR_NOT_FOUND;
}
```

## Mixer groups (see also)

The mixer group table and gain processor are annotated in `db/annotations/audio-mix.yaml`; the SDK exposes them as `busVolume` / `duck` (API 35, experimental).
