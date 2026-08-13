from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[2]
PIPELINE_DIR = (
    ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline")
sys.path.insert(0, str(PIPELINE_DIR))
sys.path.insert(0, str(ROOT / "tools/ndnsf-di"))

from llm_pipeline_lib import (  # noqa: E402
    encode_qwen_input_ids,
    qwen_transformer_model_from_stage_package,
    role_name,
    run_qwen_transformer_stage,
    warm_qwen_transformer_stage,
    write_tiny_qwen3_transformer_stage_artifacts,
)
from spec168_real_model_gate import (  # noqa: E402
    expected_stage_completion_marker,
    real_model_artifact_marker,
    real_model_readiness_marker,
    validate_real_model_binding,
)


try:
    from transformers.models.qwen3.configuration_qwen3 import Qwen3Config  # noqa: F401
    from transformers.models.qwen3.modeling_qwen3 import Qwen3ForCausalLM  # noqa: F401
except ModuleNotFoundError:
    QWEN3_TRANSFORMERS_AVAILABLE = False
else:
    QWEN3_TRANSFORMERS_AVAILABLE = True


class Spec168TinyQwenFixtureTest(unittest.TestCase):
    @unittest.skipUnless(
        QWEN3_TRANSFORMERS_AVAILABLE,
        "local Transformers installation does not expose Qwen3; run the exact locked runtime gate",
    )
    def test_three_real_qwen_stages_fit_local_gate_and_execute(self) -> None:
        roles = [role_name(index) for index in range(3)]
        with tempfile.TemporaryDirectory() as temp:
            artifacts = write_tiny_qwen3_transformer_stage_artifacts(
                temp, roles=roles, stages=3, layer_count=3,
            )
            self.assertEqual([item.role for item in artifacts], roles)
            self.assertLess(
                sum(Path(item.path).stat().st_size for item in artifacts),
                128 * 1024 * 1024,
            )
            models = []
            for index, artifact in enumerate(artifacts):
                model = qwen_transformer_model_from_stage_package(
                    artifact.path, device="cpu")
                self.assertTrue(warm_qwen_transformer_stage(
                    model, assigned_device="cpu"))
                self.assertEqual(model.ndnsf_stage_index, index)
                self.assertEqual(model.ndnsf_stage_end - model.ndnsf_stage_start, 1)
                self.assertFalse(model.ndnsf_cpu_fallback)
                models.append(model)

            payload = encode_qwen_input_ids(
                [[151644, 872, 198, 151645]], request_id="/spec168/tiny-qwen")
            for role, model in zip(roles, models):
                payload = run_qwen_transformer_stage(
                    payload, role=role, stages=3, model=model)
            response = json.loads(payload)
            self.assertEqual(
                response["schema"], "ndnsf-di-qwen-transformer-response-v1")
            self.assertEqual(response["stageCount"], 3)
            self.assertEqual(response["layerRanges"], [[0, 1], [1, 2], [2, 3]])
            self.assertEqual(response["logitsShape"], [1, 4, 151936])
            self.assertGreaterEqual(response["topToken"], 0)
            self.assertLess(response["topToken"], 151936)

    def test_real_model_gate_is_manifest_bound_not_repository_hardcoded(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            tokenizer = root / "tokenizer"
            tokenizer.mkdir()
            (tokenizer / "tokenizer.json").write_text("{}")
            (tokenizer / "tokenizer_config.json").write_text("{}")
            digest = "sha256:" + "1" * 64
            manifest = root / "stage-manifest.json"
            manifest.write_text(json.dumps({
                "repository": "NDNSF/TinyQwen3-Fixture",
                "revision": "seed-168-config-v1",
                "modelDigest": digest,
                "stages": [
                    {"runtime": "qwen-transformers"} for _ in range(3)
                ],
            }))
            args = SimpleNamespace(
                runtime="qwen-transformers",
                qwen_stage_manifest=str(manifest),
                generation_campaign_manifest=str(root / "campaign.json"),
                generation_jsonl=str(root / "generation.jsonl"),
                qwen_tokenizer_dir=str(tokenizer),
                workload_digest="sha256:" + "2" * 64,
                model_identity_digest=digest,
                qwen_model="NDNSF/TinyQwen3-Fixture",
                qwen_revision="seed-168-config-v1",
                stages=3,
            )
            validate_real_model_binding(args)
            self.assertEqual(
                real_model_artifact_marker(args.runtime),
                "LLM_PIPELINE_QWEN_STAGE_ARTIFACT_READY",
            )
            self.assertEqual(real_model_readiness_marker(
                args.runtime, deferred_selection=True,
            ), "LLM_PIPELINE_QWEN_SELECTION_PREPARE")
            args.qwen_model = "Qwen/Qwen3-0.6B"
            with self.assertRaisesRegex(ValueError, "repository"):
                validate_real_model_binding(args)

    def test_transformers_generation_uses_generation_completion_markers(self) -> None:
        self.assertEqual(expected_stage_completion_marker(
            "qwen-transformers", stage_index=0, stages=3,
            full_generation=True,
        ), "LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL")
        self.assertEqual(expected_stage_completion_marker(
            "qwen-transformers", stage_index=1, stages=3,
            full_generation=True,
        ), "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED")
        self.assertEqual(expected_stage_completion_marker(
            "qwen-transformers", stage_index=2, stages=3,
            full_generation=True,
        ), "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED")


if __name__ == "__main__":
    unittest.main()
