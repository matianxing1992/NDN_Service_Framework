from __future__ import annotations

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
OCI = ROOT / "packaging/ndnsf-di-container/oci"


class DeploymentRuntimeBoundaryTests(unittest.TestCase):
    def _lock(self, relative: str) -> dict:
        return json.loads((OCI / relative).read_text())

    def test_deployment_and_offline_sets_are_disjoint(self):
        for relative in ("locks/gpu.lock", "layered/locks/ml-runtime.lock.json"):
            lock = self._lock(relative)
            deployment = {name.lower() for name in lock["deploymentPythonPackages"]}
            exporter = {name.lower() for name in lock["offlineExporterPackages"]}
            self.assertTrue({"onnxruntime-gpu", "tokenizers"} <= deployment)
            self.assertNotIn("torch", deployment)
            self.assertNotIn("transformers", deployment)
            self.assertTrue({"torch", "transformers"} <= exporter)

    def test_final_docker_stages_use_runtime_venv_and_negative_import_guards(self):
        files = (
            OCI / "Dockerfile.gpu",
            OCI / "layered/Dockerfile.ml",
            OCI / "layered/Dockerfile.app",
        )
        for path in files:
            text = path.read_text()
            self.assertIn("onnxruntime", text, path)
            self.assertIn("tokenizers", text, path)
            self.assertIn("import torch", text, path)
            self.assertIn("import transformers", text, path)
            if path.name in {"Dockerfile.ml", "Dockerfile.gpu"}:
                self.assertIn("deploymentPythonPackages", text, path)
                self.assertIn("runtime-venv", text, path)

        layered_ml = (OCI / "layered/Dockerfile.ml").read_text()
        self.assertNotIn("COPY --from=ml-devel /opt/venv /opt/venv", layered_ml)
        gpu = (OCI / "Dockerfile.gpu").read_text()
        self.assertNotIn("COPY --from=gpu-assembler /opt/venv /opt/venv", gpu)

    def test_runtime_probes_do_not_depend_on_transformer_modules(self):
        probe = (OCI / "scripts/probe-runtime.py").read_text()
        compat = (ROOT / "packaging/ndnsf-di-container/lib/gpu_compatibility.py").read_text()
        for text in (probe, compat):
            self.assertNotIn("import torch", text)
            self.assertNotIn("import transformers", text)
        self.assertIn("onnxruntime", probe)
        self.assertIn("tokenizers", probe)


if __name__ == "__main__":
    unittest.main()
