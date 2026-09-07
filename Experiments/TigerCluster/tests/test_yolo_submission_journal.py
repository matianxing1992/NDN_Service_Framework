"""Real filesystem/process submission coordination, without a Slurm job."""
from pathlib import Path
import json
import os
import select
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def test_only_one_active_run_can_reserve_candidate_gate(tmp_path):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64,
                                gate="two-node-gpu")
    first = journal.reserve("test-first")
    assert first["state"] == "PREPARED"
    with pytest.raises(JournalError, match="ACTIVE_RUN"):
        journal.reserve("test-second")
    assert journal.get("test-first") == first


def test_unknown_submission_is_reconciled_not_retried(tmp_path):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64,
                                gate="two-node-gpu")
    first = journal.reserve("test-first")
    journal.mark_submitting("test-first")
    journal.mark_unknown("test-first")
    assert journal.reconcile("test-first", [])['state'] == 'SUBMISSION_UNKNOWN'
    with pytest.raises(JournalError, match="INVALID_TRANSITION"):
        journal.mark_submitting("test-first")
    with pytest.raises(JournalError, match="ACTIVE_RUN"):
        journal.reserve("test-second")
    matched = journal.reconcile("test-first", [{"comment": first["submissionKey"], "jobId": "197200"}])
    assert matched["state"] == "SUBMITTED"
    assert matched["jobId"] == "197200"


def test_terminal_run_is_retained_and_cannot_be_reused(tmp_path):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64,
                                gate="two-node-gpu")
    journal.reserve("test-first")
    journal.mark_submitting("test-first")
    journal.record_submission("test-first", "197200")
    journal.mark_running("test-first", "197200")
    with pytest.raises(JournalError, match="JOURNAL_JOB_MISMATCH"):
        journal.finish("test-first", "197201", "PASS")
    journal.finish("test-first", "197200", "FAIL")
    with pytest.raises(JournalError, match="INVALID_TRANSITION"):
        journal.finish("test-first", "197200", "PASS")
    journal.reserve("test-second")
    assert journal.get("test-first")["state"] == "FAIL"
    with pytest.raises(JournalError, match="RUN_ALREADY_REGISTERED"):
        journal.reserve("test-first")


def test_two_independent_processes_cannot_both_reserve(tmp_path):
    script = """
import sys
sys.path.insert(0, sys.argv[1])
from runtime.yolo_submission import SubmissionJournal, JournalError
journal = SubmissionJournal(sys.argv[2], candidate_id='sha256:' + 'a'*64, gate='two-node-gpu')
print('READY', flush=True)
sys.stdin.readline()
try:
    journal.reserve(sys.argv[3])
    print('RESERVED', flush=True)
except JournalError as e:
    print(str(e), flush=True)
"""
    children = [subprocess.Popen([sys.executable, "-B", "-c", script, str(ROOT), str(tmp_path), name],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True) for name in ("test-first", "test-second")]
    try:
        for child in children:
            assert select.select([child.stdout], [], [], 5)[0], "child failed to reach barrier"
            assert child.stdout.readline().strip() == "READY"
        for child in children:
            child.stdin.write("go\n")
            child.stdin.flush()
        outputs = [child.communicate(timeout=10) for child in children]
        assert all(child.returncode == 0 for child in children), outputs
        assert sum(out.strip() == "RESERVED" for out, _ in outputs) == 1
        assert sum(out.startswith("ACTIVE_RUN:") for out, _ in outputs) == 1
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)


def test_crash_after_submitting_never_reopens_reservation(tmp_path):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    script = """
import os, sys
sys.path.insert(0, sys.argv[1])
from runtime.yolo_submission import SubmissionJournal
j = SubmissionJournal(sys.argv[2], candidate_id='sha256:' + 'a'*64, gate='two-node-gpu')
j.reserve('test-first')
j.mark_submitting('test-first')
os._exit(23)
"""
    child = subprocess.run([sys.executable, "-B", "-c", script, str(ROOT), str(tmp_path)], timeout=10)
    assert child.returncode == 23
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64, gate="two-node-gpu")
    assert journal.get("test-first")["state"] == "SUBMITTING"
    assert journal.reconcile("test-first", [])["state"] == "SUBMISSION_UNKNOWN"
    with pytest.raises(JournalError, match="ACTIVE_RUN"):
        journal.reserve("test-second")


def test_ambiguous_and_unrelated_query_does_not_authorize_retry(tmp_path):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64, gate="two-node-gpu")
    first = journal.reserve("test-first")
    journal.mark_submitting("test-first")
    assert journal.reconcile("test-first", [{"comment": "another-run", "jobId": "10"}])["state"] == "SUBMISSION_UNKNOWN"
    with pytest.raises(JournalError, match="DUPLICATE_SUBMISSION"):
        journal.reconcile("test-first", [{"comment": first["submissionKey"], "jobId": n} for n in ("10", "11")])
    assert journal.get("test-first")["state"] == "SUBMISSION_UNKNOWN"
    with pytest.raises(JournalError, match="INVALID_TRANSITION"):
        journal.finish("test-first", "10", "INCOMPLETE")


@pytest.mark.parametrize("mutation", ["state-type", "missing-job", "candidate", "active"])
def test_corrupt_record_rejected_without_overwrite(tmp_path, mutation):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64, gate="two-node-gpu")
    journal.reserve("test-first")
    path = next(tmp_path.glob("*.json"))
    value = json.loads(path.read_text())
    if mutation == "state-type": value["runs"]["test-first"]["state"] = []
    elif mutation == "missing-job": value["runs"]["test-first"]["state"] = "RUNNING"
    elif mutation == "candidate": value["candidateId"] = "sha256:" + "b" * 64
    elif mutation == "active": value["activeRunId"] = None
    path.write_text(json.dumps(value))
    before = path.read_bytes()
    with pytest.raises(JournalError, match="JOURNAL_"):
        journal.get("test-first")
    assert path.read_bytes() == before


def test_atomic_replace_failure_preserves_previous_record(tmp_path, monkeypatch):
    from runtime.yolo_submission import SubmissionJournal
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64, gate="two-node-gpu")
    journal.reserve("test-first")
    path = next(tmp_path.glob("*.json"))
    before = path.read_bytes()
    def fail(*args):
        raise OSError("fixture disk failure")
    monkeypatch.setattr(os, "replace", fail)
    with pytest.raises(OSError, match="fixture disk failure"):
        journal.mark_submitting("test-first")
    assert journal.get("test-first")["state"] == "PREPARED"
    assert path.read_bytes() == before
    assert not list(tmp_path.glob("*.tmp-*"))


def test_directory_fsync_failure_leaves_submission_closed(tmp_path, monkeypatch):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64, gate="two-node-gpu")
    journal.reserve("test-first")
    original = os.fsync
    calls = []
    def fail_directory(fd):
        calls.append(fd)
        if len(calls) == 2:
            raise OSError("fixture directory sync failure")
        return original(fd)
    monkeypatch.setattr(os, "fsync", fail_directory)
    with pytest.raises(OSError, match="fixture directory sync failure"):
        journal.mark_submitting("test-first")
    assert journal.get("test-first")["state"] == "SUBMITTING"
    with pytest.raises(JournalError, match="ACTIVE_RUN"):
        journal.reserve("test-second")


def test_symlink_journal_cannot_redirect_writes(tmp_path):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64, gate="two-node-gpu")
    journal.reserve("test-first")
    path = next(tmp_path.glob("*.json"))
    saved = path.read_bytes()
    target = tmp_path / "old-record"
    path.rename(target)
    path.symlink_to(target)
    with pytest.raises(JournalError, match="JOURNAL_DOCUMENT"):
        journal.mark_submitting("test-first")
    assert target.read_bytes() == saved


@pytest.mark.parametrize("job_id", [True, "", "0", "12;34", "-1"])
def test_malformed_job_id_never_advances_submission(tmp_path, job_id):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64, gate="two-node-gpu")
    journal.reserve("test-first")
    journal.mark_submitting("test-first")
    with pytest.raises(JournalError, match="JOURNAL_JOB_ID"):
        journal.record_submission("test-first", job_id)
    assert journal.get("test-first")["state"] == "SUBMITTING"


def test_cancel_only_before_submission_and_preserve_run_history(tmp_path):
    from runtime.yolo_submission import SubmissionJournal, JournalError
    journal = SubmissionJournal(tmp_path, candidate_id="sha256:" + "a" * 64, gate="two-node-gpu")
    journal.reserve("test-first")
    assert journal.cancel_prepared("test-first")["state"] == "CANCELLED_BEFORE_SUBMIT"
    journal.reserve("test-second")
    journal.mark_submitting("test-second")
    with pytest.raises(JournalError, match="INVALID_TRANSITION"):
        journal.cancel_prepared("test-second")
    assert journal.get("test-second")["state"] == "SUBMITTING"
    with pytest.raises(JournalError, match="RUN_ALREADY_REGISTERED"):
        journal.reserve("test-first")
