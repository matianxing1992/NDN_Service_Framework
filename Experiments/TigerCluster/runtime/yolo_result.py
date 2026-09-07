"""Shared YOLO evidence checks. Publication readiness is NOT inference PASS.

Numerical reanalysis is a component only. Full lifecycle/role/edge/cleanup
collection is still pending in T006.
"""
from collections.abc import Mapping
import re


def decode_native_observation(payload):
    """Decode ExecutionEvidence.cpp JSON, NOT validate execution success.

    Boost PropertyTree writes scalar booleans/uint64 as strings and an empty
    array as "". Accept only canonical values at the specific native fields;
    never recursively coerce arbitrary strings or treat bool('false') as true.
    Typed JSON from other maintained producers is also accepted strictly.
    """
    import json
    if not isinstance(payload, str) or len(payload.encode('utf-8')) > 1024 * 1024:
        raise EvidenceError('NATIVE_OBSERVATION_SIZE')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise EvidenceError('NATIVE_OBSERVATION_DUPLICATE_KEY')
            result[key] = value
        return result
    def constant(value):
        raise EvidenceError('NATIVE_OBSERVATION_NONFINITE')
    try:
        row = json.loads(payload, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, RecursionError) as exc:
        raise EvidenceError('NATIVE_OBSERVATION_JSON') from exc
    if not isinstance(row, dict) or row.get('schema') != 'ndnsf-di-execution-evidence-v1':
        raise EvidenceError('NATIVE_OBSERVATION_SCHEMA')
    def boolean(value):
        if type(value) is bool:
            return value
        if type(value) is str and value in ('true', 'false'):
            return value == 'true'
        raise EvidenceError('NATIVE_OBSERVATION_BOOL')
    def unsigned(value):
        if type(value) is str and re.fullmatch(r'0|[1-9][0-9]{0,19}', value):
            value = int(value)
        if type(value) is not int or not 0 <= value < 2**64:
            raise EvidenceError('NATIVE_OBSERVATION_UINT64')
        return value
    for key in ('realCompute', 'cpuFallbackUsed', 'loadCompleted', 'warmupCompleted',
                'executionCompleted', 'exactForwardCacheHit'):
        row[key] = boolean(row.get(key))
    for key in ('processId', 'attemptEpoch', 'evidenceEpoch', 'createdAtMs', 'profileAttemptEpoch'):
        row[key] = unsigned(row.get(key))
    for key in ('roles', 'nodeProviderAssignments'):
        if row.get(key) == '':
            row[key] = []
        if not isinstance(row.get(key), list) or len(row[key]) > 10000:
            raise EvidenceError('NATIVE_OBSERVATION_ARRAY')
    if any(not isinstance(role, str) or not role for role in row['roles']):
        raise EvidenceError('NATIVE_OBSERVATION_ROLE')
    for assignment in row['nodeProviderAssignments']:
        if not isinstance(assignment, dict):
            raise EvidenceError('NATIVE_OBSERVATION_ASSIGNMENT')
        assignment['modelNode'] = boolean(assignment.get('modelNode'))
    return row


def validate_native_observation(payload, *, provider, role, request_id, attempt,
                                plan_digest, pid, runner_kind):
    """Bind a decoded observation to launcher and lifecycle facts.

    Does not qualify the referenced ORT profile, GPU allocation, dependency
    transfers or process cleanup. Those checks must precede a full PASS.
    """
    if (type(pid) is not int or pid <= 0 or type(attempt) is not int or attempt <= 0
            or any(not isinstance(v, str) or not v for v in (provider, role, request_id))
            or not isinstance(plan_digest, str)
            or not re.fullmatch(r'sha256:[0-9a-f]{64}', plan_digest)
            or runner_kind not in ('onnxruntime-cpu', 'onnxruntime-cuda', 'native-yolo-postprocess')):
        raise EvidenceError('NATIVE_EXPECTED_BINDING')
    row = decode_native_observation(payload)
    expected = dict(providerName=provider, roles=[role], requestId=request_id,
        attemptEpoch=attempt, planDigest=plan_digest, processId=pid, runnerKind=runner_kind,
        executionCompleted=True, exactForwardCacheHit=False, loadCompleted=True,
        warmupCompleted=True, cpuFallbackUsed=False,
        realCompute=runner_kind != 'native-yolo-postprocess')
    if any(row.get(k) != v for k,v in expected.items()):
        raise EvidenceError('NATIVE_EXECUTION_BINDING_OR_STATUS')
    assignments = row['nodeProviderAssignments']
    if runner_kind == 'native-yolo-postprocess':
        if assignments or row.get('gpuUuid') != '' or row.get('cudaVisibleDevices') != '':
            raise EvidenceError('NATIVE_MERGE_DEVICE')
    else:
        backend = 'CUDAExecutionProvider' if runner_kind == 'onnxruntime-cuda' else 'CPUExecutionProvider'
        if not assignments or any(a.get('role') != role or a.get('provider') != backend
                or a.get('modelNode') is not True or not isinstance(a.get('nodeName'), str)
                or not a['nodeName'] for a in assignments):
            raise EvidenceError('NATIVE_MODEL_NODE_ASSIGNMENT')
    return dict(observation=row, qualification='NATIVE_OBSERVATION_COMPONENT_ONLY')


def validate_lifecycle(root, *, case, request_id, attempt_id, candidate_id, candidate_digest):
    """Validate one externally bound, successful, no-reselection request.

    These User journal entries do not prove native execution, model loading,
    edge delivery or cleanup. Those independent components remain required.
    """
    import json
    import math
    from pathlib import Path
    from runtime.yolo_bundle import _bytes
    fields = {
        'INPUT_REFERENCE_PUBLISHED': {'referenceDigest'},
        'REQUEST_SENT': {'requestDigest'},
        'ACK_CLOSED': {'ackSnapshotDigest', 'ackCount'},
        'GRAPH_READY': {'graphDigest', 'catalogueDigest'},
        'PLACEMENT_DECISION': {'candidateId', 'candidateDigest', 'candidatePriority', 'providerCount'},
        'ARTIFACTS_READY': {'artifactDigest', 'artifactCount'},
        'PLAN_SEALED': {'planDigest'},
        'SELECTION_COMMITTED': {'selectionDigest', 'selectedRoleCount'},
        'PROVIDER_EXECUTION_STARTED': {'roleDigest', 'providerCount'},
        'TERMINAL_RESPONSE': {'resultDigest', 'requestCount', 'status'},
    }
    common = {'schema', 'caseId', 'requestId', 'attemptId', 'sequence', 'timestampUnix', 'milestone'}
    if (any(not isinstance(v, str) or not v for v in
            (case, request_id, attempt_id, candidate_id))
            or not isinstance(candidate_digest, str)
            or not re.fullmatch(r'sha256:[0-9a-f]{64}', candidate_digest)):
        raise EvidenceError('LIFECYCLE_EXPECTED_BINDING')
    path = Path(root) / 'lifecycle.jsonl'
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise EvidenceError('LIFECYCLE_SYMLINK')
    payload = _bytes(path)
    if len(payload) > 64 * 1024:
        raise EvidenceError('LIFECYCLE_SIZE')
    def pairs(items):
        value = {}
        for key, item in items:
            if key in value:
                raise EvidenceError('LIFECYCLE_DUPLICATE_KEY')
            value[key] = item
        return value
    def constant(value):
        raise EvidenceError('LIFECYCLE_NONFINITE')
    try:
        lines = payload.decode('utf-8').splitlines()
        if len(lines) != len(fields):
            raise EvidenceError('LIFECYCLE_EVENT_COUNT')
        events = [json.loads(line, object_pairs_hook=pairs, parse_constant=constant) for line in lines]
    except (UnicodeError, ValueError) as exc:
        raise EvidenceError('LIFECYCLE_JSON_INVALID') from exc
    for index, ((milestone, allowed), row) in enumerate(zip(fields.items(), events)):
        if (not isinstance(row, dict) or set(row) != common | allowed
                or row['schema'] != 'spec180-yolo-lifecycle-event-v1'
                or row['milestone'] != milestone):
            raise EvidenceError('LIFECYCLE_SCHEMA_OR_ORDER')
        if (row['caseId'], row['requestId'], row['attemptId']) != (case, request_id, attempt_id):
            raise EvidenceError('LIFECYCLE_PROTOCOL_BINDING')
        stamp = row['timestampUnix']
        if (type(row['sequence']) is not int or row['sequence'] != index
                or type(stamp) not in (int, float) or not 0 < stamp < 1e15
                or not math.isfinite(stamp)):
            raise EvidenceError('LIFECYCLE_SEQUENCE_OR_TIME')
        # time.time() may move backwards under clock correction; sequence,
        # not wall-clock monotonicity, establishes the journal ordering.
        for key in allowed:
            value = row[key]
            if key.endswith('Digest') and (not isinstance(value, str)
                    or not re.fullmatch(r'sha256:[0-9a-f]{64}', value)):
                raise EvidenceError('LIFECYCLE_DIGEST')
            if key.endswith('Count') or key == 'candidatePriority':
                minimum = 0 if key == 'candidatePriority' else 1
                if type(value) is not int or not minimum <= value <= 1000000:
                    raise EvidenceError('LIFECYCLE_COUNT')
    if (events[4]['candidateId'], events[4]['candidateDigest']) != (candidate_id, candidate_digest):
        raise EvidenceError('LIFECYCLE_CANDIDATE_MISMATCH')
    if events[-1]['status'] is not True or events[-1]['requestCount'] != 1:
        raise EvidenceError('LIFECYCLE_TERMINAL_NOT_PASS')
    return dict(events=events, requestId=request_id, attemptId=attempt_id,
                planDigest=events[6]['planDigest'], resultDigest=events[-1]['resultDigest'],
                qualification='LIFECYCLE_COMPONENT_ONLY')


def reanalyze_numerical_response(root, reference, *, case, request_id, attempt_id,
                               plan_digest, result_digest, candidate_id, candidate_digest):
    """Recompute fixed-input numerical comparison from retained User bytes.

    Caller authenticates/pins the independent reference and lifecycle inputs.
    This is one request's numerical component, NOT the complete T006 verdict.
    """
    import hashlib
    from pathlib import Path
    from runtime.yolo_profile import _read_plane
    from runtime.yolo_bundle import _bytes
    from ndnsf_distributed_inference.adapters.yolo.tensor_bundle import decode_tensor_bundle
    from ndnsf_distributed_inference.adapters.yolo.reference import compare_reference
    root = Path(root)
    record_path, payload_path = root / 'yolo-numerical.json', root / 'yolo-response.bin'
    if any(p.is_symlink() for p in (record_path, payload_path, root, *root.parents)):
        raise EvidenceError('NUMERICAL_SYMLINK')
    record = _read_plane(record_path)
    expected = dict(schemaVersion='spec180-yolo-numerical-v1', case=case, requestId=request_id,
        attemptId=attempt_id, planDigest=plan_digest, candidateId=candidate_id, candidateDigest=candidate_digest,
        manifestDigest=reference.manifest_digest, oracleDigest=reference.oracle_digest,
        fixtureDigest=reference.fixture_digest,
        inputTensorDigest='sha256:' + hashlib.sha256(reference.input_tensor.tobytes()).hexdigest(),
        responseDigest=result_digest, responsePath='yolo-response.bin')
    if any(record.get(k) != v for k, v in expected.items()):
        raise EvidenceError('NUMERICAL_LINEAGE_OR_REFERENCE')
    payload = _bytes(payload_path)
    if (not 0 < len(payload) <= 1024 * 1024 or type(record.get('responseBytes')) is not int
            or record['responseBytes'] != len(payload)
            or 'sha256:' + hashlib.sha256(payload).hexdigest() != result_digest):
        raise EvidenceError('NUMERICAL_RESPONSE_BYTES')
    tensors = decode_tensor_bundle(payload)
    if set(tensors) != {'predictions'}:
        raise EvidenceError('NUMERICAL_RESPONSE_TENSORS')
    comparison = compare_reference(reference, tensors['predictions'])
    if (comparison['matched'] is not True or record.get('matched') is not True
            or any(record.get(k) != v for k, v in comparison.items())):
        raise EvidenceError('NUMERICAL_REANALYSIS_FAILED')
    return dict(comparison, responseDigest=result_digest, qualification='NUMERICAL_COMPONENT_ONLY')


class EvidenceError(ValueError):
    pass


def validate_runtime_publication_receipt(expected, receipt):
    """Match Controller readback evidence, retaining exact artifact cardinality."""
    if (not isinstance(expected, Mapping) or not isinstance(receipt, Mapping)
            or receipt.get('schema') != 'spec180-runtime-publication-receipt-v1'):
        raise EvidenceError('CASE_RUNTIME_PUBLICATION_RECEIPT_SCHEMA_INVALID')
    for field in ('catalogueDataName', 'catalogueSigner', 'cataloguePayloadDigest'):
        if (not isinstance(expected.get(field), str) or not expected[field]
                or receipt.get(field) != expected[field]):
            raise EvidenceError('CASE_RUNTIME_PUBLICATION_RECEIPT_MISMATCH')
    def rows(document):
        values = document.get('artifacts')
        if not isinstance(values, list) or not values or len(values) > 256:
            raise EvidenceError('CASE_RUNTIME_PUBLICATION_ARTIFACT_RECEIPT_MISMATCH')
        result = {}
        for row in values:
            if (not isinstance(row, Mapping) or not isinstance(row.get('dataName'), str)
                    or not row['dataName'].startswith('/')
                    or not isinstance(row.get('payloadDigest'), str)
                    or not re.fullmatch(r'sha256:[0-9a-f]{64}', row['payloadDigest'])
                    or row['dataName'] in result):
                raise EvidenceError('CASE_RUNTIME_PUBLICATION_ARTIFACT_RECEIPT_MISMATCH')
            result[row['dataName']] = row['payloadDigest']
        return result
    if rows(expected) != rows(receipt):
        raise EvidenceError('CASE_RUNTIME_PUBLICATION_ARTIFACT_RECEIPT_MISMATCH')
    return receipt
