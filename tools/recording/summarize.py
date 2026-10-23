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
"""

import argparse
import collections
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load_annotation_names():
    """va -> name for every annotated function / global / code site (exact addresses)."""
    names = {}
    pat = re.compile(r"name:\s*([A-Za-z0-9_]+).*?va:\s*(0x[0-9a-fA-F]+)", re.S)
    for f in (ROOT / "db" / "annotations").glob("*.yaml"):
        text = f.read_text(encoding="utf-8", errors="replace")
        # inline maps: { name: X, va: 0x... }
        for m in re.finditer(r"\{\s*name:\s*([A-Za-z0-9_]+)\s*,\s*va:\s*(0x[0-9a-fA-F]+)", text):
            names[int(m.group(2), 16)] = m.group(1)
        # block entries: "- name: X\n    va: 0x..."
        for m in re.finditer(r"-\s*name:\s*([A-Za-z0-9_]+)\s*\n\s+va:\s*(0x[0-9a-fA-F]+)", text):
            names[int(m.group(2), 16)] = m.group(1)
    return names


def djb2(s):
    h = 5381
    for c in s.encode("utf-8"):
        h = ((h * 33) ^ c) & 0xFFFFFFFF
    return h


EVENT_NAMES = {}
FN_NAMES = {}  # function VA -> annotated name (set in main)


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


def event_label(ev):
    name, eid = ev.get("event", "?"), ev.get("event_id")
    if eid and name == eid:
        name = EVENT_NAMES.get(int(eid, 16), name)
    return name


def label(ev):
    k = ev.get("kind")
    if k == "message":
        return "msg " + ev.get("type", "?")
    if k == "created":
        bp = ev.get("blueprint")
        return "new " + ev.get("type", "?") + (f" [{bp}]" if bp else "")
    if k == "event":
        return f"event {event_label(ev)} -> {ev.get('target', '?')}"
    if k == "anim":
        prop = ev.get("property")
        pid = ev.get("property_id")
        if pid and prop == pid:
            prop = EVENT_NAMES.get(int(pid, 16), prop)
        val = ev.get("value")
        if isinstance(val, list):
            val = "(" + " ".join(f"{v:.2f}" for v in val[:3]) + (" ..." if len(val) > 3 else "") + ")"
        return f"anim {prop or '(game code)'} = {val} -> {ev.get('target', '?')}"
    if k == "field":
        return f"field {ev.get('field', '?')} = {ev.get('value')} -> {ev.get('target', '?')}"
    if k == "sent":
        return f"SENT {event_label(ev)} -> {ev.get('target', '?')}"
    if k == "call":
        fn = int(ev.get("fn", "0"), 16)
        named = FN_NAMES.get(fn)
        return f"call {ev.get('class', '?')}::vf+{ev.get('slot', '?')}" + (f" ({named})" if named else f" (0x{fn:x})")
    if k == "marker":
        return "marker " + ev.get("name", "")
    if k == "level":
        return "level " + ev.get("level", "")
    return k or "?"


def frame_text(va, names):
    n = names.get(va)
    return f"0x{va:x}" + (f" ({n})" if n else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--window", type=float, default=3.0, help="seconds before and after each marker (default 3)")
    ap.add_argument("--marker", help="only this marker (by name)")
    ap.add_argument("--top", type=int, default=25, help="lines per list (default 25)")
    ap.add_argument("--all", action="store_true", help="list every event near the marker, not only the unusual ones")
    ap.add_argument("--names", type=pathlib.Path, help="event-names.txt (default: the one next to the recording)")
    args = ap.parse_args()
    load_event_names(args.names or pathlib.Path(args.file).with_name("event-names.txt"))

    header, events = None, []
    with open(args.file, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            ev = json.loads(line)
            if ev.get("kind") == "header":
                header = ev
            else:
                events.append(ev)
    if not events:
        print("no events")
        return 1
    names = load_annotation_names()
    FN_NAMES.update(names)
    span = events[-1]["t"] - events[0]["t"]

    print(f"{args.file}")
    if header:
        print(f"  level {header.get('level')}, GridNode {header.get('gridnode')}, "
              f"{header.get('events')} events, {header.get('dropped_oldest', 0)} dropped")
    print(f"  {span:.1f} s recorded\n")

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
        timeline = sorted((e for e in near if label(e) in keep), key=lambda e: abs(e["t"] - t0))[: args.top]
        if timeline:
            print("  closest:")
            for e in sorted(timeline, key=lambda e: e["t"]):
                extra = ""
                if e.get("kind") == "message" and e.get("fields"):
                    extra = "  " + json.dumps(e["fields"])[:160]
                elif e.get("kind") == "message" and e.get("pointers"):
                    extra = "  -> " + ", ".join(p["type"] for p in e["pointers"][:4])
                print(f"    {e['t'] - t0:+7.3f}  {label(e)}{extra}")
        # Call stacks behind them: the frames to open in Ghidra.
        stacks = collections.Counter()
        for e in near:
            if label(e) in keep and e.get("stack"):
                frames = tuple(int(x, 16) for x in e["stack"][:6])
                stacks[(label(e), frames)] += 1
                # GridNode names frames inside a reflected class's vtable function ("callers").
                for c in e.get("callers", []):
                    names.setdefault(int(c["at"], 16), c["in"])
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
