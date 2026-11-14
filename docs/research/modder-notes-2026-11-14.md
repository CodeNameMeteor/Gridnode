# A modder's engine notes of 2026-11-14

Another modder's research (sections §1 to §50 of their notes, summarised for the user). **Nothing here is
checked in Ghidra unless it says so**: each address goes into `db/annotations/` only when the feature that
uses it is built and the function has been read. Items the notes themselves mark "not verified in game"
say so below.

## Taken into GridNode (API 41; read in Ghidra 2026-11-14, untested in game)

| What | Verdict in Ghidra | In GridNode |
|---|---|---|
| Visual environment holds without flicker: `WorldRenderer` slot 21 (0x141BFACA8 -> 0x14373E5D0), the "Future" state +0x87060 beside +0x85470 | Right. renderView hands wr + 0x85470 to the constant upload and wr + 0x87060 to Enlighten's update; the two are 0x1BF0 apart and the per-view state at 0x88C50 follows, so the state is 0x1BF0 bytes. | Holds written on the render thread into both copies; `GN_VE_SIZE` 0x1BF0. |
| Sprite DoF guard (`SpriteDofEnable` +0x247) | +0x247 is read by render code (0x14372EFF0, 0x143759360) on another object, not in the visual environment state: holds never write it. | Nothing needed. |
| Static model group member switch: 0x14318DC70 with the packed hash | Right: it is the client group's vtable slot 2 (property handler), the value a bool behind +8; the server group's slot 2 (0x1433E6430) does Enabled only. The client's checks neither index. | `static_member_set`; the layout editor switches a replaced group copy off (parking kept as fallback). |
| Fall threshold, kill volumes one by one | Right (slot 4 of 0x141A62EE0 is the fall check, `RSI` the asset, `[RSI + 0x40]` read). The volume list `[0x14257A8A0]` was not needed. | `fall_threshold_set`, `kill_volume_set` (detour on `KillVolume_testCharacter`). |
| Effects: EffectManager `[0x142416160]`, spawn 0x14329B860, move 0x14329F9A0, stop 0x1432A2080 | Right; spawn takes (manager, EffectBlueprint, transform, level, flags, velocity, three floats, flag) and returns a handle. 0x14329F850 (parameters) copies a blob whose layout was not read: not used. 0x1432973F0 tells when an effect is done. | `effect_spawn` / `effect_move` / `effect_stop` / `effect_alive`. |
| An asset by GUID: 0x142AEB5E0 | A compartment's lookup by GUID (its vtable slot +0x28), same lock as `Partition_findByName`; what it returns was not read. | `asset_find_guid` checks the result at run time (a data object, or a partition's primary instance). |
## Fixes and limits (for custom maps, later)

- **Job queue stall on more than two CPUs:** `[0x142373E70]` (a 0x400-slot queue) drains workers on a failed
  push (+0x8F0) only when the CPU count is 2 (0x142A94851); the notes patch 0x142FB92D0. When it triggers is
  unclear.
- **Enlighten lightmap ids wrap at 16 bits** (black buildings): 0x143791D70.
- **Renderer capacities:** view records scratch 0xC0000 -> 32 MB (0x1435CDB10); a draw batch handle of 10 bits
  (0x3FF -> 0x7FF at 0x1436C8CF3, 0x1436D27E8, plus spreading over 9 batches); the mesh table 4096 -> 16384
  before it reallocates (0x142563410, job 0x1435763F0; instance records clamp at 12 bits, 0xFFF); instance
  record pool 256 -> 1024 blocks (0x143567680, 0x143577780, 0x143573310, 0x143568C90); the renderer's
  instance list 200,000 -> 1,000,000 (0x1436BFC90); a dead-entity guard in mesh gathering (0x1432968E0).
- **Zone streamer 128 -> 512 slots** (0x143C80255) with a load-flag race guard (0x143C81C40, bit 4). Caveat
  from the notes: 0x143C81D20 and 0x143C81E60 keep a fixed 128-entry u16 array on the stack, which must grow
  or be clamped too.

## Time of day and the visual environment

- **Instant time-of-day jump:** pulse `Reset` (0x0DB4B330) on `TimeOfDayManagerEntity` (0x142856A30) and briefly
  change `EnlightenRuntimeSettings` +0x48 (FullResolveLateAddIterations): the ~20 s lighting catch-up becomes
  ~3 frames. The client clock: `ClientTimeOfDayClockEntity` at `[[0x142598AC0] + 0x28] + 0x30`. The clock runs
  41 game seconds per real second.
- **The whole state is 0x1BF0 bytes**, blended by 0x143703D60 (GridNode bounds holds at 0xB30, eight blocks).
  Blocks past that: Fog +0x620 (0xB0B?), SkyType +0x484 (Procedural / HDRI / Physical), SunFlare +0x710,
  FilmicEffects +0x9B0, ShaderParams +0xFA0 (16 x 0x30), EmitterParams +0x12A0 (per view, `[view + 0x550]`),
  SSR +0xE7C; 19 more blocks unmapped. Note the notes' Fog and SkyType offsets fall inside blocks GridNode
  names otherwise (sky +0x390, wind +0x6E0): read the blend before trusting either.
- **Quirks:** `Sky.Light1TakesColorFromOutdoorLight` (+0x606); a physical camera (`view + 0xCDC` and
  `UseCameraSettings` +0x59) overrides the depth of field focus; the Sprite DoF quad clip fix 0x1435C72EE ->
  `EB 7C`.
- Reflection cubemaps: 32 at most, six faces caught up (0x143707710, 0x143702AA0); auto exposure
  `[wr + 0x73440]`; the lens flare's cubic evaluator 0x1436FD9E0.
- Shader parameters: `ClientShaderParameterEntity` (0x141B04550). A blueprint filter in the blend (0x143703D60)
  can drop e.g. `Faith_HitEffect_Prefab`.
- `ServerTimeOfDayClockControllerEntity` handles 0x0DD8DDF7 and 0xC44B5C3C (pause and resume), as `ui.yaml` has.

## Rendering, shadows, video

- `WorldRenderer` is 0x1418D0 bytes; a per-view state array of stride 0x89400 at `wr + 0xB00` (count +0xA94);
  0x143759730 overwrites `wr + 0x118` / `+0x110` each frame; shadow settings +0x14, +0x135, +0xC8 are dead.
- Shadow distance 0x1437356A0 (camera params +0x8552C); a per-mesh cut 0x1437110D0 / 0x1437111B0
  (`ctx + 0x30B0`).
- VP6 video billboards: five LODs, the camera chosen through `[0x142415E50]`; LOD override 0x1437E2190 /
  0x143278E40 / 0x143279C00.
- Local PBR lights: `PBRAnalyticLightEntity` vtables 0x141BFF0A0 .. 0x141BFF3D0, property handler slot 2
  (0x14376A8D0), `UpdateLight` slot 23 (0x14376A030). Also in `ct-research.md` row 8.
- Steam's overlay (`gameoverlayrenderer64.dll` + 0x9C20F) crashes in its Present hook against 0x1436136E0.

## Characters and animation (ANT)

- **Posing without the replay component** (proven at `GameTimeSettings.TimeScale` = 0): the pose manager (+0x08
  bones, +0x88 record count, +0xA0 / +0xA8 inverse bind, +0xB0 bone -> record, +0xF0 parents, +0x210 pages; a
  record at `[page + 0x40] + slot * 0x20`); the override hook 0x142FCC7C0 / 0x142FD7C60 / 0x142FCF620; local to
  skinning 0x142FD9CF0 -> 0x142FD77B0 -> 0x142FCFC30; root scale 0x142FD8F20. Caveats: root bone 1
  (0x142FD8660), the hair freezes (`EAClothSimUpdater` 0x142FA2720, `[0x1423DCA90]`, with time scale 0), held
  weapons (`PamClientCharacterWeaponEntity` +0x120 / +0x130).
- A character's dynamic culling box 0x14391C700 (`[0x14257C180]`); physics and attachment override rows at
  0x1432E06B0 + 0x50 (held weapons, drones).

## Missions, physics, audio

- Mission sublevels in and out: 0x143A7A0A0 / 0x143A79FE0; the start point trigger 0x143383090; replaying a
  mission from its start 0x1432F5420; the mission state machine 0x143A7A380.
- Havok cache renewal 0x143F3CD00; custom mesh collision (`hknpExternMeshShape` 0x143F8A3E0); footstep
  materials (`MaterialDecl`, 0x1439AE870).
- Game speed as a multiplier stack: 0x1432F4E90 (`ct-research.md` row 4).
- Audio: several listeners (0x143018A10 / 0x14301D4B0, the listener count needs a bounds guard); voice lines
  (0x14303C5E0 -> slot 7 0x14301A920); `Voice::setParameter` 0x14301DA40 (in `sound-parameters-velocity.md`).

## AI, crowds, traffic, unlocks

- `PamClientAIBrainComponent` 0x143981A80; forcing 0x1432E7EA0 to 1 on a dead ragdoll crashes a worker
  (`+0x2A98582`).
- `PamClientMagropeRenderEntity` (0x142854230, +0x130 / +0x148).
- Swarms: `SwarmEntity` (0x142892890, `CrowdBoidsComponent`, `CrowdVariationComponent`); crowds and traffic
  0x143B880B0, 0x143B9B7E0, 0x143B8B0D0; AI auto-despawn `PamServerAIAutoDespawnManager` 0x143982AE0. (The
  behaviour fixes: not verified in game.)
- `MeshVariationDatabase` queries; `VisualUnlockAsset` presets and a crash at 32 unlocks (0x143366D40);
  `Faith_Arms_NoTattoo` is object variation 4240144052. (Both not verified in game.)

## Assets and diagnostics

- Layouts of `MeshVariationDatabase`, EBX, `.toc` and `cas.cat` (0x0FB2D1CE, 0x22C); the localisation string
  table's binary layout; catalogues of mission sublevels, start points, chapters and progression; the 100
  kill volumes.
- A hang on quit: `Main + 0x288`, `Server::idle` 0x1433433F0, `[0x1423A4228]` (a stuck async job counter,
  "carusRunnersLair"), exit code 42.
