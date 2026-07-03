import json
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "ndnsf_runtime.py"


class RuntimeDoctorTests(unittest.TestCase):
    def test_doctor_generates_missing_token_file_and_events(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            profile = {
                "name": "doctor-test",
                "controller": {
                    "prefix": "/example/hello/controller",
                    "policy_file": "examples/hello.policies",
                    "trust_schema": "examples/trust-schema.conf",
                    "bootstrap_token_file": str(tmpdir / "generated.tokens"),
                },
                "provider": {"identity": "/example/hello/provider"},
                "user": {"identity": "/example/hello/user"},
                "service_name": "/HELLO",
            }
            profile_path = tmpdir / "profile.json"
            event_log = tmpdir / "events.jsonl"
            resolved_path = tmpdir / "resolved.json"
            profile_path.write_text(json.dumps(profile), encoding="utf-8")

            proc = subprocess.run(
                [
                    "python3",
                    str(TOOL),
                    "doctor",
                    "--profile",
                    str(profile_path),
                    "--fix",
                    "--event-log",
                    str(event_log),
                    "--write-resolved",
                    str(resolved_path),
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            token_lines = [
                line
                for line in (tmpdir / "generated.tokens").read_text(encoding="utf-8").splitlines()
                if line and not line.startswith("#")
            ]
            self.assertEqual(len(token_lines), 5)
            self.assertTrue(all(len(line.split()[1]) == 8 for line in token_lines))

            events = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]
            self.assertTrue(any(event["event"] == "TOKEN_FILE_GENERATED" for event in events))
            resolved = json.loads(resolved_path.read_text(encoding="utf-8"))
            self.assertTrue(resolved["ready"])
            self.assertEqual(resolved["token_file"]["bad_token_count"], 0)

    def test_doctor_reports_missing_token_without_fix(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            profile = {
                "controller": {
                    "policy_file": "examples/hello.policies",
                    "trust_schema": "examples/trust-schema.conf",
                    "bootstrap_token_file": str(tmpdir / "missing.tokens"),
                }
            }
            profile_path = tmpdir / "profile.json"
            profile_path.write_text(json.dumps(profile), encoding="utf-8")
            proc = subprocess.run(
                ["python3", str(TOOL), "doctor", "--profile", str(profile_path)],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertNotEqual(proc.returncode, 0)
            payload = json.loads(proc.stdout)
            self.assertFalse(payload["token_file"]["exists"])

    def test_doctor_resolves_native_tracer_profile(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            event_log = tmpdir / "events.jsonl"
            resolved_path = tmpdir / "resolved.json"

            proc = subprocess.run(
                [
                    "python3",
                    str(TOOL),
                    "doctor",
                    "--profile",
                    "examples/di-native-tracer.runtime.json",
                    "--event-log",
                    str(event_log),
                    "--write-resolved",
                    str(resolved_path),
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
            payload = json.loads(proc.stdout)
            native = payload["distributed_inference"]["native_tracer"]
            self.assertTrue(native["ready"])
            self.assertEqual(native["missing_topology_nodes"], [])
            self.assertTrue(all(native["file_status"].values()))
            self.assertTrue(all(native["binaries"].values()))
            command = native["command"]
            self.assertIn("Experiments/NDNSF_DI_NativeTracer_Minindn.py", command)
            self.assertIn("--policy-bundle", command)
            self.assertIn("llm-proportional", command)
            self.assertIn("--local-execution-only", command)

            resolved = json.loads(resolved_path.read_text(encoding="utf-8"))
            resolved_native = resolved["profile"]["distributed_inference"]["native_tracer"]
            self.assertTrue(Path(resolved_native["harness"]).is_absolute())
            self.assertTrue(Path(resolved_native["tracer_dir"]).is_absolute())
            events = [json.loads(line) for line in event_log.read_text(encoding="utf-8").splitlines()]
            self.assertTrue(any(event["event"] == "DI_NATIVE_TRACER_PREFLIGHT" for event in events))


if __name__ == "__main__":
    unittest.main()
