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
PREFLIGHT_PATH = (
    ROOT / "specs/168-itiger-di-deployment-fidelity/jobs/"
    "spec168_exact_sif_cuda_preflight.py"
)
BUILDER_PATH = (
    ROOT / "specs/168-itiger-di-deployment-fidelity/jobs/"
    "build-gate-c-source-bundle.py"
)


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


preflight = load_module("spec168_exact_sif_cuda_preflight", PREFLIGHT_PATH)
builder = load_module("spec168_gate_c_bundle", BUILDER_PATH)


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

    def test_source_bundle_contains_no_model_payload(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "bundle"
            digest = builder.build(output)
            paths = [
                line.split("  ", 1)[1]
                for line in (output / "source-files.sha256").read_text().splitlines()
            ]
            modes = json.loads((output / "source-modes.json").read_text())
        self.assertTrue(digest.startswith("sha256:"))
        self.assertIn("jobs/spec168_exact_sif_cuda_preflight.py", paths)
        self.assertIn("di/ndnsf_distributed_inference/core/execution.py", paths)
        self.assertIn("di/ndnsf_distributed_inference/compatibility/manifest.json", paths)
        self.assertIn("ndnsf/ndnsf/service.py", paths)
        self.assertIn("repo/py_repoclient/artifact_transfer.py", paths)
        self.assertIn("llm_pipeline/user.py", paths)
        self.assertIn("jobs/gate-e-small-single.sbatch", paths)
        self.assertIn("jobs/gate-e-small-repeated.sbatch", paths)
        self.assertIn("jobs/spec168-cold-warm-analyzer.py", paths)
        self.assertIn("jobs/spec168_cold_warm_contract.py", paths)
        self.assertIn("jobs/prepare-qwen36.py", paths)
        self.assertIn("jobs/spec168-overlay-entrypoint.sh", paths)
        self.assertIn("compat/spec162/generation-rank-inner.sh", paths)
        self.assertEqual("0555", modes["jobs/spec168-overlay-entrypoint.sh"])
        self.assertEqual("0444", modes["source-modes.json"])
        self.assertFalse(any(path.endswith((".pt", ".sif", ".bin")) for path in paths))

    def test_source_bundle_can_pin_native_core_overlay(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            native = root / "libndn-service-framework.so"
            native.write_bytes(b"pinned-native-core-fixture")
            extension = root / "_ndnsf.cpython-310-x86_64-linux-gnu.so"
            extension.write_bytes(b"pinned-native-extension-fixture")
            repo_extension = (
                root / "_py_repoclient.cpython-310-x86_64-linux-gnu.so"
            )
            repo_extension.write_bytes(b"pinned-repo-extension-fixture")
            output = root / "bundle"
            builder.build(output, native, extension, repo_extension)
            manifest = (output / "source-files.sha256").read_text()
            copied = output / "native/lib/libndn-service-framework.so.0.1.0"
            copied_extension = output / f"native/python/{extension.name}"
            copied_repo_extension = (
                output / f"repo/py_repoclient/{repo_extension.name}"
            )
            copied_bytes = copied.read_bytes()
            copied_extension_bytes = copied_extension.read_bytes()
            copied_repo_extension_bytes = copied_repo_extension.read_bytes()
        self.assertIn("native/lib/libndn-service-framework.so.0.1.0", manifest)
        self.assertIn(f"native/python/{extension.name}", manifest)
        self.assertIn(f"repo/py_repoclient/{repo_extension.name}", manifest)
        self.assertEqual(b"pinned-native-core-fixture", copied_bytes)
        self.assertEqual(
            b"pinned-native-extension-fixture", copied_extension_bytes)
        self.assertEqual(
            b"pinned-repo-extension-fixture", copied_repo_extension_bytes)

    def test_source_bundle_rejects_incomplete_native_abi_closure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            native = root / "libndn-service-framework.so"
            native.write_bytes(b"core-only-is-unsafe")
            with self.assertRaisesRegex(ValueError, "CORE_NDNSF_AND_REPO"):
                builder.build(root / "bundle", native)

    def test_source_bundle_closure_rejects_missing_with_name_dependency(self):
        selected = {
            "jobs/owner.py": ROOT / "specs/162-itiger-qwen36-generation/jobs/"
            "build-generation-policy.py",
        }
        with self.assertRaisesRegex(ValueError, "prepare-qwen36.py"):
            builder.validate_bundle_closure(selected)


if __name__ == "__main__":
    unittest.main()
