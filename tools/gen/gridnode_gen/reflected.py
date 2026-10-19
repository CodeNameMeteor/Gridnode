"""The reflected layer: a gridnode-reflected JSON dump (docs/dump-format.md)."""

from __future__ import annotations

import json
import re

RESERVED = set("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class
compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype
default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline
int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public register
reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch template
this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t
while xor xor_eq
""".split())


def c_ident(s: str) -> str:
    out = re.sub(r"[^A-Za-z0-9_]", "_", s)
    if not out or out[0].isdigit():
        out = "_" + out
    if out in RESERVED:
        out += "_"
    return out


def addr(v) -> int:
    return int(v, 16) if isinstance(v, str) else int(v or 0)


class Reflected:
    def __init__(self, doc: dict, path: str = ""):
        if doc.get("format") != "gridnode-reflected":
            raise ValueError(f"{path}: not a gridnode-reflected dump")
        if doc.get("version") != 1:
            raise ValueError(f"{path}: unsupported dump version {doc.get('version')}")
        self.path = path
        self.doc = doc
        self.types: list[dict] = doc["types"]
        self.by_ti: dict[int, dict] = {}
        for t in self.types:
            t["_ti"] = addr(t["ti"])
            t["_super"] = addr(t.get("super"))
            t["_element"] = addr(t.get("element"))
            for f in t.get("fields", []):
                f["_type"] = addr(f.get("type"))
            self.by_ti[t["_ti"]] = t
        self._assign_idents()
        # Names as written in annotations: the display name (first in ti order wins), and idents.
        self.by_name: dict[str, dict] = {}
        for t in self.types:
            self.by_name.setdefault(t["displayName"], t)
        for t in self.types:
            self.by_name.setdefault(t["_ident"], t)

    @classmethod
    def load(cls, path: str) -> "Reflected":
        with open(path, encoding="utf-8") as f:
            return cls(json.load(f), path)

    def _assign_idents(self):
        """Unique C++ identifiers: the display name, made an identifier; the module is appended when
        two types share a name (e.g. BoneNameID in two modules), then a counter."""
        used: set[str] = set()
        order = sorted(self.types, key=lambda t: (t["category"], t["displayName"], t["_ti"]))
        for t in order:
            base = c_ident(t["displayName"])
            ident = base
            if ident in used and t.get("module"):
                ident = f"{base}_{c_ident(t['module'])}"
            n, stem = 2, ident
            while ident in used:
                ident = f"{stem}_{n}"
                n += 1
            used.add(ident)
            t["_ident"] = ident

    def get(self, ti: int) -> dict | None:
        return self.by_ti.get(ti)

    def super_of(self, t: dict) -> dict | None:
        return self.by_ti.get(t["_super"]) if t.get("_super") else None

    def chain(self, t: dict) -> list[dict]:
        out, seen = [], set()
        while t is not None and t["_ti"] not in seen:
            out.append(t)
            seen.add(t["_ti"])
            t = self.super_of(t)
        return out
