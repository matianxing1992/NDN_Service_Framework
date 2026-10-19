"""The actual YOLO policy authorizes the migrated native Repo call chain."""
from pathlib import Path
import ast
import re

from ndnsf_distributed_inference.policy import generate_trust_schema, load_or_generate_deployment
from ndnsf_distributed_inference.splitter import SplitterOutput


ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / 'examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml'
PREFIX = '/NDNSF/DistributedRepo/Object/v1/'
ACTOR = '/NDNSF-DistributeInference/example/'


def test_splitter_carries_fixed_anchor_into_core_and_artifact_trust(tmp_path):
    anchor = str(tmp_path / 'root.cert')
    output = SplitterOutput(
        application='trust-regression', controller=ACTOR + 'controller',
        group=ACTOR + 'group', user=ACTOR + 'user',
        provider_prefix=ACTOR + 'provider', services=[],
        trust_app_roots=[ACTOR.rstrip('/')], trust_anchor_file=anchor)
    config = output.to_policy_config()
    assert config['trust']['anchor_file'] == anchor
    assert config['artifact_security']['anchor_file'] == anchor
    schema = generate_trust_schema(config, ())
    assert 'type any' not in schema
    assert f'file-name "{anchor}"' in schema


def test_native_yolo_split_uses_the_selected_credential_bundle_root(tmp_path):
    tree = ast.parse((ROOT / 'Experiments/NDNSF_DI_Yolo2x2_Minindn.py').read_text())
    main = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                and node.name == 'main')
    command = next(node.value for node in main.body if isinstance(node, ast.Assign)
                   and any(isinstance(target, ast.Name) and target.id == 'split_command'
                           for target in node.targets))
    index = next(i for i, node in enumerate(command.elts)
                 if isinstance(node, ast.Constant) and node.value == '--trust-anchor-file')
    expression = compile(ast.Expression(command.elts[index + 1]), '<split-anchor>', 'eval')
    candidate = {'credentialBundles': {'yolo': {'rootCert': '/selected/root.cert'}}}
    assert eval(expression, {'naming_candidate': candidate, 'OUT': tmp_path}) == '/selected/root.cert'
    assert eval(expression, {'naming_candidate': None, 'OUT': tmp_path}) == str(
        tmp_path / 'security/root.cert')


def emitted_permissions(deployment, kind):
    text = Path(deployment.policy_file).read_text()
    return {identity: set(allows.split()) for identity, allows in re.findall(
        kind + r'-policy\s*\{\s*for\s+(\S+)\s*allow\s*\{([^}]+)\}', text)}


def test_native_file_publisher_has_discovery_and_transaction_grants(tmp_path):
    deployment = load_or_generate_deployment(CONFIG, tmp_path)
    users = emitted_permissions(deployment, 'user')
    providers = emitted_permissions(deployment, 'provider')
    for operation in ('CAPABILITY', 'STORE', 'STORE_RANGE', 'COMMIT_RANGES',
                      'ABORT_RANGES', 'STATUS'):
        service = deployment.service_policy(PREFIX + operation)
        assert deployment.controller in service.users
        assert PREFIX + operation in users[deployment.controller]
        assert deployment.user in service.users
        assert PREFIX + operation in users[deployment.user]
        assert {provider.identity for provider in service.providers} == {ACTOR + 'provider/D'}
        assert {actor for actor, allows in providers.items() if PREFIX + operation in allows} == {
            ACTOR + 'provider/D'}
    assert Path(deployment.policy_file).is_file()


def test_compute_providers_can_read_without_repo_write_or_delete_grants(tmp_path):
    deployment = load_or_generate_deployment(CONFIG, tmp_path)
    users = emitted_permissions(deployment, 'user')
    readers = {ACTOR + 'provider' + suffix for suffix in ('', '/A', '/B', '/C')}
    for operation in ('CAPABILITY', 'STATUS', 'FETCH', 'FETCH_RANGE'):
        assert readers <= set(deployment.service_policy(PREFIX + operation).users)
        assert all(PREFIX + operation in users[actor] for actor in readers)
    for operation in ('STORE', 'STORE_RANGE', 'COMMIT_RANGES', 'ABORT_RANGES', 'DELETE'):
        assert not readers.intersection(deployment.service_policy(PREFIX + operation).users)
        assert all(PREFIX + operation not in users[actor] for actor in readers)


def test_native_publisher_uses_the_advertised_repo_epoch():
    tree = ast.parse((CONFIG.parent / 'controller.py').read_text())
    function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                    and node.name == '_deploy_artifacts_to_repo')
    reference = next(node for node in ast.walk(function) if isinstance(node, ast.Call)
                     and isinstance(node.func, ast.Name) and node.func.id == 'ArtifactReference')
    epoch = next(field.value.value for field in reference.keywords
                 if field.arg == 'policy_epoch' and isinstance(field.value, ast.Constant))
    source = (ROOT / 'NDNSF-DistributedRepo/src/RepoNode.cpp').read_text()
    advertised = re.search(r'capability\.policyEpoch\s*=\s*"([^"]+)"', source).group(1)
    assert epoch == advertised
