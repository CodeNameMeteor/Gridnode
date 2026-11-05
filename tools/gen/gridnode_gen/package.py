"""gridnode package: a release zip from a Windows build.

  GridNode-<version>/      unzip anywhere: GridNode runs from its own folder
    gridnode.dll
    gridnode_inject.exe    run it with the game running: loads gridnode.dll from this folder
    mods/                  (with --examples: mods/<name>/<name>.dll for each example mod)
    start-with-the-game/
      version.dll          only to load GridNode when the game starts: then gridnode.dll, mods\ and
                           this version.dll go in the game folder (the only reason to touch it)
    sdk/
      include/gridnode/    the API (api.h, mod.hpp, ui.hpp, ...) and engine.hpp (our annotations)
      imgui/               Dear ImGui, the copy GridNode draws with (mods that draw build it in)
      rust/gridnode/       the Rust bindings crate
      templates/           gridnode_mod.py (new / pack) and the C++ and Rust mod templates
    README.txt, LICENSE, THIRD_PARTY_NOTICES.md

Nothing of the game's goes in: no dump, no types.hpp (made from the reflected dump), no game files.

With --player (build_player): GridNode-player-<version>.zip for players: the player build of
gridnode.dll (the loader, the API and mods' windows; no developer tools), the injector, version.dll,
the MEC trainer, and a README for players. No SDK.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import zipfile

# Headers a mod may include (the rest of sdk/include is GridNode's own).
PUBLIC_HEADERS = ["api.h", "mod.hpp", "ui.hpp", "projection.hpp", "version.hpp", "config.hpp", "x64.hpp", "mod_order.hpp"]
IMGUI = ["imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp", "imgui.h", "imgui_internal.h", "imconfig.h",
         "imstb_rectpack.h", "imstb_textedit.h", "imstb_truetype.h", "LICENSE.txt"]
EXAMPLES = ["hello", "mec_trainer", "world_draw", "detour_demo"]  # the research ones (probe, the old trainer) stay out
PLAYER_MODS = ["mec_trainer"]
DEV_MARKER = b"recorder: armed: recording starts"  # a string only the developer build's gridnode.dll has


def version(root: str) -> str:
    """The CMake project version, with the git commit when there is one: 0.1.0+ccd4784."""
    with open(os.path.join(root, "CMakeLists.txt"), encoding="utf-8") as f:
        m = re.search(r"project\(gridnode VERSION ([0-9.]+)", f.read())
    v = m.group(1) if m else "0.0.0"
    try:
        sha = subprocess.run(["git", "-C", root, "rev-parse", "--short", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", root, "status", "--porcelain", "--untracked-files=no"], capture_output=True, text=True).stdout.strip()
        v += f"+{sha}" + (".dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        pass
    return v


README = """GridNode {version}: a modding SDK for Mirror's Edge Catalyst (PC, the EA app build)

Install
  Unzip this folder anywhere (not in the game's folder). Start the game, then run
  gridnode_inject.exe: it loads GridNode from this folder, and INSERT opens its menu. GridNode writes
  gridnode.log here. It stays passive on another build of the game.

  To have it load with the game every time instead: copy start-with-the-game\\version.dll,
  gridnode.dll and mods\\ into the game's folder (where MirrorsEdgeCatalyst.exe is). Only for that.

Mods
  Put a mod in mods\\ (next to gridnode.dll): a DLL (mods\\name.dll), or a mod's own folder from its
  zip (mods\\name\\name.dll and its files). Mods made for a newer GridNode refuse to load.

Making a mod
  python sdk\\templates\\gridnode_mod.py new my_mod            a C++ mod (CMake), in a folder of its own
  python sdk\\templates\\gridnode_mod.py new my_mod --rust     a Rust mod (Cargo)
    The SDK is copied into the mod (gridnode-sdk\\), so the folder is a repository that builds anywhere,
    with a GitHub Actions workflow: docs/mod-repository.md in the repository.
  python sdk\\templates\\gridnode_mod.py pack my_mod.dll       a zip others unzip next to gridnode.dll
  The API: sdk\\include\\gridnode\\mod.hpp (C++), sdk\\rust\\gridnode (Rust); the guide is docs/modding.md
  in the repository: {repo}

GridNode is MIT licensed (LICENSE); Dear ImGui's notice is in THIRD_PARTY_NOTICES.md.
"""


def _copy(src: str, dst: str):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def build(root: str, build_dir: str, out_dir: str, examples: bool, repo_url: str) -> str:
    v = version(root)
    name = f"GridNode-{v}"
    stage = os.path.join(out_dir, name)
    if os.path.exists(stage):
        shutil.rmtree(stage)
    game = stage
    sdk = os.path.join(stage, "sdk")

    need = {"gridnode.dll": "gridnode.dll", "version.dll": os.path.join("start-with-the-game", "version.dll"),
            "gridnode_inject.exe": "gridnode_inject.exe"}
    for f, to in need.items():
        p = os.path.join(build_dir, f)
        if not os.path.isfile(p):
            for cfg in ("Release", "RelWithDebInfo"):
                if os.path.isfile(os.path.join(build_dir, cfg, f)):
                    p = os.path.join(build_dir, cfg, f)
        if not os.path.isfile(p):
            raise ValueError(f"{f} not found in {build_dir}: build GridNode for Windows first (docs/building.md)")
        _copy(p, os.path.join(game, to))
    os.makedirs(os.path.join(game, "mods"), exist_ok=True)
    if examples:
        mods = os.path.join(build_dir, "mods")
        for m in EXAMPLES:
            p = os.path.join(mods, m + ".dll")
            if os.path.isfile(p):
                _copy(p, os.path.join(game, "mods", m, m + ".dll"))

    engine = os.path.join(build_dir, "generated", "gridnode", "engine.hpp")
    if not os.path.isfile(engine):
        raise ValueError(f"engine.hpp not found in {build_dir}/generated: build GridNode first")
    for h in PUBLIC_HEADERS:
        p = os.path.join(root, "sdk", "include", "gridnode", h)
        if os.path.isfile(p):
            _copy(p, os.path.join(sdk, "include", "gridnode", h))
    _copy(engine, os.path.join(sdk, "include", "gridnode", "engine.hpp"))
    for f in IMGUI:
        p = os.path.join(root, "third_party", "imgui", f)
        if os.path.isfile(p):
            _copy(p, os.path.join(sdk, "imgui", f))
    rust = os.path.join(root, "bindings", "rust", "gridnode")
    for f in ("Cargo.toml", os.path.join("src", "lib.rs"), os.path.join("src", "sys.rs")):
        _copy(os.path.join(rust, f), os.path.join(sdk, "rust", "gridnode", f))
    shutil.copytree(os.path.join(root, "templates"), os.path.join(sdk, "templates"),
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "target", "build"))
    for f in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        _copy(os.path.join(root, f), os.path.join(stage, f))
    with open(os.path.join(stage, "README.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(README.format(version=v, repo=repo_url))
    for at in (os.path.join(stage, "VERSION"), os.path.join(sdk, "VERSION")):  # sdk/VERSION goes with a mod's copy
        with open(at, "w", encoding="utf-8") as f:
            f.write(v + "\n")

    zpath = os.path.join(out_dir, name + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _dirs, files in os.walk(stage):
            for f in sorted(files):
                p = os.path.join(base, f)
                z.write(p, os.path.relpath(p, out_dir).replace("\\", "/"))
    return zpath

PLAYER_README = """GridNode {version} (player build): mods for Mirror's Edge Catalyst (PC, the EA app build)

Install
  Unzip this folder anywhere (not in the game's folder). Start the game, then run
  gridnode_inject.exe: it loads GridNode from this folder. INSERT opens the menu with the mods'
  windows. GridNode writes gridnode.log here. It stays passive on another build of the game.

  To have it load with the game every time instead: copy start-with-the-game\\version.dll,
  gridnode.dll and mods\\ into the game's folder (where MirrorsEdgeCatalyst.exe is).

Mods
  mods\\mec_trainer is the MEC trainer (practice tools). Add other mods next to it: a mod's zip is
  unzipped here (it makes mods\\<name>\\). Mods made for a newer GridNode refuse to load.

Online
  The trainer never changes online play: with it loaded, Dash and time trial times are not uploaded.
  A mod that changes online services says so in gridnode.log and in the menu's Mods list.

Problems
  gridnode.log, next to gridnode.dll, says what loaded and what went wrong. Report a problem with it:
  {repo}/issues

GridNode is MIT licensed (LICENSE); Dear ImGui's notice is in THIRD_PARTY_NOTICES.md.
"""


def build_player(root: str, build_dir: str, out_dir: str, repo_url: str, mods_dir: str | None = None) -> str:
    """The players' zip: build_dir holds the player build (-DGRIDNODE_PLAYER_BUILD=ON); the trainer
    comes from mods_dir (default build_dir/mods)."""
    v = version(root)
    name = f"GridNode-player-{v}"
    stage = os.path.join(out_dir, name)
    if os.path.exists(stage):
        shutil.rmtree(stage)
    need = {"gridnode.dll": "gridnode.dll", "version.dll": os.path.join("start-with-the-game", "version.dll"),
            "gridnode_inject.exe": "gridnode_inject.exe"}
    for f, to in need.items():
        p = os.path.join(build_dir, f)
        if not os.path.isfile(p):
            for cfg in ("Release", "RelWithDebInfo"):
                if os.path.isfile(os.path.join(build_dir, cfg, f)):
                    p = os.path.join(build_dir, cfg, f)
        if not os.path.isfile(p):
            raise ValueError(f"{f} not found in {build_dir}: build the player build first (-DGRIDNODE_PLAYER_BUILD=ON)")
        if f == "gridnode.dll":
            with open(p, "rb") as fh:
                if DEV_MARKER in fh.read():
                    raise ValueError(f"{p} is a developer build: configure with -DGRIDNODE_PLAYER_BUILD=ON")
        _copy(p, os.path.join(stage, to))
    mods = mods_dir or os.path.join(build_dir, "mods")
    for m in PLAYER_MODS:
        p = os.path.join(mods, m + ".dll")
        if not os.path.isfile(p):
            raise ValueError(f"{m}.dll not found in {mods}: build the examples, or pass --mods <a build's mods folder>")
        _copy(p, os.path.join(stage, "mods", m, m + ".dll"))
    for f in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        _copy(os.path.join(root, f), os.path.join(stage, f))
    with open(os.path.join(stage, "README.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(PLAYER_README.format(version=v, repo=repo_url))
    with open(os.path.join(stage, "VERSION"), "w", encoding="utf-8") as f:
        f.write(v + "\n")
    zpath = os.path.join(out_dir, name + ".zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _dirs, files in os.walk(stage):
            for f in sorted(files):
                p = os.path.join(base, f)
                z.write(p, os.path.relpath(p, out_dir).replace("\\", "/"))
    return zpath
