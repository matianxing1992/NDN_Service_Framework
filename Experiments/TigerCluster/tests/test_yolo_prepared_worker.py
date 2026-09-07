"""Bound preparation verification at construction/start, with layout fixtures."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_worker import NodeRuntime, assigned_roles
from runtime.yolo_profile import ClosureError
from test_yolo_worker import prepared
from test_yolo_public_inventory import prepared_public, seal


def worker_inputs(tmp_path):
    kwargs = prepared(tmp_path, mode='local-cpu')
    plan = prepared_public(kwargs['public'])
    plan.update(case='local-cpu', output=str(tmp_path / 'run'),
                nodes=[{'rank': 0, 'roles': list(assigned_roles('local-cpu', 0))}])
    kwargs['output'] = Path(plan['output']) / 'node0'
    receipt = seal(kwargs['public'], plan)
    return kwargs, plan, receipt


def test_modified_public_inputs_prevent_worker_construction(tmp_path):
    kwargs, plan, receipt = worker_inputs(tmp_path)
    (kwargs['public'] / 'case.json').write_bytes(b'changed')
    with pytest.raises(ClosureError):
        NodeRuntime.from_preparation(plan, expected_receipt_digest=receipt,
                                     candidate_digest='sha256:' + 'a' * 64, **kwargs)
    assert not kwargs['output'].exists()


def test_recheck_before_child_launch_and_preserve_pinned_plan(tmp_path):
    kwargs, plan, receipt = worker_inputs(tmp_path)
    worker = NodeRuntime.from_preparation(plan, expected_receipt_digest=receipt,
                                         candidate_digest='sha256:' + 'a' * 64, **kwargs)
    try:
        plan['runId'] = 'caller-changed'
        worker._verify_prepared_boundary()  # The caller does not own the retained snapshot.
        (kwargs['public'] / 'case.json').write_bytes(b'changed')
        with pytest.raises(ClosureError):
            worker.start_forwarder(6363)
        assert worker.launches == [] and worker.leases == {}
    finally:
        worker.close()


def test_other_run_output_cannot_be_substituted(tmp_path):
    kwargs, plan, receipt = worker_inputs(tmp_path)
    kwargs['output'] = tmp_path / 'different-run'
    with pytest.raises(ValueError, match='WORKER_PREPARATION_RUN'):
        NodeRuntime.from_preparation(plan, expected_receipt_digest=receipt,
                                     candidate_digest='sha256:' + 'a' * 64, **kwargs)
    assert not kwargs['output'].exists()
