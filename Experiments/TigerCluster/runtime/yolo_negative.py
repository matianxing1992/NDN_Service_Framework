"""Negative-case observations; no timeout, file or helper alone is a PASS."""
from __future__ import annotations

import hashlib
import math
from pathlib import Path
import re
import time

from .yolo_collection import _write_once
from .yolo_result import validate_lifecycle


class NegativeUserObserver:
    """Observe one real handle, then snapshot it after the owner's shutdown.

    The application main must return only after its finally/shutdown completes.
    Re-reading the public handle then catches a response that arrived between
    the initial wait failure and shutdown. Native failure/cutpoint/cleanup
    evidence is deliberately left for the independent retained collector.
    """
    def __init__(self, *, run_id, request_id, candidate_digest, output,
                 placement_id, placement_digest, deadline_ms):
        if (not isinstance(run_id,str) or not run_id
                or not isinstance(request_id,str) or not request_id.startswith('/')
                or request_id=='/' or not isinstance(placement_id,str) or not placement_id
                or any(not isinstance(v,str) or re.fullmatch(r'sha256:[a-f0-9]{64}',v) is None
                       for v in (candidate_digest,placement_digest))
                or type(deadline_ms) is not int or not 1501<=deadline_ms<=60000):
            raise ValueError('NEGATIVE_USER_EXPECTED_BINDING')
        self.run_id,self.request_id,self.candidate_digest=run_id,request_id,candidate_digest
        self.output=Path(output)
        self.placement_id,self.placement_digest=placement_id,placement_digest
        self.deadline_ms=deadline_ms
        self.dependency_no_progress_ms=deadline_ms//2
        self.handle=None
        self.finished=False

    def _selection(self):
        return validate_lifecycle(self.output,case='negative-dependency',request_id=self.request_id,
            attempt_id='attempt-1',candidate_id=self.placement_id,candidate_digest=self.placement_digest,
            require_terminal=False)

    def terminal(self, *, handle, journal, args, request_started_at):
        if (self.handle is not None or args.request_id!=self.request_id
                or args.lifecycle_case!='negative-dependency' or args.timeout_ms!=self.deadline_ms
                or Path(args.lifecycle_output_dir)!=self.output
                or journal.request_id!=self.request_id or journal.attempt_id!='attempt-1'
                or type(request_started_at) not in (int,float) or not math.isfinite(request_started_at)):
            raise ValueError('NEGATIVE_USER_HANDLE_BINDING')
        selection=self._selection()
        if handle.execution_plan_digest!=selection['planDigest']:
            raise ValueError('NEGATIVE_USER_PLAN_BINDING')
        self.started_at=request_started_at
        elapsed=(time.monotonic()-request_started_at)*1000
        remaining=self.deadline_ms-math.ceil(elapsed)-min(5000,self.deadline_ms//4)
        if elapsed<0 or remaining<=0:
            raise TimeoutError('NEGATIVE_USER_REQUEST_BUDGET')
        self.handle=handle
        self.selection=selection
        self.wait_error=''
        self.wait_response=None
        try:
            self.wait_response=handle.response(remaining)
        except Exception as exc:
            self.wait_error=type(exc).__name__
        # This return denotes completion of observation only. Qualification
        # requires the independent consumer/producer records after node cleanup.
        return 0

    def finish_after_shutdown(self, owner_exit_code):
        if self.handle is None or self.finished or type(owner_exit_code) is not int or owner_exit_code!=0:
            raise ValueError('NEGATIVE_USER_OWNER_NOT_COMPLETE')
        self.finished=True
        snapshot_error=''
        response=self.wait_response
        try:
            latest=self.handle.response(1)
            if latest is not None: response=latest
        except Exception as exc:
            snapshot_error=type(exc).__name__
        selection=self._selection()
        if selection!=self.selection:
            raise ValueError('NEGATIVE_USER_SELECTION_CHANGED')
        if response is None and (not self.wait_error or not snapshot_error):
            raise ValueError('NEGATIVE_USER_MISSING_WAIT_RESULT')
        if response is not None and (type(response.status) is not bool
                                     or not isinstance(response.payload,(bytes,bytearray))):
            raise ValueError('NEGATIVE_USER_RESPONSE_TYPE')
        elapsed=math.ceil((time.monotonic()-self.started_at)*1000)
        if not 1<=elapsed<=self.deadline_ms:
            raise TimeoutError('NEGATIVE_USER_DEADLINE')
        value=dict(schema='tiger-yolo-negative-user-v1',qualification='OBSERVATION_ONLY',
            runId=self.run_id,requestId=self.request_id,attempt=1,
            candidateDigest=self.candidate_digest,placementCandidateId=self.placement_id,
            placementCandidateDigest=self.placement_digest,planDigest=selection['planDigest'],
            deadlineMs=self.deadline_ms,dependencyNoProgressMs=self.dependency_no_progress_ms,
            elapsedMs=elapsed,observedAfterShutdown=True,waitErrorType=self.wait_error,
            snapshotErrorType=snapshot_error,response=dict(present=response is not None,
                success=False if response is None else response.status,
                bytes=0 if response is None else len(response.payload),
                sha256=None if response is None else 'sha256:'+hashlib.sha256(response.payload).hexdigest()))
        _write_once(self.output/'negative-user.json',value)
        return value
