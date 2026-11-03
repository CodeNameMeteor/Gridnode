#!/usr/bin/env python3
"""Summarise a GridNode recording (recordings/*.jsonl from the in-game Recorder).

    python tools/recording/summarize.py recordings/20261020-201500-ksec.jsonl
    python tools/recording/summarize.py rec.jsonl --window 2 --marker "ksec 1"

Prints what was recorded, then for each marker what happened around it: the message types and
entity creations that are unusual there (seen near the marker but rare elsewhere), and the game
call stacks behind them as image VAs (0x14...), ready to open in Ghidra. Stack frames that match
an annotated function's start are named from db/annotations. Events recorded without a name are
named from event-names.txt (next to the recording, or --names; tools/events/build_names.py makes
it).

    python tools/recording/summarize.py rec.jsonl --ghidra build/rec-ghidra.json --exe bin/dumps/game.exe

writes a GridNodeSync import instead: functions to create where Ghidra has none around a frame,
a bookmark at each call site with what came through it, and a comment per function with counts.

    python tools/recording/summarize.py with-ghost.jsonl --diff without-ghost.jsonl

compares two recordings of the same thing done two ways: what happens only (or far more often) in
the first, with its call stacks, and what only happens in the second. With --diff-values, the
values too: per field and animation writer, where the two runs part (one stops changing, a sign
flips, the numbers move apart), aligned on the markers.

Per marker it also prints Faith's movement states (each one's start, length and peak speed) and the
range of every number that changes near it (speeds, predictions, animation values). Animation values
the game writes itself (no property name) are told apart by their writer: `anim writer 0x1b76af840`
(this recording's object), or by where the game calls it from for comparing runs. Name them with
--alias FILE: lines of "<address or call site> <name>".
"""

import argparse
import collections
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load_annotation_names():
    """va -> name for every annotated function, virtual (Class::name), code site and hooked function,
    and the annotated event names (into EVENT_NAMES). Uses the annotation loader (tools/gen)."""
    sys.path.insert(0, str(ROOT / "tools" / "gen"))
    try:
        from gridnode_gen import annotations
        db, _ = annotations.load(str(ROOT / "db" / "annotations"))
    except Exception as e:  # no PyYAML, or broken annotations: fall back to plain names
        print(f"(annotations not loaded: {e}; frames are named from the recording only)", file=sys.stderr)
        return {}
    names = {}
    for f in db.functions.values():
        names[f.va] = f.name
    for c in db.classes.values():
        for v in c.virtuals:
            if v.va:
                names.setdefault(v.va, f"{c.name}::{v.name}")
        for pr in c.properties:
            ANNOTATED_EVENTS[pr.hash] = pr.name
    for cs in db.code.values():
        names.setdefault(cs.va, cs.name)
    for h in db.hooks.values():
        names.setdefault(h.original, h.name)
    for e in db.events.values():
        ANNOTATED_EVENTS[e.hash] = e.name
    return names


def load_ghidra_names(path):
    """va -> name from a GridNodeSync export (your own Ghidra's names)."""
    doc = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    return {int(f["va"]): f["name"] for f in doc.get("functions", [])}


def djb2(s):
    h = 5381
    for c in s.encode("utf-8"):
        h = ((h * 33) ^ c) & 0xFFFFFFFF
    return h


EVENT_NAMES = {}
ANNOTATED_EVENTS = {}  # hash -> name, from db/annotations (events and class properties): these win
FN_NAMES = {}  # function VA -> annotated name (set in main)
STARTS = {}    # frame VA -> start of its function (the recording's "frames" line, callers, or a dump)
OWNERS = {}    # function start -> "Class::vf+0xNN" (a reflected class's vtable function; from callers)
IMAGE = None   # a PeImage of the game (--exe, or a dump found in ../dumps), for frames the recording has no start for
OBJECTS = {}   # object id -> the recording's "object" line (what a pointer was: type, blueprint, lifetime)


def load_event_names(path):
    """hash -> name from an event-names.txt (lines: name, optionally a tab and a note; # comments)."""
    if not path or not path.exists():
        return 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#"):
            continue
        name = line.split("\t", 1)[0].rstrip()
        if name:
            EVENT_NAMES[djb2(name)] = name
    return len(EVENT_NAMES)


# Faith's movement states (field "movement state", +0x90), as far as they are known (db/annotations/
# faith.yaml, 2026-10-30). Unknown ones print as their number: a wrong name misleads more than none.
MOVEMENT_STATES = {1: "on the ground", 2: "jumping", 8: "wall run", 25: "mag rope swing"}

# Names given with --alias: writer addresses (one recording) or call sites (any recording).
ALIASES = {}

# The float and bool writers of the animation system (db/annotations/ant.yaml).
WRITER_KINDS = {0x141a434d0: "float", 0x141a435c8: "bool"}


def load_aliases(path):
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        key, _, name = line.partition(" ")
        try:
            ALIASES[int(key, 16)] = name.strip()
        except ValueError:
            pass


def writer_site(ev):
    """Where the game called the animation writer from (the first frame): the same in every run."""
    st = ev.get("stack") or []
    return int(st[0], 16) if st else None


def writer_text(ev, stable=False):
    """'writer 0x1b76af840' (this recording) or 'writer from 0x1446ae87d' (stable across runs), with an
    alias when there is one."""
    w = int(ev.get("ant_writer", "0"), 16)
    site = writer_site(ev)
    name = ALIASES.get(w) or (ALIASES.get(site) if site else None)
    if stable:
        base = f"writer from 0x{site:x}" if site else "writer (no stack)"
    else:
        base = f"writer 0x{w:x}"
    return base + (f" '{name}'" if name else "")


def anim_name(ev, stable=False):
    prop = ev.get("property")
    pid = ev.get("property_id")
    if pid and prop == pid:
        prop = ANNOTATED_EVENTS.get(int(pid, 16)) or EVENT_NAMES.get(int(pid, 16), prop)
    if prop:
        return prop
    if ev.get("ant_writer"):
        return writer_text(ev, stable)
    return "(game code)"


def event_label(ev):
    name, eid = ev.get("event", "?"), ev.get("event_id")
    if eid and name == eid:
        name = ANNOTATED_EVENTS.get(int(eid, 16)) or EVENT_NAMES.get(int(eid, 16), name)
    return name


def target(ev):
    """The entity an event is about: its type, and the blueprint it came from when the recording knows it."""
    o = OBJECTS.get(ev.get("obj"))
    bp = (o or {}).get("blueprint") or ev.get("blueprint")
    return ev.get("target", ev.get("type", "?")) + (f" [{bp}]" if bp else "")


def label(ev):
    k = ev.get("kind")
    if k == "message":
        return "msg " + ev.get("type", "?")
    if k == "created":
        bp = ev.get("blueprint")
        return "new " + ev.get("type", "?") + (f" [{bp}]" if bp else "")
    if k == "event":
        return f"event {event_label(ev)} -> {target(ev)}"
    if k == "anim":
        val = ev.get("value")
        if isinstance(val, list):
            val = "(" + " ".join(f"{v:.2f}" for v in val[:3]) + (" ..." if len(val) > 3 else "") + ")"
        return f"anim {anim_name(ev)} = {val} -> {ev.get('target', '?')}"
    if k == "field":
        return f"field {ev.get('field', '?')} = {ev.get('value')} -> {ev.get('target', '?')}"
    if k == "sent":
        return f"SENT {event_label(ev)} -> {ev.get('target', '?')}"
    if k == "call":
        fn = int(ev.get("fn", "0"), 16)
        named = FN_NAMES.get(fn)
        return f"call {ev.get('class', '?')}::vf+{ev.get('slot', '?')}" + (f" ({named})" if named else f" (0x{fn:x})")
    if k == "destroyed":
        return "gone " + target(ev)
    if k == "list":
        if ev.get("first"):
            return f"list {ev.get('field', '?')} -> {target(ev)}: first reading"
        parts = [f"+{a.get('type', '?')}" for a in ev.get("added", [])] + [f"-{r.get('type', '?')}" for r in ev.get("removed", [])]
        if ev.get("reordered"):
            parts.append("order changed")
        return f"list {ev.get('field', '?')} -> {target(ev)}: " + " ".join(parts)
    if k == "marker":
        return "marker " + ev.get("name", "")
    if k == "level":
        return "level " + ev.get("level", "")
    return k or "?"


def diff_label(ev, values=False):
    """label() without the values that differ from run to run (animation and field values, list
    contents), so two recordings of the same thing line up."""
    k = ev.get("kind")
    if values and k == "anim":  # the value, but the writer by where it is called from (its address differs per run)
        return label(ev).replace(f"anim {anim_name(ev)} ", f"anim {anim_name(ev, stable=True)} ", 1)
    if values:
        return label(ev)
    if k == "anim":
        return f"anim {anim_name(ev, stable=True)} -> {ev.get('target', '?')}"
    if k == "field":
        return f"field {ev.get('field', '?')} -> {ev.get('target', '?')}"
    if k == "list":
        return f"list {ev.get('field', '?')} -> {target(ev)}: " + ("first reading" if ev.get("first") else "changed")
    if k == "marker":
        return "marker"
    return label(ev)


def compare(a, a_span, b, b_span, min_lift=2.0):
    """Label counts of two recordings (or windows of them) -> (only in A, more in A, only in B, more in B).
    Rates are per second, so recordings of different lengths compare. "More" also needs at least 3
    lines beyond what the other's rate predicts (4 against 2 is not a pattern)."""
    a_span, b_span = max(a_span, 1e-3), max(b_span, 1e-3)
    only_a, more_a, only_b, more_b = [], [], [], []
    for lab in set(a) | set(b):
        na, nb = a.get(lab, 0), b.get(lab, 0)
        ra, rb = na / a_span, nb / b_span
        if nb == 0:
            only_a.append((na, lab))
        elif na == 0:
            only_b.append((nb, lab))
        elif ra >= min_lift * rb and na - rb * a_span >= 3:
            more_a.append((ra / rb, na, nb, lab))
        elif rb >= min_lift * ra and nb - ra * b_span >= 3:
            more_b.append((rb / ra, nb, na, lab))
    only_a.sort(key=lambda x: (-x[0], x[1]))
    only_b.sort(key=lambda x: (-x[0], x[1]))
    more_a.sort(key=lambda x: (-x[0], -x[1], x[3]))
    more_b.sort(key=lambda x: (-x[0], -x[1], x[3]))
    return only_a, more_a, only_b, more_b


def function_start(va):
    if va in STARTS:
        return STARTS[va]
    s = IMAGE.function_start(va) if IMAGE is not None else None
    STARTS[va] = s
    return s


def frame_text(va, names):
    """The address first (to paste into Ghidra), then the function it is in and the offset into it:
    0x143234f21 (ClientSpawnEntity_createFromNetwork+0x51), (Class::vf+0x10 +0x3d) for a reflected
    class's vtable function, or (FUN_143234ed0+0x51) when the function has no name yet."""
    n = names.get(va)
    if n:
        return f"0x{va:x} ({n})"
    start = function_start(va)
    if not start:
        return f"0x{va:x}"
    off = va - start
    if names.get(start):
        return f"0x{va:x} ({names[start]}+0x{off:x})"
    if OWNERS.get(start):
        owner, _, shared = OWNERS[start].partition(" (shared by ")
        return f"0x{va:x} ({owner} +0x{off:x}" + (f", shared by {shared.rstrip(')')} classes)" if shared else ")")
    return f"0x{va:x} (FUN_{start:x}+0x{off:x})"


def _top(counter, n=6, width=110):
    lines = []
    for lab, c in counter.most_common(n):
        lines.append(f"{c:6d}x  {lab[:width]}")
    if len(counter) > n:
        lines.append(f"        ... and {len(counter) - n} more kinds")
    return lines


def to_ghidra(events, rec_name, img=None, depth=8, header=None):
    """A GridNodeSync import (tools/ghidra/GridNodeSync.java) made from a recording:

    - createFunctions: the start of the function around each stack frame, from the executable's
      unwind table (needs `img`); GridNodeSync makes a function there when Ghidra has none
      containing the frame (frames in code Ghidra never analysed).
    - bookmarks: one per call site (the call before each return address, the innermost `depth`
      frames), with what came through it (category "GridNode recording").
    - comments: a plate comment block per function ("GridNode [recording] (<recording>)") with what
      went through it and how often; a later import of the same recording replaces its block.
    """
    frame_hits = collections.defaultdict(collections.Counter)
    func_hits = collections.defaultdict(collections.Counter)
    traced = collections.defaultdict(collections.Counter)
    starts = {}
    no_start = set()

    def start_of(va):
        if va in starts or va in no_start:
            return starts.get(va)
        s = STARTS.get(va) or (img.function_start(va) if img else None)
        if s is None:
            no_start.add(va)
        else:
            starts[va] = s
        return s

    for ev in events:
        lab = label(ev)
        if ev.get("kind") == "call" and ev.get("fn"):
            traced[int(ev["fn"], 16)][lab] += 1 + int(ev.get("skipped_before", 0) or 0)
        stack = ev.get("stack") or []
        seen_funcs = set()
        for i, x in enumerate(stack):
            va = int(x, 16)
            if i < depth:
                frame_hits[va][lab] += 1
            s = start_of(va)
            if s is not None and s not in seen_funcs:   # recursion counts once per stack
                seen_funcs.add(s)
                func_hits[s][lab] += 1

    mark = f"GridNode [recording] ({rec_name})"
    level = (header or {}).get("level")
    where = f"Recorded in {level}." if level else ""
    comments = []
    for s, hits in func_hits.items():
        total = sum(hits.values())
        body = [mark, f"In {total} recorded stack{'s' if total != 1 else ''}. {where}".strip()] + _top(hits)
        comments.append({"va": s, "kind": "plate", "comment": "\n".join(body)})
    for fn, hits in traced.items():
        if fn in func_hits:   # merge: the function both called (traced) and on stacks
            c = next(c for c in comments if c["va"] == fn)
            c["comment"] += "\nTraced calls:\n" + "\n".join(_top(hits, 3))
        else:
            total = sum(hits.values())
            comments.append({"va": fn, "kind": "plate",
                             "comment": "\n".join([mark, f"Traced: called {total} time{'s' if total != 1 else ''}. {where}".strip()]
                                                  + _top(hits, 3))})
    bookmarks = []
    for va, hits in frame_hits.items():
        top = hits.most_common(3)
        text = "; ".join(f"{c}x {lab[:80]}" for lab, c in top)
        if len(hits) > 3:
            text += f"; +{len(hits) - 3} more"
        bookmarks.append({"va": va, "afterCall": True, "category": "GridNode recording",
                          "comment": f"{rec_name}: {text}"})
    create = [{"va": s, "at": va} for va, s in starts.items()]
    # One request per function start; `at` is a frame inside it, to check Ghidra has no function there.
    seen, uniq = set(), []
    for c in sorted(create, key=lambda c: (c["va"], c["at"])):
        if c["va"] not in seen:
            seen.add(c["va"])
            uniq.append(c)
    return {
        "format": "gridnode-ghidra", "version": 1,
        "structs": [], "functions": [], "labels": [],
        "createFunctions": uniq,
        "bookmarks": sorted(bookmarks, key=lambda b: b["va"]),
        "comments": sorted(comments, key=lambda c: c["va"]),
        "stats": {"frames": len(frame_hits), "functions": len(func_hits), "traced": len(traced),
                  "frames_without_unwind_entry": len(no_start) if (img or STARTS) else None},
    }


def _when(v):
    return "before the recording" if v is None else f"{v:.3f} s"


def object_story(events, which):
    """Everything a recording says about one object (by id, or every object that had the address)."""
    if which.lower().startswith("0x"):
        ids = [i for i, o in OBJECTS.items() if int(o["ptr"], 16) == int(which, 16)]
    else:
        ids = [int(which)] if int(which) in OBJECTS else []
    if not ids:
        print(f"no object {which} in this recording (objects are listed in recordings made with GridNode 2026-10-29 or later)")
        return 1
    for i in ids:
        o = OBJECTS[i]
        print(f"#{i} {o.get('type')} at {o.get('ptr')}" + (f" [{o['blueprint']}]" if o.get("blueprint") else ""))
        if o.get("data"):
            print(f"  data {o['data']} at {o.get('data_ptr')}")
        print(f"  created {_when(o.get('created'))}, destroyed {_when(o.get('destroyed'))}, first seen {o.get('first_seen', 0):.3f} s")
        for e in events:
            refs = [r for r in e.get("pointers", []) if r.get("obj") == i]
            refs += [r for key in ("items", "added", "removed") for r in e.get(key, []) if r.get("obj") == i]
            if e.get("obj") != i and not refs:
                continue
            how = ""
            if e.get("obj") != i:
                how = "  (points at it" + (f" at +{refs[0]['off']}" if "off" in refs[0] else "") + ")"
            print(f"  {e['t']:9.3f}  {label(e)}{how}")
        print()
    return 0


def recording_classes(events, mode="traced"):
    """The classes a recording is about. "traced": the ones traced, watched or sent events to (what you
    chose to look at); "all": also every class created, destroyed, sent an event or pointed at."""
    names = set()
    for e in events:
        k = e.get("kind")
        if k == "call":
            names.add(e.get("class"))
        elif k in ("list", "field", "sent"):
            names.add(e.get("target"))
        elif mode == "all":
            if k in ("created", "destroyed"):
                names.add(e.get("type"))
            elif k in ("event", "anim"):
                names.add(e.get("target"))
            for r in e.get("pointers", []):
                names.add(r.get("type"))
    return sorted(n for n in names if n and n != "?")


def class_structs(names, reflected_path):
    """GridNodeSync struct entries (members, size, vtable, depth) for these classes, as ghidra-export
    makes them; needs the reflected dump for sizes, bases and vtables."""
    sys.path.insert(0, str(ROOT / "tools" / "gen"))
    from gridnode_gen import annotations, ghidra_sync, reflected
    db, _ = annotations.load(str(ROOT / "db" / "annotations"))
    r = None
    if reflected_path and pathlib.Path(reflected_path).exists():
        r = reflected.Reflected.load(str(reflected_path))
    elif reflected_path:
        print(f"  (reflected dump {reflected_path} not found: annotated classes only)", file=sys.stderr)
    out = []
    for n in names:
        e = ghidra_sync._class_entry(n, db, r)
        if e is not None:
            out.append(e)
    return out


def _load_image(path):
    sys.path.insert(0, str(ROOT / "tools" / "gen"))
    from gridnode_gen.pe_image import PeImage
    return PeImage(str(path))


def read_recording(path):
    """-> (header, events, objects by id, function starts by frame)."""
    header, events, objects, starts = None, [], {}, {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            ev = json.loads(line)
            if ev.get("kind") == "header":
                header = ev
            elif ev.get("kind") == "object":
                objects[ev["id"]] = ev
            elif ev.get("kind") == "frames":
                starts.update({int(k, 16): int(v, 16) for k, v in ev.get("starts", {}).items()})
            else:
                events.append(ev)
    return header, events, objects, starts


def _scope(events, marker, window):
    """The events to compare and the seconds they cover: around each marker named `marker` (all
    markers when it is "*"), or the whole recording."""
    if not events:
        return [], 0.0
    if marker:
        at = [e["t"] for e in events if e.get("kind") == "marker" and (marker == "*" or e.get("name") == marker)]
        if not at:
            return None, 0.0
        near = [e for e in events if e.get("kind") != "marker" and any(abs(e["t"] - t) <= window for t in at)]
        span, end = 0.0, None  # windows of close markers overlap: count the time they cover once
        for t in sorted(at):
            lo, hi = t - window, t + window
            span += hi - max(lo, end) if end is not None and lo < end else hi - lo
            end = hi if end is None else max(end, hi)
        return near, span
    return [e for e in events if e.get("kind") != "marker"], events[-1]["t"] - events[0]["t"]


def _folded(frames, names):
    """One line for frames most stacks share: '(3 shared: EntityBus_fireEvent, FUN_14314e900, ...)'."""
    def short(va):
        m = re.search(r"\((\S+?)(\+0x[0-9a-f]+)?[ )]", frame_text(va, names))
        return m.group(1) if m else f"0x{va:x}"
    more = ", ..." if len(frames) > 3 else ""
    return f"          ({len(frames)} shared: {', '.join(short(v) for v in frames[:3])}{more})"


def diff(args, a_events, names):
    """--diff: what A has that B does not (and the other way round)."""
    _, b_events, b_objects, b_starts = read_recording(args.diff)
    for va, s in b_starts.items():
        STARTS.setdefault(va, s)
    a_mark = args.marker
    b_mark = args.diff_marker or args.marker
    a_scope, a_span = _scope(a_events, a_mark, args.window)
    b_scope, b_span = _scope(b_events, b_mark, args.window)
    for which, sc, mk, f in (("A", a_scope, a_mark, args.file), ("B", b_scope, b_mark, args.diff)):
        if sc is None:
            print(f"{which} ({f}) has no marker named {mk!r}")
            return 1
    if args.kinds:
        kinds = {k.strip() for k in args.kinds.split(",")} | ({"message"} if "msg" in args.kinds else set())
        a_scope = [e for e in a_scope if e.get("kind") in kinds]
        b_scope = [e for e in b_scope if e.get("kind") in kinds]
    shared = 0  # recordings exported one after another without Clear start with the same lines
    for x, y in zip(a_events, b_events):
        if x != y:
            break
        shared += 1
    if shared >= 50:
        def own(evs):
            return [e.get("name") for e in evs[shared:] if e.get("kind") == "marker"]
        print(f"Note: both recordings start with the same {shared} lines (exported without Clear in between?), "
              "so their first markers are the same moment. Compare each one's own marker (--marker / --diff-marker):")
        for which, evs in (("A", a_events), ("B", b_events)):
            if shared >= len(evs):
                print(f"  {which} is all in the other one (it is the start of it)")
            else:
                print(f"  {which}'s own markers: {', '.join(own(evs)) or '(none)'}")
        print()
    a_labels = [diff_label(e, args.values) for e in a_scope]
    saved = dict(OBJECTS)   # labels name blueprints from each recording's own object table
    OBJECTS.clear()
    OBJECTS.update(b_objects)
    b_labels = [diff_label(e, args.values) for e in b_scope]
    OBJECTS.clear()
    OBJECTS.update(saved)
    ca, cb = collections.Counter(a_labels), collections.Counter(b_labels)
    only_a, more_a, only_b, more_b = compare(ca, a_span, cb, b_span, args.min_lift)

    def where(mk):
        return f"within {args.window:g} s of " + ("every marker" if mk == "*" else f"marker {mk!r}") if mk else "whole recording"
    print(f"A: {args.file}\n   {len(a_scope)} events, {a_span:.1f} s ({where(a_mark)})")
    print(f"B: {args.diff}\n   {len(b_scope)} events, {b_span:.1f} s ({where(b_mark)})")
    print("Counts are lines in each; rates are per second, so different lengths compare.\n")
    n = args.top
    print(f"Only in A ({len(only_a)} kinds):")
    for c, lab in only_a[:n]:
        print(f"  {c:6d}  {lab}")
    print(f"\nMore in A (x = rate in A / rate in B; {len(more_a)} kinds):")
    for lift, c, cbn, lab in more_a[:n]:
        print(f"  {c:6d} vs {cbn:<6d} x{lift:6.1f}  {lab}")
    print(f"\nOnly in B ({len(only_b)} kinds):")
    for c, lab in only_b[:n]:
        print(f"  {c:6d}  {lab}")
    print(f"\nMore in B ({len(more_b)} kinds):")
    for lift, c, can, lab in more_b[:n]:
        print(f"  {c:6d} vs {can:<6d} x{lift:6.1f}  {lab}")

    if args.diff_values:
        def marker_t(evs, mk):
            if mk and mk != "*":
                return next((e["t"] for e in evs if e.get("kind") == "marker" and e.get("name") == mk), None)
            return evs[0]["t"] if evs else None
        ta, tb = marker_t(a_events, a_mark), marker_t(b_events, b_mark)
        print()
        if ta is None or tb is None:
            print("--diff-values needs one marker in each (--marker / --diff-marker), or none for the starts")
        else:
            span = args.window if a_mark else max(a_events[-1]["t"] - ta, b_events[-1]["t"] - tb)
            print("\n".join(value_diff(a_events, b_events, ta, tb, span, n)))

    # When A's own lines first appear (from the marker, or the start), and the stacks behind them.
    keep = [lab for _, lab in only_a[:n]] + [x[3] for x in more_a[:n]]
    if not keep:
        return 0
    t0 = None
    if a_mark and a_mark != "*":
        t0 = next(e["t"] for e in a_events if e.get("kind") == "marker" and e.get("name") == a_mark)
    elif a_events:
        t0 = a_events[0]["t"]
    first = {}
    # Frames most of A's stacks go through (event dispatch, the update passes) say nothing about one
    # line: fold them and show the frames that differ.
    seen_in = collections.Counter()
    with_stack = [e for e in a_scope if e.get("stack")]
    for e in with_stack:
        seen_in.update(set(e["stack"]))
    common = {int(x, 16) for x, c in seen_in.items() if c >= max(5, len(with_stack) / 5)}
    stacks = collections.Counter()
    for e, lab in zip(a_scope, a_labels):
        if lab in keep:
            first.setdefault(lab, e["t"])
            if e.get("stack"):
                stacks[(lab, tuple(int(x, 16) for x in e["stack"]))] += 1
    print(f"\nA's lines in the order they first appear (seconds from {'the marker' if a_mark and a_mark != '*' else 'the start'}):")
    for lab in sorted(first, key=first.get):
        print(f"  {first[lab] - t0:+9.3f}  {lab}")
    if stacks:
        print("\nCall stacks behind them in A (innermost first; the lines only A has first):")
        only = {lab for _, lab in only_a}
        ordered = sorted(stacks.items(), key=lambda kv: (kv[0][0] not in only, -kv[1], kv[0][0]))
        for (lab, frames), c in ordered[:min(n, 12)]:
            print(f"  {c:4d}x  {lab}")
            shown, folded = 0, []
            for va in frames:
                if va in common:
                    folded.append(va)
                    continue
                if folded:
                    print(_folded(folded, names))
                    folded = []
                print(f"          {frame_text(va, names)}")
                shown += 1
                if shown == 6:
                    break
            if folded and shown < 6:
                print(_folded(folded, names))
    return 0


# ---- numbers over time: value ranges, movement states, where two runs part -----------------------

def number(v):
    """A reading as one number: bools 0 / 1, vectors by their length; None for anything else."""
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, list) and v and all(isinstance(x, (int, float)) for x in v):
        return sum(float(x) * float(x) for x in v[:3]) ** 0.5
    if isinstance(v, str) and v in ("True", "False"):
        return 1.0 if v == "True" else 0.0
    return None


def series_key(ev, stable=False):
    """What a reading is a reading of: a field of an object, or an animation value."""
    k = ev.get("kind")
    if k == "field":
        return f"{ev.get('field', '?')} ({ev.get('target', '?')})"
    if k == "anim":
        return f"anim {anim_name(ev, stable)} ({ev.get('target', '?')})"
    return None


def series(events, t0, stable=False):
    """key -> [(t - t0, value)] in time order, for fields and animation values with a number."""
    out = collections.defaultdict(list)
    for e in events:
        key = series_key(e, stable)
        if key is None:
            continue
        v = number(e.get("value"))
        if v is not None:
            out[key].append((e["t"] - t0, v))
    for v in out.values():
        v.sort(key=lambda x: x[0])
    return out


def is_vector(events, key, stable=False):
    return any(isinstance(e.get("value"), list) for e in events if series_key(e, stable) == key)


def value_ranges(near, t0, top):
    """Lines for every number that changes near the marker: its range, when it peaked, its value at
    the marker and at the end of the window."""
    rows = []
    for key, pts in series(near, t0).items():
        vals = [v for _, v in pts]
        lo, hi = min(vals), max(vals)
        if len(pts) < 3 or hi - lo < 1e-6 or set(vals) <= {0.0, 1.0}:  # steady, or an on / off: not a range
            continue
        peak_t = next(t for t, v in pts if v == hi)
        before = [v for t, v in pts if t <= 0]
        at = before[-1] if before else pts[0][1]
        speed = "speed" in key
        rows.append((not speed, -(hi - lo) / max(abs(hi), abs(lo), 1e-6), key,
                     f"{lo:.4g} .. {hi:.4g} (peak at {peak_t:+.3f} s), at the marker {at:.4g}, at the end {pts[-1][1]:.4g}  [{len(pts)}]"))
    rows.sort()
    return [f"    {key}: {text}" for _, _, key, text in rows[:top]]


def state_timeline(near, t0):
    """Faith's movement states in the window: when each began, how long it lasted, the peak speed in it."""
    states = sorted((e for e in near if e.get("kind") == "field" and e.get("field") == "movement state"), key=lambda e: e["t"])
    if not states:
        return []
    speeds = sorted(((e["t"], float(e["value"])) for e in near if e.get("kind") == "field" and e.get("field") in ("speed (game)", "speed (measured)")
                     and isinstance(e.get("value"), (int, float))), key=lambda x: x[0])
    end = max(e["t"] for e in near)
    lines = []
    first = states[0]
    if first.get("old") is not None and first["t"] > min(e["t"] for e in near):
        lines.append(f"    (before {first['t'] - t0:+.3f} s: state {first['old']})")
    for i, e in enumerate(states):
        until = states[i + 1]["t"] if i + 1 < len(states) else end
        st = int(e["value"])
        name = MOVEMENT_STATES.get(st, "")
        peak = max((v for t, v in speeds if e["t"] <= t <= until), default=None)
        open_end = i + 1 == len(states)
        lines.append(f"    {e['t'] - t0:+8.3f} s  state {st:<3d}{(' ' + name) if name else '':<17}  "
                     f"{'for ' if not open_end else 'still, '}{until - e['t']:.3f} s"
                     + (f", peak speed {peak:.2f} m/s" if peak is not None else ""))
    return lines


def collapse(timeline):
    """Consecutive identical lines become one: (t, label, extra, repeats, last t)."""
    out = []
    for e, lab, extra in timeline:
        if out and out[-1][1] == lab and out[-1][2] == extra:
            t, l, x, n, _ = out[-1]
            out[-1] = (t, l, x, n + 1, e["t"])
        else:
            out.append((e["t"], lab, extra, 1, e["t"]))
    return out


def value_diff(a_events, b_events, a_t0, b_t0, window, top):
    """--diff-values: per field and animation writer in both runs (aligned on their markers), where
    they part. Writers are matched by where the game calls them from (their addresses differ per run)."""
    def scoped(evs, t0):
        return [e for e in evs if abs(e["t"] - t0) <= window]
    sa, sb = series(scoped(a_events, a_t0), a_t0, True), series(scoped(b_events, b_t0), b_t0, True)
    rows = []
    for key in sorted(set(sa) & set(sb)):
        pa, pb = sa[key], sb[key]
        notes = []
        # Stopped changing in one run while the other goes on.
        def last_change(p):
            last = None
            for (t1, v1), (t2, v2) in zip(p, p[1:]):
                if abs(v2 - v1) > 1e-6:
                    last = t2
            return last
        la, lb = last_change(pa), last_change(pb)
        if la is not None and lb is not None and abs(la - lb) > 0.1:
            which, stop, other = ("A", la, lb) if la < lb else ("B", lb, la)
            notes.append(f"stops changing in {which} at {stop:+.3f} s (the other until {other:+.3f} s)")
        elif (la is None) != (lb is None):
            notes.append(f"never changes in {'A' if la is None else 'B'}")
        # The sign at the marker.
        def at0(p):
            before = [v for t, v in p if t <= 0]
            return before[-1] if before else p[0][1]
        va, vb = at0(pa), at0(pb)
        if va * vb < 0 and min(abs(va), abs(vb)) > 1e-3:
            notes.append(f"opposite signs at the marker ({va:.4g} vs {vb:.4g})")
        # The first moment the values are far apart (each held until its next reading).
        scale = max(max(abs(v) for _, v in pa), max(abs(v) for _, v in pb), 1e-6)
        times = sorted({t for t, _ in pa} | {t for t, _ in pb})
        ia = ib = 0
        cur_a, cur_b = pa[0][1], pb[0][1]
        first = None
        for t in times:
            while ia < len(pa) and pa[ia][0] <= t:
                cur_a = pa[ia][1]
                ia += 1
            while ib < len(pb) and pb[ib][0] <= t:
                cur_b = pb[ib][1]
                ib += 1
            if t >= max(pa[0][0], pb[0][0]) and abs(cur_a - cur_b) > 0.1 * scale:
                first = (t, cur_a, cur_b)
                break
        if first:
            notes.append(f"apart from {first[0]:+.3f} s ({first[1]:.4g} vs {first[2]:.4g})")
        ra = (min(v for _, v in pa), max(v for _, v in pa))
        rb = (min(v for _, v in pb), max(v for _, v in pb))
        if notes:
            when = first[0] if first else (min(la or 0, lb or 0))
            rows.append((when, key, notes, ra, rb))
    rows.sort(key=lambda r: r[0])
    only_a = sorted(set(sa) - set(sb))
    only_b = sorted(set(sb) - set(sa))
    lines = [f"Values that part (aligned on the markers, within {window:g} s; earliest first):"]
    for _, key, notes, ra, rb in rows[:top]:
        lines.append(f"  {key}")
        lines.append(f"      A {ra[0]:.4g} .. {ra[1]:.4g}   B {rb[0]:.4g} .. {rb[1]:.4g}")
        for n in notes:
            lines.append(f"      {n}")
    if not rows:
        lines.append("  (none: every number both runs have moves alike)")
    if only_a:
        lines.append(f"Numbers only A has: {', '.join(only_a[:top])}" + (" ..." if len(only_a) > top else ""))
    if only_b:
        lines.append(f"Numbers only B has: {', '.join(only_b[:top])}" + (" ..." if len(only_b) > top else ""))
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--window", type=float, default=3.0, help="seconds before and after each marker (default 3)")
    ap.add_argument("--marker", help="only this marker (by name)")
    ap.add_argument("--top", type=int, default=25, help="lines per list (default 25)")
    ap.add_argument("--all", action="store_true", help="list every event near the marker, not only the unusual ones")
    ap.add_argument("--names", type=pathlib.Path, help="event-names.txt (default: the one next to the recording)")
    ap.add_argument("--ghidra", type=pathlib.Path, metavar="OUT",
                    help="write a GridNodeSync import (functions to create, bookmarks, comments) instead of the summary")
    ap.add_argument("--exe", type=pathlib.Path,
                    help="the game image (a dump, GridNode menu -> Game image) for function starts; "
                         "default: the newest dumps/*.exe next to the recordings folder")
    ap.add_argument("--depth", type=int, default=8, help="with --ghidra: frames per stack to bookmark (default 8)")
    ap.add_argument("--classes", choices=("traced", "all", "none"), default="traced",
                    help="with --ghidra: class structs and vtable types for the recording's classes: the ones traced, "
                         "watched or sent events to (default), every class it mentions, or none")
    ap.add_argument("--reflected", type=pathlib.Path,
                    help="with --ghidra: your reflected dump (default: gridnode-reflected.json next to the recordings "
                         "folder, else db/reflected/)")
    ap.add_argument("--ghidra-names", metavar="EXPORT",
                    help="also name frames with your Ghidra's names (a GridNodeSync export)")
    ap.add_argument("--object", help="one object's story: an object id from the recording, or its address (0x...)")
    ap.add_argument("--diff", metavar="B", help="compare with recording B: what happens only (or far more) in this one")
    ap.add_argument("--diff-marker", metavar="NAME",
                    help="with --diff: B's marker, when it is named differently from --marker (\"*\": every marker)")
    ap.add_argument("--min-lift", type=float, default=2.0, help="with --diff: how many times more often counts as more (default 2)")
    ap.add_argument("--kinds", metavar="LIST",
                    help="with --diff: only these line kinds, e.g. event,sent,call (message, created, destroyed, event, "
                         "sent, anim, field, list, call)")
    ap.add_argument("--values", action="store_true",
                    help="with --diff: keep animation and field values and list contents in the labels")
    ap.add_argument("--diff-values", action="store_true",
                    help="with --diff: compare the numbers too (where one run stops changing, signs, where they part)")
    ap.add_argument("--alias", type=pathlib.Path, metavar="FILE",
                    help='names for animation writers: lines of "<address or call site> <name>"')
    ap.add_argument("--states", action="store_true", help="Faith's movement states over the whole recording")
    args = ap.parse_args()
    load_event_names(args.names or pathlib.Path(args.file).with_name("event-names.txt"))
    if args.alias:
        load_aliases(args.alias)

    header, events, objects, starts = read_recording(args.file)
    OBJECTS.update(objects)
    STARTS.update(starts)
    if not events:
        print("no events")
        return 1
    names = load_annotation_names()
    if args.ghidra_names:
        for va, n in load_ghidra_names(args.ghidra_names).items():
            names.setdefault(va, n)   # annotations first, then your Ghidra's names
    FN_NAMES.update(names)
    # Function starts the recording carries in "callers" (frames in reflected classes' vtable functions).
    for e in events:
        for c in e.get("callers", []):
            if c.get("fn"):
                STARTS.setdefault(int(c["at"], 16), int(c["fn"], 16))
                OWNERS.setdefault(int(c["fn"], 16), c["in"])
    global IMAGE
    exe = args.exe
    if exe is None:  # a dump next to the recordings folder (bin/dumps), newest first
        dumps = sorted(pathlib.Path(args.file).resolve().parent.parent.glob("dumps/*.exe"), key=lambda p: p.stat().st_mtime, reverse=True)
        exe = dumps[0] if dumps else None
    if exe is not None:
        try:
            IMAGE = _load_image(exe)
        except (OSError, ValueError) as e:
            print(f"(game image {exe} not used: {e})", file=sys.stderr)
    if args.object:
        return object_story(events, args.object)
    if args.diff:
        return diff(args, events, names)
    if args.ghidra:
        if args.marker:
            at = [e["t"] for e in events if e.get("kind") == "marker" and e.get("name") == args.marker]
            events = [e for e in events if any(abs(e["t"] - t) <= args.window for t in at)]
        img = IMAGE
        doc = to_ghidra(events, pathlib.Path(args.file).name, img, args.depth, header)
        if args.classes != "none":
            ref = args.reflected
            if ref is None:
                for cand in (pathlib.Path(args.file).resolve().parent.parent / "gridnode-reflected.json",
                             ROOT / "db" / "reflected" / "gridnode-reflected.json"):
                    if cand.exists():
                        ref = cand
                        break
            names = recording_classes(events, args.classes)
            doc["structs"] = class_structs(names, ref)
            print(f"  {len(doc['structs'])} class structs ({sum('vtable' in s for s in doc['structs'])} with a vtable) for "
                  f"{len(names)} classes" + ("" if ref else " (no reflected dump found: annotated classes only)"))
        args.ghidra.parent.mkdir(parents=True, exist_ok=True)
        args.ghidra.write_text(json.dumps(doc, indent=1), encoding="utf-8")
        st = doc["stats"]
        print(f"{args.ghidra}: {len(doc['createFunctions'])} function starts, {len(doc['bookmarks'])} bookmarks, "
              f"{len(doc['comments'])} function comments")
        if img is None and not STARTS:
            print("  no function starts (no --exe, no dump in ../dumps, an older recording): bookmarks only")
        elif st["frames_without_unwind_entry"]:
            print(f"  {st['frames_without_unwind_entry']} frames had no unwind entry (bookmarked only)")
        print("Import it in Ghidra with GridNodeSync (Import, or script arguments: import FILE).")
        return 0
    span = events[-1]["t"] - events[0]["t"]

    print(f"{args.file}")
    if header:
        print(f"  level {header.get('level')}, GridNode {header.get('gridnode')}, "
              f"{header.get('events')} events, {header.get('dropped_oldest', 0)} dropped")
        w = header.get("watch_readings")
        if w:
            rate = "every frame" if not w.get("rate") else f"{w['rate']} a second"
            print(f"  watches read {w['count']} times ({rate}): {w['average_ms']:.3f} ms average, {w['worst_ms']:.3f} ms worst")
    print(f"  {span:.1f} s recorded\n")

    if args.states:
        lines = state_timeline(events, events[0]["t"])
        print("Movement states (seconds from the start):")
        print("\n".join(lines) if lines else "    (no movement state recorded: Recorder, Faith's movement)")
        print()

    totals = collections.Counter(label(e) for e in events)
    print("Most frequent:")
    for lab, n in totals.most_common(args.top):
        print(f"  {n:7d}  {lab}")
    print()

    markers = [e for e in events if e.get("kind") == "marker" and (not args.marker or e.get("name") == args.marker)]
    if not markers:
        print("No markers. (Press Home in game while recording to drop one.)")
    rate_all = {lab: n / max(span, 1e-3) for lab, n in totals.items()}
    for mk in markers:
        t0 = mk["t"]
        near = [e for e in events if abs(e["t"] - t0) <= args.window and e is not mk]
        pos = mk.get("pos")
        print(f"=== {mk.get('name')} at {t0:.3f} s" + (f", Faith at ({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})" if pos else ""))
        counts = collections.Counter(label(e) for e in near)
        win = 2 * args.window
        # Unusual: seen near the marker far more than its average rate over the recording predicts.
        scored = []
        for lab, n in counts.items():
            expected = rate_all.get(lab, 0) * win
            lift = n / max(expected, 0.05)
            scored.append((lift, n, lab))
        scored.sort(reverse=True)
        shown = scored if args.all else [s for s in scored if s[0] >= 2.0]
        print(f"  {'all' if args.all else 'unusual'} within {args.window:g} s ({len(near)} events):")
        for lift, n, lab in shown[: args.top]:
            print(f"    {n:5d}  x{lift:5.1f}  {lab}")
        if not shown:
            print("    (nothing stands out; try --all or a wider --window)")
        # Timeline of the unusual ones, closest first.
        keep = {lab for _, _, lab in shown[: args.top]}
        # The closest ones; a reading repeated tick after tick is one line (it pushed real events out).
        timeline = sorted((e for e in near if label(e) in keep), key=lambda e: abs(e["t"] - t0))[: args.top * 8]
        if timeline:
            rows = []
            for e in sorted(timeline, key=lambda e: e["t"]):
                extra = ""
                if e.get("kind") == "message" and e.get("fields"):
                    extra = "  " + json.dumps(e["fields"])[:160]
                elif e.get("kind") == "message" and e.get("pointers"):
                    extra = "  -> " + ", ".join(p["type"] for p in e["pointers"][:4])
                if e.get("obj"):
                    extra = f"  #{e['obj']}" + extra
                rows.append((e, label(e), extra))
            folded = collapse(rows)
            folded = sorted(sorted(folded, key=lambda r: abs(r[0] - t0))[: args.top], key=lambda r: r[0])
            print("  closest:")
            for t, lab, extra, n, last in folded:
                rep = f"  (x{n} over {last - t:.2f} s)" if n > 1 else ""
                print(f"    {t - t0:+7.3f}  {lab}{extra}{rep}")
        states = state_timeline(near, t0)
        if states:
            print("  movement states:")
            print("\n".join(states))
        ranges = value_ranges(near, t0, args.top)
        if ranges:
            print("  numbers (range, peak, at the marker, at the end of the window; [readings]):")
            print("\n".join(ranges))
        # Call stacks behind them: the frames to open in Ghidra.
        stacks = collections.Counter()
        for e in near:
            if label(e) in keep and e.get("stack"):
                frames = tuple(int(x, 16) for x in e["stack"][:6])
                stacks[(label(e), frames)] += 1
        if stacks:
            print("  call stacks (innermost first):")
            for (lab, frames), n in stacks.most_common(min(args.top, 12)):
                print(f"    {n:4d}x  {lab}")
                for va in frames:
                    print(f"            {frame_text(va, names)}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
