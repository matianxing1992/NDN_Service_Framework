#!/usr/bin/env python3
"""Crash-safe, at-most-once Slurm submission journal for Spec 110."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from typing import Any, Callable, Iterable, Mapping


DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
SUBMISSION_RE = re.compile(r"^spec110-submission-[0-9a-f]{20}$")
RUN_RE = re.compile(r"^spec110-run-[0-9a-f]{20}$")
CELL_RE = re.compile(r"^spec110-cell-[0-9a-f]{20}$")
CANDIDATE_RE = re.compile(r"^spec110-c1(?:-[0-9a-f]{12}){6}$")


class SubmissionError(ValueError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise SubmissionError(code + (f":{detail}" if detail else ""))


def _canonical(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def _digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _digest(value: object) -> str:
    return _digest_bytes(_canonical(value))


def _timestamp() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _atomic_create(path: Path, value: Mapping[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        _fail("SUBMISSION_JOURNAL_EXISTS", str(path))
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(_canonical(value) + b"\n")
            stream.flush()
            os.fsync(stream.fileno())
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except Exception:
        try:
            path.unlink()
        except OSError:
            pass
        raise


def _atomic_replace(path: Path, value: Mapping[str, object]) -> None:
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=str(path.parent))
    temp_path = Path(temporary)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(_canonical(value) + b"\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_path, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            temp_path.unlink()
        except FileNotFoundError:
            pass


def deterministic_job_metadata(submission_id: str) -> dict[str, str]:
    if not isinstance(submission_id, str) or SUBMISSION_RE.fullmatch(submission_id) is None:
        _fail("SUBMISSION_ID_INVALID")
    suffix = submission_id.rsplit("-", 1)[1]
    return {"jobName": "spec110-" + suffix[:16], "comment": "spec110:" + submission_id}


def record_intent(
    *,
    journal_path: Path | str,
    submission_id: str,
    run_id: str,
    candidate_id: str,
    cell_id: str,
    script_path: Path | str,
    replaces_submission_id: str | None = None,
    replacement_authorization_digest: str | None = None,
) -> dict[str, Any]:
    path = Path(journal_path)
    if path.exists():
        _fail("SUBMISSION_JOURNAL_EXISTS", str(path))
    if SUBMISSION_RE.fullmatch(str(submission_id)) is None:
        _fail("SUBMISSION_ID_INVALID")
    if RUN_RE.fullmatch(str(run_id)) is None or CELL_RE.fullmatch(str(cell_id)) is None:
        _fail("SUBMISSION_RUN_OR_CELL_INVALID")
    if CANDIDATE_RE.fullmatch(str(candidate_id)) is None:
        _fail("SUBMISSION_CANDIDATE_INVALID")
    if replaces_submission_id is not None:
        if SUBMISSION_RE.fullmatch(str(replaces_submission_id)) is None:
            _fail("REPLACEMENT_LINK_INVALID")
        if (
            not isinstance(replacement_authorization_digest, str)
            or DIGEST_RE.fullmatch(replacement_authorization_digest) is None
        ):
            _fail("REPLACEMENT_AUTHORIZATION_REQUIRED")
    elif replacement_authorization_digest is not None:
        _fail("REPLACEMENT_LINK_REQUIRED")
    script = Path(script_path)
    if not script.is_file():
        _fail("SUBMISSION_SCRIPT_MISSING")
    metadata = deterministic_job_metadata(submission_id)
    record: dict[str, Any] = {
        "schemaVersion": "spec110-submission-journal-v1",
        "state": "INTENT_RECORDED",
        "submissionId": submission_id,
        "runId": run_id,
        "candidateId": candidate_id,
        "cellId": cell_id,
        "scriptPath": str(script.resolve()),
        "scriptSha256": _digest_bytes(script.read_bytes()),
        **metadata,
        "replacesSubmissionId": replaces_submission_id,
        "replacementAuthorizationDigest": replacement_authorization_digest,
        "jobId": None,
        "observedAt": _timestamp(),
        "priorStateDigest": None,
        "stdoutDigest": None,
        "stderrDigest": None,
        "history": [{"state": "INTENT_RECORDED", "observedAt": _timestamp()}],
    }
    _atomic_create(path, record)
    return record


def _transition(
    path: Path, record: Mapping[str, object], state: str, **updates: object
) -> dict[str, Any]:
    result = dict(record)
    result["priorStateDigest"] = _digest(record)
    result["state"] = state
    result.update(updates)
    result["observedAt"] = _timestamp()
    result["history"] = list(record.get("history", [])) + [
        {"state": state, "observedAt": result["observedAt"]}
    ]
    _atomic_replace(path, result)
    return result


def _default_runner(command: list[str]):
    return subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)


def submit_once(*, runner: Callable[[list[str]], object] = _default_runner, **intent: object) -> dict[str, Any]:
    path = Path(intent["journal_path"])
    record = record_intent(**intent)
    command = [
        "sbatch", "--parsable", "--job-name", record["jobName"],
        "--comment", record["comment"], record["scriptPath"],
    ]
    try:
        completed = runner(command)
    except BaseException as error:
        return _transition(
            path, record, "SUBMISSION_UNKNOWN",
            stderrDigest=_digest_bytes(str(error).encode()), stdoutDigest=_digest_bytes(b""),
        )
    stdout = str(getattr(completed, "stdout", "") or "")
    stderr = str(getattr(completed, "stderr", "") or "")
    updates = {
        "stdoutDigest": _digest_bytes(stdout.encode()),
        "stderrDigest": _digest_bytes(stderr.encode()),
    }
    if int(getattr(completed, "returncode", 1)) != 0:
        return _transition(path, record, "CONFIRMED_NOT_SUBMITTED", **updates)
    job_id = stdout.strip().split(";", 1)[0]
    if not job_id or not job_id.isdigit():
        return _transition(path, record, "SUBMISSION_UNKNOWN", **updates)
    return _transition(path, record, "SUBMITTED", jobId=job_id, **updates)


def reconcile_unknown(
    journal_path: Path | str,
    *,
    squeue_query: Callable[[str, str], Iterable[Mapping[str, object]]],
    sacct_query: Callable[[str, str], Iterable[Mapping[str, object]]],
) -> dict[str, Any]:
    path = Path(journal_path)
    record = json.loads(path.read_text(encoding="utf-8"))
    if record.get("state") != "SUBMISSION_UNKNOWN":
        _fail("SUBMISSION_RECONCILE_STATE_INVALID", str(record.get("state")))
    rows = list(squeue_query(record["jobName"], record["comment"]))
    rows.extend(sacct_query(record["jobName"], record["comment"]))
    job_ids = sorted({str(row.get("jobId")) for row in rows if str(row.get("jobId", "")).isdigit()})
    if len(job_ids) > 1:
        _fail("SUBMISSION_RECONCILE_AMBIGUOUS", ",".join(job_ids))
    if len(job_ids) == 1:
        return _transition(path, record, "SUBMITTED", jobId=job_ids[0])
    return _transition(path, record, "CONFIRMED_NOT_SUBMITTED")


__all__ = [
    "SubmissionError", "deterministic_job_metadata", "record_intent",
    "reconcile_unknown", "submit_once",
]
