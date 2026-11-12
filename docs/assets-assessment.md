# Assets: what Frosty covers, and what GridNode should build

M6 assessment for part 3 (assets), October 2026. Goal: **custom maps for Catalyst made in Blender**.
Frosty source checked: `CadeEvs/FrostyToolsuite`, branches `1.0.6.3` (last commit September 2023),
`1.0.7` (February 2023) and `LevelEditor` (September 2022).

## Frosty and Catalyst

Frosty supports Catalyst as a full profile (`ProfileVersion.MirrorsEdgeCatalyst`, SDK
`MirrorsEdgeSDK`, standard asset loader, sources Patch / Update / Data):

| Frosty can | For Catalyst |
|---|---|
| Browse and edit EBX assets in a GUI | yes |
| Export / import meshes (MeshSet plugin) | yes, through **FBX** (Autodesk FBX SDK); Catalyst-specific paths for skinned meshes (`Skeleton_Female`) |
| Textures, sounds, localisation, bundles | yes (plugins) |
| Export EBX as XML (`EbxToXml` plugin) | yes, one way |
| Package changes as a mod (`.fbmod` + archives) and load them (Mod Manager) | yes: the Mod Manager rebuilds patched data at launch |
| Dump the shader database (FrostyCmd) | yes, with Catalyst cases |
| Edit levels | **no** for Catalyst: the `LevelEditor` branch is an unfinished 2022 prototype written for Mass Effect Andromeda |

## The licence

Frosty is licensed **CC BY-NC-ND 4.0** (README): non-commercial, and **no derivatives**. GridNode is
MIT and must not copy, port or adapt Frosty's code, and cannot ship a modified Frosty. GridNode can:

- tell users to use Frosty, as a separate tool, for what it already does well;
- produce files Frosty (or its Mod Manager) reads, written from our own understanding of the formats.

Format work in GridNode may use what Frosty's source shows about the formats (the user dropped the
clean-room rule on 2026-11-11). The licence still forbids copying, porting or adapting Frosty's code:
GridNode's readers are written from format descriptions (field orders, sizes, magic values), checked
against the game's own files, and from the game itself (GridNode's runtime reflection gives exact
layouts for every EBX type, which Frosty gets from a prebuilt SDK DLL).

## What is missing for Blender → custom maps

1. **No level editing** for Catalyst: placing, moving and wiring entities in a level.
2. **No Blender path**: meshes go through FBX only; no glTF, no add-on, and nothing for level layout
   (where objects are, what they are).
3. **No lossless EBX text format**: XML export is one way; edits have to be made in the GUI, so they
   cannot be diffed, reviewed or scripted.
4. **No headless batch export / import** for assets (FrostyCmd only creates profiles and dumps shaders).
5. **No live view**: changes are seen after packaging a mod and restarting the game.

## What GridNode has that Frosty does not

- It runs **inside the game**: entity creation, transforms, teleport, the game's own blueprints,
  the overlay, the inspector. Changes can be made and seen live.
- **Exact reflection** for every engine type from the user's own game, already used for C++ headers,
  Rust bindings and the docs.

## Recommendation for M7

Keep Frosty for meshes, textures and packaging. GridNode builds, in this order:

1. **Live layout editing in game (overlay).** Pick an entity, move / rotate it, spawn a blueprint at
   the camera, delete; save the result as a layout file (YAML: blueprint, transform, overrides). This
   is the core of map making and only GridNode can do it live.
2. **Layout files in game.** GridNode applies a layout file when the level loads (spawning the
   game's own blueprints through its creation path), so a custom map is a mod GridNode loads: no
   repacking of game data for layout changes.
3. **Blender add-on (glTF).** Export a level's layout (and a GridNode layout file) to glTF with
   placeholder meshes and the entity data in `extras`; import it back as a layout file. Real meshes
   come from Frosty's FBX export when wanted.
4. **EBX ⇄ YAML** (later): read the EBX format from our own reversing with GridNode's reflection
   for layouts, for diffable asset edits and for new assets Frosty then packages.

**Decision for you:** whether custom maps load their layout at runtime through GridNode (point 2:
no repacking, needs GridNode), or are packaged into game data (`.fbmod`-style: works without
GridNode, but needs the EBX writer first and a Frosty-compatible package). The recommendation is
runtime first: it needs no format writer and gives the live loop.
