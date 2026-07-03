import json
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
HARNESS = REPO / "Experiments" / "NDNSF_DI_NativeTracer_Minindn.py"


class NativeTracerRuntimeProfileTests(unittest.TestCase):
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
