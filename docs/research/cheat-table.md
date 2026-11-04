# The Cheat Engine table: what GridNode can use

The user shared a Cheat Engine table for the game (`MirrorsEdgeCatalyst.CT`, 2026-11-04: about 1,100
entries, two scripts). It was made for the same build. Both of its scripts' byte patterns are at the
addresses they name (checked in Ghidra on 2026-11-04: 0x1439DA8DD in 0x1439DA880, and 0x14471D67C in
0x14471D5D0). Every global it starts from exists and is read by the game's code. Much of it is
already in `db/annotations/` (MEC_Trainer's paths): Faith's position, movement state and wall counts,
the speed, the spacebar, time of day, and the jump tuning.

Nothing new from it is annotated yet. Each item goes in when the feature that uses it is built and
the code reading it is checked in Ghidra. Pointer paths are written first read first: `[g] +a -> +b`
means read the pointer at g, add a, read again, add b.

## New to GridNode, most useful first

| # | What | The table's path | For GridNode |
|---|---|---|---|
| 1 | **Health.** A health component's health (+0x20, f32) and max health (+0x24). `[0x142547F90] +0x28 -> +0` is the first component in the list that `ServerCharacterHealthComponent_registerForUpdate` (0x14336C3AD) keeps. Agrees with the Field diff window: a KSEC's `PamServerAIHealthComponent` +0x20 went 3 -> 2 -> 0. | `[[0x142547F90] +0x28] +0` -> +0x20, +0x24 | A health API (get and set, any character); the trainer: Faith's health, "one-hit KSEC". |
| 2 | **The map waypoint.** Where the player's marker is. | `[[0x1425789B0] +0x70] +0x128` -> +0x2270 (Vec3). The global is read by 0x14394BC70 only. | Trainer: "Teleport to the waypoint". Teleport exists, so this is small. |
| 3 | **The last checkpoint.** | `[[[[0x142548128] +0x70] +0x28] +0x38] +0x158` -> +0x170 (Vec3) | Trainer: "Teleport to the checkpoint"; a mod could move the checkpoint. |
| 4 | **The current mission** (built differently: see `db/annotations/gamestate.yaml`; the progression manager is a hash map of every flag's value, the active mission is `PamProgressionData.SystemGeneratedFlags.Flags[0]`). A getter at 0x1439DA880 returns `[rax+0x18]`; the table hooks it to keep rax. Also progression: `[0x14257C9D8] +0x20 -> +0x1858` (active mission; value at +0x18) and `+0x2d0` (the Shift unlock, value at +0x18). | As left. | A `mission` read for mods (level scripts, speedrun splits). |
| 5 | **The last ground position.** Her ground object (our `PamMovement.ground`) holds the last ground position at +0x0, +0x4, +0x8 (we had Y only), and a ground distance at -0x150. Through `[0x1423DA028] +0x20 -> +0x20 -> +0x40 -> +0x20`. | As left. | `gn_movement` could give all three. Small. |
| 6 | **The sky.** The table hooks 0x14471D67C (in 0x14471D5D0) to catch a sky component's data (rdx), then edits its fields: colours, sun size, clouds, fog, Rayleigh and Mie scattering. The fields are the reflected `SkyComponentData`'s. | A hook, no global. | GridNode can find sky entities and their data by type instead: a Sky window in the inspector or a mod. Medium. |
| 7 | **Faith's position, another path** ("Falcon X/Y/Z": found by someone called Falcon). | `[0x142106A68] +0x338 -> +0x10 -> +0xE8` -> +0x650 (Vec3) | Nothing new: GridNode has her position. |
| 8 | **Smaller things.** Frame rate (f32 at 0x1425639B4); Faith's yaw (her pose +0x44, +0x4c); `Main` 0x142142A48 (+0x50 the client); a second immortal path `[0x142577770] +0x70 -> +0xc0`. | As left. | Annotations when needed. |

The table's settings groups (GameTimeSettings, GameRenderSettings, post-processing, mesh, Enlighten,
network and so on) are reflected settings objects. GridNode already lists and edits them: the
inspector's Settings tab and the console. The presence and online settings entries are left alone
(online services).
