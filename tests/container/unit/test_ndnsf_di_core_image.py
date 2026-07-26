from __future__ import annotations

import ast
from pathlib import Path
import unittest


ROOT=Path(__file__).resolve().parents[3]
CORE=ROOT/"NDNSF-DistributedInference/ndnsf_distributed_inference/core"


class CoreProfileStaticTest(unittest.TestCase):
    def test_core_has_no_sdk_app_planner_or_model_import(self):
        forbidden=("ndnsf_distributed_inference.sdk","ndnsf_distributed_inference.app_sdk",
                   "ndnsf_distributed_inference.planner","onnx","torch","transformers")
        found=[]
        for path in CORE.glob("*.py"):
            tree=ast.parse(path.read_text())
            for node in ast.walk(tree):
                module=(node.module or "") if isinstance(node,ast.ImportFrom) else ""
                names=[a.name for a in node.names] if isinstance(node,ast.Import) else []
                if any(any(value==item or value.startswith(item+".") for item in forbidden)
                       for value in [module,*names]): found.append((path.name,module,names))
        self.assertEqual(found,[])

    def test_core_profile_does_not_own_root_init(self):
        text=(ROOT/"NDNSF-DistributedInference/packaging/python/core/pyproject.toml").read_text()
        self.assertIn('include = ["ndnsf_distributed_inference.core*"]',text)
        self.assertNotIn('include = ["ndnsf_distributed_inference"]',text)

    def test_oci_source_selects_explicit_owner_profiles_without_weights(self):
        dockerfile=(ROOT/"packaging/ndnsf-di-container/oci/Dockerfile.gpu").read_text()
        self.assertIn('ARG NDNSF_DI_OWNER_PROFILES="core sdk app planner adapters/onnx adapters/qwen adapters/llama ops compat"',dockerfile)
        self.assertIn('packaging/python/$profile',dockerfile)
        self.assertNotIn("COPY models",dockerfile)
        self.assertNotIn("COPY weights",dockerfile)

    def test_sealed_manifest_and_probe_assert_profile_identity_only(self):
        seal=(ROOT/"packaging/ndnsf-di-container/oci/scripts/prepare-sealed-context.py").read_text()
        probe=(ROOT/"packaging/ndnsf-di-container/oci/scripts/probe-runtime.py").read_text()
        for profile in ("core", "sdk", "app", "planner", "adapters/onnx",
                        "adapters/qwen", "adapters/llama", "ops", "compat"):
            self.assertIn(f'"{profile}"',seal)
        self.assertIn('"modelWeightsIncluded": False',probe)
        self.assertIn('"ndnsf-di-core"',probe)
        self.assertIn('"ndnsf-di-adapter-qwen"',probe)

    def test_spec111_static_gate_never_starts_a_container_runtime(self):
        tree=ast.parse(Path(__file__).read_text())
        imported={alias.name for node in ast.walk(tree) if isinstance(node,ast.Import)
                  for alias in node.names}
        self.assertTrue(imported.isdisjoint({"subprocess", "docker", "apptainer"}))


if __name__=="__main__": unittest.main()
