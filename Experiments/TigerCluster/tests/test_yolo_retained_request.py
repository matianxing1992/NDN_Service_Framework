"""Real lifecycle reader; numerical and native boundaries are explicit doubles."""
import hashlib
import json
from pathlib import Path
import sys

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result
from test_yolo_lifecycle_result import journal, D


@pytest.mark.parametrize('fault', ['none', 'role-digest', 'selection-digest', 'ack-count',
    'provider-count', 'role-count', 'index', 'attempt', 'numerical'])
def test_request_join_keeps_release_and_placement_identities_distinct(tmp_path, monkeypatch, fault):
    providers = {role: '/app/'+role for role in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')}
    def digest(value):
        return 'sha256:'+hashlib.sha256(json.dumps(value, sort_keys=True,
            separators=(',', ':'), ensure_ascii=False).encode()).hexdigest()
    rows = journal()
    for row in rows:
        row['caseId'] = 'local-cpu'
        if fault == 'attempt': row['attemptId'] = 'attempt-2'
    rows[8]['roleDigest'] = digest(providers)
    rows[7]['selectionDigest'] = digest({'plan': D, 'ack': D})
    if fault == 'role-digest': rows[8]['roleDigest'] = D
    if fault == 'selection-digest': rows[7]['selectionDigest'] = D
    if fault == 'ack-count': rows[2]['ackCount'] = 1
    if fault == 'provider-count': rows[4]['providerCount'] = 3
    if fault == 'role-count': rows[7]['selectedRoleCount'] = 3
    root = tmp_path/'node0/user/requests/0'
    root.mkdir(parents=True)
    (root/'lifecycle.jsonl').write_text('\n'.join(json.dumps(r) for r in rows)+'\n')
    calls = []
    release = 'sha256:'+'2'*64
    def numerical(path, reference, **binding):
        assert path == root and reference == 'reference'
        assert binding['candidate_digest'] == D and binding['plan_digest'] == D
        if fault == 'numerical': raise ValueError('numeric mismatch')
        return {'matched': True}
    monkeypatch.setattr(result, 'reanalyze_numerical_response', numerical)
    def execution(nodes, path, **binding):
        calls.append(binding)
        assert path == root/'yolo-public-assignments.json'
        assert binding['candidate_digest'] == release != D
        assert binding['execution_plan_digest'] == D and binding['attempt'] == 1
        assert binding['providers_by_role'] == providers
        return {'qualification': 'fixture'}
    monkeypatch.setattr(result, 'collect_retained_dependencies', execution)
    def collect():
        return result.collect_retained_request({0: {'root': tmp_path/'node0'}}, 'reference',
            plan={'case': 'local-cpu', 'requests': [{'index': 0, 'requestId': '/run/request/1'}]},
            request_index=1 if fault == 'index' else 0, runtime_candidate_digest=release,
            placement_candidate_id='shared-backbone-two-shard-v1', placement_candidate_digest=D,
            graph_digest=D, catalogue_digest=D, providers_by_role=providers)
    if fault == 'none':
        assert collect()['qualification'] == 'RETAINED_REQUEST_COMPONENT_ONLY'
        assert len(calls) == 1
    else:
        with pytest.raises(ValueError): collect()
        assert not calls
