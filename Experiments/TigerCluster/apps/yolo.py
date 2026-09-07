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


def _installed_yolo_owner():
    import importlib.util
    import sys
    path = Path(APP_DIR).parents[3] / 'Experiments/NDNSF_DI_YoloAckDriven_Minindn.py'
    name = '_spec183_installed_yolo_owner'
    spec = importlib.util.spec_from_file_location(name, path)
    owner = importlib.util.module_from_spec(spec)
    sys.modules[name] = owner
    spec.loader.exec_module(owner)
    return owner


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
    # The run namespace names this application instance, not a Provider.
    # Older plan fixtures omit the explicit alias; prepared runtime always
    # records it, and downstream routing must consume the resulting group.
    app_name = plan.get('applicationName', namespace)
    if app_name != namespace:
        identity_inventory(namespace, {'application': app_name})
    config['controller'], config['group'] = names['controller'], app_name + '/sync'
    config['runtime'] = {**config.get('runtime', {}), 'user_identity': names['user'],
                         'application_name': app_name, 'provider_prefix': namespace, 'identities': {
                             **names, 'group': config['group']}}
    config['trust'] = {**config.get('trust', {}), 'app_roots': [namespace],
                       'anchor_file': '/config/root.cert'}
    config.pop('authorization_summary', None)  # Recomputed by maintained policy generation.
    # The maintained materializer regenerates the complete Repo service set.
    config['services'] = [service]
    return config


def validate_preparation_roots(public: Path, private: Path) -> None:
    """Apptainer may pre-create the empty issuer HOME for --home."""
    for root in (public, private):
        if any(p.is_symlink() for p in (root, *root.parents)) or not root.is_dir():
            raise ValueError('YOLO_PREPARE_OUTPUT_NOT_EMPTY')
    if any(public.iterdir()):
        raise ValueError('YOLO_PREPARE_OUTPUT_NOT_EMPTY')
    children = list(private.iterdir())
    if children and (children != [private / 'root'] or children[0].is_symlink()
                     or not children[0].is_dir() or any(children[0].iterdir())):
        raise ValueError('YOLO_PREPARE_OUTPUT_NOT_EMPTY')


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
    from types import SimpleNamespace
    from runtime.identities import (issue, issue_yolo_recipients, issue_yolo_offers,
        install_yolo_trust, _read_credential, _create_credential, _credential_document)
    from runtime.yolo_profile import _read_plane

    public, private = Path('/config'), Path('/identities')
    validate_preparation_roots(public, private)
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
    owner = _installed_yolo_owner()
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
    from runtime.yolo_bundle import preparation_inventory
    receipt = {'schema': 'tiger-yolo-preparation-v1', 'status': 'PREPARED',
               'qualification': 'NOT_EVALUATED', 'runId': plan['runId'],
               'candidateDigest': candidate_digest, 'protectionEpoch': protection_epoch,
               'catalogueDataName': catalogue_name, 'catalogueSigner': names['controller'],
               'templateDigest': template_digest, 'packageManifestDigest': manifest_digest,
               'registryDigest': registry_digest,
               'publicFiles': preparation_inventory(public, plan)}
    _credential_document(public / 'preparation.json', receipt)
    return receipt


def preparation_arguments(descriptor: Path, expected_digest: str) -> dict:
    """Decode a pinned internal prepare descriptor with fixed mount paths."""
    from runtime.identities import _read_credential
    from runtime.yolo_profile import _object
    wire = _read_credential(Path(descriptor))
    if 'sha256:' + hashlib.sha256(wire).hexdigest() != expected_digest:
        raise ValueError('YOLO_PREPARE_DESCRIPTOR_DIGEST')
    value = json.loads(wire, object_pairs_hook=_object)
    fields = {'schema', 'plan', 'templateDigest', 'manifestDigest', 'registryDigest',
              'protectionEpoch', 'candidateId', 'candidateDigest'}
    if not isinstance(value, dict) or set(value) != fields or value['schema'] != 'tiger-yolo-prepare-input-v1':
        raise ValueError('YOLO_PREPARE_DESCRIPTOR')
    for key in ('templateDigest', 'manifestDigest', 'registryDigest', 'candidateDigest'):
        if not isinstance(value[key], str) or not re.fullmatch(r'sha256:[0-9a-f]{64}', value[key]):
            raise ValueError('YOLO_PREPARE_DESCRIPTOR_HASH')
    if (not isinstance(value['plan'], dict)
            or value['plan'].get('schema') != 'tiger-yolo-run-plan-v1'
            or not isinstance(value['candidateId'], str)
            or not re.fullmatch(r'[A-Za-z0-9_.-]{1,128}', value['candidateId'])
            or not isinstance(value['protectionEpoch'], str)
            or value['protectionEpoch'] == 'plaintext-v1'
            or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', value['protectionEpoch'])):
        raise ValueError('YOLO_PREPARE_DESCRIPTOR_BINDING')
    return dict(plan=value['plan'], template_path=Path('/inputs/template.json'),
        template_digest=value['templateDigest'], package=Path('/artifacts'),
        manifest_digest=value['manifestDigest'],
        registry=Path('/inputs/trust/contracts/trust-root-registry-v1.json'),
        registry_digest=value['registryDigest'],
        authority_private=Path('/inputs/private/artifact-policy-authority.key'),
        protection_epoch=value['protectionEpoch'], candidate_id=value['candidateId'],
        candidate_digest=value['candidateDigest'])


def main(argv=None):
    """Internal container command. Operator qualification belongs to submit.py."""
    import argparse
    parser = argparse.ArgumentParser(description=main.__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    prepare = commands.add_parser('prepare')
    prepare.add_argument('--descriptor', type=Path, required=True)
    prepare.add_argument('--descriptor-sha256', required=True)
    probe = commands.add_parser('repo-probe')
    probe.add_argument('--probe-id', required=True)
    probe.add_argument('--seconds', type=float, required=True)
    args = parser.parse_args(argv)
    try:
        if args.action == 'prepare':
            options = preparation_arguments(args.descriptor, args.descriptor_sha256)
            receipt = prepare_in_container(**options)
        else:
            receipt = probe_repo_in_container(args.probe_id, args.seconds)
    except Exception as exc:
        # Never print exception values: private input paths or credentials may
        # occur in errors from imported libraries. Detailed diagnosis is local.
        import traceback
        frames = [{'file': Path(frame.filename).name, 'line': frame.lineno, 'function': frame.name}
                  for frame in traceback.extract_tb(exc.__traceback__)[-8:]]
        print(json.dumps({'status': 'FAILED', 'qualification': 'NOT_EVALUATED',
                          'errorType': type(exc).__name__, 'frames': frames}, sort_keys=True))
        return 2
    print(json.dumps(receipt, sort_keys=True))
    return 0


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


def wait_controller_publication(worker, *, seconds: float, peer_failure: Path | None = None):
    """Wait for complete signed/readback publication, then match its receipt.

    Requires the prepared Worker factory. The marker is only a completion
    fence for the receipt write; the shared validator checks its actual rows.
    This does not prove remote-node reachability or Repo readiness.
    """
    from runtime.yolo_profile import _read_plane
    if worker.rank != 0 or worker._preparation_binding is None:
        raise ValueError('YOLO_PUBLICATION_PREPARATION_REQUIRED')
    worker._verify_prepared_boundary()
    worker.wait_marker('controller', 'SPEC180_RUNTIME_CATALOGUE_PUBLISHED',
                       seconds=seconds, peer_failure=peer_failure)
    worker._verify_prepared_boundary()
    expected = _read_plane(worker.public / 'runtime-publication.json')
    receipt_path = worker.output / 'controller/runtime-publication-receipt.json'
    if any(p.is_symlink() for p in (receipt_path, *receipt_path.parents)):
        raise ValueError('YOLO_PUBLICATION_RECEIPT_SYMLINK')
    receipt = _read_plane(receipt_path)
    from runtime.yolo_result import validate_runtime_publication_receipt
    result = validate_runtime_publication_receipt(expected, receipt)
    worker.check()
    return result


def wait_provider_ready(worker, role: str, *, seconds: float,
                         peer_failure: Path | None = None) -> None:
    """Native permission/runtime readiness, NOT model assembly or GPU proof."""
    from runtime.yolo_worker import PROVIDER_ROLES
    if worker._preparation_binding is None or role not in PROVIDER_ROLES or role not in worker.roles:
        raise ValueError('YOLO_PROVIDER_PREPARATION_REQUIRED')
    worker._verify_prepared_boundary()
    identity = worker._preparation_binding[0]['identities'][role]
    # Native source emits this only after hasProviderPermissionForService()
    # succeeds and provisioningState->markReady(). Single-role launch is fixed.
    marker = 'NDNSF_DI_NATIVE_PROVIDER_READY provider=' + identity + ' activeRoles=1\n'
    worker.wait_marker(role, marker, seconds=seconds, peer_failure=peer_failure)
    worker._verify_prepared_boundary()
    worker.check()


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


def probe_repo_in_container(probe_id: str, seconds: float):
    """Actual normal Repo RPC in a finite User process; not an inference gate.

    The outer Worker owns a hard process deadline and the sole User HOME lease.
    Probe output is exclusive and separate from warmup/measured request evidence.
    """
    import time
    from runtime.yolo_profile import _read_plane
    from runtime.identities import _credential_document, identity_inventory
    if (not isinstance(probe_id, str) or not re.fullmatch(r'[a-f0-9]{32}', probe_id)
            or isinstance(seconds, bool) or not isinstance(seconds, (int, float))
            or not math.isfinite(seconds) or not 0 < seconds <= 300):
        raise ValueError('YOLO_REPO_PROBE_ARGUMENTS')
    target = Path('/output/requests/repo-readiness/receipt.json')
    if target.exists() or any(p.is_symlink() for p in (target, *target.parents)):
        raise ValueError('YOLO_REPO_PROBE_OUTPUT')
    config = _read_plane(Path('/config/case.json'))
    runtime = config['runtime']
    names = identity_inventory(runtime['provider_prefix'], runtime['identities'])
    if (config['controller'] != names['controller'] or runtime['user_identity'] != names['user']
            or config['group'] != runtime['application_name'] + '/sync'
            or config['trust']['anchor_file'] != '/config/root.cert'):
        raise ValueError('YOLO_REPO_PROBE_IDENTITIES')
    # Imports intentionally remain inside the container-only command.
    from ndnsf import ServiceUser
    from py_repoclient.orchestration import NetworkDistributedRepoClient
    deadline = time.monotonic() + seconds
    user = ServiceUser(group=config['group'], controller=names['controller'], user=names['user'],
        trust_schema='/config/trust-schema.conf', permission_wait_ms=min(6000, max(1, int(seconds * 1000))),
        adaptive_admission=False, handler_threads=1, ack_threads=1)
    repo = None
    attempts = 0
    try:
        user.start()
        repo = NetworkDistributedRepoClient(user=user, service_name='/NDNSF/DistributedRepo',
            upload_prefix=names['user'] + '/NDNSF-DISTRIBUTED-REPO/UPLOAD',
            ack_timeout_ms=500, timeout_ms=3000, control_mode='normal', enable_targeted_fallback=False)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('YOLO_REPO_PROBE_DEADLINE')
            attempts += 1
            try:
                capability = repo.capability(timeout_ms=max(1, min(3000, int(remaining * 1000))))
            except (RuntimeError, TimeoutError):
                time.sleep(max(0, min(0.2, deadline - time.monotonic())))
                continue
            if not isinstance(capability, dict) or capability.get('repoNode') != names['repo']:
                raise ValueError('YOLO_REPO_PROBE_WRONG_REPO')
            if time.monotonic() >= deadline:
                raise TimeoutError('YOLO_REPO_PROBE_DEADLINE')
            break
    finally:
        try:
            if repo is not None:
                repo.close()
        finally:
            user.stop()
    receipt = dict(schema='tiger-yolo-repo-readiness-v1', probeId=probe_id,
                   user=names['user'], repo=names['repo'], attempts=attempts,
                   status='READY', qualification='NOT_EVALUATED', capability=capability)
    _credential_document(target, receipt)
    return receipt


def wait_repo_ready(worker, *, seconds: float, peer_failure: Path | None = None):
    """Consume a fresh finite capability probe; no model mount or stored PASS reuse."""
    import secrets
    from runtime.yolo_profile import _read_plane
    if (worker.rank != 0 or worker._preparation_binding is None
            or isinstance(seconds, bool) or not isinstance(seconds, (int, float))
            or not math.isfinite(seconds) or not 0 < seconds <= 300):
        raise ValueError('YOLO_REPO_PREPARATION_OR_BUDGET')
    worker._verify_prepared_boundary()
    names = worker._preparation_binding[0]['identities']
    probe_id = secrets.token_hex(16)
    rc = worker.run_user('repo-readiness', [PYTHON, '-m', 'apps.yolo', 'repo-probe',
        '--probe-id', probe_id, '--seconds', str(seconds)], package=None,
        seconds=seconds + worker.cleanup_seconds, peer_failure=peer_failure)
    if rc != 0:
        raise RuntimeError('YOLO_REPO_PROBE_EXIT')
    receipt_path = worker.output / 'user/requests/repo-readiness/receipt.json'
    if any(p.is_symlink() for p in (receipt_path, *receipt_path.parents)):
        raise ValueError('YOLO_REPO_PROBE_OUTPUT')
    receipt = _read_plane(receipt_path)
    expected = dict(schema='tiger-yolo-repo-readiness-v1', probeId=probe_id,
                    user=names['user'], repo=names['repo'], status='READY', qualification='NOT_EVALUATED')
    if (any(receipt.get(k) != v for k, v in expected.items())
            or type(receipt.get('attempts')) is not int or receipt['attempts'] <= 0
            or not isinstance(receipt.get('capability'), dict)
            or receipt['capability'].get('repoNode') != names['repo']):
        raise ValueError('YOLO_REPO_PROBE_RECEIPT')
    worker._verify_prepared_boundary()
    worker.check()
    return receipt


def wait_network_ready(worker, *, probe_id: str, seconds: float,
                       peer_failure: Path | None = None):
    """Run on both ranks concurrently; caller must accept BOTH fresh receipts."""
    from runtime.yolo_profile import _read_plane
    if (worker._preparation_binding is None or worker.mode not in ('two-node-gpu', 'negative-dependency')
            or worker.rank not in (0, 1) or not isinstance(probe_id, str)
            or not re.fullmatch(r'[a-f0-9]{32}', probe_id)
            or isinstance(seconds, bool) or not isinstance(seconds, (int, float))
            or not math.isfinite(seconds) or not 0 < seconds <= 120):
        raise ValueError('YOLO_NETWORK_PROBE_SCOPE')
    worker._verify_prepared_boundary()
    plan = worker._preparation_binding[0]
    roles = ('BackboneNeck', 'DetectShard0')
    own, peer = roles[worker.rank], roles[1 - worker.rank]
    expected = dict(schema='tiger-yolo-network-readiness-v1', probeId=probe_id,
        producer=plan['identities'][own], peer=plan['identities'][peer],
        receivedName=plan['identities'][peer] + '/SPEC183-NETWORK/' + probe_id + '/data',
        status='READY', qualification='NOT_EVALUATED')
    if worker.run_network_probe([PYTHON, '-m', 'apps.yolo_network',
        '--namespace', plan['namespace'], '--role', own, '--probe-id', probe_id,
        '--identity', plan['identities'][own], '--peer-identity', plan['identities'][peer],
        '--seconds', str(seconds)], seconds=seconds + worker.cleanup_seconds,
        peer_failure=peer_failure) != 0:
        raise RuntimeError('YOLO_NETWORK_PROBE_EXIT')
    path = worker.output / own / 'requests/network-readiness/receipt.json'
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError('YOLO_NETWORK_PROBE_OUTPUT')
    receipt = _read_plane(path)
    if receipt != expected:
        raise ValueError('YOLO_NETWORK_PROBE_RECEIPT')
    worker._verify_prepared_boundary()
    worker.check()
    return receipt


def configure_network(worker, barrier, *, endpoints: list[dict]):
    """Start exact-SIF NFD and perform owned nfdc setup before readiness probes.

    Endpoints come from the verified allocation, not profile-invented hostnames.
    Both ranks call this concurrently. This establishes command success and
    local management access; signed peer traffic is still checked afterwards.
    """
    import ipaddress
    import stat
    import time
    from runtime.baseline import route_commands
    from runtime.identities import _credential_document, identity_inventory
    from runtime.yolo_profile import _read_plane
    from runtime.yolo_worker import StartupBarrier
    if worker._preparation_binding is None or not isinstance(barrier, StartupBarrier):
        raise ValueError('YOLO_NETWORK_PREPARATION')
    plan, _, candidate = worker._preparation_binding
    if (barrier.rank != worker.rank or barrier.binding['runId'] != plan['runId']
            or barrier.binding['candidateDigest'] != candidate
            or barrier.ranks != tuple(n['rank'] for n in plan['nodes'])):
        raise ValueError('YOLO_NETWORK_BINDING')
    if not isinstance(endpoints, list) or len(endpoints) != len(barrier.ranks):
        raise ValueError('YOLO_NETWORK_ENDPOINTS')
    peers = {}
    port = plan['effectiveBehavior']['profile']['cluster']['tcpPort']
    for node in endpoints:
        if (not isinstance(node, dict) or set(node) != {'rank', 'address', 'port'}
                or type(node['rank']) is not int or node['rank'] not in barrier.ranks
                or node['rank'] in peers or type(node['port']) is not int
                or node['port'] != port or not 1024 <= node['port'] <= 65535
                or not isinstance(node['address'], str)):
            raise ValueError('YOLO_NETWORK_ENDPOINTS')
        address = ipaddress.IPv4Address(node['address'])
        if address.is_unspecified or address.is_multicast or (len(barrier.ranks) == 2 and address.is_loopback):
            raise ValueError('YOLO_NETWORK_ADDRESS')
        peers[node['rank']] = dict(node)
    if len({p['address'] for p in peers.values()}) != len(peers):
        raise ValueError('YOLO_NETWORK_DUPLICATE_NODE')
    config = _read_plane(worker.public / 'case.json')
    group = plan['applicationName'] + '/sync'
    if config['group'] != group or config['runtime']['application_name'] != plan['applicationName']:
        raise ValueError('YOLO_NETWORK_SYNC_NAME')
    identity_inventory(plan['namespace'], {'sync': group})
    worker._verify_prepared_boundary()
    commands = []
    try:
        peer_failure = (barrier.directory / ('failed-' + str(1 - worker.rank) + '.json')
                        if len(peers) == 2 else None)
        barrier.remaining()
        worker.start_forwarder(port=port)
        socket_path = worker.node / 'nfd.sock'
        while True:
            remaining = barrier.remaining()
            if socket_path.is_symlink():
                raise ValueError('YOLO_NFD_SOCKET_SYMLINK')
            if socket_path.exists():
                if not stat.S_ISSOCK(socket_path.stat().st_mode):
                    raise ValueError('YOLO_NFD_SOCKET_TYPE')
                break
            time.sleep(min(0.1, remaining))
        def execute(arguments):
            seconds = min(20, barrier.remaining() - worker.cleanup_seconds)
            if seconds <= 0:
                raise TimeoutError('YOLO_NETWORK_COMMAND_BUDGET')
            invocation = 'nfd-command-' + str(len(commands))
            rc = worker.run_management(invocation, arguments, seconds=seconds, peer_failure=peer_failure)
            commands.append({'invocation': invocation, 'arguments': arguments, 'exitCode': rc})
            if rc != 0:
                raise RuntimeError('YOLO_NETWORK_COMMAND_EXIT')
        execute(['status', 'report'])
        barrier.publish('nfd-ready', peers[worker.rank])
        observed = barrier.wait('nfd-ready')
        if observed != peers:
            raise ValueError('YOLO_NETWORK_PEER_BINDING')
        if len(peers) == 2:
            peer = peers[1 - worker.rank]
            calls = route_commands(plan['namespace'], peer['address'], peer['port'], sync_prefix=group)
        else:
            calls = [['strategy', 'set', 'prefix', group, 'strategy', '/localhost/nfd/strategy/multicast'],
                     ['face', 'list'], ['route', 'list']]
        for arguments in calls:
            execute(arguments)
        _credential_document(worker.output / 'network-setup.json', {
            'schema': 'tiger-yolo-network-setup-v1', 'runId': plan['runId'], 'rank': worker.rank,
            'candidateDigest': candidate, 'endpoint': peers[worker.rank], 'syncPrefix': group,
            'commands': commands, 'qualification': 'NOT_EVALUATED'})
        barrier.publish('routes-ready', {'namespace': plan['namespace'], 'syncPrefix': group})
        return commands
    except BaseException as exc:
        try:
            barrier.publish('failed', {'errorType': type(exc).__name__})
        except Exception as publication_error:
            raise exc from publication_error
        raise


def start_workload(worker, barrier, *, repo_free_bytes: int, permission_wait_ms: int,
                   network_probe_seconds: float):
    """Join prepared/routed workers and start actual Controller/Repo/Providers.

    Caller still owns candidate qualification, NFD/route setup, requests and
    final teardown. This function does not invent routing-readiness records.
    Both ranks execute it; returned readiness does not establish inference.
    """
    from runtime.yolo_profile import _read_plane
    from runtime.yolo_worker import PROVIDER_ROLES, StartupBarrier
    if worker._preparation_binding is None or not isinstance(barrier, StartupBarrier):
        raise ValueError('YOLO_STARTUP_PREPARATION')
    if (type(permission_wait_ms) is not int or not 1 <= permission_wait_ms <= 120000
            or type(repo_free_bytes) is not int or not 0 < repo_free_bytes <= 2**63 - 1
            or isinstance(network_probe_seconds, bool) or not isinstance(network_probe_seconds, (int, float))
            or not math.isfinite(network_probe_seconds) or not 0 < network_probe_seconds <= 120):
        raise ValueError('YOLO_STARTUP_BUDGET')
    plan, _, candidate = worker._preparation_binding
    if (barrier.binding['runId'] != plan['runId'] or barrier.binding['candidateDigest'] != candidate
            or barrier.rank != worker.rank or barrier.ranks != tuple(n['rank'] for n in plan['nodes'])):
        raise ValueError('YOLO_STARTUP_BINDING')
    config = _read_plane(worker.public / 'case.json')
    services = [s for s in config['services'] if not s['name'].startswith('/NDNSF/DistributedRepo/')]
    if (len(services) != 1 or set(services[0]['roles']) != PROVIDER_ROLES
            or config['controller'] != plan['identities']['controller']
            or config['group'] != config['runtime']['application_name'] + '/sync'
            or config['runtime']['application_name'] != plan['applicationName']):
        raise ValueError('YOLO_STARTUP_CONFIG')
    try:
        peer_failure = (barrier.directory / ('failed-' + str(1 - worker.rank) + '.json')
                        if len(barrier.ranks) == 2 else None)
        # A single budget covers all subsequent waits; no fresh timeout per stage.
        routes = barrier.wait('routes-ready')
        if any(r != {'namespace': plan['namespace'], 'syncPrefix': config['group']} for r in routes.values()):
            raise ValueError('YOLO_STARTUP_ROUTES')
        if len(barrier.ranks) == 2:
            if barrier.remaining() <= network_probe_seconds + worker.cleanup_seconds:
                raise TimeoutError('YOLO_STARTUP_NETWORK_BUDGET')
            receipt = wait_network_ready(worker, probe_id=barrier.binding['probeId'], seconds=network_probe_seconds,
                                         peer_failure=peer_failure)
            barrier.publish('network-ready', receipt)
            paired = barrier.wait('network-ready')
            roles = ('BackboneNeck', 'DetectShard0')
            for rank, value in paired.items():
                peer = plan['identities'][roles[1 - rank]]
                expected = dict(schema='tiger-yolo-network-readiness-v1',
                    probeId=barrier.binding['probeId'], producer=plan['identities'][roles[rank]], peer=peer,
                    receivedName=peer + '/SPEC183-NETWORK/' + barrier.binding['probeId'] + '/data',
                    status='READY', qualification='NOT_EVALUATED')
                if value != expected:
                    raise ValueError('YOLO_STARTUP_NETWORK_PAIR')
        if worker.rank == 0:
            start_controller(worker)
            publication = wait_controller_publication(worker, seconds=barrier.remaining(), peer_failure=peer_failure)
            start_repo(worker, identity=plan['identities']['repo'], free_bytes=repo_free_bytes)
            repo_budget = min(300, barrier.remaining() - worker.cleanup_seconds)
            if repo_budget <= 0:
                raise TimeoutError('YOLO_STARTUP_REPO_BUDGET')
            repo = wait_repo_ready(worker, seconds=repo_budget, peer_failure=peer_failure)
            barrier.publish('control-ready', {'publication': publication, 'repo': repo})
        control = barrier.wait('control-ready', ranks=(0,))[0]
        expected_publication = _read_plane(worker.public / 'runtime-publication.json')
        from runtime.yolo_result import validate_runtime_publication_receipt
        validate_runtime_publication_receipt(expected_publication, control['publication'])
        if (control['repo'].get('status') != 'READY'
                or control['repo'].get('repo') != plan['identities']['repo']):
            raise ValueError('YOLO_STARTUP_REPO')
        own_roles = sorted(PROVIDER_ROLES.intersection(worker.roles))
        for role in own_roles:
            barrier.remaining()
            worker.start_provider(role, identity=plan['identities'][role], service=services[0]['name'],
                group=config['group'], controller=config['controller'], permission_wait_ms=permission_wait_ms)
        for role in own_roles:
            wait_provider_ready(worker, role, seconds=barrier.remaining(), peer_failure=peer_failure)
        barrier.publish('providers-ready', {r: plan['identities'][r] for r in own_roles})
        ready = barrier.wait('providers-ready')
        for node in plan['nodes']:
            expected = {r: plan['identities'][r] for r in node['roles'] if r in PROVIDER_ROLES}
            if ready[node['rank']] != expected:
                raise ValueError('YOLO_STARTUP_PROVIDER_SET')
        return {'status': 'RUNTIME_READY', 'qualification': 'NOT_EVALUATED',
                'control': control, 'providers': ready}
    except BaseException as exc:
        try:
            barrier.publish('failed', {'errorType': type(exc).__name__})
        except Exception as publication_error:
            raise exc from publication_error  # Preserve both errors; outer owner tears down.
        raise


def run_requests(worker, plan: dict, *, package: Path, catalog_data_name: str,
                 catalog_signer: str, permission_wait_ms: int,
                 request_deadline_ms: int, process_timeout_seconds: float,
                 protection_epoch: str,
                 accept_request, peer_failure: Path | None = None,
                 remaining_seconds=None):
    """Stop on the first process/evidence failure, keeping Providers alive.

    ``accept_request(request, output)`` must raise on incomplete/invalid
    evidence. Exit zero alone never accepts a request. Physical source/SIF,
    policy, catalogue and model validation belongs to the preceding gates.
    """
    if not callable(accept_request):
        raise ValueError('YOLO_RESULT_VALIDATOR_REQUIRED')
    if remaining_seconds is not None and not callable(remaining_seconds):
        raise ValueError('YOLO_WORKLOAD_BUDGET_CALLBACK')
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
    from runtime.yolo_profile import _read_plane
    candidate = _read_plane(worker.public / 'offer-trust-root.json')
    if (candidate.get('schema') != 'spec180-provider-offer-trust-v1'
            or not isinstance(candidate.get('candidateId'), str)
            or not re.fullmatch(r'[A-Za-z0-9_.-]{1,128}', candidate['candidateId'])
            or not isinstance(candidate.get('candidateDigest'), str)
            or not re.fullmatch(r'sha256:[a-f0-9]{64}', candidate['candidateDigest'])):
        raise ValueError('YOLO_USER_CANDIDATE_BINDING')
    if worker._preparation_binding is None or worker._preparation_binding[0] != plan:
        raise ValueError('YOLO_USER_PREPARATION_REQUIRED')
    worker._verify_prepared_boundary()
    if candidate['candidateDigest'] != worker._preparation_binding[2]:
        raise ValueError('YOLO_USER_CANDIDATE_BINDING')
    for request in requests:
        i = str(request['index'])
        output = '/output/requests/' + i
        argv = ['/usr/bin/env', 'SPEC181_PROTECTION_EPOCH=' + protection_epoch,
                'SPEC180_CANDIDATE_ID=' + candidate['candidateId'],
                'SPEC180_CANDIDATE_DIGEST=' + candidate['candidateDigest'],
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
                '--native-tensor-input', '--input-size', '640', '--retain-numerical-response',
                '--request-id', request['requestId'], '--lifecycle-output-dir', output,
                '--lifecycle-case', plan['case'],
                '--envelope-key-file', '/identities/user/request-envelope.key']
        seconds = process_timeout_seconds
        if remaining_seconds is not None:
            seconds = min(seconds, remaining_seconds() - worker.cleanup_seconds)
            if not math.isfinite(seconds) or seconds <= 0:
                raise TimeoutError('YOLO_WORKLOAD_BUDGET')
        worker.run_user(i, argv, package=package, seconds=seconds,
                        peer_failure=peer_failure)
        accept_request(request, Path(request['output']))


def run_normal_node(worker, startup, *, completion_factory, endpoints,
                    startup_options, request_options, accept_request):
    """Own the normal node lifecycle behind the external qualification gates.

    Both ranks call this. The completion barrier is created only after startup
    with its own frozen workload budget and exclusive directory; it shares
    the run/candidate/probe identity, not the consumed startup deadline.
    Rank 1 remains alive until rank 0 finishes validated requests. No public
    submit command is enabled here, and node cleanup is not inference PASS.
    """
    from runtime.yolo_result import write_worker_receipt
    if (worker._preparation_binding is None or worker.mode not in
            ('local-cpu', 'single-node-gpu', 'two-node-gpu')
            or not callable(completion_factory) or not callable(accept_request)):
        raise ValueError('YOLO_NODE_OWNER_SCOPE')
    plan = worker._preparation_binding[0]
    completion = None
    rows = None
    def notify(error):
        # A stage may already have reported failure; preserve original error
        # and still attempt notification on the other phase's control lane.
        for barrier in (completion, startup):
            if barrier is not None:
                try:
                    barrier.publish('failed', {'errorType': type(error).__name__})
                except Exception:
                    pass
    try:
        try:
            configure_network(worker, startup, endpoints=endpoints)
            start_workload(worker, startup, **startup_options)
            completion = completion_factory()
            if (completion.binding != startup.binding or completion.rank != worker.rank
                    or completion.ranks != startup.ranks or completion.directory == startup.directory):
                raise ValueError('YOLO_COMPLETION_BINDING')
            peer_failure = (completion.directory / ('failed-' + str(1-worker.rank) + '.json')
                            if len(completion.ranks) == 2 else None)
            if worker.rank == 0:
                def accept(request, output):
                    completion.remaining()
                    accept_request(request, output)
                run_requests(worker, plan, accept_request=accept,
                             peer_failure=peer_failure, remaining_seconds=completion.remaining,
                             **request_options)
            count = len(plan['requests']) if worker.rank == 0 else 0
            completion.publish('workload-complete', {'requestCount': count})
            completed = completion.wait('workload-complete')
            expected = {rank: {'requestCount': len(plan['requests']) if rank == 0 else 0}
                        for rank in completion.ranks}
            if completed != expected:
                raise ValueError('YOLO_COMPLETION_REQUEST_COVER')
        except BaseException as exc:
            notify(exc)
            raise
        finally:
            rows = worker.close()
        return write_worker_receipt(worker, rows)
    except BaseException as exc:
        notify(exc)
        from runtime.identities import _credential_document
        try:
            _credential_document(worker.output / 'node-failure.json', {
                'schema': 'tiger-yolo-node-failure-v1', 'runId': plan['runId'],
                'case': worker.mode, 'rank': worker.rank,
                'errorType': type(exc).__name__, 'cleanup': rows,
                'qualification': 'FAILED'})
        except Exception as recording_error:
            raise exc from recording_error
        raise


if __name__ == '__main__':
    raise SystemExit(main())
