# Custom meshes (with Frosty)

How to get a mesh you made into the game and place it with the layout editor. Frosty makes and
packages the mesh; GridNode places it. Catalyst has one level (the city) split into sublevels, and
the library already streams any of them in, so a mesh in any city sublevel can be placed anywhere.

There are two phases. **Phase 1 works with GridNode as it is** and is the one to try first.
Phase 2 needs a GridNode feature that is not built yet.

You need Frosty Editor and Frosty Mod Manager with the Mirror's Edge Catalyst profile, and Blender
(or any tool that writes FBX). Frosty's licence (CC BY-NC-ND) means GridNode only tells you how to
use it; it does not include or change it.

The Frosty steps below are the usual workflow, but menu names differ between Frosty versions: where
a step says "the mesh editor's Import", look for the equivalent in yours.

## Phase 1: replace a prop's mesh

Every copy of one prop in the city becomes your mesh. Pick a prop that is not used much.

### 1. Choose the prop

In game, with the layout editor, pick a small prop that appears in only a few places (a crate, a
sign) and press **B** to add it to the library. Hover the entry: the tooltip shows its mesh's name,
for example `objects/props/.../crate_01_Mesh`. Note it.

### 2. Export it from Frosty

1. Open Frosty Editor with the Catalyst profile.
2. In the Data Explorer, find the mesh by the name you noted (the path, without `_Mesh` at the end
   if it is not found with it).
3. Open it and use the mesh editor's **Export** to write an FBX.

Starting from the game's own export keeps the structure Frosty expects on import: the LOD nodes and
the mesh sections (one per material), with their names.

### 3. Make your mesh in Blender

1. Import the FBX.
2. Replace the geometry inside each LOD's section with yours, **keeping the objects' names and the
   hierarchy**. The simplest first test is a cube in place of LOD0's section, with the other LODs
   copies of it.
3. Keep one section per original section: each takes that section's material in game.
4. Export as FBX with the same settings (scale 1, the axes as imported).

Frostbite is Y-up and right-handed. If the mesh comes in turned or mirrored, fix the export axes, not
the geometry.

### 4. Import it and make a mod

1. In Frosty Editor, open the same mesh and use **Import** with your FBX.
2. Check it in the mesh preview.
3. **File > Export mod** (`.fbmod`), with a name and version.

### 5. Play with it

1. Add the mod in Frosty Mod Manager and launch the game from there.
2. Inject GridNode as usual.
3. Go to one of the places the prop is: it should be your mesh.
4. Place it elsewhere from the library (the entry you added in step 1 still works: it is found by
   the mesh's name), copy it, and save it in a layout.

**Collision stays the original prop's.** The mesh is only what is drawn; the collision comes from
the prop's physics data, which this does not change. Pick a prop whose shape is close to your mesh,
or accept that collision will not match for now.

If the game crashes on load, the usual cause is a section or LOD missing or renamed: re-export the
original and compare the hierarchy with yours.

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
