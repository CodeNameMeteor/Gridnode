# Progress / handoff

Update at the end of each session. Keep it short. The full handover for a new agent is
[handover.md](handover.md).

## Custom dashes of 2026-11-13 (branch gn/custom-dashes, game-tested in part)

- The networked spawn is solved (`docs/research/custom-dashes.md`): every object of ours in the data registry
  (`DataRegistry_register`, spawn.yaml), the ServerBlueprintEntity under Faith's root bus (its owner has a ghost),
  every copy with its GUID before it. Claim-lab `networked-dash` (git-ignored) proved it; the dash ran, restart
  worked.
- `sdk/src/dashes.cpp` (internal, not in the API): a dash copied from one of the city's (marker, start box, area box,
  start arrow, end circle, guide arrows, challenge with a new GUID, nothing online), registered, placed, streamed in;
  ids out 5 s after, at level end and unload; the stats detour keeps the game's bests (index 1 off, bests put back).
  Name = the challenge's Sid. The arrows' pointing axis is learnt from the city's.
- Layout editor **Custom dashes**: place, move (free camera), size the start, guide arrows, name, Rebuild, best time
  per dash in the layout. Game-tested 2026-11-13: placing, moving, running, the best time, restart and exit (on a
  dash not remade during the run). Untested: the name in game, the arrow axis, full unload with a dash placed.
- Research briefs queued (`.workflow/queue-2026-11-13.md`): dash-map-register (map icon; the real dash's ContentId),
  dash-end-screen-best, dash-ghost, networked-prefab-api, registry-hardening, custom-trigger-volumes,
  ugc-time-trials, fbmod-dash-export, entity-liveness.

## Layout editor research of 2026-11-12: volumes, dashes, runtime bundle loading

Goal (user): place any volume (blockers, dash start / finish, mission parts, teleports), move and resize it, and
place any object from any area without that area streaming in.
- `docs/research/volume-catalogue.md`: what the game places, trigger shapes (a `Geometry` link to a shape), dashes
  (start `OBBData` linked as `StartGeometry`, finish = the end arrow's `EndVolume`, links in `mainlogic`).
- `assets used-by NAME` finds the files that place an asset.
- Bundles load from a mod at runtime (claim-lab `bundle-load`, game-tested): a sublevel request copy on the server
  thread, ready in 0.16 s, assets found through the resource group id from `ResourceManager_registerBundleGroup`
  (`docs/research/zone-streaming.md`).
- A prefab from a loaded bundle spawns anywhere (game-tested: a usable magrope from `AnchorArea3` beside Faith in
  another district, kept after a teleport; gone on a checkpoint or level reload). Removing it works too
  (`Entity_requestDestroy` per listed entity). API 38 (`sdk/src/bundles.cpp`, untested in game):
  `gn::bundleLoad` / `bundleReady` / `bundleRelease`, `gn::prefabSpawn` / `prefabLive` / `prefabDestroy`; handles
  dropped at level end; a mod's released and destroyed when it unloads. Game-tested
  with the claim lab's `bundle-api` (load, spawn, destroy, release, checkpoint reload, teleport with the parent
  held, mod unload). Parents load automatically from `assets bundle-parents`' table; the layout editor places
  prefabs from any area (`PlacedPrefab` edits, saved in the layout). Game-tested 2026-11-12: a magrope placed
  from the editor, kept after a teleport away and back. A misspelt bundle name stays loading in the game for
  good: GridNode now gives up after 30 s. A prefab holding a spawner (`SpawnReferenceObjectData`, e.g.
  `pf_placemstub`) is refused: its client half needs the server's network data and crashed the client thread.
- A movement blocker made at runtime (claim-lab `runtime-data`, game-tested 2026-11-12): a
  `PamOBBCollisionEntityData` and an `ObjectBlueprint` built in our memory with the types' create functions,
  spawned on both realms. It blocks Faith, honours the Cant flags and shows in the volume overlay. The world
  place goes in the data's `Transform` (+0x20): `PamOBBCollision_enable` resolves it there, and an identity
  put the body at the world origin. Next: this in the layout editor (place, resize, choose the type), then
  dash start / finish volumes the same way.
- Layout editor (game-tested 2026-11-12): **Movement blockers** places that blocker at the crosshair (size,
  can't stand / climb / vault / wallrun; `PlacedBlocker` edits, saved). Placed prefabs and blockers show as
  boxes while the free camera flies, are picked by a click or their row, and move with the move keys (spawned
  again there when the keys rest). `bundles.cpp` `blockerSpawn` builds the data (internal, not in the API).
- Mods menu: **Reload all** reloads the reloadable mods and loads mod DLLs added since startup (untested in game).

## Findings and implementation plan of 2026-11-11: fixes, performance, API 36 (branch, untested in game)
The plan (code review of `main` at f685e95, Frosty's SDK and formats; a PDF from the user) checked against the
code first: its findings in parts 1 and 2 hold (both logs say `no page within 2 GB ... (0 tries)`; `validate
--strict --reflected` gave exactly 67 errors). Host tests, `gridnode.py check` with the user's dump and
`verify-image` (1,123 checks, 0 errors) pass. **Nothing here has run in game.**

Done:
- **1.1 the reserve**: `near_alloc.hpp` (host-tested with a fake address map); the downward search asks about
  each candidate. The first `patches:` lines should now say `64 KB kept near the game's code`.
- **1.2 annotations**: 0 errors against the dump; `types.hpp` from the dump compiles. `validate` catches C type
  names without a dump; `gridnode.py check --dump FILE` (validate, `types.hpp`, its compile) before a push.
  Old vtables kept in notes: `ZoneStreamerEntity` 0x141cfe4c0, `PamClientEchoCustomizationManagerEntity`
  0x141c73c18 (secondary tables? open, Ghidra).
- **1.3 mod identity**: `mod_call.hpp`; every place GridNode calls a mod sets the callback running on the
  thread; an API call whose return address is in gridnode.dll goes to it.
- **1.4** every `mod.hpp` / `ui.hpp` trampoline guarded. **1.5**: dependency check before start, the
  `gn_mod_load` export checked in the file, `patch_bytes` over several instructions written paused,
  `codeWrite` waits, x64 `66 REX.W`, root mods find DLLs beside them, kept closures freed with the DLL,
  raw addresses in `api.cpp` / `layouts.cpp` / `events.cpp` use annotated names, `StaticModelGroup_placementHash`
  fingerprinted. Left: the ANT vtables in `faith.cpp` (no reflected type: need names from Ghidra).
- **2.1 steps 1 and 3**: a detour whose first instruction is 5 bytes or more is written in one store; threads
  are listed with `NtGetNextThread` (`threads.cpp`). The `detour entry` line in the Hooks list now says how
  long the write or pause took.
- **2.2** `Registry::typeOfLive` for the creator, destructor and event hooks. **2.4** copy-free
  `settings_at` / `console_command_at`, OutputDebugString only under a debugger, `event_send` log throttled,
  event names built at startup.
- **API 36**: `hook_virtual` and friends (3.1, `vhook_set.hpp` host-tested; `gn::VirtualHook`),
  `GN_MOD_MIN_API` and `gn::has` (3.3), `gn::Field<T>` (3.4), `entity_component(s)` (3.5), `ui_type_view`
  (4.3), `data_path` and its startup line (4.4).
- **4.1** `verify-image` finds the TypeInfo list head by the code linking types into it (27 sites, own
  pattern, `type_list.py`). **4.2** `notes:` on reflected fields (`notes.yaml`): validated, `meta::fieldNotes`,
  docs, the inspector's tooltip and read-only grey-out.
- **5 step 1**: `tools/assets` (`gridnode.py assets check / list / extract`), synthetic tests only.

Game-tested 2026-11-11 (user's run): the reserve is made (`64 KB kept near the game's code`, 1 try); detour
entries pause the game 1.2 to 25.6 ms (were 99 to 132 ms); `hook_virtual` on setVisible fires; `gn::Field`
TimeScale, Faith's components, the data folder line, the type view and the read-only note work; a non-mod DLL is
skipped from the file; reload and unload of detour-demo and a full unload are clean. Fixed after: class hooks were
missing from the Hooks list (now listed, and logged when taken out); `assets check` stopped at the first
catalogue without NyanNyanNyanNyan (`Data\Win32\gameconfigurations\anchorinstallpackage\cas.cat`): such
catalogues are now listed with their first 48 bytes and the rest read on. The second run (2026-11-11): every
`.toc` and `.sb` read (24 superbundles, 622 bundles; 151,832 ebx, 74,646 res, 101,056 chunk entries), so the tree
format is confirmed; every `cas.cat` starts with the signed header `00 d1 ce 01` (as the tocs do): the reader
now skips it (and tries the XOR form when the magic is not behind it). The class hook was taken out at both
unloads (`virtual PamClientCharacterEntity (and subclasses)+0x3d8 taken out`). Third run: `assets check`
passes: 11 catalogues (two counts after the magic, 115,276 entries, no patch entries in Data), every bundle
entry found in a catalogue, blocks LZ4 or stored (no Oodle). Step 1 of part 5 is done (extracting needs
`pip install lz4`): `assets extract gameplay/movement/climbing/ladders/pf_climbladder_768_nobrackets` gave
7,680 bytes (4,430 stored) starting `CE D1 B2 0F`, the EBX version 2 magic.

Step 2, EBX to YAML (2026-11-12): `tools/assets/gridnode_assets/ebx.py`, `assets ebx` and `assets ebx-check`.
Read from the user's own files (staged from the install: the index files and the four archives under 400 MB),
lossless: read, to YAML, back and written gives the same bytes for all 621 EBX files those archives hold,
which is every EBX of three superbundles (frontend 147, globals 632 entries, ui 18). Format facts found on the
way (the header of ebx.py): header 0x10..0x14 are the instance-run count, the exported-run count (the first
runs are exported) and the number of types the instances are of; a type aligned to 8 or more keeps its
8-byte header in the payload, one aligned to 4 does not; an exported instance's GUID is aligned for its type;
an array is listed after the arrays in its elements; array 0 (empty) takes the type of the first array met;
the payload never ends right at the data's end (padded to the next 16). Type codes seen: 0, 2, 3, 4, 7, 8, 10,
12, 14, 15, 16, 19, 21, 23; FileRef (9), String (6), 64-bit and 8/16-bit signed ints are read as described
but not seen yet. Not run yet: `ebx-check` over the whole install (43,040 more EBX files in archives over
the 400 MB staging limit; about 20 minutes on the user's PC). Step 3, YAML to EBX (2026-11-12): `assets ebx-build FILE.yaml` writes the EBX after checking the file's type table
against the reflected dump (`ebx.check_types`): type codes, parents, struct and enum types by name, enum values.
The writer refuses an unknown field or a reference to nothing. Checked on the 621 files: no disagreement; the
only differences are a few fields and one type (`InternetSimulationSettings`) in the data but not in the
running game (pipeline or debug only: warnings), and value types whose flags have 0x8000 in the game
(ignored). The file's second field offset is not the runtime offset (`EventConnection.TargetEvent`: 0x20 there,
0x14 in the game). An edited ladder (`Length` 7.68 to 9.5) builds and reads back. Not tested: an edited
file in the game (needs a way to load it: the `.fbmod` writer). Next (user, 2026-11-12): `assets deps`
(everything a blueprint needs, for placing objects from other areas), then the `.fbmod` writer (needs
real `.fbmod` files and a test with Frosty's Mod Manager), then the layout editor exporting to `.fbmod`.

`assets deps NAME [--into BUNDLE]` (2026-11-12; `tools/assets/gridnode_assets/deps.py`): imports followed by file GUID
(read from each EBX header, cached by SHA-1 in `bin/assets/index/`), resources by the res ids in ResourceRef fields
(a bundle's resRid is signed: masked to 64 bits), chunks by their GUIDs found in the EBX or resource bytes (no res
format needed). On the staged files: `levels/sp/frontend/frontend` needs 132 EBX, 10 res, 13 chunks, every one
found; 56 of them are not in `sp_maincity`. Not checked: whether a GUID scan can find a chunk that is not really
used (it lists more, never less), and objects whose files are in archives not staged here. The user's Frosty mods
are staged for the `.fbmod` writer (read-only access to Documents\Frosty).

`.fbmod` reader and writer (2026-11-12; `tools/assets/gridnode_assets/fbmod.py`, `assets fbmod-info`, `assets fbmod-build`):
the format worked out from the user's 15 mods (format notes in the module header). FROSTY versions 1, 3, 5: all 11
of the user's read and write back byte for byte; FBMODV1/V2 (4 mods): their header is the toc/sb tree, read only.
Bundle hashes are djb2-xor of the lower-case bundle name (resolved to names with `--data`); the SHA-1 is of the stored
(compressed) data; data is the game's payload blocks (LZ4 0x0970, as Frosty). EBX in the mods (21, written by
Frosty) read and rebuild with the same values. Test mod: `bin/fbmods/GridNode No Offline Warning.fbmod`, zhpete's
No Offline Warning rebuilt through YAML by GridNode (same values, same two bundles). Not tested: any GridNode-written
`.fbmod` in Frosty's Mod Manager or the game. Game-tested (user, 2026-11-12): the GridNode-built No Offline Warning
works through Frosty's Mod Manager, and GridNode under it logs `game data from ...\ModData\Default\ (-dataPath ...)`.

Full `ebx-check` on the user's install (2026-11-12): 43,434 of 43,661 written back the same. The 227 others
(`--keep`, grouped by section): -0.0 written as 0.0 (the writer's `v or 0`), and arrays of 2-byte elements
started 2-aligned (the game starts every array 4-aligned). Both fixed: 44 of the kept files (all four groups) now
write back the same. The one type disagreement was -1 vs 0xFFFFFFFF (the same value). Rerun on the whole install
(user, 2026-11-12): all 43,661 EBX files written back the same, 0 type disagreements. Step 2 and 3 done.

`assets usage` (2026-11-12): a catalogue of the types EBX instances are of (counts, files, superbundles, examples,
fields), to plan the general volume editor (user, 2026-11-12: place any volume, blocker, dash start/end, mission
parts, teleports; move and resize them; load any object anywhere, not only where its area streams in). Untested on
the whole install.

mec-trainer 1.10.3 (2026-11-12, user's ask; game-tested, merged #11): the player info (position, speed, state) can be dragged
while the GridNode menu is open; its place is kept (`hud.player_info_x` / `_y`), clamped to the screen; Settings > HUD
has "Reset place".

Second round (API 37, the user's choices of 2026-11-11; untested in game):
- **2.1 step 2**: detours made between `hooks_begin` and `hooks_commit` (per thread) are written in one
  pause; the loader wraps every `gn_mod_load`, so all mods get it without a rebuild.
- **3.2**: `gn_mod_load2(api, self)` (preferred when exported; `GN_MOD` exports both) and 20 `_m` calls;
  `mod.hpp` uses them when the host has them. Tick listeners are named after their mod.
- **3.6**: hypotheses in `unverified` namespaces in `engine.hpp`. `NamedChallengeFinish_statsCall` promoted
  (patch_call checks it every run). The other 24 GridNode uses are named `unverified::` with a note per file
  (assets.cpp, bodies.cpp, layouts.cpp, sounds.cpp): those notes are the list to confirm in Ghidra.
  `gridnode.py unverified-check` (CI).
- **2.3**: `EntityIndex` (host-tested): by address and per class; destruction removes at once; the dead set
  is only for entities made before GridNode; pruning every second as a backstop; `entities_of_class` and
  `entities_near` read the matching classes' lists only. Not done: the spatial grid for `entities_near`.

Game-tested 2026-11-12 (user's run): each mod's detours written in one pause (mec-trainer 2 in 2.2 ms,
detour-demo 2 in 2.0 ms; reloads need none, the entries stay); the trainer's detour features, god mode and
bunny hop work; the Entities tab and level changes look as before; detour-demo's API 36 features still work;
two reloads and a full unload are clean (7 code patches undone, 2,160 hooks out, gridnode.dll freed); the
trainer's camera / time hold come back when it unloads (the mod handle). GridNode's own two startup
detours still pause one at a time (6.3 and 1.2 ms): batching them would save a few ms at startup.

Entities made before GridNode loaded (2026-11-12; untested in game): injected into a running level, the
index missed them (the trainer's volumes: 152 of 400 in one area). Read in Ghidra: the game keeps a list
per class and realm, `ClassInfo_addToWorldList` (0x1431661f0, 163 callers): head at ClassInfo + (realm +
6) * 0x10, a second at + 8; node {next, prev link} at a per-class offset in the entity. At startup GridNode
walks every entity class's lists once and finds each node's entity by stepping back to the address whose
vtable is that class (`class_lists.hpp`, host-tested); the log says how many it found. Game test: 14,878
found (92 classes, 0 unresolved, 16 ms) of ~71k, none of the logic entities (NotEntity, DelayEntity,
CompareBoolEntity, TimelineEntity) and no trigger volumes. So it also walks the entity buses: every
entity keeps its bus's interface at +0x20; buses form a tree (parent +8, first child +0x28, next
+0x20) and each holds its entities at +0x38, one per id in its data's list (data +0x60, list +0xd0 or
+0xf8 by flag bit 28 at +0x34; EntityBus_deliverEvent). From the found entities it walks up to every
root and down the whole tree. Game test: +25,572 (4 roots, 16,577 buses, 172 ms), 41,676 of ~71k:
TimelineEntity data now, still no barriers or triggers. Ghidra (FUN_143150100): a bus keeps only the
peers its data lists by id (those with connections), so those are on no list. Last pass (untested in
game): GridNode reads the game's private read-write memory once for a word in the image with one of
the known buses 0x20 bytes on, and keeps it if its vtable is an entity class. Game test (2026-11-12):
works; injected mid-level the trainer's volumes and barriers all show; 3.9 GB read in 1.3 s.

Optional dependencies (2026-11-12): the trainer could not be unloaded while
detour-demo (which wants it optionally and imports its godMode) was loaded, and detour-demo loaded first
never saw the trainer. Now unloading or reloading a mod unloads its optional dependents first and loads
them again after; loading a mod reloads the loaded mods that want it optionally (`mods.cpp`,
`optionalDependents`). A needed dependency still blocks the unload. Game test: works (unload and load
of the trainer with detour-demo loaded). Before merge, also tested: injection mid-level, the player
build, and a mod built for 1.0.0 (loads and works).

Still not done: moving GridNode's three internal "every subclass" pools onto `hook_virtual`, the spatial
grid, parts 5.2 to 5.6 (a separate PR).

In-game test (developer build, all mods from this build in `bin\mods`):
1. Start the game, inject, load a level. `gridnode.log`: the first `patches:` lines say `64 KB kept near the
   game's code`; `startup: game data from ...` names the Data folder.
2. GridNode menu → Hooks: each `detour entry` says `one instruction, written in N ms` or `threads paused for
   N ms`. Send the numbers (they were 99 to 132 ms each).
3. Detour demo window → API 36: `setVisible ... hooked`, the count rises when Faith's visibility changes
   (a cutscene, the replay), TimeScale reads 1.000, Faith's components are listed, the data folder shows.
4. Inspector → Shortcuts → Faith (client): a `detour-demo: PamClientCharacterEntity` section above the fields.
5. Inspector → Types → `PamCharacterEntityData` → default instance: `PamUseRunnersGhostShader` is blue with a
   tooltip and greyed out.
6. Mods → Reload detour-demo, then Unload it: the log's `hooked virtual PamClientCharacterEntity+0x3d8 in N
   of M vtables` line comes back after the reload; after the unload the Hooks list no longer names
   detour-demo, the inspector section goes, and nothing crashes.
7. Put an empty file named `notamod.dll` in `mods\` and restart: the log says `notamod.dll does not export
   gn_mod_load; skipped` (it is no longer loaded to find out). Delete it after.
8. With the game closed, from the repo: `python tools/gen/gridnode.py assets check --data "<the Data folder
   from step 1>"`. Send the whole output (catalogue layout, bundle count, codecs found).
Send: `gridnode.log`, the numbers from step 2, the output of step 8.

## Mixer groups: API 35 (2026-11-10; game-tested, stable)
- `busVolume` / `duck` (API 35) move the game's mixer groups. Ghidra-confirmed: group table, property blocks, the gain processor's update (`db/annotations/audio-mix.yaml`). Game-confirmed: the hook on the gain processor, per-group offset with fades, restore, duck, and unload undoing it. Not tested: which thread the mixer runs on, and unloading while the mixer is inside the hook. Stable (the user asked, 2026-11-10). `busVolume(bus)` only reads the group's end gain (parents, pause mixes and offsets included); it does not set a slider.

## Research notes of 2026-11-06: what "confirmed" means below (2026-11-11)
- The sections below up to Version 1.0.0 are bulk research (another agent, Ghidra and the MECMP sources). Where they
  say "confirmed", read: claimed by that research. Their annotations entered `db/annotations/` as `hypothesis`
  (2026-11-11) and become `confirmed` only when checked again in Ghidra; nothing in them is game-confirmed.

## Custom maps & static mesh custom loading: synthetic DirectX 11 ingestion and vertex declarations (2026-11-06; to be confirmed in game)
- **Synthetic Direct3D 11 mesh ingestion pipeline resolved**: Documented in [custom-mesh-loading-dx11.md](research/custom-mesh-loading-dx11.md) and annotated in `db/annotations/render.yaml` (confirmed in Ghidra; to be confirmed in game).
  - Runtime GPU buffer allocation (`MeshLod_createGpuBuffers` at `0x14354fe00`): Direct creation of `ID3D11Buffer` vertex and index buffers via render device slot 21 (`0xa8`) without offline `.ebx` or `.sb` compilation. Automatically sets status flag `lod->statusFlags |= 0x20000000` (Resident / Ready). Index buffer offset (`lod->indexOffset` at `+0x5c`) defines vertex buffer byte size, supporting contiguous single-buffer memory layouts `[Vertices || Indices]`.
  - Dynamic input layout & vertex declaration caching (`GraphicsPlatform_createOrGetVertexDeclaration` at `0x1435b8f30`): Dynamically resolves and caches `ID3D11InputLayout` wrappers in an `eastl::map` at `*g_platformInfo + 0xb0` from an 84-byte `GeometryDeclarationDesc` (16 element descriptors: `{usage, format, offset, stream}`). Works for any custom vertex format at runtime without requiring pre-compiled shader permutations.
  - Section shader permutation resolution (`MeshSetSection_initShadersFromDeclaration` at `0x14354b140`): Populates 18-element declaration table (`MeshSetSectionRecord` at `section + 0x00`) for standard rendering (slot 1), hardware instancing (`WorldTransform4x3Float` at slot 5), and auxiliary position streams (slot 12).
  - Reflected asset & material bindings (`RigidMeshAsset` and `MeshMaterial`): Mapped exact byte offsets (`+0x10 Name`, `+0x20 MeshSetResource`, `+0x44 EnlightenType`, `+0x60 Materials`, `+0x68 NameHash`, `+0x6c StreamingEnable`). Setting `EnlightenType = EnlightenType_Dynamic` (0) or `EnlightenType_LightProbe` (1) and `StreamingEnable = false` (0) bypasses offline radiosity baking and streaming queues, lighting custom geometry dynamically via spherical harmonics irradiance probes.
  - Added low-level structs to `db/annotations/render.yaml`: `VertexElementDesc` (`0x04`, align 1), `GeometryDeclarationDesc` (`0x54`, align 4), `MeshLodGpuRecord` (`0x48`, align 8), `MeshSetSectionRecord` (`0x90`, align 8), `MeshSetSection` (`0x130`, align 8), `MeshLod` (`0xa8`, align 8), `MeshSetResource` (`0x90`, align 8).
  - Strict database validation: `gridnode.py validate --strict` maintains 0 errors, 0 warnings across 1,125 items; `verify-image` passes 1,106 checks against `MirrorsEdgeCatalyst_dump_SCY.exe` (0 errors, 0 warnings).

## Havok dynamic physics simulation and movable props research (2026-11-06; to be confirmed in game)
- **Dynamic Havok simulation & movable props pipeline resolved**: Documented in [havok-dynamic-physics-props.md](research/havok-dynamic-physics-props.md) and annotated in `db/annotations/physics.yaml` (confirmed in Ghidra; to be confirmed in game).
  - Subsystem architecture & core strides: Decouples collision bodies (`hknpBody`, stride `0x90` / 144 bytes at `*(world + 0x20)`) from simulation dynamics (`hknpMotion`, stride `0x80` / 128 bytes at `*(world + 0xe0)`). Static bodies have `motionId = 0` and `flags = 1`. Dynamic bodies have `motionId > 0`, `flags & 2 != 0`, and `motionType = 2`.
  - Mass properties & inertia derivation: `PhysicsEntityFactory_createBody` (`0x143516450`) with `motionType = 2` automatically triggers `FUN_1434cf0d0` to compute shape mass properties, diagonalize the inertia tensor into principal axes, encode inverse inertia $I_{xx}^{-1}, I_{yy}^{-1}, I_{zz}^{-1}$ and inverse mass $m^{-1}$ as 16-bit half-floats at `motion + 0x20..0x26`, and claim a motion index from the pool via `hknpWorld_allocateMotion` (`0x143f43540`).
  - Velocity & impulse manipulation API: Fully decompiled direct physics functions:
    - `hknpWorld_setBodyVelocity` (`0x143f3af60`), `hknpWorld_setBodyLinearVelocity` (`0x143f3b139`), `hknpWorld_setBodyAngularVelocity` (`0x143f3b200`), `hknpWorld_setPointVelocity` (`0x143f3b3a0`).
    - `hknpWorld_applyLinearImpulse` (`0x143f3b450`): adds $\Delta v = m^{-1} J$ directly to linear velocity at `motion + 0x40`.
    - `hknpWorld_applyAngularImpulse` (`0x143f3b4f0`): rotates world torque impulse into principal inertia frame and computes $\Delta \omega = I^{-1} L_{\text{local}}$ at `motion + 0x50`.
    - `hknpWorld_applyPointImpulse` (`0x143f3b610`): computes torque $L = (P - \text{centerOfMass}) \times J$ and applies both linear and angular impulses.
    - Wakes sleeping bodies / simulation islands via `FUN_143f43fa0` (`body->simulationIslandId != -1`).
  - Collision contact event callbacks: Havok event system dispatches via `hknpEventManager` (`world + 0x540`, `0x143f7d0b0`) using event signals (1 = `manifoldStatus`, 2 = `manifoldProcess`, 5 = `contactImpulse`, 6 = `contactImpulseClipped`, 10 = `bodyActivation`). Frostbite bridge `HavokRigidBodyHook_registerSignals` (`0x1434f9580`) and contact manifold decompression `hknpContactImpulseEvent_unpack` (`0x143f3ddf0`) extracting contact normal and 4 contact points in world coordinates.
  - Command processor protocol: Decoded 28 API commands from `FUN_143f7bc40` (`createBody`, `addBody`, `destroyBody`, `setBodyTransform`, `setBodyVelocity`, `applyLinearImpulse`, `applyPointImpulse`, `setGravity`, `setMaterialFriction`, `setMaterialRestitution`).
  - Open for further research: Runtime cloth/soft-body modifier hooks (`hknpSoftContactModifier`), compound kinematic-dynamic constraint joints, and sound effect parameter binding upon high-velocity impacts.

## Missions, objectives, and checkpoint scripting research (2026-11-06; to be confirmed in game)
- **Missions, objectives & checkpoint progression resolved**: Documented in [missions-objectives-checkpoints.md](research/missions-objectives-checkpoints.md) and annotated in `db/annotations/levels.yaml`, `events.yaml`, and `messages.yaml` (confirmed in Ghidra; to be confirmed in game).
  - Subsystem architecture & state machine: `PamServerMissionManagerEntity` (vtable `0x141c7add0`) manages mission transitions via event dispatcher `vf4` (`0x143a74bf0`): `StartMission` (`0x68fb16fc`, cancels conflicting missions via `0x143a79520`, sets state 2 / InProgress), `RestartCurrentMission` (`0xf258e0db`), `GetLastCheckpointTransform` (`0xc5b7322f`, outputs `*DAT_14257c9d8 + 0xe0`), `AbandonMission` (`0x41ee0da7`), and `CompleteMission` (`0x37bb9bfc`).
  - Objectives subsystem & verified events: `ServerObjectiveEntity` (vtable `0x141b4eaa8`, size `0x100`) event handler `vf4` (`0x143383810`) handles verified event hashes: `ShowObjective` (`0xd0488c09`), `HideObjective` (`0x23ccf4aa`), `Success` (`0x7f3e7106`), and completion broadcast `OnSuccess` (`0xbb06a967`). `ClientObjectiveEntity` (vtable `0x141b2a308`, size `0xf8`) controls HUD waypoint visibility via flag bit 7 (`0x80`) at `+0x18`.
  - Checkpoint placement & level reload pipeline: `PamServerCheckpointEntity` (vtable `0x141c7e750`, size `0x150`) binds spawn transform (`0x87525469` at `+0x80`), sequence index (`0x0cc9279b` at `+0x70`), and trigger orientation (`0xae496a36` at `+0xd0`). Faith's active reload position lives at `*DAT_14257c9d8 + 0xe0`. Writing directly to `*DAT_14257c9d8 + 0xe0` allows mods to set arbitrary custom checkpoints anywhere in the world. Level reloads are triggered via `ServerLevel_reloadLastCheckpoint` (`0x143341d40`), executing `ServerRoundResetMessage` (`0x4bb8de5c`) and `ServerLevelRestartMessage` (`0xf8429a0e`).
  - Open for further research: Dynamic blueprint sequence stitching for custom campaign chapters, offline save game injection for objective state, and custom audio voice line synchronization during objective milestones.

## Camera view matrix decoupling and 6-DOF freedom research (2026-11-06; to be confirmed in game)
- **Camera pipeline and 6-DOF decoupling resolved**: Documented in [camera-view-matrix-6dof.md](research/camera-view-matrix-6dof.md) and annotated in `db/annotations/camera.yaml` (confirmed in Ghidra; to be confirmed in game).
  - Subsystem architecture & root cause: Discovered why the camera flips upside down or traps in gimbal lock when wall-walking or changing gravity. `ClientCameraViewManager_frameUpdate` (`0x14327ba00`) calls `ClientCameraViewManager_updateModifiers` (`0x14327e850`), resetting `manager->worldBasisMatrix` (`manager + 0x10f0`) to an exact identity matrix with up = $(0, 1, 0)$ each frame. `FPSCamera_computeTransforms` (`0x14327fb80`) multiplies the raw head transform $\mathbf{T}_{\text{head}}$ (from provider `FPSCamera + 0x150`) by `manager + 0x10f0`. On walls/ceilings, Faith's proxy up is perpendicular or opposed to world Y, while Frostbite's frustum builder (`0x14352d4c0` / `0x143536f10`) clamps pitch within $[-89^\circ, +89^\circ]$ relative to the matrix up axis, forcing violent $180^\circ$ roll snapping.
  - Camera hierarchy & vtables reversed: `CameraObject` base class (`0xb0`, refCount at `+0x08`, transforms at `+0x10` and `+0x50`, velocity `+0x90`, fov `+0xac`), `FPSCamera` (`0x190`, vtable `0x141b18e58`, type 5, headTrackProvider `+0x150`, parentTransform `+0x160`), `FreeCamera` (`0x210`, vtable `0x141b18c78`, type 3, ancestor of `DemoRecorderCamera`).
  - 6-DOF roll support: Input concept IDs confirmed from `InputConceptId::toString` (`0x14468ca20`): `0x78` (Pitch), `0x79` (Yaw), and `0x7a` (`ConceptFreeCameraRotateZ` / Roll). GridNode's per-object vtable hook on slots `+0x68` (`camFrameUpdate`) and `+0x70` (`camOutput`) bypasses the Frostbite pitch clamp and allows continuous arbitrary roll angles $\phi \in [-180^\circ, 180^\circ]$.
  - Decoupling architectures:
    1. *GridNode `camera_set` override*: Replaces active camera vtable slots `+0x68` and `+0x70` with arbitrary 6-DOF matrices (full roll, no pitch clamp).
    2. *Surface-aligned `worldBasisMatrix` injection*: Injecting surface normal $\hat{n}$ into `manager + 0x10f0` allows Faith to run on walls with first-person perspective tracking her feet.
    3. *Property dispatch `0x846f73f0` (`OverrideTransform`)*: Handled by `ClientCameraComponent` (`0x141b0a298`).
  - Open for further research: ANT foot IK surface normal raycasting along proxy $-\hat{u}$, dynamic surface normal interpolation during wall-run transitions, and inverted pitch culling behavior in Frostbite GBuffer.

## Visual environment parameters, render hook, and timelines research (2026-11-06; to be confirmed in game)
- **Visual environment & timelines pipeline resolved**: Documented in [visual-environment.md](research/visual-environment.md) and annotated in `db/annotations/render.yaml`, `entity.yaml`, and `faith.yaml` (confirmed in Ghidra & runtime traces; to be confirmed in game).
  - Subsystem architecture & state layout: Destination buffer is `worldRenderer + 0x85470` (`0xb30` / 2,864 bytes). Complete catalog of 8 functional parameter blocks documented with exact byte offsets: Outdoor Light (`+0x000` to `+0x1d0`), Tonemap (`+0x1d0` to `+0x330`), Color Correction / Grading (`+0x330` to `+0x390`), Sky & Clouds (`+0x390` to `+0x6e0`), Global Wind (`+0x6e0` to `+0xa50`), Depth of Field (`+0xa50` to `+0xac0`), Vignette (`+0xac0` to `+0xb00`), and Film Grain (`+0xb00` to `+0xb30`).
  - The Render Hook (eliminating flicker): Post-update simulation writes flicker because `VisualEnvironmentManager_update` (`0x143705b30`) and `VisualEnvironmentContext_blend` (`0x143703d60`) recompute layer curves on the client view update (`0x1431a40e0`). Hooking **Slot 21** (`0x141bfaca8`, original `0x14373e5d0`) of `WorldRenderer` vtable (`0x141bfac00`) executes synchronously on the Render Thread immediately prior to constant buffer upload (`0x1436fab20` / `0x1436f80c0`), ensuring 100% flicker-free parameter overrides.
  - `TimelineEntity` architecture (`0x141beab28`): Reflected class (size `0x490` / 1,168 bytes, factory `0x14368c060`, ctor `0x14368d590`, dtor `0x14368dc60`). Property connections at `+0x30` StartTime, `+0x40` EndTime, `+0x50` PlaybackSpeed, `+0x60` ExternalTime, `+0x98` CurrentTime, and `+0xb0` output property (`0xa4fabb19`).
  - Scrubbing & external clock bypass: Controlled via `Timeline_SetTime` (`0x143691120`). Passing `force = true` overrides the external time connection guard at `timeline + 0x60`, forcing immediate keyframe evaluation (`0x1436903e0`) and event dispatch (`0x1436922c0`).
  - Open for further research: Custom 3D LUT texture injection via `ColorCorrectionComponent`, Camera View Manager autofocus coupling, and dynamic timeline track synthesis.

## Map markers and presence research (2026-11-06; to be confirmed in game)
- **Map markers & presence pipeline resolved**: Documented in [map-markers-and-presence.md](research/map-markers-and-presence.md) and annotated in `db/annotations/ui.yaml` and `online.yaml` (confirmed in Ghidra & MECMP; to be confirmed in game).
  - Subsystem architecture: `PresenceInfoManagerPool` (vtable `0x141c74bb8`) coordinates level entities `PamClientPresenceInfoManagerEntity` (vtable `0x141c74be8`, creator `0x141c74cb8`, factory `0x143a35310`, ctor `0x143a38de0`). Slots vector at `+0x50` / `+0x58` has capacity configured via `PamSocialPlaySettings + 0x20` (typically 5 to 16 slots).
  - Slot layout & 16-byte SIMD alignment: Contiguous `0x50`-byte slots (`shown` at `+0x00`, `priority` at `+0x04`, `OnlineId` at `+0x08`, `worldPos` at `+0x30`, `blueprintInstance` at `+0x40`, `entityHandle` at `+0x48`). World position argument in `PresenceInfoManager_assignSlot` (`0x143a405e0`) is loaded via `MOVAPS` (`0x143a40681`) and **must be 16-byte aligned** (`alignas(16)`), otherwise causing a General Protection Fault (`0xC0000005`).
  - Slot eviction & priority locking: `assignSlot` evicts occupied slots only when incoming priority > existing slot priority. Game online friends use priority `0`; mods use priority `1000` to prevent eviction by background friend queries.
  - Prefab spawning & UI projection: Spawns `PF_PresenceInfo` blueprint on first assignment, binding `PamClientPresenceInfoEntity` (vtable `0x141c74ac0`, ctor `0x143a38c10`) with property connectors for tooltip name (`0xbf943de7`), world transform (`0x7f30d4fe`), Nucleus ID (`0xbb211b07`), valid (`0x0d6ef4f3`), and shown (`0xcacba3e1`), driving `PamUIMapMarkerEntity` on the map and radar.
  - Hook point: Patches `PresenceInfoManagerPool_update` vtable slot at `0x141c74bc8` (original `0x143a4bb30`), running on client game thread before native slot updates to inject positions and assert `shown = 1`.
  - Runner Tag resolution & solo test mirror: Asynchronously resolved by gateway via Nucleus ID (`OnlineId.id` at `+0x08`). Local player online identity read from `*0x14257cc68 + 0x30` (`g_ownPresenceUser`). Singleplayer testing mirrors local `OnlineId` for loopback verification.
  - Open for further research: Offline custom tag texture injection without EA Nucleus server connectivity, 3D in-world pin geometry / beacon recoloring, and tooltip rich-text formatting.

## Custom maps, meshes, and asset spawning research (2026-11-06; to be confirmed in game)
- **Asset discovery, dynamic partition injection, instanced groups & material override pipeline resolved**: Documented in [custom-maps-assets-pipeline.md](research/custom-maps-assets-pipeline.md) and annotated in `db/annotations/resources.yaml`, `render.yaml`, `spawn.yaml`, and `levels.yaml` (confirmed in Ghidra & reflection; to be confirmed in game).
  - Dynamic in-memory partition injection: `Partition` struct (`0x30` bytes: `+0x08` Guid, `+0x18` name, `+0x20` instances array `DataContainer**`, `+0x28` count `uint16_t`). `PartitionMapNode` (`0x18` bytes: `+0x00` path, `+0x08` partition, `+0x10` next). Synthetic partitions can be dynamically allocated and inserted into `compartment + 0x158` bucket linked lists under mutex lock (`FUN_142a9f970` / `FUN_142aa1840`) using case-insensitive DJB2-XOR hash (`0x142aeb940`). Engine lookups (`Partition_findByName` `0x142aeb310`, `Partition_findByGuid` `0x142aeb230`, `resourceGroup_findOrCreateEntry`) resolve injected partitions without requiring `.ebx` or `.sb` mod super-bundles.
  - Hardware-instanced multi-mesh groups (`StaticModelGroup`): `StaticModelGroupEntityData` (`0xb0` bytes, vtable `0x141b73f40`) holds `MemberDatas` array at `+0x90`. Each `StaticModelGroupMemberData` (`0x68` bytes) holds `InstanceTransforms` array at `+0x08` (`LinearTransform`), `MeshAsset*` at `+0x38`, and `InstanceCount` at `+0x40`. `ClientStaticModelGroupEntity` (vtable `0x141afbe90`, size `0x108`) and `ServerStaticModelGroupEntity` (vtable `0x141b5e3a0`, size `0xc0`). `ClientStaticModelGroup_placeMember` (`0x14318fbc0`) transforms member instances by the group world transform and submits updates to `RenderWorld` (`GameWorlds[0] + 0x48`). `StaticModelGroup_placementHash` (`0x143b722e0`) quantizes position, rotation, and scale into a 16-byte placement hash at `+0x78` for GPU hardware instancing batching.
  - Material parameter & texture overrides without shader recompilation: `RigidMeshAsset` (`0x78` bytes, vtable `0x141bbdab0`) holds `Materials` array at `+0x60` (`MeshMaterial*`). `MeshMaterial` (`0x70` bytes, vtable `0x141bbdae0`) holds `ShaderInstance` at `+0x10` (`SurfaceShaderInstanceDataStruct*`, size `0x28` bytes). `VectorParameters` at `+0x10` (stride `0x20`, `Vec4 Value` at `+0x00`, `ParameterName` string at `+0x10`), `TextureParameters` at `+0x20` (stride `0x10`, `ParameterName` at `+0x00`, `TextureBaseAsset* Value` at `+0x08`). Cloned materials can override colors, emissive accents, and texture maps dynamically.
  - Binary `MeshSetResource` & GPU ingestion: `MeshSet_deserializeFromRes` (`0x1435404c0`) reads binary RES headers (`+0x88` streamed flag, `+0x8c` LOD count, `+0x8e` section count, `+0x20 + i*8` LOD pointers). `MeshLod_createGpuBuffers` (`0x14354fe00`) creates D3D11 vertex and index buffers via render device slot 21 (`0xa8`) and asserts ready flag `0x20000000` in `lod + 0x50`.
  - Sublevel streaming anchoring & Level Root Bus: `EntityBus_getLevel` (`0x14314c5e0`) retrieves `*(Level**)(*(uintptr_t*)bus + 0x38)`. Client Level root bus can be retrieved from `*(ClientGameWorld + 0x20)`. Spawning into child buses created under the level root bus (via `ClientEntity_spawnIntoBus` `0x1431c44b0` / `fb_EntityBus_create` `0x143146e90`) makes custom map geometry immune to sublevel stream-out unloads.
  - Four custom collision strategies: Prop borrowing (Phase 1), standalone Havok boxes / hulls (API 34, `PhysicsEntityFactory_createBody` `0x143516450` via `DummyBarrierEntity`), static compound part injection (`hknpShapeInstance` in `GroupRigidBody`), and precompiled Havok assets in `.fbmod`.
  - Layout stability & editor improvements: Keying edits by `(partitionGuid, dataBusPeerFlags)` or `IndexInBlueprint` rather than fragile coordinates, and non-destructive removal by parking at $y = -15,000$ (`kParkDepth`).
  - Open for further research: Synthetic DX11 vertex buffer generation directly from raw vertex arrays, dynamic lightmap and Enlighten radiosity bypass, and Havok static compound runtime triangle insertion broadphase cost.

## Havok world facts, static compound shapes, and body flags research (2026-11-06; to be confirmed in game)
- **World allocation, compound shapes & body flags resolved**: Documented in [havok-world-internals.md](research/havok-world-internals.md) and annotated in `db/annotations/physics.yaml` (confirmed in Ghidra; to be confirmed in game).
  - `hknpWorld` allocation & layout: `0x5a0` bytes (1440 bytes), aligned to `0x10`, allocated at `0x1434d0ff0` via `hkMemoryRouter::alignedAlloc`. Primary vtable `0x141d51418` (`hknpWorld::vftable`), constructor at `0x143f3e820`. Capacity at `+0x28` (`m_bodyCapacity`), mask at `+0x2c` (`0x3fffffff`), used body count at `+0x68` (`m_numBodiesUsed`, which GridNode's `freePlaces` reads), embedded body manager at `+0x10` (size `0x90`), bodies array at `+0x20` (stride `0x90`), motions at `+0xe0` (stride `0x80`), gravity at `+0x4c0`.
  - Static compound shape & 6-byte AABB tree: `m_instances` at `+0x60` (stride `0x80` / 128 bytes); compressed AABB tree at `+0xc0` (nodes at `+0x10`, count at `+0x18`, root min at `+0x20`, root max at `+0x30`). Node decompression formula in `0x143e99090` verified with constant $0.0044247787 \equiv \frac{1}{226.0}$ at `0x1425a8e10` (`HavokAabb6Step`): $\text{step} = (\text{parent.max} - \text{parent.min}) / 226$, $\text{min} = \text{parent.min} + (b \gg 4)^2 \times \text{step}$, $\text{max} = \text{parent.max} - (b \ \& \ 15)^2 \times \text{step}$. Byte 3 bit 7 = inner node flag (right child offset $= (((b_3 \ \& \ \text{0x7f}) \ll 16) \ | \ u_{16}) \times 2$); leaf node bytes 4..5 are `int16_t` instance index into `m_instances`.
  - `GroupRigidBody` (`0x1434eda80`): `GroupRigidBody_getInstanceTransform` takes `(GroupRigidBody* self, LinearTransform* out, uint32_t partIndex)`. It reads part instance index from `self + 0x110 + partIndex * 4`, retrieves `hknpStaticCompoundShape*` from `self + 0x58`, and delegates to `hknpShapeInstance_buildTransform` (`0x143f50740`), which multiplies rotation columns by scale `+0x40`.
  - `hknpBody` flags at `+0x58`: Bit 0 (`0x0001`) = `IS_STATIC`, Bit 1 (`0x0002`) = `IS_DYNAMIC`, Bit 2 (`0x0004`) = `IS_KEYFRAMED`, Bit 3 (`0x0008`) = `IS_ACTIVE`, Bit 10 (`0x0400`) = `TRANSFORM_DIRTY` / `AABB_DIRTY`. Immovable check tests `flags & 5` (static or keyframed).
  - Open for further research: broadphase dynamic tree rebalancing under heavy kinematic mutation, runtime body slot exhaustion behavior, and in-game part-to-instance mesh transform alignment.

## Character spawning, outfits, bone poses, and vocals research (2026-11-06; to be confirmed in game)
- **Four character pillars resolved**: Documented in [characters-spawning-outfits-poses-voices.md](research/characters-spawning-outfits-poses-voices.md) and annotated in `db/annotations/character.yaml`, `spawn.yaml`, and `audio.yaml` (confirmed in Ghidra & MECMP; to be confirmed in game).
  - Spawning & Despawn: `ServerCharacterSpawnEntity::spawn` (vtable `+0xd0`, `0x143390830`) with `SpawnInfo` (size `0x70`, `player = null`, `useTransform = 1`). Teardown via spawner vtable `+0x108` (`ServerCharacterSpawnEntity_unspawn`, `0x14338e4c0`), calling `ServerCharacterSpawnEntity_removeSpawned` (`0x14338e5d0`) and `Entity_requestDestroy(character, bus)` (`0x143167700`) to safely destroy ghosts and clean spawner lists.
  - Outfits & Customization: `PamServerCharacterEntity::applyCustomization` (vtable `+0x340`, `0x14336cf00`) calls `ServerCharacterEntity_applyCustomizeData` (`0x14336c860`), querying `fb::TI_ServerCharacterCustomizationComponent` (`0x1431c6eb0`) to bind `CustomizeCharacterData` visual unlocks and dirtying network replication (`NetState_setDirty`). Client ghost shader toggled via `PamClientCharacterEntity + 0x334` (`bool ghostShader`).
  - Bone Pose Injection: `PamClientReplayPlayerComponent` (vtable `0x141c48a28`, size `0x310`) in `PamClientReplayPlayerComponent_update` (`0x1439501a0`). Keyframes use stride `0x20` (Vec4 translation, Vec4 quaternion). Setting `skipRoot` (`+0x2e6`) to 1 bypasses automatic root placement, eliminating the severe altitude drift bug from Bone 1 (`AITrajectory`) rotation errors.
  - Vocals & 3D Audio: `VoiceOverSystem` (`*0x1423e3150`) manages speech lines via `VoiceOverSystem_submitConversation` (`0x1430477f0`), while `VoiceOverSystem_setSources` (`0x1430467f0`) dynamically binds 3D audio emitters to `PamClientCharacterEntity` transforms for moving spatial dialogue. One-shot barks use `SoundSystem_createVoice` (`0x142fe2ee0`) and `Voice_setTransform` (`0x14301dce0`).
  - Open for further research: Isolated spawner instantiation without borrowing level spawners, `CustomizeCharacterData` asset cataloging, and direct ANT animatable pose model injection.

## Havok character controller and up-vector / gravity research (2026-11-06; to be confirmed in game)
- **Character proxy and support check resolved**: Documented in [character-controller-proxy.md](research/character-controller-proxy.md) and annotated in `db/annotations/physics.yaml` (confirmed in Ghidra; to be confirmed in game).
  - `fb::CharacterPhysicsBody` (vtable `0x141bab5e0`, proxy at `+0x50`, support info at `+0x108`, up-vector at `+0x120`) and `hknpCharacterProxy` (vtable `0x141d53348`, size `0x130`, normalized up-vector at `+0xd0`, $\cos(	heta_{	ext{max}})$ at `+0x120`).
  - Constants confirmed: `contactTolerance = 0.1f` (`cinfo + 0x48`), `keepDistance = 0.05f` (`cinfo + 0x74`), `maxCharacterSpeedForSolver = 10.0f` (`cinfo + 0x78`), `layer = 14` (`Character`), and `maxSlope` from `CharacterPhysicsData + 0x44` (default $45^\circ$).
  - Up-vector derivation: In `0x1434e4400`, the initial up-vector $\hat{u}$ is calculated from world gravity as $\hat{u} = 	ext{normalize}(-ec{g}_{	ext{world}})$.
  - Support query (`0x143f60710`): Evaluates contact normal $\hat{n}$ against down-direction $\hat{d} = -\hat{u}$. Condition $\cos^2(	heta_{	ext{max}}) \le 1 - (\hat{n} \cdot \hat{d})^2 = \sin^2(lpha)$ identifies sliding slopes ($> 45^\circ$, state 2) vs walkable ground ($\le 45^\circ$, state 1); state 0 is midair/free fall.
  - The gravity trick (wall/ceiling walking): The up-vector does **not** dynamically follow gravity during simulation. Calling `set_gravity` (API 21) updates `hknpWorld + 0x4c0` but leaves `body + 0x120` and `proxy + 0xd0` pointing up $(0, 1, 0)$, causing wall contacts to evaluate as sliding/unsupported. Synchronizing `body + 0x120` and `proxy + 0xd0` with $-	ext{normalize}(ec{g}_{	ext{new}})$ allows Faith to walk on walls/ceilings.
  - Subsystems that break: Camera view matrix hardcodes world Y-up (causing inversion on ceilings and gimbal lock singularities on walls); ANT two-bone foot IK casts downward along $(0, -1, 0)$ and misses the wall; movement controller fall threshold $v_y < -5.0 	ext{ m/s}$ triggers falling state; level kill volumes assume horizontal floor planes.
  - Open for further research: Camera view matrix decoupling (`ClientCameraViewManager::updateView`), ANT locomotion ground-normal override, and trainer dynamic up-vector synchronizer in `mec_trainer.cpp`.

## UI pause controller and pause state research (2026-11-06; to be confirmed in game)
- **Pause counters & exact formula resolved**: Documented in [ui-pause-system.md](research/ui-pause-system.md) and annotated in `db/annotations/ui.yaml` (inferred; to be confirmed in game).
  - `UIPauseController_request` (`0x143b16210`) has only 3 callers in the entire binary, all in `PamClientUIPauseEntity` (`0x141cbd4a8`, events `0x0dd8ddf7` on / `0xc44b5c3c` off). The entity reads boolean `PostponePauseUntilScreenIsFadedIn` at `data + 0x18`: when true, it only increments `pauseRequestsA` (soft pause), explaining why `pauseRequestsB` stayed 0 with the Pause Menu open.
  - The "time value" in `UIPauseController_applyToClient` (`0x143b16ae0`) is the local player subview simulation timescale factor $s$ (`0x1432a8710`). The exact pause formula is $\text{isPaused} = (A \ge 1) \land (s < 10^{-6} \lor B \ge 1)$. The Pause Menu freezes timescale to $0.0$, satisfying the formula.
  - Mod pause/resume: Mods can call `UIPauseController_request(ctrl, 1, 1)` to pause immediately ($B=1$) and `request(ctrl, 0, 1)` to resume. Broadcast messages on change: `PamplonaGamePausedMessage` (`0x0dc84585` / `0xbbc34556`, bool at `+0x48`) and `ClientMetricsPauseGameMessage` (`0xd65bfe6b` / `0x3e6ab63c`).
  - Open for further research: timescale manipulation without pausing (`subview + 0x3c`, `+0x64`, `+0x8c`); input capture routing during mod pause; in-game screen counter testing.

## Ghost & Echo Runner Replay Pipeline (2026-11-06; to be confirmed in game)
- **Ghost recording, bitstream compression, and playback pipeline reversed**: Documented in [ghost-echo-replay-system.md](research/ghost-echo-replay-system.md) and annotated in `db/annotations/replay.yaml` and `messages.yaml`.
  - Recording clock: `PamClientReplayRecordingComponent_updateAll` (`0x14394dac0`) samples bone transforms at 30 Hz ($33.333\text{ ms}$) from ANT's pose buffer (`*(updater + 0x28)`) with stride 0x20 per bone into `ReplayRecorder` (`0xf0` bytes, capacity 450 frames / ~15s).
  - Bitstream Version 5: `ReplaySetup_WriteHeader` (`0x1439516b0`) embeds 32-bit DJB2 checksum keyed by bone IDs, animated translation flags, and quantization bit lengths (10 and 11).
  - Keyframe compression: `ReplayKeyframe_Write` (`0x143951450`) emits I-frames every 30 frames or when displacement $> 5.0\text{ m}$, and P-frames with 10-bit quantized delta position (clamp $\pm 5.0\text{ m}$) and 11-bit quantized quaternion. Also generates downsampled 10 Hz 3D coordinate trajectory for map HUD trails.
  - Playback & character placement: `PamClientReplayPlayerComponent` (vtable `0x141c48a28`, update `0x1439501a0`) decodes frames via `ReplayPlayback_DecodeKeyframe` (`0x14394e390`), ticks interpolation (LERP translation + SLERP quaternion) in `ReplayPlayback_Tick` (`0x1439729f0`), and places the puppet in world via `FUN_142fd8f20` using Bone 0 (root) and Bone 1 (`AITrajectory`).
  - Hologram shader & customizations: Rendered via forward shader `Shaders/SpecialUseCase/GhostProjectionVSFAsset` (`0x141c48540`, ctor `0x143944740`), `PamUseRunnersGhostShader` (`0x141c80098`), and `PamGhostInRvColorBurnPass` (`0x141c800c8`). Visual skins driven by `PamEchoCustomizationData` (`0x141c91648`, size `0x28`) and `PamClientEchoCustomizationManagerEntity` (`0x141c73c18`).
  - Network endpoints: Mapped `PamplonaAuthenticated.setRunnersRouteReplay` (`0x143a0c890`), `PamSocialPlayNamedChallengeUploadReplayMessage` (`0x955f5cbd`), `Pamplona.getPlayerGhosts` (`0x1439f98d0`), and JSON parser `0x1439b6950` (`PamEchoGhostRecord`).

## Standalone collision and touch classifier research (2026-11-06; to be confirmed in game)
- **Barrier dependence and level reload crash resolved**: Documented in [standalone-collision.md](research/standalone-collision.md) and annotated in `db/annotations/physics.yaml` (inferred; to be confirmed in game).
  - The crash in `FUN_143913840+0x5` on level reload was caused by dereferencing freed barrier data when Faith touches a box after a level reload. `SurfaceTouch_classifyBody` (`0x143913840`) looks up the body's parent from `PhysicsManager` (`0x1434ee250`) and reads traversal flags (`+0x90` Climb, `+0x91` Vault, `+0x92` Stand, `+0x93` Wallrun).
  - Designed a pinned static dummy barrier entity (`DummyBarrierEntity` at `+0x30` using native vtable `0x141c4e258` and static data) that completely eliminates barrier borrowing, removing both the 3-second load delay and the reload crash.
  - Shape creation: `PhysicsEntityFactory_createBody` (`0x143516450`) supports Box (type 1), Sphere (type 2, `0x143f6c460`), Capsule (type 3, `0x143f6b310`), and Prebuilt shapes (type 7).
  - Server world verified: `PamOBBCollision_enable` (`0x143333310`) shows `ServerGameWorld` (`GameWorlds[1]`) uses the identical factory and body creation pipeline with realm = 1.

## Sound parameters and 3D velocity research (2026-11-06; to be confirmed in game)
- **Voice `setParameter` and velocity resolved**: Documented in [sound-parameters-velocity.md](research/sound-parameters-velocity.md) and annotated in `db/annotations/audio.yaml` (inferred; to be confirmed in game).
  - Vtable slot `+0x18` (`0x14301da40`) **is** `Voice::setParameter(uint32_t key, float value)`. In Windows x64 ABI, it receives `key` in `EDX` and `value` in `XMM2`, passing them straight through to graph parameter setter `0x1430060b0` and flagging the voice dirty in `SoundManager` (`0x1423e29b8`) via `0x143018bb0`. Proven by `SoundEntity::updateProperties` (`0x14303d214`--`0x14303d231`).
  - Slot `+0x78` (`0x14301dd30`) is `Voice::clearVelocity`, zeroing the `State3D` velocity fields (`+0x40`, `+0x50`, `+0x70`).
  - Slot `+0x80` (`0x143021f60`) is `Voice::updateVelocity(const float* newPos, float dt)`, calling `State3D_update` (`0x143021cc8`) to differentiate velocity $\vec{v} = \frac{\Delta \vec{x}}{\Delta t}$ and store it at `State3D + 0x40`.
  - Direct velocity override: `*(float[4]*)(*(uintptr_t*)(voice + 0x28) + 0x40)` allows mods to directly write linear velocity vectors for 3D Doppler effect without numerical position differentiation.

## Version 1.0.0 (2026-11-06)
The user confirmed `menuOpen` works and set the version to 1.0.0 (`CMakeLists.txt`, `version.hpp`, the Rust crate, docs). Next: tag `v1.0.0` after the commit; CI builds the zips ([releasing](releasing.md)).

## Final checks before release (2026-11-06)
Committed `18664df` (API 34). Then: MinGW developer and player builds, release zips, a template mod built against the zip, Linux fixture build + ctest, Rust harness, gen tests, strict docs: all pass. Fixed a `%zu` in `sampler.cpp`'s log (MinGW's log formatting printed it wrongly). Docs: getting-started (zips, uninstall), modding (API 34, what is stable), release notes (experimental list), `docs/research/open-research.md` (twelve large pieces of work for another model). Left for the release: pick the version, tag, publish. Not committed: the docs edits and the sampler fix above.

## Pause counters and standalone boxes, first test (2026-11-06)
Log from the user: the pause line read `all 830251856, full-screen 0` in every state. Cause: `PamUISystem +0x8c8` is a **pointer** to the controller (`PamUISystem_update` loads it), Ghidra's struct and our annotation had it inline, so `ui_pause_state` read the pointer's low half. Fixed (api.cpp, ui.yaml). Standalone boxes: the log said `no usable client barrier: 578 tracked, 289 server's, 289 without data`: `data+0x78` (what `PamOBBCollision_enableClient` reads) is not an array for the client barriers; the reflection puts `PhysicsBodies` at +0x70. The setup now tries +0x70 then +0x78 and logs the data's bytes if neither works. Tested the same day: the pause counters now read right (all 0 in play, 1 with the pause menu, full-screen stays 0, so `menuOpen` is `menus > 0`), and standalone boxes are made (`made as a game physics object (id 1000637)`; the user: it seemed to work). Still to check: that they block Faith, moving, and a level change.

## Standalone boxes crash after a level reload (2026-11-06)
Log: after the level ended and loaded again, the box was remade 0.3 s after the new physics world appeared (`id 100017b`) and the game crashed in `FUN_143913840+0x5` (reading -1; called from the character's surface probe `FUN_142f450c0`, which classifies the body Faith touches through the object's owner pointers: a barrier's data flags at +0x90..0x93). Cause (most likely): the game keeps the parent pointer (the barrier's `+0x30`) of each box, and that barrier was gone or half loaded. Fixes: native boxes are forgotten on a new world and made again only 3 s after it; a destroy listener on `PamOBBCollisionEntity` destroys the boxes made from a barrier, before it goes, through the barrier's own teardown calls (they are made again from another barrier). Untested: the user retests the reload and walking through streamed areas.

## Release readiness (2026-11-06)
API 34 is the last before the first release. Added: `docs/tools.md` (every tool, how to run it), `docs/releasing.md` (checklist), root `CONTRIBUTING.md` and `SECURITY.md`, a known-issues entry for the experimental APIs. CI's checks pass locally (clippy, fmt, ruff, gen tests, `validate --strict`, `rust --check`, strict docs, MinGW build, ctest). Left: the user's test of API 34 (and the 1.9.1 pause line), then fill "Not tested in game" in the release notes, pick the version, tag.

## Havok reference checked (2026-11-06)
Another model's `hknp` reference was checked against the annotations: [research/havok-reference.md](research/havok-reference.md) lists what holds, what is wrong (the body layout mixes in the creation info; the manager chain) and what is open (character proxy, compound shapes, ray filter).

## API 34: standalone collision boxes (2026-11-06; untested in game)
From `standalone_havok_boxes_research_and_test_plan.md`, checked in Ghidra. `box_backend(1)` makes client-world boxes the way `PamOBBCollision_enableClient` (0x143333590) builds its body: `PhysicsCreationInfo_ctor(info, bus, realm 0, owner, entity+0x30)`, `PhysicsFactory_find(world+0x58, TI_PhysicsEntityFactory)`, `PhysicsEntityFactory_create(factory, &id, info, rigidData, desc)`, then `RigidBody_transformSource`, `_setTransformNative(true)`, `_enable`, `_registerSpatial`. Teardown: `_unregisterSpatial`, `_disable`, `RigidBody_destroy(owner, &id, {0,1})` (the barrier's deinit). The setup (bus +0x20, data +0x28, first RigidBodyData at `**(data+0x78)`, owner from the byte at rigid+0x2a) is borrowed from a live client barrier each flush; the report's `clientCharacter+0x28` was wrong. The box description is a 0x40 byte object with the game's vtable 0x141b41a00 (type 1 = box), kind 2, layer 1, half extents at +0x20. Skipped from the barrier's code: `FUN_1434f8980` and `FUN_1434efca0` (listener hookup), which a static box should not need. The transform lives in pinned, never freed storage (the game keeps the pointer). Risks: the parent pointer (barrier +0x30) is kept by the game and dangles if that barrier dies first; the server world still uses lent records; no fingerprints for the new functions (they need bytes from a game dump). Trainer 1.10.0, World tab, "Standalone boxes".

## Layout editor: crash when undoing a copy (2026-11-06; fix confirmed by the user)
Log: the third copy destroyed, then a crash in the same server tick, in the trainer's ground ray cast
(`FUN_1434de340` -> `PhysicsOwner_objectOfHit`: the owner's table pointer was ASCII, freed memory). Cause
(from `PhysicsWorld_flush`'s notes): the game releases a physics record only in the next flush, but
`destroyEntity` frees the physics component at once, so a query between the two reads a freed component.
Which copy crashes depends on where the ray goes (two earlier destroys were fine). Fix: after a
server entity is destroyed, server ray casts (`raycast`, `raycastObject`) answer unavailable until the
server world's next flush, a second at most (`physicsStaleNote` / `physicsQueriesSafe`, bodies.cpp). A
mod's ground check fails for one or two frames after an undo. Other server queries (the editor's own
`entities_near`, body functions) are not guarded. The destroy runs after the flush in our post-update;
doing it just before the flush, as the engine's own unload does, would be cleaner (hook point: bodies.cpp's
`flushHook`).

## MECMP compared with the API (2026-11-06)
Another model compared `D:\Dev\MECMP` with API 29. Taken: **API 31 `localize`** (0x1436a8d40). Not taken, and why:
- Dash and time trial events: `on_message` already delivers them (category 0x54776fc6; types started 0xc164590c,
  finished 0xd71d027f with the time at +0x58 ms, cancelled 0x9a9a27c6, time trial finished 0x9b35497c); a mod
  reads the fields itself (MECMP's `runs.h` has the layout). A typed wrapper is possible later.
- Map markers (`PresenceInfoManager_assignSlot` 0x143a405e0, slots of 0x50 bytes, the pool update slot
  0x141c74bc8): needs a hook on the pool update and a real online id (the icon is the Runner Tag), so it only
  suits online mods. Not exposed.
- Character spawn, outfits, bone poses, vocals, gamepad and pause hooks: large, each needs its own research and
  game testing (see `research/replay-ghosts.md`); MECMP keeps them. Pause also needs the UISystem instance.

## The ReGlitched branch (2026-11-06)
Another model read `mecmp-reglitched-physics`. Taken: **API 33 `raycast_filtered`** (the ignore list in `PhysicsRayInput`
was already annotated). Already in GridNode: the third-person body (`ClientCharacter_GetViewMode`, the 27-slot body
hook), boxes (`box_add`, which borrow a static body), sounds and rays. Checked in Ghidra and **wrong in the report**: the
voice vtable (0x141acfad0) +0x18 is not `setParameter` (it registers the voice with the sound manager) and +0x78 takes no
vector (it clears the 3D state's velocity fields), so no `sound_set_velocity` / `sound_set_parameter`. Not taken:
standalone Havok boxes (`PhysicsEntityFactory_create` 0x1435169e0, `RigidBody_transformSource` 0x1434f6d90 exist in Ghidra;
creating a body without a borrowed one is the one real gain, but a wrong call crashes the physics world: needs its own
research and an in-game test), the audio graph cloning and PCM injection (MECMP's voice chat), the conversation system
(`VoiceOverSystem_*` are annotated, not exposed).

## Second MECMP comparison (2026-11-06)
Another model went through MECMP's other files. Taken: **API 32 `ui_pause_state`** (the UI system global 0x1424157f0,
checked in Ghidra: the getter 0x143269ae0 is `mov rax, [rip+..]; ret`). Paused means requests A >= 1 and either B >= 1
or a time value below 1e-6 (`UIPauseController_applyToClient`). Not taken yet, all from MECMP and unchecked here:
- **Safe save reload**: `PamplonaRestartGameMessage` (category 0x0dc84585, type 0x04aea817, vtable 0x141c8eee0, 0x38 bytes)
  with "ResumeSaveGame" at +0x30, queued on the client manager. Ghidra shows the helpers exist (poolAlloc 0x142a770c0,
  Message_ctor 0x142a37120, MessageManager_queueMessage 0x142a3ad30); reloads the checkpoint, so it needs an in-game test.
- **Damage and death**: health component vtable slots 0x100 (apply damage) and 0xf0 (set health, death at <= 0).
- **AI targets and teams**: a `ServerPlayer` made for a character, team at +0x139c, `AIPlayerEnableAsTargetMessage`.
- **Press-F triggers**: `PamServerSpatialEntryInputTriggerEntity`, fire through trigger vtable slot 0x88.
- **Outfits and map pins**: see the first comparison above.

## API 30: streaming focus points (2026-11-06; untested in game)
- `streaming_focus_add/move/remove` (`layouts.cpp`, `focusApi*`): up to 4 points a mod owns, added to the streamers'
  focus lists ahead of the layout editor's own, merged when within 60 m, synced every 2 s and 250 ms after a move.
  Released on unload (`forgetStreamingFocus`). Needs the known streamer (`focusKnown()`).
- Trainer 1.7.1: World tab has Hazards first; the sun sliders and the streaming box sit in a closed "Look and streaming" header; the timeline buttons are gone (the API stays).
  "Load the world around the free camera": Open: does a far district really stream in,
  and how fast does it unload again?

## API 29: the visual environment and timelines (2026-11-06; untested in game)
- **`visual_env_get/set/release`**: holds values in the visual environment state (MAIN +0x50 -> +0x90 ->
  +0x1c0 -> +0x30, state +0x85470: sun, tonemap, colour correction, sky, wind, depth of field, vignette,
  film grain), written back after each server and client world update like the clock hold. Offsets
  are the cinematic tools'; only the state offset was checked in Ghidra (two functions use +0x85470). The
  update itself is still not hooked: if a held value flickers, hook it (research/visual-environment.md).
- **`timeline_time` / `timeline_set_time`**: calls the game's `Timeline_SetTime` (0x143691120, checked in
  Ghidra: time +0x98, force skips the external-time check).
- Trainer 1.6.0: World tab "Look" sliders and a timeline list / seek, for testing.
- **Left out on purpose** (the synthesis doc, 2026-11-06): class instance lists (`ClassInfo` +0x60 and the
  node at entity +0x40: TimelineEntity's constructor zeroes +0x30..0x48 after the base constructor, so
  the node is not at +0x40 there; unverified), zone streaming foci (`ZoneFocusAdd` takes two pointers,
  not one; what they are is unknown), lights, audio listeners, weather, ropes, AI. See
  "Research for another model" below. The fall threshold needs no API: `ant_live_field` already sets
  `RayCastCollisionProcessorAsset.FallThreshold`.

## Time of day hold, ANT guard, vtables (2026-11-06)
- **API 28 `time_of_day_hold`** (user: lighting is better). The cinematic tools' VisualsManager (shared 2026-11-06)
  writes the same clock as `time_of_day_set` (`[[[g]+8]+0x28]+0x30`) but rewrites it before and after the
  visual environment's update, so the lighting never reads a clock the game moved on. A hold now writes it
  back after each server and each client world update (`timeOfDayTick`, ticks.cpp); the trainer's freeze
  uses it. The visual environment update itself is not hooked (see API 29 above).
- **Body hook** takes exactly the 27-slot vtable at 0x141c3cbf0 (`research/vtables.md`); seen working in game
  (slot 15).
- **ANT live mapping** checks the layout is still what was read before running a type's loader (a crash
  in FallingDriverAsset's loader on 2026-11-06: the layout's memory had changed). When it has moved, the
  layout is looked up again through the registry (hash -> schema -> layout) and used if name and id match.
- Trainer 1.5.4: kill volumes off rescues below y -300 (was -50) and a long fall (state 0, 100 or more
  below the last ground; the game kills those itself). User: both work.
- ANT live mapping: a stale layout is looked up again through the registry (user: works).

## Free camera speed (2026-11-05)
The free camera and `camera behind` cost about 20 fps (165 to 145). Found with the sampler: the update
pass waited on the client thread, which re-ran `hookBody()` every 60 frames; it always refused
("more than 256 entries") after 257 `VirtualQuery` calls, as `vtableSlots` counted executable pages and
every page of the image is executable. Now `vtableSlots` counts pointers into PE code sections, and a
refused vtable is not scanned again. User confirmed the free camera is much better (165 fps); the
third-person body install and `camera behind` fps after the fix are untested. New: console `sample [seconds]`
(sampler.cpp: samples the three busiest threads, logs top functions and the innermost game function on
the stack; compare two runs by address in Ghidra) and console `perf on|off` (a `ui: perf` log line every 5 s per camera mode; off by default). The body hook
no longer measures the vtable (Frostbite vtables run back to back with no RTTI pointer, so a run of
code pointers never ends): it copies 512 slots; `vtableSlots` (camera only) is `countVtableSlots`, host-tested.

## Current task: getting GridNode ready for its first release (plan, 2026-11-04)
Everything up to API 22 is committed (d96c362). The plan below is in order: the "must" items before
a release, "should" before if there is time, the rest after. Open decisions for the user are marked
**(decide)**.

**Must**
1. **Stability on a clean setup.** Unzip the release zip somewhere new (not `D:\Dev\GridNode\bin`),
   inject with `gridnode_inject.exe`, then once more with `version.dll`; with no mods, then each
   example mod. Load the city, the map, a mission, a Dash, a time trial, the menus, a level change,
   quit. Reload and unload each mod a few times. Fix every crash. Quitting during a Dash
   crashed twice with GridNode injected (`0x143164d50`, a null world in `FUN_1439e08f0`'s mission
   clean-up: the game's own code, guarded by GridNode since 2026-11-05): check it in this pass.
2. **Online safety audit.** One list of everything that could reach EA's services or Beat Revival,
   checked in code: the console's online commands (confirmation; mods: refused without
   `online_allow`, enforced in `consoleRunFor` since 2026-11-04), settings marked online (mods
   warned), the trainer's upload block, progression flags marked online (set, logged: user's call,
   many are not online), nothing else sending. A short docs page "GridNode and online play" (modding.md
   has the rule now: "Online services").
3. **What the release is (decide).** Proposed: two zips. *GridNode* for players: the player build
   (no inspector, console, editor, recorder), `gridnode_inject.exe`, `version.dll`, `mods/`. *GridNode
   SDK* for modders: the developer build, `sdk/` (headers, ImGui, Rust crate, templates) and the
   example mods. `gridnode.py package` makes one zip today; add `--player`.
4. **Docs that match the release.** README (its status still says M6/M7 and lists old examples),
   getting started (unzip anywhere and inject; the game folder only for `version.dll`), shipping a
   mod (`mods/<name>/`, `gridnode_mod.py pack`), a troubleshooting page (where the log is, "build
   check failed" = another game build, what a `crash:` block means, how to report one), known issues,
   release notes.
5. **API 1.0.** From the release on the table only grows at its end, and a mod built for API N loads
   on any GridNode with API N or newer (GN_MOD returns 2 otherwise, and the log says so). Before
   freezing: review `api.h` for names and threads we would regret, and mark what is experimental in
   the docs (gravity and body velocities: the city has almost no dynamic bodies; waypoint: street
   height; checkpoint: the table's path, not understood).
6. **Version and how to release (decide).** 1.0.0, or 0.9 as a beta first. A tag, a GitHub release
   from CI's zips, the release notes.

**Should**
7. **The example mods in the release.** Players get the MEC trainer (and hello?). World draw, probe
   and the detour demo are test harnesses: source in the SDK zip only.
8. **The "untested" list.** 29 notes in this file say untested: test, mark experimental, or take out.
9. **A friend's PC.** One install by someone else from the zip and the docs alone.
10. **Cost.** Startup (about 2 s, 2,164 hooks) and frame time with and without GridNode, written down.

**After the release:** spawnable physics objects, the layout editor (paused on purpose), a scripting layer only if
mods ask for one (Lua was skipped on 2026-10-20, PLAN.md), the 335 `hypothesis` functions.

## Research for another model (Ghidra, 2026-11-06)
Each is self-contained: the answer goes into `db/annotations/` and a research page, with the addresses
checked in the decompiler. Claims from the cinematic tools' synthesis are leads, not facts.
1. ~~Class instance lists~~ answered by another model (2026-11-06), partly checked: only spatial entities
   (SpatialEntity and below) have the node at entity +0x40 and a ClassInfo list; `Entity` is 0x30 bytes
   and `TimelineEntity` derives from it directly (no node, its +0x30..0x50 are property connections),
   so its ClassInfo +0x60..+0x78 are 0. A class walk would work for spatial classes only; the list
   head, the node's prev / next order and the end of a walk are still unchecked. `entities_of_class`
   (GridNode's own table of created entities) already finds timelines.
2. ~~ZoneFocusAdd / Remove~~ answered, and already in `research/zone-streaming.md` (2026-10-21): `(focus
   manager = streamer +0x280, key = 0, focus interface)`; the interface's slot 0 `(self, key, out)` writes
   position and direction. The caller bytes in `ZoneStreamerFocusEntity`'s Enable handler were re-read
   2026-11-06 and match. The layout editor keeps 8 focus objects of its own; exposing them as an API
   (`streaming_focus_*`) is the open step.
3. ~~The visual environment update~~ answered by another model, **checked in Ghidra 2026-11-06**: the blend is 0x143703d60,
   called per active context from 0x143705b30 (under the VE mutex); the per-frame hook point is slot 21
   (0x141BFACA8) of the world renderer's vtable (0x141BFAC00), on the render thread, with `worldRenderer +
   0x85470` as the destination. Writing there, before and after the blend, would replace the post-update
   write-back (no flicker). Checked: slot 21 holds 0x14373e5d0, which passes `worldRenderer + 0x85470` to the blend and other calls
   (six uses); 0x143705b30 locks the mutex and loops over the contexts at +0x2358 / +0x2360, as said. Thread not checked.
4. ~~Dynamic lights~~ answered, **partly checked in Ghidra**: creator 0x14375e000 `(ctx, ...)` (allocates 0xf0, constructor
   0x14375f020(mem, ctx + 0x10, *(ctx + 0xd0)), links with 0x14375c4a0; the light data comes from the context,
   not a parameter; its one caller is 0x143766ec0). Vtables are PBRSphereLightEntity / PBRSpotLightEntity.
   update 0x14376a030 sets the dirty byte at entity +0xa2 and queues the entity to the light
   manager (0x14256a3e0): checked. Colour, radius and the other fields: unchecked. Fields: flags +0x80, colour and intensity +0xd8..0xe0, radius and attenuation +0x220.
   Not enough for a `light_create` yet: the light entity data and the world to add it to are missing.
5. ~~Audio listeners~~ answered, **checked in Ghidra 2026-11-06, all as said**: sound manager at 0x1423e29b8 (listener array +0x30 / +0x38);
   add `(mgr, sourceEntity, viewId)` 0x143018a10 (listener object 0x80, entity +0x70, view +0x78, no-op if
   present); remove `(mgr, sourceEntity)` 0x14301d4b0; nearest-listener 0x142fe50b0 takes the listeners'
   transforms via vtable slot 1. Listeners are matched by the entity their vtable slot 6 returns. A second listener on the free camera is
   possible (an entity with a transform is needed). Not built.
6. ~~The long-fall kill~~ answered, **unchecked**: state 0 with `last_ground_y - y >= 100` (or the air time
   running out) starts the death. Cleaner than the trainer's teleport: keep `last_ground_y` at her y while in
   state 0 (`GN_MOVE_LAST_GROUND_Y`). The air-time limit is not found, so this may only cover the distance.
   Not built: the teleport works (confirmed).
7. ~~Swarms and traffic~~ **checked in Ghidra 2026-11-06; the other model had parts wrong.** The class is
   `CrowdPlacementBoidsEntity` (entity +0x28 bus, +0x30 its data); its per-frame step is 0x143b9b7e0 (called
   from 0x143b9bc10 with the crowd component and its dt). In it:
   - **Agent buffer** (component +0x70 begin, +0x78 end): two `float4` per agent, 0x20 bytes, indexed by
     entity +0x44 (first slot) + i, count at entity +0x48. The *even* float4 is the previous position, the
     *odd* one the current (the other model had it the other way). The step copies odd to even, then writes the
     new position from the source record to odd.
   - **Source records**: 0x24 bytes each, from the swarm manager (entity +0x98), index entity +0xa0 + i:
     position x, y, z at +0x00, heading as a half-float in degrees at +0x0c. `y <= -10042` means the agent is
     not spawned (both float4s are set to its position).
   - **Render buffer**: one `float4` per agent (slot entity +0x44 + i): previous position and a half-float
     heading in radians in the last 16 bits.
   - **Density**: a `Float32` property (hash 0xbdd8feb6, its default is the data's +0x78) read every step as a
     percentage; the spawner's reserve (spawner +0x34) is set to `count * (100 - value) / 100`, clamped 0..1,
     and the respawn (0x143b9cbe0) pops a free agent (the pool at spawner +0x40 / +0x48) only while the pool
     is bigger than the reserve. So 100 = every agent active, 0 = none spawn. Writing the data's +0x78 should
     therefore be a density control (untested; it needs a `CrowdPlacementBoidsEntity`'s data from
     `entities_of_class`). The respawn also needs the time at the context +0x14 to pass the spawner's
     +0x30 and rolls a chance per spawn point (0x10-byte records at spawner +0x60 / +0x68).
   - Still unchecked: the agent cap (`SwarmEntityData +0x84`), speed (the behaviour classes 0x141CCEF20 /
     0x141CCEED8 / 0x141CCEEA0 / 0x141CCEE60 and settings 0x141CD2CA8 are only vtables here), and how many
     of these entities a level has.
- **Dash and time trial events (2026-11-06, Option 8)**: Reversing completed in `docs/research/dash-and-time-trial-events.md` (annotations in `db/annotations/messages.yaml` and `ui.yaml`). Mapped all 35 messages of category `0x54776fc6` (`kCategorySocialPlay`) with constructors and vtables. Decoded `PamSocialPlayNamedChallengeFinishedMessage` (`0xd71d027f`, constructor `0x144733a30`) payload (`+0x48 ContentId`, `+0x4c Index`, `+0x50 Delta`, `+0x54 HadRecord`, `+0x58 Value`, `+0x60 NewRecord`) and run time formula `runCs = (newRecord || !hadRecord) ? val : val + delta`. Decoded UGC time trial finish `PamSocialPlayChallengeFinishedMessage` (`0x9b35497c`, constructor `0x144733550`) and separated timed runs from Beat L.E. / ReachThis via `CheckpointOrder != 0`. Mapped metadata resolution (`PamServerNamedChallengeEntity` vtable `0x141c7ec70`, `PamServerChallengeMarkerEntity` vtable `0x141c7e3e8`) and string localization via `localize` (`0x1436a8d40`). Designed typed C++ event interface (`onDash`, `onTimeTrial`). Pure research to be confirmed in game.

- **Free camera streaming & zone streamer focus points (2026-11-06, Option 6)**: Reversing completed in `docs/research/freecam-streaming-and-focus-points.md` (annotations in `db/annotations/levels.yaml`). Proved that the vanilla game does NOT stream around the free camera (`ZoneStreamerFocus_add` at `0x143c7e7c0` is called solely by `ZoneStreamerFocusEntity` parented to Faith; the camera subsystem has zero focus integration). Concluded API 30 (`streamingFocus*`) is essential to prevent distant districts unloading during freecam flight or remote layout placement. Explained why trainer tests showed nothing (masked by Faith's 300--400m local streaming radius, while distant areas require active district `ZoneStreamerEntity` and `SubLevelEntity`). Confirmed focus manager layout (`streamer + 0x280`, `Records` at `+0x08`, `Positions` at `+0x2a8`, `FocusCount` at `+0x2c8`, `GridData` at `+0x2d8`), position evaluation (`0x143c81850`), spatial AABB cell containment `ZoneStreamer_findZone` (`0x143c7fc80`), zone load `0x143c810d0`, and zone unload `0x143c81240`. Pure research to be confirmed in game.

8. The 377 `hypothesis` functions (the count was 335 above; the other model counted 377): resources 98, render 64,
   levels 47, reflection 36, startup 32, input 20, 13 other files 80. Still to check, area by area.

From item 2 (checked): the game's `ZoneStreamerFocus_remove` writes out of bounds when the record is missing (index 0xffffffff). GridNode never
calls it: `focusApply` sets the removed flag (bit 0) on a record it found first.

## Done earlier
Everything before 2026-11-06 (the Volumes tab, what was checked in game 2026-11-02, the long list of
additions up to API 27) is in [progress-archive.md](progress-archive.md).

## Older next steps (research; after the release plan above)
1. The 335 `hypothesis` functions left (all have signatures now): read them one by one, area by area
   (resources, render, levels, reflection, startup first).
2. In game: the class picker, and every-frame watches (their cost shows as *last reading*; does a
   ~0.1 s ghost now show in the server list?).
3. Check `gn_movement.speed`, now the length of her velocity at movement ground +0xd0 (x, z at
   +0xd0, +0xd8): `speed (game)` should follow `speed (measured)`. The speed probe matched it in
   82-86% of samples in two runs (`speed-3`, `speed-4`); the old trainer path
   ([[[0x1423e29b8] + 0x2378] + 0x10] + 0x438, a Cheat Engine path past the end of its object;
   properly slot +0x1e8 of the object at 0x1423dd6f8) 77%; our old path (Faith +0x28 -> +0x1f0 ->
   +0x10 -> +0x438) never (+0x1f0 held garbage or a MathEntity). The first pick, movement state
   +0x1bc, matched in `speed-3` only and read 0 throughout `speed-4`: one run is not enough. Writing
   the speed (the trainer's position load) did not keep it with +0x1bc; it now scales her velocity
   and writes the old trainer's field too (untested).
Later: layout editor (occluders in the overlay, copying a whole group prop, confirm magrope `Target`).
Later (user, 2026-10-30): the trainer spawning volumes such as triggers. Leads found in Ghidra, not
looked into: `HavokVisualDebugger` and `HavokCaptureToFile` are reflected settings fields in two
settings classes (one also has `SavePoint`, `DisableCutscenes`, `ShowTriggerDebugText`); the Havok
Visual Debugger is Havok 2013.3's (path in the exe), which streams to Havok's own viewer app over the
network rather than drawing in game. Also present: `PhysicsDebugSettings`, `DebugRenderSettings`,
`EnableDebugRendering`, `DebugRenderer`, `hkpTriggerVolume`, `WithinPlayerTriggerVolumeCondition`.
`ShowTriggerDebugText` and the debug-render settings may show triggers in game, if retail kept the
renderer: try them in the console first. Our own `gn::ui::box3d` can draw trigger boxes either way.

## Decisions and gotchas
- Patching game code is allowed since 2026-11-01 (prefer pointer swaps; everything undoable through GridNode).
  Online services (user, 2026-11-04): mods may change them on purpose after `online_allow` (warned,
  logged, shown in the Mods list); the trainer never does, and keeps its upload block. PC only.
- Never commit game data or user dumps (dumps, FBX game meshes, event-names.txt, logs, recordings).
- Commit / push only when asked. Deploy test builds under a unique file name first.
- A fingerprint's bytes come from a dump, never from a disassembly listing (one instruction has
  several encodings; a guess turned MECMP's own spawner off on the right game).
- The game finds spawners by their data in a bus: a second spawner with the same data and bus (MECMP's
  "own spawner") is found instead of the game's one. Keep the game's character last instead.
- The u32 at a character's bus +0x58 is the same for every ghost of a spawner (low bits of the
  spawner's data pointer): not a per-character id.
- A dump made with hooks in place shows hook pointers in the slots; the dump button restores them,
  and the checker reports a pointer outside the game as "hooked when dumped".
- The user's local checkout `D:\Dev\GridNode` is far behind `main` with uncommitted annotation edits
  from another agent (most wrong; the repo already has the corrected versions). Steps to clean it up
  were given 2026-10-22 (back up, `git restore`, remove the 4 untracked YAML files, pull).
