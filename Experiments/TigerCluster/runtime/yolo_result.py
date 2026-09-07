"""Shared YOLO evidence checks. Publication readiness is NOT inference PASS.

Numerical reanalysis is a component only. Full lifecycle/role/edge/cleanup
collection is still pending in T006.
"""
from collections.abc import Mapping
import re


def resolve_role_output(root, container_path):
    """Translate only an exact /output descendant into an owned role output."""
    from pathlib import Path
    root = Path(root)
    if (not isinstance(container_path, str) or not container_path.startswith('/output/')
            or '\\' in container_path or '\x00' in container_path):
        raise EvidenceError('ROLE_OUTPUT_NAMESPACE')
    parts = container_path[len('/output/'):].split('/')
    if any(part in ('', '.', '..') for part in parts):
        raise EvidenceError('ROLE_OUTPUT_COMPONENT')
    path = root.joinpath(*parts)
    if any(p.is_symlink() for p in (path, *path.parents)) or not root.is_dir() or not path.is_file():
        raise EvidenceError('ROLE_OUTPUT_FILE')
    return path


def collect_role_execution(worker, *, role, provider, request_id, attempt, plan_digest):
    """Join real launcher/log/output bindings with native and ORT components.

    Provider identity comes from the verified prepared role plan. The full
    collector must additionally validate cleanup, complete role cover,
    certified model coverage, physical GPU and dependency-edge observations.
    """
    if (worker.closed is not True or role not in worker.roles
            or role not in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')):
        raise EvidenceError('ROLE_COLLECTION_SCOPE')
    launches = [item for item in worker.launches
                if item.get('role') == role and item.get('invocation') is None]
    if len(launches) != 1 or launches[0].get('startError'):
        raise EvidenceError('ROLE_COLLECTION_LAUNCH')
    if role == 'Merge':
        runner = 'native-yolo-postprocess'
    elif worker.mode == 'local-cpu':
        runner = 'onnxruntime-cpu'
    else:
        runner = 'onnxruntime-cuda'
    native = read_native_observation(worker.children.log_dir / (role + '.log'),
        provider=provider, role=role, request_id=request_id, attempt=attempt,
        plan_digest=plan_digest, pid=launches[0].get('pid'), runner_kind=runner)
    profile = None
    if runner != 'native-yolo-postprocess':
        path = resolve_role_output(worker.output / role, native['observation'].get('providerProfilePath'))
        profile = validate_ort_profile(path, native['observation'])
    return dict(native=native, profile=profile, rank=worker.rank,
                qualification='ROLE_EXECUTION_COMPONENT_ONLY')


def validate_worker_cleanup(worker, rows):
    """Check actual NodeRuntime ownership after close, never caller counts.

    This proves only the launched set's cleanup. The final collector must
    separately require every planned role and finite invocation was launched.
    """
    if (worker.closed is not True or worker.leases or worker.children.children
            or worker.finite_children.children):
        raise EvidenceError('CLEANUP_OWNERS_REMAIN')
    expected = {}
    for launch in worker.launches:
        role = launch.get('role')
        invocation = launch.get('invocation')
        if (not isinstance(role, str) or not role or launch.get('startError')
                or type(launch.get('pid')) is not int or launch['pid'] <= 0
                or (invocation is not None and (not isinstance(invocation, str) or not invocation))):
            raise EvidenceError('CLEANUP_LAUNCH_INCOMPLETE')
        name = role + '-' + invocation if invocation is not None else role
        if name in expected:
            raise EvidenceError('CLEANUP_LAUNCH_DUPLICATE')
        expected[name] = (launch['pid'], invocation is not None)
    if not expected or not isinstance(rows, list) or len(rows) != len(expected):
        raise EvidenceError('CLEANUP_INVENTORY')
    seen = set()
    for row in rows:
        if not isinstance(row, dict) or row.get('name') not in expected or row['name'] in seen:
            raise EvidenceError('CLEANUP_ROW_IDENTITY')
        name = row['name']
        seen.add(name)
        pid, finite = expected[name]
        if (type(row.get('pid')) is not int or row['pid'] != pid
                or row.get('reaped') is not True or row.get('forced') is not False
                or row.get('leaseReleased') is not True
                or any(row.get(k) for k in ('cleanupError', 'cleanupTimedOut', 'leaseError'))
                or type(row.get('exitCode')) is not int):
            raise EvidenceError('CLEANUP_NOT_CLEAN')
        if finite:
            if row.get('kind') != 'finite' or row['exitCode'] != 0:
                raise EvidenceError('CLEANUP_FINITE_EXIT')
        elif (row.get('kind') == 'finite' or row.get('exitedBeforeCleanup') is not False
                or row['exitCode'] not in (0, -15, 143)):
            raise EvidenceError('CLEANUP_SERVICE_EXIT')
    return dict(childCount=len(expected), qualification='CLEANUP_COMPONENT_ONLY')


def read_native_observation(path, **binding):
    """Select exactly one role/request observation from an owned bounded log.

    Caller supplies the actual launcher's log path and process binding. This
    does not establish host/GPU identity or replace independent ORT evidence.
    """
    import hashlib
    from pathlib import Path
    from runtime.yolo_bundle import _bytes
    path = Path(path)
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise EvidenceError('NATIVE_LOG_SYMLINK')
    payload = _bytes(path)
    prefix = 'NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED '
    selected = []
    try:
        lines = payload.decode('utf-8').splitlines()
    except UnicodeError as exc:
        raise EvidenceError('NATIVE_LOG_ENCODING') from exc
    for line in lines:
        if not line.startswith(prefix):
            continue
        encoded = line[len(prefix):]
        row = decode_native_observation(encoded)
        if row.get('requestId') == binding.get('request_id') and row.get('roles') == [binding.get('role')]:
            selected.append(encoded)
    if len(selected) != 1:
        raise EvidenceError('NATIVE_LOG_OBSERVATION_COUNT')
    result = validate_native_observation(selected[0], **binding)
    return dict(result, logDigest='sha256:' + hashlib.sha256(payload).hexdigest())


def validate_ort_profile(path, observation):
    """Cross-check profile bytes against one already-bound native record.

    Caller resolves the container profile path inside that Provider's owned
    output and binds the observation first. Model coverage, allocation/GPU,
    dependency and cleanup checks are separate; this is not inference PASS.
    """
    import hashlib
    import json
    from pathlib import Path
    from runtime.yolo_bundle import _bytes
    path = Path(path)
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise EvidenceError('ORT_PROFILE_SYMLINK')
    roles = observation.get('roles')
    kind = observation.get('runnerKind')
    if (not isinstance(roles, list) or len(roles) != 1 or not isinstance(roles[0], str)
            or not roles[0] or kind not in ('onnxruntime-cpu', 'onnxruntime-cuda')
            or not isinstance(observation.get('requestId'), str) or not observation['requestId']
            or observation.get('profileRequestId') != observation['requestId']
            or type(observation.get('attemptEpoch')) is not int or observation['attemptEpoch'] <= 0
            or type(observation.get('profileAttemptEpoch')) is not int
            or observation['profileAttemptEpoch'] != observation['attemptEpoch']):
        raise EvidenceError('ORT_PROFILE_REQUEST_BINDING')
    payload = _bytes(path)
    def pairs(items):
        value = {}
        for key, item in items:
            if key in value:
                raise EvidenceError('ORT_PROFILE_DUPLICATE_KEY')
            value[key] = item
        return value
    def constant(value):
        raise EvidenceError('ORT_PROFILE_NONFINITE')
    try:
        events = json.loads(payload, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise EvidenceError('ORT_PROFILE_JSON') from exc
    if not isinstance(events, list) or not events or len(events) > 100000:
        raise EvidenceError('ORT_PROFILE_EVENTS')
    assignments = []
    expected_backend = 'CUDAExecutionProvider' if kind == 'onnxruntime-cuda' else 'CPUExecutionProvider'
    for event in events:
        if not isinstance(event, dict):
            raise EvidenceError('ORT_PROFILE_EVENT')
        if event.get('cat') != 'Node':
            continue
        name, args = event.get('name'), event.get('args', {})
        if not isinstance(name, str) or not name or not isinstance(args, dict):
            raise EvidenceError('ORT_PROFILE_NODE')
        backend = args.get('provider', '')
        if backend == '':
            if name.endswith('_kernel_time'):
                raise EvidenceError('ORT_PROFILE_KERNEL_PROVIDER_MISSING')
            continue
        if backend != expected_backend:
            raise EvidenceError('ORT_PROFILE_BACKEND')
        assignments.append(dict(role=roles[0], nodeName=name, provider=backend, modelNode=True))
    if not assignments or assignments != observation.get('nodeProviderAssignments'):
        raise EvidenceError('ORT_PROFILE_ASSIGNMENT_MISMATCH')
    return dict(profileDigest='sha256:' + hashlib.sha256(payload).hexdigest(),
        modelNodeEvents=len(assignments), qualification='ORT_PROFILE_COMPONENT_ONLY')


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
