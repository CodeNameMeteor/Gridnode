#!/usr/bin/env python3
"""Build GridNode's event-name dictionary: names for the event and property hashes the game uses.

Frostbite names events and properties by a hash of their name (djb2 with xor: h = h * 33 ^ c,
from 5381). The game keeps only the hashes, so the Recorder shows most events as hex. This tool
collects candidate names from what you have locally, checks each against the hashes, and writes
the names that match to event-names.txt, which the Recorder (and tools/recording/summarize.py)
read.

    python tools/events/build_names.py --xml "PreFindings/docs/XML Dump" \
        --reflected bin/gridnode-reflected.json --exe PreFindings/MirrorsEdgeCatalyst_dump_SCY.exe \
        --recordings bin/recordings --out bin/recordings/event-names.txt --report event-names-report.md

Every input is optional; use what you have:

  --xml         an EBX dump as XML (Frosty's export). Event and property ids there are either
                resolved ("In [00597302]", checked here, as Frosty's own dictionary has wrong
                guesses) or bare hashes ("0x0ecd6d47"), which become the hashes to find names for.
                Names of objects, fields and enum values in the dump are candidates too.
  --reflected   GridNode's reflected type dump: every type, field and enum value name.
  --exe         the game executable (or a dump of it): its strings.
  --recordings  Recorder exports (*.jsonl): their unnamed events are hashes to find names for.
  --names       more candidate names, one per line (repeatable).
  --combine N   also try two-word names made of the N most common words (default 1500; 0: off).

Nothing here is game data, but what it writes is derived from it: keep the outputs local.
`gridnode hash NAME` and `gridnode unhash HASH` (tools/gen) hash one name or look one hash up.
"""

import argparse
import collections
import json
import pathlib
import re
import sys

MASK = 0xFFFFFFFF

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "gen"))
try:  # the same hash as `gridnode hash` / `gridnode unhash` (tools/gen)
    from gridnode_gen.hashes import name_hash as djb2  # noqa: E402
except ImportError:  # run on its own
    def djb2(s: str) -> int:
        h = 5381
        for c in s.encode("utf-8", "surrogateescape"):
            h = ((h * 33) ^ c) & MASK
        return h


def extend(h: int, s: str) -> int:
    for c in s.encode("utf-8", "surrogateescape"):
        h = ((h * 33) ^ c) & MASK
    return h


RESOLVED = re.compile(r"<Id>(.*?) \[([0-9A-Fa-f]{8})\]</Id>")
BARE = re.compile(r"<Id>0x([0-9A-Fa-f]{1,8})</Id>")
TAGVAL = re.compile(r"<(\w+)>([^<>\n]{1,80})</\1>")
REFCLASS = re.compile(r"\[(\w+)\] [0-9a-f-]{36}")
IDENT = re.compile(rb"[A-Za-z_][A-Za-z0-9_]{1,63}")
WORD = re.compile(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|\d+")


def words_of(name: str):
    return WORD.findall(name)


class Book:
    def __init__(self):
        self.names = {}                       # hash -> name (verified)
        self.wanted = collections.Counter()   # unnamed hash -> times seen
        self.context = collections.defaultdict(collections.Counter)  # hash -> "Source -> Target" counts
        self.candidates = set()   # names tried as they are, with affixes, and split into words
        self.plain = set()        # names tried only as they are (the executable's many strings)
        self.frosty_wrong = 0
        self.how = {}                         # hash -> how its name was found (not "as is": a guess)

    def add_name(self, name: str) -> bool:
        if not name:
            return False
        h = djb2(name)
        if h in self.names:
            return False
        self.names[h] = name
        return True


def harvest_xml(book: Book, root: pathlib.Path):
    files = sorted(root.rglob("*.xml"))
    for f in files:
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in RESOLVED.finditer(text):
            name, h = m.group(1), int(m.group(2), 16)
            book.wanted[h] += 1
            if djb2(name) == h:
                book.add_name(name)
            else:
                book.frosty_wrong += 1
        # Event connections: bare hashes, with the classes on both ends.
        for block in re.finditer(r"<(EventConnection|PropertyConnection|LinkConnection)>(.*?)</\1>", text, re.S):
            body = block.group(2)
            src = re.search(r"<Source>\[(\w+)\]", body)
            dst = re.search(r"<Target>\[(\w+)\]", body)
            ctx = f"{src.group(1) if src else '?'} -> {dst.group(1) if dst else '?'}"
            for m in BARE.finditer(body):
                h = int(m.group(1), 16)
                book.wanted[h] += 1
                book.context[h][f"{block.group(1)[:-10]}: {ctx}"] += 1
        for m in BARE.finditer(text):
            book.wanted[int(m.group(1), 16)] += 0  # seen outside connections too
        for m in TAGVAL.finditer(text):
            tag, val = m.group(1), m.group(2).strip()
            book.candidates.add(tag)
            if tag in ("Name", "FieldName", "EventName", "Id", "ValueName") or (len(val) < 48 and re.fullmatch(r"[A-Za-z_][\w ]*", val)):
                book.candidates.add(val)
                book.candidates.add(val.rsplit("/", 1)[-1])
        for m in REFCLASS.finditer(text):
            book.candidates.add(m.group(1))
    return len(files)


def harvest_reflected(book: Book, path: pathlib.Path):
    d = json.loads(path.read_text(encoding="utf-8"))
    n = 0
    for t in d.get("types", []):
        for k in ("name", "displayName"):
            if t.get(k):
                book.candidates.add(t[k])
                n += 1
        for f in t.get("fields", []) or []:
            if f.get("name"):
                book.candidates.add(f["name"])
                n += 1
        for v in t.get("values", []) or t.get("enumValues", []) or []:
            name = v.get("name") if isinstance(v, dict) else None
            if name:
                book.candidates.add(name)
                n += 1
    return n


def harvest_exe(book: Book, path: pathlib.Path):
    data = path.read_bytes()
    n = 0
    for m in IDENT.finditer(data):
        s = m.group(0).decode("ascii", "replace").strip()
        if 2 <= len(s) <= 64:
            book.plain.add(s)
            n += 1
    return n


def harvest_recordings(book: Book, root: pathlib.Path):
    n = 0
    for f in sorted(root.glob("*.jsonl")):
        with open(f, encoding="utf-8") as fh:
            for line in fh:
                if '"kind":"event"' not in line:
                    continue
                ev = json.loads(line)
                eid = ev.get("event_id")
                if not eid:
                    continue
                h = int(eid, 16)
                if ev.get("event") == eid:  # unnamed when recorded
                    book.wanted[h] += 1
                    book.context[h][f"recorded: -> {ev.get('target', '?')}"] += 1
                else:
                    book.wanted[h] += 1
                    book.add_name(ev["event"])
                n += 1
    return n


AFFIX_PRE = ["", "On", "Set", "Get", "Is", "Start", "Stop", "Enable", "Disable", "Show", "Hide", "Play", "Trigger", "Reset", "Request", "Force"]
AFFIX_SUF = ["", "ed", "d", "s", "Started", "Stopped", "Finished", "Changed", "Enabled", "Disabled", "Done", "Completed", "In", "Out", "Event"]


def crack(book: Book, combine: int):
    want = {h for h in book.wanted if h not in book.names}
    found = 0

    def hit(name, how="as is"):
        nonlocal found
        h = djb2(name)
        if h in want and h not in book.names:
            book.names[h] = name
            book.how[h] = how
            want.discard(h)
            found += 1

    for c in book.plain | book.candidates:  # as they are
        if djb2(c) in want:
            hit(c)
    for c in list(book.candidates):  # enum values are often hashed without their type prefix
        if "_" in c:
            book.candidates.add(c.split("_", 1)[1])
            book.candidates.add(c.rsplit("_", 1)[1])
    cands = {c for c in book.candidates if c and len(c) <= 48}
    # The candidates and their words (from every source), with common prefixes / suffixes.
    words = collections.Counter()
    for c in cands | book.plain:
        for w in words_of(c):
            if len(w) > 1:
                words[w[0].upper() + w[1:].lower() if w.isupper() and len(w) > 3 else w[0].upper() + w[1:]] += 1
    bases = cands | set(words)
    pres = [(p, djb2(p)) for p in AFFIX_PRE]
    for b in bases:
        for p, hp in pres:
            hb = extend(hp, b)
            for suf in AFFIX_SUF:
                if extend(hb, suf) in want:
                    hit(p + b + suf, "as is" if not p and not suf and b in cands else "affix")
    # Two words.
    if combine > 0 and want:
        top = [w for w, _ in words.most_common(combine)]
        for p in ("", "On"):
            hp = djb2(p)
            for w1 in top:
                h1 = extend(hp, w1)
                for w2 in top:
                    h = extend(h1, w2)
                    if h in want:
                        hit(p + w1 + w2, "two words")
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xml", type=pathlib.Path)
    ap.add_argument("--reflected", type=pathlib.Path)
    ap.add_argument("--exe", type=pathlib.Path)
    ap.add_argument("--recordings", type=pathlib.Path)
    ap.add_argument("--names", type=pathlib.Path, action="append", default=[])
    ap.add_argument("--combine", type=int, default=1500)
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("event-names.txt"))
    ap.add_argument("--report", type=pathlib.Path)
    args = ap.parse_args()

    book = Book()
    if args.xml:
        print(f"xml: {harvest_xml(book, args.xml)} files, {len(book.names)} names Frosty resolved "
              f"({book.frosty_wrong} of its guesses were wrong), {len(book.wanted)} hashes")
    if args.recordings:
        print(f"recordings: {harvest_recordings(book, args.recordings)} events")
    if args.reflected:
        print(f"reflected: {harvest_reflected(book, args.reflected)} names")
    if args.exe:
        print(f"exe: {harvest_exe(book, args.exe)} strings")
    for p in args.names:
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                book.candidates.add(line.split("\t")[-1])
    before = sum(1 for h in book.wanted if h in book.names)
    found = crack(book, args.combine)
    named = sum(1 for h in book.wanted if h in book.names)
    total = len(book.wanted)
    print(f"hashes seen: {total}; named: {named} ({found} found here, {before} already known); unnamed: {total - named}")

    # Everything the game is seen to use, named, plus every candidate that is a plain identifier
    # (the Recorder looks names up by hash, so extra names cost nothing but memory).
    def line(h):
        how = book.how.get(h, "data")
        return book.names[h] + ("" if how in ("data", "as is") else f"\t# guess ({how})")

    used = sorted((h for h in book.wanted if h in book.names), key=lambda h: book.names[h].lower())
    extra = sorted((h for h in book.names if h not in book.wanted), key=lambda h: book.names[h].lower())
    lines = ["# GridNode event and property names, generated by tools/events/build_names.py (keep it local:",
             "# it is made from game data). One name per line; after a tab, how a guessed name was found."]
    lines += [line(h) for h in used]
    lines += [line(h) for h in extra]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    guesses = sum(1 for h in used if book.how.get(h, "data") not in ("data", "as is"))
    print(f"wrote {args.out} ({len(used)} names in use, {guesses} of them guesses; {len(extra)} more)")

    if args.report:
        r = ["# Event names", "", f"{total} hashes seen, {named} named, {total - named} unnamed.", "",
             "## Unnamed, most used first", "", "| hash | seen | where |", "|---|---|---|"]
        for h, n in book.wanted.most_common():
            if h in book.names:
                continue
            where = "; ".join(f"{c} ×{k}" for c, k in book.context[h].most_common(3))
            r.append(f"| {h:08x} | {n} | {where} |")
        r += ["", "## Named", "", "Found by: *data* (named in the dump or a recording), *as is* (a name or string that",
              "matches exactly), *affix* (a name with On/Set/...ed etc. added) or *two words* (two common words",
              "joined): the last two are guesses that fit the hash, usually right but not always.", "",
              "| hash | name | found by | seen | where |", "|---|---|---|---|---|"]
        for h, n in book.wanted.most_common():
            if h in book.names:
                where = "; ".join(f"{c} ×{k}" for c, k in book.context[h].most_common(2))
                r.append(f"| {h:08x} | {book.names[h]} | {book.how.get(h, 'data')} | {n} | {where} |")
        args.report.write_text("\n".join(r) + "\n", encoding="utf-8")
        print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
