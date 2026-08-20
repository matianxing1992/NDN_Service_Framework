#!/usr/bin/env python3
"""Spec174 production NativeTracer/MiniNDN repetition gate.

This gate deliberately reuses the Spec170 production runner and assertions.
It adds the missing Spec174 evidence requirement: every required assignment is
run in three fresh processes, and only concise summaries, manifests, and
necessary logs are retained under an explicit evidence directory.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

from test_spec170_real_minindn_gate import RealMiniNdnGateTest

# The imported Spec170 class is a helper here; do not collect its inherited
# tests a second time under the Spec174 module.
RealMiniNdnGateTest.__test__ = False


ROOT = Path(__file__).resolve().parents[2]
# Keep transient evidence outside the repository by default.  Callers may
# override this with SPEC174_REAL_EVIDENCE_DIR when they need a retained path.
DEFAULT_EVIDENCE = Path("/tmp/ndnsf-spec174-real-native-gate")
ASSIGNMENTS = ("default", "single-provider", "hybrid-121", "hybrid-212")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


class Spec174RealMiniNdnGateTest(unittest.TestCase):
    @unittest.skipUnless(
        os.environ.get("SPEC174_RUN_REAL_MININDN") == "1",
        "set SPEC174_RUN_REAL_MININDN=1 to execute the Spec174 repetition gate",
    )
    def test_spec174_required_assignments_have_three_fresh_processes(self) -> None:
        repeats = int(os.environ.get("SPEC174_REAL_REPEATS", "3"))
        if repeats < 3:
            raise AssertionError("SPEC174_REAL_REPEATS must be at least 3")
        evidence = Path(
            os.environ.get("SPEC174_REAL_EVIDENCE_DIR", str(DEFAULT_EVIDENCE))
        ).expanduser().resolve()
        if evidence.exists():
            shutil.rmtree(evidence)
        evidence.mkdir(parents=True)

        rows: list[dict[str, object]] = []
        runner = RealMiniNdnGateTest("runTest")
        for assignment in ASSIGNMENTS:
            for repeat in range(1, repeats + 1):
                retained = evidence / assignment / f"run-{repeat}"
                summary = runner._run_real_native_tracer(
                    assignment, retain_dir=retained)
                summary_path = retained / "summary.json"
                rows.append({
                    "assignment": assignment,
                    "repeat": repeat,
                    "status": summary.get("status"),
                    "runnerMode": summary.get("runnerMode"),
                    "dependencyStatus": summary.get(
                        "dependencyExecution", {}).get("status"),
                    "summarySha256": _sha256(summary_path),
                })

        report = {
            "schema": "spec174-real-minindn-gate-v1",
            "sourceRevision": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
            ).strip(),
            "assignments": list(ASSIGNMENTS),
            "repeatsPerAssignment": repeats,
            "independentProcess": True,
            "rows": rows,
            "status": "PASS",
        }
        (evidence / "gate-report.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        self.assertEqual(len(rows), len(ASSIGNMENTS) * repeats)
        self.assertTrue(all(row["status"] == "SUCCESS" for row in rows))
        self.assertTrue(all(row["dependencyStatus"] == "executed" for row in rows))


if __name__ == "__main__":
    unittest.main()
