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
    for launch in launches:
        if launch['role'] in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'):
            launch['launchNonce'] = format(launch['pid'], '064x')
    rows = [dict(name=x['role'] + ('-'+x['invocation'] if 'invocation' in x else ''), pid=x['pid'],
        kind='finite' if 'invocation' in x else 'service', exitedBeforeCleanup=False,
        forced=False, reaped=True, leaseReleased=True, exitCode=0) for x in launches]
    plan = dict(runId='test-run', case='local-cpu', output=str(tmp_path),
                requests=[dict(index=i, requestId='/request/'+str(i)) for i in range(2)])
    output = tmp_path / 'node0'
    output.mkdir()
    (output/'logs').mkdir()
    for item in rows:
        (output/'logs'/(item['name']+'.log')).write_text('fixture log '+item['name'])
    import json
    for launch in launches:
        if launch.get('launchNonce'):
            (output/'logs'/(launch['role']+'.log')).write_text('TIGER_PROVIDER_PROCESS_STARTED '+json.dumps(
                dict(schema='tiger-provider-process-v1', nonce=launch['launchNonce'],
                     role=launch['role'], pid=launch['pid']))+'\n')
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
    assert receipt['schema'] == 'tiger-yolo-node-receipt-v3'
    import hashlib
    for launch in receipt['launches']:
        content = (state.output/launch['logPath']).read_bytes()
        assert launch['logDigest'] == 'sha256:'+hashlib.sha256(content).hexdigest()
        assert launch['logBytes'] == len(content)
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


@pytest.mark.parametrize('fault', ['missing', 'symlink', 'directory'])
def test_node_receipt_requires_actual_regular_logs(tmp_path, fault):
    state, rows = worker(tmp_path)
    target = state.output/'logs'/(rows[0]['name']+'.log')
    target.unlink()
    if fault == 'symlink': target.symlink_to(state.output/'logs'/(rows[1]['name']+'.log'))
    if fault == 'directory': target.mkdir()
    with pytest.raises(ValueError): result.write_worker_receipt(state, rows)
    assert not (state.output/'node-receipt.json').exists()


@pytest.mark.parametrize('fault', ['none', 'receipt-hash', 'changed-log', 'log-symlink',
    'receipt-symlink', 'plan', 'preparation', 'rank'])
def test_retained_receipt_binds_logs_without_recreating_worker(tmp_path, fault):
    import hashlib
    state, rows = worker(tmp_path)
    receipt = result.write_worker_receipt(state, rows)
    path = state.output/'node-receipt.json'
    digest = 'sha256:'+hashlib.sha256(path.read_bytes()).hexdigest()
    plan, prep, candidate = state._preparation_binding
    if fault == 'receipt-hash': digest = 'sha256:'+'0'*64
    if fault == 'plan': plan = dict(plan, runId='other')
    if fault == 'preparation': prep = 'sha256:'+'0'*64
    log = state.output/receipt['launches'][0]['logPath']
    if fault == 'changed-log': log.write_text('changed')
    if fault == 'log-symlink':
        log.unlink()
        log.symlink_to(state.output/receipt['launches'][1]['logPath'])
    if fault == 'receipt-symlink':
        target = state.output/'original.json'
        path.rename(target)
        path.symlink_to(target)
    def read():
        return result.read_node_log_receipt(state.output, receipt_digest=digest, plan=plan,
            preparation_digest=prep, candidate_digest=candidate, rank=1 if fault == 'rank' else 0)
    if fault == 'none':
        evidence = read()
        assert len(evidence['logs']) == len(state.launches)
        assert evidence['qualification'] == 'NODE_LOG_COMPONENT_ONLY'
    else:
        with pytest.raises(ValueError): read()


@pytest.mark.parametrize('fault', ['forced', 'missing-cleanup', 'duplicate-cleanup',
    'finite-exit', 'false-summary', 'missing-request', 'duplicate-plan-request'])
def test_reader_rechecks_receipt_semantics_not_only_hash(tmp_path, fault):
    import hashlib
    import json
    state, rows = worker(tmp_path)
    receipt = result.write_worker_receipt(state, rows)
    plan, prep, candidate = state._preparation_binding
    if fault == 'forced': receipt['cleanup'][0]['forced'] = True
    if fault == 'missing-cleanup': receipt['cleanup'].pop()
    if fault == 'duplicate-cleanup': receipt['cleanup'][-1] = dict(receipt['cleanup'][0])
    if fault == 'finite-exit': receipt['cleanup'][-1]['exitCode'] = 1
    if fault == 'false-summary': receipt['cleanupSummary']['childCount'] = 999
    if fault == 'missing-request':
        receipt['launches'].pop()
        receipt['cleanup'].pop()
        receipt['cleanupSummary']['childCount'] -= 1
    if fault == 'duplicate-plan-request':
        plan['requests'][1]['requestId'] = plan['requests'][0]['requestId']
        receipt['planDigest'] = 'sha256:'+hashlib.sha256(json.dumps(plan, sort_keys=True,
            separators=(',', ':'), allow_nan=False).encode()).hexdigest()
    path = state.output/'node-receipt.json'
    path.write_text(json.dumps(receipt))
    digest = 'sha256:'+hashlib.sha256(path.read_bytes()).hexdigest()
    # A matching staging hash is necessary, not a substitute for semantics.
    with pytest.raises(ValueError):
        result.read_node_log_receipt(state.output, receipt_digest=digest, plan=plan,
            preparation_digest=prep, candidate_digest=candidate, rank=0)
