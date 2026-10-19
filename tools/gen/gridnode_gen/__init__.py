"""gridnode-gen: the GridNode type database tools.

The database has two layers (docs/PLAN.md section 4.1):
  - reflected: each user's own dump of the engine's reflection data (gridnode-reflected JSON)
  - annotations: our hand-written findings (db/annotations/*.yaml)

This package validates both, and generates the C++ headers the SDK is built on.
"""

__version__ = "0.2.0"
