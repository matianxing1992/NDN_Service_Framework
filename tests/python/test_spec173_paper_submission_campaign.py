import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

import yaml


REPO = Path(__file__).resolve().parents[2]
RUNNER = REPO / "Experiments" / "paper_submission_campaign.py"
REGISTRATION = (
    REPO / "specs/173-paper-submission-evidence/contracts/experiment-registration.yaml"
)
TOOLCHAIN = (
    REPO / "specs/173-paper-submission-evidence/evidence/toolchain-manifest.json"
)


def load_runner_module():
    spec = importlib.util.spec_from_file_location("paper_submission_campaign", RUNNER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class PaperSubmissionCampaignTests(unittest.TestCase):
    def run_dry_run(self, output: Path, mode: str):
        completed = subprocess.run(
            [
                sys.executable,
                str(RUNNER),
                "--registration",
                str(REGISTRATION),
                "--toolchain-manifest",
                str(TOOLCHAIN),
                "--output-root",
                str(output),
                "--mode",
                mode,
                "--dry-run",
            ],
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)
        return json.loads((output / "dry-run-plan.json").read_text())

    def test_pilot_dry_run_expands_frozen_plan_without_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "pilot"
            plan = self.run_dry_run(output, "pilot")
            self.assertEqual(plan["mode"], "pilot")
            self.assertFalse(plan["manuscriptEligible"])
            self.assertEqual(len(plan["blocks"]), 4)
            self.assertEqual(sum(len(block["cells"]) for block in plan["blocks"]), 6)
            self.assertEqual(
                plan["registrationSha256"],
                plan["campaignManifest"]["registrationSha256"],
            )
            self.assertEqual(
                plan["toolchainManifestSha256"],
                plan["campaignManifest"]["toolchainManifestSha256"],
            )
            self.assertFalse(any(output.rglob("cell-result.json")))

    def test_confirmatory_dry_run_expands_every_registered_cell_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            plan = self.run_dry_run(Path(temporary) / "confirmatory", "confirmatory")
            self.assertTrue(plan["manuscriptEligible"])
            self.assertEqual(len(plan["blocks"]), 16)
            cells = [cell for block in plan["blocks"] for cell in block["cells"]]
            self.assertEqual(len(cells), 37)
            self.assertEqual(len({cell["cellId"] for cell in cells}), 37)
            by_comparison = {}
            for cell in cells:
                by_comparison[cell["comparison"]] = by_comparison.get(cell["comparison"], 0) + 1
            self.assertEqual(
                by_comparison,
                {
                    "one-provider-baseline": 18,
                    "admission-control": 12,
                    "custom-selection": 6,
                    "selective-ack-correctness": 1,
                },
            )

    def test_confirmatory_block_and_system_order_is_deterministic(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = self.run_dry_run(root / "first", "confirmatory")
            second = self.run_dry_run(root / "second", "confirmatory")
            first_order = [
                (block["blockId"], [cell["system"] for cell in block["cells"]])
                for block in first["blocks"]
            ]
            second_order = [
                (block["blockId"], [cell["system"] for cell in block["cells"]])
                for block in second["blocks"]
            ]
            self.assertEqual(first_order, second_order)
            self.assertTrue(any(
                systems != ["ndnsf", "grpc", "nsc"]
                for block_id, systems in first_order
                if block_id.startswith("one-provider-baseline")
            ))

    def test_commands_preserve_registered_workload_and_mechanism_semantics(self):
        with tempfile.TemporaryDirectory() as temporary:
            plan = self.run_dry_run(Path(temporary) / "confirmatory", "confirmatory")
            cells = {cell["cellId"]: cell for block in plan["blocks"] for cell in block["cells"]}

            def command(comparison, repetition, rate, system):
                return cells[f"{comparison}--{repetition}--rps-{rate}--{system}"]["command"]

            ndnsf = command("one-provider-baseline", "r1", 10, "ndnsf")
            for token in (
                "--workload-mode", "open-loop", "--rate-rps", "10.0",
                "--duration", "60", "--warmup", "10", "--request-timeout-ms", "5000",
                "--timeout-ms", "5000", "--disable-adaptive-admission-control",
            ):
                self.assertIn(token, ndnsf)
            self.assertEqual(ndnsf[ndnsf.index("--provider-nodes") + 1], "ucla")

            admission = command("admission-control", "r2", 100, "ndnsf-admission-enabled")
            self.assertIn("--adaptive-admission-control", admission)
            self.assertNotIn("--disable-adaptive-admission-control", admission)

            custom = command("custom-selection", "r3", 30, "ndnsf-custom-selection")
            self.assertEqual(custom[custom.index("--strategy") + 1], "custom-selection")
            self.assertEqual(custom[custom.index("--ack-timeout-ms") + 1], "100")
            self.assertEqual(
                custom[custom.index("--provider-request-delay-ms-series") + 1],
                "100,20,5",
            )
            self.assertIn("--disable-adaptive-admission-control", custom)

            grpc = command("one-provider-baseline", "r1", 100, "grpc")
            self.assertEqual(grpc[grpc.index("--count") + 1], "6000")
            self.assertEqual(grpc[grpc.index("--timeout-s") + 1], "5.0")
            self.assertEqual(grpc[grpc.index("--seed") + 1], "17301")

            nsc = command("one-provider-baseline", "r1", 100, "nsc")
            self.assertEqual(nsc[nsc.index("--rate-series") + 1], "100.0")
            self.assertEqual(nsc[nsc.index("--request-deadline-ms") + 1], "5000")

    def test_registration_is_frozen_before_plan_expansion(self):
        registration = yaml.safe_load(REGISTRATION.read_text())
        self.assertEqual(registration["status"], "refrozen-after-preimplementation-audit")
        self.assertIn("frozenAt", registration)

    def test_preflight_validates_frozen_toolchain_without_starting_minindn(self):
        completed = subprocess.run(
            [
                sys.executable,
                str(RUNNER),
                "--registration", str(REGISTRATION),
                "--toolchain-manifest", str(TOOLCHAIN),
                "--preflight",
            ],
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)
        report = json.loads(completed.stdout)
        self.assertEqual(report["status"], "pass")
        self.assertFalse(report["miniNdnStarted"])
        self.assertTrue(all(check["pass"] for check in report["checks"]))

    def test_attempt_directories_are_monotonic_and_never_overwritten(self):
        runner = load_runner_module()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = runner.next_attempt_directory(root)
            self.assertEqual(first.name, "attempt-0001")
            first.mkdir()
            (first / "evidence.txt").write_text("keep me")
            second = runner.next_attempt_directory(root)
            self.assertEqual(second.name, "attempt-0002")
            self.assertEqual((first / "evidence.txt").read_text(), "keep me")

    def test_resume_accepts_only_hash_verified_terminal_cell(self):
        runner = load_runner_module()
        with tempfile.TemporaryDirectory() as temporary:
            cell_dir = Path(temporary) / "ndnsf"
            cell_dir.mkdir()
            summary = cell_dir / "summary.json"
            summary.write_text('{"successful": 10}\n')
            expected = {
                "cellId": "one-provider-baseline--r1--rps-10--ndnsf",
                "comparison": "one-provider-baseline",
                "repetition": "r1",
                "seed": 17301,
                "rateRps": 10,
                "system": "ndnsf",
                "outputDirectory": str(cell_dir),
                "command": ["fake", "--output-dir", str(cell_dir)],
            }
            manifest = runner.cell_manifest(
                expected,
                registration_sha256="a" * 64,
                toolchain_sha256="b" * 64,
                toolchain={"source": {"ndnsf": {"head": "c" * 40}}},
            )
            (cell_dir / "cell-manifest.json").write_text(
                json.dumps(manifest, sort_keys=True) + "\n"
            )
            result = {
                "schemaVersion": 1,
                "status": "valid",
                "exitCode": 0,
                "requiredSummaries": ["summary.json"],
                "artifactHashes": {"summary.json": runner.sha256_file(summary)},
            }
            (cell_dir / "cell-result.json").write_text(
                json.dumps(result, sort_keys=True) + "\n"
            )
            valid, reason = runner.verify_cell_attempt(
                cell_dir, expected, "a" * 64, "b" * 64
            )
            self.assertTrue(valid, reason)

            summary.write_text('{"successful": 11}\n')
            valid, reason = runner.verify_cell_attempt(
                cell_dir, expected, "a" * 64, "b" * 64
            )
            self.assertFalse(valid)
            self.assertIn("hash", reason)

    def test_partial_or_failed_cell_is_never_reusable(self):
        runner = load_runner_module()
        with tempfile.TemporaryDirectory() as temporary:
            cell_dir = Path(temporary)
            expected = {
                "cellId": "cell",
                "comparison": "comparison",
                "repetition": "r1",
                "seed": 1,
                "rateRps": 1,
                "system": "ndnsf",
                "outputDirectory": str(cell_dir),
                "command": ["fake"],
            }
            (cell_dir / "cell-manifest.json").write_text(json.dumps(
                runner.cell_manifest(expected, "a" * 64, "b" * 64, {"source": {}})
            ))
            (cell_dir / "cell-result.json").write_text(json.dumps({
                "schemaVersion": 1,
                "status": "invalid",
                "exitCode": 1,
                "requiredSummaries": [],
                "artifactHashes": {},
            }))
            valid, reason = runner.verify_cell_attempt(
                cell_dir, expected, "a" * 64, "b" * 64
            )
            self.assertFalse(valid)
            self.assertIn("terminal valid", reason)

    def test_run_cell_writes_terminal_hashes_and_refuses_overwrite(self):
        runner = load_runner_module()
        toolchain = json.loads(TOOLCHAIN.read_text())
        with tempfile.TemporaryDirectory() as temporary:
            cell_dir = Path(temporary) / "cell"
            cell = {
                "cellId": "cell",
                "comparison": "one-provider-baseline",
                "repetition": "r1",
                "seed": 17301,
                "rateRps": 10,
                "system": "ndnsf",
                "outputDirectory": str(cell_dir),
                "command": ["fake", "--output-dir", str(cell_dir)],
            }
            calls = []

            def fake_run(command, **kwargs):
                calls.append(command)
                Path(cell["outputDirectory"], "summary.json").write_text(
                    '{"scheduled_requests": 600, "successful_requests": 600}\n'
                )
                Path(cell["outputDirectory"], "request-trace.csv").write_text(
                    "request,latency_ms\n1,5.0\n"
                )
                kwargs["stdout"].write("fake command completed\n")
                return SimpleNamespace(returncode=0)

            valid, reason = runner.run_cell(
                cell, "a" * 64, "b" * 64, toolchain, command_runner=fake_run
            )
            self.assertTrue(valid, reason)
            self.assertEqual(calls, [cell["command"]])
            result = json.loads((cell_dir / "cell-result.json").read_text())
            self.assertIn("request-trace.csv", result["artifactHashes"])
            valid, reason = runner.verify_cell_attempt(
                cell_dir, cell, "a" * 64, "b" * 64
            )
            self.assertTrue(valid, reason)
            with self.assertRaises(FileExistsError):
                runner.run_cell(
                    cell, "a" * 64, "b" * 64, toolchain, command_runner=fake_run
                )

    def test_failed_member_invalidates_whole_block_without_retry(self):
        runner = load_runner_module()
        toolchain = json.loads(TOOLCHAIN.read_text())
        with tempfile.TemporaryDirectory() as temporary:
            attempt = Path(temporary) / "attempt-0001"
            cells = []
            for system in ("ndnsf", "grpc"):
                cell_dir = attempt / system
                cells.append({
                    "cellId": f"block--{system}",
                    "comparison": "one-provider-baseline",
                    "repetition": "r1",
                    "seed": 17301,
                    "rateRps": 10,
                    "system": system,
                    "outputDirectory": str(cell_dir),
                    "command": ["fake", system, "--output-dir", str(cell_dir)],
                })
            call_count = 0

            def fake_run(command, **kwargs):
                nonlocal call_count
                call_count += 1
                if command[1] == "ndnsf":
                    Path(cells[0]["outputDirectory"], "summary.json").write_text("{}\n")
                    return SimpleNamespace(returncode=0)
                return SimpleNamespace(returncode=3)

            valid, reason = runner.execute_block(
                "block", cells, attempt, "a" * 64, "b" * 64, toolchain,
                command_runner=fake_run,
            )
            self.assertFalse(valid)
            self.assertEqual(call_count, 2)
            result = json.loads((attempt / "block-result.json").read_text())
            self.assertEqual(result["status"], "infrastructure-invalid")
            self.assertEqual(result["rerunScope"], "whole-matched-block")
            self.assertIn("--resume", result["rerunInstruction"])

    def test_valid_block_is_reusable_only_when_every_member_verifies(self):
        runner = load_runner_module()
        toolchain = json.loads(TOOLCHAIN.read_text())
        with tempfile.TemporaryDirectory() as temporary:
            attempt = Path(temporary) / "attempt-0001"
            cells = []
            for system in ("ndnsf", "grpc"):
                cell_dir = attempt / system
                cells.append({
                    "cellId": f"block--{system}",
                    "comparison": "one-provider-baseline",
                    "repetition": "r1",
                    "seed": 17301,
                    "rateRps": 10,
                    "system": system,
                    "outputDirectory": str(cell_dir),
                    "command": ["fake", system, "--output-dir", str(cell_dir)],
                })

            def fake_run(command, **kwargs):
                target = Path(command[-1])
                (target / "summary.json").write_text("{}\n")
                return SimpleNamespace(returncode=0)

            valid, reason = runner.execute_block(
                "block", cells, attempt, "a" * 64, "b" * 64, toolchain,
                command_runner=fake_run,
            )
            self.assertTrue(valid, reason)
            valid, reason = runner.verify_block_attempt(
                "block", cells, attempt, "a" * 64, "b" * 64
            )
            self.assertTrue(valid, reason)
            (attempt / "grpc" / "summary.json").write_text('{"changed": true}\n')
            valid, reason = runner.verify_block_attempt(
                "block", cells, attempt, "a" * 64, "b" * 64
            )
            self.assertFalse(valid)
            self.assertIn("grpc", reason)


if __name__ == "__main__":
    unittest.main()
