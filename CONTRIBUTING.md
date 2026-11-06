# Contributing to GridNode

Welcome. The short version:

- **Findings about the engine** (addresses, offsets, virtuals): add them to `db/annotations/*.yaml` with a source, a
  confidence and a note on how to check them. [docs/contributing.md](docs/contributing.md) has the rules and the Ghidra sync.
- **Code**: C++17, 4 spaces, K&R braces; comments say why. Build and test steps are in
  [docs/building.md](docs/building.md); [docs/tools.md](docs/tools.md) lists the tools.
- **Before a pull request**: `python tools/gen/gridnode.py validate --strict`, the host tests (`ctest`), and for an
  `api.h` change `python tools/gen/gridnode.py rust` plus `cargo fmt`, and a row in [docs/modding.md](docs/modding.md).
- **Say what you tested in game** and what you did not.
- **Never commit** anything extracted from the game: image dumps, `db/reflected/`, `event-names.txt`, game meshes,
  logs, recordings.
- **Online play**: GridNode must not make cheating easy. Changes to online services need `online_allow` and loud
  warnings ([docs/online.md](docs/online.md)); the trainer never opts in.
