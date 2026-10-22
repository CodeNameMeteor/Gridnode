# Startup: from the main menu to in game

Research 2026-10-22, from a GridNode recording started as early as the overlay allowed and stopped
once in game (20261022-172659-startup.jsonl: 22 s, 113,385 records, 90,984 of them entity
creations). The recording starts on the front end (GridNode loads there), so the engine's own
boot is not in it. Messages are the engine's (category, type); times in seconds.

| t | What | Messages (first seen) |
|---|---|---|
| 1.1 | intro movie done | `PamUISystemIntroMovieDoneMessage`, `UINetworkVideoDoneMessage` |
| 1.79 | the front end is left: its player, connection and level go | `ServerPlayerLeftLevelMessage`, `ServerPlayerDestroyMessage`, `ClientPlayerDeletedMessage`, `ClientCharacterLocalPlayerDeletedMessage`, `ClientLevelUnloadMessage` |
| 1.90 | front end unloaded; the server loads the city | `ClientLevelUnloadedMessage`, `ServerLevelUnloadedMessage`, `ServerPeerLoadLevelMessage` |
| 2.0–2.4 | the local server stops and restarts; the client connects and asks for the level | `ServerStoppedMessage`, `ClientConnectionInitializedMessage`, `NetworkConnectedMessage`, `ClientConnectedMessage`, `ClientLoadLevelMessage`, `CoreExitIngameMessage` |
| 4.82 | GridNode's "level started" (SP_MainCity) | |
| 9.80 | zone streaming starts | `ZoneStreamerZoneChangedMessage` |
| 10.28 | **client level loaded** | `ClientLevelFinalizedMessage`, `ClientLevelLoadedMessage` |
| 10.57 | sublevels' entities created | `SubLevelEntitiesCreatedMessage` |
| 10.59–10.68 | **the local player and its character** | `ClientPlayerLocalSetMessage`, `ClientPlayerConnectMessage`, `ClientControllableSpawnDoneMessage`, `ClientCharacterLocalPlayerSetMessage` |
| 11.19 | **in game** | `NetworkFirstPlayerEnteredMessage`, `CoreEnteredIngameMessage`, `ClientEnteredIngameMessage` |
| 11.52 | paused (the "continue" screen) | `PamplonaGamePausedMessage` |
| 17.66 | **the player spawned** (after continuing) | `NetworkOnPlayerSpawnedMessage`, then `ClientPlayerEnterEntryMessage`, `PamplonaDistrictChangedMessage` |
| 17.82 | input flowing | `ClientInputUnchangedInputMessage` |

For the SDK: these are the lifecycle points mods wait for. "In game" (`CoreEnteredIngameMessage`)
comes before the player can move; "spawned" (`NetworkOnPlayerSpawnedMessage` /
`ClientPlayerEnterEntryMessage`) is when Faith is really there. MECMP waits a fixed ~3 s after a
level starts before spawning ghosts; waiting for the spawned message instead would be exact.
