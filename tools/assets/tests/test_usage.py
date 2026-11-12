"""Tests for `assets usage` on a small install made in a temporary folder (no game data)."""

from __future__ import annotations

import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import yaml  # noqa: E402

from gridnode_assets import cli  # noqa: E402
from tests.test_deps import install  # noqa: E402


class Usage(unittest.TestCase):
    def test_catalogue(self):
        with tempfile.TemporaryDirectory() as d:
            install(d)
            out = os.path.join(d, "usage.yaml")
            with redirect_stdout(io.StringIO()) as text:
                self.assertEqual(cli.main(["usage", "--data", os.path.join(d, "Data"), "--out", out]), 0)
            self.assertIn("1 types in 2 EBX files", text.getvalue())
            with open(out, encoding="utf-8") as f:
                rows = yaml.safe_load(f)
            self.assertEqual(rows[0]["type"], "Thing")
            self.assertEqual((rows[0]["instances"], rows[0]["files"]), (2, 2))
            self.assertEqual(rows[0]["fields"], {"Other": "ref", "Resource": "ResourceRef"})
            self.assertEqual(sorted(rows[0]["examples"]), ["objects/a", "objects/b"])
            with redirect_stdout(io.StringIO()):
                cli.main(["usage", "--data", os.path.join(d, "Data"), "--out", out, "--like", "volume"])
            with open(out, encoding="utf-8") as f:
                self.assertEqual(yaml.safe_load(f), [])


if __name__ == "__main__":
    unittest.main()
