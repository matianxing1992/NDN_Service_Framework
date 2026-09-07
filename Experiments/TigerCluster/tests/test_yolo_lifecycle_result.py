"""Lifecycle is lineage evidence, not proof of Provider execution."""
import ast
import json
import sys
from pathlib import Path

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result

ROOT = Path(__file__).resolve().parents[3]
D = 'sha256:' + '1' * 64


def journal():
    tree = ast.parse((ROOT / 'Experiments/NDNSF_DI_YoloAckDriven_Minindn.py').read_text())
    values = {}
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id in {'MILESTONES', '_LIFECYCLE_FIELD_ALLOWLIST'}:
                    values[target.id] = ast.literal_eval(node.value)
    rows = []
    for i, milestone in enumerate(values['MILESTONES']):
        row = dict(schema='spec180-yolo-lifecycle-event-v1', caseId='two-node',
                   requestId='/run/request/1', attemptId='attempt-1', sequence=i,
                   timestampUnix=100.0 + i, milestone=milestone)
        for field in values['_LIFECYCLE_FIELD_ALLOWLIST'][milestone]:
            row[field] = D if field.endswith('Digest') else 4
        rows.append(row)
    rows[4].update(candidateId='shared-backbone-two-shard-v1', candidatePriority=0)
    rows[-1].update(status=True, requestCount=1)
    return rows


def collect(tmp_path, rows):
    (tmp_path / 'lifecycle.jsonl').write_text('\n'.join(json.dumps(r) for r in rows) + '\n')
    return result.validate_lifecycle(tmp_path, case='two-node', request_id='/run/request/1',
        attempt_id='attempt-1', candidate_id='shared-backbone-two-shard-v1', candidate_digest=D)


def test_valid_lifecycle_is_only_lineage(tmp_path):
    evidence = collect(tmp_path, journal())
    assert evidence['qualification'] == 'LIFECYCLE_COMPONENT_ONLY'
    assert evidence['planDigest'] == D
    assert evidence['resultDigest'] == D


@pytest.mark.parametrize('index,key,value', [
    (0, 'sequence', False), (1, 'timestampUnix', True),
    (1, 'timestampUnix', float('nan')), (1, 'timestampUnix', float('inf')),
    (1, 'timestampUnix', 0), (2, 'ackCount', True), (2, 'ackCount', 0),
    (1, 'timestampUnix', 10 ** 400),
    (4, 'candidatePriority', -1), (4, 'candidateId', D),
    (4, 'candidateDigest', 'sha256:' + '2' * 64),
    (5, 'artifactDigest', 'missing'), (6, 'planDigest', ''),
    (7, 'selectedRoleCount', 0), (8, 'attemptId', 'attempt-2'),
    (8, 'requestId', '/run/request/other'), (9, 'requestCount', 2),
    (9, 'status', 1), (9, 'status', False), (0, 'caseId', 'other'),
    (4, 'payload', 'must-not-appear'),
])
def test_invalid_lineage_rejected(tmp_path, index, key, value):
    rows = journal()
    rows[index][key] = value
    with pytest.raises(result.EvidenceError):
        collect(tmp_path, rows)


def test_clock_adjustment_not_misreported_as_reordering(tmp_path):
    rows = journal()
    rows[5]['timestampUnix'] = 99.0
    collect(tmp_path, rows)


def test_missing_or_reordered_events_rejected(tmp_path):
    rows = journal()
    with pytest.raises(result.EvidenceError):
        collect(tmp_path, rows[:-1])
    rows[4], rows[5] = rows[5], rows[4]
    with pytest.raises(result.EvidenceError):
        collect(tmp_path, rows)


def test_duplicate_json_key_and_symlink_rejected(tmp_path):
    rows = journal()
    collect(tmp_path, rows)
    path = tmp_path / 'lifecycle.jsonl'
    data = path.read_text().replace('"sequence": 0', '"sequence": 0, "sequence": 0', 1)
    path.write_text(data)
    args = dict(case='two-node', request_id='/run/request/1', attempt_id='attempt-1',
                candidate_id='shared-backbone-two-shard-v1', candidate_digest=D)
    with pytest.raises(result.EvidenceError):
        result.validate_lifecycle(tmp_path, **args)
    path.rename(tmp_path / 'other.jsonl')
    path.symlink_to(tmp_path / 'other.jsonl')
    with pytest.raises(result.EvidenceError, match='SYMLINK'):
        result.validate_lifecycle(tmp_path, **args)
