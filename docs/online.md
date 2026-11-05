# GridNode and online play

Mirror's Edge Catalyst still has online parts: Dash and time trial leaderboards, ghosts, shared
runs and stats on EA's servers, and community services such as Beat Revival. GridNode's rule
(2026-11-04): **mods may change online play on purpose, the player can always see which ones do,
and the MEC trainer is never used to cheat on leaderboards.**

## What GridNode itself does

GridNode makes no network connections of its own: no downloads, no telemetry, no update checks.
Everything below is about what the game does while GridNode and mods run in it.

| Where online play could be reached | What happens |
|---|---|
| The console's online commands (the game's online settings and services) | The menu's console asks the player first (shown in orange). A mod's `gn::console` refuses them until the mod calls `gn::allowOnline`. |
| Settings classes marked online | `gn::settings` still works (settings are memory, which GridNode cannot gate) but logs a warning once per mod and class. |
| Progression flags the game marks as synced to online stats | Set like any other (most never reach a service); each one is logged. |
| Dash and time trial uploads (finish times, splits, the run's ghost) | Blocked while the MEC trainer is loaded (below). |
| A mod's own code | A mod is a DLL in the game: GridNode cannot stop code that bypasses its API. Install mods you trust. |

## A mod that changes online play

```cpp
GN_MOD("my-online-mod") {
    gn::allowOnline("talks to the community Dash server instead of EA's"); // one line, shown to the player
    return true;
}
```

From that call on, the mod's online console commands run and each is logged with `!!! ONLINE`. The
call writes a warning block to `gridnode.log`, and the menu's Mods list shows the mod in orange with
its reason, for as long as it is loaded. Restoring or redirecting a service is fine; posting times
nobody ran is not what this is for. Details: [Writing a mod](modding.md#online-services).

## The MEC trainer

The trainer changes the game (god mode, noclip, game speed, saved positions), so it never calls
`allowOnline`. While it is loaded, the game's upload requests for Dash finish times
(`UploadNamedChallengeStatsRequest`), time trial times and splits (`UploadStatsRequest`) and the
run's ghost (`UploadReplayRequestMessageBase`) are dropped before they leave, and a new best Dash
time is not saved. Unload the trainer (its Settings tab, or the menu's Mods list) and restart the
level before a run that should count.

## For players

- The Mods list (INSERT) says which mods change online play, in orange.
- `gridnode.log` has a `!!! ONLINE` block for each, and a line for each online command one runs.
- The player build has no console window; mods follow the same rules there.
