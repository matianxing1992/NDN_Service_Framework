"""Run the maintained ACK-driven YOLO User once per registered request.

This is a coordinator component, not a submission entrypoint or an inference
verdict. Caller must qualify the candidate, prepare signed material, establish
readiness and supply the independent request evidence validator first.
"""
from __future__ import annotations

from pathlib import Path
import math

from runtime.baseline import PYTHON

APP_DIR = '/opt/ndnsf-di/replay/repo/examples/python/NDNSF-DistributedInference/yolo_2x2'


def _control_config(worker, role):
    from runtime.yolo_profile import _read_plane
    if worker.rank != 0 or role not in worker.roles or worker.closed:
        raise ValueError('YOLO_CONTROL_ROLE')
    # Reuse the bounded, duplicate-key-rejecting JSON reader. This verifies
    # syntax/path safety only; candidate hashes and policy authorization are
    # still established by the coordinator before any launch.
    config = _read_plane(worker.public / 'case.json')
    if not isinstance(config.get('runtime'), dict):
        raise ValueError('YOLO_CONTROL_CONFIG')
    return config


def start_controller(worker):
    """Start the existing signing/publishing Controller; not a READY verdict."""
    _control_config(worker, 'controller')
    from runtime.yolo_profile import _read_plane
    _read_plane(worker.public / 'runtime-publication.json')
    return worker.start_service('controller', [
        PYTHON, APP_DIR + '/controller.py', '--config', '/config/case.json',
        '--generated-policy-dir', '/output/generated-policy',
        '--spec180-runtime-publication-file', '/config/runtime-publication.json'])


def start_repo(worker, *, identity: str, free_bytes: int):
    """Start the real Repo with its policy-derived identity and local store.

    Caller supplies verified writable capacity from allocation preflight.
    The advertised value is not a capacity measurement made by this helper.
    """
    config = _control_config(worker, 'repo')
    prefix = config['runtime'].get('provider_prefix')
    from runtime.identities import identity_inventory
    identities = identity_inventory(prefix, {'repo': identity})
    if identities['repo'] != prefix + '/repo':
        raise ValueError('YOLO_REPO_IDENTITY')
    if type(free_bytes) is not int or free_bytes <= 0 or free_bytes > 2**63 - 1:
        raise ValueError('YOLO_REPO_CAPACITY')
    return worker.start_service('repo', [
        PYTHON, APP_DIR + '/repo_node.py', '--config', '/config/case.json',
        '--generated-policy-dir', '/output/generated-policy',
        '--provider-id', 'repo', '--repo-node', identity,
        '--storage-dir', '/output/repo-store', '--free-bytes', str(free_bytes),
        '--memory-cache-bytes', str(64 * 1024 * 1024), '--preallocate-bytes', '0',
        '--failure-domain', 'node0', '--handler-threads', '1', '--ack-threads', '1'])


def run_requests(worker, plan: dict, *, package: Path, catalog_data_name: str,
                 catalog_signer: str, permission_wait_ms: int,
                 request_deadline_ms: int, process_timeout_seconds: float,
                 accept_request, peer_failure: Path | None = None):
    """Stop on the first process/evidence failure, keeping Providers alive.

    ``accept_request(request, output)`` must raise on incomplete/invalid
    evidence. Exit zero alone never accepts a request. Physical source/SIF,
    policy, catalogue and model validation belongs to the preceding gates.
    """
    if not callable(accept_request):
        raise ValueError('YOLO_RESULT_VALIDATOR_REQUIRED')
    if (type(permission_wait_ms) is not int or not 1 <= permission_wait_ms <= 120000
            or type(request_deadline_ms) is not int or not 1500 < request_deadline_ms <= 60000
            or isinstance(process_timeout_seconds, bool)
            or not isinstance(process_timeout_seconds, (int, float))
            or not math.isfinite(process_timeout_seconds)
            or process_timeout_seconds < (permission_wait_ms + request_deadline_ms) / 1000):
        raise ValueError('YOLO_USER_BUDGET')
    for value in (catalog_data_name, catalog_signer):
        if (not isinstance(value, str) or not value.startswith('/') or value == '/'
                or any(ord(c) < 33 or ord(c) == 127 for c in value)):
            raise ValueError('YOLO_CATALOG_NAME')
    if plan.get('case') not in ('local-cpu', 'single-node-gpu', 'two-node-gpu'):
        # Negative-dependency needs a separately wired post-Selection fault
        # owner and validator; do not run it as a nominal success schedule.
        raise ValueError('YOLO_NORMAL_CASE')
    if worker.mode != plan['case'] or worker.rank != 0:
        raise ValueError('YOLO_WORKER_CASE')
    requests = plan.get('requests')
    expected = 4 if plan['case'] == 'two-node-gpu' else 2
    if not isinstance(requests, list) or len(requests) != expected:
        raise ValueError('YOLO_REQUEST_COUNT')
    seen = set()
    for i, request in enumerate(requests):
        if (not isinstance(request, dict) or set(request) != {'index', 'warmup', 'requestId', 'output'}
                or type(request['index']) is not int or request['index'] != i
                or type(request['warmup']) is not bool or request['warmup'] != (i == 0)):
            raise ValueError('YOLO_REQUEST_SCHEDULE')
        identity = request['requestId']
        if (not isinstance(identity, str) or not identity.startswith('/') or identity == '/'
                or any(ord(c) < 33 or ord(c) == 127 for c in identity) or identity in seen):
            raise ValueError('YOLO_REQUEST_ID')
        seen.add(identity)
        if request['output'] != str(worker.output / 'user' / 'requests' / str(i)):
            raise ValueError('YOLO_REQUEST_OUTPUT')
    for name in ('case.json', 'catalogue-registry.json', 'offer-trust-root.json',
                 'offer-public-key-map.json'):
        path = worker.public / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('YOLO_USER_INPUT:' + name)
    key = worker.homes['user'] / 'request-envelope.key'
    if key.is_symlink() or not key.is_file():
        raise ValueError('YOLO_USER_ENVELOPE_KEY')
    for request in requests:
        i = str(request['index'])
        output = '/output/requests/' + i
        argv = [PYTHON, APP_DIR + '/user.py', '--config', '/config/case.json',
                '--generated-policy-dir', output + '/generated-policy',
                '--canonical-package', '/artifacts',
                '--catalogue-registry', '/config/catalogue-registry.json',
                '--offer-trust-root', '/config/offer-trust-root.json',
                '--offer-public-key-map', '/config/offer-public-key-map.json',
                '--catalog-data-name', catalog_data_name, '--catalog-signer', catalog_signer,
                '--ack-timeout-ms', '1500', '--timeout-ms', str(request_deadline_ms),
                '--permission-wait-ms', str(permission_wait_ms),
                '--native-tensor-input', '--input-size', '640',
                '--request-id', request['requestId'], '--lifecycle-output-dir', output,
                '--lifecycle-case', plan['case'],
                '--envelope-key-file', '/identities/user/request-envelope.key']
        worker.run_user(i, argv, package=package, seconds=process_timeout_seconds,
                        peer_failure=peer_failure)
        accept_request(request, Path(request['output']))
