#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = (
    ROOT
    / "packaging/ndnsf-di-container/oci/layered/scripts/verify-layer-contract.py"
)


class LayerContractTests(unittest.TestCase):
    def test_current_layer_contract_passes(self):
        spec = importlib.util.spec_from_file_location("verify_layer_contract", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        report = module.run(ROOT)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["stableSourceCount"], 6)
        self.assertEqual(report["appSourceCount"], 3)
        ml_lock = json.loads(
            (
                ROOT
                / "packaging/ndnsf-di-container/oci/layered/locks/ml-runtime.lock.json"
            ).read_text()
        )
        self.assertIn(
            "cuda-cupti-12-4=12.4.127-1",
            ml_lock["pythonRuntimePackages"],
        )
        app_lock = json.loads(
            (
                ROOT
                / "packaging/ndnsf-di-container/oci/layered/locks/app-runtime.lock.json"
            ).read_text()
        )
        patch = (
            ROOT
            / app_lock["buildCompatibilityPatches"]["ndn-svs-boost-1.71"]["path"]
        )
        self.assertTrue(patch.is_file())

    def test_driver_targets_exist_in_dockerfiles(self):
        layered = ROOT / "packaging/ndnsf-di-container/oci/layered"
        ml = (layered / "Dockerfile.ml").read_text()
        ndn = (layered / "Dockerfile.ndn").read_text()
        app = (layered / "Dockerfile.app").read_text()
        self.assertIn(" AS ml-devel", ml)
        self.assertIn(" AS ml-runtime", ml)
        self.assertIn(" AS ndn-devel", ndn)
        self.assertIn(" AS ndn-runtime", ndn)
        self.assertIn(" AS app-runtime", app)
        self.assertIn(
            "--targets=ndn-service-framework,libndn-service-framework.pc,"
            "App_ServiceController,di-native-provider",
            app,
        )
        builder = app.split("FROM ${NDN_RUNTIME_IMAGE}", 1)[0]
        self.assertGreater(
            builder.index("ARG APP_BUILD_ID"),
            builder.index("/opt/venv/bin/pip install"),
        )


if __name__ == "__main__":
    unittest.main()
