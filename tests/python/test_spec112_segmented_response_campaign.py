from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Experiments"))

import spec112_segmented_campaign as campaign  # noqa: E402


class Spec112SegmentedCampaignTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.candidate_id = "spec112-0123456789abcdefabcd"
        self.candidate_dir = self.root / self.candidate_id
        self.candidate_dir.mkdir()
        self.manifest_path = self.candidate_dir / "candidate-manifest.json"
        self.manifest = {
            "schemaVersion": "spec112-candidate-v1",
            "candidateId": self.candidate_id,
            "identitySha256": "0123456789abcdefabcd" + "0" * 44,
            "identity": {"schemaVersion": "spec112-candidate-v1"},
        }
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def test_candidate_is_required_validated_and_cell_must_be_direct_fresh_child(self) -> None:
        cli = subprocess.run(
            [sys.executable, str(REPO / "Experiments/NDNSF_Segmented_Response_Minindn.py")],
            cwd=str(REPO),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(cli.returncode, 0)
        self.assertIn("--candidate-manifest", cli.stderr)
        self.assertIn("--output-dir", cli.stderr)

        loaded = campaign.load_candidate_manifest(self.manifest_path, verify_current=False)
        self.assertEqual(loaded["candidateId"], self.candidate_id)
        cell = self.candidate_dir / "boundary-async-normal"
        campaign.reserve_cell_directory(cell, self.manifest_path)
        self.assertTrue(cell.is_dir())
        with self.assertRaisesRegex(RuntimeError, "already exists"):
            campaign.reserve_cell_directory(cell, self.manifest_path)
        with self.assertRaisesRegex(ValueError, "direct child"):
            campaign.reserve_cell_directory(self.root / "outside", self.manifest_path)

        self.manifest_path.write_text('{"candidateId":"wrong"}', encoding="utf-8")
        with self.assertRaises(ValueError):
            campaign.load_candidate_manifest(self.manifest_path, verify_current=False)

    def test_global_ownership_lock_and_external_owner_detection(self) -> None:
        lock_path = self.root / "minindn.lock"
        owner_path = self.root / "owner.json"
        lock = campaign.acquire_minindn_ownership(
            lock_path,
            owner_path,
            process_rows=[],
            own_pid=os.getpid(),
        )
        self.addCleanup(lock.close)
        owner = json.loads(owner_path.read_text(encoding="utf-8"))
        self.assertEqual(owner["pid"], os.getpid())
        with self.assertRaisesRegex(RuntimeError, "ownership lock"):
            campaign.acquire_minindn_ownership(
                lock_path,
                self.root / "owner-2.json",
                process_rows=[],
                own_pid=os.getpid(),
            )

        rows = [
            {"pid": 9001, "ppid": 1, "cmdline": "python3 Other_MiniNDN.py --run"},
            {"pid": os.getpid(), "ppid": 1, "cmdline": "this test"},
        ]
        conflicts = campaign.find_conflicting_minindn_owners(rows, own_pid=os.getpid())
        self.assertEqual([row["pid"] for row in conflicts], [9001])

    def test_forced_environment_and_repeated_fault_plan(self) -> None:
        env = campaign.role_environment({"LD_LIBRARY_PATH": "/existing"}, REPO)
        self.assertEqual(env["NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE"], "1")
        self.assertIn(str(REPO.parent / "ndn-svs/build"), env["LD_LIBRARY_PATH"])
        self.assertTrue(env["SPEC112_FORCED_INLINE_SVS"] == "1")
        self.assertEqual(env["NDNSF_SVS_ASYNC_PUBLISH"], "1")
        sync_env = campaign.role_environment({}, REPO, svs_sync_publish=True)
        self.assertEqual(sync_env["NDNSF_SVS_ASYNC_PUBLISH"], "0")

        self.assertEqual(campaign.expand_sizes("8000x2,64,4000x2"), [8000, 8000, 64, 4000, 4000])
        normal = campaign.execution_plan("8000x2,64", "normal", "none")
        self.assertEqual(normal["expandedSizes"], [8000, 8000, 64])
        fault = campaign.execution_plan(
            "64", "targeted", "degraded-provider-after-targeted-bootstrap")
        self.assertEqual(fault["expandedSizes"], [64, 64])
        self.assertEqual(fault["pauseAfterIndex"], 0)
        with self.assertRaises(ValueError):
            campaign.execution_plan("64", "normal", "degraded-provider-after-targeted-bootstrap")

        topology = self.root / "zero.conf"
        topology.write_text("[links]\na:b loss=0 bw=1000\n", encoding="utf-8")
        self.assertEqual(campaign.verify_zero_loss_topology(topology)["configuredLossPercent"], [0.0])
        topology.write_text("[links]\na:b loss=1 bw=1000\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "0%"):
            campaign.verify_zero_loss_topology(topology)

        self.assertEqual(
            campaign.resource_stop_reason(
                self.root,
                started_monotonic=10.0,
                now_monotonic=12.0,
                wall_timeout_seconds=1.0,
                min_free_bytes=0,
            ),
            "wall-timeout",
        )
        self.assertEqual(
            campaign.resource_stop_reason(
                self.root,
                started_monotonic=10.0,
                now_monotonic=10.0,
                wall_timeout_seconds=100.0,
                min_free_bytes=2**63,
            ),
            "disk-floor",
        )

    def test_no_reference_proof_and_summary_schema_are_machine_readable_and_immutable(self) -> None:
        cell = self.candidate_dir / "boundary-async-normal"
        cell.mkdir()
        (cell / "provider.log").write_text("ordinary provider log\n", encoding="utf-8")
        (cell / "user.log").write_text("ordinary user log\n", encoding="utf-8")
        proof = campaign.no_reference_proof(cell, forced_value="1")
        self.assertTrue(proof["verified"])
        self.assertEqual(proof["markerCount"], 0)

        summary = campaign.make_cell_summary(
            candidate_id=self.candidate_id,
            cell_id=cell.name,
            mode="normal",
            svs_publish="async",
            fault_profile="none",
            requested_sizes=[64, 4000],
            results=[{"ok": True}, {"ok": False}],
            owner={"pid": 123},
            provider_epoch={"pid": 456, "session": 789},
            no_reference=proof,
            user_return_code=1,
            provider_return_code=0,
            provider_alive=True,
            user_hung=False,
            wall_stop=False,
            disk_stop=False,
            elapsed_seconds=1.25,
        )
        campaign.write_cell_and_candidate_summaries(cell, self.manifest_path, summary)
        stored = json.loads((cell / "cell-summary.json").read_text(encoding="utf-8"))
        aggregate = json.loads((self.candidate_dir / "campaign-summary.json").read_text(encoding="utf-8"))
        csv_text = (self.candidate_dir / "campaign-cells.csv").read_text(encoding="utf-8")
        self.assertEqual(stored["schemaVersion"], "spec112-segmented-cell-v1")
        self.assertEqual(stored["status"], "FAILURE")
        self.assertEqual(aggregate["cellCount"], 1)
        self.assertIn("boundary-async-normal", csv_text)
        self.assertEqual((cell / "cell-summary.json").stat().st_mode & 0o777, 0o644)
        self.assertEqual((self.candidate_dir / "campaign-summary.json").stat().st_mode & 0o777, 0o644)
        self.assertEqual((self.candidate_dir / "campaign-cells.csv").stat().st_mode & 0o777, 0o644)
        with self.assertRaisesRegex(RuntimeError, "already recorded"):
            campaign.write_cell_and_candidate_summaries(cell, self.manifest_path, summary)

        (cell / "provider.log").write_text("LARGE_RESPONSE_REFERENCE_PUBLISHED\n", encoding="utf-8")
        self.assertFalse(campaign.no_reference_proof(cell, forced_value="1")["verified"])

    def test_live_provider_without_exit_code_is_a_clean_running_state(self) -> None:
        proof = {"verified": True, "markerCount": 0}
        summary = campaign.make_cell_summary(
            candidate_id=self.candidate_id,
            cell_id="live-provider",
            mode="normal",
            svs_publish="async",
            fault_profile="none",
            requested_sizes=[8000],
            results=[{"ok": True}],
            owner={"pid": 1},
            provider_epoch={"pid": 2},
            no_reference=proof,
            user_return_code=0,
            provider_return_code=None,
            provider_alive=True,
            user_hung=False,
            wall_stop=False,
            disk_stop=False,
            elapsed_seconds=1.0,
        )
        self.assertEqual(summary["status"], "SUCCESS")
        summary["providerAlive"] = False
        self.assertFalse(summary["providerAlive"])


if __name__ == "__main__":
    unittest.main()
