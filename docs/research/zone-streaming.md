# Zone streaming (why far-away placing and keeping copies loaded fail)

Findings of 2026-10-21 (EA app build), from in-game logs and the executable (Ghidra, then a
disassembly of the dump when Ghidra was closed). Work in progress.

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

## The streamer's focus points (to be finished in Ghidra)

In the ZoneStreamer (code around `0x143c7e000`–`0x143c8a000`):

| Offset | Holds (inferred) |
|---|---|
| +0x288 | focus records, 0x18 bytes each (+0x10 set to -1 when reset, `0x143c81780`) |
| +0x2a8 | focus positions (x, y, z read at `0x143c820c0`) |
| +0x2c8 | focus count |
| +0x2d8 | the zone grid (zone lookup of a position: `0x143c7fa60`, or `0x143c7fb40` with zone heights) |
| +0x2e8 | an object whose vtable +8 takes (positions, count, ...) |
| +0x2f0 | zone states, 8 bytes each: flags byte (bit 0, bit 1, bit 2), u16 at +4 |

`0x143c82069` walks the focus positions and finds each one's zone. A **second focus point** at a
chosen position should make the streamer load the zones around it, as it does around Faith: the
way to load a source far away before placing from it, and to keep the zones holding copies'
sources loaded.

The game has entities for this: `ZoneStreamerFocusEntityData` (`FocusPoint` +0x20, a
LinearTransform; `AutoEnabled` +0x60; TypeInfo `0x14289fc00`, creator `0x143c82570`) and
`ZoneStreamerFocusEntity` (TypeInfo `0x14289ed60`, vtable `0x141cfe460`). `0x143c7fe50` appends a
0x18-byte record {0, caller, -1} to a list (count at +0x48): possibly a focus entity registering.

Next, in Ghidra:

1. How a `ZoneStreamerFocusEntity` adds its position to +0x2a8 (its vtable; the references to its
   TypeInfo at `0x143c7f183`, `0x143c7fda1`, `0x143c8009f`).
2. Then either spawn a focus entity from a copy of a loaded `ZoneStreamerFocusEntityData` with our
   position (the engine's own path, removed with the destroy sequence), or call the same
   registration with GridNode's own record.
