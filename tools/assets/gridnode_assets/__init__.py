"""GridNode Assets: reads Mirror's Edge Catalyst's data files where the game keeps them.

Step 1 of docs/PLAN part 5 (findings and implementation plan, 2026-11-11): the store (catalogues,
superbundle indexes and bundles, payload blocks). The format facts were taken from reading Frosty's
source for the Catalyst profile and are unchecked against the game's files until `gridnode.py assets
check` has run on an install; this code is written from those facts, not from Frosty's code (its
licence forbids adapting it). Nothing here writes into the game's folders; extracted files go to a
git-ignored folder. Game data is never committed.
"""

__version__ = "0.1.0"
