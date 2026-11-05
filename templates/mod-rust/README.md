# @TITLE@

@DESCRIPTION@ A mod for [GridNode](https://github.com/CodeNameMeteor/Gridnode), the modding SDK for
Mirror's Edge Catalyst (PC).

## Install

Install GridNode (its release zip), then unzip this mod's zip next to `gridnode.dll`: it goes in
`mods/@NAME@/`.

## Build

Needs Rust (the x86_64-pc-windows-msvc target, the default on Windows). The GridNode crate is in
`gridnode-sdk/rust/gridnode`.

```text
cargo build --release
python gridnode-sdk/templates/gridnode_mod.py pack target/release/@NAME@.dll --version 0.1.0
```

A newer GridNode SDK: `python gridnode-sdk/templates/gridnode_mod.py update-sdk --sdk <new release>/sdk`.
