from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "Experiments" / "NDNSF_DI_LlmPipeline_Minindn.py"


def load_experiment():
    spec = importlib.util.spec_from_file_location("spec109_qwen_experiment", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec109QwenManifestTest(unittest.TestCase):
    def test_model_revision_dtype_stage_and_gpu_mapping_are_parameterized(self):
        module = load_experiment()
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stages = []
            for index in range(3):
                stages.append({
                    "role": f"/LLM/Stage/{index}", "path": f"/project/stage-{index}.onnx",
                    "inputNames": ["hidden_states"], "outputNames": ["hidden_states_out"],
                    "cacheInputs": [], "cacheOutputs": [],
                })
            (root / "qwen-onnx-service-manifest.json").write_text(json.dumps({
                "model": "Qwen/Qwen2.5-7B-Instruct", "modelRevision": "a" * 40,
                "dtype": "float16", "stages": stages,
            }), encoding="utf-8")
            plan_path, manifest_path = module.write_native_qwen_bundle(
                root, execution_provider="cuda", device_ids=["0", "1"])
            plan = json.loads(plan_path.read_text(encoding="utf-8"))["services"][0]
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))["services"][0]
            self.assertEqual(plan["modelRepository"], "Qwen/Qwen2.5-7B-Instruct")
            self.assertEqual(plan["modelRevision"], "a" * 40)
            self.assertEqual(plan["dtype"], "float16")
            self.assertEqual(len(plan["roles"]), 3)
            self.assertEqual(
                [row["metadata"]["deviceId"] for row in manifest["artifacts"]],
                ["0", "1", "0"],
            )
            self.assertTrue(all(
                row["metadata"]["executionProvider"] == "cuda" and
                row["metadata"]["allowCpuFallback"] == "false"
                for row in manifest["artifacts"]
            ))


if __name__ == "__main__":
    unittest.main()
