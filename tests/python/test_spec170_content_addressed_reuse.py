from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PIPELINE_DIR = ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline"
sys.path.insert(0, str(PIPELINE_DIR))

from llm_pipeline_lib import (  # noqa: E402
    role_name,
    write_tiny_transformer_stage_artifacts,
)


class Spec170ContentAddressedReuseTest(unittest.TestCase):
    def test_repeated_runs_share_immutable_stage_objects(self) -> None:
        roles = [role_name(index) for index in range(3)]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            store = root / "content-store"
            first = root / "run-1"
            second = root / "run-2"
            first_artifacts = write_tiny_transformer_stage_artifacts(
                first, roles=roles, stages=3, layer_count=3,
                content_store=store,
            )
            second_artifacts = write_tiny_transformer_stage_artifacts(
                second, roles=roles, stages=3, layer_count=3,
                content_store=store,
            )

            for left, right in zip(first_artifacts, second_artifacts):
                left_path = Path(left.path)
                right_path = Path(right.path)
                self.assertTrue(left_path.is_symlink())
                self.assertTrue(right_path.is_symlink())
                self.assertEqual(left_path.resolve(), right_path.resolve())
                self.assertTrue(left.metadata["contentAddress"].startswith("sha256:"))
                self.assertEqual(
                    left.metadata["contentAddress"],
                    right.metadata["contentAddress"],
                )

            objects = sorted((store / "sha256").glob("*.pt"))
            self.assertEqual(len(objects), 3)
            self.assertEqual(
                sum(path.stat().st_size for path in first.rglob("*")
                    if path.is_file() and not path.is_symlink()),
                0,
            )
            index = json.loads((store / "index-v1.json").read_text())
            self.assertEqual(index["schema"], "ndnsf-di-content-store-v1")
            self.assertEqual(len(index["entries"]), 3)


if __name__ == "__main__":
    unittest.main()
