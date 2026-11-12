"""The engine's TypeInfo list head, found in the game's code instead of taken from the annotations.

Every reflected type registers itself the same way when the game starts: it loads the list head,
stores it as its own `next` (TypeInfo +0x08), and stores itself as the new head:

    48 8b 05 rel32      mov rax, [head]
    48 89 05 rel32      mov [type + 8], rax
    48 8d 05 rel32      lea rax, [type]
    48 89 05 rel32      mov [head], rax

A match counts only when the first and last operands name the same address and the second is the
third plus 8, so unrelated code with the same opcodes is left out. In Catalyst's image 27 sites match,
all naming 0x14215d028, the annotated TypeInfoListHead (2026-11-11). The idea of finding the head by
the code that links into it is the one Frosty's SDK uses; this pattern was read off the game's own
image, not taken from Frosty. It also finds the head in another Frostbite 3 game's image without an
address (the dump tool runs on one build only by decision: this is for checks and research).
"""

from __future__ import annotations

import re
import struct
from collections import Counter

from .pe_image import PeImage

_LINK = re.compile(rb"\x48\x8b\x05(....)\x48\x89\x05(....)\x48\x8d\x05(....)\x48\x89\x05(....)", re.S)


def find_in(code: bytes, va: int) -> Counter:
    """Heads named by the registration sites in one block of code at `va`: head -> sites."""
    heads: Counter = Counter()
    for m in _LINK.finditer(code):
        at = va + m.start()
        head = at + 7 + struct.unpack("<i", m.group(1))[0]
        nxt = at + 14 + struct.unpack("<i", m.group(2))[0]
        ti = at + 21 + struct.unpack("<i", m.group(3))[0]
        head2 = at + 28 + struct.unpack("<i", m.group(4))[0]
        if head == head2 and nxt == ti + 8:
            heads[head] += 1
    return heads


def find(img: PeImage) -> tuple[int, int, int]:
    """(head, sites naming it, sites naming anything else); head 0 when no site matches."""
    heads: Counter = Counter()
    for va, _vsize, raw, raw_size, executable, _name in img.sections:
        if executable and raw_size:
            heads += find_in(img.data[raw:raw + raw_size], va)
    if not heads:
        return 0, 0, 0
    head, n = heads.most_common(1)[0]
    return head, n, sum(heads.values()) - n
