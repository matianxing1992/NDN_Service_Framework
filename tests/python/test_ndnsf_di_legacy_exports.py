from __future__ import annotations

import json
from pathlib import Path
import unittest

import ndnsf_distributed_inference as legacy


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json"


class LegacyExportsTest(unittest.TestCase):
    def test_every_inventoried_root_export_remains_resolvable(self) -> None:
        payload = json.loads(MANIFEST.read_text(encoding="utf-8"))
        names = sorted(
            entry["surfaceId"].split(":", 1)[1]
            for entry in payload["entries"]
            if entry["surfaceKind"] == "python-export"
        )
        # The Phase-1 AST inventory counted the initial ``__all__ = [...]``
        # assignment but missed the historical ``__all__ += [...]`` block.
        # The immutable pre-separation module actually exposed 174 names.
        self.assertEqual(len(names), 174)
        self.assertEqual(len(names), len(set(names)))
        for name in names:
            with self.subTest(name=name):
                self.assertIn(name, legacy.__all__)
                self.assertIsNotNone(getattr(legacy, name))


if __name__ == "__main__":
    unittest.main()
