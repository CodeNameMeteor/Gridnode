# Custom meshes (with Frosty)

How to get a mesh you made into the game and place it with the layout editor. Frosty makes and
packages the mesh; GridNode places it. Catalyst has one level (the city) split into sublevels, and
the library already streams any of them in, so a mesh in any city sublevel can be placed anywhere.

There are two phases. **Phase 1 works with GridNode as it is** and is the one to try first.
Phase 2 needs a GridNode feature that is not built yet.

You need Frosty Editor and Frosty Mod Manager with the Mirror's Edge Catalyst profile, and Blender
(or any tool that writes FBX). Frosty's licence (CC BY-NC-ND) means GridNode only tells you how to
use it; it does not include or change it.

Confirmed on 2026-10-22: a crate scaled three times, made this way, imported in Frosty and showed
in game at the new size (with the original collision).

## Phase 1: replace a prop's mesh

Every copy of one prop in the city becomes your mesh. Pick a prop that is not used much.

### 1. Choose the prop

In game, with the layout editor, pick a small prop that appears in only a few places (a crate, a
sign) and press **B** to add it to the library. Hover the entry: the tooltip shows its mesh's name,
for example `objects/props/.../crate_01_Mesh`. Note it.

### 2. Export it from Frosty

1. Open Frosty Editor with the Catalyst profile.
2. In the Data Explorer, find the mesh by the name you noted (it ends in `_Mesh`).
3. **Double-click it** to open its editor (a 3D preview with a toolbar above it), and use that
   toolbar's **Export** to write an FBX. Keep this file: it is the *template* below.

Use the mesh editor's own **Export** and **Import** buttons. The Import in the Data Explorer's
right-click menu is a different command (it asks for `.bin` / `.res` files).

### 3. Make your mesh in Blender

Start from an empty scene, so Blender's default cube, camera and light do not end up in the file:
**File > New > General**, then press **A**, **X**, **Delete**.

1. **File > Import > FBX**, the file from step 2. It comes in as one empty per LOD
   (`<mesh>_lod0`, `_lod1`, ...) with the mesh's sections inside. Blender renames sections that
   share a name (`Crate`, `Crate.001`, ...); that is fine, the tool below puts the names back.
2. Change the meshes as you like: edit them, or replace them with your own shapes. Scale or turn
   whole objects if you want; the tool applies that to the vertices. Keep a UV map on each mesh.
3. **File > Export > FBX**, with **Geometry > Tangent Space** and **Triangulate Faces** ticked
   (Frosty refuses a mesh without tangents: "Mesh must be exported with tangents/binormals").

A first test that needs no modelling: select only the LOD empties (not the meshes), press **S**,
**3**, **Enter** to make the mesh three times bigger, and export.

### 4. Make it Frosty's again, then import it

Blender's file is not laid out the way Frosty reads it (renamed sections, transforms instead of
vertices, values per polygon corner): imported as it is, the mesh is invisible. GridNode's tool puts
your geometry into Frosty's own export, keeping its names and structure:

```
python tools/mesh/frosty_fbx.py <the template from step 2>.fbx <your Blender export>.fbx
```

It writes `<your Blender export>_frosty.fbx` and says, per LOD, what it took from your file, and
the size of yours against the original. If it says your mesh was moved and you did not mean to
move it (an easy accident when scaling several objects at once), run it again with `--no-move`.
If it cannot match a section to an object, name the object to use with `--mesh NAME`.

Then, in Frosty, open the same mesh, use its toolbar's **Import** with the `_frosty.fbx` file, and
check the preview. **File > Export Mod** (`.fbmod`), with a name and version.

### 5. Play with it

1. Add the mod in Frosty Mod Manager and launch the game from there.
2. Inject GridNode as usual.
3. Go to one of the places the prop is: it should be your mesh.
4. Place it elsewhere from the library (the entry you added in step 1 still works: it is found by
   the mesh's name), copy it, and save it in a layout.

**Collision stays the original prop's.** The mesh is only what is drawn; the collision comes from
the prop's physics data, which this does not change. Pick a prop whose shape is close to your mesh,
or accept that collision will not match for now.

If the game crashes on load, or the mesh is invisible in Frosty's preview, run the tool again and
read its notes; a Blender export imported without the tool is invisible.

## Phase 2: a new mesh alongside the originals (needs GridNode work)

To keep the original prop and add your mesh as something new:

1. In Frosty, **duplicate** the original mesh asset under a new name (Frosty's duplicate command in
   the Data Explorer, where your version has it), import your FBX into the duplicate, and make sure
   it is in the same bundle (sublevel) as the original.
2. Nothing in the level uses the new mesh, so the game never draws it by itself. **GridNode will
   need to place a mesh by name:** find the loaded MeshAsset by its name, copy an existing static
   model's data in memory with its mesh pointed at yours, and spawn that. This is planned (see
   [the plan](PLAN.md), M7, custom meshes) and not built yet.

Phase 1 proves the Frosty side (that your mesh imports, loads and draws). Phase 2 then needs only
the GridNode side.

## What to send back after a test

- the mesh name you replaced, and whether the game loaded with the mod;
- `gridnode.log`, if anything about placing it went wrong;
- a screenshot of it in game.
