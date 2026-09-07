"""Independently read ORT events; synthetic profiles are not GPU evidence."""
import json
from pathlib import Path
import sys

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result


def fixture():
    events = [dict(cat='Session', name='model_run'), dict(cat='Node', name='Conv_kernel_time',
        args={'provider': 'CUDAExecutionProvider'})]
    record = dict(runnerKind='onnxruntime-cuda', roles=['BackboneNeck'], requestId='/request/1',
        attemptEpoch=1, profileRequestId='/request/1', profileAttemptEpoch=1,
        nodeProviderAssignments=[dict(role='BackboneNeck', nodeName='Conv_kernel_time',
            provider='CUDAExecutionProvider', modelNode=True)])
    return events, record


def test_profile_assignments_recomputed_from_bytes(tmp_path):
    events, record = fixture()
    path = tmp_path / 'ort.json'
    path.write_text(json.dumps(events))
    evidence = result.validate_ort_profile(path, record)
    assert evidence['modelNodeEvents'] == 1
    assert evidence['qualification'] == 'ORT_PROFILE_COMPONENT_ONLY'


@pytest.mark.parametrize('fault', ['old-request', 'old-attempt', 'cpu-fallback', 'missing-provider',
    'missing-node', 'extra-node', 'wrong-name', 'empty', 'bad-json', 'symlink'])
def test_inconsistent_profile_rejected(tmp_path, fault):
    events, record = fixture()
    if fault == 'old-request': record['profileRequestId'] = '/request/old'
    if fault == 'old-attempt': record['profileAttemptEpoch'] = 2
    if fault == 'cpu-fallback': events[1]['args']['provider'] = 'CPUExecutionProvider'
    if fault == 'missing-provider': events[1]['args'] = {}
    if fault == 'missing-node': events.pop()
    if fault == 'extra-node': events.append(events[1])
    if fault == 'wrong-name': events[1]['name'] = 'Other_kernel_time'
    if fault == 'empty': events = []
    path = tmp_path / 'ort.json'
    path.write_text('invalid' if fault == 'bad-json' else json.dumps(events))
    if fault == 'symlink':
        path.rename(tmp_path / 'real.json')
        path.symlink_to(tmp_path / 'real.json')
    with pytest.raises(result.EvidenceError):
        result.validate_ort_profile(path, record)


def test_cpu_profile_and_non_kernel_fence(tmp_path):
    events, record = fixture()
    record['runnerKind'] = 'onnxruntime-cpu'
    record['nodeProviderAssignments'][0]['provider'] = 'CPUExecutionProvider'
    events[1]['args']['provider'] = 'CPUExecutionProvider'
    events.insert(1, dict(cat='Node', name='Conv_fence_before', args={}))
    path = tmp_path / 'ort.json'
    path.write_text(json.dumps(events))
    assert result.validate_ort_profile(path, record)['modelNodeEvents'] == 1
