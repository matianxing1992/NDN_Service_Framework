import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "Experiments/run_spec171_provider_transition.py"
SPEC = importlib.util.spec_from_file_location("spec171_transition_runner", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class Spec171ProviderTransitionRunnerTest(unittest.TestCase):
    def test_static_and_preregistered_commands_differ_only_in_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            common = (root / "trace.csv", root / "build", 171)
            static = MODULE.build_command(
                "grpc-static-3", "grpc", "ucla,wustl,uiuc",
                root / "static", *common)
            registered = MODULE.build_command(
                "grpc-preregistered-4", "grpc", "",
                root / "registered", *common)
            self.assertIn("--provider-scope", static)
            self.assertNotIn("--provider-scope", registered)
            self.assertIn("--grpc-no-health-routing", static)
            self.assertIn("--block-network", static)

    def test_ndnsf_command_has_no_provider_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            command = MODULE.build_command(
                "ndnsf", "ndnsf", "", root / "ndnsf",
                root / "trace.csv", root / "build", 171)
            self.assertNotIn("--provider-scope", command)
            self.assertNotIn("--ndnsf-response-retry", command)

            retry_command = MODULE.build_command(
                "ndnsf", "ndnsf", "", root / "ndnsf-retry",
                root / "trace.csv", root / "build", 171,
                ndnsf_response_retry=True)
            self.assertIn("--ndnsf-response-retry", retry_command)


if __name__ == "__main__":
    unittest.main()
