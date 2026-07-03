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

    def test_profile_validate_reports_unknown_native_field(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            profile = {
                "distributed_inference": {
                    "native_tracer": {
                        "enabled": True,
                        "llm_planner_mode_typo": "proportional",
                    }
                }
            }
            profile_path = tmpdir / "bad-profile.json"
            profile_path.write_text(json.dumps(profile), encoding="utf-8")

            proc = subprocess.run(
                [
                    "python3",
                    str(TOOL),
                    "profile",
                    "validate",
                    "--profile",
                    str(profile_path),
                    "--require-di",
                ],
                cwd=REPO,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertNotEqual(proc.returncode, 0)
            payload = json.loads(proc.stdout)
            self.assertFalse(payload["valid"])
            self.assertIn(
                "distributed_inference.native_tracer.llm_planner_mode_typo",
                payload["errors"],
            )

    def test_di_validate_and_print_default_profile(self) -> None:
        validate_proc = subprocess.run(
            ["python3", str(TOOL), "di", "validate"],
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(validate_proc.returncode, 0, validate_proc.stderr + validate_proc.stdout)
        validate_payload = json.loads(validate_proc.stdout)
        self.assertTrue(validate_payload["valid"])
        self.assertEqual(validate_payload["errors"], [])

        print_proc = subprocess.run(
            ["python3", str(TOOL), "di", "print"],
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(print_proc.returncode, 0, print_proc.stderr + print_proc.stdout)
        payload = json.loads(print_proc.stdout)
        native = payload["resolved"]["distributed_inference"]["native_tracer"]
        self.assertTrue(payload["validation"]["valid"])
        self.assertTrue(native["enabled"])
        self.assertTrue(Path(native["harness"]).is_absolute())
        self.assertEqual(payload["resolved"]["service_name"], "/Inference/NativeTracer")

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

    def test_di_doctor_uses_native_tracer_profile_by_default(self) -> None:
        proc = subprocess.run(
            ["python3", str(TOOL), "di", "doctor"],
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
        self.assertTrue(native["enabled"])
        self.assertEqual(payload["profile"]["service_name"], "/Inference/NativeTracer")

    def test_di_launchers_print_underlying_commands(self) -> None:
        cases = [
            ("run", "Experiments/NDNSF_DI_NativeTracer_Minindn.py"),
            ("campaign", "run_llm_full_network_campaign.py"),
            ("sweep", "run_rate_sweep_campaign.py"),
            ("search", "run_llm_proportional_rps_search.py"),
        ]
        for subcommand, script in cases:
            with self.subTest(subcommand=subcommand):
                proc = subprocess.run(
                    [
                        "python3",
                        str(TOOL),
                        "di",
                        subcommand,
                        "--dry-run",
                        "--",
                        "--out-root",
                        "/tmp/ndnsf-wrapper-test",
                    ],
                    cwd=REPO,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )

                self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
                payload = json.loads(proc.stdout)
                command = payload["command"]
                self.assertIn(script, " ".join(command))
                self.assertIn("--runtime-profile", command)
                self.assertIn("examples/di-native-tracer.runtime.json", command)
                self.assertIn("--out-root", command)
                self.assertIn("/tmp/ndnsf-wrapper-test", command)


if __name__ == "__main__":
    unittest.main()
