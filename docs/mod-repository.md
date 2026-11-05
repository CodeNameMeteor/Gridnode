# Your mod in its own repository

A GridNode mod does not need GridNode's source, its tools or this repository. It needs the **SDK**
from a GridNode release: the API headers (or the Rust crate) and, for a mod with a window, Dear
ImGui. `gridnode_mod.py new` copies that SDK into the mod, so the mod's folder is a repository of its
own that builds on any PC and in GitHub Actions.

## Start one

Download a GridNode release zip and unzip it anywhere. Then, where you keep your projects:

```text
python <GridNode release>/sdk/templates/gridnode_mod.py new my_mod            C++ (CMake)
python <GridNode release>/sdk/templates/gridnode_mod.py new my_mod --rust     Rust (Cargo)
cd my_mod
git init && git add . && git commit -m "My mod"
```

What you get (C++):

```text
my_mod/
  CMakeLists.txt              builds my_mod.dll against gridnode-sdk/
  src/my_mod.cpp              the mod: a window, a setting, its own key (F6), a client tick
  gridnode-sdk/               the SDK, copied in: commit it
    include/gridnode/         api.h, mod.hpp, ui.hpp, engine.hpp (the game's addresses GridNode knows)
    imgui/                    Dear ImGui, the same copy GridNode draws with
    templates/gridnode_mod.py pack and update-sdk
    VERSION                   the GridNode release it came from
  .github/workflows/build.yml builds on every push and keeps the mod's zip as an artifact
  README.md, .gitignore
```

A Rust mod has `Cargo.toml`, `src/lib.rs` and `gridnode-sdk/rust/gridnode` (the crate) instead.
`--no-vendor` points the build at the SDK where it is rather than copying it (for a quick test).

Committing `gridnode-sdk/` (about 3 MB for C++, under 1 MB for Rust) is what makes the repository
build anywhere: a clone builds with no download and no path to set, and the mod is always built
against the same SDK. It holds no game data: `engine.hpp` is GridNode's own annotations.

## Build it

C++: Visual Studio 2022 (the "Desktop development with C++" workload) and CMake.

```text
cmake -S . -B build -A x64
cmake --build build --config Release          -> build/Release/my_mod.dll
```

Rust: `cargo build --release` -> `target/release/my_mod.dll` (on Windows; the
`x86_64-pc-windows-msvc` target is the default there).

MinGW works too (`-G Ninja` with `x86_64-w64-mingw32-g++`); the template links the C++ runtime in,
so the DLL needs nothing else on the player's PC.

## Try it in the game

Install a GridNode release (its zip, unzipped anywhere: [Getting started](getting-started.md)) and
copy your DLL into its `mods/my_mod/` folder. Start the game, run `gridnode_inject.exe`, press
INSERT (or F6, the template's own key).

While the game runs: rebuild, copy the DLL over the old one, and GridNode's menu → Mods → Reload
(or tick "when changed" to reload on every copy). The developer build's tools (the inspector, the
console, the Recorder) help while you make the mod; players never need them.

## Only your window

Players see your mod's windows in GridNode's menu. To show only yours:

- **Your own key.** `gn::ui::setMenuOpen(open)` (API 27) opens the overlay as INSERT does, so a
  hotkey of your mod opens it with your window. The template does this with F6, and its window has
  the row where the player rebinds it:

  ```cpp
  gn::hotkey("open", "Open My mod", VK_F6, [] { gn::ui::setMenuOpen(!gn::ui::menuOpen()); });
  gn::ui::hotkeyRow("open", "Open this window"); // in your window
  ```

- **No GridNode window.** Shipping GridNode with your mod, put a `gridnode.ini` next to
  `gridnode.dll`:

  ```ini
  [ui]
  gridnode_window=0   ; the menu shows only the mods' windows
  menu_key=NONE       ; INSERT does nothing: your key opens it (leave this out to keep INSERT)
  ```

- **No developer tools.** Ship the **player build** of GridNode (the players' zip,
  `GridNode-player-*.zip`): the loader, the API and the mods' windows, without the inspector,
  console, Recorder, editor or the type dump ([Shipping a mod](shipping.md)).

## Ship it

```text
python gridnode-sdk/templates/gridnode_mod.py pack build/Release/my_mod.dll --version 1.0.0 --file data/
```

makes `my-mod-1.0.0.zip` with `mods/my_mod/my_mod.dll` and its files: players unzip it next to
`gridnode.dll`. Attach it to a GitHub release of your repository (the workflow's artifact is the
same zip). Say which GridNode it needs: the one in `gridnode-sdk/VERSION` or newer.

## A newer GridNode

```text
python gridnode-sdk/templates/gridnode_mod.py update-sdk --sdk <new GridNode release>/sdk
```

replaces `gridnode-sdk/` with the new release's SDK; rebuild and commit. The API only grows, so a
mod built for an older GridNode keeps loading on newer ones; one built with the new SDK needs that
GridNode or newer (an older one refuses it, and its log says "built for a newer GridNode").

## What a mod should not do

- Patch the game's code or hook its functions outside GridNode's API (`gn::hookSlot`,
  `gn::hookFunction`, `gn::patchBytes`): those are listed in the menu, shared with other mods, and
  undone when the mod unloads.
- Change online play without `gn::allowOnline` ([GridNode and online play](online.md)).
- Ship anything of the game's (its files, a dump of it, extracted meshes).
