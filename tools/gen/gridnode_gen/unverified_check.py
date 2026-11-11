"""GridNode's own code and the hypothesis annotations (plan 3.6). engine.hpp puts hypothesis items in
`unverified` namespaces. GridNode's code (sdk/src) may name one only when the file says so: a comment
`// unverified: <names> (<what depends on them>)` naming each item it uses. The comments are the list
of what is still to confirm; confirming an item in Ghidra or in game and marking it confirmed or
inferred removes it from engine.hpp's unverified namespaces, and then from the comment."""

from __future__ import annotations

import os
import re

_USE = re.compile(r"\bunverified::([A-Za-z_][A-Za-z0-9_]*)")
_NOTE = re.compile(r"^\s*//\s*unverified:")


def check(src_dir: str) -> list[str]:
    problems = []
    for dirpath, _dirs, files in os.walk(src_dir):
        for fn in sorted(files):
            if not fn.endswith((".cpp", ".hpp", ".h")):
                continue
            path = os.path.join(dirpath, fn)
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = f.read().split("\n")
            # A note runs from a "// unverified:" line over the comment lines after it.
            noted: set[str] = set()
            in_note = False
            for line in lines:
                if _NOTE.match(line):
                    in_note = True
                elif not line.lstrip().startswith("//"):
                    in_note = False
                if in_note:
                    noted.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", line))
            for n, line in enumerate(lines, 1):
                if line.lstrip().startswith("//"):
                    continue
                for name in _USE.findall(line):
                    if name not in noted:
                        problems.append(f"{os.path.relpath(path)}:{n}: uses unverified::{name} without a '// unverified:' note naming it")
    return problems
