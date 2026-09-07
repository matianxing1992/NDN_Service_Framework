"""Shared YOLO evidence checks. Publication readiness is NOT inference PASS.

Numerical reanalysis is a component only. Full lifecycle/role/edge/cleanup
collection is still pending in T006.
"""
from collections.abc import Mapping
import re


def read_public_dependency_contract(path, *, request_id, attempt, plan_digest, providers_by_role):
    """Join User-retained producer/consumer projections against external facts.

    The record is not a signature or execution proof. Caller obtains expected
    identities from the request/placement, not from this file. Application
    input has no Provider publication peer and is returned separately.
    """
    import hashlib
    import json
    from pathlib import Path
    from runtime.yolo_bundle import _bytes
    if (not isinstance(request_id, str) or not request_id.strip('/')
            or any(c.isspace() for c in request_id)
            or type(attempt) is not int or not 0 < attempt < 2**64
            or not isinstance(plan_digest, str) or not re.fullmatch(r'sha256:[0-9a-f]{64}', plan_digest)
            or not isinstance(providers_by_role, Mapping) or not 0 < len(providers_by_role) <= 64
            or any(not isinstance(v, str) or not v or any(c.isspace() for c in v)
                   for pair in providers_by_role.items() for v in pair)):
        raise EvidenceError('PUBLIC_DEPENDENCY_EXPECTED_BINDING')
    path = Path(path)
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise EvidenceError('PUBLIC_DEPENDENCY_SYMLINK')
    payload = _bytes(path)
    if len(payload) > 1024 * 1024:
        raise EvidenceError('PUBLIC_DEPENDENCY_SIZE')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise EvidenceError('PUBLIC_DEPENDENCY_DUPLICATE_FIELD')
            result[key] = value
        return result
    def constant(value):
        raise EvidenceError('PUBLIC_DEPENDENCY_NONFINITE')
    try:
        document = json.loads(payload, object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeError, RecursionError, json.JSONDecodeError) as exc:
        raise EvidenceError('PUBLIC_DEPENDENCY_JSON') from exc
    if (not isinstance(document, dict) or set(document) != {'schema', 'assignments'}
            or document['schema'] != 'yolo-public-assignments-v2'
            or not isinstance(document['assignments'], list)
            or len(document['assignments']) != len(providers_by_role)):
        raise EvidenceError('PUBLIC_DEPENDENCY_SCHEMA')
    fields = {'schema', 'requestId', 'attempt', 'planDigest', 'sessionId', 'provider', 'role', 'inputs', 'outputs', 'model'}
    edge_fields = ('scope', 'producer', 'consumer', 'planned_name')
    session = request_id.strip('/') + '/attempt/' + str(attempt)
    inputs, outputs, application_inputs, roles = {}, {}, [], set()
    model_bindings = {}
    for row in document['assignments']:
        if (not isinstance(row, dict) or set(row) != fields
                or not isinstance(row['role'], str) or row['role'] not in providers_by_role
                or row['role'] in roles or row['schema'] != 'tiger-yolo-public-assignment-v2'
                or row['requestId'] != request_id or type(row['attempt']) is not int
                or row['attempt'] != attempt or row['planDigest'] != plan_digest
                or row['sessionId'] != session or row['provider'] != providers_by_role[row['role']]):
            raise EvidenceError('PUBLIC_DEPENDENCY_ROLE_BINDING')
        roles.add(row['role'])
        model = row['model']
        if (not isinstance(model, dict) or set(model) != {'modelManifestDigest', 'artifactDigest'}
                or any(not isinstance(v, str) or re.fullmatch(r'sha256:[0-9a-f]{64}', v) is None
                       for v in model.values())):
            raise EvidenceError('PUBLIC_DEPENDENCY_MODEL_BINDING')
        model_bindings[row['role']] = model
        for direction, collection in (('inputs', inputs), ('outputs', outputs)):
            if not isinstance(row[direction], list) or len(row[direction]) > 64:
                raise EvidenceError('PUBLIC_DEPENDENCY_EDGE_COUNT')
            for edge in row[direction]:
                if (not isinstance(edge, dict) or set(edge) != set(edge_fields)
                        or any(not isinstance(v, str) or any(c.isspace() for c in v) for v in edge.values())
                        or not edge['scope'] or not edge['planned_name'].startswith('/')
                        or edge['consumer'] not in providers_by_role
                        or (edge['producer'] and edge['producer'] not in providers_by_role)
                        or edge['producer'] == edge['consumer']
                        or edge['consumer' if direction == 'inputs' else 'producer'] != row['role']):
                    raise EvidenceError('PUBLIC_DEPENDENCY_EDGE_BINDING')
                key = tuple(edge[field] for field in edge_fields)
                if key in collection:
                    raise EvidenceError('PUBLIC_DEPENDENCY_DUPLICATE_EDGE')
                collection[key] = edge
                if direction == 'inputs' and not edge['producer']:
                    application_inputs.append(edge)
    paired_inputs = {key: value for key, value in inputs.items() if value['producer']}
    if not outputs or outputs.keys() != paired_inputs.keys() or len(outputs) > 64:
        raise EvidenceError('PUBLIC_DEPENDENCY_PAIR_MISMATCH')
    return dict(sessionId=session, edges=[outputs[key] for key in sorted(outputs)],
        modelBindings=model_bindings,
        applicationInputs=application_inputs, sourceDigest='sha256:'+hashlib.sha256(payload).hexdigest(),
        qualification='PUBLIC_DEPENDENCY_COMPONENT_ONLY')


def collect_dependency_result(path, logs_by_role, *, request_id, attempt, plan_digest, providers_by_role):
    """Join public contracts to logs already bound by the caller to owned PIDs.

    APPLICATION_INPUT is pre-satisfied from authenticated request input by the
    native handler, not published by another Provider. Its verification belongs
    to request ingress, not the inter-Provider dependency log pair check.
    """
    contract = read_public_dependency_contract(path, request_id=request_id,
        attempt=attempt, plan_digest=plan_digest, providers_by_role=providers_by_role)
    if set(logs_by_role) != set(providers_by_role):
        raise EvidenceError('DEPENDENCY_LOG_ROLE_COVERAGE')
    result = validate_dependency_edges(logs_by_role, contract['edges'], session_id=contract['sessionId'])
    return dict(result, publicContractDigest=contract['sourceDigest'],
        modelBindings=contract['modelBindings'],
        applicationInputCount=len(contract['applicationInputs']))


def validate_dependency_edges(logs_by_role, edges, *, session_id):
    """Pair DATA_V1 publication/verified fetch logs for sealed expected edges.

    Caller binds each log to its actual Provider launch, and derives edges and
    session_id from the real sealed request plan, never from observed logs.
    This is not a physical-link or full-inference qualification verdict.
    """
    from pathlib import Path
    import hashlib
    from runtime.yolo_bundle import _bytes
    fields = {'session', 'scope', 'producer', 'consumer', 'direction', 'payload_bytes', 'planned_name', 'status'}
    edge_fields = {'scope', 'producer', 'consumer', 'planned_name'}
    if (not isinstance(edges, list) or not 0 < len(edges) <= 64
            or not isinstance(session_id, str) or not session_id or any(c.isspace() for c in session_id)):
        raise EvidenceError('DEPENDENCY_EXPECTED_CONTRACT')
    keys = []
    for edge in edges:
        if (not isinstance(edge, dict) or set(edge) != edge_fields
                or any(not isinstance(v, str) or not v or any(c.isspace() for c in v) for v in edge.values())
                or not edge['planned_name'].startswith('/') or edge['producer'] == edge['consumer']):
            raise EvidenceError('DEPENDENCY_EXPECTED_EDGE')
        key = tuple(edge[k] for k in sorted(edge_fields))
        if key in keys:
            raise EvidenceError('DEPENDENCY_EXPECTED_DUPLICATE')
        keys.append(key)
    marker = 'NDNSF_DI_DEPENDENCY_OBJECT '
    records = {}
    log_digests = {}
    for role, filename in logs_by_role.items():
        path = Path(filename)
        if any(p.is_symlink() for p in (path, *path.parents)):
            raise EvidenceError('DEPENDENCY_LOG_SYMLINK')
        payload = _bytes(path)
        log_digests[role] = 'sha256:' + hashlib.sha256(payload).hexdigest()
        for line in payload.decode('utf-8').splitlines():
            if marker not in line:
                continue
            tokens = line.split(marker, 1)[1].split()
            row = {}
            for token in tokens:
                key, sep, value = token.partition('=')
                if not sep or key in row:
                    raise EvidenceError('DEPENDENCY_LOG_FIELDS')
                row[key] = value
            if set(row) != fields:
                raise EvidenceError('DEPENDENCY_LOG_FIELDS')
            if row['session'] != session_id:
                continue
            key = tuple(row[k] for k in sorted(edge_fields))
            if key not in keys:
                raise EvidenceError('DEPENDENCY_UNPLANNED_EDGE')
            direction = row['direction']
            expected_role = row['producer'] if direction == 'publish-ndnsf-data-v1' else row['consumer']
            if (direction not in ('publish-ndnsf-data-v1', 'fetch-ndnsf-data-v1')
                    or role != expected_role or row['status'] != 'ok'
                    or not re.fullmatch(r'[1-9][0-9]{0,19}', row['payload_bytes'])):
                raise EvidenceError('DEPENDENCY_TRANSPORT_OR_OWNER')
            pair = (key, direction)
            if pair in records:
                raise EvidenceError('DEPENDENCY_DUPLICATE_OBSERVATION')
            if int(row['payload_bytes']) >= 2**64:
                raise EvidenceError('DEPENDENCY_BYTES_RANGE')
            records[pair] = int(row['payload_bytes'])
    for key in keys:
        published = records.get((key, 'publish-ndnsf-data-v1'))
        fetched = records.get((key, 'fetch-ndnsf-data-v1'))
        if published is None or published != fetched:
            raise EvidenceError('DEPENDENCY_PAIR_MISSING_OR_BYTES')
    return dict(edgeCount=len(keys), logDigests=log_digests, qualification='DEPENDENCY_COMPONENT_ONLY')


def collect_owned_dependency_result(workers, path, *, request_id, attempt, plan_digest, providers_by_role):
    """Join all four YOLO roles using closed prepared Workers' actual logs.

    Still a component gate: allocation/GPU/certified graph, numerical result
    and final cleanup receipts must also pass before experiment qualification.
    Workers are live launcher ownership objects, not caller-provided log paths.
    """
    expected = {'BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'}
    if (set(providers_by_role) != expected or not isinstance(workers, (list, tuple))
            or not 1 <= len(workers) <= 2):
        raise EvidenceError('OWNED_DEPENDENCY_SCOPE')
    logs, observations, ranks, modes = {}, {}, set(), set()
    for worker in workers:
        if (worker.closed is not True or worker.rank in ranks
                or worker.mode not in ('local-cpu', 'single-node-gpu', 'two-node-gpu')):
            raise EvidenceError('OWNED_DEPENDENCY_WORKER')
        worker._verify_prepared_boundary()
        ranks.add(worker.rank)
        modes.add(worker.mode)
        for role in sorted(set(worker.roles) & expected):
            if role in logs:
                raise EvidenceError('OWNED_DEPENDENCY_DUPLICATE_ROLE')
            observations[role] = collect_role_execution(worker, role=role,
                provider=providers_by_role[role], request_id=request_id,
                attempt=attempt, plan_digest=plan_digest)
            logs[role] = worker.children.log_dir / (role + '.log')
    if (len(modes) != 1 or logs.keys() != expected
            or ranks != ({0, 1} if modes == {'two-node-gpu'} else {0})):
        raise EvidenceError('OWNED_DEPENDENCY_ROLE_COVERAGE')
    dependencies = collect_dependency_result(path, logs, request_id=request_id,
        attempt=attempt, plan_digest=plan_digest, providers_by_role=providers_by_role)
    if any(observations[role]['native']['logDigest'] != dependencies['logDigests'][role]
           for role in expected):
        raise EvidenceError('OWNED_DEPENDENCY_LOG_CHANGED')
    return dict(roles=observations, dependencies=dependencies,
        qualification='OWNED_DEPENDENCY_COMPONENT_ONLY')


def collect_request_result(root, reference, *, case, request_id, attempt_id,
                           candidate_id, candidate_digest, graph_digest, catalogue_digest):
    """Join one lifecycle and recomputed response, with frozen graph identity.

    Caller authenticates the frozen reference/catalogue/graph. This does not
    replace native role, GPU, dependency or cleanup qualification.
    """
    lifecycle = validate_lifecycle(root, case=case, request_id=request_id,
        attempt_id=attempt_id, candidate_id=candidate_id, candidate_digest=candidate_digest)
    graph = lifecycle['events'][3]
    if graph['graphDigest'] != graph_digest or graph['catalogueDigest'] != catalogue_digest:
        raise EvidenceError('REQUEST_GRAPH_CATALOGUE_BINDING')
    numerical = reanalyze_numerical_response(root, reference, case=case,
        request_id=lifecycle['requestId'], attempt_id=lifecycle['attemptId'],
        plan_digest=lifecycle['planDigest'], result_digest=lifecycle['resultDigest'],
        candidate_id=candidate_id, candidate_digest=candidate_digest)
    return dict(lifecycle=lifecycle, numerical=numerical,
                qualification='REQUEST_RESULT_COMPONENT_ONLY')


def collect_retained_request(nodes, reference, *, plan, request_index,
                             runtime_candidate_digest, placement_candidate_id,
                             placement_candidate_digest, graph_digest,
                             catalogue_digest, providers_by_role, allocation_expected=None):
    """Join retained lifecycle/numerical/role/dependency evidence for one request.

    Runtime candidate identity (the packaged release) is NOT the selected
    catalogue candidate identity (the model partition). Both are explicit.
    This does not replace allocation/GPU/certified graph qualification.
    """
    import hashlib
    import json
    from pathlib import Path
    requests = plan.get('requests')
    if (not isinstance(requests, list) or type(request_index) is not int
            or not 0 <= request_index < len(requests) or 0 not in nodes
            or not isinstance(requests[request_index], dict)
            or type(requests[request_index].get('index')) is not int
            or requests[request_index]['index'] != request_index):
        raise EvidenceError('RETAINED_REQUEST_INDEX')
    request_id = requests[request_index]['requestId']
    # Normal Spec183 cases disable retry/reselection; a new attempt must not
    # silently become the accepted evidence for this fixed single invocation.
    attempt = 1
    root = Path(nodes[0]['root'])/'user'/'requests'/str(request_index)
    request = collect_request_result(root, reference, case=plan['case'],
        request_id=request_id, attempt_id='attempt-1', candidate_id=placement_candidate_id,
        candidate_digest=placement_candidate_digest, graph_digest=graph_digest,
        catalogue_digest=catalogue_digest)
    lifecycle = request['lifecycle']
    events = lifecycle['events']
    def digest(value):
        return 'sha256:'+hashlib.sha256(json.dumps(value, ensure_ascii=False,
            sort_keys=True, separators=(',', ':'), allow_nan=False).encode()).hexdigest()
    if (set(providers_by_role) != {'BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'}
            or any(not isinstance(v, str) or not v for v in providers_by_role.values())):
        raise EvidenceError('RETAINED_REQUEST_PROVIDERS')
    count = len(set(providers_by_role.values()))
    if (events[8]['roleDigest'] != digest(dict(providers_by_role))
            or events[8]['providerCount'] != count or events[4]['providerCount'] != count
            or events[7]['selectedRoleCount'] != len(providers_by_role)
            or events[2]['ackCount'] < count
            or events[7]['selectionDigest'] != digest({'plan': lifecycle['planDigest'],
                                                       'ack': events[2]['ackSnapshotDigest']})):
        raise EvidenceError('RETAINED_REQUEST_SELECTION_BINDING')
    execution = collect_retained_dependencies(nodes, root/'yolo-public-assignments.json',
        plan=plan, candidate_digest=runtime_candidate_digest, providers_by_role=providers_by_role,
        request_id=lifecycle['requestId'], attempt=attempt, execution_plan_digest=lifecycle['planDigest'],
        allocation_expected=allocation_expected)
    return dict(request=request, execution=execution, requestIndex=request_index,
        qualification='RETAINED_REQUEST_COMPONENT_ONLY')


def write_worker_receipt(worker, rows):
    """Persist prepared-run ownership after normal-case service/User cleanup.

    This is not a final experiment receipt: management/probe completeness,
    inference, allocation and edges are still independently qualified.
    """
    import hashlib
    import json
    from pathlib import Path
    from runtime.identities import _credential_document
    from runtime.yolo_worker import assigned_roles
    from runtime.yolo_bundle import _bytes
    if worker._preparation_binding is None:
        raise EvidenceError('NODE_RECEIPT_PREPARATION_REQUIRED')
    worker._verify_prepared_boundary()
    plan, preparation_digest, candidate_digest = worker._preparation_binding
    if (worker.mode not in ('local-cpu', 'single-node-gpu', 'two-node-gpu')
            or plan.get('case') != worker.mode
            or worker.output != Path(plan['output']) / ('node' + str(worker.rank))
            or set(worker.roles) != set(assigned_roles(worker.mode, worker.rank))):
        raise EvidenceError('NODE_RECEIPT_RUN_SCOPE')
    cleanup = validate_worker_cleanup(worker, rows)
    services = [r['role'] for r in worker.launches if r.get('invocation') is None]
    if set(services) != set(worker.roles) - {'user'}:
        raise EvidenceError('NODE_RECEIPT_SERVICE_COVER')
    requests = plan.get('requests')
    count = 4 if worker.mode == 'two-node-gpu' else 2
    if (not isinstance(requests, list) or len(requests) != count
            or any(not isinstance(r, dict) or type(r.get('index')) is not int or r['index'] != i
                   or not isinstance(r.get('requestId'), str) or not r['requestId']
                   for i,r in enumerate(requests))
            or len({r['requestId'] for r in requests}) != count):
        raise EvidenceError('NODE_RECEIPT_REQUEST_PLAN')
    actual = {r['invocation'] for r in worker.launches
              if r.get('role') == 'user' and r.get('invocation') != 'repo-readiness'}
    expected = {str(i) for i in range(count)} if worker.rank == 0 else set()
    if actual != expected:
        raise EvidenceError('NODE_RECEIPT_REQUEST_COVER')
    def digest(value):
        return 'sha256:' + hashlib.sha256(json.dumps(value, sort_keys=True,
            separators=(',', ':'), allow_nan=False).encode()).hexdigest()
    launches = []
    for row in worker.launches:
        if (not isinstance(row.get('argv'), list) or not row['argv']
                or any(not isinstance(v, str) for v in row['argv'])):
            raise EvidenceError('NODE_RECEIPT_ARGV')
        invocation = row.get('invocation')
        nonce = row.get('launchNonce')
        if invocation is None and row['role'] in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'):
            if not isinstance(nonce, str) or re.fullmatch(r'[0-9a-f]{64}', nonce) is None:
                raise EvidenceError('NODE_RECEIPT_LAUNCH_NONCE')
        elif nonce is not None:
            raise EvidenceError('NODE_RECEIPT_UNEXPECTED_NONCE')
        tag = row['role'] + ('-' + invocation if invocation is not None else '')
        if not tag or '/' in tag or '\\' in tag or tag in ('.', '..'):
            raise EvidenceError('NODE_RECEIPT_LOG_NAME')
        relative = 'logs/' + tag + '.log'
        log = worker.output / relative
        if any(p.is_symlink() for p in (log, *log.parents)):
            raise EvidenceError('NODE_RECEIPT_LOG_SYMLINK')
        content = _bytes(log)
        if nonce is not None:
            from runtime.yolo_launch_witness import namespace_pid_from_log
            namespace_pid_from_log(content, nonce=nonce, role=row['role'])
        launches.append(dict(role=row['role'], invocation=invocation,
            pid=row['pid'], argvDigest=digest(row['argv']), logPath=relative,
            logBytes=len(content), logDigest='sha256:'+hashlib.sha256(content).hexdigest(), launchNonce=nonce))
    receipt = dict(schema='tiger-yolo-node-receipt-v3', runId=plan['runId'], case=worker.mode,
        rank=worker.rank, planDigest=digest(plan), preparationDigest=preparation_digest,
        candidateDigest=candidate_digest, launches=launches, cleanup=rows,
        cleanupSummary=cleanup, qualification='NODE_CLEANUP_COMPONENT_ONLY')
    _credential_document(worker.output / 'node-receipt.json', receipt)
    return receipt


def read_node_log_receipt(root, *, receipt_digest, plan, preparation_digest, candidate_digest, rank):
    """Bind retained node logs to a receipt digest obtained from trusted staging.

    Does not recreate a Worker or prove a receipt truthful by its own hash.
    Caller authenticates expected receipt/plan identities and separately checks
    allocation, runtime ownership provenance and execution results. Cleanup
    record semantics and frozen invocation coverage are rechecked here.
    """
    import hashlib
    import json
    from pathlib import Path
    from runtime.yolo_bundle import _bytes
    from runtime.yolo_profile import _object
    from runtime.yolo_worker import assigned_roles
    def digest(value):
        return 'sha256:'+hashlib.sha256(value).hexdigest()
    if (type(rank) is not int or rank not in (0, 1)
            or any(not isinstance(v, str) or re.fullmatch(r'sha256:[0-9a-f]{64}', v) is None
                   for v in (receipt_digest, preparation_digest, candidate_digest))):
        raise EvidenceError('NODE_LOG_EXPECTED_BINDING')
    root = Path(root)
    path = root/'node-receipt.json'
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise EvidenceError('NODE_LOG_RECEIPT_SYMLINK')
    content = _bytes(path)
    if digest(content) != receipt_digest:
        raise EvidenceError('NODE_LOG_RECEIPT_DIGEST')
    try:
        receipt = json.loads(content, object_pairs_hook=_object)
        json.dumps(receipt, allow_nan=False)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise EvidenceError('NODE_LOG_RECEIPT_JSON') from exc
    fields = {'schema', 'runId', 'case', 'rank', 'planDigest', 'preparationDigest',
              'candidateDigest', 'launches', 'cleanup', 'cleanupSummary', 'qualification'}
    if (not isinstance(receipt, dict) or set(receipt) != fields
            or receipt['schema'] != 'tiger-yolo-node-receipt-v3'
            or type(receipt['rank']) is not int or receipt['rank'] != rank
            or receipt['runId'] != plan['runId'] or receipt['case'] != plan['case']
            or receipt['planDigest'] != digest(json.dumps(plan, sort_keys=True,
                separators=(',', ':'), allow_nan=False).encode())
            or receipt['preparationDigest'] != preparation_digest
            or receipt['candidateDigest'] != candidate_digest
            or receipt['qualification'] != 'NODE_CLEANUP_COMPONENT_ONLY'
            or not isinstance(receipt['launches'], list) or not 0 < len(receipt['launches']) <= 64):
        raise EvidenceError('NODE_LOG_RECEIPT_BINDING')
    roles = set(assigned_roles(receipt['case'], rank))
    logs, services = {}, set()
    for launch in receipt['launches']:
        if (not isinstance(launch, dict) or set(launch) != {'role', 'invocation', 'pid',
                'argvDigest', 'logPath', 'logDigest', 'logBytes', 'launchNonce'}
                or launch['role'] not in roles or type(launch['pid']) is not int or launch['pid'] <= 0
                or type(launch['logBytes']) is not int or launch['logBytes'] < 0
                or (launch['invocation'] is not None and (not isinstance(launch['invocation'], str)
                    or not launch['invocation']))
                or any(not isinstance(launch[key], str) or re.fullmatch(r'sha256:[0-9a-f]{64}', launch[key]) is None
                       for key in ('argvDigest', 'logDigest'))):
            raise EvidenceError('NODE_LOG_LAUNCH')
        tag = launch['role'] + ('-'+launch['invocation'] if launch['invocation'] is not None else '')
        if '/' in tag or '\\' in tag or tag in logs or launch['logPath'] != 'logs/'+tag+'.log':
            raise EvidenceError('NODE_LOG_PATH')
        path = root/launch['logPath']
        if any(p.is_symlink() for p in (path, *path.parents)):
            raise EvidenceError('NODE_LOG_SYMLINK')
        payload = _bytes(path)
        if len(payload) != launch['logBytes'] or digest(payload) != launch['logDigest']:
            raise EvidenceError('NODE_LOG_CONTENT')
        if launch['invocation'] is None and launch['role'] in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'):
            from runtime.yolo_launch_witness import namespace_pid_from_log
            namespace_pid_from_log(payload, nonce=launch['launchNonce'], role=launch['role'])
        elif launch['launchNonce'] is not None:
            raise EvidenceError('NODE_LOG_UNEXPECTED_NONCE')
        logs[tag] = dict(launch, path=str(path))
        if launch['invocation'] is None:
            services.add(launch['role'])
    if services != roles - {'user'}:
        raise EvidenceError('NODE_LOG_SERVICE_COVERAGE')
    cleanup = validate_cleanup_records(receipt['launches'], receipt['cleanup'])
    if receipt['cleanupSummary'] != cleanup:
        raise EvidenceError('NODE_LOG_CLEANUP_SUMMARY')
    count = 4 if plan['case'] == 'two-node-gpu' else 2
    requests = plan.get('requests')
    if (not isinstance(requests, list) or len(requests) != count
            or any(not isinstance(row, dict) or type(row.get('index')) is not int or row['index'] != i
                   or not isinstance(row.get('requestId'), str) or not row['requestId']
                   for i, row in enumerate(requests))
            or len({row['requestId'] for row in requests}) != count):
        raise EvidenceError('NODE_LOG_REQUEST_PLAN')
    actual = {row['invocation'] for row in receipt['launches']
              if row['role'] == 'user' and row['invocation'] != 'repo-readiness'}
    if actual != ({str(i) for i in range(count)} if rank == 0 else set()):
        raise EvidenceError('NODE_LOG_REQUEST_COVERAGE')
    return dict(receipt=receipt, logs=logs, qualification='NODE_LOG_COMPONENT_ONLY')


def read_retained_device_binding(root, *, receipt_digest, allocation_digest, gpu_probe_digest,
                                plan, preparation_digest, candidate_digest, rank, expected):
    """Recompute allocation/probe agreement from externally trusted file hashes.

    Staging authenticates hashes and expected job/comment. These files cannot
    authenticate themselves, and this function never queries an expired job.
    """
    import hashlib
    import json
    from pathlib import Path
    from runtime.yolo_bundle import _bytes
    from runtime.yolo_profile import _object
    from runtime.yolo_allocation import validate_task_allocation
    from runtime.yolo_gpu_probe import read_probe
    if (plan['case'] not in ('single-node-gpu', 'two-node-gpu')
            or not isinstance(expected, dict)
            or set(expected) != {'job_id', 'submission_key', 'partition', 'gpu_type'}):
        raise EvidenceError('RETAINED_ALLOCATION_EXPECTED')
    node = read_node_log_receipt(root, receipt_digest=receipt_digest, plan=plan,
        preparation_digest=preparation_digest, candidate_digest=candidate_digest, rank=rank)
    root = Path(root)
    def digest(payload):
        return 'sha256:'+hashlib.sha256(payload).hexdigest()
    def canonical(value):
        return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)
    def read(name, expected_digest):
        if not isinstance(expected_digest, str) or re.fullmatch(r'sha256:[0-9a-f]{64}', expected_digest) is None:
            raise EvidenceError('RETAINED_ALLOCATION_DIGEST')
        path = root/name
        if any(p.is_symlink() for p in (path, *path.parents)):
            raise EvidenceError('RETAINED_ALLOCATION_SYMLINK')
        payload = _bytes(path)
        if digest(payload) != expected_digest:
            raise EvidenceError('RETAINED_ALLOCATION_CONTENT')
        value = json.loads(payload, object_pairs_hook=_object)
        canonical(value)
        return value
    allocation = read('slurm-allocation.json', allocation_digest)
    if (not isinstance(allocation, dict) or set(allocation) != {'schema', 'runId',
            'preparationDigest', 'candidateDigest', 'expected', 'receipt', 'task', 'sources'}
            or allocation['schema'] != 'tiger-yolo-allocation-v1' or allocation['runId'] != plan['runId']
            or allocation['preparationDigest'] != preparation_digest
            or allocation['candidateDigest'] != candidate_digest or allocation['expected'] != expected):
        raise EvidenceError('RETAINED_ALLOCATION_RUN')
    sources, task = allocation['sources'], allocation['task']
    keys = {'SLURM_JOB_ID', 'SLURM_STEP_ID', 'SLURM_NODEID', 'SLURM_PROCID', 'SLURM_LOCALID',
        'SLURM_NTASKS', 'SLURM_JOB_NUM_NODES', 'SLURM_GPUS_ON_NODE', 'SLURMD_NODENAME',
        'SLURM_STEP_GPUS', 'CUDA_VISIBLE_DEVICES'}
    if (not isinstance(sources, dict) or set(sources) != {'job', 'step', 'hosts', 'stepHosts'}
            or any(not isinstance(v, str) for v in sources.values())
            or not isinstance(task, dict) or set(task) != {'hostname', 'uid', 'environment'}
            or not isinstance(task['environment'], dict) or set(task['environment']) != keys):
        raise EvidenceError('RETAINED_ALLOCATION_SOURCES')
    observed = validate_task_allocation(sources['job'].encode(), sources['step'].encode(),
        sources['hosts'].encode(), step_hostnames=sources['stepHosts'].encode(), **expected,
        rank=rank, node_count=1 if plan['case'] == 'single-node-gpu' else 2, **task)
    if canonical(observed) != canonical(allocation['receipt']):
        raise EvidenceError('RETAINED_ALLOCATION_RECOMPUTE')
    probe = read('gpu-probe.json', gpu_probe_digest)
    role = 'BackboneNeck' if rank == 0 else 'DetectShard0'
    tag = role+'-gpu-device'
    if (not isinstance(probe, dict) or set(probe) != {'schema', 'rank', 'role', 'nonce',
            'binding', 'logPath', 'logDigest', 'allocationDigest', 'qualification'}
            or probe['schema'] != 'tiger-yolo-gpu-probe-v1' or type(probe['rank']) is not int
            or probe['rank'] != rank or probe['role'] != role
            or probe['allocationDigest'] != allocation_digest or probe['logPath'] != 'logs/'+tag+'.log'
            or probe['qualification'] != 'CUDA_VISIBILITY_COMPONENT_ONLY' or tag not in node['logs']):
        raise EvidenceError('RETAINED_GPU_PROBE_BINDING')
    launch = node['logs'][tag]
    if launch['invocation'] != 'gpu-device' or launch['logDigest'] != probe['logDigest']:
        raise EvidenceError('RETAINED_GPU_PROBE_LAUNCH')
    payload = _bytes(Path(launch['path']))
    if digest(payload) != launch['logDigest']:
        raise EvidenceError('RETAINED_GPU_PROBE_LOG_CHANGED')
    binding = read_probe(payload, nonce=probe['nonce'], visible=observed['visible'])
    if canonical(binding) != canonical(probe['binding']):
        raise EvidenceError('RETAINED_GPU_PROBE_RECOMPUTE')
    launches = node['receipt']['launches']
    probe_index = next(i for i, row in enumerate(launches) if row['logPath'] == probe['logPath'])
    if any(i < probe_index and row['invocation'] is None
           and row['role'] in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')
           for i, row in enumerate(launches)):
        raise EvidenceError('RETAINED_GPU_PROBE_ORDER')
    return dict(allocation=observed, gpuBinding=binding, uid=task['uid'],
        allocationDigest=allocation_digest, gpuProbeDigest=gpu_probe_digest,
        qualification='RETAINED_DEVICE_COMPONENT_ONLY')


def collect_retained_dependencies(nodes, public_path, *, plan, candidate_digest,
                                   providers_by_role, request_id, attempt, execution_plan_digest,
                                   allocation_expected=None):
    """Cross-process four-role join; node digests come from trusted staging.

    Each node entry has root, receiptDigest and preparationDigest. GPU cases
    additionally require trusted allocationDigest and gpuProbeDigest. Device
    bindings are recomputed from retained evidence, never accepted as inputs.
    This join checks device agreement, not allocation provenance or full PASS.
    """
    from runtime.yolo_worker import assigned_roles
    expected = {'BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'}
    if (plan['case'] not in ('local-cpu', 'single-node-gpu', 'two-node-gpu')
            or not isinstance(nodes, dict) or any(type(rank) is not int for rank in nodes)
            or set(nodes) != ({0, 1} if plan['case'] == 'two-node-gpu' else {0})
            or set(providers_by_role) != expected):
        raise EvidenceError('RETAINED_NODE_COVERAGE')
    roles, logs, devices = {}, {}, {}
    for rank, node in sorted(nodes.items()):
        fields = {'root', 'receiptDigest', 'preparationDigest'}
        if plan['case'] != 'local-cpu':
            fields.update(('allocationDigest', 'gpuProbeDigest'))
        if not isinstance(node, dict) or set(node) != fields:
            raise EvidenceError('RETAINED_NODE_ENTRY')
        if plan['case'] != 'local-cpu':
            devices[rank] = read_retained_device_binding(node['root'],
                receipt_digest=node['receiptDigest'], allocation_digest=node['allocationDigest'],
                gpu_probe_digest=node['gpuProbeDigest'], plan=plan,
                preparation_digest=node['preparationDigest'], candidate_digest=candidate_digest,
                rank=rank, expected=allocation_expected)
    if len(devices) == 2:
        first, second = devices[0], devices[1]
        if (any(first['allocation'][k] != second['allocation'][k]
                for k in ('jobId', 'stepId', 'submissionKey', 'hosts'))
                or first['allocation']['hostname'] == second['allocation']['hostname']
                or first['gpuBinding']['uuid'] == second['gpuBinding']['uuid']
                or first['uid'] != second['uid']):
            raise EvidenceError('RETAINED_ALLOCATION_CROSS_NODE')
    for rank, node in sorted(nodes.items()):
        for role in sorted(set(assigned_roles(plan['case'], rank)) & expected):
            roles[role] = collect_retained_role_execution(node['root'],
                receipt_digest=node['receiptDigest'], plan=plan,
                preparation_digest=node['preparationDigest'], candidate_digest=candidate_digest,
                rank=rank, role=role, provider=providers_by_role[role], request_id=request_id,
                attempt=attempt, execution_plan_digest=execution_plan_digest,
                gpu_binding=devices[rank]['gpuBinding'] if rank in devices and role != 'Merge' else None)
            logs[role] = roles[role]['logPath']
    dependencies = collect_dependency_result(public_path, logs, request_id=request_id,
        attempt=attempt, plan_digest=execution_plan_digest, providers_by_role=providers_by_role)
    if any(roles[role]['native']['logDigest'] != dependencies['logDigests'][role] for role in expected):
        raise EvidenceError('RETAINED_DEPENDENCY_LOG_CHANGED')
    return dict(roles=roles, dependencies=dependencies, devices=devices,
        qualification='RETAINED_DEPENDENCY_COMPONENT_ONLY')


def collect_retained_role_execution(root, *, receipt_digest, plan, preparation_digest,
                                    candidate_digest, rank, role, provider,
                                    request_id, attempt, execution_plan_digest,
                                    gpu_binding=None):
    """Offline role execution check anchored to a trusted node receipt identity.

    No reconstructed Worker is used. Provider and execution-plan bindings come
    from verified runtime Selection, independently of the node configuration.
    gpu_binding is the independently resolved allocation UUID and actual launch
    selector, never copied from the Provider observation. Its provenance is
    established by the caller. Graph and full result qualification remain.
    """
    from pathlib import Path
    node = read_node_log_receipt(root, receipt_digest=receipt_digest, plan=plan,
        preparation_digest=preparation_digest, candidate_digest=candidate_digest, rank=rank)
    if (role not in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')
            or role not in node['logs'] or node['logs'][role]['invocation'] is not None):
        raise EvidenceError('RETAINED_ROLE_OWNERSHIP')
    launch = node['logs'][role]
    runner = ('native-yolo-postprocess' if role == 'Merge' else
              'onnxruntime-cpu' if plan['case'] == 'local-cpu' else 'onnxruntime-cuda')
    native = read_native_observation(launch['path'], provider=provider, role=role,
        request_id=request_id, attempt=attempt, plan_digest=execution_plan_digest,
        pid=launch['pid'], runner_kind=runner, launch_nonce=launch['launchNonce'])
    if native['logDigest'] != launch['logDigest']:
        raise EvidenceError('RETAINED_ROLE_LOG_CHANGED')
    if runner == 'onnxruntime-cuda':
        if not isinstance(gpu_binding, dict) or set(gpu_binding) != {'uuid', 'visible'}:
            raise EvidenceError('RETAINED_ROLE_GPU_BINDING')
        device = validate_device_binding(native['observation'],
            expected_uuid=gpu_binding['uuid'], expected_visible=gpu_binding['visible'])
    else:
        if gpu_binding is not None:
            raise EvidenceError('RETAINED_ROLE_CPU_BINDING')
        device = validate_device_binding(native['observation'])
    profile = None
    if role != 'Merge':
        profile_path = resolve_role_output(Path(root)/role, native['observation'].get('providerProfilePath'))
        profile = validate_ort_profile(profile_path, native['observation'])
    return dict(native=native, profile=profile, device=device, rank=rank, logPath=launch['path'],
        receiptDigest=receipt_digest, qualification='RETAINED_ROLE_COMPONENT_ONLY')


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
    if (len(launches) != 1 or launches[0].get('startError')
            or not isinstance(launches[0].get('launchNonce'), str)
            or re.fullmatch(r'[0-9a-f]{64}', launches[0]['launchNonce']) is None):
        raise EvidenceError('ROLE_COLLECTION_LAUNCH')
    if role == 'Merge':
        runner = 'native-yolo-postprocess'
    elif worker.mode == 'local-cpu':
        runner = 'onnxruntime-cpu'
    else:
        runner = 'onnxruntime-cuda'
    native = read_native_observation(worker.children.log_dir / (role + '.log'),
        provider=provider, role=role, request_id=request_id, attempt=attempt,
        plan_digest=plan_digest, pid=launches[0].get('pid'), runner_kind=runner,
        launch_nonce=launches[0].get('launchNonce'))
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
    return validate_cleanup_records(worker.launches, rows)


def validate_cleanup_records(launches, rows):
    """Shared live/offline record semantics; does not prove OS ownership alone."""
    if not isinstance(launches, list):
        raise EvidenceError('CLEANUP_LAUNCH_INVENTORY')
    expected = {}
    for launch in launches:
        if not isinstance(launch, dict):
            raise EvidenceError('CLEANUP_LAUNCH_INCOMPLETE')
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


def read_native_observation(path, *, launch_nonce=None, **binding):
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
    host_pid = binding.get('pid')
    if launch_nonce is not None:
        from runtime.yolo_launch_witness import namespace_pid_from_log
        if type(host_pid) is not int or host_pid <= 0:
            raise EvidenceError('NATIVE_HOST_PID')
        binding['pid'] = namespace_pid_from_log(payload, nonce=launch_nonce, role=binding.get('role'))
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
    return dict(result, logDigest='sha256:' + hashlib.sha256(payload).hexdigest(), hostProcessId=host_pid)


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


def validate_device_binding(observation, *, expected_uuid=None, expected_visible=None):
    """Match native CUDA-runtime identity to independently resolved allocation.

    Expected UUID/selector must come from allocated-node preflight plus the
    actual launch, not from the observation itself. This is not a substitute
    for Slurm allocation provenance or ORT model-node execution checks.
    """
    uuid = r'GPU-[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}'
    if not isinstance(observation, dict):
        raise EvidenceError('DEVICE_OBSERVATION')
    runner = observation.get('runnerKind')
    if runner in ('onnxruntime-cpu', 'native-yolo-postprocess'):
        if (expected_uuid is not None or expected_visible is not None
                or observation.get('gpuUuid') != ''
                or observation.get('gpuUuids', []) not in ([], '')
                or observation.get('cudaVisibleDevices') != ''
                or observation.get('gpuIdentitySource', '') != ''):
            raise EvidenceError('CPU_DEVICE_EXPOSURE')
        return dict(qualification='CPU_DEVICE_COMPONENT_ONLY')
    if (runner != 'onnxruntime-cuda' or not isinstance(expected_uuid, str)
            or re.fullmatch(uuid, expected_uuid) is None
            or not isinstance(expected_visible, str)
            or re.fullmatch(r'(?:0|[1-9][0-9]*|'+uuid+r')', expected_visible) is None
            or (expected_visible.startswith('GPU-') and expected_visible.lower() != expected_uuid.lower())):
        raise EvidenceError('GPU_EXPECTED_ALLOCATION')
    expected_uuid = 'GPU-' + expected_uuid[4:].lower()
    device = observation.get('device')
    if (not isinstance(device, dict) or device.get('kind') != 'cuda' or device.get('id') != '0'
            or observation.get('gpuUuid') != expected_uuid
            or observation.get('gpuUuids') != [expected_uuid]
            or observation.get('cudaVisibleDevices') != expected_visible
            or observation.get('gpuIdentitySource') != 'cuda-runtime-pci+driver-uuid'):
        raise EvidenceError('GPU_RUNTIME_ALLOCATION_MISMATCH')
    return dict(gpuUuid=expected_uuid, qualification='GPU_IDENTITY_COMPONENT_ONLY')


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
    # These fields come from the native Selection assembly. Require their
    # presence even in component evidence; this is not an independent match
    # against the certified package (modelDigest can be a legacy plan digest).
    artifacts = row.get('artifactDigests')
    if (not isinstance(row.get('modelDigest'), str)
            or re.fullmatch(r'sha256:[0-9a-f]{64}', row['modelDigest']) is None
            or not isinstance(artifacts, dict) or set(artifacts) != {role}
            or not isinstance(artifacts[role], str)
            or re.fullmatch(r'sha256:[0-9a-f]{64}', artifacts[role]) is None):
        raise EvidenceError('NATIVE_MODEL_IDENTITY')
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
