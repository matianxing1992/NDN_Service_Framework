from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "specs/168-itiger-di-deployment-fidelity/jobs/spec168_campaign.py"


def digest(character: str) -> str:
    return "sha256:" + character * 64


class Spec168CampaignContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "campaigns"
        self.results = self.root / "results"
        self.baseline = self.root / "baseline.json"
        self.bindings = self.root / "bindings.json"
        self.baseline.write_text(json.dumps({
            "schema": "ndnsf-di.spec168-baseline.v1",
            "runtime": {"path": "/project/runtime.sif", "sha256": digest("2")},
            "smallModelControl": {
                "stageManifestPath": "/project/small/stage-manifest.json",
                "stageManifestSha256": "3" * 64,
            },
            "largeModelNegativeControl": {
                "stageManifestPath": "/project/large/stage-manifest.json",
                "stageManifestSha256": "4" * 64,
            },
        }, sort_keys=True), encoding="utf-8")
        self.binding_data = {
            "baselineDigest": "sha256:" + hashlib.sha256(
                self.baseline.read_bytes()).hexdigest(),
            "sourceDigest": digest("1"),
            "runtimeSifDigest": digest("2"),
            "localFixtureManifestDigest": digest("a"),
            "remoteSmallStageManifestDigest": digest("3"),
            "remoteLargeStageManifestDigest": digest("4"),
            "sourceBundleDigest": digest("b"),
            "localGateManifestDigest": digest("c"),
            "exactSifPreflightDigest": digest("d"),
            "strategyDigest": digest("5"),
            "promptSetDigest": digest("6"),
            "routeDigest": digest("7"),
            "analyzerDigest": digest("8"),
            "scheduleDigest": digest("9"),
        }
        self.bindings.write_text(
            json.dumps(self.binding_data, sort_keys=True), encoding="utf-8")

    def run_cli(self, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            check=check,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def freeze(self, *, check: bool = True) -> subprocess.CompletedProcess[str]:
        return self.run_cli(
            "freeze", "--baseline", str(self.baseline),
            "--bindings", str(self.bindings),
            "--output-root", str(self.output), check=check,
        )

    def test_freeze_is_atomic_content_addressed_and_run_once(self) -> None:
        created = self.freeze()
        campaign_id = created.stdout.strip()
        self.assertRegex(campaign_id, r"^spec168-campaign-v3-[0-9a-f]{20}$")
        campaign_root = self.output / campaign_id
        self.assertEqual(
            {"experiment-manifest.json", "schedule-manifest.json", "campaign-manifest.json"},
            {item.name for item in campaign_root.iterdir()},
        )
        campaign = json.loads((campaign_root / "campaign-manifest.json").read_text())
        self.assertEqual(campaign_id, campaign["campaignId"])
        self.assertEqual("FROZEN", campaign["state"])
        experiment = json.loads(
            (campaign_root / "experiment-manifest.json").read_text())
        self.assertRegex(
            experiment["candidateId"], r"^spec168-candidate-v3-[0-9a-f]{20}$")
        self.assertEqual(
            "content-addressed:" + digest("a"),
            experiment["assetReferences"]["localFixtureStageManifest"],
        )
        repeated = self.freeze(check=False)
        self.assertNotEqual(0, repeated.returncode)
        self.assertIn("already exists", repeated.stderr)

    def test_every_binding_is_required_and_changes_identity(self) -> None:
        first = self.freeze().stdout.strip()
        baseline_bound = {
            "baselineDigest", "runtimeSifDigest",
            "remoteSmallStageManifestDigest", "remoteLargeStageManifestDigest",
        }
        for index, key in enumerate(
            sorted(set(self.binding_data) - baseline_bound), start=10
        ):
            changed = dict(self.binding_data)
            changed[key] = "sha256:" + format(index, "x")[-1] * 64
            candidate = self.root / f"bindings-{key}.json"
            candidate.write_text(json.dumps(changed), encoding="utf-8")
            result = self.run_cli(
                "derive", "--baseline", str(self.baseline),
                "--bindings", str(candidate),
            ).stdout.strip()
            self.assertNotEqual(first, result, key)
        alternate_baseline = self.root / "baseline-alternate.json"
        alternate = json.loads(self.baseline.read_text())
        alternate["lineageNote"] = "distinct valid baseline"
        alternate_baseline.write_text(json.dumps(alternate, sort_keys=True))
        changed = dict(self.binding_data)
        changed["baselineDigest"] = "sha256:" + hashlib.sha256(
            alternate_baseline.read_bytes()).hexdigest()
        alternate_bindings = self.root / "bindings-baselineDigest.json"
        alternate_bindings.write_text(json.dumps(changed))
        result = self.run_cli(
            "derive", "--baseline", str(alternate_baseline),
            "--bindings", str(alternate_bindings),
        ).stdout.strip()
        self.assertNotEqual(first, result, "baselineDigest")
        baseline_locations = {
            "runtimeSifDigest": ("runtime", "sha256"),
            "remoteSmallStageManifestDigest": (
                "smallModelControl", "stageManifestSha256"),
            "remoteLargeStageManifestDigest": (
                "largeModelNegativeControl", "stageManifestSha256"),
        }
        for index, (key, location) in enumerate(baseline_locations.items(), start=13):
            alternate = json.loads(self.baseline.read_text())
            replacement = format(index, "x")[-1] * 64
            alternate[location[0]][location[1]] = replacement
            baseline_path = self.root / f"baseline-{key}.json"
            baseline_path.write_text(json.dumps(alternate, sort_keys=True))
            changed = dict(self.binding_data)
            changed[key] = "sha256:" + replacement
            changed["baselineDigest"] = "sha256:" + hashlib.sha256(
                baseline_path.read_bytes()).hexdigest()
            binding_path = self.root / f"bindings-{key}.json"
            binding_path.write_text(json.dumps(changed))
            result = self.run_cli(
                "derive", "--baseline", str(baseline_path),
                "--bindings", str(binding_path),
            ).stdout.strip()
            self.assertNotEqual(first, result, key)
        missing = dict(self.binding_data)
        missing.pop("sourceDigest")
        self.bindings.write_text(json.dumps(missing), encoding="utf-8")
        failed = self.freeze(check=False)
        self.assertNotEqual(0, failed.returncode)
        self.assertIn("BINDING_FIELDS_INVALID", failed.stderr)

    def test_validate_rejects_manifest_mutation(self) -> None:
        campaign_id = self.freeze().stdout.strip()
        path = self.output / campaign_id / "campaign-manifest.json"
        self.run_cli("validate", "--manifest", str(path))
        document = json.loads(path.read_text())
        document["bindingDigests"]["sourceDigest"] = digest("a")
        path.write_text(json.dumps(document), encoding="utf-8")
        failed = self.run_cli("validate", "--manifest", str(path), check=False)
        self.assertNotEqual(0, failed.returncode)
        self.assertIn("FROZEN_CAMPAIGN_MUTATED", failed.stderr)

    def test_schedule_is_ordered_gated_and_never_auto_submits(self) -> None:
        campaign_id = self.freeze().stdout.strip()
        path = self.output / campaign_id / "schedule-manifest.json"
        schedule = json.loads(path.read_text())
        self.assertEqual([
            "focused", "real-minindn", "exact-container-overlay",
            "exact-sif-cuda-preflight", "candidate-audit",
            "remote-small-single", "remote-small-repeated", "remote-large-single",
            "clean-reproduction",
        ], [item["phase"] for item in schedule["phases"]])
        self.assertTrue(schedule["manualRemoteSubmissionOnly"])
        self.assertFalse(schedule["automaticRetry"])
        for index, item in enumerate(schedule["phases"]):
            self.assertEqual(schedule["phases"][:index][-1:][0]["phase"] if index else None,
                             item["requires"][-1] if item["requires"] else None)

    def test_claim_is_single_writer_and_payload_free(self) -> None:
        campaign_id = self.freeze().stdout.strip()
        manifest = self.output / campaign_id / "campaign-manifest.json"
        claimed = self.run_cli(
            "claim", "--manifest", str(manifest),
            "--result-root", str(self.results),
        )
        run_root = Path(claimed.stdout.strip())
        self.assertEqual({"run-claim.json"}, {item.name for item in run_root.iterdir()})
        self.assertLess(sum(item.stat().st_size for item in run_root.iterdir()), 4096)
        self.assertFalse(list(run_root.rglob("*.sif")))
        self.assertFalse(list(run_root.rglob("*.pt")))
        repeated = self.run_cli(
            "claim", "--manifest", str(manifest),
            "--result-root", str(self.results), check=False,
        )
        self.assertNotEqual(0, repeated.returncode)
        self.assertIn("already claimed", repeated.stderr)

    def test_baseline_runtime_and_stage_digests_must_match_bindings(self) -> None:
        changed = dict(self.binding_data)
        changed["runtimeSifDigest"] = digest("f")
        self.bindings.write_text(json.dumps(changed), encoding="utf-8")
        failed = self.freeze(check=False)
        self.assertNotEqual(0, failed.returncode)
        self.assertIn("BASELINE_RUNTIME_MISMATCH", failed.stderr)


if __name__ == "__main__":
    unittest.main()
