#pragma once
// Landing prediction for the trainer: where a jump or a fall from any height comes down, drawn in the world
// (the arc, a ring on the ground, its coordinates) and on the HUD. For "skydiving" (a fall from far up: is
// she high enough, where does she land) and speedrun setups.
//
// The game's own prediction (gn::landingPrediction) only looks a fraction of a second ahead, so this one
// simulates the arc: her velocity from her last positions, gravity and her top fall speed measured from her
// own falls (saved, so the next run starts with them), and a sphere swept along the arc with the game's ray
// cast for the first thing it meets.

#include "gridnode/mod.hpp"

namespace landing {

void loadSettings();
void saveSettings();
void clientTick(const gn::Movement& m, bool flying); // client thread: in the air or not, the fall's start height
void serverTick(void* faith, float dt);             // server thread: samples, the arc, the ray casts
void draw();                                        // render thread, every frame: the arc, the ring, the text
void hudLines();                                    // render thread: lines for the trainer's HUD window
void options();                                     // render thread: the options (Player tab)
bool shown();                                       // on, for the trainer's on-screen list

} // namespace landing
