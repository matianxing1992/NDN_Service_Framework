#!/usr/bin/env python3
"""Keep the current source-bundle validator contract.

The historical Repo job checks were retired with the legacy Tiger jobs; the validator
and its missing-resource negative case still guard a path the container owner uses.
"""

from pathlib import Path
import unittest
import importlib.util

ROOT = Path(__file__).resolve().parents[2]
SOURCE_BUNDLE = ROOT / "Experiments/validate_spec167_source_bundle.py"


def load_source_bundle_validator():
    spec = importlib.util.spec_from_file_location(
        "spec167_source_bundle", SOURCE_BUNDLE
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class Spec167ItigerJobContractTest(unittest.TestCase):
    def test_source_bundle_validator_rejects_missing_runtime_helper(self):
        import hashlib
        import json
        import tempfile

        validator = load_source_bundle_validator()
        with tempfile.TemporaryDirectory() as temporary:
            root = __import__("pathlib").Path(temporary)
            experiments = root / "Experiments"
            experiments.mkdir()
            entries = []
            for name in (
                "NDNSF_DistributedRepo_Artifact_Itiger.py",
                "NDNSF_DistributedRepo_Artifact_Minindn.py",
                "analyze_spec167_itiger_repo.py",
            ):
                path = experiments / name
                path.write_text("# fixture\n")
                entries.append({
                    "bytes": path.stat().st_size,
                    "path": f"Experiments/{name}",
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                })
            manifest = root / "source-manifest.json"
            manifest.write_text(json.dumps({"files": entries}))
            errors = validator.validate_source_bundle(root, manifest)
            self.assertTrue(
                any("spec164_artifact_campaign.py" in error for error in errors)
            )

if __name__ == "__main__":
    unittest.main()
