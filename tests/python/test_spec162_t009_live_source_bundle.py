"""Keep the archived source-bundle builder self-verifying.

The historical job scripts and their command-line assertions were retired with the legacy
Tiger jobs. What remains guards the builder itself: its manifest covers every declared
source, and it refuses a non-empty output directory.
"""

from __future__ import annotations

import hashlib
from importlib.machinery import SourceFileLoader
import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ARCHIVE = ROOT / "tests/fixtures/legacy-qwen-source-bundle"
JOBS = ARCHIVE / "jobs"
EXACT_SIF = ROOT / "tests/fixtures/legacy-exact-sif-gate"
SCRIPT = JOBS / "build-t009-live-source-bundle.py.txt"
# The archived builder still declares these inputs; map each to the tracked copy that
# preserves the bytes it was originally run against. The historical job scripts that used
# to supply several of them were retired, so those entries now resolve through `ARCHIVE`.
SHARED_INPUTS = {
    "jobs/build-generation-policy.py": EXACT_SIF / "build-generation-policy.py.txt",
    "jobs/generation-rank-inner.sh":
        ROOT / "tests/fixtures/legacy-tiger-three-node/generation-rank-inner.sh.txt",
    "jobs/nfd.conf.in": ROOT / "tests/fixtures/legacy-qwen-sif-rendering/nfd.conf.in",
    "jobs/prepare-qwen36.py": EXACT_SIF / "prepare-qwen36.py.txt",
    "jobs/run-repo-node.py": EXACT_SIF / "run-repo-node.py.txt",
    "jobs/register-qwen36-repo.py": EXACT_SIF / "register-qwen36-repo.py.txt",
}


def load_module():
    name = "build_t009_live_source_bundle"
    spec = importlib.util.spec_from_loader(name, SourceFileLoader(name, str(SCRIPT)))
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    module.SOURCES = {
        name: (SHARED_INPUTS.get(name, ARCHIVE / (name + ".txt"))
               if module.FEATURE_ROOT in path.parents
               else ROOT / path.relative_to(module.PROJECT_ROOT))
        for name, path in module.SOURCES.items()
    }
    return module


class T009LiveSourceBundleTests(unittest.TestCase):
    def test_deferred_planner_records_graph_after_ack_closed(self):
        placement = (
            ROOT / "NDNSF-DistributedInference"
            / "ndnsf_distributed_inference/app_sdk/placement.py"
        ).read_text(encoding="utf-8")
        self.assertIn(
            '"NDNSF_DI_AUTOPLANNING_GRAPH_READY"',
            placement,
        )
        self.assertIn('"after=ACK_CLOSED"', placement)
        self.assertLess(
            placement.index("collaboration = self.service_user.begin_collaboration"),
            placement.index("closed = collaboration.acks_closed()"),
        )
        self.assertLess(
            placement.index("closed = collaboration.acks_closed()"),
            placement.index("model_descriptor = adapter.describe_model"),
        )

    def test_builds_self_verifying_bundle(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "bundle"
            result = module.build(output)
            manifest = output / "source-files.sha256"
            self.assertEqual(
                result["sourceCount"], len(module.SOURCES))
            self.assertEqual(
                result["sourceManifestSha256"],
                hashlib.sha256(manifest.read_bytes()).hexdigest(),
            )
            rows = manifest.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(rows), len(module.SOURCES))
            for row in rows:
                expected, relative = row.split("  ", 1)
                self.assertEqual(
                    hashlib.sha256((output / relative).read_bytes()).hexdigest(),
                    expected,
                )

    def test_rejects_nonempty_output(self):
        module = load_module()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "bundle"
            output.mkdir()
            (output / "old").write_text("old", encoding="utf-8")
            with self.assertRaisesRegex(
                    module.BundleError, "OUTPUT_DIRECTORY_NOT_EMPTY"):
                module.build(output)


if __name__ == "__main__":
    unittest.main()
