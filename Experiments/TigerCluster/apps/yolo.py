"""Run the maintained ACK-driven YOLO User once per registered request.

This is a coordinator component, not a submission entrypoint or an inference
verdict. Caller must qualify the candidate, prepare signed material, establish
readiness and supply the independent request evidence validator first.
"""
from __future__ import annotations

from pathlib import Path
import math
import re
import json
import hashlib
import os

from runtime.baseline import PYTHON

APP_DIR = '/opt/ndnsf-di/replay/repo/examples/python/NDNSF-DistributedInference/yolo_2x2'


def configuration_for_run(template: dict, plan: dict) -> dict:
    """Bind authorization identities, retaining the frozen model graph.

    Role capabilities constrain eligible Providers; no request assignment or
    synthetic ACK is produced. The policy loader remains the schema authority.
    """
    from runtime.identities import identity_inventory
    from runtime.yolo_worker import PROVIDER_ROLES
    if plan.get('schema') != 'tiger-yolo-run-plan-v1':
        raise ValueError('YOLO_PREPARE_RUN_PLAN')
    namespace = plan['namespace']
    names = identity_inventory(namespace, plan['identities'])
    if not {'user', 'controller', 'repo', *PROVIDER_ROLES}.issubset(names):
        raise ValueError('YOLO_PREPARE_IDENTITIES')
    config = json.loads(json.dumps(template, allow_nan=False))
    services = config.get('services')
    if not isinstance(services, list):
        raise ValueError('YOLO_PREPARE_SERVICES')
    inference = [s for s in services if isinstance(s, dict)
                 and not str(s.get('name', '')).startswith('/NDNSF/DistributedRepo/')]
    if len(inference) != 1 or set(inference[0].get('roles', [])) != set(PROVIDER_ROLES):
        raise ValueError('YOLO_PREPARE_ROLE_GRAPH')
    service = inference[0]
    if not isinstance(service.get('name'), str) or not service['name'].startswith('/'):
        raise ValueError('YOLO_PREPARE_SERVICE_NAME')
    if service.get('artifacts'):
        raise ValueError('YOLO_PREPARE_LOCAL_MODEL_BYPASS')
    service['users'] = [names['user']]
    service['providers'] = [{'identity': names[role], 'roles': [role]}
                            for role in sorted(PROVIDER_ROLES)]
    config['controller'], config['group'] = names['controller'], namespace + '/sync'
    config['runtime'] = {**config.get('runtime', {}), 'user_identity': names['user'],
                         'provider_prefix': namespace, 'identities': {
                             **names, 'group': config['group']}}
    config['trust'] = {**config.get('trust', {}), 'app_roots': [namespace],
                       'anchor_file': '/config/root.cert'}
    config.pop('authorization_summary', None)  # Recomputed by maintained policy generation.
    # The maintained materializer regenerates the complete Repo service set.
    config['services'] = [service]
    return config


def prepare_in_container(plan: dict, *, template_path: Path, template_digest: str,
                         package: Path, manifest_digest: str, registry: Path,
                         registry_digest: str, authority_private: Path,
                         protection_epoch: str, candidate_id: str,
                         candidate_digest: str) -> dict:
    """Offline preparation using installed owners, never a qualification gate.

    Invoked only by the audited prepare entrypoint in the exact SIF. Inputs
    are already candidate-bound and read-only. /config and /identities are
    private, initially empty writable preparation mounts; runtime gets only
    public config and each role's own HOME. No NFD, RPC or model execution is
    started here. A failure retains partial output and prohibits in-place retry.
    """
    import importlib.util
    import sys
    from types import SimpleNamespace
    from runtime.identities import (issue, issue_yolo_recipients, issue_yolo_offers,
        install_yolo_trust, _read_credential, _create_credential, _credential_document)
    from runtime.yolo_profile import _read_plane

    public, private = Path('/config'), Path('/identities')
    for root in (public, private):
        if root.is_symlink() or not root.is_dir() or any(root.iterdir()):
            raise ValueError('YOLO_PREPARE_OUTPUT_NOT_EMPTY')
    template_wire = _read_credential(Path(template_path))
    if 'sha256:' + hashlib.sha256(template_wire).hexdigest() != template_digest:
        raise ValueError('YOLO_PREPARE_TEMPLATE_DIGEST')
    if 'sha256:' + hashlib.sha256(_read_credential(Path(registry))).hexdigest() != registry_digest:
        raise ValueError('YOLO_PREPARE_REGISTRY_DIGEST')
    manifest_path = Path(package) / 'manifest.json'
    # Manifest may exceed the small credential limit; reuse the bounded plane reader.
    from runtime.baseline import digest
    manifest = _read_plane(manifest_path)
    if 'sha256:' + digest(manifest_path) != manifest_digest:
        raise ValueError('YOLO_PREPARE_MANIFEST_DIGEST')
    config = configuration_for_run(_read_plane(template_path), plan)
    # Only the installed source owner is imported, never a host checkout.
    runner_path = Path(APP_DIR).parents[3] / 'Experiments/NDNSF_DI_YoloAckDriven_Minindn.py'
    module_name = '_spec183_installed_yolo_owner'
    spec = importlib.util.spec_from_file_location(module_name, runner_path)
    owner = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = owner
    spec.loader.exec_module(owner)
    from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter
    from ndnsf_distributed_inference.policy import write_policy_bundle
    build_yolo26n_adapter(package, registry_path=registry)  # Signed catalogue and actual graph digest.
    owner._validate_policy_loader_compatibility(config)

    namespace, names = plan['namespace'], plan['identities']
    issue(namespace, names)
    homes = {role: private / role for role in names}
    install_yolo_trust(registry, expected_registry_digest=registry_digest,
        authority_private=authority_private, user_home=homes['user'], public=public,
        protection_epoch=protection_epoch)
    issue_yolo_recipients(namespace, homes, public, names)
    service = config['services'][0]['name']
    issue_yolo_offers(namespace, homes, public, names, service=service,
        candidate_id=candidate_id, candidate_digest=candidate_digest,
        trust_schema=namespace + '/trust')
    _create_credential(homes['user'] / 'request-envelope.key', os.urandom(32))
    policy_path = owner._materialize_case_config('Y-B', config, public)
    _create_credential(public / 'case.json', policy_path.read_bytes())
    write_policy_bundle(public / 'case.json', public)
    catalogue_name = names['controller'] + '/NDNSF/DI/catalogue/v1'
    owner.build_runtime_publication_file(
        SimpleNamespace(case='Y-B', output=public, identities={'controller': names['controller']}),
        {'package': Path(package), 'manifest': manifest,
         'registry': public / 'contracts/trust-root-registry-v1.json',
         'descriptor': {'catalogueDataName': catalogue_name, 'catalogueSigner': names['controller']}})
    receipt = {'schema': 'tiger-yolo-preparation-v1', 'status': 'PREPARED',
               'qualification': 'NOT_EVALUATED', 'runId': plan['runId'],
               'candidateDigest': candidate_digest, 'protectionEpoch': protection_epoch,
               'catalogueDataName': catalogue_name, 'catalogueSigner': names['controller'],
               'templateDigest': template_digest, 'packageManifestDigest': manifest_digest,
               'registryDigest': registry_digest}
    _credential_document(public / 'preparation.json', receipt)
    return receipt


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
        '--spec180-runtime-publication-file', '/config/runtime-publication.json',
        '--spec180-runtime-receipt-file', '/output/runtime-publication-receipt.json'])


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
                 protection_epoch: str,
                 accept_request, peer_failure: Path | None = None):
    """Stop on the first process/evidence failure, keeping Providers alive.

    ``accept_request(request, output)`` must raise on incomplete/invalid
    evidence. Exit zero alone never accepts a request. Physical source/SIF,
    policy, catalogue and model validation belongs to the preceding gates.
    """
    if not callable(accept_request):
        raise ValueError('YOLO_RESULT_VALIDATOR_REQUIRED')
    if (not isinstance(protection_epoch, str) or protection_epoch == 'plaintext-v1'
            or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', protection_epoch)):
        raise ValueError('YOLO_PROTECTED_EPOCH_REQUIRED')
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
    for name in ('case.json', 'contracts/trust-root-registry-v1.json', 'contracts/authority.pub', 'offer-trust-root.json',
                 'offer-public-key-map.json', 'recipient-public-keys.json'):
        path = worker.public / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('YOLO_USER_INPUT:' + name)
    for name in ('request-envelope.key', 'requester.key', 'authority/artifact-policy-authority.key'):
        key = worker.homes['user'] / name
        if any(p.is_symlink() for p in (key, *key.parents)) or not key.is_file():
            raise ValueError('YOLO_USER_PRIVATE_INPUT')
    for request in requests:
        i = str(request['index'])
        output = '/output/requests/' + i
        argv = ['/usr/bin/env', 'SPEC181_PROTECTION_EPOCH=' + protection_epoch,
                'NDNSF_DI_RECIPIENT_PUBLIC_KEY_MAP=/config/recipient-public-keys.json',
                'SPEC181_REQUESTER_PRIVATE_KEY=/identities/user/requester.key',
                'NDNSF_SPEC180_CONFIG_ROOT=/identities/user/authority',
                PYTHON, APP_DIR + '/user.py', '--config', '/config/case.json',
                '--generated-policy-dir', output + '/generated-policy',
                '--canonical-package', '/artifacts',
                '--catalogue-registry', '/config/contracts/trust-root-registry-v1.json',
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
