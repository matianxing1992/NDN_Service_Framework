"""Negative-case observations; no timeout, file or helper alone is a PASS."""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import re
import time

from .yolo_collection import _write_once
from .yolo_result import validate_lifecycle


def _native_name(value):
    """Canonicalize numeric NNI components emitted by ndn-cxx.

    The retained public contract renders integer components as decimal URI
    text, while ndn-cxx may emit the same components as escaped NNI bytes.
    Only labelled numeric components are normalized; opaque Name components
    remain exact.
    """
    labels = {'ATTEMPT', 'ROUND', 'RANK', 'MICROBATCH'}
    parts = value.split('/')
    for index in range(len(parts) - 1):
        if parts[index] in labels and re.fullmatch(r'[0-9]+', parts[index + 1]):
            number = int(parts[index + 1])
            width = max(1, (number.bit_length() + 7) // 8)
            parts[index + 1] = ''.join(
                f'%{byte:02X}' for byte in number.to_bytes(width, 'big'))
    return '/'.join(parts)


def _read_json(path, *, limit=65536):
    from .yolo_bundle import _bytes
    from .yolo_profile import _object
    path = Path(path)
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError('NEGATIVE_RECORD_SYMLINK')
    payload = _bytes(path)
    if len(payload) > limit:
        raise ValueError('NEGATIVE_RECORD_SIZE')
    try:
        value = json.loads(payload, object_pairs_hook=_object)
        json.dumps(value, allow_nan=False)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ValueError('NEGATIVE_RECORD_JSON') from exc
    return value, 'sha256:' + hashlib.sha256(payload).hexdigest()


def read_negative_user(output, *, run_id, request_id, candidate_digest,
                       placement_id, placement_digest, deadline_ms):
    """Re-read bounded observations; absence of a response is never a verdict."""
    # Reuse the externally supplied binding checks, not fields from the record.
    expected = NegativeUserObserver(run_id=run_id, request_id=request_id,
        candidate_digest=candidate_digest, output=output, placement_id=placement_id,
        placement_digest=placement_digest, deadline_ms=deadline_ms)
    selection = expected._selection()
    value, digest = _read_json(Path(output) / 'negative-user.json')
    fields = {'schema', 'qualification', 'runId', 'requestId', 'attempt',
        'candidateDigest', 'placementCandidateId', 'placementCandidateDigest',
        'planDigest', 'deadlineMs', 'dependencyNoProgressMs', 'elapsedMs',
        'observedAfterShutdown', 'waitErrorType', 'snapshotErrorType', 'response'}
    if (not isinstance(value, dict) or set(value) != fields
            or value['schema'] != 'tiger-yolo-negative-user-v1'
            or value['qualification'] != 'OBSERVATION_ONLY'
            or value['runId'] != run_id or value['requestId'] != request_id
            or type(value['attempt']) is not int or value['attempt'] != 1
            or value['candidateDigest'] != candidate_digest
            or value['placementCandidateId'] != placement_id
            or value['placementCandidateDigest'] != placement_digest
            or value['planDigest'] != selection['planDigest']
            or type(value['deadlineMs']) is not int or value['deadlineMs'] != deadline_ms
            or type(value['dependencyNoProgressMs']) is not int
            or value['dependencyNoProgressMs'] != expected.dependency_no_progress_ms
            or type(value['elapsedMs']) is not int or not 1 <= value['elapsedMs'] <= deadline_ms
            or value['observedAfterShutdown'] is not True
            or any(not isinstance(value[k], str) or re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,127}|', value[k]) is None
                   for k in ('waitErrorType', 'snapshotErrorType'))):
        raise ValueError('NEGATIVE_USER_RECORD_BINDING')
    response = value['response']
    if (not isinstance(response, dict) or set(response) != {'present', 'success', 'bytes', 'sha256'}
            or type(response['present']) is not bool or type(response['success']) is not bool
            or type(response['bytes']) is not int or not 0 <= response['bytes'] < 2**64):
        raise ValueError('NEGATIVE_USER_RESPONSE_RECORD')
    if response['present']:
        if not isinstance(response['sha256'], str) or re.fullmatch(r'sha256:[a-f0-9]{64}', response['sha256']) is None:
            raise ValueError('NEGATIVE_USER_RESPONSE_DIGEST')
    elif (response != dict(present=False, success=False, bytes=0, sha256=None)
          or not value['waitErrorType'] or not value['snapshotErrorType']):
        raise ValueError('NEGATIVE_USER_ABSENCE_RECORD')
    return dict(observation=value, lifecycle=selection, sourceDigest=digest,
                qualification='NEGATIVE_USER_COMPONENT_ONLY')


def read_negative_cutpoint(logs, *, contract, request_id, plan_digest, providers_by_role):
    """Join a source-emitted output suppression with the consumer's exact failure.

    Logs must already be bound to closed node receipts. No operator-authored
    failure flags or wall-clock ordering across hosts are accepted here.
    """
    from .yolo_bundle import _bytes
    from .yolo_profile import _object
    edges = [edge for edge in contract['edges']
             if edge['producer'] == 'DetectShard0' and edge['consumer'] == 'Merge']
    if not edges or set(logs) != {'BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'}:
        raise ValueError('NEGATIVE_CUTPOINT_EDGE')
    session = contract['sessionId']
    records, failures, dependencies, digests = [], [], [], {}
    for role, filename in logs.items():
        path = Path(filename)
        if any(p.is_symlink() for p in (path, *path.parents)):
            raise ValueError('NEGATIVE_LOG_SYMLINK')
        payload = _bytes(path)
        if len(payload) > 16 * 1024 * 1024:
            raise ValueError('NEGATIVE_LOG_SIZE')
        digests[role] = 'sha256:' + hashlib.sha256(payload).hexdigest()
        for line in payload.decode('utf-8').splitlines():
            if 'NDNSF_DI_OUTPUT_WITHHELD ' in line:
                try:
                    record = json.loads(line.split('NDNSF_DI_OUTPUT_WITHHELD ', 1)[1], object_pairs_hook=_object)
                except (ValueError, RecursionError) as exc:
                    raise ValueError('NEGATIVE_CUTPOINT_JSON') from exc
                if role != 'DetectShard0':
                    raise ValueError('NEGATIVE_CUTPOINT_OWNER')
                records.append(record)
            if 'NDNSF_DI_NATIVE_FAILURE ' in line:
                failures.append((role, line.split('NDNSF_DI_NATIVE_FAILURE ', 1)[1]))
            if 'NDNSF_DI_DEPENDENCY_OBJECT ' in line:
                tokens = line.split('NDNSF_DI_DEPENDENCY_OBJECT ', 1)[1].split()
                parts = [token.partition('=') for token in tokens]
                if any(not sep for _, sep, _ in parts) or len({k for k, _, _ in parts}) != len(parts):
                    raise ValueError('NEGATIVE_DEPENDENCY_LOG_FIELDS')
                dependency = {k: v for k, _, v in parts}
                dependencies.append(dependency)
    if len(records) != 1:
        raise ValueError('NEGATIVE_CUTPOINT_COUNT')
    row = records[0]
    matching_edges = [edge for edge in edges
                      if _native_name(edge['planned_name']) == _native_name(row.get('plannedDataName', ''))]
    if len(matching_edges) != 1:
        raise ValueError('NEGATIVE_CUTPOINT_EDGE_IDENTITY')
    edge = matching_edges[0]
    fields = {'schema', 'session', 'requestId', 'attempt', 'planDigest',
        'producerRole', 'consumerRole', 'manifestDataName', 'plannedDataName',
        'endpointDigest', 'contentDigest', 'bytes', 'provider', 'providerBootId',
        'round', 'microbatch', 'operationKind', 'tensor', 'atMs'}
    if not isinstance(row, dict) or set(row) != fields:
        raise ValueError('NEGATIVE_CUTPOINT_SCHEMA')
    # Boost property_tree emits numeric leaf values as decimal JSON strings.
    for field in ('attempt', 'bytes', 'atMs'):
        if (not isinstance(row[field], str) or re.fullmatch(r'[1-9][0-9]{0,19}', row[field]) is None
                or int(row[field]) >= 2**64):
            raise ValueError('NEGATIVE_CUTPOINT_NUMBER')
    for field in ('round', 'microbatch'):
        if (not isinstance(row[field], str) or re.fullmatch(r'(?:0|[1-9][0-9]{0,19})', row[field]) is None
                or int(row[field]) >= 2**64):
            raise ValueError('NEGATIVE_CUTPOINT_NUMBER')
    if (row['schema'] != 'ndnsf-di-withheld-output-v1'
            or row['session'] not in (request_id, session)
            or row['requestId'] != request_id or row['attempt'] != '1'
            or row['planDigest'] != plan_digest or row['producerRole'] != edge['producer']
            or row['consumerRole'] != edge['consumer']
            or _native_name(row['plannedDataName']) != _native_name(edge['planned_name'])
            or _native_name(row['manifestDataName']) != _native_name(edge['planned_name'].rstrip('/') + '/MANIFEST')
            or row['provider'] != providers_by_role['DetectShard0']
            or not isinstance(row['providerBootId'], str) or not row['providerBootId']
            or not isinstance(row['operationKind'], str) or not row['operationKind']
            or not isinstance(row['tensor'], str) or not row['tensor']
            or any(not isinstance(row[k], str) or re.fullmatch(r'sha256:[a-f0-9]{64}', row[k]) is None
                   for k in ('endpointDigest', 'contentDigest'))):
        raise ValueError('NEGATIVE_CUTPOINT_BINDING')
    for dependency in dependencies:
        if (dependency.get('session') in (request_id, session)
                and dependency.get('producer') == 'DetectShard0'
                and dependency.get('consumer') == 'Merge'
                and dependency.get('planned_name') in (None, 'none', row['plannedDataName'])):
            raise ValueError('NEGATIVE_WITHHELD_EDGE_WAS_TRANSFERRED')
    failure_suffix = ' role=Merge reason=failed to fetch signed exact Data: ' + row['manifestDataName']
    allowed_failures = {
        'session=' + candidate + failure_suffix
        for candidate in (request_id, session)
    }
    if len(failures) != 1 or failures[0][0] != 'Merge' or failures[0][1] not in allowed_failures:
        raise ValueError('NEGATIVE_CONSUMER_EXACT_FAILURE')
    return dict(edge=edge, cutpoint=row, logDigests=digests,
        qualification='NEGATIVE_CUTPOINT_COMPONENT_ONLY')


def collect_negative_verdict(nodes, *, plan, runtime_candidate_digest,
                             placement_candidate_id, placement_candidate_digest,
                             graph_digest, catalogue_digest, providers_by_role,
                             allocation_expected, request_deadline_ms):
    """Derive the rejection from retained sources after both ranks have exited."""
    from . import yolo_result as result
    from .yolo_bundle import verify_preparation
    roles = {'BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'}
    requests = plan.get('requests')
    if (plan.get('case') != 'negative-dependency' or not isinstance(nodes, dict)
            or set(nodes) != {0, 1} or any(type(k) is not int for k in nodes)
            or not isinstance(requests, list) or len(requests) != 1
            or set(requests[0]) != {'index', 'warmup', 'requestId', 'output'}
            or type(requests[0]['index']) is not int or requests[0]['index'] != 0
            or requests[0]['warmup'] is not False or set(providers_by_role) != roles
            or any(providers_by_role[r] != plan['identities'][r] for r in roles)):
        raise ValueError('NEGATIVE_RETAINED_PLAN')
    root = Path(plan['output'])
    closed, devices, logs = {}, {}, {}
    for rank in (0, 1):
        node = nodes[rank]
        if (set(node) != {'root', 'receiptDigest', 'preparationDigest', 'allocationDigest', 'gpuProbeDigest'}
                or Path(node['root']) != root / ('node' + str(rank))):
            raise ValueError('NEGATIVE_RETAINED_NODE')
        closed[rank] = result.read_node_log_receipt(node['root'],
            receipt_digest=node['receiptDigest'], plan=plan, rank=rank,
            preparation_digest=node['preparationDigest'], candidate_digest=runtime_candidate_digest)
        devices[rank] = result.read_retained_device_binding(node['root'],
            receipt_digest=node['receiptDigest'], allocation_digest=node['allocationDigest'],
            gpu_probe_digest=node['gpuProbeDigest'], plan=plan, rank=rank,
            preparation_digest=node['preparationDigest'], candidate_digest=runtime_candidate_digest,
            expected=allocation_expected)
        logs.update({role: closed[rank]['logs'][role]['path']
                     for role in roles & closed[rank]['logs'].keys()})
    if nodes[0]['preparationDigest'] != nodes[1]['preparationDigest']:
        raise ValueError('NEGATIVE_PREPARATION_CROSS_NODE')
    preparation = verify_preparation(root / 'public', plan,
        expected_receipt_digest=nodes[0]['preparationDigest'], candidate_digest=runtime_candidate_digest)
    if any(preparation[k] != v for k, v in {
            'placementCandidateId': placement_candidate_id,
            'placementCandidateDigest': placement_candidate_digest,
            'graphDigest': graph_digest, 'catalogueDigest': catalogue_digest}.items()):
        raise ValueError('NEGATIVE_PREPARATION_COLLECTION_BINDING')
    first, second = devices[0], devices[1]
    if (any(first['allocation'][k] != second['allocation'][k]
            for k in ('jobId', 'stepId', 'submissionKey', 'hosts'))
            or first['allocation']['hostname'] == second['allocation']['hostname']
            or first['gpuBinding']['uuid'] == second['gpuBinding']['uuid']
            or first['uid'] != second['uid']):
        raise ValueError('NEGATIVE_ALLOCATION_CROSS_NODE')
    output = root / 'node0/user/requests/0'
    if Path(requests[0]['output']) != output:
        raise ValueError('NEGATIVE_REQUEST_OUTPUT')
    user = read_negative_user(output, run_id=plan['runId'], request_id=requests[0]['requestId'],
        candidate_digest=runtime_candidate_digest, placement_id=placement_candidate_id,
        placement_digest=placement_candidate_digest, deadline_ms=request_deadline_ms)
    lifecycle, observation = user['lifecycle'], user['observation']
    events = lifecycle['events']
    def digest(value):
        return 'sha256:' + hashlib.sha256(json.dumps(value, ensure_ascii=False,
            sort_keys=True, separators=(',', ':'), allow_nan=False).encode()).hexdigest()
    count = len(set(providers_by_role.values()))
    if (events[3]['graphDigest'] != graph_digest or events[3]['catalogueDigest'] != catalogue_digest
            or events[8]['roleDigest'] != digest(providers_by_role)
            or events[8]['providerCount'] != count or events[4]['providerCount'] != count
            or events[7]['selectedRoleCount'] != 4 or events[2]['ackCount'] < count
            or events[7]['selectionDigest'] != digest({'plan': lifecycle['planDigest'],
                                                      'ack': events[2]['ackSnapshotDigest']})):
        raise ValueError('NEGATIVE_SELECTION_BINDING')
    if observation['response']['success']:
        raise ValueError('NEGATIVE_UNEXPECTED_SUCCESS')
    contract = result.read_public_dependency_contract(output / 'yolo-public-assignments.json',
        request_id=requests[0]['requestId'], attempt=1, plan_digest=lifecycle['planDigest'],
        providers_by_role=providers_by_role)
    cutpoint = read_negative_cutpoint(logs, contract=contract,
        request_id=requests[0]['requestId'], plan_digest=lifecycle['planDigest'], providers_by_role=providers_by_role)
    execution = {}
    # Both sides really computed on their allocated GPUs before the remote head
    # reached the publication cutpoint. Merge intentionally cannot complete.
    for rank, role in ((0, 'BackboneNeck'), (1, 'DetectShard0')):
        node = nodes[rank]
        checked = result.collect_retained_role_execution(node['root'],
            receipt_digest=node['receiptDigest'], plan=plan, rank=rank, role=role,
            preparation_digest=node['preparationDigest'], candidate_digest=runtime_candidate_digest,
            provider=providers_by_role[role], request_id=requests[0]['requestId'], attempt=1,
            execution_plan_digest=lifecycle['planDigest'], gpu_binding=devices[rank]['gpuBinding'])
        native = checked['native']['observation']
        model = contract['modelBindings'][role]
        if (native['modelDigest'] != model['modelManifestDigest']
                or native['artifactDigests'].get(role) != model['artifactDigest']
                or checked['native']['logDigest'] != cutpoint['logDigests'][role]):
            raise ValueError('NEGATIVE_COMPUTE_MODEL_BINDING')
        execution[role] = checked
    for rank in (0, 1):
        for role in roles & closed[rank]['logs'].keys():
            if closed[rank]['logs'][role]['logDigest'] != cutpoint['logDigests'][role]:
                raise ValueError('NEGATIVE_LOG_CHANGED')
    rejection = dict(schema='tiger-yolo-expected-rejection-v1', status='REJECTED',
        qualification='EXPECTED_REJECTION_COMPONENT_ONLY', case='negative-dependency',
        runId=plan['runId'], requestId=requests[0]['requestId'], attempt=1,
        candidateDigest=runtime_candidate_digest,
        selection=dict(status='COMMITTED', selectedProvider=providers_by_role['Merge'],
                       selectionCount=1, reselectionCount=0),
        failure=dict(boundary='DEPENDENCY_DATA_MISSING',
            edge=dict(producer='DetectShard0', consumer='Merge', plannedName=cutpoint['edge']['planned_name']),
            observedAfterSelection=True, reselected=False),
        response=dict(present=observation['response']['present'], success=False),
        cleanup=dict(qualification='CLEANUP_COMPONENT_ONLY', allChildrenReaped=True,
                     forced=False, remainingChildren=0, deadlineSatisfied=True),
        elapsedMs=observation['elapsedMs'], deadlineMs=request_deadline_ms)
    verdict = result.finalize_expected_rejection(rejection, plan=plan,
        request_id=requests[0]['requestId'], attempt=1, candidate_digest=runtime_candidate_digest,
        request_deadline_ms=request_deadline_ms)
    return dict(verdict, userObservation=user, cutpoint=cutpoint, devices=devices,
        execution=execution, publicContractDigest=contract['sourceDigest'],
        nodeReceiptDigests={rank: nodes[rank]['receiptDigest'] for rank in (0, 1)})


class NegativeUserObserver:
    """Observe one real handle, then snapshot it after the owner's shutdown.

    The application main must return only after its finally/shutdown completes.
    Re-reading the public handle then catches a response that arrived between
    the initial wait failure and shutdown. Native failure/cutpoint/cleanup
    evidence is deliberately left for the independent retained collector.
    """
    def __init__(self, *, run_id, request_id, candidate_digest, output,
                 placement_id, placement_digest, deadline_ms):
        if (not isinstance(run_id,str) or not run_id
                or not isinstance(request_id,str) or not request_id.startswith('/')
                or request_id=='/' or not isinstance(placement_id,str) or not placement_id
                or any(not isinstance(v,str) or re.fullmatch(r'sha256:[a-f0-9]{64}',v) is None
                       for v in (candidate_digest,placement_digest))
                or type(deadline_ms) is not int or not 1501<=deadline_ms<=60000):
            raise ValueError('NEGATIVE_USER_EXPECTED_BINDING')
        self.run_id,self.request_id,self.candidate_digest=run_id,request_id,candidate_digest
        self.output=Path(output)
        self.placement_id,self.placement_digest=placement_id,placement_digest
        self.deadline_ms=deadline_ms
        self.dependency_no_progress_ms=deadline_ms//2
        self.handle=None
        self.finished=False

    def _selection(self):
        return validate_lifecycle(self.output,case='negative-dependency',request_id=self.request_id,
            attempt_id='attempt-1',candidate_id=self.placement_id,candidate_digest=self.placement_digest,
            require_terminal=False)

    def terminal(self, *, handle, journal, args, request_started_at):
        if (self.handle is not None or args.request_id!=self.request_id
                or args.lifecycle_case!='negative-dependency' or args.timeout_ms!=self.deadline_ms
                or Path(args.lifecycle_output_dir)!=self.output
                or journal.request_id!=self.request_id or journal.attempt_id!='attempt-1'
                or type(request_started_at) not in (int,float) or not math.isfinite(request_started_at)):
            raise ValueError('NEGATIVE_USER_HANDLE_BINDING')
        selection=self._selection()
        if handle.execution_plan_digest!=selection['planDigest']:
            raise ValueError('NEGATIVE_USER_PLAN_BINDING')
        self.started_at=request_started_at
        elapsed=(time.monotonic()-request_started_at)*1000
        remaining=self.deadline_ms-math.ceil(elapsed)-min(5000,self.deadline_ms//4)
        if elapsed<0 or remaining<=0:
            raise TimeoutError('NEGATIVE_USER_REQUEST_BUDGET')
        self.handle=handle
        self.selection=selection
        self.wait_error=''
        self.wait_response=None
        try:
            self.wait_response=handle.response(remaining)
        except Exception as exc:
            self.wait_error=type(exc).__name__
        # This return denotes completion of observation only. Qualification
        # requires the independent consumer/producer records after node cleanup.
        return 0

    def finish_after_shutdown(self, owner_exit_code):
        if self.handle is None or self.finished or type(owner_exit_code) is not int or owner_exit_code!=0:
            raise ValueError('NEGATIVE_USER_OWNER_NOT_COMPLETE')
        self.finished=True
        snapshot_error=''
        response=self.wait_response
        try:
            latest=self.handle.response(1)
            if latest is not None: response=latest
        except Exception as exc:
            snapshot_error=type(exc).__name__
        selection=self._selection()
        if selection!=self.selection:
            raise ValueError('NEGATIVE_USER_SELECTION_CHANGED')
        if response is None and (not self.wait_error or not snapshot_error):
            raise ValueError('NEGATIVE_USER_MISSING_WAIT_RESULT')
        if response is not None and (type(response.status) is not bool
                                     or not isinstance(response.payload,(bytes,bytearray))):
            raise ValueError('NEGATIVE_USER_RESPONSE_TYPE')
        elapsed=math.ceil((time.monotonic()-self.started_at)*1000)
        if not 1<=elapsed<=self.deadline_ms:
            raise TimeoutError('NEGATIVE_USER_DEADLINE')
        value=dict(schema='tiger-yolo-negative-user-v1',qualification='OBSERVATION_ONLY',
            runId=self.run_id,requestId=self.request_id,attempt=1,
            candidateDigest=self.candidate_digest,placementCandidateId=self.placement_id,
            placementCandidateDigest=self.placement_digest,planDigest=selection['planDigest'],
            deadlineMs=self.deadline_ms,dependencyNoProgressMs=self.dependency_no_progress_ms,
            elapsedMs=elapsed,observedAfterShutdown=True,waitErrorType=self.wait_error,
            snapshotErrorType=snapshot_error,response=dict(present=response is not None,
                success=False if response is None else response.status,
                bytes=0 if response is None else len(response.payload),
                sha256=None if response is None else 'sha256:'+hashlib.sha256(response.payload).hexdigest()))
        _write_once(self.output/'negative-user.json',value)
        return value
