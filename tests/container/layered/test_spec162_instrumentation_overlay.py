#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = (
    ROOT
    / "packaging/ndnsf-di-container/oci/layered/scripts"
    / "build-spec162-instrumented-candidate.py"
)


def load_module():
    spec = importlib.util.spec_from_file_location(
        "build_spec162_instrumented_candidate", SCRIPT
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class Spec162InstrumentationOverlayTests(unittest.TestCase):
    def test_overlay_is_parent_and_source_hash_bound(self):
        module = load_module()
        dockerfile = module.DOCKERFILE.read_text(encoding="utf-8")
        self.assertIn("FROM ${BASE_IMAGE}", dockerfile)
        self.assertIn("PROVIDER_SHA256", dockerfile)
        self.assertIn("REPO_ORCHESTRATION_SHA256", dockerfile)
        self.assertIn("REPO_SERVICE_NAMES_SHA256", dockerfile)
        self.assertIn("REPO_PERSISTENCE_SHA256", dockerfile)
        self.assertIn("INSTRUMENTATION_SEAL", dockerfile)
        self.assertIn("--from=instrumentation_seal", dockerfile)
        self.assertNotIn("apt-get", dockerfile)
        self.assertNotIn("pip install", dockerfile)

    def test_seal_covers_image_runtime_analysis_and_build_inputs(self):
        module = load_module()
        sources = module.selected_sources()
        self.assertIn(
            "image/ndnsf_distributed_inference/provider.py", sources)
        self.assertIn("image/py_repoclient/orchestration.py", sources)
        self.assertIn("image/py_repoclient/service_names.py", sources)
        self.assertIn("image/py_repoclient/persistence.py", sources)
        self.assertIn("llm_pipeline/user.py", sources)
        self.assertIn("jobs/analyze-generation-smoke.py", sources)
        self.assertIn("jobs/analyze-generation-formal.py", sources)
        self.assertIn(
            "build/Dockerfile.spec162-instrumentation", sources)
        self.assertIn("build/qwen36-overlay.lock.json", sources)
        self.assertTrue(all(path.is_file() for path in sources.values()))
        self.assertIn(
            "NDNSF_DI_ACK_DECISION",
            sources[
                "image/ndnsf_distributed_inference/provider.py"
            ].read_text(encoding="utf-8"),
        )
        self.assertNotIn(
            "RepoCapacityReservation",
            sources["image/py_repoclient/orchestration.py"].read_text(
                encoding="utf-8"),
        )
        service_names = sources[
            "image/py_repoclient/service_names.py"
        ].read_text(encoding="utf-8")
        self.assertNotIn("RESERVE_CAPACITY", service_names)
        self.assertNotIn("RELEASE_CAPACITY", service_names)

    def test_source_seal_is_deterministic_and_parent_bound(self):
        module = load_module()
        first = module.source_seal(module.selected_sources())
        second = module.source_seal(module.selected_sources())
        self.assertEqual(first, second)
        self.assertEqual(first["baseImageId"], module.BASE_IMAGE_ID)
        self.assertRegex(first["sealDigest"], r"^sha256:[0-9a-f]{64}$")
        self.assertGreater(len(first["sources"]), 10)


if __name__ == "__main__":
    unittest.main()
