# @TITLE@

@DESCRIPTION@ A mod for [GridNode](https://github.com/CodeNameMeteor/Gridnode), the modding SDK for
Mirror's Edge Catalyst (PC).

## Install

Install GridNode (its release zip), then unzip this mod's zip next to `gridnode.dll`: it goes in
`mods/@NAME@/`. In game, @OPEN_KEY@ opens its window.

## Build

Needs Visual Studio 2022 (C++ desktop) and CMake. The GridNode SDK is in `gridnode-sdk/`.

```text
cmake -S . -B build -A x64
cmake --build build --config Release
python gridnode-sdk/templates/gridnode_mod.py pack build/Release/@NAME@.dll --version 0.1.0
```

To try it, copy `build/Release/@NAME@.dll` into GridNode's `mods/@NAME@/`. With the game running,
GridNode's menu (INSERT) → Mods → Reload loads a new build. A newer GridNode SDK:
`python gridnode-sdk/templates/gridnode_mod.py update-sdk --sdk <new release>/sdk`.
