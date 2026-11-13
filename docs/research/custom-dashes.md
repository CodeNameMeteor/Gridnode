# Custom dashes

A dash of the player's own, run like the game's: its free-roam marker, a start box, the end circle and guide arrows,
placed anywhere in the city with the layout editor. Worked out 2026-11-13 (claim-lab `networked-dash`, then
`sdk/src/dashes.cpp`); Ghidra findings in `db/annotations/spawn.yaml` (data registry) and the workflow runs
`bus-network-ids`, `dash-name`, `dash-map-icon`.

## What a dash is made of (the city's own, from mainlogic)

Each dash is a layer of `levels/sp/sp_maincity/mainlogic/mainlogic`; the links between its parts are in the
mainlogic SubWorldData. One dash (Anchor 02, dumped in game):

| Part | Data | How the dash prefab is connected to it |
|---|---|---|
| the dash (free-roam marker at its origin; the map entry's Transform is the same place) | SpatialPrefabReferenceObjectData, Blueprint `Gameplay/Social/PF_RunnersRoute` (102 objects) | |
| start box | OBBData (half 3.43 x 2.56 x 1.02 m) | link `StartGeometry` |
| area box beside the marker | OBBData (half 11.9 x 3.7 x 3.7 m) | link `0x32107415` |
| end circle | ObjectReferenceObjectData, `PF_DashArrow_End` (its EndVolume is the finish) | link `StopGeometry` -> `EndVolume`; properties `0x13f3c4fc` -> `0x4db752b4`, `0x90100ae4` -> `0xc2490554` (flags 0x12); `Activated` -> `Enabled` |
| start arrow | SpatialPrefabReferenceObjectData, `PF_DashArrow_Start` | `Activated` -> `Enabled` (flags 0x11) |
| guide arrows | ObjectReferenceObjectData, `PF_DashArrow_Medium` (scaled, turned along the route) | `Activated` -> `Enabled` |
| the challenge | PamNamedChallengeSelectorEntityData (another file) -> PamNamedChallengeData | link `ChallengeSelector` |

The arrow models point along one axis of their transform that is not forward: GridNode learns it from the city's
arrows (the axis that points toward the end circle) and logs it.

## Placing one so the client gets it

1. **The data registry.** The network sends an entity's data as (bundle slot, class id, index), found by the data's
   exact address (0x143138aa0); data not in the registry writes nothing and the stream goes out of step. Every
   object GridNode makes is registered under a bundle id of its own (`DataRegistry_register`, ids 1903 down; the
   game's bundles use low ids). Ids are taken out 5 s after the dash goes, at level end and on unload: one left in
   at game exit crashed the registry's destructor.
2. **A bus the client knows.** A ghost's parent bus is sent as (owner's ghost, bus id) (0x143490350). A bus only the
   server made has an id the client does not have. GridNode creates the ServerBlueprintEntity (a reference to its
   blueprint with StreamRealm 2, `SpatialPrefabReference_create`) directly under Faith's root bus, whose owner has a
   ghost, then sends it stream-in (0xd27bc17e). The client makes its half from the network.
3. **GUIDs.** A data object with flag 0x100 at +0x0a has its instance GUID in the 16 bytes before it; the shared
   content manager matches challenge data by it. Copies carry the source's GUID (the challenge a new one); before
   they had one, restart from the end screen did nothing.

## The challenge and the stats

The challenge is a copy of the template dash's PamNamedChallengeData with a new GUID; its type copy has
RequiresOnline, HasDuration (the Started message and the replay) and SendRefreshLeaderboardMessage off. Its Sid is
the typed name: the localizer translates only ids starting with `ID_` and gives other text back as it is.

It keeps the template's stat keys: with keys of its own, restart and exit did nothing. `NamedChallengeStats_onFinished`
is detoured for custom challenges: player index 1 (the online upload) off during the call, and every best it changed
and the pending list put back after (as the MEC trainer's anti-cheat). GridNode keeps the time and the best (in the
layout).

## Editing

Moved parts are not made again while the free camera flies (every remake is a network spawn: it stuttered and
cancelled runs) or during a run (the challenge entity's state +0x78 not 0); they are made again when the free camera
stops, or with Rebuild.

## Open

- Map icon: the client content manager registers each dash from the map database (dash-map-icon); a custom dash is
  not registered, has the real dash's ContentId and no icon (research `dash-map-register`).
- The end screen shows the game's best for the template's stat keys, not GridNode's (`dash-end-screen-best`).
- Ghosts of the best run (`dash-ghost`); writing a dash as a Frosty mod (`fbmod-dash-export`).
