#!/usr/bin/env python3
"""GridNode type database tools. Run `python tools/gen/gridnode.py --help`."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gridnode_gen.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
