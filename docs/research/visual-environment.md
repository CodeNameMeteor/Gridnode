# The visual environment

The game's look (sun, tonemap, colour, sky, wind, depth of field, vignette, grain) is one state the
engine recomputes every frame. GridNode's `visual_env_*` (API 29) holds values in it.

**Where:** global `VisualEnvMain` (0x142142a48) `+0x50 -> +0x90 -> +0x1c0 -> +0x30` is the manager; its
current state starts at `+0x85470`. Ghidra (2026-11-06): `0x14373e5d0` and `0x143742bd0` use that
offset; `0x143703d60` (7,487 bytes) is named in the synthesis as the per-frame blend, not checked.
The chain and the blocks below come from the cinematic tools' VisualsManager (offsets there are the GUI's
shared-memory ones minus 0x10), not yet read in the game by GridNode.

**Blocks** (offset in the state; fields are offsets in the block, floats unless said):

| Block | Offset | Fields |
|---|---|---|
| Outdoor light | +0x000 | sun colour +0x00 (3 f), sky colour +0x60, ground colour +0x70, sun rotation X +0x84, Y +0x88, intensity +0x94, angular radius +0x98, disk intensity +0x9c, specular scale +0xa4, penumbra +0xc4, enable +0x118 (u8), separate shadow direction +0x119 (u8) |
| Tonemap | +0x1d0 | bloom scale +0x00, EV +0x94, exposure compensation +0x98, dark / light adaptation +0xa4 / +0xa8, min / max EV +0xb4 / +0xb8, method +0xc0, black / white point +0xc4 / +0xc8, shoulder / toe +0xd0 / +0xd4, auto exposure +0x148 (u8) |
| Colour correction | +0x330 | brightness +0x00, contrast +0x10, saturation +0x20 (3 f each), hue +0x34, max HDR +0x40, enable +0x44 (u8), grading +0x45 (u8) |
| Sky and clouds | +0x390 | cloud colours +0x10 / +0x20, sun size +0x118 / +0x11c, wind direction +0x150, altitudes +0x154 / +0x190, speeds +0x160 / +0x19c |
| Wind | +0x6e0 | direction +0x6e4, strength +0x6e8, variation +0x6ec / +0x6f0, turbulence +0x6f8 / +0x6fc |
| Depth of field | +0xa50 | focus distance +0x04, blur factor +0x08, blur add +0x0c, simple near / far +0x18..0x24, sprite near / far +0x38..0x44, sprite max blur +0x48, enable +0x58 (u8) |
| Vignette | +0xac0 | colour +0x00, scale +0x14, exponent +0x1c, opacity +0x20, enable +0x24 (u8) |
| Film grain | +0xb00 | colour scale +0x00, texture scale +0x14, enable +0x28 (u8), linear filtering +0x29 (u8) |

**Sprite depth of field:** the method sits in the render settings (manager `+0x138`, `+0x154` int,
`+0x247` u8); the cinematic tools put the game's own value back after their writes, and patch a jump at
`0x1435c72ee` to stop bokeh artefacts. GridNode does neither.

**Open:** the cinematic tools write before and after the game's visual environment update. GridNode
writes after each world update only; if a held value flickers, find that update's caller and hook it.
