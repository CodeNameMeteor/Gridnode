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

Choose what to record (messages, entities created and destroyed, entity events, call stacks), play, and press
Home the moment the thing you are looking for happens. **Export** writes
`recordings/<time>-<name>.jsonl` next to gridnode.dll: a header line, then one line per event.
The **Ignore** lists leave out message types and event targets that are too frequent to be
useful. F8 only pauses: an export holds everything since the last **Clear**, so press Clear between
recordings you want to keep apart.

### Objects: what a pointer was

Every object a recorded line points at (the entity an event went to, a message's pointers, a list's
entries) gets an entry in the recording's object table: its type, and for entities created while
recording, their data, blueprint and creation time; when it was destroyed, if it was. An address the
game reuses for a new object gets a new entry. Lines carry `"obj": N` (and `"obj"` on each pointer);
the table follows the events as `object` lines:

```json
{"kind":"object","id":2,"ptr":"0x1b4015800","type":"PamServerCharacterEntity","blueprint":"...","created":12.4,"destroyed":20.1,"first_seen":12.4}
```

`created: null` means it existed before the recording started. With **destroyed** ticked (next to
*Entities created*), each entity destroyed while recording is a `destroyed` line, with its call stack;
it comes from the destructor hooks GridNode already has (vtable +0x38 of every entity class).

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

### Watching fields

**Watch fields** records every change to a field of the live entities of a class (any subclass
too): give the class, the offset and its type (byte, i32, f32), and a label. They are read every frame while recording (the *readings a second* slider reads them less often; the menu shows how long a reading takes,
and the export's header keeps the average and worst as `watch_readings`); each change is a `field` line with the value and the old one (the
first reading of each entity is marked `first`). **Add the city alert manager's state** watches
the KSEC alert's active flag, level, kills, cameras seeing and active hotspots
(`docs/research/city-alert.md`). Watches last until the game closes. An entity of a watched class is watched from the moment it is
created (so a list it fills at once shows as additions) and dropped when it is destroyed; ones that
existed before the watch are found within 2 s, and when recording starts.

The **list** type watches a vector of pointers instead (begin at the offset, end 8 bytes after, as
`eastl::vector` keeps them): each change is a `list` line with what was `added` and `removed` (with
each entry's type and object id), `reordered` when the entries both readings share changed order, and
the list as it is now (`items`, the first 64). The first reading of each entity is marked `first`.
Up to 256 entries are read. **Add the spawners' lists** watches the characters each character spawner
has spawned, on the server (+0x148) and the client (+0xb0) (`docs/research/replay-ghosts.md`).

### Sending an event

**Send an event (research)** delivers an event to every live entity of a class, as one of the
level's connections would: the entity's `onEvent` (vtable +0x20) with an `EntityEvent`
{0x141a4a8b0, id, 0}, on its realm's thread. Give the event by name (hashed with djb2-xor) or as
`0x…`. Each delivery is recorded as a `sent` line, so what the game did next shows in the same
recording. It is a research tool: an event the class does not expect may do nothing, or something
unexpected.

### Tracing a class's calls

**Choosing a class.** Every field that takes a class (tracing, watching fields, sending an event) has
a **...** button that lists classes to choose from: the ones **alive now** (with how many), the ones
**in this recording** (with how many lines are about them), or **all classes**. Type part of a name to
narrow it, hover a row to see its base classes, click or press Enter to pick.

**Trace a class's calls**: type a class (`ClientCharacterSpawnEntity`, `PamClientReplayEntity`), tick
**and subclasses** if wanted, and press **Trace**. While recording, every call to one of its virtual
functions is recorded: the object, the vtable slot, the function, the return address and the stack.
Each slot is recorded at most N times a second (the slider, 20 by default); the calls in between are
counted (`skipped_before` on the next one). In the export:

```json
{"kind":"call","class":"ClientCharacterSpawnEntity","slot":"0xd8","fn":"0x1432aa460","ptr":"0x1b4015800","from":"0x143234f51", ...}
```

`summarize.py` names the function when it is annotated
(`call ClientCharacterSpawnEntity::vf+0xd8 (ClientCharacterSpawnEntity_onCharacterSpawned)`).

How: each slot of the class's vtables is swapped (a pointer swap, as every hook) for a small thunk
that saves the argument registers, records the call and jumps to the original, so any signature
passes through unchanged. Slot 0 (`getType`) is never traced: GridNode tells an object's class by
that slot, so tracing it made the class's objects unknown to the inspector and the watches. A traced
class stays hooked until the game exits; unticking it stops the
recording. Avoid tracing very common base classes (`Entity`, with subclasses) while playing: every
call then goes through the recorder.

### Who called

Each exported stack also has `callers`: the frames that are inside a function of a reflected
class's vtable, named `Class::vf+0xNN` (with how many classes share it). The function is found in
the executable's unwind table. Those frames say which entity's code made the event or message
happen; `summarize.py` prints the names beside the addresses.

## Reading it

```
python tools/recording/summarize.py recordings/20261020-201905-safehousetp.jsonl
```

lists the most frequent events, and for each marker what was unusual around it (seen near the
marker far more often than elsewhere) with the call stacks behind it. Each frame is its image address first (to paste into Ghidra's Go
To), then the function it is in and how far into it:

```
0x143234f21 (ClientSpawnEntity_createFromNetwork+0x51)   an annotated function
0x1431635a5 (TextureRefParameterEvent::vf+0x30 +0x25)    a reflected class's vtable function
0x14347c5ab (FUN_14347c480+0x12b)                        not named yet (Ghidra's FUN_ name)
```

Function starts come from the recording itself (recordings since 2026-10-29 end with a `frames`
line), else from a game dump: `--exe`, or by default the newest `dumps/*.exe` next to the recordings
folder. Names come from `db/annotations`; `--ghidra-names EXPORT` adds your own Ghidra's names (a
GridNodeSync export) where the annotations have none. Events and properties named in
`db/annotations/events.yaml` are named even in older recordings. `--window 2` narrows the time around a marker,
`--all` lists everything near it. Lines about an object carry its id (`#12`), and targets are named
with their blueprint when the recording knows it.

```
python tools/recording/summarize.py rec.jsonl --object 12          # one object: what it was, when made and destroyed, every line about it
python tools/recording/summarize.py rec.jsonl --object 0x1b4015800 # every object that had this address
```

### Comparing two recordings

Record the same thing done two ways (a kick and a slow open of the same door, a ghost and no
ghost), with a marker at the moment that matters in each, then:

```
python tools/recording/summarize.py kick-door.jsonl --diff slow-door.jsonl --marker "marker 1" --diff-marker "marker 2" --window 2
```

lists what happens only in the first (A), far more often in A (`x` is how many times more often,
per second, so different lengths compare), only in B, and far more in B; then A's own lines in the
order they first appear (seconds from the marker) and the call stacks behind them, the lines only A
has first. Without `--marker` it compares the whole recordings; `--marker "*"` takes every marker.
Values (animation and field values, list contents) are left out so the two line up; `--values` keeps
them. `--kinds event,sent,call` compares only those line kinds (a part of the level streaming in can
otherwise fill the list), `--min-lift 3` asks for a bigger difference ("more" also needs at least 3
lines beyond what the other recording's rate predicts). It says so when both recordings start with
the same lines (exported without Clear). In the stacks, frames that a fifth or more of A's stacks
go through (event dispatch, the update passes) are folded into one `(N shared: ...)` line.

Many events reach their target from the queued-event pass (`FUN_143125240` under
`fb_UpdateManager_runPass`, e.g. `DoorKick` from `ClientGameWorld_postUpdate`): their stack shows
the delivery, not the code that posted them.

### Into Ghidra

```
python tools/recording/summarize.py rec.jsonl --ghidra build/rec-ghidra.json --exe bin/dumps/MirrorsEdgeCatalyst-<time>.exe
```

writes a file for GridNodeSync's *Import* (or `import FILE` as script arguments):

- a function at the start of each recorded frame's function (from the executable's unwind table, so
  `--exe` is a dump from GridNode menu → *Game image*), made only where Ghidra has no function around
  that frame;
- a bookmark (category *GridNode recording*) on the call before each return address in the innermost
  frames (`--depth`, 8 by default), with what came through it;
- a plate comment block `GridNode [recording] (<file>)` on each of those functions: in how many stacks
  it was and for what, and, for traced functions, how often they were called. Importing the same
  recording again replaces its block and bookmarks; other recordings' blocks stay.

It also carries a struct and vtable type for each class the recording traced (calls, list and
field watches, events sent to it), with the annotated members of the class and its bases, so their
functions read `this->...` after the import. `--classes all` adds every class seen in the recording
(created, destroyed, events, animations, messages; many more structs), `--classes none` leaves them
out. Sizes, bases and vtables come from the dump (`--reflected`, default `gridnode-reflected.json`
next to the `recordings` folder, else `db/reflected/`); without it only annotated classes get one.

`--marker NAME` (with `--window`) keeps only the lines around that marker. Without `--exe` only the
bookmarks are written.

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
