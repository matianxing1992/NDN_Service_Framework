#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/fixtures/legacy-exact-sif-gate"
PREFLIGHT_PATH = FIXTURES / "spec168_exact_sif_cuda_preflight.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


preflight = load_module("spec168_exact_sif_cuda_preflight", PREFLIGHT_PATH)


class ExactSifCudaPreflightTest(unittest.TestCase):
    def fixture(self, root: Path) -> argparse.Namespace:
        stages = []
        for index in range(3):
            path = root / f"stage-{index}.pt"
            path.write_bytes((f"stage-{index}" * 17).encode())
            stages.append({
                "stageIndex": index,
                "stageCount": 3,
                "role": f"/LLM/Pipeline/Stage/{index}",
                "path": str(path),
                "bytes": path.stat().st_size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "layerRange": {"start": index, "endExclusive": index + 1},
            })
        manifest = {
            "modelDigest": "sha256:" + "1" * 64,
            "revision": "revision-1",
            "runtimeSifSha256": "2" * 64,
            "stages": stages,
        }
        manifest_path = root / "stage-manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        reference_path = root / "reference.json"
        reference_path.write_text(json.dumps({
            "prompts": [{
                "formattedInputIds": [1, 2, 3],
                "referenceGeneratedTokenIds": [4],
            }],
        }), encoding="utf-8")
        return argparse.Namespace(
            stage_manifest=manifest_path,
            reference=reference_path,
            expected_stage_manifest_digest="sha256:" + hashlib.sha256(
                manifest_path.read_bytes()).hexdigest(),
            expected_model_digest="sha256:" + "1" * 64,
            expected_revision="revision-1",
            expected_sif_digest="sha256:" + "2" * 64,
            expected_source_digest="sha256:" + "3" * 64,
            expected_source_bundle_digest="sha256:" + "4" * 64,
        )

    def test_static_gate_binds_all_identities_and_three_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = self.fixture(Path(temporary))
            with mock.patch.dict("os.environ", {
                "SPEC168_RUNTIME_SIF_SHA256": args.expected_sif_digest,
                "SPEC168_SOURCE_DIGEST": args.expected_source_digest,
                "SPEC168_SOURCE_BUNDLE_DIGEST":
                    args.expected_source_bundle_digest,
            }, clear=False):
                result = preflight.verify_static_inputs(args)
        self.assertEqual(3, len(result["stages"]))
        self.assertEqual(args.expected_stage_manifest_digest,
                         result["stageManifestDigest"])

    def test_static_gate_rejects_wrong_runtime_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = self.fixture(Path(temporary))
            with mock.patch.dict("os.environ", {
                "SPEC168_RUNTIME_SIF_SHA256": "sha256:" + "9" * 64,
                "SPEC168_SOURCE_DIGEST": args.expected_source_digest,
                "SPEC168_SOURCE_BUNDLE_DIGEST":
                    args.expected_source_bundle_digest,
            }, clear=False):
                with self.assertRaisesRegex(
                        preflight.PreflightError, "RUNTIME_SIF_DIGEST_MISMATCH"):
                    preflight.verify_static_inputs(args)


if __name__ == "__main__":
    unittest.main()
