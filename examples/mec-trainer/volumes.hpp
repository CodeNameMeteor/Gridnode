#pragma once
// Trigger volumes for the trainer: the level's area triggers drawn in the world, sorted into
// categories (cutscene, checkpoint, ...) by what their events reach, each category on or off.

namespace volumes {

void loadSettings();
void clientTick(float dt); // reads the game (client thread)
void draw();               // the shapes (render thread, every frame)
void tab();                // the Volumes tab (render thread)
void levelChanged();       // forget what was read (GridNode's thread)
bool anyShown();           // for the trainer's on-screen list
void unload();             // the trainer is being unloaded: nothing more is read
void logAimed();           // the trigger in the middle of the screen to gridnode.log (any thread; while picking)
void togglePick();         // picking (that trigger lit up) on / off
void toggleShow();         // the volumes shown / hidden (any thread)

} // namespace volumes
