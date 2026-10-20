# Layout editor

The layout editor moves, turns and copies objects in the running game, and saves what you did as
a *layout* that is applied again when the level loads. It is a developer tool: it is in the
developer build of GridNode and not in the player build (see [Shipping a mod](shipping.md)).

Open the GridNode menu (Insert) and choose **Layout editor**.

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

Shift makes a step 4 times larger, Ctrl a quarter. The step sizes are in the editor. Holding a key
repeats it; one hold is one undo step.

Most props in a level are *group props*: copies the game draws as one batch with the rest of
their group. They cannot be moved one by one. Select one and press **Make standalone** in the
editor: the group's copy is hidden and a standalone copy takes its place, which moves like any
object.

## Copies

| Key | Does |
|---|---|
| C | copy the selection |
| V | paste at the crosshair |
| G | duplicate in place |

A copy is a new object made from the original's data. Copies are saved with the layout. With
several objects selected, Copy takes them all, and Paste puts the leading one at the crosshair and
the rest where they were from it. New copies are selected, ready to move. **Remove this copy** in
the editor takes a copy away.

A copy belongs to the part of the level its original comes from. When that part streams out (you
fly or teleport far away), the copy goes with it, and GridNode places it again when the part
loads. A library of objects you can place anywhere is planned.

## Teleport

**T** teleports Faith to where the crosshair points, facing the way the camera looks, and goes
back to her view. The editor also saves named teleport points per level
(`layouts/<level>/teleports.txt`).

## Layouts

A layout is a file, `layouts/<level>/<name>.yaml` next to gridnode.dll. The editor saves, loads,
deletes and unloads them; **Unload** puts the level back as the game has it. With *Load the last
used layout when a level starts* on, the layout you last used for a level comes back when it
loads.

## Keys

Every key can be changed in the editor under **Keys** (saved in `layouts/settings.txt`). Escape
while choosing a key leaves the action without one; Backspace sets the default.
