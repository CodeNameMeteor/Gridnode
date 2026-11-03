# Zone streaming (why far-away placing and keeping copies loaded fail)

Findings of 2026-10-21 (EA app build), from in-game logs and the executable (Ghidra, then a
disassembly of the dump when Ghidra was closed). Confirmed in game on 2026-10-21 (3 streamers found; copies kept and a library entry placed 1.1 km
from its source).

## What the logs showed

- The city's districts are sublevels such as `Levels/SP/SP_MainCity/CentralCityBuildings_ZS`. Such a
  sublevel never streams out as a whole: a **ZoneStreamer** loads and unloads *zones* inside it
  around Faith. Dropping the sublevel's StreamOut (GridNode's pin) keeps nothing, and a StreamIn on
  it loads nothing. When Faith teleports away, the zone holding a copy's source unloads, and the
  copy with it (its owner list is the zone's).
- What the editor sees is where Faith is, not where the free camera is: placing far away worked
  only while Faith was still near the source.
- The same mesh can be in two places: the district's detailed object and a `Backdrops/...`
  sublevel's distant version. A copy re-made from the backdrop one looked wrong ("kind of loaded").

## ZoneStreamerSettings.PinVisitedZones does nothing

`ZoneStreamerSettings` (TypeInfo `0x14289fba0`; `TestZoneHeights` +0x28, `PinVisitedZones` +0x29)
is registered at `0x143c7ed30` and kept in the global `0x1425a3fa8` (getter `0x143c7fe40`). Only
three places use that global: the registration, the getter, and `0x143c82069`. The getter's two
callers (`0x143c854ed`, `0x143c866b4`) and `0x143c82069` read +0x28 only. Nothing reads +0x29:
the option is left over and has no effect in this build (confirmed in game: ticking it changed
nothing).

## Focus points (worked out 2026-10-21 in Ghidra)

A ZoneStreamerEntity's **focus manager** is at +0x280. It keeps records of 0x18 bytes at +0x08
(end +0x10, count +0x48): `{u64, focus, -1, flags}`, flags bit 0 = removed.

- `ZoneStreamer_update` (0x143c81900) calls 0x143c81850 each frame: for every record not removed,
  `focus->vtable[0](focus, record[0], out)`, where `out` (0x20 bytes, the streamer's +0x2a8) gets a
  position (float4) and a direction (float4). 0x143c82060 then finds each position's zone, and
  0x143c7f570 drops removed records.
- `ZoneStreamerFocus_add` (0x143c7e7c0) appends `{a2, focus, -1, 0}`. `ZoneStreamerFocus_remove`
  (0x143c81390) marks the matching record removed, and writes out of bounds when none matches.
- `ZoneStreamerFocusEntity` (0x58 bytes; creator 0x143c7f0c0) is the game's own focus: its
  interface is at +0x40 (vtable 0x141cfe528, slot 0 = 0x143c7f9a0 reads its `FocusPoint`), the
  streamer at +0x30 and its focus manager at +0x48; Enable (event 0x8ba423e4) adds it, Disable
  removes it.
- `ZoneStreamerSettings.PinVisitedZones` is never read (see above).

**What GridNode does** (layout editor): it keeps up to 8 focus objects of its own, in static memory
(a record may point at one until the streamer drops it), each a vtable with that one function. Every
2 s it places them at the sources of the level's copies and, for 45 s, where a library entry is being
placed from, and registers them with every live zone streamer on its own thread (the add above; to
remove, it marks its own record once found). A streamer whose grid does not hold a point finds no zone
for it. Library entries save their source's position for this.

The streamers are not made through a creator GridNode hooks, so they are found as the game's focus
entities find them (0x143c7d800): each game world (`GameWorlds`, 0x1424a9b98: [0] client,
[1] server) has an index (+0x28) into every ClassInfo's per-world entity lists, whose first node is
at ClassInfo + (index + 6) * 0x10 (0x143164d50); a ZoneStreamerEntity's node is at +0x268, and a
node's first field is the next node.
