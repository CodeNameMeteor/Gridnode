# Layout editor

The layout editor moves, turns and copies objects in the running game, and saves what you did as
a *layout* that is applied again when the level loads. It is a developer tool: it is in the
developer build of GridNode and not in the player build (see [Shipping a mod](shipping.md)).

Open the GridNode menu (Insert) and choose **Layout editor**.

!!! note "Not finished"
    What is here works as described, but the editor is not finished.

## Free camera

**F7** (or the editor's *Free camera* box) detaches the camera from Faith. With the menu closed
the camera flies and the game gets no input, so Faith stays where she is and shows her
third-person body. With the menu open the camera keeps its place: hold the right mouse button to
look, and left-click objects to pick them along the cursor.

| Key | While flying |
|---|---|
| Mouse | look |
| W A S D | fly |
| Q / E | down / up |
| Shift / Ctrl | faster / slower |
| Wheel | flying speed |
| F7 | back to Faith |

The keys listed on screen (top left) are the ones that work while the camera flies.

## Picking and the outline

Click, or press **R**, to pick what the crosshair points at. The selection gets an outline: a box
around the model as it is drawn, with its axes (X red, Y green, Z blue) from the middle of the
box. The small dot is the object's pivot, the point it turns about; on many models it is a
corner. The outline shows only while the free camera is on.

**Shift + click** (or Shift + R) adds an object to the selection, or takes it out if it is
already in. The first object picked leads: it has the yellow outline and the axes, and the rest
(light blue) move and turn with it, about its pivot, as if fixed to it. A plain click replaces
the selection; clicking empty sky clears it.

## Moving and turning

Directions follow the camera: "right" is the camera's right along the ground.

| Key | Does |
|---|---|
| Arrows | move away / toward / left / right |
| Page Up / Page Down | move up / down |
| Z / X | turn left / right |
| N / M | tilt left / right |
| K / L | tip away / toward |
| H | flip upside down |
| U | stand upright again |
| Ctrl + Z | undo (also with the menu open) |
| Tab | mouse: look / pointer (for the handles) |
| Y | handles: move / rotate |

Shift makes a step 4 times larger, Ctrl a quarter. The step sizes are in the editor. Holding a key
repeats it; one hold is one undo step.

### Handles

The leading selection has handles from the middle of its box: three **arrows** to move it along
an axis, or three **rings** to turn it about one. **Y** switches between them, and the editor has
the same choice, plus whether the handles follow the object's own axes or the world's. Drag one
with the left mouse button; hold **Ctrl** to snap to the step sizes. A drag is one undo step, and
the rest of a multiple selection follows.

The handles need a cursor. With the menu open there is one. While flying with the menu closed,
**Tab** turns the mouse into a pointer (the right button still looks, and a click picks what is
under the cursor); Tab again goes back to looking.

Most props in a level are *group props*: copies the game draws as one batch with the rest of
their group. They cannot be moved one by one, so the first time you move one (a handle, or a move
key) GridNode makes it *standalone*: the group's copy is hidden and a standalone copy takes its
place, selected as before. A handle drag goes on once that is done; with a key, press again.
**Make standalone** in the editor does the same without moving it.

**Move group props in place (experimental)**, in the editor, moves a group prop itself instead: its
collision, the object that culls it, and its mesh streaming position. It is a test of whether the
game then draws it where it was moved; such moves are not saved in the layout yet.

## Copies

| Key | Does |
|---|---|
| C | copy the selection |
| V | paste at the crosshair |
| G | duplicate in place |

A copy is a new object made from the original's data. Copies are saved with the layout. With
several objects selected, Copy takes them all, and Paste puts the leading one at the crosshair and
the rest where they were from it. New copies are selected, ready to move. **Remove this copy** in
the editor destroys a copy (undo makes it again from the layout).

A copy belongs to the part of the level its original comes from. When that part streams out (you
fly or teleport far away), the copy goes with it, and GridNode places it again when the part
loads.

## Barriers and magropes

Under **Barriers and magropes (research)**, **Show barriers and magropes** draws the ones near the
camera (the range slider sets how far):

- **barriers** (invisible walls and "can't" zones) as orange boxes, grey when switched off; the list
  gives each one's size and flags (can't stand, climb, vault, wallrun);
- **magropes** as cyan circles labelled Swing, PullUp or PullDown, with an arrow toward the side they
  are used from, their own axes (red right, green up, blue forward) and their triggers (green: where
  Faith has to be). A moved magrope's old triggers show in grey.

Click one in the world or in a list to select it (yellow). Then, while flying, the move and turn keys
move it, or use its position field (typed values apply on Enter) and the turn buttons. **Copy** makes
a copy a metre and a half to the camera's right; select it and move it. **Remove this copy** and
**Put back** undo either. Moves and copies are saved with the layout and come back when it loads, and
when their part of the city streams back in. Ctrl+Z does not cover them yet.

## Library

The library keeps objects you like, by name, across sessions and levels (`layouts/library.txt`).
Select one or more objects and press **B** (or **Add the selection** under *Library* in the
editor, with an optional name); a multiple selection becomes one entry that keeps how its objects
sit together. **Place** puts the entry where the crosshair points, as copies (saved in the layout
like any copy).

An entry remembers each object's *mesh*, not the object itself, and where its source is: GridNode
copies any loaded object drawn with that mesh. The city loads in zones around Faith; placing an
entry from far away asks the game to load the zone around its source first (the status line says
so; it can take a few seconds). The zones holding the sources of your copies are kept loaded while
the copies are within 400 m of Faith or the free camera (up to 8 places), so copies stay when Faith
moves about near them, and their sources load again when she comes back to them.
Entries added before 2026-10-21 evening have no source position: remove and add them again. Catalyst has one level (the city), so an entry can be placed anywhere in it.

Your own meshes can be placed the same way once they are in the game: see
[Custom meshes](custom-meshes.md).

## Prefabs from any part of the level (experimental)

Under **Prefabs from any part of the level**, any prefab of the city can be placed wherever you are: a
magrope from Anchor while you stand in Downtown. GridNode loads the part of the level it comes from (and
that part's parents) without the game streaming it in around you, and spawns the prefab with its logic
running, so a magrope works.

First make the list once (it reads your game files; it takes a few minutes):

```text
python tools/gen/gridnode.py assets bundle-parents --data "<the game's Data folder>"
```

It writes `assets/prefabs.txt` and `assets/bundle-parents.txt` next to gridnode.dll. Type part of a name in
**Find**, pick one (or type an asset and its bundle yourself), and press **Place at the crosshair**. The list
of placed prefabs shows what each is doing (loading its bundle, placed), or **Remove** it. They are saved
with the layout and come back when it loads, and after a checkpoint or level reload. A prefab whose logic
expects links from the level (a dash's start and finish) does nothing on its own yet, and one that holds a
spawner (a character) is refused: its client half needs the server's data.

## Movement blockers (experimental)

Under **Movement blockers**, set a size and what Faith can't do on it (stand, climb, vault, wallrun), and
press **Place a blocker at the crosshair**: an invisible wall stands on what the crosshair points at. GridNode
makes it (it needs one barrier of the level loaded to copy its physics settings from). Saved with the layout.

## Moving placed prefabs and blockers

Placed prefabs (blue boxes) and blockers (pink boxes, their real size) show while you fly the free camera. A new
one is selected at once; click one in the world while flying, or its row in the list, to select it (yellow). The move keys
then move and turn it, as any object (Shift: 4 steps, Ctrl: a quarter). It is drawn at the new place at once
and spawned again there when the keys rest. The selected one's row also takes a typed position and turn, and
a blocker's size and flags. Undo does not cover them yet.

## Teleport

**T** teleports Faith to where the crosshair points, facing the way the camera looks, and leaves
the free camera where it is: you keep flying, and F7 goes back to her when you want.
**Shift + T** teleports her and goes straight back to her view. Going back to her view as she
teleports makes the game reload the area around her, which takes copies placed there with it, so
T is the safe one. The editor also saves named teleport points per level
(`layouts/<level>/teleports.txt`).

## Layouts

A layout is a file, `layouts/<level>/<name>.yaml` next to gridnode.dll. The editor saves, loads,
deletes and unloads them; **Unload** puts the level back as the game has it. With *Load the last
used layout when a level starts* on, the layout you last used for a level comes back when it
loads.

## Keys

Every key can be changed in the editor under **Keys** (saved in `layouts/settings.txt`). Escape
while choosing a key leaves the action without one; Backspace sets the default.
