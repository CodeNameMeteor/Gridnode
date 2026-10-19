"""The small type language used in annotations.

    u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool void      primitives
    Name                                                 a reflected type or an annotation struct
    T*  T**  const T*                                    pointers
    T[N]                                                 fixed array (N decimal or 0x hex)

Signatures: "ret(type name, type name, ...)"; parameter names are optional.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

PRIMITIVES = {
    # name: (C++ type, size, alignment)
    "u8": ("uint8_t", 1, 1), "u16": ("uint16_t", 2, 2), "u32": ("uint32_t", 4, 4), "u64": ("uint64_t", 8, 8),
    "i8": ("int8_t", 1, 1), "i16": ("int16_t", 2, 2), "i32": ("int32_t", 4, 4), "i64": ("int64_t", 8, 8),
    "f32": ("float", 4, 4), "f64": ("double", 8, 8), "bool": ("bool", 1, 1), "void": ("void", 0, 1),
}

_TYPE_RE = re.compile(r"^\s*(const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*((?:\*\s*)*)(?:\[\s*(0x[0-9a-fA-F]+|\d+)\s*\])?\s*$")


@dataclass(frozen=True)
class TypeRef:
    base: str            # primitive or type name
    pointers: int = 0
    const: bool = False
    count: int | None = None  # fixed array length

    @property
    def is_primitive(self) -> bool:
        return self.base in PRIMITIVES

    @property
    def is_pointer(self) -> bool:
        return self.pointers > 0

    def named_type(self) -> str | None:
        """The non-primitive type this refers to, if any."""
        return None if self.is_primitive else self.base

    def __str__(self) -> str:
        s = ("const " if self.const else "") + self.base + "*" * self.pointers
        return s + (f"[{self.count}]" if self.count is not None else "")


class TypeError_(ValueError):
    pass


def parse_type(text: str) -> TypeRef:
    m = _TYPE_RE.match(str(text))
    if not m:
        raise TypeError_(f"not a type: {text!r}")
    const, base, stars, count = m.groups()
    n = int(count, 0) if count else None
    if n is not None and n <= 0:
        raise TypeError_(f"array length must be positive: {text!r}")
    t = TypeRef(base=base, pointers=stars.count("*"), const=bool(const), count=n)
    if t.base == "void" and not t.is_pointer:
        if t.count is not None:
            raise TypeError_(f"void array: {text!r}")
    return t


@dataclass(frozen=True)
class Param:
    type: TypeRef
    name: str | None


@dataclass(frozen=True)
class Signature:
    returns: TypeRef
    params: tuple[Param, ...] = field(default_factory=tuple)

    def named_types(self) -> set[str]:
        out = {p.type.named_type() for p in self.params} | {self.returns.named_type()}
        return {n for n in out if n}


_SIG_RE = re.compile(r"^\s*(.+?)\s*\((.*)\)\s*$")


def parse_signature(text: str) -> Signature:
    m = _SIG_RE.match(str(text))
    if not m:
        raise TypeError_(f"not a signature: {text!r}")
    ret = parse_type(m.group(1))
    params = []
    body = m.group(2).strip()
    if body and body != "void":
        for i, part in enumerate(body.split(",")):
            part = part.strip()
            # "type name": the name is the last identifier, unless the whole thing is just a type.
            pm = re.match(r"^(.*?[\*\s])\s*([A-Za-z_][A-Za-z0-9_]*)$", part)
            if pm and pm.group(1).strip():
                try:
                    params.append(Param(parse_type(pm.group(1)), pm.group(2)))
                    continue
                except TypeError_:
                    pass
            params.append(Param(parse_type(part), None))
    return Signature(ret, tuple(params))


def djb2_xor(name: str) -> int:
    """Frostbite's field / event / property name hash: h = 5381; h = (h * 33) ^ c."""
    h = 5381
    for c in name.encode("utf-8"):
        h = ((h * 33) ^ c) & 0xFFFFFFFF
    return h
