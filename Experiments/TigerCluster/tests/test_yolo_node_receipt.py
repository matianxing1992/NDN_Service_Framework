"""Receipt contract fixtures; do not represent a native deployment."""
from pathlib import Path
from types import SimpleNamespace as NS
import sys

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result
from runtime.yolo_worker import assigned_roles


def worker(tmp_path):
    roles = assigned_roles('local-cpu', 0)
    names = [r for r in roles if r != 'user']
    launches = [dict(role=r, pid=100+i, argv=['fixture', r]) for i,r in enumerate(names)]
    launches += [dict(role='user', invocation=str(i), pid=200+i, argv=['fixture', str(i)]) for i in range(2)]
    rows = [dict(name=x['role'] + ('-'+x['invocation'] if 'invocation' in x else ''), pid=x['pid'],
        kind='finite' if 'invocation' in x else 'service', exitedBeforeCleanup=False,
        forced=False, reaped=True, leaseReleased=True, exitCode=0) for x in launches]
    plan = dict(runId='test-run', case='local-cpu', output=str(tmp_path),
                requests=[dict(index=i, requestId='/request/'+str(i)) for i in range(2)])
    output = tmp_path / 'node0'
    output.mkdir()
    state = NS(closed=True, leases={}, children=NS(children=[]), finite_children=NS(children=[]),
        launches=launches, roles=roles, rank=0, mode='local-cpu', output=output,
        _preparation_binding=(plan, 'sha256:'+'1'*64, 'sha256:'+'2'*64),
        _verify_prepared_boundary=lambda: None)
    return state, rows


def test_receipt_is_exclusive_and_not_inference_pass(tmp_path):
    state, rows = worker(tmp_path)
    receipt = result.write_worker_receipt(state, rows)
    assert receipt['qualification'] == 'NODE_CLEANUP_COMPONENT_ONLY'
    assert receipt['runId'] == 'test-run'
    assert (state.output / 'node-receipt.json').is_file()
    with pytest.raises((ValueError, OSError)):
        result.write_worker_receipt(state, rows)


@pytest.mark.parametrize('fault', ['unprepared', 'changed-preparation', 'missing-service',
    'missing-request', 'duplicate-request', 'wrong-case', 'wrong-output', 'failed-cleanup'])
def test_partial_or_unbound_worker_never_gets_receipt(tmp_path, fault):
    state, rows = worker(tmp_path)
    if fault == 'unprepared': state._preparation_binding = None
    if fault == 'changed-preparation':
        def reject(): raise ValueError('changed preparation')
        state._verify_prepared_boundary = reject
    if fault == 'missing-service':
        state.launches.pop(0)
        rows.pop(0)
    if fault == 'missing-request':
        state.launches.pop()
        rows.pop()
    if fault == 'duplicate-request':
        state._preparation_binding[0]['requests'][1]['requestId'] = '/request/0'
    if fault == 'wrong-case': state._preparation_binding[0]['case'] = 'other'
    if fault == 'wrong-output': state._preparation_binding[0]['output'] = str(tmp_path / 'other')
    if fault == 'failed-cleanup': rows[0]['forced'] = True
    with pytest.raises(ValueError):
        result.write_worker_receipt(state, rows)
    assert not (state.output / 'node-receipt.json').exists()
