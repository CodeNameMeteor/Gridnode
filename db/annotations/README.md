# db/annotations

Hand-written findings about the engine that reflection does not provide: runtime class members,
vtable slots, function signatures, globals, hook points, constants, entity events and the build
fingerprint. One YAML file per subsystem (`physics.yaml`, `gamestate.yaml`, `spawn.yaml`, ...). The
schema and how to check a change (`gridnode.py validate`) are in
[docs/annotations.md](../../docs/annotations.md); how to add findings, Ghidra sync and fingerprints
in [docs/contributing.md](../../docs/contributing.md).

The build generates `sdk/include/gridnode/engine.hpp` from these files; never edit that header by
hand.

Annotations name reflected types but never copy their field lists, so this folder holds only our
own reversing results.
