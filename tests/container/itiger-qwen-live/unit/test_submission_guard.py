from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from _support import load_tool


submission = load_tool("spec110_submission")
D = "sha256:" + "a" * 64


def args(root: Path):
    script = root / "job.sh"
    script.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    return {
        "journal_path": root / "submission.json",
        "submission_id": "spec110-submission-" + "a" * 20,
        "run_id": "spec110-run-" + "a" * 20,
        "candidate_id": "spec110-c1" + "-" + "a" * 12 + "-" + "b" * 12 + "-" + "c" * 12 + "-" + "d" * 12 + "-" + "e" * 12 + "-" + "f" * 12,
        "cell_id": "spec110-cell-" + "a" * 20,
        "script_path": script,
    }


class SubmissionGuardTests(unittest.TestCase):
    def test_intent_is_durable_before_exactly_one_sbatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            values = args(Path(tmp))
            calls = []
            def runner(command):
                journal = json.loads(values["journal_path"].read_text())
                self.assertEqual(journal["state"], "INTENT_RECORDED")
                calls.append(command)
                return subprocess.CompletedProcess(command, 0, stdout="12345;cluster\n", stderr="")
            result = submission.submit_once(**values, runner=runner)
            self.assertEqual(result["state"], "SUBMITTED")
            self.assertEqual(result["jobId"], "12345")
            self.assertEqual(len(calls), 1)

    def test_existing_journal_blocks_second_sbatch_even_if_not_submitted(self):
        with tempfile.TemporaryDirectory() as tmp:
            values = args(Path(tmp))
            calls = []
            def rejected(command):
                calls.append(command)
                return subprocess.CompletedProcess(command, 1, stdout="", stderr="rejected")
            first = submission.submit_once(**values, runner=rejected)
            self.assertEqual(first["state"], "CONFIRMED_NOT_SUBMITTED")
            with self.assertRaisesRegex(submission.SubmissionError, "SUBMISSION_JOURNAL_EXISTS"):
                submission.submit_once(**values, runner=rejected)
            self.assertEqual(len(calls), 1)

    def test_unknown_outcome_is_never_resubmitted_and_can_reconcile(self):
        with tempfile.TemporaryDirectory() as tmp:
            values = args(Path(tmp))
            calls = []
            def interrupted(command):
                calls.append(command)
                raise OSError("transport lost")
            unknown = submission.submit_once(**values, runner=interrupted)
            self.assertEqual(unknown["state"], "SUBMISSION_UNKNOWN")
            with self.assertRaisesRegex(submission.SubmissionError, "SUBMISSION_JOURNAL_EXISTS"):
                submission.submit_once(**values, runner=interrupted)
            reconciled = submission.reconcile_unknown(
                values["journal_path"],
                squeue_query=lambda job_name, comment: [{"jobId": "54321", "jobName": job_name, "comment": comment}],
                sacct_query=lambda job_name, comment: [],
            )
            self.assertEqual(reconciled["state"], "SUBMITTED")
            self.assertEqual(reconciled["jobId"], "54321")
            self.assertEqual(len(calls), 1)

    def test_authoritative_empty_reconciliation_records_not_submitted(self):
        with tempfile.TemporaryDirectory() as tmp:
            values = args(Path(tmp))
            unknown = submission.submit_once(
                **values, runner=lambda command: (_ for _ in ()).throw(OSError("lost"))
            )
            self.assertEqual(unknown["state"], "SUBMISSION_UNKNOWN")
            result = submission.reconcile_unknown(
                values["journal_path"],
                squeue_query=lambda job_name, comment: [],
                sacct_query=lambda job_name, comment: [],
            )
            self.assertEqual(result["state"], "CONFIRMED_NOT_SUBMITTED")

    def test_replacement_requires_new_identity_link_and_authorization(self):
        with tempfile.TemporaryDirectory() as tmp:
            values = args(Path(tmp))
            values["replaces_submission_id"] = "spec110-submission-" + "b" * 20
            with self.assertRaisesRegex(submission.SubmissionError, "REPLACEMENT_AUTHORIZATION_REQUIRED"):
                submission.record_intent(**values)
            values["replacement_authorization_digest"] = D
            record = submission.record_intent(**values)
            self.assertEqual(record["replacesSubmissionId"], "spec110-submission-" + "b" * 20)


if __name__ == "__main__":
    unittest.main()
