"""Durable allocate-once bookkeeping on a prevalidated shared filesystem.

The caller must use the SAME shared root from every operator, verify its locking
semantics on Tiger, and validate runtime gates before reserving. This module
never runs sbatch or establishes model qualification. A local test of flock is
not proof of the cluster filesystem's behavior.
"""
from __future__ import annotations

from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import time

from .yolo_profile import HASH, ClosureError, _read_plane


class JournalError(ValueError):
    pass


TERMINAL = {"PASS", "FAIL", "INCOMPLETE"}
CLOSED = TERMINAL | {"CANCELLED_BEFORE_SUBMIT"}
STATES = CLOSED | {"PREPARED", "SUBMITTING", "SUBMISSION_UNKNOWN", "SUBMITTED", "RUNNING"}


def verify_operator_python(bundle, *, seconds):
    """Verify the actual batch interpreter and frozen operator dependency pins."""
    script = '''import sys, pathlib, importlib.metadata as metadata
import jsonschema, numpy
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    line=line.split('#',1)[0].strip()
    if not line: continue
    parts=line.split(';')
    if len(parts)>2: raise ValueError('OPERATOR_REQUIREMENT_FORMAT')
    if len(parts)==2:
        if parts[1].strip()!='python_version < "3.9"': raise ValueError('OPERATOR_REQUIREMENT_MARKER')
        if sys.version_info[:2]>=(3,9): continue
    name,version=parts[0].strip().split('==')
    if metadata.version(name)!=version: raise ValueError('OPERATOR_REQUIREMENT_VERSION:'+name)
print('OPERATOR_REQUIREMENTS_OK')
'''
    result=subprocess.run(['/usr/bin/python3','-c',script,str(Path(bundle)/'requirements-operator.txt')],
        check=True,capture_output=True,timeout=seconds,env={'PATH':'/usr/bin:/bin','LC_ALL':'C'})
    if result.stdout!=b'OPERATOR_REQUIREMENTS_OK\n' or result.stderr:
        raise JournalError('OPERATOR_REQUIREMENTS_REJECTED')


def observe_submission(*, submission_key, partition, since, seconds):
    """Find an uncertain submission by its exact comment, never submit a retry."""
    import datetime
    import math
    if (not isinstance(submission_key,str) or re.fullmatch(r'spec183-[a-f0-9]{64}',submission_key) is None
            or not isinstance(partition,str) or re.fullmatch(r'[A-Za-z0-9_-]{1,64}',partition) is None
            or not isinstance(since,str) or re.fullmatch(r'\d{4}-\d{2}-\d{2}',since) is None
            or isinstance(seconds,bool) or not isinstance(seconds,(int,float))
            or not math.isfinite(seconds) or seconds<=0):
        raise JournalError('SUBMISSION_QUERY_ARGUMENTS')
    datetime.datetime.strptime(since,'%Y-%m-%d')
    uid=os.getuid()
    deadline=time.monotonic()+seconds
    commands=[['/usr/bin/sacct','-X','-n','-P','--duplicates','--user='+str(uid),
        '--starttime='+since,'--format=JobIDRaw,Comment%80,UID,Partition,Cluster'],
        ['/usr/bin/squeue','--noheader','--user='+str(uid),'--format=%i|%k|%U|%P']]
    outputs=[]
    matches=[]
    for index,command in enumerate(commands):
        remaining=deadline-time.monotonic()
        if remaining<=0:
            raise TimeoutError('SUBMISSION_QUERY_DEADLINE')
        result=subprocess.run(command,check=True,capture_output=True,timeout=remaining,
                              env={'PATH':'/usr/bin:/bin','LC_ALL':'C'})
        if len(result.stdout)>4*1024*1024 or result.stderr:
            raise JournalError('SUBMISSION_QUERY_OUTPUT')
        output=result.stdout.decode('ascii')
        outputs.append(output)
        for line in output.splitlines():
            if not line.strip(): continue
            fields=line.split('|')
            if len(fields)!=(5 if index==0 else 4):
                raise JournalError('SUBMISSION_QUERY_FORMAT')
            job,comment,user,part=fields[:4]
            if comment!=submission_key: continue
            if (re.fullmatch(r'[1-9][0-9]{0,9}',job) is None or user!=str(uid)
                    or part!=partition or (index==0 and fields[4]!='itiger')):
                raise JournalError('SUBMISSION_QUERY_BINDING')
            matches.append(dict(comment=comment,jobId=job))
    return dict(schema='tiger-yolo-submission-query-v1',submissionKey=submission_key,
                uid=uid,partition=partition,since=since,commands=commands,
                accounting=outputs[0],queue=outputs[1],jobs=matches)


class SubmissionJournal:
    def __init__(self, root: Path, *, candidate_id: str, gate: str):
        root = Path(root)
        if (not root.is_absolute() or ".." in root.parts or not root.is_dir()
                or any(p.is_symlink() for p in (root,) + tuple(root.parents))):
            raise JournalError("JOURNAL_ROOT")
        if not isinstance(candidate_id, str) or not HASH.fullmatch(candidate_id):
            raise JournalError("JOURNAL_CANDIDATE")
        if gate not in ("local-cpu", "single-node-gpu", "two-node-gpu", "negative-dependency"):
            raise JournalError("JOURNAL_GATE")
        self.root, self.candidate, self.gate = root, candidate_id, gate
        self.key = hashlib.sha256((candidate_id + ":" + gate).encode()).hexdigest()
        self.path = root / (self.key + ".json")

    @contextmanager
    def _locked(self):
        fd = os.open(str(self.root / (self.key + ".lock")),
                     os.O_CREAT | os.O_WRONLY | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise JournalError("JOURNAL_LOCK_TYPE")
            deadline = time.monotonic() + 2.0
            while True:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise JournalError("JOURNAL_LOCK_TIMEOUT")
                    time.sleep(0.02)
            yield
        finally:
            os.close(fd)

    def _load(self):
        if not os.path.lexists(str(self.path)):
            return {"schema": "tiger-yolo-submissions-v1", "candidateId": self.candidate,
                    "gate": self.gate, "activeRunId": None, "runs": {}}
        try:
            value = _read_plane(self.path)
        except ClosureError as exc:
            raise JournalError("JOURNAL_DOCUMENT") from exc
        if (not isinstance(value, dict)
                or set(value) != {"schema", "candidateId", "gate", "activeRunId", "runs"}
                or value["schema"] != "tiger-yolo-submissions-v1"
                or value["candidateId"] != self.candidate or value["gate"] != self.gate
                or not isinstance(value["runs"], dict)):
            raise JournalError("JOURNAL_RECORD")
        active = []
        for name, row in value["runs"].items():
            self._run_id(name)
            if (not isinstance(row, dict) or set(row) != {"runId", "state", "jobId", "submissionKey"}
                    or row["runId"] != name or not isinstance(row["state"], str) or row["state"] not in STATES
                    or row["submissionKey"] != self._submission_key(name)):
                raise JournalError("JOURNAL_RECORD")
            needs_job = row["state"] in TERMINAL | {"SUBMITTED", "RUNNING"}
            if needs_job != (row["jobId"] is not None):
                raise JournalError("JOURNAL_RECORD_JOB_STATE")
            if row["jobId"] is not None:
                self._job_id(row["jobId"])
            if row["state"] not in CLOSED:
                active.append(name)
        if active != ([] if value["activeRunId"] is None else [value["activeRunId"]]):
            raise JournalError("JOURNAL_ACTIVE_RECORD")
        return value

    def _save(self, value):
        data = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
        if len(data) > 4 * 1024 * 1024:
            raise JournalError("JOURNAL_CAPACITY")
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=str(self.root), prefix=self.key + ".tmp-", delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(str(temporary), str(self.path))
            temporary = None
            fd = os.open(str(self.root), os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        finally:
            if temporary is not None:
                temporary.unlink()

    @staticmethod
    def _run_id(run_id):
        if not isinstance(run_id, str) or not re.fullmatch(r"[a-z][a-z0-9-]{1,47}", run_id):
            raise JournalError("JOURNAL_RUN_ID")

    @staticmethod
    def _job_id(job_id):
        if not isinstance(job_id, str) or not re.fullmatch(r"[1-9][0-9]{0,19}", job_id):
            raise JournalError("JOURNAL_JOB_ID")

    def _submission_key(self, run_id):
        return "spec183-" + hashlib.sha256((self.key + ":" + run_id).encode()).hexdigest()

    def reserve(self, run_id: str) -> dict:
        self._run_id(run_id)
        with self._locked():
            value = self._load()
            if run_id in value["runs"]:
                raise JournalError("RUN_ALREADY_REGISTERED")
            if value["activeRunId"] is not None:
                raise JournalError("ACTIVE_RUN:" + value["activeRunId"])
            row = {"runId": run_id, "state": "PREPARED", "jobId": None,
                   "submissionKey": self._submission_key(run_id)}
            value["runs"][run_id] = row
            value["activeRunId"] = run_id
            self._save(value)
            return dict(row)

    def get(self, run_id: str) -> dict:
        self._run_id(run_id)
        with self._locked():
            value = self._load()
            if run_id not in value["runs"]:
                raise JournalError("RUN_NOT_REGISTERED")
            return dict(value["runs"][run_id])

    def _change(self, run_id, *, expected, state, job_id=None):
        self._run_id(run_id)
        if job_id is not None:
            self._job_id(job_id)
        with self._locked():
            value = self._load()
            row = value["runs"].get(run_id)
            if row is None or row["state"] not in expected or value["activeRunId"] != run_id:
                raise JournalError("INVALID_TRANSITION")
            if job_id is not None:
                if row["jobId"] is not None and row["jobId"] != job_id:
                    raise JournalError("JOURNAL_JOB_MISMATCH")
                row["jobId"] = job_id
            row["state"] = state
            if state in CLOSED:
                value["activeRunId"] = None
            self._save(value)
            return dict(row)

    def mark_submitting(self, run_id):
        """Must be durably recorded BEFORE the sole sbatch call."""
        return self._change(run_id, expected={"PREPARED"}, state="SUBMITTING")

    def cancel_prepared(self, run_id):
        """Only safe before the durable SUBMITTING boundary; never a retry."""
        return self._change(run_id, expected={"PREPARED"}, state="CANCELLED_BEFORE_SUBMIT")

    def mark_unknown(self, run_id):
        return self._change(run_id, expected={"SUBMITTING", "SUBMISSION_UNKNOWN"},
                            state="SUBMISSION_UNKNOWN")

    def record_submission(self, run_id, job_id):
        """Record a validated sbatch response, not a guess after timeout."""
        self._job_id(job_id)
        return self._change(run_id, expected={"SUBMITTING", "SUBMISSION_UNKNOWN"}, state="SUBMITTED", job_id=job_id)

    def mark_running(self, run_id, job_id):
        self._job_id(job_id)
        return self._change(run_id, expected={"SUBMITTED"}, state="RUNNING", job_id=job_id)

    def finish(self, run_id, job_id, status):
        """Caller must establish allocation termination and collector verdict.

        This releases bookkeeping, not a model qualification receipt. Terminal
        rows cannot be rewritten and unacknowledged submissions cannot finish.
        """
        if not isinstance(status, str) or status not in TERMINAL:
            raise JournalError("JOURNAL_TERMINAL_STATUS")
        self._job_id(job_id)
        return self._change(run_id, expected={"SUBMITTED", "RUNNING"},
                            state=status, job_id=job_id)

    def reconcile(self, run_id, jobs):
        """Consume exact Slurm comment/job matches supplied by the query owner.

        Empty query results are not proof of non-submission: remain UNKNOWN.
        Neither transport recovery nor this API ever invokes sbatch.
        """
        self._run_id(run_id)
        if not isinstance(jobs, list):
            raise JournalError("JOURNAL_QUERY")
        matches = set()
        for job in jobs:
            if (not isinstance(job, dict) or set(job) != {"comment", "jobId"}
                    or not isinstance(job["comment"], str)):
                raise JournalError("JOURNAL_QUERY")
            self._job_id(job["jobId"])
            if job["comment"] == self._submission_key(run_id):
                matches.add(job["jobId"])
        if len(matches) != 1:
            result = self.mark_unknown(run_id)
            if matches:
                raise JournalError("DUPLICATE_SUBMISSION:" + ",".join(sorted(matches)))
            return result
        return self._change(run_id, expected={"SUBMITTING", "SUBMISSION_UNKNOWN"},
                            state="SUBMITTED", job_id=next(iter(matches)))
