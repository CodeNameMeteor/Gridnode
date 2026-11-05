# Troubleshooting

Almost everything GridNode does is written to **`gridnode.log`**, next to `gridnode.dll` (GridNode's
folder, or the game's folder when it starts with `version.dll`). The run before is kept as
`gridnode.prev.log`. Start there, and send both with any report.

## GridNode does not load

| What you see | Why, and what to do |
|---|---|
| The injector: `FAILED: gridnode.dll not found` | Keep `gridnode_inject.exe` next to `gridnode.dll` (run it from the unzipped folder). |
| The injector: `Hint: run gridnode_inject.exe as administrator` / `The game runs as administrator` | The game runs with more rights than the injector: run the injector as administrator. |
| The injector: `FAILED: the game could not load gridnode.dll` | A missing dependency, or Windows blocked the downloaded file: right click `gridnode.dll` → Properties → Unblock, or unzip again. Antivirus software can also stop it. |
| No `gridnode.log` at all | GridNode never started: run the injector after the game has started, or (loading with the game) check `version.dll`, `gridnode.dll` and `mods/` are next to `MirrorsEdgeCatalyst.exe`. |
| `this is not the game build GridNode supports` | Another build of the game (not the EA app's, or updated since). GridNode does nothing on it, on purpose. |
| `the engine's type list never filled; GridNode stays passive` | GridNode was injected too early or into something else; start the game fully, then inject. |

## A mod does not load

| In the log | Why, and what to do |
|---|---|
| `returned 2 from gn_mod_load: it was built for a newer GridNode` (Mods list: "needs a newer GridNode") | Install the GridNode the mod asks for, or newer. |
| The Mods list: `needs <id>, which is not loaded` (or `... or newer`) | Install that mod (or a newer one): this one is not started without it. |
| `returned 1 from gn_mod_load` (Mods list: `failed (1)`) | The mod itself said no: its own lines just above say why. |
| `ui: built with Dear ImGui ..., GridNode has ...` | The mod was built against another Dear ImGui than GridNode's: it runs without its windows. Rebuild it with the SDK of this GridNode. |
| `[mod] shares function 0x... with other-mod` | Two mods hook the same place. Both run; if something misbehaves, try each mod alone and report both names. |
| `patch at ... refused: <mod>'s patch is there` | Two mods patch the same code: only the first one's patch is in. |

## The game crashes

The log ends with a `crash:` block: the exception, where it happened (`gridnode.dll+0x...`, a mod's
name, or the game's code), the registers and the call stack. If the game restarted since, the block
is in `gridnode.prev.log`.

- Note what you were doing (the level, a mission, a Dash, quitting, a mod's button).
- Try again without mods (move them out of `mods/`): if it no longer crashes, add them back one at
  a time.
- Report it with both logs: to the mod's author when the block names a mod, else to GridNode
  ([issues](https://github.com/CodeNameMeteor/Gridnode/issues)).

## Other problems

| Problem | What to do |
|---|---|
| INSERT does nothing | Another tool (MECMP, an overlay) may use it: set `menu_key` in `gridnode.ini` ([Writing a mod](modding.md#ui-dear-imgui)). With `menu_key=NONE`, only a mod's own key opens the menu. |
| The menu shows no GridNode window | `gridnode_window=0` in `gridnode.ini`: the mods' windows only. |
| Keys stop working after reloading mods | Look for lines about hotkeys in the log and send it; unloading and loading the mod from the Mods list puts its keys back. |
| A Dash or time trial time is not uploaded | The MEC trainer blocks uploads while it is loaded ([GridNode and online play](online.md)). |
| The game is slow with GridNode | A developer build tracks every entity for the inspector; `inspector=0` in `[ui]`, or the player build, costs less. |
