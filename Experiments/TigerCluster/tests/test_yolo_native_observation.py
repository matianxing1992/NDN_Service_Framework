"""Actual PropertyTree scalar representation; no native execution claim."""
import json
from pathlib import Path
import sys

import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import yolo_result as result


def observation():
    return dict(schema='ndnsf-di-execution-evidence-v1', providerName='/app/worker-a',
        roles=['BackboneNeck'], requestId='/app/request/1', planDigest='sha256:'+'1'*64,
        processId='123', attemptEpoch='1', evidenceEpoch='1', createdAtMs='100',
        profileAttemptEpoch='1', realCompute='true', cpuFallbackUsed='false',
        loadCompleted='true', warmupCompleted='true', executionCompleted='true',
        exactForwardCacheHit='false', runnerKind='onnxruntime-cuda', nodeProviderAssignments=[dict(role='BackboneNeck',
            nodeName='conv', provider='CUDAExecutionProvider', modelNode='true')])


def test_property_tree_scalars_are_decoded_without_truthiness():
    row = result.decode_native_observation(json.dumps(observation()))
    assert row['processId'] == 123 and type(row['processId']) is int
    assert row['executionCompleted'] is True
    assert row['cpuFallbackUsed'] is False
    assert row['nodeProviderAssignments'][0]['modelNode'] is True


@pytest.mark.parametrize('field,value', [
    ('processId', True), ('processId', '01'), ('processId', '-1'),
    ('processId', '1.0'), ('processId', ' 1'), ('processId', 1.0),
    ('processId', str(2**64)), ('processId', None),
    ('executionCompleted', 'True'), ('executionCompleted', 1),
    ('executionCompleted', 'yes'), ('executionCompleted', None),
    ('roles', {}), ('roles', [1]), ('nodeProviderAssignments', [True]),
])
def test_ambiguous_native_fields_rejected(field, value):
    row = observation()
    row[field] = value
    with pytest.raises(result.EvidenceError):
        result.decode_native_observation(json.dumps(row))


def test_empty_property_tree_array_and_typed_scalars():
    raw = observation()
    raw['nodeProviderAssignments'] = ''
    raw['processId'] = 123
    raw['cpuFallbackUsed'] = False
    parsed = result.decode_native_observation(json.dumps(raw))
    assert parsed['nodeProviderAssignments'] == []
    assert parsed['cpuFallbackUsed'] is False


def test_duplicate_nonfinite_and_oversized_payload_rejected():
    data = json.dumps(observation())
    for payload in [data.replace('"processId": "123"', '"processId": "123", "processId": "123"'),
                    data.replace('"processId": "123"', '"processId": NaN'),
                    ' ' * (1024*1024 + 1)]:
        with pytest.raises(result.EvidenceError):
            result.decode_native_observation(payload)


def validate(row):
    return result.validate_native_observation(json.dumps(row), provider='/app/worker-a',
        role='BackboneNeck', request_id='/app/request/1', attempt=1,
        plan_digest='sha256:'+'1'*64, pid=123, runner_kind='onnxruntime-cuda')


def test_real_provider_name_not_hardcoded_to_example_prefix():
    assert validate(observation())['qualification'] == 'NATIVE_OBSERVATION_COMPONENT_ONLY'


@pytest.mark.parametrize('key,value', [('providerName', '/example/provider/BackboneNeck'),
    ('attemptEpoch', '2'), ('processId', '124'), ('requestId', '/other'),
    ('executionCompleted', 'false'), ('exactForwardCacheHit', 'true'),
    ('cpuFallbackUsed', 'true'), ('runnerKind', 'onnxruntime-cpu'),
    ('warmupCompleted', 'false'), ('nodeProviderAssignments', '')])
def test_failed_or_unrelated_execution_cannot_pass(key, value):
    row = observation()
    row[key] = value
    with pytest.raises(result.EvidenceError):
        validate(row)
