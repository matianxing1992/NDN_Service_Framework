from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "Experiments/spec115_v3_history_rewrite.py"


class Spec115HistoryRewriteTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_tool(self, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=check,
        )

    def write_manifest(self, rows: list[dict[str, object]]) -> Path:
        path = self.root / "ownership.json"
        path.write_text(json.dumps({"changeGroups": rows}), encoding="utf-8")
        return path

    def test_manifest_accepts_one_v3_owner_and_separate_extensions(self) -> None:
        manifest = self.write_manifest([
            {
                "changeGroup": "v3-codec",
                "classification": "official-v3",
                "sourceCommits": ["old", "fix"],
                "targetCommit": "v3-new",
                "paths": ["ndn-svs/core.cpp"],
                "acceptanceGate": "v3-wire",
            },
            {
                "changeGroup": "mapping-repair",
                "classification": "fork-extension",
                "sourceCommits": ["extension"],
                "targetCommit": "extension-new",
                "paths": ["ndn-svs/svspubsub.cpp"],
                "acceptanceGate": "svspubsub",
            },
        ])

        result = self.run_tool("manifest", str(manifest))
        receipt = json.loads(result.stdout)
        self.assertTrue(receipt["valid"])
        self.assertEqual(receipt["officialV3Owner"], "v3-new")

    def test_manifest_rejects_split_v3_owners_and_extension_collision(self) -> None:
        split = self.write_manifest([
            {
                "changeGroup": "v3-codec",
                "classification": "official-v3",
                "sourceCommits": ["a"],
                "targetCommit": "v3-a",
                "paths": ["a"],
                "acceptanceGate": "a",
            },
            {
                "changeGroup": "v3-validation",
                "classification": "official-v3",
                "sourceCommits": ["b"],
                "targetCommit": "v3-b",
                "paths": ["b"],
                "acceptanceGate": "b",
            },
        ])
        failed = self.run_tool("manifest", str(split), check=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("exactly one target", failed.stderr)

        collision = self.write_manifest([
            {
                "changeGroup": "v3-codec",
                "classification": "official-v3",
                "sourceCommits": ["a"],
                "targetCommit": "v3-a",
                "paths": ["a"],
                "acceptanceGate": "a",
            },
            {
                "changeGroup": "mapping-repair",
                "classification": "fork-extension",
                "sourceCommits": ["b"],
                "targetCommit": "v3-a",
                "paths": ["b"],
                "acceptanceGate": "b",
            },
        ])
        failed = self.run_tool("manifest", str(collision), check=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("must not share", failed.stderr)

    def test_nine_owner_manifest_requires_separate_producer_and_receiver(self) -> None:
        oid = lambda value: f"{value:040x}"
        groups = [
            "pubsub-selection-name-only",
            "bounded-piggyback-delivery",
            "parallel-sync-receive",
            "ordered-async-production",
            "v2-interest-signer",
            "official-v3-protocol",
            "sparse-mapping-recovery",
            "failure-atomic-segmented-publication",
            "bounded-segmented-fetch-repair",
        ]
        document = {
            "base": oid(100),
            "sourceHead": oid(101),
            "finalHead": oid(102),
            "finalTree": oid(103),
            "owners": [
                {
                    "order": index,
                    "commit": oid(index),
                    "changeGroup": group,
                    "sourceCommits": [f"source-{index}"],
                    "boundary": f"boundary-{index}",
                }
                for index, group in enumerate(groups, 1)
            ],
            "externalHarness": {"owner": "NDNSF", "paths": ["examples/interop"]},
        }
        path = self.root / "nine.json"
        path.write_text(json.dumps(document), encoding="utf-8")

        receipt = json.loads(self.run_tool("manifest", str(path)).stdout)
        self.assertEqual(receipt["ownerCount"], 9)
        self.assertNotEqual(receipt["producerOwner"], receipt["receiverOwner"])

        document["owners"][8]["commit"] = document["owners"][7]["commit"]
        path.write_text(json.dumps(document), encoding="utf-8")
        failed = self.run_tool("manifest", str(path), check=False)
        self.assertNotEqual(failed.returncode, 0)

    def test_force_validation_requires_exact_expected_oid_lease(self) -> None:
        expected = "a" * 40
        accepted = self.run_tool(
            "force-command", "--expected-old", expected, "--",
            "git", "push", "origin",
            f"--force-with-lease=refs/heads/master:{expected}",
            "candidate:refs/heads/master",
        )
        self.assertTrue(json.loads(accepted.stdout)["valid"])

        for unsafe in (
            ["git", "push", "--force", "origin", "candidate:master"],
            ["git", "push", "--force-with-lease", "origin", "candidate:master"],
        ):
            failed = self.run_tool(
                "force-command", "--expected-old", expected, "--", *unsafe,
                check=False,
            )
            self.assertNotEqual(failed.returncode, 0)

    def test_refs_require_shared_v3_ancestor_and_master_equality(self) -> None:
        repo = self.root / "repo"
        repo.mkdir()
        subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.email", "spec115@example.invalid"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.name", "Spec 115"], cwd=repo, check=True)
        (repo / "source").write_text("v3\n", encoding="utf-8")
        subprocess.run(["git", "add", "source"], cwd=repo, check=True)
        subprocess.run(["git", "commit", "-qm", "complete v3"], cwd=repo, check=True)
        v3 = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
        subprocess.run(["git", "branch", "-M", "master"], cwd=repo, check=True)
        (repo / "source").write_text("master\n", encoding="utf-8")
        subprocess.run(["git", "commit", "-qam", "master tail"], cwd=repo, check=True)
        master = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
        subprocess.run(["git", "update-ref", "refs/remotes/origin/master", master], cwd=repo, check=True)
        subprocess.run(["git", "branch", "Experimental", master], cwd=repo, check=True)
        subprocess.run(["git", "checkout", "-q", "Experimental"], cwd=repo, check=True)
        (repo / "extension").write_text("extension\n", encoding="utf-8")
        subprocess.run(["git", "add", "extension"], cwd=repo, check=True)
        subprocess.run(["git", "commit", "-qm", "extension"], cwd=repo, check=True)

        result = self.run_tool(
            "refs", "--repo", str(repo), "--v3", v3,
            "--master", "master", "--origin-master", "origin/master",
            "--experimental", "Experimental",
        )
        receipt = json.loads(result.stdout)
        self.assertTrue(receipt["valid"])
        self.assertEqual(receipt["master"], master)


if __name__ == "__main__":
    unittest.main()
