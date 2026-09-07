"""Prepare command/mount boundary; actual native preparation is a later gate."""
import hashlib
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps import yolo
from runtime.baseline import container_command


def descriptor(tmp_path):
    value = dict(schema='tiger-yolo-prepare-input-v2', plan={'schema': 'tiger-yolo-run-plan-v1'},
        templateDigest='sha256:' + 'a' * 64, manifestDigest='sha256:' + 'b' * 64,
        registryDigest='sha256:' + 'c' * 64, placementCandidateDigest='sha256:' + 'd' * 64,
        runtimeCandidateDigest='sha256:' + 'e' * 64,
        protectionEpoch='epoch-1', placementCandidateId='spec183-test')
    path = tmp_path / 'prepare.json'
    path.write_text(json.dumps(value))
    return path, 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()


def test_internal_command_passes_fixed_paths_and_propagates_failure(tmp_path, monkeypatch, capsys):
    path, digest = descriptor(tmp_path)
    calls = []
    def prepare(**kwargs):
        calls.append(kwargs)
        raise RuntimeError('SECRET MUST NOT APPEAR')
    monkeypatch.setattr(yolo, 'prepare_in_container', prepare)
    assert yolo.main(['prepare', '--descriptor', str(path), '--descriptor-sha256', digest]) == 2
    assert calls[0]['package'] == Path('/artifacts')
    assert calls[0]['authority_private'] == Path('/inputs/private/artifact-policy-authority.key')
    assert calls[0]['placement_candidate_digest'] == 'sha256:' + 'd' * 64
    assert calls[0]['runtime_candidate_digest'] == 'sha256:' + 'e' * 64
    output = capsys.readouterr().out
    assert 'SECRET' not in output and json.loads(output)['status'] == 'FAILED'
    assert json.loads(output)['frames']


def test_changed_descriptor_never_calls_prepare(tmp_path, monkeypatch):
    path, digest = descriptor(tmp_path)
    path.write_text('{}')
    def forbidden(**kwargs):
        pytest.fail('prepare must not run on changed descriptor')
    monkeypatch.setattr(yolo, 'prepare_in_container', forbidden)
    assert yolo.main(['prepare', '--descriptor', str(path), '--descriptor-sha256', digest]) == 2


@pytest.mark.parametrize('mutation', ['legacy-schema', 'missing-runtime', 'bad-runtime'])
def test_ambiguous_candidate_descriptor_is_rejected(tmp_path, mutation):
    path, _ = descriptor(tmp_path)
    value = json.loads(path.read_text())
    if mutation == 'legacy-schema':
        value['schema'] = 'tiger-yolo-prepare-input-v1'
    elif mutation == 'missing-runtime':
        del value['runtimeCandidateDigest']
    else:
        value['runtimeCandidateDigest'] = None
    path.write_text(json.dumps(value))
    digest = 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()
    with pytest.raises(ValueError, match='YOLO_PREPARE_DESCRIPTOR'):
        yolo.preparation_arguments(path, digest)


def test_empty_issuer_home_created_by_apptainer_is_allowed(tmp_path):
    public, private = tmp_path / 'public', tmp_path / 'private'
    public.mkdir()
    (private / 'root').mkdir(parents=True)
    yolo.validate_preparation_roots(public, private)
    (private / 'root' / 'old-key').write_bytes(b'preserve')
    with pytest.raises(ValueError, match='NOT_EMPTY'):
        yolo.validate_preparation_roots(public, private)


@pytest.mark.parametrize('bad_candidate', [False, True])
def test_issuer_binds_offers_to_placement_but_receipt_to_runtime(tmp_path, monkeypatch, bad_candidate):
    """Run the preparation producer with mocked crypto/model owners, not a SIF."""
    from types import ModuleType, SimpleNamespace
    from runtime import identities, yolo_bundle
    from test_yolo_preparation import inputs
    template, plan = inputs()
    plan['runId'] = 'run-1'
    public, private, package = (tmp_path / name for name in ('public', 'private', 'package'))
    for root in (public, private, package):
        root.mkdir(mode=0o700)
    template_path, registry = tmp_path / 'template.json', tmp_path / 'registry.json'
    template_path.write_text(json.dumps(template))
    registry.write_text('{}')
    (package / 'manifest.json').write_text(json.dumps({'catalogue': {'candidates': [{
        'candidateId': 'shared-backbone-two-shard-v1', 'candidateDigest': 'sha256:' + '1' * 64}]}}))
    monkeypatch.setattr(yolo, 'Path', lambda value: {'/config': public, '/identities': private}.get(str(value), Path(value)))
    def issue(namespace, names):
        for role in names:
            (private / role).mkdir(mode=0o700)
    monkeypatch.setattr(identities, 'issue', issue)
    monkeypatch.setattr(identities, 'install_yolo_trust', lambda *a, **kw: None)
    monkeypatch.setattr(identities, 'issue_yolo_recipients', lambda *a, **kw: None)
    offers = {}
    monkeypatch.setattr(identities, 'issue_yolo_offers', lambda *a, **kw: offers.update(kw))
    def materialize(case, config, root):
        path = root / 'case-policy.json'
        path.write_text(json.dumps(config))
        return path
    owner = SimpleNamespace(_validate_policy_loader_compatibility=lambda config: None,
                            _materialize_case_config=materialize,
                            build_runtime_publication_file=lambda *a: None)
    monkeypatch.setattr(yolo, '_installed_yolo_owner', lambda: owner)
    adapter = ModuleType('ndnsf_distributed_inference.adapters.yolo')
    adapter.build_yolo26n_adapter = lambda *a, **kw: SimpleNamespace(
        graph=SimpleNamespace(graph_digest='sha256:'+'3'*64),
        splitter=SimpleNamespace(catalogue_digest='sha256:'+'4'*64))
    policy = ModuleType('ndnsf_distributed_inference.policy')
    policy.write_policy_bundle = lambda *a: None
    monkeypatch.setitem(sys.modules, adapter.__name__, adapter)
    monkeypatch.setitem(sys.modules, policy.__name__, policy)
    monkeypatch.setattr(yolo_bundle, 'preparation_inventory', lambda *a: {})
    digest = lambda path: 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()
    placement, runtime = 'sha256:' + '1' * 64, 'sha256:' + '2' * 64
    options = dict(template_path=template_path,
        template_digest=digest(template_path), package=package,
        manifest_digest=digest(package / 'manifest.json'), registry=registry,
        registry_digest=digest(registry), authority_private=tmp_path / 'unused.key',
        protection_epoch='epoch-1', placement_candidate_id='shared-backbone-two-shard-v1',
        placement_candidate_digest=('sha256:' + '9' * 64 if bad_candidate else placement),
        runtime_candidate_digest=runtime)
    if bad_candidate:
        with pytest.raises(ValueError, match='YOLO_PREPARE_PLACEMENT_CANDIDATE'):
            yolo.prepare_in_container(plan, **options)
        assert not offers and not any(private.iterdir())
        return
    receipt = yolo.prepare_in_container(plan, **options)
    assert offers['candidate_digest'] == placement
    assert receipt['candidateDigest'] == runtime
    assert receipt['placementCandidateDigest'] == placement
    assert receipt['graphDigest'] == 'sha256:'+'3'*64
    assert receipt['catalogueDigest'] == 'sha256:'+'4'*64
    assert json.loads((public / 'preparation.json').read_text())['candidateDigest'] == runtime


@pytest.mark.parametrize('scope', ['worker', 'network', 'gpu', 'offline'])
def test_private_inputs_only_mounted_for_offline_preparation(scope):
    kwargs = {'preparation_inputs': Path('/sealed-inputs')}
    if scope != 'worker':
        kwargs['prepare'] = Path('/homes')
    if scope == 'network':
        kwargs['node'] = Path('/node')
    if scope == 'gpu':
        kwargs.update(gpu=True, gpu_device='0')
    args = ({'apptainer': '/bin/apptainer', 'sif': '/image.sif'}, Path('/bundle'),
            Path('/homes/root'), Path('/public'), Path('/output'), ['python', '-m', 'apps.yolo'])
    if scope != 'offline':
        with pytest.raises(ValueError, match='PREPARATION_INPUT_SCOPE'):
            container_command(*args, **kwargs)
    else:
        command = container_command(*args, **kwargs)
        assert '/sealed-inputs:/inputs:ro' in command
        assert '/homes:/identities:rw' in command
        assert '/public:/config:rw' in command
        assert '--nv' not in command
