import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
NATIVE_DIR = (
    REPO
    / "examples"
    / "python"
    / "NDNSF-DistributedInference"
    / "native_di_tracer"
)


def load_module(name: str, path: Path):
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class SweepRuntimeProfileTests(unittest.TestCase):
    def test_proportional_rps_search_defaults_from_profile(self) -> None:
        module = load_module("run_llm_proportional_rps_search", NATIVE_DIR / "run_llm_proportional_rps_search.py")
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            profile = {
                "distributed_inference": {
                    "native_tracer": {
                        "enabled": True,
                        "out": str(tmpdir / "native-tracer"),
                        "tracer_dir": str(NATIVE_DIR),
                        "target_rps": 7.5,
                    }
                }
            }
            profile_path = tmpdir / "profile.json"
            profile_path.write_text(json.dumps(profile), encoding="utf-8")

            defaults = module.runtime_profile_defaults(str(profile_path), "")
            self.assertEqual(defaults["out_root"], str(tmpdir / "native-tracer" / "rps-search"))
            self.assertEqual(defaults["target_rps_list"], "7.5")
            self.assertEqual(defaults["model_spec"], str(NATIVE_DIR / "llm_model_spec_qwen_tiny_proportional.json"))
            self.assertEqual(defaults["provider_profiles"], str(NATIVE_DIR / "llm_provider_profiles_2_4_8.json"))

    def test_rate_sweep_defaults_from_resolved_profile(self) -> None:
        module = load_module("run_rate_sweep_campaign", NATIVE_DIR / "run_rate_sweep_campaign.py")
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            resolved = {
                "profile": {
                    "distributed_inference": {
                        "native_tracer": {
                            "enabled": True,
                            "out": str(tmpdir / "native-tracer"),
                            "target_rps": 3.0,
                            "provider_check_timeout": 55,
                            "activation_pad_bytes": 128,
                            "role_execution_delay_ms": 9.5,
                            "requests": 6,
                            "concurrency": 2,
                        }
                    }
                }
            }
            resolved_path = tmpdir / "resolved.json"
            resolved_path.write_text(json.dumps(resolved), encoding="utf-8")

            defaults = module.runtime_profile_defaults("", str(resolved_path))
            self.assertEqual(defaults["out_root"], str(tmpdir / "native-tracer" / "rate-sweep"))
            self.assertEqual(defaults["target_rps_list"], "3.0")
            self.assertEqual(defaults["provider_check_timeout"], 55)
            self.assertEqual(defaults["activation_pad_bytes"], 128)
            self.assertEqual(defaults["role_execution_delay_ms"], 9.5)
            self.assertEqual(defaults["requests"], 6)
            self.assertEqual(defaults["concurrency"], 2)


if __name__ == "__main__":
    unittest.main()
