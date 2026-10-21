# Recording what the game does

The Recorder writes down what the game does while you play: the messages it sends, the entities it
creates, the events entities send each other, with the game's call stack for each. Record around
something you want to understand (a KSEC, a door opening, a level loading), then read the
recording, or open its call stacks in Ghidra, instead of guessing. It is a developer tool, not in
the player build.

## Making a recording

Open the GridNode menu (Insert) and choose **Recorder**.

| Key | Does |
|---|---|
| F8 | record / pause |
| Home | drop a marker (named, with Faith's position) |

Choose what to record (messages, entities created, entity events, call stacks), play, and press
Home the moment the thing you are looking for happens. **Export** writes
`recordings/<time>-<name>.jsonl` next to gridnode.dll: a header line, then one line per event.
The **Ignore** lists leave out message types and event targets that are too frequent to be
useful.

### Animation flags

With **Animation flags** on, the recording also has every change to an animated object's ANT
*game state*: the bools, ints and enums (and, with *with floats and vectors*, the floats and
vectors) that ANT reads to drive doors, props and characters. Each `anim` line has the value, the
time, the object it belongs to (class, position), and who wrote it:

- a **WriteAntGameState entity** (logic in a prefab): its class, the property it was written
  from (`property`, named like events) and the game state's ANT asset (`game_state`, a GUID);
- **game code**: no entity; the call stack shows where from.

Only changes are recorded: most values are written every frame, unchanged. Floats and vectors are
recorded at most 10 times a second each. For a door, record, open it, drop a marker, and look for
the `anim` lines on the door around the marker: they show which flag it set and when.

Writes by game code are seen once the game state interface they go through has been met through a
logic write first (GridNode wraps each interface the first time it sees it); if a door is driven
only by code, its first writes in a session can be missed.

## Reading it

```
python tools/recording/summarize.py recordings/20261020-201905-safehousetp.jsonl
```

lists the most frequent events, and for each marker what was unusual around it (seen near the
marker far more often than elsewhere) with the call stacks behind it as image addresses (0x14…),
named where `db/annotations` knows the function. `--window 2` narrows the time around a marker,
`--all` lists everything near it.

## Event names

Frostbite keeps events and properties by a hash of their name, so many events are recorded as
hex (`e2b5d766`). `tools/events/build_names.py` finds names for them from what you have on your
computer and writes `recordings/event-names.txt`, which the Recorder reads when it starts and
`summarize.py` reads for recordings made before it existed.

```
python tools/events/build_names.py --xml "PreFindings/docs/XML Dump" ^
    --reflected bin/gridnode-reflected.json --exe PreFindings/MirrorsEdgeCatalyst_dump_SCY.exe ^
    --recordings bin/recordings --out bin/recordings/event-names.txt --report event-names-report.md
```

Every input is optional:

- `--xml`: an EBX dump as XML (Frosty's export). Its event and property ids are either named
  (`In [00597302]`, checked against the hash) or bare hashes, which become the ones to find
  names for. Names in the dump are tried too.
- `--reflected`: GridNode's reflected type dump (every type, field and enum value name).
- `--exe`: the game executable, or a dump of it: its strings.
- `--recordings`: your exports; their unnamed events are hashes to find names for.
- `--names`: more names to try, one per line.

Besides trying names as they are, the tool tries them with common prefixes and suffixes (`On`,
`Set`, `Start`, `…ed`, `…Finished`) and joins two common words (`--combine`, the number of words;
0 turns it off). Those names fit the hash but are guesses: almost always right, sometimes not. In
the file they are marked `# guess` after a tab, and the report (`--report`) lists every hash with
where it is used, named or not, so the unnamed ones can be worked on.

The file is made from game data: keep it on your computer, and don't commit it.

### What a first run found

A run on 102 files of the dump and two recordings named 302 of the 807 hashes seen (55 of them
guesses). Among them:

| Event | Sent to | Likely meaning |
|---|---|---|
| `StreamIn` / `StreamOut` | `ServerBlueprintEntity`, `ServerSubLevelEntity` | a part of the level loads / unloads |
| `FireMemory` / `LoadMemory` | `ServerEventMemoryEntity` | an event remembered, then replayed |
| `SetVariation1`, `SetVariation2` | `ObjectVariationSwitchEntity` | an object switches its look |
| `OnCulled` | from `PamAnimatableCullingEntity` | an animated object stopped being drawn |

Most of the rest are names designers chose inside prefabs (an AI's `OnMeleeHit`, the city alert's
`SpawnDrone`), which only a larger dictionary or the full dump will name.
