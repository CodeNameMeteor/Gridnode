# KSEC spawning (research, started 2026-11-04)

Goal: a trainer option that keeps free-roam KSEC out of the city (and takes away the ones there),
while missions keep theirs. From two recordings (`ksec-patrol`, `ksec-door`, 2026-11-04), the
executable in Ghidra and our annotations (`db/annotations/spawn.yaml`, from MECMP).

## Where KSEC come from

- Every character in the city is made by a **`PamServerCharacterSpawnEntity`** (vtable
  0x141c4b388; 993 of them when the city loads). Its data is a `CharacterSpawnReferenceObjectData`:
  `Blueprint` +0x18, `ControllableTransform` +0xa0 (its place), `Team` +0x138, `Enabled` +0x17c,
  `AutoSpawn` +0x17f, `Template` +0x198 (a `PamCharacterSpawnTemplateData`, an asset: name at
  +0x10). The spawner's own enabled flag is +0x2a4; what it spawned is a vector at +0x148 / +0x150.
  A spawned character points back at its spawner at +0x138.
- Spawners are driven by **`PamAIGroup`** entities (AIGroupType: Default, MonsterCloset, HeliDrop,
  AssaultMode) and get the event `Spawn` (0x0dc320de). In `ksec-patrol` (31.20 s) an `IntEntity`
  counter's `Increment` fired an `EventSplitterEntity`, which sent `Spawn` to two groups and four
  spawners in the same tick: four KSEC (with their AI states, weapons and managers) appeared at
  (-179, 55, 177), next to Faith.
- In `ksec-door` the alert path shows the rest: a camera / kill raises the city alert (`Lock` to the
  safe-spawn tracker at 35.12 s, the manager's kill and level events at 42.18 s), then
  `PamServerAIAirSpawnPointProvider` and a helicopter (`ServerVehicleSpawnEntity` Spawn, TakeControl)
  and eight more KSEC from `PamServerCharacterSpawnEntity` Spawn, and a `PamAIGroup` Spawn again at
  55.38 s. Groups of type MonsterCloset are the likely "out of a door" ones (to confirm).

Faith's own safe spawn is one of these spawners too (the trainer's checkpoint read, 2026-11-04:
"spawn 1a92ddcd0 (PamServerCharacterSpawnEntity)"): a block must never touch the spawner of the
local player's character, nor any spawner while a respawn is due.

## How to stop them (read in Ghidra)

- The spawn itself is the spawner's vtable +0xd0, `ServerCharacterSpawnEntity_spawnAtSpawnPoint`
  (0x143390830, shared with the base class). It already returns null when the game refuses a spawn
  (the player checks at its start), so its callers handle "no character": a detour that returns
  null for a KSEC spawner in free roam should stop the spawn without side effects (to test).
- To take away the ones already there: `character_despawn` (API 19) on characters whose spawner
  is a KSEC one.

## What the spawner log showed (2026-11-04)

Six runs of the trainer's logger (free roam at three places, under the city, and in the mission
"Be Like Water"):

- **Team does not separate them:** every character read team 2.
- **The template does.** KSEC are `Characters/SpawnTemplates/Enemy/...`: `SpawnTemplate_AIMelee_SubProtector`
  (and `_V02`, `_V03`), `SpawnTemplate_AIMelee_Protector`, `SpawnTemplate_AIRanged_Enforcer01` / `02` /
  `_Lowsense`. Everyone else is `.../Male/` or `.../Female/` (Runner_03, HighCaste03, Black_November_01-04
  and `_Gun`, Dogen_Henchwoman, Rebecca, and `..._ErrorTemplate` for the crowd), and ghosts are
  `.../Projection/SpawnTemplate_Projection_Replay`.
- **Missions use the same KSEC templates** (Protector, SubProtector in "Be Like Water"): telling a
  mission's KSEC apart needs the mission index (`gn::mission`, 0 in free roam).
- The spawners' own position (their data's `ControllableTransform`) is local to their blueprint: no
  spawner was within 200 m by it, so the logger now lists characters only.

Built from this: the trainer's World tab → **No KSEC (free roam)**.

## Still to find

1. Whether a mission's KSEC spawn before the mission index is set (they would be stopped then).
2. The helicopter and drones (`ServerVehicleSpawnEntity`, `PamServerAIAirSpawnPointProvider`) only
   come with an alert; the alert option ends alerts already.
