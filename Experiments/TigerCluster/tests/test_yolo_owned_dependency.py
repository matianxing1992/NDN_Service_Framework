"""Owner-join tests; role/native/ORT and dependency boundaries are doubles."""
from pathlib import Path
import sys
from types import SimpleNamespace as NS

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result


@pytest.mark.parametrize('fault', ['none', 'unclosed', 'duplicate-rank', 'duplicate-role',
    'missing-role', 'wrong-mode', 'mixed-mode', 'changed-log', 'preparation'])
def test_owned_dependency_join_uses_all_actual_role_paths(tmp_path, monkeypatch, fault):
    providers = {role: '/app/'+role for role in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')}
    checked, observed = [], {}
    def verify(rank):
        if fault == 'preparation': raise ValueError('changed prepared input')
        checked.append(rank)
    workers = [NS(closed=True, rank=rank, mode='two-node-gpu', roles=roles,
        children=NS(log_dir=tmp_path/str(rank)),
        _verify_prepared_boundary=lambda rank=rank: verify(rank))
        for rank, roles in ((0, ['BackboneNeck', 'Merge']), (1, ['DetectShard0', 'DetectShard1']))]
    if fault == 'unclosed': workers[1].closed = False
    if fault == 'duplicate-rank': workers[1].rank = 0
    if fault == 'duplicate-role': workers[1].roles.append('Merge')
    if fault == 'missing-role': workers[1].roles.pop()
    if fault == 'wrong-mode': workers[1].mode = 'unknown'
    if fault == 'mixed-mode': workers[1].mode = 'local-cpu'
    def role(worker, **binding):
        assert binding['provider'] == providers[binding['role']]
        assert binding['request_id'] == 'request' and binding['attempt'] == 2
        assert binding['plan_digest'] == 'sha256:'+'a'*64
        observed[binding['role']] = worker.children.log_dir/(binding['role']+'.log')
        return {'native': {'logDigest': 'sha256:'+'b'*64}}
    monkeypatch.setattr(result, 'collect_role_execution', role)
    def dependencies(path, logs, **binding):
        assert path == tmp_path/'public.json'
        assert logs == observed and set(logs) == set(providers)
        return {'logDigests': {name: 'sha256:'+('c' if fault == 'changed-log' else 'b')*64 for name in logs}}
    monkeypatch.setattr(result, 'collect_dependency_result', dependencies)
    def run():
        return result.collect_owned_dependency_result(workers, tmp_path/'public.json',
            request_id='request', attempt=2, plan_digest='sha256:'+'a'*64, providers_by_role=providers)
    if fault == 'none':
        evidence = run()
        assert checked == [0, 1] and set(evidence['roles']) == set(providers)
        assert evidence['qualification'] == 'OWNED_DEPENDENCY_COMPONENT_ONLY'
    else:
        with pytest.raises(ValueError): run()
