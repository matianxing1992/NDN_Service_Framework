import json
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
HARNESS = REPO / "Experiments" / "NDNSF_DI_NativeTracer_Minindn.py"


def load_harness():
    spec = importlib.util.spec_from_file_location("spec105_native_tracer_harness", HARNESS)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class NativeTracerRuntimeProfileTests(unittest.TestCase):
    def test_provider_evidence_overrides_optimistic_profile_label(self) -> None:
        module = load_harness()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "provider.log"
            record = {
                "schema": "ndnsf-di-execution-evidence-v1",
                "providerName": "/p0", "providerBootId": "boot",
                "runnerKind": "synthetic-delay", "realCompute": False,
                "modelDigest": "sha256:m", "planDigest": "sha256:p",
                "artifactDigests": {"/s0": "sha256:a"},
            }
            path.write_text("NDNSF_DI_EXECUTION_EVIDENCE " + json.dumps(record) + "\n",
                            encoding="utf-8")
            evidence = module.collect_provider_execution_evidence([path])
            self.assertEqual(module.derive_runner_classification(evidence),
                             "synthetic-delay")
            # No requested/profile value is accepted by the classifier.
            self.assertNotEqual(module.derive_runner_classification(evidence),
                                "qwen-onnx-native")

    def test_runtime_profile_drives_local_execution_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "native-tracer-profile-run"
            proc = subprocess.run(
                [
                    "python3",
                    str(HARNESS),
                    "--runtime-profile",
                    "examples/di-native-tracer.runtime.json",
                    "--out",
                    str(out_dir),
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=30,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            summary = json.loads((out_dir / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "SUCCESS")
            self.assertEqual(summary["assignmentRequested"], "llm-proportional")
            self.assertEqual(summary["assignmentResolved"], "llm-proportional")
            self.assertEqual(summary["miniNDNRun"], "skipped-local-execution-only")
            self.assertEqual(summary["runtimeProfile"]["profile"], "examples/di-native-tracer.runtime.json")
            self.assertEqual(summary["runtimeProfile"]["policyBundle"], "llm-proportional")
            self.assertTrue(summary["runtimeProfile"]["localExecutionOnly"])


if __name__ == "__main__":
    unittest.main()
