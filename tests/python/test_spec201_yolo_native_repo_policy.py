"""The actual YOLO policy authorizes the migrated native Repo call chain."""
from pathlib import Path
import ast
import re

from ndnsf_distributed_inference.policy import load_or_generate_deployment


ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / 'examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml'
PREFIX = '/NDNSF/DistributedRepo/Object/v1/'
ACTOR = '/NDNSF-DistributeInference/example/'


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
