# Input

## Keys, actions and the animation system

1. **Devices.** The PC input devices (global 0x14256d7c0) keep the keyboard's state: one int per
   `InputDeviceKeys` value at +0x1acc (Space, 57, at +0x1bb0).
2. **Actions.** Each frame the keys the player bound become **actions** in the player's
   `EntryInputState` (0x530 bytes, `ClientPlayer +0x14b0`). Actions are known by a 32-bit hash;
   a registry (0x1424aa1a0) maps each hash to an index. Indices below the first digital index are
   analog floats (+0x350); the rest are bits in +0x4b8. Catalyst has 43: 6 analog (move, look) and
   37 digital.
3. **Animation input.** `PamClientAntInputComponent` gets the state every frame (0x143911c80) and
   feeds the movement actions to the animation system (ANT), which decides what Faith does: jump,
   vault, coil, climb, slide.

Jump is digital action 38, hash **0xb5a0a981** (bit 32): the only bit Space sets.

## Pressing an action

When the player has flag 0x40 (`ClientPlayer +0x1520`), the animation input component reads the
player's **override** state (+0x14c0, made by the game's 0x143203d50) instead of the live one.
GridNode's `gn::actionPress(hash, ticks)` copies the live state into the override, sets the action
and raises the flag while the action is held; `gn::jump()` presses the jump action for four ticks.
Movement and look carry on (they are copied), and it works whatever key the player bound to jump.

`gn::keyPress(key, ticks)` presses a key instead, as the keyboard would (MEC_Trainer's way); it
depends on the binding.

## Open questions

- Names of the other actions (the hashes are not plain names in GridNode's dictionary).
- Shift, Ctrl and E did not show in the watch: they may be read elsewhere, or were not pressed.
