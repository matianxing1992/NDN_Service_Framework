import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
CAMPAIGN = (
    REPO
    / "examples"
    / "python"
    / "NDNSF-DistributedInference"
    / "native_di_tracer"
    / "run_llm_full_network_campaign.py"
)


def load_campaign_module():
    spec = importlib.util.spec_from_file_location("run_llm_full_network_campaign", CAMPAIGN)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class LlmCampaignRuntimeProfileTests(unittest.TestCase):
    def test_campaign_defaults_from_runtime_profile(self) -> None:
        module = load_campaign_module()
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            profile_path = tmpdir / "profile.json"
            profile_path.write_text(
                json.dumps(
                    {
                        "distributed_inference": {
                            "native_tracer": {
                                "enabled": True,
                                "out": str(tmpdir / "native-tracer"),
                                "llm_planner_mode": "proportional",
                                "provider_check_timeout": 77,
                                "role_execution_delay_ms": 12.5,
                                "llm_stage_execution_delay_scale": 3.0,
                                "target_rps": 4.0,
                                "open_loop_duration_s": 2.0,
                                "open_loop_driver_mode": "process-pool",
                                "submission_spacing_ms": 33,
                                "runtime_v1_context_tokens": 2048,
                                "runtime_v1_generated_tokens": 64,
                                "runtime_v1_prefix_id": "profile-prefix",
                            }
                        }
                    }
                ),
                encoding="utf-8",
            )

            defaults = module.runtime_profile_defaults(str(profile_path), "")
            self.assertEqual(defaults["out_root"], str(tmpdir / "native-tracer" / "campaign"))
            self.assertEqual(defaults["modes"], "proportional")
            self.assertEqual(defaults["provider_check_timeout"], 77)
            self.assertEqual(defaults["role_execution_delay_ms"], 12.5)
            self.assertEqual(defaults["stage_execution_delay_scale"], 3.0)
            self.assertEqual(defaults["target_rps"], 4.0)
            self.assertEqual(defaults["open_loop_duration_s"], 2.0)
            self.assertEqual(defaults["open_loop_driver_mode"], "process-pool")
            self.assertEqual(defaults["submission_spacing_ms"], 33)
            self.assertEqual(defaults["runtime_v1_context_tokens"], 2048)
            self.assertEqual(defaults["runtime_v1_generated_tokens"], 64)
            self.assertEqual(defaults["runtime_v1_prefix_id"], "profile-prefix")


if __name__ == "__main__":
    unittest.main()
