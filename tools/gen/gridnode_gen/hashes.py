"""Frostbite name hashes: hash names, and turn hashes back into names with a dictionary.

Frostbite stores many names only as a 32-bit hash: property and event ids in blueprint
connections (PropertyConnection.SourceFieldId / TargetFieldId, EventConnection.SourceEvent /
TargetEvent), and the names in EBX type descriptors. The hash is h = 5381; h = (h * 33) ^ c over
the bytes (what Frosty calls Fnv1). A hash cannot be reversed, only looked up: the dictionary is
every name GridNode knows (reflected types, fields and enum values, annotations), optionally every
string in the game image, and any word lists you add.
"""

from __future__ import annotations

import re

from .ctypes_ import djb2_xor

name_hash = djb2_xor


def _variants(name: str):
    yield name
    # Enum values are often hashed without their type prefix (FieldAccessType_Source -> Source).
    if "_" in name:
        yield name.split("_", 1)[1]
        yield name.rsplit("_", 1)[1]


def dictionary(reflected=None, db=None, image=None, word_files=(), min_len=2) -> dict[int, str]:
    names: set[str] = set()
    if reflected is not None:
        for t in reflected.types.values() if isinstance(reflected.types, dict) else reflected.types:
            for key in ("name", "displayName"):
                v = t.get(key) if isinstance(t, dict) else getattr(t, key, None)
                if v:
                    names.update(_variants(v))
            fields = t.get("fields", []) if isinstance(t, dict) else getattr(t, "fields", [])
            for f in fields:
                n = f.get("name") if isinstance(f, dict) else getattr(f, "name", None)
                if n:
                    names.add(n)
            values = t.get("values", []) if isinstance(t, dict) else getattr(t, "values", [])
            for v in values:
                n = v.get("name") if isinstance(v, dict) else getattr(v, "name", None)
                if n:
                    names.update(_variants(n))
    if db is not None:
        for c in db.classes.values():
            names.add(c.name)
            names.update(m.name for m in c.members)
            names.update(p.name for p in c.properties)
        names.update(db.structs)
    if image is not None:
        for s in re.findall(rb"[\x20-\x7e]{%d,64}" % max(min_len, 3), image):
            text = s.decode("ascii")
            names.add(text)
            # Identifiers inside longer strings ("fb::ClientCharacterEntity", "Levels/SP/...").
            names.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]{%d,}" % (min_len - 1), text))
    for path in word_files:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                w = line.strip()
                if w and not w.startswith("#"):
                    names.add(w)
    out: dict[int, str] = {}
    for n in sorted(names, key=lambda s: (len(s), s)):  # shortest name wins a collision
        out.setdefault(name_hash(n), n)
    return out


def parse_hash(text: str) -> int:
    text = text.strip()
    v = int(text, 16) if text.lower().startswith("0x") else int(text, 0) if text.isdigit() or text.startswith("-") else int(text, 16)
    return v & 0xFFFFFFFF  # Frosty shows them signed too
