"""Exercise the shipped ops wheel's read-only configuration path, not inference."""

import copy
import contextlib
import hashlib
import importlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import select
import signal
import socket
import subprocess
import sys
import time
from types import SimpleNamespace
import zipfile
import uuid
import venv

import pytest


ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize('change', ['none', 'launch', 'allocation', 'symlink', 'active',
                                  'nonterminal', 'foreign-boot', 'foreign-config', 'integer-proof'])
def test_cancel_before_allocation_requires_bound_scheduler_proof(ops, tmp_path, monkeypatch, change):
    backend = importlib.import_module('_spec217_ops.slurm')
    root, evidence = tmp_path / 'pending', tmp_path / 'evidence'
    backend.cluster._private_directory(root)
    backend.cluster._private_directory(evidence)
    reference = dict(boot_id='a' * 32, configuration_sha256='b' * 64, job_id='42')
    state = dict(schema=backend.SCHEMA, deployment_id='pending', boot_id='a' * 32,
                 configuration_sha256='b' * 64, evidence_root=str(evidence),
                 status='AWAITING_NODE_AUDIT', job_reference=reference, failure=None,
                 needs_closeout=False, launches=[])
    scheduler = dict(reference=reference, terminal=True, state='CANCELLED', status='TERMINAL',
                     live_verified=False, never_allocated=True)
    if change == 'launch':
        state['launches'] = [dict(id='unexpected')]
    elif change == 'allocation':
        backend.cluster.write_json(evidence / 'allocation.json', {})
    elif change == 'symlink':
        (evidence / 'allocation.json').symlink_to(evidence / 'absent')
    elif change == 'active':
        scheduler.update(state='RUNNING', live_verified=True)
    elif change == 'nonterminal':
        scheduler['terminal'] = False
    elif change == 'foreign-boot':
        reference['boot_id'] = 'c' * 32
    elif change == 'foreign-config':
        reference['configuration_sha256'] = 'c' * 64
    elif change == 'integer-proof':
        scheduler['never_allocated'] = 1

    class Adapter:
        def deployment_status(self, value, **kwargs):
            assert value == reference
            return copy.deepcopy(scheduler)

        cancel_deployment = deployment_status

    monkeypatch.setattr(backend, 'SlurmApptainerAdapter', Adapter)
    backend.cluster.write_json(root / 'state.json', state)
    result = backend.snapshot(state, timeout=2)
    closed = backend.run_slurm('down', tmp_path / 'unused.json', root, timeout=2)
    persisted = json.loads((root / 'state.json').read_text())
    if change == 'none':
        assert result['status'] == 'STOPPED' and result['actor_cleanup'] == 'ZERO'
        assert result['cleanup_scope'] == 'SCHEDULER_NEVER_ALLOCATED'
        assert result['execution'] == 'NOT_STARTED' and result['oracle'] == 'NOT_RUN'
        assert result['failure'] == 'DEPLOYMENT_CANCELLED_BEFORE_ALLOCATION'
        assert closed['operation_stage'] == persisted['status'] == 'STOPPED'
        assert persisted['needs_closeout'] is False
    else:
        assert result['status'] == closed['status'] == 'UNKNOWN'
        assert result['actor_cleanup'] == 'UNVERIFIED' and persisted['needs_closeout'] is True


@pytest.fixture(scope="module")
def ops(tmp_path_factory):
    root = tmp_path_factory.mktemp("deployment-wheel")
    source = root / "source/NDNSF-DistributedInference"
    profiles = source / "packaging/python/ops"
    shutil.copytree(ROOT / "NDNSF-DistributedInference/packaging/python/ops", profiles,
                    ignore=shutil.ignore_patterns("build", "dist", "*.egg-info", "__pycache__"))
    package = source / "ndnsf_distributed_inference"
    package.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/__init__.py",
                    package / "__init__.py")
    shutil.copytree(ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/ops", package / "ops",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copytree(ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility",
                    package / "compatibility", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    compat_profile = source / "packaging/python/compat"
    shutil.copytree(ROOT / "NDNSF-DistributedInference/packaging/python/compat", compat_profile,
                    ignore=shutil.ignore_patterns("build", "dist", "*.egg-info", "__pycache__"))
    helper = root / "source/packaging/ndnsf-di-container/lib/allocation_topology.py"
    helper.parent.mkdir(parents=True)
    shutil.copyfile(ROOT / "packaging/ndnsf-di-container/lib/allocation_topology.py", helper)
    processes = root / "source/Experiments/TigerCluster/runtime/processes.py"
    processes.parent.mkdir(parents=True)
    shutil.copyfile(ROOT / "Experiments/TigerCluster/runtime/processes.py", processes)
    for name in ('__init__.py', 'base.py', 'slurm_apptainer.py'):
        relative = Path('packaging/ndnsf-di-container/lib/adapters') / name
        target = root / 'source' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, target)
    for name in ('run-allocation-topology.sh', 'configure-allocation-routes.sh',
                 'run-container.sh', 'stage-sif.sh', 'validate-local-sif-build-record.py'):
        relative = Path('Experiments/TigerCluster/adapters/slurm-apptainer/scripts') / name
        target = root / 'source' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, target)
    wheels = root / "wheels"
    subprocess.run([sys.executable, "-m", "pip", "wheel", "--no-deps", str(profiles), str(compat_profile),
                    "-w", str(wheels)],
                   check=True, capture_output=True, text=True, timeout=120)
    assert len(list(wheels.glob("*.whl"))) == 2
    environment = root / 'environment'
    venv.EnvBuilder(with_pip=False, system_site_packages=True).create(environment)
    installed_root = environment / 'lib' / ('python%d.%d' % sys.version_info[:2]) / 'site-packages'
    for wheel in wheels.glob("*.whl"):
        with zipfile.ZipFile(wheel) as archive:
            archive.extractall(installed_root)
    # Install the existing declared pure Python Name codec into this isolated
    # fixture environment; native DI/runtime packages are not copied.
    import ndn.encoding
    shutil.copytree(Path(ndn.encoding.__file__).parents[1], installed_root / 'ndn',
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    installed = installed_root / 'ndnsf_distributed_inference/ops'
    # A private package name isolates these configuration tests from any host
    # DI owner. Both parser and helper bytes come from the actual built wheel.
    spec = importlib.util.spec_from_file_location(
        "_spec217_ops", installed / "__init__.py", submodule_search_locations=[str(installed)])
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    parser = importlib.import_module("_spec217_ops.deployment_config")
    cli = importlib.import_module("_spec217_ops.cli")
    assert Path(parser.__file__).parent == installed
    yield parser, cli
    for name in list(sys.modules):
        if name == "_spec217_ops" or name.startswith("_spec217_ops."):
            del sys.modules[name]


@pytest.fixture
def deployment(tmp_path):
    for name in ("trust.conf", "controller.cert", "policy.json", "plan.json", "manifest.json",
                 "tokenizer.json", "key.pem", "input.bin", "options.json"):
        (tmp_path / name).write_bytes(b"configuration path fixture\n")
    prefix = tmp_path / "release"
    ledger = {"schema": 1, "files": {}}
    for relative in (
            "bin/App_ServiceController", "bin/DI_NativeArtifactAuthority", "bin/DI_NativeRequester",
            "bin/di-native-provider", "bin/DistributedRepoServer", "bin/ndnsf-identity-check",
            "libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker", "lib/libndn-service-framework.so",
            "libexec/ndnsf-di/ndnsf-di-device-query",
            "lib/libndnsf-distributed-inference.so"):
        target = prefix / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(b"installed file identity fixture\n")
        target.chmod(0o755)
        ledger["files"][str(target)] = {"installed": {
            "kind": "file", "mode": 0o755, "sha256": hashlib.sha256(target.read_bytes()).hexdigest()}}
    (tmp_path / "installed.json").write_text(json.dumps(ledger))
    actors = []
    for actor_id, kind in (("controller", "controller"), ("authority", "artifact-authority"),
                           ("requester", "requester"), ("p0", "provider"), ("p1", "provider")):
        home = tmp_path / "credentials" / actor_id
        home.mkdir(parents=True)
        actors.append({"id": actor_id, "kind": kind, "identity": "/test/" + actor_id,
                       "node": "local", "credential_home": str(home.relative_to(tmp_path)),
                       "resources": {"cpus": 1}})
    config = {
        "schema": "ndnsf-di-deployment-v1", "deployment_id": "deployment-test",
        "release": {"prefix": "release", "manifest": "installed.json"},
        "platform": {"kind": "host"},
        "nodes": [{"id": "local", "host": "local", "cpus": 8, "gpu_devices": [],
                   "nfd": {"mode": "managed", "socket": "nfd.sock", "address": "127.0.0.1", "port": 6363}}],
        "actors": actors,
        "network": {"group": "/test/group", "service": "/test/inference", "artifact_service": "/test/grant"},
        "security": {"trust_schema": "trust.conf", "controller_certificate": "controller.cert",
                     "controller_policy": "policy.json"},
        "storage": {**{key: key for key in ("state_root", "evidence_root", "scratch_root", "cache_root", "repo_root")},
                    "max_bytes": 1024 * 1024},
        "model": {"plan": "plan.json", "manifest": "manifest.json", "tokenizer": "tokenizer.json"},
        "requester": {"grant": {"requester_private_key_file": "key.pem", "authority_public_key_file": "key.pem",
                                  "protection_epoch": "epoch-1"},
                      "catalog": {"source": {"file": "plan.json"}},
                      "turns": [{"input_file": "input.bin", "options_file": "options.json",
                                 "output_file": "evidence_root/output.json"}]},
        "artifact_authority": {"authority": {
            "authority_private_key_file": "key.pem", "requester_public_key_file": "key.pem",
            "content_key_file": "key.pem", "recipient_public_key_files": {"/test/p0": "key.pem"},
            "allowed_model_manifests": ["sha256:" + "a" * 64], "content_key_id": "key-1",
            "protection_epoch": "epoch-1"}},
    }
    return tmp_path, config


def write_config(root, config):
    path = root / "deployment.json"
    path.write_text(json.dumps(config))
    return path


@contextlib.contextmanager
def local_management_child(env, ignore_term=False):
    setup = "import signal; signal.signal(signal.SIGTERM, signal.SIG_IGN); " if ignore_term else ""
    child = subprocess.Popen([sys.executable, '-c',
        setup + "import os,time; os.write(1,b'R'); time.sleep(60)"], env=env,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        start_new_session=True)
    try:
        assert select.select([child.stdout], [], [], 5)[0]
        assert child.stdout.read(1) == b'R'
        yield child
    finally:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait(timeout=5)
        child.stdout.close()


@contextlib.contextmanager
def management_operation(ops, deployment, ignore_term=False):
    """Persist a real child ownership record; no simulated native inference."""
    parser, _ = ops
    base, config = deployment
    path = write_config(base, config)
    normalized = parser.load_deployment(path)
    cluster = importlib.import_module('_spec217_ops.cluster')
    _, root = cluster._locator(path)
    cluster._private_directory(root)
    evidence = base / 'evidence_root' / uuid.uuid4().hex
    cluster._private_directory(evidence)
    state = {'schema': 'ndnsf-di-host-operation-v1', 'deployment_id': config['deployment_id'],
             'boot_id': uuid.uuid4().hex, 'status': 'COMPLETE', 'launches': [],
             'configuration_sha256': hashlib.sha256(json.dumps(normalized, sort_keys=True,
                 separators=(',', ':')).encode()).hexdigest(),
             'evidence_root': str(evidence)}
    env = cluster._environment(base, base / 'nfd.sock', 'p1', state)
    with local_management_child(env, ignore_term) as child:
        proof = cluster.process_identity(child.pid)
        row = {'id': 'p1', 'kind': 'provider', 'proof': proof,
               'expected_exe': proof['exe'], 'bindings': proof['bindings'],
               'log': str(evidence / 'p1.log')}
        state['launches'] = [row]
        cluster.write_json(root / 'state.json', state)
        yield cluster, path, root, state, child


def test_cluster_status_absent_does_not_create_state(ops, deployment):
    _, cli = ops
    base, config = deployment
    path = write_config(base, config)
    assert cli.main(['cluster', 'status', '--config', str(path)]) == 0
    assert not (base / 'state_root').exists()


def test_cluster_creates_every_private_directory_with_group_writable_umask(ops, tmp_path):
    cluster = importlib.import_module('_spec217_ops.cluster')
    target = tmp_path / 'state' / 'deployment' / 'boot'
    previous = os.umask(0o002)
    try:
        cluster._private_directory(target)
    finally:
        os.umask(previous)
    for path in (target, target.parent, target.parent.parent):
        assert path.stat().st_mode & 0o777 == 0o700


def test_cluster_rejects_writable_ancestor_before_creating_children(ops, tmp_path):
    cluster = importlib.import_module('_spec217_ops.cluster')
    parent = tmp_path / 'unsafe'
    parent.mkdir(mode=0o700)
    parent.chmod(0o777)
    try:
        with pytest.raises(cluster.ClusterError, match='untrusted ancestor'):
            cluster._private_directory(parent / 'state' / 'deployment')
        assert not (parent / 'state').exists()
    finally:
        parent.chmod(0o700)


def test_cluster_repeat_up_never_resubmits_or_preflights_active_services(ops, deployment, monkeypatch):
    with management_operation(ops, deployment) as (cluster, path, _, _, child):
        def forbidden(*args, **kwargs):
            raise AssertionError('repeat up launched or preflighted another operation')
        monkeypatch.setattr(cluster, '_host_preflight', forbidden)
        monkeypatch.setattr(cluster.subprocess, 'run', forbidden)
        assert cluster.run_cluster('up', path)['status'] == 'COMPLETE'
        assert child.poll() is None


@pytest.mark.parametrize('reverse_actors', (False, True))
@pytest.mark.parametrize('controller_failure', (False, True, 'path-call', 'path-call-deep'))
@pytest.mark.parametrize('debug', (False, True))
def test_controller_readiness_precedes_dependent_launches(
        ops, deployment, monkeypatch, reverse_actors, controller_failure, debug):
    # Exercise management ordering only; these controlled stubs are not native readiness evidence.
    parser, _ = ops
    base, config = deployment
    config['nodes'][0]['nfd']['mode'] = 'external'
    requester = next(a for a in config['actors'] if a['kind'] == 'requester')
    requester['external'] = True
    for key in ('node', 'credential_home', 'resources'):
        del requester[key]
    config['requester'] = {}
    if reverse_actors:
        config['actors'].reverse()
    source = write_config(base, config)
    cluster = importlib.import_module('_spec217_ops.cluster')
    root = base / 'state_root/deployment-test'
    spawned = []
    released = False
    controller_polls = 0
    group_verified = False

    class Child:
        def poll(self):
            return None

    def spawn(state, directory, actor_id, kind, argv, env, guard):
        guard()
        assert group_verified, 'role spawned before group forwarding validation'
        assert kind == 'controller' or released, 'dependent spawned before Controller readiness'
        spawned.append(kind)
        row = {'id': actor_id, 'kind': kind, 'proof': {'pid': 0}}
        state['launches'].append(row)
        return Child(), row

    def ready(row, actor, state):
        nonlocal released, controller_polls
        if actor['kind'] != 'controller':
            assert released
            return True
        assert spawned == ['controller']
        controller_polls += 1
        if controller_polls == 1:
            return False
        if controller_failure == 'path-call':
            unexpected = Path('/controlled-management-error')
            unexpected()
        if controller_failure == 'path-call-deep':
            def fail_at_origin():
                unexpected = Path('/controlled-management-error')
                unexpected()

            def call_deeper(depth):
                if depth:
                    call_deeper(depth - 1)
                else:
                    fail_at_origin()

            call_deeper(12)
        if controller_failure:
            raise cluster.ClusterError('CONTROLLER_READINESS_REJECTED')
        released = True
        return True

    def close(state, directory):
        state['status'] = 'STOPPED'
        return state

    monkeypatch.setattr(cluster, '_host_preflight', lambda _: None)
    monkeypatch.setattr(cluster, '_check_budget', lambda _: 0)
    monkeypatch.setattr(cluster, '_native_launch', lambda *args: (['controlled-stub'], {}))
    monkeypatch.setattr(cluster, '_spawn', spawn)
    monkeypatch.setattr(cluster, '_ready', ready)
    monkeypatch.setattr(cluster, '_close', close)
    monkeypatch.setattr(cluster, '_snapshot', lambda state, *args: state)
    monkeypatch.setattr(cluster.shutil, 'which', lambda _: '/usr/bin/true')
    def run(argv, **kwargs):
        nonlocal group_verified
        assert argv[-2:] != ['status', 'report'], 'readiness must not cache strategy/list'
        output = b''
        if argv[-2:] == ['strategy', 'list']:
            output = ('prefix=' + config['network']['group'] +
                      ' strategy=/localhost/nfd/strategy/multicast/v=5\n').encode()
            group_verified = True
        return subprocess.CompletedProcess(argv, 0, stdout=output, stderr=b'')
    monkeypatch.setattr(cluster.subprocess, 'run', run)
    monkeypatch.setattr(cluster.time, 'sleep', lambda _: None)
    result = cluster._up(source, root, 5, debug)
    assert result['diagnostic_logging'] is debug
    if controller_failure:
        if controller_failure in ('path-call', 'path-call-deep'):
            assert result['failure'] == "'PosixPath' object is not callable"
            boundary = result['failure_boundary']
            assert boundary['type'] == 'TypeError'
            assert 0 < len(boundary['frames']) <= 8
            origin = 'fail_at_origin' if controller_failure == 'path-call-deep' else 'ready'
            assert boundary['frames'][-1]['function'] == origin
            if controller_failure == 'path-call-deep':
                assert len(boundary['frames']) == 8
            assert boundary['frames'][-1]['line'] > 0
            persisted = json.loads((Path(result['evidence_root']) / 'operation.json').read_text())
            assert persisted['failure_boundary'] == boundary
        else:
            assert result['failure'] == 'CONTROLLER_READINESS_REJECTED'
        assert result['status'] == 'STOPPED'
        assert spawned == ['controller']
    else:
        assert result['status'] == 'EXTERNAL_UNVERIFIED'
        assert spawned[0] == 'controller'
        assert sorted(spawned[1:]) == ['artifact-authority', 'provider', 'provider']
    assert controller_polls == 2


@pytest.mark.parametrize('mode', ('managed', 'external'))
@pytest.mark.parametrize('case', ('valid', 'command-failure', 'best-route', 'specific-override',
                                  'malformed'))
def test_group_forwarding_is_verified_and_external_nfd_is_never_mutated(
        ops, tmp_path, monkeypatch, mode, case):
    import time

    cluster = importlib.import_module('_spec217_ops.cluster')
    calls = []
    config = {'network': {'group': '/test/group'}, 'nodes': [{'nfd': {'mode': mode}}]}
    output = 'prefix=/test/group strategy=/localhost/nfd/strategy/multicast/v=5\n'
    if case == 'best-route':
        output = 'prefix=/ strategy=/localhost/nfd/strategy/best-route/v=5\n'
    if case == 'specific-override':
        output += 'prefix=/test/group/v=3 strategy=/localhost/nfd/strategy/best-route/v=5\n'
    if case == 'malformed':
        output = 'not a strategy dataset\n'

    def run(argv, **kwargs):
        calls.append(argv[1:])
        assert kwargs['timeout'] <= 5
        rc = 1 if case == 'command-failure' else 0
        return subprocess.CompletedProcess(argv, rc,
                                           stdout=output.encode(), stderr=b'')

    monkeypatch.setattr(cluster.subprocess, 'run', run)
    guards = []
    def guard():
        guards.append(True)
    if case == 'valid':
        cluster._configure_group_strategy('nfdc', {}, config, tmp_path,
                                          time.monotonic() + 10, guard)
    else:
        with pytest.raises(cluster.ClusterError, match='NFD_GROUP_'):
            cluster._configure_group_strategy('nfdc', {}, config, tmp_path,
                                              time.monotonic() + 10, guard)
    if mode == 'external':
        assert calls == [['strategy', 'list']]
    else:
        assert calls[0] == ['strategy', 'set', 'prefix', '/test/group',
                            'strategy', '/localhost/nfd/strategy/multicast']
        assert len(calls) == (1 if case == 'command-failure' else 2)
    assert len(guards) == 2 * len(calls)
    receipt = json.loads((tmp_path / 'group-forwarding.json').read_text())
    assert len(receipt['commands']) == len(calls)


def test_cluster_down_works_after_runtime_inputs_disappear_and_retains_data(ops, deployment):
    with management_operation(ops, deployment) as (cluster, path, root, state, _):
        base, _ = deployment
        (base / 'plan.json').unlink()
        (base / 'release/bin/di-native-provider').unlink()
        retained = base / 'repo_root/retained.data'
        retained.parent.mkdir()
        retained.write_bytes(b'original Data fixture')
        result = cluster.run_cluster('down', path)
        assert result['status'] == 'STOPPED'
        assert result['cleanup'][0]['status'] == 'ZERO'
        assert result['cleanup'][0]['exitCode'] == -signal.SIGTERM
        assert retained.read_bytes() == b'original Data fixture'
        assert (root / 'state.json').is_file()
        assert (Path(state['evidence_root']) / 'operation.json').is_file()


def test_cluster_interrupted_owner_is_unknown_and_never_resubmits(ops, deployment):
    with management_operation(ops, deployment) as (cluster, path, root, state, child):
        state.update(status='REQUEST_RUNNING', manager=cluster.process_identity(os.getpid()))
        cluster.write_json(root / 'state.json', state)
        # A still-live PID without the held operation lock is not an active up.
        assert cluster.run_cluster('status', path)['status'] == 'UNKNOWN'
        assert cluster.run_cluster('up', path)['status'] == 'UNKNOWN'
        assert child.poll() is None


@pytest.mark.parametrize('completion,observation', [
    ('RUNNING', 'known-exit'), ('RUNNING', 'missing-exit'), ('RUNNING', 'unknown-cleanup'),
    ('EXITED', 'known-exit'), ('INTERRUPTED', 'known-exit'),
])
def test_cluster_closeout_records_interrupted_requester_without_inventing_exit(
        ops, deployment, monkeypatch, completion, observation):
    with management_operation(ops, deployment) as (cluster, path, root, state, child):
        state['launches'][0]['kind'] = 'requester'
        state.update(status='REQUEST_RUNNING', manager=cluster.process_identity(os.getpid()),
                     failure='FIRST_RECORDED_FAILURE',
                     failure_boundary={'type': 'FixtureError', 'frames': []})
        # Synthetic prior measurements test preservation, not inference performance.
        state['requester'] = {'completion': completion,
                              'exit_code': 0 if completion == 'EXITED' else None,
                              'active_wall_seconds': 0.125, 'wall_seconds': 0.25,
                              'started_unix_seconds': time.time(), 'oracle': 'NOT_RUN'}
        prior = copy.deepcopy(state['requester'])
        cluster.write_json(root / 'state.json', state)
        stop = cluster.stop_owned

        def observe(proof):
            if observation == 'unknown-cleanup':
                return {'status': 'UNKNOWN', 'error': 'injected ownership observation gap'}
            result = stop(proof)
            if observation == 'missing-exit':
                result.pop('exitCode', None)
                result.pop('reaped', None)
            return result

        monkeypatch.setattr(cluster, 'stop_owned', observe)
        result = cluster.run_cluster('down', path)
        expected = 'UNKNOWN' if observation == 'unknown-cleanup' else 'STOPPED'
        assert result['status'] == expected
        assert result['failure'] == 'FIRST_RECORDED_FAILURE'
        requester = result['requester']
        if completion == 'RUNNING':
            assert requester['completion'] == 'INTERRUPTED'
            assert requester['cleanup'] == result['cleanup'][0]
            assert requester['exit_code'] == (
                -signal.SIGTERM if observation == 'known-exit' else None)
            for key, value in prior.items():
                if key not in ('completion', 'exit_code'):
                    assert requester[key] == value
        else:
            assert requester == prior
        frozen = copy.deepcopy(requester)
        repeated = cluster.run_cluster('down', path)
        assert repeated['status'] == expected and repeated['requester'] == frozen
        assert repeated['failure'] == 'FIRST_RECORDED_FAILURE'
        saved = json.loads((root / 'state.json').read_text())
        assert saved['requester'] == frozen
        assert saved['failure_boundary'] == {'type': 'FixtureError', 'frames': []}
        if observation == 'unknown-cleanup':
            assert child.poll() is None


def test_cluster_cannot_adopt_a_different_boots_valid_process_proof(ops, deployment):
    with management_operation(ops, deployment) as (cluster, path, root, state, owned):
        other_state = dict(state, boot_id=uuid.uuid4().hex)
        env = cluster._environment(deployment[0], deployment[0] / 'nfd.sock', 'p1', other_state)
        with local_management_child(env) as external:
            proof = cluster.process_identity(external.pid)
            state['launches'][0].update(proof=proof, bindings=proof['bindings'])
            cluster.write_json(root / 'state.json', state)
            assert cluster.run_cluster('down', path)['status'] == 'UNKNOWN'
            assert owned.poll() is None
            assert external.poll() is None


def test_repeated_down_preserves_forced_exit_evidence_and_current_unknown(
        ops, deployment, monkeypatch):
    with management_operation(ops, deployment, ignore_term=True) as (
            cluster, path, root, state, child):
        stop = cluster.stop_owned
        monkeypatch.setattr(cluster, 'stop_owned',
                            lambda proof: stop(proof, term_seconds=0, kill_seconds=1))
        first = cluster.run_cluster('down', path)
        assert first['status'] == 'STOPPED'
        assert first['cleanup'][0]['forced'] is True
        assert first['cleanup'][0]['exitCode'] == -signal.SIGKILL
        assert first['cleanup'][0]['reaped'] is True
        writes = []
        publish = cluster.write_json

        def observed_write(target, value):
            if target == root / 'state.json' and value['status'] == 'STOPPING':
                writes.append(copy.deepcopy(value['cleanup']))
            publish(target, value)

        monkeypatch.setattr(cluster, 'write_json', observed_write)
        for _ in range(2):
            repeated = cluster.run_cluster('down', path)
            assert repeated['status'] == 'STOPPED'
            assert repeated['cleanup'][0]['forced'] is True
            assert repeated['cleanup'][0]['exitCode'] == -signal.SIGKILL
            assert repeated['cleanup'][0]['reaped'] is True
            saved = json.loads((Path(state['evidence_root']) / 'operation.json').read_text())
            assert saved['cleanup'] == repeated['cleanup']
        monkeypatch.setattr(cluster, 'stop_owned',
                            lambda _: {'status': 'UNKNOWN', 'error': 'controlled audit failure'})
        uncertain = cluster.run_cluster('down', path)
        assert uncertain['status'] == 'UNKNOWN'
        assert uncertain['cleanup'][0]['status'] == 'UNKNOWN'
        assert uncertain['cleanup'][0]['forced'] is True
        assert uncertain['cleanup'][0]['exitCode'] == -signal.SIGKILL
        assert writes and all(rows and rows[0]['forced'] is True for rows in writes)


def test_cluster_recovers_only_its_exact_interrupted_launch(ops, deployment):
    with management_operation(ops, deployment) as (cluster, path, root, state, _):
        del state['launches'][0]['proof']
        state['status'] = 'STARTING'
        cluster.write_json(root / 'state.json', state)
        result = cluster.run_cluster('down', path)
        assert result['status'] == 'STOPPED'
        assert result['cleanup'][0]['status'] == 'ZERO'


def test_cluster_rejects_nonprivate_operation_directory(ops, deployment):
    with management_operation(ops, deployment) as (cluster, path, root, _, child):
        root.chmod(0o755)
        try:
            with pytest.raises(cluster.ClusterError, match='mode 0700'):
                cluster.run_cluster('down', path)
            assert child.poll() is None
        finally:
            root.chmod(0o700)


def test_cluster_ready_record_is_bound_to_the_real_child_boot_and_name(ops, deployment):
    with management_operation(ops, deployment) as (cluster, _, _, state, child):
        row = state['launches'][0]
        actor = {'id': 'p1', 'kind': 'provider', 'identity': '/test/p1'}
        record = {'event': 'NDNSF_DEPLOYMENT_READY', 'actor_id': 'p1',
                  'boot_id': state['boot_id'], 'config_sha256': state['configuration_sha256'],
                  'identity': '/8=test/8=p1', 'native_boot_id': 'native-fixture', 'pid': child.pid}
        Path(row['log']).write_text(json.dumps(record) + '\n')
        assert cluster._ready(row, actor, state)
        for key, value in [('boot_id', 'stale'), ('pid', child.pid + 1),
                           ('config_sha256', '0' * 64), ('identity', '/other/p1')]:
            Path(row['log']).write_text(json.dumps(dict(record, **{key: value})) + '\n')
            assert not cluster._ready(row, actor, state)


@pytest.mark.parametrize('debug', (False, True))
def test_cluster_native_launch_uses_isolated_env_and_only_this_providers_key(
        ops, deployment, monkeypatch, debug):
    parser, _ = ops
    base, config = deployment
    (base / 'trust-root-registry-v1.json').write_text('{}')
    (base / 'recipients.json').write_text(json.dumps({'/8=test/8=p1': 'key.pem'}))
    config['actors'][-1]['parameters'] = {'grant-authority-public-key': 'key.pem',
        'recipient-key-map': 'recipients.json', 'enable-admission-lease': False,
        'allow-selection-generation-options': True, 'repo-fetch-timeout-ms': 900000,
        'conversation-retention-ms': 3600000}
    path = write_config(base, config)
    native = parser.load_deployment(path)
    cluster = importlib.import_module('_spec217_ops.cluster')
    directory = base / 'native-inputs'
    directory.mkdir()
    state = {'boot_id': uuid.uuid4().hex, 'configuration_sha256': 'a' * 64,
             'diagnostic_logging': debug}
    monkeypatch.setenv('PYTHONPATH', '/development/only')
    monkeypatch.setenv('LD_LIBRARY_PATH', '/development/libs')
    monkeypatch.setenv('NDN_LOG', '*=TRACE')
    argv, env = cluster._native_launch(native, native['actors'][-1], directory, state)
    assert argv[0] == str(base / 'release/bin/di-native-provider')
    assert '--allow-selection-generation-options' in argv
    assert argv[argv.index('--repo-fetch-timeout-ms') + 1] == '900000'
    assert argv[argv.index('--conversation-retention-ms') + 1] == '3600000'
    assert '--enable-admission-lease' not in argv
    assert '--recipient-key-map' not in argv
    assert '--grant-authority-public-key' not in argv
    assert 'PYTHONPATH' not in env and 'LD_LIBRARY_PATH' not in env
    assert env['NDN_LOG'] == ('*=DEBUG' if debug else '*=NONE')
    assert env['HOME'] == str(base / 'credentials/p1')
    assert env['NDN_CLIENT_TPM'] == 'tpm-file:'
    assert 'NDNSF_CONTROLLER_GENERATION_STATE' not in env
    mapping = Path(env['SPEC181_PROVIDER_RECIPIENT_KEY_MAP'])
    assert json.loads(mapping.read_text()) == {'/test/p1': str(base / 'key.pem')}
    assert mapping.stat().st_mode & 0o777 == 0o600


def test_controller_generation_path_is_stable_private_and_native_owned(
        ops, deployment, monkeypatch):
    parser, _ = ops
    base, config = deployment
    path = write_config(base, config)
    native = parser.load_deployment(path)
    actor = next(a for a in native['actors'] if a['kind'] == 'controller')
    cluster = importlib.import_module('_spec217_ops.cluster')
    monkeypatch.setenv('NDNSF_CONTROLLER_GENERATION_STATE', '/tmp/unrelated-generation')
    expected = str(Path(actor['credential_home']) / '.ndn/controller-generation.state')
    for boot in ('a' * 32, 'b' * 32):
        state = {'boot_id': boot, 'configuration_sha256': 'c' * 64}
        _, env = cluster._native_launch(native, actor, base, state)
        assert env['NDNSF_CONTROLLER_GENERATION_STATE'] == expected
    assert not Path(expected).exists()  # The native Controller owns its state writes.


def test_cluster_cancellation_after_intent_does_not_spawn_requester(ops, deployment, monkeypatch):
    with management_operation(ops, deployment) as (cluster, _, root, state, _):
        env = cluster._environment(deployment[0], deployment[0] / 'nfd.sock', 'requester', state)

        def cancelled():
            raise cluster.ClusterError('DEPLOYMENT_CANCELLED')

        def forbidden(*args, **kwargs):
            raise AssertionError('Popen called after cancellation')

        with monkeypatch.context() as patch:
            patch.setattr(cluster.subprocess, 'Popen', forbidden)
            with pytest.raises(cluster.ClusterError, match='DEPLOYMENT_CANCELLED'):
                cluster._spawn(state, root, 'requester', 'requester', [sys.executable], env, cancelled)
        persisted = cluster._read_state(root)
        assert persisted['launches'][-1]['spawn_failed'] is True
        assert not persisted['launches'][-1].get('proof')


@pytest.mark.parametrize('observation', ('initial-empty', 'always-empty', 'cancel', 'early-exit'))
def test_cluster_spawn_requires_complete_real_child_identity(
        ops, deployment, monkeypatch, observation):
    with management_operation(ops, deployment) as (cluster, _, root, state, _):
        env = cluster._environment(deployment[0], deployment[0] / 'nfd.sock', 'p2', state)
        identity = cluster.process_identity
        popen = subprocess.Popen
        children = []
        observations = []

        def start(*args, **kwargs):
            child = popen(*args, **kwargs)
            children.append(child)
            if observation == 'early-exit':
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    try:
                        current = identity(child.pid)
                    except OSError:
                        # Exiting between stat and environ is still unobserved;
                        # wait for actual Z without reaping or weakening proof.
                        current = None
                    if current and current['state'] == 'Z':
                        break
                    time.sleep(0.01)
                else:
                    raise AssertionError('test child did not become an unreaped zombie')
            return child

        def observe(pid):
            current = identity(pid)
            if current and observation != 'early-exit' and (
                    not observations or observation in ('always-empty', 'cancel')):
                current = dict(current, bindings={key: '' for key in env
                    if key.startswith('NDNSF_DEPLOYMENT_')})
            observations.append(current)
            return current

        def guard():
            if observation == 'cancel' and observations:
                raise cluster.ClusterError('DEPLOYMENT_CANCELLED')

        def forbidden_signal(*args, **kwargs):
            raise AssertionError('capture attempted cleanup without confirmed ownership')

        monkeypatch.setattr(cluster.subprocess, 'Popen', start)
        monkeypatch.setattr(cluster, 'process_identity', observe)
        monkeypatch.setattr(cluster, 'stop_owned', forbidden_signal)
        code = 'pass' if observation == 'early-exit' else 'import time; time.sleep(60)'
        try:
            if observation in ('always-empty', 'cancel'):
                message = ('DEPLOYMENT_CANCELLED' if observation == 'cancel'
                           else 'cannot capture native process identity')
                with pytest.raises(cluster.ClusterError, match=message):
                    cluster._spawn(state, root, 'p2', 'provider',
                                   [sys.executable, '-c', code], env, guard)
                row = cluster._read_state(root)['launches'][-1]
                assert not row.get('proof') and not row.get('spawn_failed')
            else:
                _, row = cluster._spawn(state, root, 'p2', 'provider',
                                        [sys.executable, '-c', code], env, guard)
                assert row['proof']['bindings'] == row['bindings']
                assert cluster._valid_intent(row, state)
                assert cluster.owned_state(row['proof']) == (
                    'EXITED' if observation == 'early-exit' else 'RUNNING')
                assert cluster._read_state(root)['launches'][-1]['proof'] == row['proof']
                if observation == 'initial-empty':
                    assert len(observations) >= 2
            assert len(children) == 1
        finally:
            # Fixture owns these exact real children; no production cleanup is bypassed.
            for child in children:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait(timeout=5)


def test_cpu_translation_is_cwd_independent_and_does_not_create_state(ops, deployment, monkeypatch):
    parser, _ = ops
    root, config = deployment
    path = write_config(root, config)
    before = sorted(str(item.relative_to(root)) for item in root.rglob("*"))
    monkeypatch.chdir(root.parent)
    translated = parser.load_deployment(path)
    assert translated["requester"]["core"] == {
        "requester_identity": "/test/requester", "authority_identity": "/test/controller",
        "group": "/test/group", "trust_schema_file": str(root / "trust.conf")}
    assert translated["requester"]["grant"]["authority_identity"] == "/test/authority"
    assert translated["requester"]["turns"][0]["output_file"] == str(root / "evidence_root/output.json")
    assert sorted(str(item.relative_to(root)) for item in root.rglob("*")) == before
    assert parser.check_deployment(path)["native_admission"] == "NOT_RUN"


@pytest.fixture
def conversation_deployment(deployment):
    root, config = deployment
    (root / "conversation.key").write_bytes(b"k" * 32)
    (root / "conversation.key").chmod(0o600)
    config["requester"]["request"] = {"security_policy_digest": "sha256:" + "a" * 64}
    config["requester"]["conversation"] = {
        "schema": "ndnsf-di-native-conversation-v1",
        "journal": {"identity": "requester", "state_root": "state_root/conversation",
                    "keys": [{"id": "active", "file": "conversation.key"}]},
        "owner": {"requester_identity": "/test/requester", "service_name": "/test/inference",
                  "security_domain_digest": "sha256:" + "a" * 64}}
    return root, config


def test_conversation_paths_are_relocated_without_creating_journal(ops, conversation_deployment, monkeypatch):
    parser, _ = ops
    root, config = conversation_deployment
    path = write_config(root, config)
    before = sorted(str(p.relative_to(root)) for p in root.rglob("*"))
    monkeypatch.chdir(root.parent)
    translated = parser.load_deployment(path)["requester"]["conversation"]
    assert translated["journal"]["state_root"] == str(root / "state_root/conversation")
    assert translated["journal"]["keys"][0]["file"] == str(root / "conversation.key")
    assert translated["owner"] == config["requester"]["conversation"]["owner"]
    assert sorted(str(p.relative_to(root)) for p in root.rglob("*")) == before


@pytest.mark.parametrize("case", ["journal", "owner", "keys", "key_file", "requester",
                                  "service", "security", "overlap", "identity_parent",
                                  "identity_separator"])
def test_conversation_missing_conflicting_and_overlapping_inputs_reject(
        ops, conversation_deployment, case):
    parser, _ = ops
    root, config = conversation_deployment
    conversation = config["requester"]["conversation"]
    if case in ("journal", "owner"):
        del conversation[case]
    elif case == "keys":
        conversation["journal"]["keys"] = []
    elif case == "key_file":
        conversation["journal"]["keys"][0]["file"] = "absent.key"
    elif case == "requester":
        conversation["owner"]["requester_identity"] = "/test/other"
    elif case == "service":
        conversation["owner"]["service_name"] = "/test/other"
    elif case == "security":
        conversation["owner"]["security_domain_digest"] = "sha256:" + "b" * 64
    elif case == "identity_parent":
        conversation["journal"]["identity"] = ".."
    elif case == "identity_separator":
        conversation["journal"]["identity"] = "other/requester"
    else:
        conversation["journal"]["state_root"] = str(root)
    with pytest.raises(parser.DeploymentConfigError):
        parser.load_deployment(write_config(root, config))
    assert not (root / "state_root").exists()


@pytest.mark.parametrize("field", ["output_file", "checkpoint_output_file"])
@pytest.mark.parametrize("file", ["journal.jsonl", "journal.lock"])
def test_conversation_outputs_cannot_overwrite_native_journal(
        ops, conversation_deployment, field, file):
    parser, _ = ops
    root, config = conversation_deployment
    config["requester"]["turns"][0][field] = "state_root/conversation/requester/" + file
    with pytest.raises(parser.DeploymentConfigError, match="output overlaps native conversation journal"):
        parser.load_deployment(write_config(root, config))
    assert not (root / "state_root").exists()


@pytest.mark.parametrize('identity', ['/test/p1', '/8=test/8=p1'])
def test_provider_grant_references_select_only_own_key(ops, deployment, identity):
    parser, _ = ops
    root, config = deployment
    (root / "trust-root-registry-v1.json").write_text('{}')
    mapping = root / "recipients.json"
    # A different actor's node-local private key need not exist on this node.
    mapping.write_text(json.dumps({"/8=test/8=p1": "key.pem", "/test/p0": "/absent/key.pem"}))
    actor = config["actors"][-1]
    actor['identity'] = identity
    actor["parameters"] = {"grant-authority-public-key": "key.pem",
                           "recipient-key-map": "recipients.json"}
    parsed = parser.load_deployment(write_config(root, config))
    params = parsed["actors"][-1]["parameters"]
    assert params["recipient-key-map"] == str(mapping)
    assert parser.provider_recipient_map(mapping, actor["identity"]) == {
        identity: str(root / "key.pem")}
    assert not (root / "state_root").exists()


@pytest.mark.parametrize('case,expected', [
    ('partial', 'required together'), ('registry', 'required input is unavailable'),
    ('identity', 'no recipient key'), ('duplicate', 'duplicate canonical'),
    ('key', 'private key file is unavailable'), ('value', 'expected nonempty text'),
])
def test_provider_grant_reference_failures_precede_mutation(ops, deployment, case, expected):
    parser, _ = ops
    root, config = deployment
    registry = root / "trust-root-registry-v1.json"
    registry.write_text('{}')
    mapping = root / "recipients.json"
    entries = {"/test/p1": "key.pem"}
    params = {"grant-authority-public-key": "key.pem", "recipient-key-map": "recipients.json"}
    if case == 'partial':
        del params['recipient-key-map']
    elif case == 'registry':
        registry.unlink()
    elif case == 'identity':
        entries = {"/test/p0": "key.pem"}
    elif case == 'duplicate':
        entries['/8=test/8=p1'] = 'key.pem'
    elif case == 'key':
        entries['/test/p1'] = 'absent.pem'
    else:
        entries['/test/p1'] = 7
    mapping.write_text(json.dumps(entries))
    config['actors'][-1]['parameters'] = params
    with pytest.raises(parser.DeploymentConfigError, match=expected):
        parser.load_deployment(write_config(root, config))
    assert not (root / 'state_root').exists()


def test_three_gpu_providers_and_explicit_sharing(ops, deployment):
    parser, _ = ops
    root, config = deployment
    third = copy.deepcopy(config["actors"][-1])
    third.update(id="p2", identity="/test/p2", credential_home="credentials/p2")
    (root / third["credential_home"]).mkdir()
    config["actors"].append(third)
    config["nodes"][0]["gpu_devices"] = ["0", "1", "2"]
    for index, actor in enumerate(config["actors"][-3:]):
        actor["resources"]["gpu_devices"] = [str(index)]
        actor["parameters"] = {"offer-backend": "onnxruntime-cuda"}
    parser.load_deployment(write_config(root, config))
    config["actors"][-1]["resources"]["gpu_devices"] = ["0"]
    with pytest.raises(parser.DeploymentConfigError, match="shared device"):
        parser.load_deployment(write_config(root, config))
    for actor in (config["actors"][-3], config["actors"][-1]):
        actor["resources"]["allow_sharing"] = True
    parser.load_deployment(write_config(root, config))


def test_cpu_overcommit_requires_every_reservation_to_opt_in(ops, deployment):
    parser, _ = ops
    root, config = deployment
    config["nodes"][0]["cpus"] = 1
    for actor in config["actors"]:
        actor["resources"]["allow_sharing"] = True
    parser.load_deployment(write_config(root, config))
    config["actors"][0]["resources"]["allow_sharing"] = False
    with pytest.raises(parser.DeploymentConfigError, match="CPU overcommit"):
        parser.load_deployment(write_config(root, config))


def test_library_link_identity_and_existing_native_provider_references(ops, deployment):
    parser, _ = ops
    root, config = deployment
    config["requester"]["request"] = {"provider_names": ["/test/p0"]}
    config["requester"]["fixed_placement"] = {"role_providers": {"stage-0": "/test/p1"}}
    library = root / "release/lib/libndn-service-framework.so"
    target = library.with_suffix(".so.0.1.0")
    library.rename(target)
    library.symlink_to(target.name)
    ledger_path = root / "installed.json"
    ledger = json.loads(ledger_path.read_text())
    ledger["files"][str(target)] = ledger["files"][str(library)]
    ledger["files"][str(library)] = {"installed": {"kind": "symlink", "mode": 0o777, "target": target.name}}
    ledger_path.write_text(json.dumps(ledger))
    path = write_config(root, config)
    parser.load_deployment(path)
    library.unlink()
    library.symlink_to("different.so")
    with pytest.raises(parser.DeploymentConfigError, match="library link identity mismatch"):
        parser.load_deployment(path)


def test_optional_owned_and_external_repo(ops, deployment):
    parser, _ = ops
    root, config = deployment
    repo = {"id": "repo", "kind": "repo", "identity": "/test/repo", "external": True}
    config["actors"].append(repo)
    config["network"]["repo_service"] = "/test/repo-service"
    parsed = parser.load_deployment(write_config(root, config))
    assert parsed["actors"][-1]["external"] is True
    (root / "credentials/repo").mkdir()
    repo.update(external=False, node="local", credential_home="credentials/repo", resources={"cpus": 1})
    parser.load_deployment(write_config(root, config))


@pytest.mark.parametrize("kind, payload", [("artifact-authority", "artifact_authority"),
                                            ("requester", "requester"), ("controller", None)])
def test_external_roles_do_not_require_private_process_inputs(ops, deployment, kind, payload):
    parser, _ = ops
    root, config = deployment
    actor = next(row for row in config["actors"] if row["kind"] == kind)
    for key in ("node", "credential_home", "resources"):
        actor.pop(key)
    actor["external"] = True
    if payload:
        config[payload] = {}
    else:
        config["security"].pop("controller_policy")
        (root / "policy.json").unlink()
    parsed = parser.load_deployment(write_config(root, config))
    assert next(row for row in parsed["actors"] if row["kind"] == kind)["external"]


def test_provider_production_options_retain_native_contracts(ops, deployment):
    parser, _ = ops
    root, config = deployment
    parameters = {
        "allow-selection-generation-options": True, "enable-admission-lease": True,
        "require-execution-lease": True, "execution-policy": "LEGACY_READY_SET_V1",
        "admission-lease-ttl-ms": 1000,
    }
    config["actors"][-1]["parameters"] = parameters
    parsed = parser.load_deployment(write_config(root, config))
    assert parsed["actors"][-1]["parameters"] == parameters
    parameters["require-execution-lease"] = False
    with pytest.raises(parser.DeploymentConfigError, match="requires execution lease"):
        parser.load_deployment(write_config(root, config))


def _tiger_config(root, config):
    image = root / "candidate.sif"
    image.write_bytes(b"image digest fixture, not a runnable SIF")
    record = {
        'schemaVersion': 'ndnsf-local-sif-build-v3', 'status': 'BUILT_UNQUALIFIED',
        'buildInput': {'method': 'local-apptainer-definition'},
        'sourceValidation': {'status': 'PASS'}, 'hostRole': 'apptainer-driver-only',
        'containerNativeBuild': {
            'status': 'PASS', 'schemaVersion': 'spec170-sif-build-boundary-v2',
            'containerNativeBuild': True, 'runtimeLayout': 'installed-v1',
            'cleanDependencyBaseRequired': True, 'staleBaseArtifactsReplaced': False,
            'hostBinaryInputs': [],
        },
        'sif': {'path': str(image), 'bytes': image.stat().st_size,
                'sha256': 'sha256:' + hashlib.sha256(image.read_bytes()).hexdigest()},
    }
    _write_tiger_record(root, record)
    config["platform"] = {
        "kind": "slurm-apptainer", "sif": "candidate.sif",
        "build_record": "build-record.json",
        "sif_sha256": "sha256:" + hashlib.sha256(image.read_bytes()).hexdigest(),
        "apptainer": sys.executable,
        "slurm": {"partition": "fixture", "account": "fixture", "wallTime": "00:10:00",
                  "tasksPerNode": 1, "cpusPerTask": 8, "memory": "8G", "gpu": {"type": "fixture", "count": 1}},
    }
    config["release"]["prefix"] = "/opt/remote-image-runtime"
    for obj in (config["security"], config["model"]):
        for key in obj:
            obj[key] = "/remote/inputs/" + obj[key]
    for key in config["storage"].keys() - {"max_bytes"}:
        config["storage"][key] = "/remote/" + key
    config["nodes"][0]["host"] = "allocation-node-0"
    config["nodes"][0]["nfd"]["socket"] = "/remote/nfd.sock"
    for actor in config["actors"]:
        actor["credential_home"] = "/remote/credentials/" + actor["id"]
    config["requester"]["grant"]["requester_private_key_file"] = "/remote/requester-private.pem"
    config["requester"]["grant"]["authority_public_key_file"] = "/remote/authority-public.pem"
    config["requester"]["catalog"]["source"]["file"] = "/remote/model.onnx"
    for turn in config["requester"]["turns"]:
        for key in turn:
            turn[key] = "/remote/" + turn[key]
    authority = config["artifact_authority"]["authority"]
    for key in ("authority_private_key_file", "requester_public_key_file", "content_key_file"):
        authority[key] = "/remote/" + authority[key]
    authority["recipient_public_key_files"] = {"/test/p0": "/remote/provider-public.pem"}
    return config


def _write_tiger_record(root, record):
    record.pop('recordDigest', None)
    record['recordDigest'] = 'sha256:' + hashlib.sha256(
        json.dumps(record, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    (root / 'build-record.json').write_text(json.dumps(record))


@pytest.mark.parametrize('mutation, expected', [
    ('missing', 'missing fields build_record'),
    ('record', 'DIGEST_MISMATCH'),
    ('image', 'SIF_HASH_MISMATCH'),
    ('native', 'BOUNDARY_INVALID'),
    ('status', 'NOT_PASS'),
])
def test_tiger_configuration_requires_actual_image_provenance(ops, deployment, mutation, expected):
    parser, _ = ops
    root, config = deployment
    _tiger_config(root, config)
    record = json.loads((root / 'build-record.json').read_text())
    if mutation == 'missing':
        del config['platform']['build_record']
    elif mutation == 'image':
        original = (root / 'candidate.sif').read_bytes()
        (root / 'candidate.sif').write_bytes(b'x' * len(original))
    elif mutation == 'record':
        record['hostRole'] = 'host-native-builder'
        (root / 'build-record.json').write_text(json.dumps(record))
    else:
        if mutation == 'native':
            record['containerNativeBuild']['hostBinaryInputs'] = ['/host/native.so']
        else:
            record['status'] = 'FAILED'
        _write_tiger_record(root, record)
    with pytest.raises(parser.DeploymentConfigError, match=expected):
        parser.load_deployment(write_config(root, config))


def test_host_rejects_container_build_record(ops, deployment):
    parser, _ = ops
    root, config = deployment
    config['platform']['build_record'] = 'build-record.json'
    with pytest.raises(parser.DeploymentConfigError, match='host cannot declare'):
        parser.load_deployment(write_config(root, config))


@pytest.mark.parametrize('reverse', [False, True])
def test_owned_credential_home_mounts_cannot_nest(ops, deployment, reverse):
    parser, _ = ops
    root, config = deployment
    first, second = config['actors'][:2]
    if reverse:
        first, second = second, first
    second['credential_home'] = first['credential_home'] + '/nested'
    (root / second['credential_home']).mkdir()
    with pytest.raises(parser.DeploymentConfigError, match='disjoint credential homes'):
        parser.load_deployment(write_config(root, config))


def test_tiger_distinct_nodes_can_use_same_node_local_credential_path(ops, deployment):
    parser, _ = ops
    root, config = deployment
    _tiger_config(root, config)
    node = copy.deepcopy(config['nodes'][0])
    node.update(id='second', host='allocation-node-1')
    node['nfd']['address'] = '192.0.2.2'
    config['nodes'].append(node)
    config['actors'][-1]['node'] = 'second'
    config['actors'][-1]['credential_home'] = config['actors'][0]['credential_home']
    result = parser.load_deployment(write_config(root, config))
    assert result['actors'][-1]['credential_home'] == result['actors'][0]['credential_home']
    assert result['actors'][-1]['node'] != result['actors'][0]['node']


def test_tiger_credential_home_cannot_alias_a_nested_linux_path(ops, deployment):
    parser, _ = ops
    root, config = deployment
    _tiger_config(root, config)
    config['actors'][1]['credential_home'] = '/' + config['actors'][0]['credential_home'] + '/nested'
    with pytest.raises(parser.DeploymentConfigError, match='node/runtime path must be absolute'):
        parser.load_deployment(write_config(root, config))


@pytest.fixture
def node_container(ops, deployment):
    parser, _ = ops
    root, raw = deployment
    (root / 'trust-root-registry-v1.json').write_text(json.dumps({
        'artifactPolicyAuthority': {'publicKeyPath': root.name + '/key.pem'}}))
    (root / 'credentials').chmod(0o700)
    for actor in raw['actors']:
        (root / actor['credential_home']).chmod(0o700)
        if actor['kind'] == 'provider':
            private = root / (actor['id'] + '-private.pem')
            private.write_text('isolated fixture private key')
            mapping = root / (actor['id'] + '-recipient.json')
            mapping.write_text(json.dumps({actor['identity']: str(private)}))
            actor['parameters'] = {'grant-authority-public-key': 'key.pem',
                                   'recipient-key-map': str(mapping)}
    for obj, key, name in (
            (raw['requester']['grant'], 'requester_private_key_file', 'requester-private.pem'),
            (raw['artifact_authority']['authority'], 'authority_private_key_file', 'issuer-private.pem'),
            (raw['artifact_authority']['authority'], 'content_key_file', 'content-key.bin')):
        (root / name).write_text('isolated private input')
        obj[key] = name
    socket_dir = root.parent / 'sockets' / hashlib.sha256(str(root).encode()).hexdigest()[:12]
    socket_dir.parent.mkdir(mode=0o700, exist_ok=True)
    socket_dir.mkdir(mode=0o700)
    raw['nodes'][0]['nfd']['socket'] = str(socket_dir / 'n')
    assert len(os.fsencode(raw['nodes'][0]['nfd']['socket'])) < 108
    config = parser.load_deployment(write_config(root, raw))
    tiger = _tiger_config(root, copy.deepcopy(raw))
    config['platform'] = tiger['platform']
    for key in ('sif', 'build_record'):
        config['platform'][key] = str(root / config['platform'][key])
    config['nodes'][0]['host'] = socket.gethostname()
    evidence = root / 'operation'
    evidence.mkdir(mode=0o700)
    frozen = evidence / 'configuration.json'
    container = importlib.import_module('_spec217_ops.container')
    pool = evidence / 'nodes' / config['nodes'][0]['id']
    container.cluster._private_directory(pool)
    # Mount-only cases do not consume a native observation. GPU admission
    # cases replace this placeholder with the fully bound native event.
    container.cluster.write_json(pool / 'gpu-allocation.json', {})
    endpoint = socket.socket(socket.AF_UNIX)
    endpoint.bind(raw['nodes'][0]['nfd']['socket'])
    capture = root / 'captured.json'
    fake = root / 'apptainer'
    fake.write_text('#!' + sys.executable + '\nimport json,os,sys\nfrom pathlib import Path\n'
                    'args=sys.argv[1:]\n'
                    'envfile=Path(args[args.index("--env-file")+1])\n'
                    'Path(' + repr(str(capture)) + ').write_text(json.dumps('
                    '{"argv":args,"envfile":envfile.read_text(),"host_env":dict(os.environ)}))\n')
    fake.chmod(0o755)
    config['platform']['apptainer'] = str(fake)
    boot = 'b' * 32

    def freeze():
        frozen.write_text(json.dumps(config))
        frozen.chmod(0o600)
        digest = hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        return ['--deployment', str(frozen), '--boot-id', boot, '--configuration-sha256', digest,
                '--release-sha256', hashlib.sha256(Path(config['release']['manifest']).read_bytes()).hexdigest()]

    def run(*extra, environment=None, workload=('/bin/true',)):
        wrapper = container.SCRIPTS / 'run-container.sh'
        poison = root / 'poison/ndnsf_distributed_inference'
        poison.mkdir(parents=True, exist_ok=True)
        (poison / '__init__.py').write_text('raise RuntimeError("POISONED_CHECKOUT_IMPORTED")\n')
        installed_root = Path(parser.__file__).parents[2]
        env = dict(os.environ, PYTHONPATH=str(poison.parent),
                   PATH=str(installed_root.parents[2] / 'bin') + os.pathsep + os.defpath,
                   SLURM_JOB_ID='12345', SLURM_STEP_ID='3',
                   NDNSF_SIF_CACHE_DIR=str(root / 'image-cache'),
                   APPTAINERENV_LD_LIBRARY_PATH='/undeclared/host/libs',
                   SINGULARITYENV_HOME='/foreign/credentials')
        env.update(environment or {})
        command = ['sh', str(wrapper), *freeze(), *extra]
        if workload:
            command += ['--', *workload]
        return subprocess.run(command,
                              env=env, capture_output=True, text=True, timeout=15)
    try:
        yield config, container, frozen, capture, run
    finally:
        endpoint.close()


def test_normalized_wrapper_scopes_actual_cpu_mounts_and_stages_exact_image(node_container):
    config, container, frozen, capture, run = node_container
    result = run('--actor', 'p0')
    assert result.returncode == 0, result.stderr
    item = json.loads(capture.read_text())
    argv = item['argv']
    assert '--cleanenv' in argv and '--containall' in argv and '--nv' not in argv
    assert argv[argv.index('--no-mount') + 1] == 'hostfs,bind-paths'
    binds = [argv[index + 1] for index, word in enumerate(argv) if word == '--bind']
    actor = next(actor for actor in config['actors'] if actor['id'] == 'p0')
    assert actor['credential_home'] + ':' + actor['credential_home'] + ':rw' in binds
    for path in config['model'].values():
        assert path + ':' + path + ':ro' in binds
    for other in config['actors']:
        if other != actor:
            assert not any(other['credential_home'] in bind for bind in binds)
    assert not any(config['release']['prefix'] in bind for bind in binds)
    assert not any(key.startswith(('APPTAINERENV_', 'SINGULARITYENV_')) for key in item['host_env'])
    assert 'CUDA_VISIBLE_DEVICES=\n' in item['envfile']
    assert 'NDNSF_DEPLOYMENT_ACTOR_ID=p0\n' in item['envfile']
    assert Path(argv[-2]).read_bytes() == Path(config['platform']['sif']).read_bytes()
    assert any('SIF_STAGE_VERIFY_PASS' in path.read_text()
               for path in frozen.parent.glob('actors/p0/staging/*/*.log'))


@pytest.mark.parametrize('actor_id', ['requester', 'repo'])
def test_native_large_data_directory_uses_owned_budgeted_disk(node_container, actor_id):
    config, container, frozen, capture, run = node_container
    if actor_id == 'repo':
        repo = copy.deepcopy(next(actor for actor in config['actors'] if actor['id'] == 'p0'))
        home = Path(repo['credential_home']).parent / actor_id
        container.cluster._private_directory(home)
        repo.update(id=actor_id, kind='repo', identity='/test/repo',
                    credential_home=str(home), parameters={})
        config['actors'].append(repo)
        config['network']['repo_service'] = '/test/repo-service'
    config['storage']['evidence_root'] = str(frozen.parent)
    result = run('--actor', actor_id)
    assert result.returncode == 0, result.stderr
    item = json.loads(capture.read_text())
    argv = item['argv']
    binds = [argv[index + 1] for index, word in enumerate(argv) if word == '--bind']
    source = frozen.parent / 'actors' / actor_id / 'large-data'
    assert str(source) + ':/tmp/ndnsf-large-data:rw' in binds
    assert not any(bind.endswith(':/tmp:rw') for bind in binds)
    assert source.stat().st_uid == os.getuid() and source.stat().st_mode & 0o7777 == 0o700
    assert 'NDNSF_REQUEST_LARGE_DATA_DIR=' not in item['envfile']
    before = container.cluster._check_budget(config)
    (source / 'publication.wire').write_bytes(b'x' * 1024)
    assert container.cluster._check_budget(config) == before + 1024


@pytest.mark.parametrize('bad', ['symlink', 'file', 'mode'])
def test_large_data_source_rejects_unsafe_directory_before_staging(node_container, bad):
    config, container, frozen, capture, run = node_container
    evidence = frozen.parent / 'actors/requester'
    container.cluster._private_directory(evidence)
    source = evidence / 'large-data'
    if bad == 'symlink':
        source.symlink_to(frozen.parent.parent, target_is_directory=True)
    elif bad == 'file':
        source.write_bytes(b'preserve unrelated file')
    else:
        source.mkdir(mode=0o755)
        source.chmod(0o755)
    result = run('--actor', 'requester')
    assert result.returncode != 0
    assert not capture.exists()
    assert not (evidence / 'staging').exists()
    if bad == 'file':
        assert source.read_bytes() == b'preserve unrelated file'


@pytest.mark.parametrize('other', ['/tmp', '/tmp/ndnsf-large-data/input'])
def test_large_data_target_rejects_intersecting_mount_plan(node_container, other):
    config, container, frozen, _, _ = node_container
    actor = next(item for item in config['actors'] if item['id'] == 'requester')
    evidence = frozen.parent / 'actors/requester'
    with pytest.raises(container.cluster.ClusterError, match='CONTAINER_LARGE_DATA_TARGET_CONFLICT'):
        container._large_data_mount(config, actor, evidence,
                                    {evidence: 'rw', Path(other): 'ro'})
    assert not (evidence / 'large-data').exists()


def test_journal_parent_checks_new_large_data_target_before_staging(node_container, monkeypatch):
    config, container, frozen, _, _ = node_container
    config['requester']['conversation'] = {'journal': {
        'state_root': '/tmp', 'identity': 'owned-journal', 'keys': []}}
    actor = next(item for item in config['actors'] if item['id'] == 'requester')
    # Model container destinations outside /tmp. The host fixture's release,
    # credentials and evidence all live below /tmp, which would reject this
    # journal root before the newly planned large-data mount is considered.
    config['release']['prefix'] = '/opt/ndnsf-test-runtime'
    home = Path('/var/lib/ndnsf-test/credentials')
    evidence = Path('/var/lib/ndnsf-test/evidence')
    mounts = {evidence: 'rw', home: 'rw', Path('/tmp/owned-journal'): 'rw'}
    created = []
    monkeypatch.setattr(container.cluster, '_private_directory', created.append)
    monkeypatch.setenv('SLURM_JOB_ID', '12345')
    monkeypatch.setattr(container, 'load_frozen', lambda *a: config)
    monkeypatch.setattr(container, 'role_mounts', lambda *a, **kw: (
        'requester', home, evidence, mounts))
    monkeypatch.setattr(container, 'stage_image', lambda *a, **kw: pytest.fail('staging reached'))
    args = SimpleNamespace(deployment=frozen, configuration_sha256='a' * 64,
                           boot_id='b' * 32, release_sha256='c' * 64,
                           actor='requester', management=None, node=None)
    with pytest.raises(container.cluster.ClusterError, match='CONTAINER_JOURNAL_PARENT_CONFLICT'):
        container.execute(args)
    assert created == [evidence / 'large-data']


def test_requester_journal_mount_preserves_state_and_hides_other_identities(node_container):
    config, container, frozen, capture, run = node_container
    root = frozen.parent.parent / 'conversation-journals'
    identity = root / 'requester'
    container.cluster._private_directory(identity)
    sibling = root / 'another-identity'
    container.cluster._private_directory(sibling)
    files = [identity / 'journal.jsonl', sibling / 'journal.jsonl']
    for path in files:
        path.write_bytes(b'original native journal bytes')
        path.chmod(0o600)
    original = [(path.stat().st_dev, path.stat().st_ino, path.read_bytes()) for path in files]
    config['requester']['conversation'] = {'journal': {
        'state_root': str(root), 'identity': 'requester', 'keys': []}}
    result = run('--actor', 'requester')
    assert result.returncode == 0, result.stderr
    argv = json.loads(capture.read_text())['argv']
    binds = [argv[index + 1] for index, word in enumerate(argv) if word == '--bind']
    source = frozen.parent / 'actors/requester/journal-parent'
    parent_bind = str(source) + ':' + str(root) + ':ro'
    child_bind = str(identity) + ':' + str(identity) + ':rw'
    assert binds.index(parent_bind) < binds.index(child_bind)
    assert not any(bind.startswith(str(root) + ':') for bind in binds)
    assert list(source.iterdir()) == [source / 'requester']
    assert list((source / 'requester').iterdir()) == []
    assert source.stat().st_mode & 0o7777 == 0o700
    assert (source / 'requester').stat().st_mode & 0o7777 == 0o700
    assert [(path.stat().st_dev, path.stat().st_ino, path.read_bytes())
            for path in files] == original


@pytest.mark.parametrize('bad', ['symlink', 'mode', 'extra-entry', 'nonempty', 'conflict'])
def test_requester_journal_parent_rejects_unsafe_plan_before_staging(node_container, bad):
    config, container, frozen, capture, run = node_container
    root = frozen.parent if bad == 'conflict' else frozen.parent.parent / 'conversation-journals'
    config['requester']['conversation'] = {'journal': {
        'state_root': str(root), 'identity': 'requester', 'keys': []}}
    source = frozen.parent / 'actors/requester/journal-parent'
    container.cluster._private_directory(source)
    if bad == 'symlink':
        (source / 'requester').symlink_to(frozen.parent.parent, target_is_directory=True)
    elif bad == 'mode':
        source.chmod(0o755)
    elif bad == 'extra-entry':
        (source / 'unexpected').write_bytes(b'do not expose')
    elif bad == 'nonempty':
        container.cluster._private_directory(source / 'requester')
        (source / 'requester/unexpected').write_bytes(b'do not expose')
    result = run('--actor', 'requester')
    assert result.returncode != 0
    assert not capture.exists()
    assert not (frozen.parent / 'actors/requester/staging').exists()
    if bad == 'conflict':
        assert 'CONTAINER_JOURNAL_PARENT_CONFLICT' in result.stderr


def test_requester_journal_parent_cannot_mask_image_runtime(node_container):
    config, container, frozen, _, _ = node_container
    root = Path(config['release']['prefix']).parent
    config['requester']['conversation'] = {'journal': {
        'state_root': str(root), 'identity': 'requester', 'keys': []}}
    actor = next(item for item in config['actors'] if item['id'] == 'requester')
    evidence = frozen.parent / 'actors/requester'
    with pytest.raises(container.cluster.ClusterError, match='CONTAINER_RUNTIME_OVERLAY_FORBIDDEN'):
        container._journal_root_mount(config, actor, evidence, {root / 'requester': 'rw'})
    assert not (evidence / 'journal-parent').exists()


def test_requester_repo_mounts_keep_native_sidecar_inode_and_contents(node_container):
    config, container, frozen, capture, run = node_container
    parent = Path(config['storage']['repo_root'])
    parent.mkdir(mode=0o700)
    paths = [parent / 'canonical', parent / 'encrypted']
    for key, path in zip(('repository', 'encrypted_repository'), paths):
        config['requester'][key] = {'path': str(path), 'max_bytes': 1024}
    first = Path(str(paths[0]) + '.authority.lock')
    first.write_bytes(b'previous native owner\n')
    first.chmod(0o600)
    identity = (first.stat().st_dev, first.stat().st_ino)
    result = run('--actor', 'requester')
    assert result.returncode == 0, result.stderr
    argv = json.loads(capture.read_text())['argv']
    binds = [argv[index + 1] for index, word in enumerate(argv) if word == '--bind']
    assert str(parent) + ':' + str(parent) + ':rw' not in binds
    for path in paths:
        lock = Path(str(path) + '.authority.lock')
        assert str(path) + ':' + str(path) + ':rw' in binds
        assert str(lock) + ':' + str(lock) + ':rw' in binds
        assert lock.stat().st_mode & 0o777 == 0o600
        assert lock.stat().st_uid == os.getuid() and lock.stat().st_nlink == 1
    assert first.read_bytes() == b'previous native owner\n'
    assert (first.stat().st_dev, first.stat().st_ino) == identity
    assert Path(str(paths[1]) + '.authority.lock').read_bytes() == b''


@pytest.mark.parametrize('bad', ['symlink', 'hardlink', 'directory', 'mode', 'owner'])
def test_requester_repo_lock_admission_precedes_any_creation(node_container, monkeypatch, bad):
    config, container, frozen, capture, run = node_container
    parent = frozen.parent / 'locks'
    parent.mkdir(mode=0o700)
    absent = parent / 'a.authority.lock'
    invalid = parent / 'b.authority.lock'
    if bad == 'directory':
        invalid.mkdir(mode=0o700)
    elif bad == 'symlink':
        original = parent / 'original'
        original.write_bytes(b'preserve original')
        invalid.symlink_to(original)
    else:
        invalid.write_bytes(b'preserve native owner')
        invalid.chmod(0o600)
        if bad == 'hardlink':
            os.link(invalid, parent / 'alias')
        elif bad == 'mode':
            invalid.chmod(0o644)
        elif bad == 'owner':
            uid = os.getuid()
            monkeypatch.setattr(container.os, 'getuid', lambda: uid + 1)
    with pytest.raises(container.cluster.ClusterError, match='CONTAINER_.*INVALID'):
        container._prepare_repository_locks({absent, invalid})
    assert not absent.exists()
    if bad not in ('directory', 'symlink'):
        assert invalid.read_bytes() == b'preserve native owner'
    if bad == 'symlink':
        assert original.read_bytes() == b'preserve original'


@pytest.mark.parametrize('bad', ['foreign-home', 'readonly-input'])
def test_requester_repo_locks_wait_for_complete_mount_admission(node_container, bad):
    config, container, frozen, capture, run = node_container
    parent = Path(config['storage']['repo_root'])
    parent.mkdir(mode=0o700)
    first = parent / 'canonical'
    config['requester']['repository'] = {'path': str(first), 'max_bytes': 1024}
    lock = Path(str(first) + '.authority.lock')
    if bad == 'foreign-home':
        foreign = next(actor for actor in config['actors'] if actor['id'] == 'p0')
        config['requester']['encrypted_repository'] = {
            'path': str(Path(foreign['credential_home']) / 'store'), 'max_bytes': 1024}
        expected = 'CONTAINER_FOREIGN_CREDENTIAL_EXPOSURE'
    else:
        lock.write_bytes(b'read-only declared input')
        lock.chmod(0o600)
        config['requester']['catalog']['source']['file'] = str(lock)
        expected = 'CONTAINER_BIND_MODE_CONFLICT'
    result = run('--actor', 'requester')
    assert result.returncode != 0 and expected in result.stderr
    assert not capture.exists()
    if bad == 'foreign-home':
        assert not lock.exists()
    else:
        assert lock.read_bytes() == b'read-only declared input'


def test_normalized_wrapper_scopes_multigpu_by_ordered_admitted_uuids(node_container):
    config, container, frozen, capture, run = node_container
    next(actor for actor in config['actors'] if actor['id'] == 'p0')['resources']['gpu_devices'] = ['allocated:1', 'allocated:0']
    config['nodes'][0]['gpu_devices'] = ['allocated:0', 'allocated:1']
    config['platform']['slurm']['gpu']['count'] = 2
    _write_node_gpu_observation(config, container, frozen)
    result = run('--actor', 'p0', environment={'CUDA_VISIBLE_DEVICES': '5,7', 'SLURM_STEP_GPUS': '5,7'})
    assert result.returncode == 0, result.stderr
    item = json.loads(capture.read_text())
    assert '--nv' in item['argv']
    assert 'CUDA_VISIBLE_DEVICES=GPU-1111-abcd,GPU-3333-abcd\n' in item['envfile']
    assert 'SLURM_STEP_GPUS=5,7\n' in item['envfile']


def test_normalized_wrapper_accepts_own_recipient_name_text_alias(node_container):
    config, _, _, capture, run = node_container
    actor = next(actor for actor in config['actors'] if actor['id'] == 'p0')
    path = Path(actor['parameters']['recipient-key-map'])
    original = json.loads(path.read_text())
    path.write_text(json.dumps({'/8=test/8=p0': original[actor['identity']]}))
    result = run('--actor', 'p0')
    assert result.returncode == 0, result.stderr
    assert capture.exists()


def test_node_staging_timeout_closes_real_shell_and_worker(node_container, monkeypatch):
    config, container, frozen, _, _ = node_container
    helper_root = frozen.parent / 'controlled-helper'
    helper_root.mkdir(mode=0o700)
    pid_file = helper_root / 'worker.pid'
    (helper_root / 'stage-sif.sh').write_text(
        '#!/bin/sh\nsleep 60 &\nchild=$!\n'
        'trap \'kill "$child" 2>/dev/null; wait "$child"; exit 143\' TERM\n'
        'printf "%s\\n" "$child" > ' + str(pid_file) + '\nwait "$child"\n')
    monkeypatch.setattr(container, 'SCRIPTS', helper_root)
    actor = next(actor for actor in config['actors'] if actor['id'] == 'p0')
    evidence = frozen.parent / 'actors/p0'
    container.cluster._private_directory(evidence)
    args = SimpleNamespace(boot_id='b' * 32, configuration_sha256='a' * 64, timeout=0.3)
    with pytest.raises(subprocess.TimeoutExpired):
        container.stage_image(config, args, evidence, actor['credential_home'],
                              actor['id'], config['nodes'][0]['nfd']['socket'])
    state = json.loads((evidence / 'staging' / config['deployment_id'] / 'state.json').read_text())
    assert state['status'] == 'STOPPED'
    assert state['cleanup'][0]['status'] == 'ZERO'
    assert not Path('/proc', pid_file.read_text().strip()).exists()
    assert container.cluster.owned_state(state['launches'][0]['proof']) == 'EXITED'


@pytest.mark.parametrize('status', ['STARTING', 'UNKNOWN', 'lock-held'])
def test_node_staging_preserves_prior_ownership_and_operation_lock(node_container, monkeypatch, status):
    config, container, frozen, _, _ = node_container
    actor = next(actor for actor in config['actors'] if actor['id'] == 'p0')
    evidence = frozen.parent / 'actors/p0'
    root = evidence / 'staging' / config['deployment_id']
    container.cluster._private_directory(root)
    args = SimpleNamespace(boot_id='b' * 32, configuration_sha256='a' * 64, timeout=0.3)
    old = {'schema': 'ndnsf-di-host-operation-v1', 'deployment_id': config['deployment_id'],
           'boot_id': args.boot_id, 'configuration_sha256': args.configuration_sha256,
           'evidence_root': str(root), 'status': 'UNKNOWN' if status == 'lock-held' else status,
           'launches': []}
    container.cluster.write_json(root / 'state.json', old)
    before = (root / 'state.json').read_bytes()

    def forbidden_spawn(*args, **kwargs):
        raise AssertionError('unfinished staging ownership was replaced')

    monkeypatch.setattr(container.cluster, '_spawn', forbidden_spawn)
    lock = container.cluster._lock(root) if status == 'lock-held' else contextlib.nullcontext()
    with lock:
        with pytest.raises(container.cluster.ClusterError, match='DEPLOYMENT_BUSY|SIF_STAGE_PREVIOUS_UNCLOSED'):
            container.stage_image(config, args, evidence, actor['credential_home'],
                                  actor['id'], config['nodes'][0]['nfd']['socket'])
    assert (root / 'state.json').read_bytes() == before


@pytest.mark.parametrize('mutation, expected', [
    ('config', 'CONFIGURATION_MISMATCH'), ('release', 'RELEASE_MANIFEST_MISMATCH'),
    ('mode', 'FROZEN_FILE_NOT_PRIVATE'), ('boot', 'FROZEN_BINDING_INVALID'),
])
def test_node_frozen_input_requires_all_operation_bindings(node_container, mutation, expected):
    config, container, frozen, _, run = node_container
    assert run('--actor', 'p0').returncode == 0
    digest = hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    release = hashlib.sha256(Path(config['release']['manifest']).read_bytes()).hexdigest()
    boot = 'b' * 32
    if mutation == 'config':
        changed = copy.deepcopy(config)
        changed['deployment_id'] = 'changed'
        frozen.write_text(json.dumps(changed))
    elif mutation == 'release':
        release = '0' * 64
    elif mutation == 'mode':
        frozen.chmod(0o644)
    else:
        boot = 'not-an-operation-boot'
    with pytest.raises(container.cluster.ClusterError, match=expected):
        container.load_frozen(frozen, digest, boot, release)


@pytest.mark.parametrize('visibility', ['', '2,2', '2,3', '-1', 'GPU-a,,GPU-b'])
def test_normalized_wrapper_rejects_bad_step_gpu_before_staging(node_container, visibility):
    config, _, frozen, capture, run = node_container
    next(actor for actor in config['actors'] if actor['id'] == 'p0')['resources']['gpu_devices'] = ['0']
    config['nodes'][0]['gpu_devices'] = ['0']
    result = run('--actor', 'p0', environment={'CUDA_VISIBLE_DEVICES': visibility})
    assert result.returncode != 0
    assert 'CONTAINER_STEP_GPU_VISIBILITY_INVALID' in result.stderr
    assert not capture.exists()
    assert not (frozen.parent.parent / 'image-cache').exists()


@pytest.mark.parametrize('mutation, expected', [
    ('record', 'DIGEST_MISMATCH'), ('image', 'SIF_DIGEST_MISMATCH'),
    ('node', 'ALLOCATION_NODE_MISMATCH'), ('home', 'aliased-home'),
    ('ancestor', 'untrusted ancestor'),
    ('output', 'FOREIGN_CREDENTIAL_EXPOSURE'), ('overlay', 'RUNTIME_OVERLAY_FORBIDDEN'),
])
def test_normalized_wrapper_rejects_incorrect_node_inputs(node_container, mutation, expected):
    config, _, frozen, capture, run = node_container
    actor_id = 'p0'
    if mutation == 'record':
        path = Path(config['platform']['build_record'])
        record = json.loads(path.read_text())
        record['hostRole'] = 'host-build'
        path.write_text(json.dumps(record))
    elif mutation == 'image':
        path = Path(config['platform']['sif'])
        path.write_bytes(b'x' * path.stat().st_size)
    elif mutation == 'node':
        config['nodes'][0]['host'] = 'wrong-allocation-node'
    elif mutation == 'home':
        actor = next(actor for actor in config['actors'] if actor['id'] == actor_id)
        link = frozen.parent.parent / 'aliased-home'
        link.symlink_to(actor['credential_home'])
        actor['credential_home'] = str(link)
    elif mutation == 'ancestor':
        Path(config['actors'][0]['credential_home']).parent.chmod(0o775)
    elif mutation == 'output':
        actor_id = 'requester'
        config['requester']['turns'][0]['output_file'] = str(
            Path(config['actors'][0]['credential_home']).parent / 'output.json')
    else:
        config['model']['plan'] = config['release']['prefix'] + '/runtime-file'
    result = run('--actor', actor_id)
    assert result.returncode != 0
    assert expected in result.stderr
    assert not capture.exists()


@pytest.mark.parametrize('management', ['nfd', 'nfdc'])
def test_normalized_management_uses_separate_home_and_socket_mount(node_container, management):
    config, _, _, capture, run = node_container
    result = run('--management', management, '--node', 'local')
    assert result.returncode == 0, result.stderr
    item = json.loads(capture.read_text())
    argv = item['argv']
    binds = [argv[index + 1] for index, word in enumerate(argv) if word == '--bind']
    for actor in config['actors']:
        assert not any(actor['credential_home'] in bind for bind in binds)
    endpoint = config['nodes'][0]['nfd']['socket']
    target = str(Path(endpoint).parent) if management == 'nfd' else endpoint
    mode = 'rw' if management == 'nfd' else 'ro'
    assert target + ':' + target + ':' + mode in binds
    assert '--nv' not in argv


def test_normalized_wrapper_cannot_start_an_external_forwarder(node_container):
    config, _, _, capture, run = node_container
    config['nodes'][0]['nfd']['mode'] = 'external'
    result = run('--management', 'nfd', '--node', 'local')
    assert result.returncode != 0
    assert 'CONTAINER_EXTERNAL_NFD_NOT_OWNED' in result.stderr
    assert not capture.exists()


def test_wrapper_default_enters_image_installed_native_owner(node_container):
    config, _, _, capture, run = node_container
    result = run('--actor', 'p0', workload=None)
    assert result.returncode == 0, result.stderr
    item = json.loads(capture.read_text())
    argv = item['argv']
    entry = str(Path(config['release']['prefix']) / 'bin/ndnsf-di')
    assert entry in argv and argv[argv.index(entry) + 1] == '_node'
    assert 'PATH=' not in item['envfile']
    ledger = config['release']['manifest']
    assert ledger + ':' + ledger + ':ro' in argv


def _role_args(config, frozen, actor='requester', management=None, timeout=1):
    return SimpleNamespace(deployment=frozen, boot_id='b' * 32,
        configuration_sha256=hashlib.sha256(
            json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
        release_sha256=hashlib.sha256(Path(config['release']['manifest']).read_bytes()).hexdigest(),
        actor=actor, management=management, node='local' if management else None,
        timeout=timeout, debug=False)


def _role_environment(monkeypatch, args, role_id):
    for key, value in {'NDNSF_DEPLOYMENT_ACTOR_ID': role_id,
                       'NDNSF_DEPLOYMENT_BOOT_ID': args.boot_id,
                       'NDNSF_DEPLOYMENT_CONFIG_SHA256': args.configuration_sha256,
                       'SLURM_JOB_ID': '12345', 'SLURM_STEP_ID': '3'}.items():
        monkeypatch.setenv(key, value)


@pytest.mark.parametrize('mode', ['complete', 'wrong-ready', 'service-exit', 'service-stop',
                                 'timeout', 'identity-fail', 'residual-worker', 'cancel-ready',
                                 'cancel-pre-ready', 'cancel-requester', 'stale-cancel',
                                 'malformed-cancel'])
def test_inner_owner_tracks_real_child_readiness_terminal_and_cleanup(node_container, monkeypatch, mode):
    config, container, frozen, _, run = node_container
    node = importlib.import_module('_spec217_ops.node')
    monkeypatch.setattr(node, '_CANCEL_CHECK_INTERVAL', 0.01)
    cancel_checks = []
    if mode == 'timeout':
        monkeypatch.setattr(node, '_CANCEL_CHECK_INTERVAL', 1.0)
        original_cancelled = container.cluster._cancelled

        def check_cancelled(*args):
            cancel_checks.append(time.monotonic())
            return original_cancelled(*args)

        monkeypatch.setattr(container.cluster, '_cancelled', check_cancelled)
    identity = Path(config['release']['prefix']) / 'bin/ndnsf-identity-check'
    identity.write_text('#!/bin/sh\nexit ' + ('9' if mode == 'identity-fail' else '0') + '\n')
    ledger_path = Path(config['release']['manifest'])
    ledger = json.loads(ledger_path.read_text())
    ledger['files'][str(identity)]['installed']['sha256'] = hashlib.sha256(identity.read_bytes()).hexdigest()
    ledger_path.write_text(json.dumps(ledger))
    actor_id = ('p0' if mode in ('service-exit', 'service-stop', 'wrong-ready', 'cancel-ready',
                               'cancel-pre-ready', 'stale-cancel', 'malformed-cancel') else 'requester')
    assert run('--actor', actor_id).returncode == 0
    args = _role_args(config, frozen, actor=actor_id, timeout=0.4)
    _role_environment(monkeypatch, args, actor_id)
    original = container.cluster._native_launch
    spawned = []

    def launch(current, actor, directory, state, **kwargs):
        _, env = original(current, actor, directory, state, **kwargs)
        record = {'event': 'NDNSF_DEPLOYMENT_READY', 'identity': actor['identity'],
                  'actor_id': actor['id'], 'boot_id': state['boot_id'],
                  'config_sha256': state['configuration_sha256'], 'native_boot_id': 'controlled-child'}
        if mode == 'wrong-ready':
            record['boot_id'] = 'wrong-boot'
        code = ('import json,os,time; item=' + repr(record) + '; item["pid"]=os.getpid(); '
                'print(json.dumps(item),flush=True); ' +
                ('time.sleep(60)' if mode in ('wrong-ready', 'timeout') else 'time.sleep(0.03)'))
        if actor['kind'] == 'requester':
            code = 'import time; ' + ('time.sleep(60)' if mode == 'timeout' else 'time.sleep(0.03)')
        if mode == 'service-stop':
            code += '; time.sleep(0.1); import signal; os.kill(os.getppid(),signal.SIGTERM); time.sleep(60)'
        if mode in ('cancel-ready', 'cancel-pre-ready', 'cancel-requester', 'stale-cancel',
                    'malformed-cancel'):
            marker = (frozen.parent / 'actors' / actor_id / 'runtime' /
                      config['deployment_id'] / 'cancel.json')
            body = ('{' if mode == 'malformed-cancel' else
                    json.dumps({'boot_id': 'old-boot' if mode == 'stale-cancel' else args.boot_id}))
            if mode == 'cancel-pre-ready':
                code = 'import time; time.sleep(0.03)'
            code += ('; time.sleep(0.12); from pathlib import Path; Path(' + repr(str(marker)) +
                     ').write_text(' + repr(body) + '); time.sleep(60)')
        if mode == 'residual-worker':
            code = ('import os,signal,time\nread,write=os.pipe()\npid=os.fork()\n'
                    'if pid == 0:\n os.close(read)\n signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
                    ' os.write(write,b"R")\n time.sleep(60)\n os._exit(0)\n'
                    'os.close(write)\nos.read(read,1)\ntime.sleep(0.03)\n')
        return [sys.executable, '-c', code], env

    real_spawn = container.cluster._spawn

    def spawn(*argv, **kwargs):
        child, row = real_spawn(*argv, **kwargs)
        spawned.append(child)
        return child, row

    monkeypatch.setattr(container.cluster, '_native_launch', launch)
    monkeypatch.setattr(container.cluster, '_spawn', spawn)
    result = node.run_role(args)
    assert result['status'] == 'STOPPED'
    assert result['host_audit'] == 'UNKNOWN' and result['cleanup_scope'] == 'PID_NAMESPACE'
    assert result['pid_namespace'] == os.readlink('/proc/self/ns/pid')
    assert result['job_id'] == '12345' and result['step_id'] == '3'
    assert all(item['status'] == 'ZERO' for item in result['cleanup'])
    assert all(container.cluster.owned_state(row['proof']) == 'EXITED' for row in result['launches'])
    if mode == 'complete':
        assert result['readiness'] == 'FINITE_EXITED' and result['execution'] == 'COMPLETE'
        assert result['exit_code'] == 0 and result['failure'] is None
    elif mode == 'service-stop':
        assert result['readiness'] == 'NATIVE_READY' and result['execution'] == 'SERVICE_STOPPED'
        assert result['exit_code'] == -signal.SIGTERM and result['failure'] is None
    elif mode == 'cancel-ready':
        assert result['readiness'] == 'NATIVE_READY'
        assert result['execution'] == 'SERVICE_STOPPED' and result['failure'] is None
    elif mode in ('cancel-pre-ready', 'cancel-requester'):
        assert result['execution'] == 'INTERRUPTED'
        assert result['failure'] == 'DEPLOYMENT_CANCELLED'
    else:
        assert result['execution'] == 'FAILED' and result['failure']
        if mode == 'wrong-ready':
            assert result['readiness'] == 'UNVERIFIED'
        if mode == 'identity-fail':
            assert len(result['launches']) == 1
        if mode == 'residual-worker':
            assert result['exit_code'] == 0
            assert result['failure'] == 'REQUESTER_GROUP_CLEANUP:ZERO'
            assert next(item for item in result['cleanup'] if item['id'] == actor_id)['forced']
        if mode == 'stale-cancel':
            assert result['failure'].startswith('NATIVE_ROLE_TIMEOUT:')
        if mode == 'malformed-cancel':
            assert result['failure'] != 'DEPLOYMENT_CANCELLED'
        if mode == 'timeout':
            assert len(cancel_checks) == 1
    receipt = frozen.parent / 'actors' / actor_id / 'receipt.json'
    before = receipt.read_bytes()
    with pytest.raises(container.cluster.ClusterError, match='NATIVE_ROLE_ALREADY_ENTERED'):
        node.run_role(args)
    assert receipt.read_bytes() == before
    assert all(child.poll() is not None for child in spawned)


@pytest.mark.parametrize('physical', ['5', 'GPU-1234-abcd'])
def test_native_launch_preserves_physical_mask_and_offers_logical_cuda(node_container, physical):
    config, container, frozen, _, _ = node_container
    actor = next(actor for actor in config['actors'] if actor['id'] == 'p0')
    actor['resources']['gpu_devices'] = [physical]
    directory = frozen.parent / 'translation'
    directory.mkdir(mode=0o700)
    argv, env = container.cluster._native_launch(config, actor, directory,
        {'boot_id': 'b' * 32, 'configuration_sha256': 'a' * 64}, visible_devices=physical)
    assert env['CUDA_VISIBLE_DEVICES'] == physical
    assert argv[argv.index('--offer-device') + 1] == 'cuda:0'


@pytest.fixture
def allocation_facility(node_container, monkeypatch):
    """Replace only external Slurm/SIF facilities with real owned POSIX children."""
    config, container, frozen, _, run = node_container
    assert run('--actor', 'p0').returncode == 0
    module = importlib.import_module('_spec217_ops.allocation')
    args = _role_args(config, frozen, actor=None, timeout=6)
    args.routes_only = False
    args.audit_node = args.observe_role = None
    args.evidence = frozen.parent
    monkeypatch.setenv('SLURM_JOB_ID', '12345')
    monkeypatch.setenv('SLURM_JOB_NODELIST', config['nodes'][0]['host'])
    installed_root = Path(container.__file__).parents[2]
    python = str(installed_root.parents[2] / 'bin/python3')
    monkeypatch.setattr(module, 'sys', SimpleNamespace(executable=python))
    events = frozen.parent / 'facility-events.jsonl'
    helper = frozen.parent / 'facility.py'
    helper.write_text('''import json,os,signal,socket,sys,time
from pathlib import Path
os.umask(0o077)
payload=json.loads(sys.argv[1]); root=Path(payload['root']); mode=payload['mode']
role=payload.get('role'); kind=payload.get('kind'); node=payload.get('node')
def write(path,value):
 path.parent.mkdir(parents=True,exist_ok=True,mode=0o700)
 temporary=path.with_suffix('.tmp'); temporary.write_text(json.dumps(value)); temporary.chmod(0o600)
 temporary.replace(path)
def event(value):
 fd=os.open(root/'facility-events.jsonl',os.O_CREAT|os.O_WRONLY|os.O_APPEND,0o600)
 os.write(fd,(json.dumps(value)+'\\n').encode()); os.close(fd)
base=payload['binding']
if payload['facility']=='hosts':
 print(payload['host'],flush=True); time.sleep(0.03); sys.exit(0)
if payload['facility']=='gpu':
 value=dict(base,schema='ndnsf-di-gpu-probe-v1',event='NDNSF_DI_GPU_PROBE',role_id=payload['probe_role'],
            step_id='3',step_gpus='2,6',device_order='PCI_BUS_ID',host=socket.gethostname(),visibility='0,1',
            visible_uuids=['GPU-3333-abcd','GPU-5555-abcd'],
            inventory=[dict(physical_id='5',uuid='GPU-5555-abcd'),dict(physical_id='3',uuid='GPU-3333-abcd')])
 if mode=='gpu-wrong-binding': value['boot_id']='wrong-boot'
 print(json.dumps(value),flush=True); event(['gpu',node]); time.sleep(0.03); sys.exit(0)
if payload['facility']=='audit':
 suffix=role or 'final'
 value=dict(base,schema='ndnsf-di-host-node-audit-v1',node_id=node,
            host=socket.gethostname(),processes=[],errors=[],
            status='UNKNOWN' if mode=='audit-fail' else ('OBSERVED' if role else 'ZERO'))
 if role:
  receipt=json.loads(Path(payload['native_receipt']).read_text())
  proof=dict(receipt['launches'][0]['proof'],nspid=[receipt['launches'][0]['proof']['pid']],
             pid_namespace=receipt['pid_namespace'],cgroup='0::/slurm/job_12345/step_3')
  value.update(role_id=role,native_process=proof)
 write(root/'nodes'/node/('host-audit-'+suffix+'.json'),value)
 event(['audit',suffix]); time.sleep(0.03); sys.exit(0 if value['status']!='UNKNOWN' else 1)
if payload['facility']=='routes':
 value=dict(base,schema='ndnsf-di-host-operation-v1',status='STOPPED',execution='COMPLETE',
            roles=payload['roles'])
 for item in payload['roles']:
  receipt=dict(base,schema='ndnsf-di-node-role-v1',role_id=item['id'],role_kind='nfdc',
               node_id=item['node'],step_id='3',cleanup_scope='PID_NAMESPACE',readiness='MANAGEMENT_VERIFIED',
               status='STOPPED',execution='COMPLETE',failure=None,cleanup=[],exit_code=0)
  write(root/'nodes'/item['node']/'nfdc'/'receipt.json',receipt)
 write(root/'routes.json',value); event(['routes']); time.sleep(0.03); sys.exit(0)
if mode=='pre-entry-fail' and kind=='controller':
 event(['pre-entry-fail',role]); time.sleep(0.03); sys.exit(8)
path=(root/'nodes'/node/kind/'receipt.json' if kind in ('nfd','nfdc') else root/'actors'/role/'receipt.json')
value=dict(base,schema='ndnsf-di-node-role-v1',role_id=role,role_kind=kind,node_id=node,
           cleanup_scope='PID_NAMESPACE',status='SERVICES_READY',execution='RUNNING',failure=None,
           readiness='SOCKET_BOUND' if kind=='nfd' else 'NATIVE_READY',cleanup=[],exit_code=None)
proof=dict(pid=os.getpid(),uid=os.getuid(),start_ticks=int(Path('/proc/self/stat').read_text().rsplit(')',1)[1].split()[19]),
           exe=os.readlink('/proc/self/exe'),bindings={'NDNSF_DEPLOYMENT_ACTOR_ID':role,
             'NDNSF_DEPLOYMENT_BOOT_ID':base['boot_id'],
             'NDNSF_DEPLOYMENT_CONFIG_SHA256':base['configuration_sha256']})
value.update(step_id='3',pid_namespace=os.readlink('/proc/self/ns/pid'),launches=[dict(id=role,proof=proof)])
if kind=='requester': value.update(status='REQUEST_RUNNING',readiness='FINITE_RUNNING')
if mode=='pre-ready-cancel' and kind=='controller':
 value.update(status='STARTING',execution='STARTING',readiness='UNVERIFIED')
if mode=='wrong-ready' and kind=='controller': value['boot_id']='wrong-boot'
if mode=='service-fail' and kind=='controller':
 value.update(status='STOPPED',execution='FAILED',failure='controlled native service failure')
 write(path,value); event(['role',role]); time.sleep(0.03); sys.exit(9)
def stop(signum,frame):
 value.update(status='STOPPED',execution='INTERRUPTED' if kind=='requester' else 'SERVICE_STOPPED',
              exit_code=-signum)
 write(path,value); event(['stopped',role]); sys.exit(0)
signal.signal(signal.SIGTERM,stop)
runtime=path.parent/'runtime'/payload['deployment_id']
runtime.mkdir(parents=True,exist_ok=True,mode=0o700)
write(path,value); event(['role',role])
if mode=='service-degraded' and kind=='controller':
 time.sleep(0.08); value.update(execution='FAILED',failure='native child failed during drain'); write(path,value)
if kind=='nfdc':
 value.update(status='STOPPED',execution='COMPLETE',readiness='MANAGEMENT_VERIFIED',exit_code=0)
 write(path,value); time.sleep(0.03); sys.exit(0)
if kind=='requester' and mode=='signal': os.kill(os.getppid(),signal.SIGTERM)
if kind=='requester' and mode not in ('timeout','signal'):
 time.sleep(0.25)
 code=7 if mode=='requester-fail' else 0
 value.update(status='STOPPED',execution='FAILED' if code else 'COMPLETE',exit_code=code)
 write(path,value); sys.exit(code)
while True:
 marker=runtime/'cancel.json'
 if mode!='ignore-cancel' and marker.exists() and json.loads(marker.read_text()).get('boot_id')==base['boot_id']:
  stop(signal.SIGTERM,None)
 time.sleep(0.05)
''')
    real_spawn = container.cluster._spawn
    children = []
    scenario = {'mode': 'complete'}

    def spawn(state, root, tag, kind, command, env, guard):
        payload = dict(root=str(frozen.parent), mode=scenario['mode'],
                       deployment_id=config['deployment_id'], binding={
            'boot_id': args.boot_id, 'configuration_sha256': args.configuration_sha256,
            'release_sha256': args.release_sha256, 'job_id': '12345'})
        if command[0].endswith('scontrol'):
            payload.update(facility='hosts', host=config['nodes'][0]['host'])
        elif '--routes-only' in command:
            payload.update(facility='routes', roles=[role for role in module._roles(config)
                                                     if role['kind'] == 'nfdc'])
        elif '--audit-node' in command:
            payload.update(facility='audit', node=command[command.index('--audit-node') + 1],
                           role=command[command.index('--observe-role') + 1]
                                if '--observe-role' in command else None)
            if payload['role']:
                native = next(role for role in module._roles(config) if role['id'] == payload['role'])
                payload['native_receipt'] = str(module._receipt_path(args, native))
        elif '--management' in command and command[command.index('--management') + 1] == 'gpu-probe':
            payload.update(facility='gpu', node=command[command.index('--node') + 1], probe_role=tag)
        else:
            if '--actor' in command:
                role = next(role for role in config['actors']
                            if role['id'] == command[command.index('--actor') + 1])
            else:
                node_id = command[command.index('--node') + 1]
                role_kind = command[command.index('--management') + 1]
                role = dict(id=container.management_role_id(role_kind, node_id),
                            node=node_id, kind=role_kind)
            payload.update(facility='role', role=role['id'], node=role['node'], kind=role['kind'])
        child, row = real_spawn(state, root, tag, kind,
                               [sys.executable, str(helper), json.dumps(payload)], env, guard)
        children.append(child)
        child.allocation_argv = list(command)
        return child, row

    monkeypatch.setattr(container.cluster, '_spawn', spawn)
    # External facilities are substituted after argv construction. The actual
    # production process ownership, waiting, receipts and teardown remain active.
    monkeypatch.setattr(module, '_executable', lambda command: [
        '/usr/bin/' + command[0] if command[0] in ('srun', 'scontrol') else command[0], *command[1:]])
    try:
        yield config, module, args, scenario, events, children
    finally:
        for child in children:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=5)


@pytest.mark.parametrize('mode', ['complete', 'wrong-ready', 'service-fail', 'service-degraded',
                                 'requester-fail', 'audit-fail', 'timeout', 'signal', 'pre-entry-fail',
                                 'ignore-cancel', 'pre-ready-cancel'])
def test_allocation_owner_orders_real_steps_and_keeps_failure_boundaries(
        allocation_facility, monkeypatch, mode):
    config, module, args, scenario, events, children = allocation_facility
    scenario['mode'] = mode
    if mode in ('timeout', 'pre-ready-cancel'):
        args.timeout = 2
    if mode == 'ignore-cancel':
        monkeypatch.setattr(module, '_ROLE_STOP_GRACE_SECONDS', 0.1)
    result = module.run_allocation(args, config)
    rows = [json.loads(line) for line in events.read_text().splitlines()]
    if mode == 'complete':
        assert result['status'] == 'STOPPED' and result['execution'] == 'COMPLETE'
        assert result['failure'] is None and result['oracle'] == 'NOT_RUN'
        launches = [row[1] for row in rows if row[0] == 'role']
        assert launches == [module.management_role_id('nfd', config['nodes'][0]['id']),
                            'controller', 'authority', 'p0', 'p1', 'requester']
        assert rows.index(['routes']) < rows.index(['role', 'controller'])
        assert rows.index(['audit', 'controller']) < rows.index(['role', 'authority'])
        expected_stop_order = list(reversed(launches[:-1]))
        assert [row[1] for row in rows if row[0] == 'stopped'] == expected_stop_order
        assert result['role_stop_requests'] == [dict(role_id=role, status='EXITED')
                                                for role in expected_stop_order]
    else:
        assert result['execution'] in ('FAILED', 'INTERRUPTED') and result['failure']
        if mode in ('wrong-ready', 'service-fail', 'service-degraded', 'audit-fail'):
            assert ['role', 'requester'] not in rows
        if mode == 'audit-fail':
            assert result['status'] == 'UNKNOWN'
        if mode == 'pre-entry-fail':
            assert result['status'] == 'STOPPED' and result['execution'] == 'FAILED'
            assert not (args.deployment.parent / 'actors/controller/receipt.json').exists()
            assert ['role', 'requester'] not in rows
        if mode == 'pre-ready-cancel':
            assert result['role_stop_requests'][0] == dict(role_id='controller', status='EXITED')
            assert ['role', 'requester'] not in rows
        if mode == 'ignore-cancel':
            assert result['failure'] == 'ALLOCATION_ROLE_STOP_GRACE_EXPIRED:p1'
            assert result['role_stop_requests'] == [dict(role_id='p1', status='GRACE_EXPIRED')]
    assert all(item['status'] == 'ZERO' for item in result['cleanup'])
    assert all(module.cluster.owned_state(row['proof']) == 'EXITED' for row in result['launches'])
    assert all(child.poll() is not None for child in children)
    target = args.deployment.parent / 'allocation.json'
    before = target.read_bytes()
    count = len(children)
    with pytest.raises(module.cluster.ClusterError, match='ALLOCATION_BOOT_ALREADY_ENTERED'):
        module.run_allocation(args, config)
    assert len(children) == count and target.read_bytes() == before


def test_allocation_route_owner_reuses_finite_native_management(allocation_facility):
    config, module, args, _, events, children = allocation_facility
    args.routes_only = True
    result = module.run_allocation(args, config)
    assert result['status'] == 'STOPPED' and result['execution'] == 'COMPLETE'
    assert [role['kind'] for role in result['roles']] == ['nfdc']
    assert [json.loads(line)[0] for line in events.read_text().splitlines()] == ['role']
    assert all(child.poll() is not None for child in children)


@pytest.mark.parametrize('mode', ['disjoint', 'alias-sharing', 'declared-sharing', 'gpu-wrong-binding',
                                  'relative-disjoint', 'relative-alias-sharing',
                                  'relative-declared-sharing'])
def test_allocation_uses_observed_physical_gpu_map_before_native_launch(allocation_facility, mode):
    config, module, args, scenario, events, children = allocation_facility
    relative = mode.startswith('relative-')
    mode = mode[len('relative-'):] if relative else mode
    scenario['mode'] = mode
    config['platform']['slurm']['gpu']['count'] = 2
    config['nodes'][0]['gpu_devices'] = ['3', '5'] if mode == 'disjoint' else ['3', 'GPU-3333-abcd']
    first = next(actor for actor in config['actors'] if actor['id'] == 'p0')
    second = next(actor for actor in config['actors'] if actor['id'] == 'p1')
    first['resources']['gpu_devices'] = ['3']
    second['resources']['gpu_devices'] = ['5'] if mode == 'disjoint' else ['GPU-3333-abcd']
    if relative:
        config['nodes'][0]['gpu_devices'] = ['allocated:0', 'allocated:1' if mode == 'disjoint' else '3']
        first['resources']['gpu_devices'] = ['allocated:0']
        second['resources']['gpu_devices'] = ['allocated:1'] if mode == 'disjoint' else ['3']
    first['resources']['allow_sharing'] = second['resources']['allow_sharing'] = mode == 'declared-sharing'
    module.cluster.write_json(args.deployment, config)
    args.configuration_sha256 = hashlib.sha256(
        json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    result = module.run_allocation(args, config)
    rows = [json.loads(line) for line in events.read_text().splitlines()]
    assert rows[0] == ['gpu', config['nodes'][0]['id']]
    if mode in ('alias-sharing', 'gpu-wrong-binding'):
        assert result['execution'] == 'FAILED'
        expected = 'SHARING_NOT_ALLOWED' if mode == 'alias-sharing' else 'OBSERVATION_MISMATCH'
        assert expected in result['failure']
        assert not any(row[0] == 'role' for row in rows)
    else:
        assert result['execution'] == 'COMPLETE' and result['status'] == 'STOPPED'
        for actor_id in ('p0', 'p1'):
            command = next(child.allocation_argv for child in children
                           if '--actor' in child.allocation_argv and
                           child.allocation_argv[child.allocation_argv.index('--actor') + 1] == actor_id)
            assert '--gpus-per-node=2' in command and '--gpu-bind=none' in command


def test_allocation_maximum_actor_and_node_identifiers_remain_cleanup_owned(allocation_facility):
    config, module, args, _, _, children = allocation_facility
    node_id = 'n' * 128
    config['nodes'][0]['id'] = node_id
    for actor in config['actors']:
        actor['node'] = node_id
        if actor['id'] == 'p0':
            actor['id'] = 'p' * 128
    module.cluster.write_json(args.deployment, config)
    args.configuration_sha256 = hashlib.sha256(
        json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    result = module.run_allocation(args, config)
    assert result['status'] == 'STOPPED' and result['execution'] == 'COMPLETE'
    assert all(len(row['id']) <= 128 for row in result['launches'])
    assert all(item['status'] == 'ZERO' for item in result['cleanup'])
    assert all(child.poll() is not None for child in children)


def test_allocation_step_gpu_reservation_does_not_infer_slurm_mask_numbers(node_container):
    config, _, _, _, _ = node_container
    module = importlib.import_module('_spec217_ops.allocation')
    actor = copy.deepcopy(next(actor for actor in config['actors'] if actor['id'] == 'p0'))
    actor['resources']['gpu_devices'] = ['3', 'GPU-5555-abcd']
    observed = {'devices': [{'physical_id': '3', 'uuid': 'GPU-3333-abcd'},
                            {'physical_id': '5', 'uuid': 'GPU-5555-abcd'}]}
    command = module.step_arguments(config, actor, observed)
    assert '--gpus-per-node=2' in command and '--gpu-bind=none' in command
    assert module._gpu_slots(actor, observed) == [0, 1]
    actor['resources']['gpu_devices'] = ['5']
    assert '--gpu-bind=none' in module.step_arguments(config, actor, observed)
    assert module._gpu_slots(actor, observed) == [1]
    actor['resources']['gpu_devices'] = ['3', 'GPU-3333-abcd']
    with pytest.raises(module.cluster.ClusterError, match='GPU_NOT_UNIQUELY_ALLOCATED'):
        module.step_arguments(config, actor, observed)
    actor['resources']['gpu_devices'] = ['7']
    with pytest.raises(module.cluster.ClusterError, match='GPU_NOT_UNIQUELY_ALLOCATED'):
        module.step_arguments(config, actor, observed)
    actor['resources']['gpu_devices'] = []
    assert '--gres=none' in module.step_arguments(config, actor)
    actor['resources']['gpu_devices'] = ['GPU-not-a-scheduler-slot']
    with pytest.raises(module.cluster.ClusterError, match='GPU_SELECTOR_INVALID'):
        module.step_arguments(config, actor)
    actor['resources']['gpu_devices'] = ['0', '00']
    with pytest.raises(module.cluster.ClusterError, match='GPU_SELECTOR_INVALID'):
        module.step_arguments(config, actor, observed)


@pytest.mark.parametrize('selectors, expected', [
    (['allocated:0'], [0]), (['allocated:1'], [1]),
    (['allocated:1', 'allocated:0'], [1, 0]),
    (['allocated:0', '3'], 'GPU_NOT_UNIQUELY_ALLOCATED'),
    (['allocated:1', 'GPU-5555-abcd'], 'GPU_NOT_UNIQUELY_ALLOCATED'),
    (['allocated:2'], 'GPU_NOT_UNIQUELY_ALLOCATED'),
    (['allocated:00'], 'GPU_SELECTOR_INVALID'), (['allocated:-1'], 'GPU_SELECTOR_INVALID'),
    (['allocated:64'], 'GPU_SELECTOR_INVALID'), (['allocated:'], 'GPU_SELECTOR_INVALID'),
])
def test_allocation_relative_gpu_slots_require_observed_unique_devices(node_container, selectors, expected):
    config, _, _, _, _ = node_container
    module = importlib.import_module('_spec217_ops.allocation')
    actor = copy.deepcopy(next(actor for actor in config['actors'] if actor['id'] == 'p0'))
    actor['resources']['gpu_devices'] = selectors
    observed = {'devices': [{'physical_id': '3', 'uuid': 'GPU-3333-abcd'},
                            {'physical_id': '5', 'uuid': 'GPU-5555-abcd'}]}
    if isinstance(expected, list):
        assert '--gpu-bind=none' in module.step_arguments(config, actor, observed)
        assert module._gpu_slots(actor, observed) == expected
        with pytest.raises(module.cluster.ClusterError, match='GPU_OBSERVATION_REQUIRED'):
            module.step_arguments(config, actor)
    else:
        with pytest.raises(module.cluster.ClusterError, match=expected):
            module.step_arguments(config, actor, observed)


@pytest.mark.parametrize('selector, valid', [
    ('allocated:0', True), ('allocated:1', False), ('allocated:00', False),
    ('allocated:-1', False), ('allocated:64', False), ('allocated:', False),
])
def test_relative_gpu_configuration_is_slurm_only_and_reservation_bounded(ops, deployment, selector, valid):
    parser, _ = ops
    root, config = deployment
    config['nodes'][0]['gpu_devices'] = [selector]
    path = write_config(root, config)
    with pytest.raises(parser.DeploymentConfigError, match='allocation-relative'):
        parser.load_deployment(path)
    config = _tiger_config(root, config)
    config['platform']['slurm']['gpu']['count'] = 1
    before = sorted(str(item.relative_to(root)) for item in root.rglob('*'))
    path = write_config(root, config)
    if valid:
        assert parser.load_deployment(path)['nodes'][0]['gpu_devices'] == [selector]
    else:
        with pytest.raises(parser.DeploymentConfigError, match='allocation-relative'):
            parser.load_deployment(path)
    assert sorted(str(item.relative_to(root)) for item in root.rglob('*')) == before




def test_external_requester_is_rejected_before_any_allocation_process(allocation_facility):
    config, module, args, _, events, children = allocation_facility
    requester = next(actor for actor in config['actors'] if actor['kind'] == 'requester')
    requester['external'] = True
    with pytest.raises(module.cluster.ClusterError, match='OWNED_FINITE_REQUESTER_REQUIRED'):
        module.run_allocation(args, config)
    assert not events.exists() and not children


def _gpu_event(config, args, role_id, visible=None, visibility='0,1'):
    return dict(event='NDNSF_DI_GPU_PROBE', schema='ndnsf-di-gpu-probe-v1',
        boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
        release_sha256=args.release_sha256, role_id=role_id, job_id='12345', step_id='3',
        step_gpus='2,6', device_order='PCI_BUS_ID', host=config['nodes'][0]['host'],
        visibility=visibility, visible_uuids=visible or ['GPU-3333-abcd', 'GPU-1111-abcd'],
        inventory=[dict(physical_id='1', uuid='GPU-1111-abcd'), dict(physical_id='3', uuid='GPU-3333-abcd')])


def _write_node_gpu_observation(config, container, frozen):
    args = _role_args(config, frozen, actor='p0')
    event = _gpu_event(config, args, container.management_role_id('gpu-probe', 'local'))
    with pytest.MonkeyPatch.context() as scope:
        scope.setenv('SLURM_JOB_ID', '12345')
        observed = container.read_gpu_probe(json.dumps(event), args, config['nodes'][0],
                                            event['role_id'], 2)
    path = frozen.parent / 'nodes/local/gpu-allocation.json'
    container.cluster.write_json(path, observed)
    return path


def test_gpu_roles_receive_distinct_declared_uuid_subsets_of_full_step(node_container):
    config, container, frozen, capture, run = node_container
    config['platform']['slurm']['gpu']['count'] = 2
    config['nodes'][0]['gpu_devices'] = ['allocated:0', 'allocated:1']
    for actor_id, selector, uuid in [('p0', 'allocated:0', 'GPU-3333-abcd'),
                                     ('p1', 'allocated:1', 'GPU-1111-abcd')]:
        actor = next(item for item in config['actors'] if item['id'] == actor_id)
        actor['resources']['gpu_devices'] = [selector]
        _write_node_gpu_observation(config, container, frozen)
        result = run('--actor', actor_id,
                     environment={'CUDA_VISIBLE_DEVICES': '0,1', 'SLURM_STEP_GPUS': '2,6'})
        assert result.returncode == 0, result.stderr
        values = dict(line.split('=', 1) for line in
                      json.loads(capture.read_text())['envfile'].splitlines())
        assert values['CUDA_VISIBLE_DEVICES'] == uuid
        assert values['SLURM_STEP_GPUS'] == '2,6'


@pytest.mark.parametrize('mutation', ['missing', 'mode', 'job', 'config'])
def test_gpu_wrapper_rejects_unadmitted_observation_before_image_staging(node_container, mutation):
    config, container, frozen, capture, run = node_container
    config['platform']['slurm']['gpu']['count'] = 2
    config['nodes'][0]['gpu_devices'] = ['allocated:0', 'allocated:1']
    next(item for item in config['actors'] if item['id'] == 'p0')['resources']['gpu_devices'] = ['allocated:1']
    path = _write_node_gpu_observation(config, container, frozen)
    if mutation == 'missing':
        path.unlink()
    elif mutation == 'mode':
        path.chmod(0o644)
    else:
        record = json.loads(path.read_text())
        key = 'job_id' if mutation == 'job' else 'configuration_sha256'
        record['native_probe'][key] = 'foreign'
        container.cluster.write_json(path, record)
    result = run('--actor', 'p0',
                 environment={'CUDA_VISIBLE_DEVICES': '0,1', 'SLURM_STEP_GPUS': '2,6'})
    assert result.returncode != 0
    assert not capture.exists()
    assert not (frozen.parent.parent / 'image-cache').exists()


def test_gpu_probe_wrapper_uses_exact_native_tool_without_nfd_or_model_mount(node_container):
    config, _, frozen, capture, run = node_container
    config['platform']['slurm']['gpu']['count'] = 2
    Path(config['nodes'][0]['nfd']['socket']).unlink()
    result = run('--management', 'gpu-probe', '--node', 'local', workload=None,
                 environment={'CUDA_VISIBLE_DEVICES': '0,1', 'SLURM_STEP_GPUS': '2,6'})
    assert result.returncode == 0, result.stderr
    item = json.loads(capture.read_text())
    assert '--nv' in item['argv']
    assert item['argv'][-3:] == [str(Path(config['release']['prefix']) /
        'libexec/ndnsf-di/ndnsf-di-device-query'), '--count', '2']
    binds = [item['argv'][index + 1] for index, value in enumerate(item['argv']) if value == '--bind']
    assert not any(config['nodes'][0]['nfd']['socket'] in bind for bind in binds)
    assert not any(path in bind for path in config['model'].values() for bind in binds)
    assert 'CUDA_DEVICE_ORDER=PCI_BUS_ID\n' in item['envfile']
    assert 'NDNSF_DEPLOYMENT_RELEASE_SHA256=' in item['envfile']
    assert frozen.exists()


@pytest.mark.parametrize('distributed', [False, True])
def test_post_job_oracle_wrapper_uses_fixed_cpu_workload_and_exact_read_only_files(node_container, distributed):
    config, container, frozen, capture, run = node_container
    owner = importlib.import_module('_spec217_ops.allocation')
    config['nodes'][0]['host'] = 'compute-a'
    if distributed:
        config['requester']['turns'][0]['checkpoint_output_file'] = str(frozen.parent / 'checkpoint.json')
        second = copy.deepcopy(config['nodes'][0])
        second.update(id='second', host='compute-b')
        config['nodes'].append(second)
        next(actor for actor in config['actors'] if actor['id'] == 'p1')['node'] = 'second'
        for actor in config['actors']:
            if actor['kind'] == 'provider':
                actor['resources']['gpu_devices'] = ['0']
    Path(config['nodes'][0]['nfd']['socket']).unlink()
    args = SimpleNamespace(deployment=frozen)
    # File payloads are irrelevant to mount/argv checks. The production native
    # oracle's independent C++ fixtures own receipt/result admission assertions.
    for filename in ('allocation.json', 'operation.json'):
        container.cluster.write_json(frozen.parent / filename, {})
    for node in config['nodes']:
        directory = frozen.parent / 'nodes' / node['id']
        container.cluster.write_json(directory / 'host-audit-final.json', {})
        container.cluster.write_json(directory / 'gpu-allocation.json', {})
    for role in owner._roles(config):
        container.cluster.write_json(owner._receipt_path(args, role), {})
        container.cluster.write_json(frozen.parent / 'nodes' / role['node'] /
                                      ('host-audit-' + role['id'] + '.json'), {})
    for actor in config['actors']:
        if actor['kind'] not in ('requester', 'provider'):
            continue
        runtime = frozen.parent / 'actors' / actor['id'] / 'runtime' / config['deployment_id']
        container.cluster.write_json(runtime / (actor['id'] + '-statistics.jsonl'), {})
        if actor['kind'] == 'requester':
            container.cluster.write_json(runtime / (actor['id'] + '.json'), {})
    for turn in config['requester']['turns']:
        for key in ('input_file', 'options_file', 'output_file', 'checkpoint_output_file'):
            if key in turn and not Path(turn[key]).exists():
                container.cluster.write_json(Path(turn[key]), {})
    result = run('--management', 'oracle', '--node', 'local', workload=None,
                 environment={'SLURM_JOB_ID': '', 'SLURM_STEP_ID': ''})
    assert result.returncode == 0, result.stderr
    item = json.loads(capture.read_text())
    expected = [str(Path(config['release']['prefix']) / 'bin/spec190-multiturn-oracle'),
                '--deployment-state', str(frozen.parent / 'allocation.json'), '--require-multi-token']
    if distributed:
        expected.append('--require-distributed-gpu')
    assert item['argv'][-len(expected):] == expected
    assert '--nv' not in item['argv']
    assert 'SLURM_JOB_ID=' not in item['envfile']
    binds = [item['argv'][index + 1] for index, value in enumerate(item['argv']) if value == '--bind']
    writable = [value for value in binds if value.endswith(':rw')]
    assert len(writable) == 2  # Verifier HOME and its own evidence only.
    assert str(frozen.parent) + ':' + str(frozen.parent) + ':ro' not in binds
    assert str(frozen.parent / 'allocation.json') + ':' + str(frozen.parent / 'allocation.json') + ':ro' in binds
    if distributed:
        checkpoint = config['requester']['turns'][0]['checkpoint_output_file']
        assert checkpoint + ':' + checkpoint + ':ro' in binds
    assert not any(path in bind for path in config['model'].values() for bind in binds)
    assert not any(actor['credential_home'] in bind for actor in config['actors'] for bind in binds)
    assert not any(config['nodes'][0]['nfd']['socket'] in bind for bind in binds)
    blocked = run('--management', 'oracle', '--node', 'local', workload=('/bin/true',),
                  environment={'SLURM_JOB_ID': ''})
    assert blocked.returncode != 0 and 'CONTAINER_ORACLE_WORKLOAD_FIXED' in blocked.stderr


def test_gpu_native_mapping_uses_uuid_instead_of_slurm_or_nvml_number_equality(node_container, monkeypatch):
    config, container, frozen, _, _ = node_container
    config['platform']['slurm']['gpu']['count'] = 2
    args = _role_args(config, frozen)
    monkeypatch.setenv('SLURM_JOB_ID', '12345')
    role = container.management_role_id('gpu-probe', config['nodes'][0]['id'])
    record = _gpu_event(config, args, role)
    result = container.read_gpu_probe('platform warning\n' + json.dumps(record), args,
                                       config['nodes'][0], role, 2)
    assert record['step_gpus'] == '2,6'
    assert [item['physical_id'] for item in result['devices']] == ['3', '1']
    actor = {'id': 'provider', 'resources': {'gpu_devices': ['3', 'GPU-1111-abcd']}}
    assert container.gpu_slots(actor, result) == [0, 1]
    path = frozen.parent / 'nodes' / config['nodes'][0]['id'] / 'gpu-allocation.json'
    container.cluster.write_json(path, result)
    assert container.load_gpu_allocation(path, args, config, config['nodes'][0]) == result
    result['devices'].reverse()
    container.cluster.write_json(path, result)
    with pytest.raises(container.cluster.ClusterError, match='GPU_ALLOCATION_MISMATCH'):
        container.load_gpu_allocation(path, args, config, config['nodes'][0])


@pytest.mark.parametrize('mutation', ['boot_id', 'configuration_sha256', 'release_sha256', 'job_id',
    'role_id', 'device_order', 'visibility', 'host', 'step_id', 'duplicate-event',
    'visible-count', 'duplicate-visible', 'duplicate-inventory', 'missing-uuid'])
def test_gpu_native_observation_rejects_wrong_binding_or_ambiguous_identity(node_container, monkeypatch, mutation):
    config, container, frozen, _, _ = node_container
    args = _role_args(config, frozen)
    monkeypatch.setenv('SLURM_JOB_ID', '12345')
    role = container.management_role_id('gpu-probe', config['nodes'][0]['id'])
    record = _gpu_event(config, args, role)
    if mutation == 'visible-count':
        record['visible_uuids'].pop()
    elif mutation == 'duplicate-visible':
        record['visible_uuids'][1] = record['visible_uuids'][0]
    elif mutation == 'duplicate-inventory':
        record['inventory'].append(dict(record['inventory'][0]))
    elif mutation == 'missing-uuid':
        record['inventory'].pop()
    elif mutation != 'duplicate-event':
        record[mutation] = 'wrong-binding'
    output = json.dumps(record)
    if mutation == 'duplicate-event':
        output += '\n' + output
    with pytest.raises(container.cluster.ClusterError, match='GPU_'):
        container.read_gpu_probe(output, args, config['nodes'][0], role, 2)


@pytest.mark.parametrize('mode', ['single', 'wrong-uuid', 'multi', 'reversed'])
def test_native_role_checks_actual_step_uuid_before_service_launch(node_container, monkeypatch, mode):
    config, container, frozen, _, run = node_container
    node = importlib.import_module('_spec217_ops.node')
    actor = next(actor for actor in config['actors'] if actor['id'] == 'p0')
    multi = mode in ('multi', 'reversed')
    actor['resources']['gpu_devices'] = ['3', '1'] if multi else ['3']
    config['nodes'][0]['gpu_devices'] = ['3', '1']
    config['platform']['slurm']['gpu']['count'] = 2
    args = _role_args(config, frozen, actor='p0', timeout=2)
    _role_environment(monkeypatch, args, 'p0')
    monkeypatch.setenv('SLURM_STEP_GPUS', '2,6')
    visibility = '0,1' if multi else '0'
    monkeypatch.setenv('CUDA_VISIBLE_DEVICES', visibility)
    monkeypatch.setenv('LD_LIBRARY_PATH', '/image/runtime/libraries')
    record = _gpu_event(config, args, container.management_role_id('gpu-probe', 'local'))
    pool = container.read_gpu_probe(json.dumps(record), args, config['nodes'][0], record['role_id'], 2)
    container.cluster.write_json(frozen.parent / 'nodes/local/gpu-allocation.json', pool)
    assert run('--actor', 'p0', environment={'CUDA_VISIBLE_DEVICES': '0,1', 'SLURM_STEP_GPUS': '2,6'}).returncode == 0
    real_spawn = container.cluster._spawn
    real_launch = container.cluster._native_launch
    children = []

    def spawn(state, root, tag, kind, command, env, guard):
        if command[0].endswith('/ndnsf-di-device-query'):
            assert env['CUDA_DEVICE_ORDER'] == 'PCI_BUS_ID'
            assert env['LD_LIBRARY_PATH'] == '/image/runtime/libraries'
            assert env['NDNSF_DEPLOYMENT_RELEASE_SHA256'] == args.release_sha256
            uuids = ['GPU-3333-abcd', 'GPU-1111-abcd'] if multi else ['GPU-3333-abcd']
            if mode == 'wrong-uuid':
                uuids = ['GPU-1111-abcd']
            elif mode == 'reversed':
                uuids.reverse()
            event = _gpu_event(config, args, env['NDNSF_DEPLOYMENT_ACTOR_ID'], uuids, visibility)
            command = [sys.executable, '-c', 'import time; print(' + repr(json.dumps(event)) +
                       ',flush=True); time.sleep(0.03)']
        elif command[0].endswith('/ndnsf-identity-check'):
            command = [sys.executable, '-c', 'import time; time.sleep(0.03)']
        child, row = real_spawn(state, root, tag, kind, command, env, guard)
        children.append(child)
        return child, row

    def launch(current, owned_actor, root, state, **kwargs):
        _, env = real_launch(current, owned_actor, root, state, **kwargs)
        ready = dict(event='NDNSF_DEPLOYMENT_READY', identity=owned_actor['identity'],
            actor_id=owned_actor['id'], boot_id=args.boot_id, config_sha256=args.configuration_sha256,
            native_boot_id='controlled-native-role')
        code = ('import json,os,signal,time; item=' + repr(ready) +
                '; item["pid"]=os.getpid(); print(json.dumps(item),flush=True); '
                'time.sleep(0.1); os.kill(os.getppid(),signal.SIGTERM); time.sleep(60)')
        return [sys.executable, '-c', code], env

    monkeypatch.setattr(container.cluster, '_spawn', spawn)
    monkeypatch.setattr(container.cluster, '_native_launch', launch)
    result = node.run_role(args)
    assert result['status'] == 'STOPPED'
    if mode in ('wrong-uuid', 'reversed'):
        assert result['execution'] == 'FAILED' and 'STEP_GPU_UUID_MISMATCH' in result['failure']
        assert all(row['kind'] != 'provider' for row in result['launches'])
    else:
        assert result['execution'] == 'SERVICE_STOPPED' and result['failure'] is None
        assert [item['uuid'] for item in result['gpu_admission']['devices']] == (
            ['GPU-3333-abcd', 'GPU-1111-abcd'] if multi else ['GPU-3333-abcd'])
    assert all(child.poll() is not None for child in children)


@pytest.mark.parametrize('mode', ['observed', 'foreign-protected', 'unbound-survivor',
                                 'known-moved', 'wrong-namespace', 'wrong-binding',
                                 'wrong-cgroup', 'unreadable'])
def test_host_audit_correlates_real_pid_namespace_and_refuses_unknown(node_container, monkeypatch, mode):
    config, container, frozen, _, run = node_container
    assert run('--actor', 'p0').returncode == 0
    module = importlib.import_module('_spec217_ops.allocation')
    args = _role_args(config, frozen, actor='p0', timeout=5)
    args.audit_node = config['nodes'][0]['id']
    args.observe_role = 'p0'
    monkeypatch.setenv('SLURM_JOB_ID', '12345')
    root = frozen.parent / 'audit-fixture' / config['deployment_id']
    container.cluster._private_directory(root)
    state = dict(schema='ndnsf-di-host-operation-v1', deployment_id=config['deployment_id'],
                 boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
                 evidence_root=str(root), status='STARTING', launches=[])
    env = dict(os.environ, NDNSF_DEPLOYMENT_ACTOR_ID='p0', NDNSF_DEPLOYMENT_BOOT_ID=args.boot_id,
               NDNSF_DEPLOYMENT_CONFIG_SHA256=args.configuration_sha256)
    child, row = container.cluster._spawn(state, root, 'p0', 'provider',
        [sys.executable, '-c', 'import time; time.sleep(60)'], env, lambda: None)
    actor = next(actor for actor in config['actors'] if actor['id'] == 'p0')
    receipt = dict(schema='ndnsf-di-node-role-v1', boot_id=args.boot_id,
        configuration_sha256=args.configuration_sha256, release_sha256=args.release_sha256,
        role_id='p0', role_kind='provider', node_id=actor['node'], job_id='12345', step_id='3',
        cleanup_scope='PID_NAMESPACE', launches=[row], pid_namespace=os.readlink('/proc/self/ns/pid'))
    if mode == 'wrong-namespace':
        receipt['pid_namespace'] = 'pid:[0]'
    if mode == 'wrong-binding':
        receipt['configuration_sha256'] = '0' * 64
    target = frozen.parent / 'actors/p0/receipt.json'
    container.cluster.write_json(target, receipt)
    original_read = Path.read_text
    after_observation = False

    def read_text(path, *argv, **kwargs):
        if str(path) == '/proc/self/cgroup':
            return '0::/slurm/job_12345/step_99\n'
        if str(path) == '/proc/' + str(child.pid) + '/cgroup':
            # This host is not Slurm. Only cgroup text is controlled; PID,
            # starttime, namespace, NSpid and environment are actual kernel data.
            wrong = mode == 'wrong-cgroup' or (mode == 'known-moved' and after_observation)
            return '0::/slurm/job_' + ('99999' if wrong else '12345') + '/step_3\n'
        return original_read(path, *argv, **kwargs)

    monkeypatch.setattr(Path, 'read_text', read_text)
    original_identity = module.process_identity

    def identity(pid):
        if mode == 'foreign-protected' and pid == os.getpid():
            raise PermissionError('controlled protected process outside Slurm job')
        if mode == 'unreadable' and pid == child.pid:
            raise PermissionError('controlled unreadable live process')
        proof = original_identity(pid)
        if after_observation and pid == child.pid and mode in ('unbound-survivor', 'known-moved'):
            proof['bindings'] = {}
        return proof

    monkeypatch.setattr(module, 'process_identity', identity)
    try:
        result = module.audit_node(args, config)
        if mode in ('observed', 'foreign-protected', 'unbound-survivor', 'known-moved'):
            assert result['status'] == 'OBSERVED'
            proof = result['native_process']
            assert proof['pid'] == child.pid and proof['nspid'][-1] == row['proof']['pid']
            assert proof['start_ticks'] == row['proof']['start_ticks']
            args.observe_role = None
            after_observation = True
            if mode == 'unbound-survivor':
                # Remove prior PID proof to exercise independent job-cgroup admission.
                (frozen.parent / 'nodes' / args.audit_node / 'host-audit-p0.json').unlink()
            final = module.audit_node(args, config)
            assert final['status'] == ('UNKNOWN' if mode == 'known-moved' else 'SURVIVORS')
        else:
            assert result['status'] == 'UNKNOWN' and result['failure']
    finally:
        container.cluster._close(state, root)
        child.poll()
    assert state['status'] == 'STOPPED' and child.poll() is not None
    if mode in ('observed', 'foreign-protected', 'unbound-survivor', 'known-moved'):
        args.observe_role = None
        result = module.audit_node(args, config)
        assert result['status'] == 'ZERO' and result['processes'] == []


@pytest.mark.parametrize('unbound,step,auditor_tag', [
    (False, '3', False), (True, '3', False), (True, 'batch', False),
    (True, 'extern', False), (True, None, False), (False, '3', True)])
def test_recovery_audit_distinguishes_original_job_from_auditor(
        node_container, monkeypatch, unbound, step, auditor_tag):
    """Real PID/start/namespace; control cgroup text and optionally remove binding tags."""
    config, container, frozen, _, _ = node_container
    module = importlib.import_module('_spec217_ops.allocation')
    args = _role_args(config, frozen, timeout=5)
    args.audit_node = config['nodes'][0]['id']
    args.observe_role = None
    args.audit_target_job = '12345'
    args.recovery_id = 'c' * 32
    monkeypatch.setenv('SLURM_JOB_ID', '23456')
    root = frozen.parent / 'recovery-fixture' / config['deployment_id']
    container.cluster._private_directory(root)
    state = dict(schema='ndnsf-di-host-operation-v1', deployment_id=config['deployment_id'],
                 boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
                 evidence_root=str(root), status='STARTING', launches=[])
    tag = module._audit_id(args.audit_node) if auditor_tag else 'p0'
    env = dict(os.environ, NDNSF_DEPLOYMENT_ACTOR_ID=tag,
               NDNSF_DEPLOYMENT_BOOT_ID=args.boot_id,
               NDNSF_DEPLOYMENT_CONFIG_SHA256=args.configuration_sha256)
    child, _ = container.cluster._spawn(state, root, tag, 'provider',
        [sys.executable, '-c', 'import time; time.sleep(60)'], env, lambda: None)
    original_read = Path.read_text

    def read_text(path, *argv, **kwargs):
        if str(path) == '/proc/self/cgroup':
            return '0::/slurm/job_23456/step_3\n'
        if str(path) == '/proc/' + str(child.pid) + '/cgroup':
            # Equal step numbers across different jobs must not hide a survivor.
            return '0::/slurm/job_12345' + ('/step_' + step if step else '') + '\n'
        return original_read(path, *argv, **kwargs)

    monkeypatch.setattr(Path, 'read_text', read_text)
    original_identity = module.process_identity

    def identity(pid):
        proof = original_identity(pid)
        if unbound and pid == child.pid and proof is not None:
            proof['bindings'] = {}
        return proof

    monkeypatch.setattr(module, 'process_identity', identity)
    try:
        result = module.audit_node(args, config)
        assert result['status'] == 'SURVIVORS'
        assert any(row['pid'] == child.pid for row in result['processes'])
        assert result['job_id'] == '12345' and result['auditor_job_id'] == '23456'
        assert result['recovery_id'] == args.recovery_id
    finally:
        container.cluster._close(state, root)
        child.poll()
    result = module.audit_node(args, config)
    assert result['status'] == 'ZERO' and result['processes'] == []
    assert result['errors'] == [] and result['job_id'] == '12345'
    assert (frozen.parent / 'nodes' / args.audit_node / 'host-audit-recovery.json').is_file()
    assert not (frozen.parent / 'nodes' / args.audit_node / 'host-audit-final.json').exists()


def test_installed_allocation_and_route_shell_dispatch_use_explicit_interpreter(node_container):
    _, container, _, _, _ = node_container
    installed_root = Path(container.__file__).parents[2]
    python = str(installed_root.parents[2] / 'bin/python3')
    for name in ('run-allocation-topology.sh', 'configure-allocation-routes.sh'):
        script = container.SCRIPTS / name
        command = ['/bin/bash', str(script), '--deployment', '/bound/input.json',
                   '--control-python', python, '--help']
        result = subprocess.run(command, env=dict(os.environ, PATH='/usr/bin:/bin',
                                PYTHONPATH='/poisoned/checkout'), capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stderr
        assert '--configuration-sha256' in result.stdout and '--audit-node' in result.stdout
        missing = subprocess.run(['/bin/bash', str(script), '--deployment', '/bound/input.json'],
                                 capture_output=True, text=True, timeout=10)
        assert missing.returncode == 2 and 'CONTROL_PYTHON_REQUIRED' in missing.stderr


@pytest.mark.parametrize('readback', ['valid', 'wrong-face', 'wrong-prefix'])
def test_inner_management_checks_native_route_readback(node_container, monkeypatch, readback):
    config, container, frozen, _, run = node_container
    node_module = importlib.import_module('_spec217_ops.node')
    remote = copy.deepcopy(config['nodes'][0])
    remote.update(id='remote', host='other-node')
    remote['nfd']['address'] = '192.0.2.2'
    config['nodes'].append(remote)
    config['network']['routes'] = [{'from': 'local', 'to': 'remote', 'prefix': '/test/group'}]
    assert run('--management', 'nfdc', '--node', 'local').returncode == 0
    args = _role_args(config, frozen, actor=None, management='nfdc')
    _role_environment(monkeypatch, args, 'nfdc.local')
    commands = []
    monkeypatch.setattr(node_module.shutil, 'which', lambda name: '/usr/bin/nfdc')

    def finite(state, root, argv, env, guard):
        commands.append(argv[1:])
        if argv[1:] == ['strategy', 'list']:
            return 'prefix=/test/group strategy=/localhost/nfd/strategy/multicast\n'
        if argv[1:3] == ['face', 'list']:
            uri = 'udp4://192.0.2.2:6363' if readback != 'wrong-face' else 'udp4://192.0.2.3:6363'
            return 'faceid=257 remote=' + uri + ' local=udp4://192.0.2.1:6363\n'
        if argv[1:3] == ['route', 'show']:
            prefix = '/8=test/8=group' if readback != 'wrong-prefix' else '/test/other'
            return 'prefix=' + prefix + ' nexthop=257 origin=static cost=10\n'
        return ''

    monkeypatch.setattr(node_module, '_finite', finite)
    result = node_module.run_role(args)
    if readback == 'valid':
        assert result['execution'] == 'COMPLETE' and result['readiness'] == 'MANAGEMENT_VERIFIED'
        assert ['route', 'add', 'prefix', '/test/group', 'nexthop', 'udp4://192.0.2.2:6363', 'cost', '10'] in commands
        assert ['route', 'show', 'prefix', '/test/group'] in commands
    else:
        assert result['execution'] == 'FAILED' and 'READBACK_MISSING' in result['failure']
    assert result['status'] == 'STOPPED' and result['host_audit'] == 'UNKNOWN'


def test_tiger_submit_check_does_not_read_remote_credentials(ops, deployment):
    parser, _ = ops
    root, config = deployment
    config = _tiger_config(root, config)
    path = write_config(root, config)
    assert parser.check_deployment(path)["platform"] == "slurm-apptainer"
    assert not Path("/remote/requester-private.pem").exists()
    config["platform"]["slurm"]["unknown"] = True
    with pytest.raises(parser.DeploymentConfigError, match="unknown fields"):
        parser.load_deployment(write_config(root, config))


@pytest.mark.parametrize('change', ['complete', 'missing-audit', 'foreign-audit', 'wrong-job',
                                  'forced', 'missing-role', 'failed-role', 'wrong-proof',
                                  'survivor', 'external', 'failed-request', 'partial-start',
                                  'identity-fail', 'not-spawned', 'pre-entry-fail',
                                  'false-exit', 'float-exit', 'float-pid',
                                  'early-native-exit', 'early-wrong-reason', 'early-foreign-boot',
                                  'early-foreign-config', 'early-foreign-release', 'early-foreign-job',
                                  'early-foreign-node', 'early-foreign-role', 'early-foreign-host',
                                  'early-foreign-cgroup',
                                  'early-audit-error', 'early-bad-scope', 'early-success-role',
                                  'early-zero-exit', 'early-bool-exit', 'early-float-exit',
                                  'early-none-exit', 'early-foreign-owner', 'early-open-role',
                                  'early-complete', 'early-forced', 'early-survivor',
                                  'early-missing-final'])
def test_slurm_snapshot_consumes_bound_closeout_without_runtime_inputs(node_container, monkeypatch, change):
    """Receipt admission only: no assertion about synthetic native inference results."""
    config, container, frozen, _, run = node_container
    if change == 'external':
        next(actor for actor in config['actors'] if actor['kind'] == 'controller')['external'] = True
    assert run('--actor', 'p0').returncode == 0
    owner = importlib.import_module('_spec217_ops.allocation')
    backend = importlib.import_module('_spec217_ops.slurm')
    args = _role_args(config, frozen)
    root = frozen.parent
    boot, digest = args.boot_id, args.configuration_sha256
    reference = dict(boot_id=boot, configuration_sha256=digest,
                     release_sha256=args.release_sha256, job_id='12345')
    state = dict(schema=backend.SCHEMA, deployment_id=config['deployment_id'], boot_id=boot,
                 configuration_sha256=digest, evidence_root=str(root), status='AWAITING_NODE_AUDIT',
                 job_reference=reference, failure=None, needs_closeout=True, launches=[])
    scheduler = dict(reference=reference, terminal=True, status='TERMINAL',
                     state='COMPLETED', exit_code='0:0')
    class Adapter:
        def deployment_status(self, ref, **kwargs):
            assert ref == reference
            return copy.deepcopy(scheduler)

        cancel_deployment = deployment_status

    monkeypatch.setattr(backend, 'SlurmApptainerAdapter', Adapter)
    monkeypatch.delenv('SLURM_JOB_ID', raising=False)
    roles = owner._roles(config)
    binding = dict(boot_id=boot, configuration_sha256=digest,
                   release_sha256=args.release_sha256, job_id='12345')

    def groups(tag):
        env = dict(NDNSF_DEPLOYMENT_ACTOR_ID=tag, NDNSF_DEPLOYMENT_BOOT_ID=boot,
                   NDNSF_DEPLOYMENT_CONFIG_SHA256=digest)
        proof = dict(pid=321, pgid=321, uid=os.getuid(), start_ticks=123, exe='/controlled/native',
                     bindings=env)
        return dict(launches=[dict(id=tag, bindings=env, proof=proof)],
                    cleanup=[dict(id=tag, status='ZERO', forced=False)])

    allocation = dict(binding, **groups('controlled-srun'), schema='ndnsf-di-host-operation-v1',
        deployment_id=config['deployment_id'], evidence_root=str(root / 'supervisor' / config['deployment_id']),
        status='STOPPED', execution='COMPLETE', readiness='SERVICES_READY', failure=None,
        roles=roles, external_references=[])
    partial = change in ('partial-start', 'identity-fail', 'not-spawned', 'pre-entry-fail')
    if partial:
        roles = [role for role in roles if role['kind'] in ('nfd', 'nfdc', 'controller')]
        allocation.update(roles=roles, execution='FAILED', failure='controlled startup failure')
    if change in ('not-spawned', 'pre-entry-fail'):
        tag = container.management_role_id('step', 'controller')
        attempt = groups(tag)
        if change == 'not-spawned':
            attempt['launches'][0].pop('proof')
            attempt['launches'][0]['spawn_failed'] = True
        allocation['launches'] += attempt['launches']
        allocation['cleanup'] += attempt['cleanup']
    for role in roles:
        receipt = dict(binding, **groups(role['id']), schema='ndnsf-di-node-role-v1',
            role_id=role['id'], role_kind=role['kind'], node_id=role['node'], step_id='3',
            cleanup_scope='PID_NAMESPACE', status='STOPPED', failure=None, exit_code=0,
            execution='COMPLETE' if role['kind'] in ('requester', 'nfdc') else 'SERVICE_STOPPED',
            pid_namespace='pid:[controlled]')
        if role['kind'] == 'controller' and partial:
            receipt.update(execution='FAILED', failure='controlled startup failure')
            if change == 'identity-fail':
                receipt.update(groups('management-0'))
        if role['id'] == 'p0':
            if change == 'forced':
                receipt['cleanup'][0]['forced'] = True
            if change == 'failed-role':
                receipt.update(execution='FAILED', failure='controlled native exit')
        if role['kind'] == 'requester' and change == 'failed-request':
            receipt.update(execution='FAILED', failure='native requester exit7', exit_code=7)
            allocation.update(execution='FAILED', failure='native requester exit7')
        if role['kind'] == 'requester' and change.startswith('early-'):
            if change != 'early-complete':
                allocation.update(execution='FAILED', failure='native requester exited before observation')
            if change not in ('early-success-role', 'early-complete'):
                receipt.update(execution='FAILED', failure='native requester exit1', exit_code=1)
            if change in ('early-zero-exit', 'early-bool-exit', 'early-float-exit', 'early-none-exit'):
                receipt['exit_code'] = {'early-zero-exit': 0, 'early-bool-exit': True,
                                        'early-float-exit': 1.0, 'early-none-exit': None}[change]
            if change == 'early-foreign-owner':
                receipt['launches'][0]['proof']['uid'] = os.getuid() + 1
            if change == 'early-open-role':
                receipt['status'] = 'RUNNING'
            if change == 'early-forced':
                receipt['cleanup'][0]['forced'] = True
        if role['kind'] == 'requester' and change in ('false-exit', 'float-exit'):
            receipt['exit_code'] = False if change == 'false-exit' else 0.0
        if role['id'] == 'p0' and change == 'float-pid':
            receipt['launches'][0]['proof']['pid'] = 321.0
        if not ((change == 'missing-role' and role['id'] == 'p0') or
                (change in ('not-spawned', 'pre-entry-fail') and role['kind'] == 'controller')):
            container.cluster.write_json(owner._receipt_path(args, role), receipt)
        proof = dict(receipt['launches'][0]['proof'], nspid=[321],
                     pid_namespace=receipt['pid_namespace'], cgroup='0::/slurm/job_12345/step_3')
        observed = dict(binding, schema='ndnsf-di-host-node-audit-v1', node_id=role['node'],
                        role_id=role['id'], status='OBSERVED', native_process=proof)
        if role['kind'] == 'requester' and change.startswith('early-'):
            observed = dict(binding, schema='ndnsf-di-host-node-audit-v1', node_id=role['node'],
                            host=config['nodes'][0]['host'], status='UNKNOWN', errors=[],
                            cleanup_scope='HOST_UID_JOB',
                            auditor_cgroup='0::/slurm/job_12345/step_9',
                            failure='ALLOCATION_NATIVE_HOST_PID_NOT_OBSERVED:' + role['id'])
            fields = {'early-foreign-boot': 'boot_id', 'early-foreign-config': 'configuration_sha256',
                      'early-foreign-release': 'release_sha256', 'early-foreign-job': 'job_id',
                      'early-foreign-node': 'node_id', 'early-foreign-role': 'role_id',
                      'early-foreign-host': 'host'}
            if change in fields:
                observed[fields[change]] = 'foreign'
            if change == 'early-foreign-cgroup':
                observed['auditor_cgroup'] = '0::/slurm/job_999/step_9'
            if change == 'early-wrong-reason':
                observed['failure'] = 'ALLOCATION_NATIVE_HOST_PID_NOT_OBSERVED:another-role'
            if change == 'early-audit-error':
                observed['errors'] = ['unreadable process']
            if change == 'early-bad-scope':
                observed['cleanup_scope'] = 'PID_NAMESPACE'
        if role['id'] == 'p0' and change == 'wrong-proof':
            observed['native_process']['start_ticks'] += 1
        if not (partial and role['kind'] == 'controller'):
            container.cluster.write_json(root / 'nodes' / role['node'] /
                                          ('host-audit-' + role['id'] + '.json'), observed)
    for node in config['nodes']:
        audit = dict(binding, schema='ndnsf-di-host-node-audit-v1', node_id=node['id'],
                     host=node['host'], status='ZERO', cleanup_scope='HOST_UID_JOB', processes=[], errors=[])
        if change == 'foreign-audit':
            audit['boot_id'] = 'foreign'
        if change in ('survivor', 'early-survivor'):
            audit['processes'] = [{'pid': 999}]
        if change not in ('missing-audit', 'early-missing-final'):
            container.cluster.write_json(root / 'nodes' / node['id'] / 'host-audit-final.json', audit)
    if change == 'wrong-job':
        allocation['job_id'] = '999'
    if change == 'external':
        allocation['external_references'] = [dict(id=actor['id'], kind=actor['kind'], verification='NOT_PROBED')
                                             for actor in config['actors'] if actor['external']]
    container.cluster.write_json(root / 'allocation.json', allocation)
    # Status/down must not re-admit missing model, release or image runtime inputs.
    Path(config['platform']['sif']).unlink()
    Path(config['release']['manifest']).unlink()
    result = backend.snapshot(state, timeout=2)
    assert result['oracle'] == 'NOT_RUN'
    if change in ('complete', 'external', 'failed-request', 'early-native-exit') or partial:
        expected = ('FAILED' if partial else
                    {'complete': 'COMPLETE', 'external': 'EXTERNAL_UNVERIFIED',
                     'failed-request': 'FAILED', 'early-native-exit': 'FAILED'}[change])
        assert result['status'] == expected and result['actor_cleanup'] == 'ZERO'
        assert result['requester']['exit_code'] == (
            None if partial else 7 if change == 'failed-request' else 1 if change == 'early-native-exit' else 0)
        if partial:
            assert result['requester']['completion'] == 'NOT_STARTED'
    else:
        assert result['status'] == 'UNKNOWN' and result['actor_cleanup'] == 'UNVERIFIED'
        assert result.get('allocation_error')
    operation = root / 'operation' / config['deployment_id']
    container.cluster._private_directory(operation)
    container.cluster.write_json(operation / 'state.json', state)
    closed = backend.run_slurm('down', frozen, operation, timeout=2)
    persisted = json.loads((operation / 'state.json').read_text())
    assert (persisted['status'] == 'STOPPED') == (result['actor_cleanup'] == 'ZERO')
    assert persisted['needs_closeout'] == (result['actor_cleanup'] != 'ZERO')
    assert closed['status'] == result['status']


@pytest.mark.parametrize('change', ['none', 'boot', 'config', 'release', 'job', 'node', 'host',
                                  'auditor-job', 'auditor-cgroup', 'recovery-id', 'survivor',
                                  'unreadable', 'missing-owner', 'forced-owner', 'live',
                                  'existing-allocation', 'allocation-symlink', 'auditor-step',
                                  'missing-step', 'failed-owner', 'missing-exit', 'bool-exit',
                                  'null-cgroup', 'numeric-cgroup'])
def test_missing_allocation_requires_independent_bound_recovery(node_container, monkeypatch, change):
    """Admission fixtures only; runtime recovery is checked separately on Tiger."""
    config, container, frozen, _, _ = node_container
    container.cluster.write_json(frozen, config)
    backend = importlib.import_module('_spec217_ops.slurm')
    owner = importlib.import_module('_spec217_ops.allocation')
    args = _role_args(config, frozen)
    evidence, nonce = frozen.parent, 'c' * 32
    reference = dict(boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
                     release_sha256=args.release_sha256, job_id='12345')
    state = dict(schema=backend.SCHEMA, deployment_id=config['deployment_id'],
                 boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
                 evidence_root=str(evidence), status='AWAITING_NODE_AUDIT',
                 job_reference=reference, failure='EDQUOT', needs_closeout=True,
                 launches=[], recovery_id=nonce)
    scheduler = dict(reference=reference, terminal=True, status='TERMINAL',
                     state='FAILED', exit_code='1:0', live_verified=False)
    if change == 'live':
        scheduler.update(terminal=False, status='RUNNING', state='RUNNING', live_verified=True)

    class Adapter:
        def deployment_status(self, ref, **kwargs):
            assert ref == reference
            return copy.deepcopy(scheduler)

        cancel_deployment = deployment_status

    monkeypatch.setattr(backend, 'SlurmApptainerAdapter', Adapter)
    node = config['nodes'][0]
    tag = owner._audit_id(node['id'])
    bindings = dict(NDNSF_DEPLOYMENT_ACTOR_ID=tag,
                    NDNSF_DEPLOYMENT_BOOT_ID=args.boot_id,
                    NDNSF_DEPLOYMENT_CONFIG_SHA256=args.configuration_sha256)
    audit_root = evidence / 'recovery' / nonce / node['id'] / config['deployment_id']
    container.cluster._private_directory(audit_root)
    audit_state = dict(schema='ndnsf-di-host-operation-v1', deployment_id=config['deployment_id'],
        boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
        evidence_root=str(audit_root), recovery_id=nonce, status='STOPPED', audit_exit_code=0,
        launches=[dict(id=tag, bindings=bindings, proof=dict(pid=321, pgid=321,
            uid=os.getuid(), start_ticks=123, bindings=bindings))],
        cleanup=[dict(id=tag, status='ZERO', forced=change == 'forced-owner')])
    if change != 'missing-owner':
        if change == 'failed-owner':
            audit_state['audit_exit_code'] = 9
        elif change == 'missing-exit':
            audit_state.pop('audit_exit_code')
        elif change == 'bool-exit':
            audit_state['audit_exit_code'] = False
        container.cluster.write_json(audit_root / 'state.json', audit_state)
    audit = dict(schema='ndnsf-di-host-node-audit-v1', node_id=node['id'], host=node['host'],
        boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
        release_sha256=args.release_sha256, job_id='12345', auditor_job_id='23456',
        recovery_id=nonce, auditor_cgroup='0::/slurm/job_23456/step_0\n', auditor_step_id='0',
        status='ZERO', cleanup_scope='HOST_UID_JOB', processes=[], errors=[])
    fields = {'boot': 'boot_id', 'config': 'configuration_sha256', 'release': 'release_sha256',
              'job': 'job_id', 'node': 'node_id', 'host': 'host', 'auditor-job': 'auditor_job_id',
              'auditor-cgroup': 'auditor_cgroup', 'recovery-id': 'recovery_id',
              'auditor-step': 'auditor_step_id'}
    if change in fields:
        audit[fields[change]] = 'foreign'
    if change == 'survivor':
        audit.update(status='SURVIVORS', processes=[dict(pid=999)])
    if change == 'unreadable':
        audit.update(status='UNKNOWN', errors=['UNREADABLE:999'])
    if change == 'missing-step':
        audit.pop('auditor_step_id')
    if change in ('null-cgroup', 'numeric-cgroup'):
        audit['auditor_cgroup'] = None if change == 'null-cgroup' else 7
    target = evidence / 'nodes' / node['id'] / 'host-audit-recovery.json'
    container.cluster.write_json(target, audit)
    if change == 'existing-allocation':
        container.cluster.write_json(evidence / 'allocation.json', {})
    if change == 'allocation-symlink':
        (evidence / 'allocation.json').symlink_to(evidence / 'absent')
    result = backend.snapshot(state, timeout=2)
    if change == 'none':
        assert result['actor_cleanup'] == 'ZERO' and result['status'] == 'FAILED', result
        assert result['execution'] == 'FAILED' and result['failure'] == 'EDQUOT'
        assert result['oracle'] == 'NOT_RUN' and result['readiness'] == 'UNVERIFIED'
    else:
        assert result['actor_cleanup'] == 'UNVERIFIED' and result['status'] == 'UNKNOWN'
    assert not (evidence / 'allocation.json').exists() or change == 'existing-allocation'


@pytest.mark.parametrize('outcome', ['zero', 'exit', 'stale', 'late-exit'])
def test_down_runs_owned_audit_for_missing_allocation(node_container, monkeypatch, outcome):
    """Real bounded child ownership, with fixture audit replies rather than Slurm."""
    config, container, frozen, _, _ = node_container
    container.cluster.write_json(frozen, config)
    backend = importlib.import_module('_spec217_ops.slurm')
    args = _role_args(config, frozen)
    reference = dict(boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
                     release_sha256=args.release_sha256, job_id='12345')
    state = dict(schema=backend.SCHEMA, deployment_id=config['deployment_id'],
        boot_id=args.boot_id, configuration_sha256=args.configuration_sha256,
        evidence_root=str(frozen.parent), status='AWAITING_NODE_AUDIT',
        job_reference=reference, failure='EDQUOT', needs_closeout=True, launches=[])
    scheduler = dict(reference=reference, terminal=True, status='TERMINAL',
                     state='FAILED', exit_code='1:0', live_verified=False)

    class Adapter:
        def deployment_status(self, ref, **kwargs):
            assert ref == reference
            return copy.deepcopy(scheduler)

        cancel_deployment = deployment_status

    monkeypatch.setattr(backend, 'SlurmApptainerAdapter', Adapter)
    monkeypatch.setattr(backend.shutil, 'which', lambda name: sys.executable)
    real_spawn = container.cluster._spawn
    commands, children = [], []
    reply = '''import json,sys,time
from pathlib import Path
argv=sys.argv[1:]
get=lambda key:argv[argv.index(key)+1]
time.sleep(0.15)
if OUTCOME == 'exit': raise SystemExit(9)
root=Path(get('--deployment')).parent
config=json.loads((root/'configuration.json').read_text())
node=next(n for n in config['nodes'] if n['id']==get('--audit-node'))
value=dict(schema='ndnsf-di-host-node-audit-v1',node_id=node['id'],host=node['host'],
    boot_id=get('--boot-id'),configuration_sha256=get('--configuration-sha256'),
    release_sha256=get('--release-sha256'),job_id=get('--audit-target-job'),
    auditor_job_id='23456',auditor_cgroup='0::/slurm/job_23456/step_0\\n',
    auditor_step_id='0',recovery_id=get('--recovery-id') if OUTCOME!='stale' else 'd'*32,
    status='ZERO',cleanup_scope='HOST_UID_JOB',processes=[],errors=[])
path=root/'nodes'/node['id']/'host-audit-recovery.json'
path.write_text(json.dumps(value));path.chmod(0o600)
if OUTCOME == 'late-exit': raise SystemExit(9)
'''.replace('OUTCOME', repr(outcome))

    def spawn(owner, root, tag, kind, argv, env, guard):
        assert kind == 'audit'
        commands.append(list(argv))
        child, row = real_spawn(owner, root, tag, kind,
            [sys.executable, '-c', reply, *argv[1:]], env, guard)
        children.append(child)
        return child, row

    monkeypatch.setattr(container.cluster, '_spawn', spawn)
    operation = frozen.parent / 'manager' / config['deployment_id']
    container.cluster._private_directory(operation)
    container.cluster.write_json(operation / 'state.json', state)
    result = backend.run_slurm('down', frozen, operation, timeout=5)
    persisted = json.loads((operation / 'state.json').read_text())
    assert len(commands) == len(config['nodes']) == 1
    assert '--gres=none' in commands[0] and '--immediate=15' in commands[0]
    assert commands[0][commands[0].index('--audit-target-job') + 1] == '12345'
    assert children and all(child.poll() is not None for child in children)
    assert not (frozen.parent / 'allocation.json').exists()
    if outcome == 'zero':
        assert result['operation_stage'] == persisted['status'] == 'STOPPED', result
        assert result['status'] == 'FAILED' and result['actor_cleanup'] == 'ZERO'
        assert result['failure'] == 'EDQUOT' and result['oracle'] == 'NOT_RUN'
        assert persisted['needs_closeout'] is False
    else:
        assert result['status'] == 'UNKNOWN' and result['actor_cleanup'] == 'UNVERIFIED'
        assert persisted['needs_closeout'] is True


@pytest.mark.parametrize('lost_submit_reply', [False, True])
def test_installed_slurm_caller_persists_intent_and_never_resubmits(
        ops, deployment, monkeypatch, lost_submit_reply):
    parser, cli = ops
    root, config = deployment
    config = _tiger_config(root, config)
    for key in config['storage'].keys() - {'max_bytes'}:
        config['storage'][key] = str(root / key)
    path = write_config(root, config)
    package = parser.__package__
    manager = importlib.import_module(package + '.cluster')
    backend = importlib.import_module(package + '.slurm')
    adapter_type = backend.SlurmApptainerAdapter
    operation = root / 'state_root' / config['deployment_id']
    state_path = operation / 'state.json'
    calls = []
    timeouts = []
    def runner(command, **options):
        calls.append(command)
        timeouts.append(options['timeout'])
        state = json.loads(state_path.read_text())
        reference = state['job_reference']
        if command[0] == 'sbatch':
            assert state['status'] == 'SUBMITTING' and reference['job_id'] is None
            assert hashlib.sha256(options['input'].encode()).hexdigest() == reference['script_sha256']
            assert '--deployment' in options['input']
            if lost_submit_reply:
                raise subprocess.TimeoutExpired(command, options['timeout'])
            output = '42\n'
        elif command[0] == 'squeue':
            output = ''
        elif command[0] == 'sacct':
            comment = adapter_type._deployment_comment(reference)
            submitted = time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime())
            output = (f'42|{os.getuid()}|{comment}|COMPLETED|0:0|node-a|'
                      f'{reference["job_name"]}|{submitted}\n')
        else:
            raise AssertionError('unexpected scheduler side effect: ' + str(command))
        return subprocess.CompletedProcess(command, 0, stdout=output, stderr='')
    monkeypatch.setattr(backend, 'SlurmApptainerAdapter', lambda: adapter_type(runner=runner))
    first = manager.run_cluster('up', path, timeout=5)
    assert first['status'] == 'UNKNOWN'  # Scheduler completion cannot prove node cleanup/native success.
    assert first['job_reference']['job_id'] == '42'
    state = json.loads(state_path.read_text())
    boot = state['boot_id']
    assert state['needs_closeout']
    first_count = len(calls)
    repeated = manager.run_cluster('up', path, timeout=0.25)
    assert repeated['boot_id'] == boot and repeated['status'] == 'UNKNOWN'
    assert all(0 < value <= 0.25 for value in timeouts[first_count:])
    assert cli.main(['cluster', 'up', '--config', str(path), '--timeout', '5']) == 1
    assert sum(command[0] == 'sbatch' for command in calls) == 1
    # Public status/down locate the persisted reference even after model/image inputs vanish.
    (root / 'candidate.sif').unlink()
    inspected = manager.run_cluster('status', path, timeout=5)
    closed = manager.run_cluster('down', path, timeout=5)
    assert inspected['boot_id'] == closed['boot_id'] == boot
    assert closed['status'] == 'UNKNOWN' and closed['actor_cleanup'] == 'UNVERIFIED'
    assert not any(command[0] == 'scancel' for command in calls)


@pytest.mark.parametrize("change, message", [
    (lambda c: c.update(unknown=True), "unknown fields"),
    (lambda c: c["actors"].append(copy.deepcopy(c["actors"][-1])), "duplicate actor"),
    (lambda c: c["actors"][-1].update(identity="/8=test/8=p0"), "canonical NDN identity"),
    (lambda c: c["actors"][-1].update(node="absent"), "unknown node"),
    (lambda c: c["nodes"][0].update(host="other-host"), "one local node"),
    (lambda c: c["nodes"][0].update(cpus=1), "CPU overcommit"),
    (lambda c: c["nodes"][0].update(cpus=True), "expected integer"),
    (lambda c: c["actors"][-1]["resources"].update(gpu_devices=["2"]), "unavailable node device"),
    (lambda c: c["storage"].update(scratch_root="cache_root/scratch"), "disjoint"),
    (lambda c: c["requester"].update(core={"requester_identity": "/wrong/requester"}), "conflicts"),
    (lambda c: c["security"].update(controller_certificate="missing.cert"), "unavailable"),
    (lambda c: c["actors"][-1].update(parameters={"provider": "/wrong"}), "unknown fields"),
    (lambda c: c["actors"][-1].update(parameters={"workers": 0}), "expected integer"),
    (lambda c: c["actors"][-1].update(parameters={"repo-fetch-timeout-ms": 0}), "expected integer"),
    (lambda c: c["actors"][-1].update(parameters={"repo-fetch-timeout-ms": -1}), "expected integer"),
    (lambda c: c["actors"][-1].update(parameters={"repo-fetch-timeout-ms": True}), "expected integer"),
    (lambda c: c["actors"][-1].update(parameters={"repo-fetch-timeout-ms": 2147483648}), "native signed int"),
    (lambda c: c["actors"][-1].update(parameters={"conversation-retention-ms": 0}), "expected integer"),
    (lambda c: c["actors"][-1].update(parameters={"conversation-retention-ms": -1}), "expected integer"),
    (lambda c: c["actors"][-1].update(parameters={"conversation-retention-ms": True}), "expected integer"),
    (lambda c: c["actors"][-1].update(parameters={"conversation-retention-ms": 3600001}), "native retention limit"),
    (lambda c: c["requester"]["grant"].update(content_key_file="key.pem"), "issuer private inputs"),
    (lambda c: c["artifact_authority"]["authority"].update(protection_epoch="other"), "conflicts"),
    (lambda c: c["requester"]["turns"][0].update(output_file="key.pem"), "overlaps protected input"),
    (lambda c: c["requester"]["turns"][0].update(checkpoint_output_file="input.bin"), "overlaps protected input"),
    (lambda c: c["requester"].update(statistics_file="credentials/requester/pib.db"), "overlaps installed runtime or credentials"),
    (lambda c: c["requester"].update(repository={"path": "."}), "overlaps protected input"),
    (lambda c: c["requester"].update(request={"provider_names": ["/unknown/provider"]}), "undeclared Provider"),
    (lambda c: c["requester"].update(fixed_placement={"role_providers": {"stage-0": "/unknown/provider"}}), "undeclared Provider"),
    (lambda c: c["requester"].update(test_stream_barrier={"ready_file": "ready"}), "experiment harness field"),
    (lambda c: c["requester"]["turns"][0].update(output_file="credentials"), "regular file path"),
])
def test_invalid_configuration_is_rejected_without_mutation(ops, deployment, change, message):
    parser, _ = ops
    root, config = deployment
    change(config)
    path = write_config(root, config)
    before = sorted(str(item.relative_to(root)) for item in root.rglob("*"))
    with pytest.raises(parser.DeploymentConfigError, match=message):
        parser.load_deployment(path)
    assert sorted(str(item.relative_to(root)) for item in root.rglob("*")) == before


def test_installed_file_tampering_and_duplicate_json_keys(ops, deployment):
    parser, _ = ops
    root, config = deployment
    path = write_config(root, config)
    (root / "release/bin/di-native-provider").write_bytes(b"changed")
    with pytest.raises(parser.DeploymentConfigError, match="identity/mode mismatch"):
        parser.load_deployment(path)
    path.write_text('{"schema": "one", "schema": "two"}')
    with pytest.raises(parser.DeploymentConfigError, match="duplicate JSON key"):
        parser.load_deployment(path)


def test_executable_mode_must_match_ledger_and_caller_access(ops, deployment):
    parser, _ = ops
    root, config = deployment
    path = write_config(root, config)
    binary = root / "release/bin/di-native-provider"
    binary.chmod(0o641)
    ledger_path = root / "installed.json"
    ledger = json.loads(ledger_path.read_text())
    ledger["files"][str(binary)]["installed"]["mode"] = 0o641
    ledger_path.write_text(json.dumps(ledger))
    with pytest.raises(parser.DeploymentConfigError, match="identity/mode mismatch"):
        parser.load_deployment(path)


def test_cluster_check_never_imports_app_or_spawns_processes(ops, deployment, monkeypatch, capsys):
    parser, cli = ops
    root, config = deployment
    path = write_config(root, config)

    def reject_spawn(*args, **kwargs):
        raise AssertionError("configuration check spawned a process")

    class RejectApp:
        def find_spec(self, fullname, path=None, target=None):
            if fullname.startswith("ndnsf_distributed_inference.app_sdk") or fullname in {"ndnsf", "_ndnsf"}:
                raise AssertionError("configuration check imported runtime")

    monkeypatch.setattr(subprocess, "Popen", reject_spawn)
    hook = RejectApp()
    sys.meta_path.insert(0, hook)
    try:
        assert cli.main(["cluster", "check", "--config", str(path)]) == 0
    finally:
        sys.meta_path.remove(hook)
    summary = json.loads(capsys.readouterr().out)
    assert summary["status"] == "CONFIGURATION_CHECKED"
    assert summary["owned_actors"] == 5
    assert "key.pem" not in json.dumps(summary)
    assert not (root / "state_root").exists()


def test_actual_wheel_namespace_cluster_check(ops, deployment):
    _, cli = ops
    root, config = deployment
    path = write_config(root, config)
    installed = Path(cli.__file__).parents[2]
    code = """
import importlib.abc
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, sys.argv[1])
class RejectRuntime(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if (fullname.startswith('ndnsf_distributed_inference.app_sdk') or
                fullname == 'ndnsf' or fullname.startswith('ndnsf.') or fullname == '_ndnsf'):
            raise AssertionError('unexpected runtime import: ' + fullname)
sys.meta_path.insert(0, RejectRuntime())
def reject_spawn(*args, **kwargs):
    raise AssertionError('check spawned a process')
subprocess.Popen = reject_spawn
import ndnsf_distributed_inference.ops.cli as cli
assert Path(sys.argv[1]) in Path(cli.__file__).resolve().parents
raise SystemExit(cli.main(['cluster', 'check', '--config', sys.argv[2]]))
"""
    result = subprocess.run([sys.executable, "-I", "-c", code, str(installed), str(path)],
                            cwd=root.parent, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert json.loads(result.stdout)["status"] == "CONFIGURATION_CHECKED"
    assert not (root / "state_root").exists()
