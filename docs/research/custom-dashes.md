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

## Map icon and the best shown (2026-11-13, untested in game)

- The challenge copy gets a ContentId of its own (`0xf1d00000` up, +0x98) and HideInMap (+0x9c) off: the client's
  shared content manager (`PamClientSharedContentManager`, ui.yaml) keys markers by ContentId, and with the template's
  the real dash's marker was found and ours could not be added (`dash-map-register` c5, c6).
- Before the dash is placed, the client adds its map marker as the manager's vf15 does for the level's:
  `addChallengeMarker(manager, layout entry, challenge)`, the entry a LinearTransform (the marker's place) then the
  challenge's GUID at +0x40; the constructor copies it. The manager's lists keyed by stat name (+0x280) and by
  ContentId (+0x2b0) are left alone (their readers skip a challenge they do not find). No removal function is known:
  a dash taken away hides its marker (+0x225 off, then `PamNamedChallengeMapMarker_refresh`).
- The best the dash's own screens and its marker show comes from the server as DashState's best for the stat key
  (the template's). `PamClientNamedChallengeEntity_storeResult` is detoured: for our challenge it gets GridNode's best
  (the layout's, or the last run if better; none: no best).

Game-tested 2026-11-13: the map icon (own ContentId) and the screens' best. The end screen's record, the difference
and "new record" come from the finished message, made inside `NamedChallengeStats_onFinished` from DashState's best
for the stat key: the run was compared with the template's 13.96 s. The stats hook now writes GridNode's best (none:
9999999) into the main stat's best for the call and the template's back after.

## Parts and stars (2026-11-13 evening, untested)

- Arrow kinds: the city's dashes' arrows of each kind are copied (their scale); a kind no dash uses but in the city's
  bundle: the medium copy with its Blueprint (+0x18) swapped. Turn right is turn left with its side row turned round.
  Large and turn right are only in `belikewater` (not loaded in free roam).
- More start boxes: copies of the start box, each with the template's StartGeometry connections. The log says how
  many start boxes the city's dashes have at most. Whether the game uses more than one link: game test.
- Stars: `PamNamedChallengeData` InternalThreshold1Star..3Star (+0x18, +0x1c, +0x20, ints, centiseconds,
  `NamedChallenge_starsForValue`). InternalThresholdCancel (+0x24) is not read yet: kept at least 3 minutes and twice
  the 1-star time (a custom dash may be longer than its template).

## Open

- Whether the marker shows on the map and in the dash list, and whether the end screen reads the stored best
  (`dash-end-screen-best` c11): game test.
- The marker's stars come from the template's thresholds; the dash list keyed by stat name still finds the template.
- Ghosts of the best run (`dash-ghost`: the game keeps none for dashes); writing a dash as a Frosty mod
  (`fbmod-dash-export`: c3 refuted, the EBX side unverified).
