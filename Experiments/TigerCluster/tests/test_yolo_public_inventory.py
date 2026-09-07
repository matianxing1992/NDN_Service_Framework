"""Public-byte integrity fixtures; not signed material or inference evidence."""
import hashlib
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_bundle import preparation_inventory, verify_preparation
from runtime.yolo_profile import ClosureError


def prepared_public(tmp_path):
    roles = ['BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge', 'controller', 'user', 'repo', 'nfd0']
    plan = dict(namespace='/run', runId='run-1', identities={r: '/run/' + r for r in roles})
    files = ['root.cert', 'wrong-root.cert', 'identities.json', 'recipient-public-keys.json',
        'offer-public-key-map.json', 'offer-trust-root.json', 'case-policy.json', 'case.json',
        'trust-schema.conf', 'controller.policies', 'service-manifest.json', 'service-manifest.json.sha256',
        'native-execution-plan.json', 'native-execution-plan.json.sha256', 'runtime-publication.json',
        'contracts/authority.pub', 'contracts/catalogue.pub', 'contracts/model.pub']
    files += [r + '.cert' for r in roles]
    files += [d + '/' + r + '.pub' for d in ['offers', 'recipients'] for r in roles[:4]]
    for name in files:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'public-fixture')
    (tmp_path / 'contracts/trust-root-registry-v1.json').write_text(json.dumps({
        'catalogue': {'publicKeyPath': 'contracts/catalogue.pub'},
        'modelManifest': {'publicKeyPath': 'contracts/model.pub'},
        'artifactPolicyAuthority': {'publicKeyPath': 'contracts/authority.pub'}}))
    return plan


def seal(tmp_path, plan):
    receipt = dict(schema='tiger-yolo-preparation-v1', status='PREPARED', qualification='NOT_EVALUATED',
        runId=plan['runId'], candidateDigest='sha256:' + 'a' * 64,
        publicFiles=preparation_inventory(tmp_path, plan))
    wire = json.dumps(receipt).encode()
    (tmp_path / 'preparation.json').write_bytes(wire)
    return 'sha256:' + hashlib.sha256(wire).hexdigest()


def test_prepared_bytes_recompute_without_claiming_qualification(tmp_path):
    plan = prepared_public(tmp_path)
    digest = seal(tmp_path, plan)
    result = verify_preparation(tmp_path, plan, expected_receipt_digest=digest,
                                candidate_digest='sha256:' + 'a' * 64)
    assert result['qualification'] == 'NOT_EVALUATED'
    assert result['publicFiles']['case.json']['bytes'] == len(b'public-fixture')


@pytest.mark.parametrize('fault', ['extra', 'directory', 'missing', 'private-pem', 'symlink',
                                  'changed', 'receipt', 'run', 'candidate'])
def test_public_inventory_rejects_incomplete_or_replaced_material(tmp_path, fault):
    plan = prepared_public(tmp_path)
    digest = seal(tmp_path, plan)
    candidate = 'sha256:' + 'a' * 64
    if fault == 'extra':
        (tmp_path / 'requester.key').write_bytes(b'not-public')
    elif fault == 'directory':
        (tmp_path / 'models').mkdir()
    elif fault == 'missing':
        (tmp_path / 'root.cert').unlink()
    elif fault == 'private-pem':
        (tmp_path / 'root.cert').write_bytes(b'-----BEGIN PRIVATE KEY-----\nsecret\n')
    elif fault == 'symlink':
        (tmp_path / 'root.cert').unlink()
        (tmp_path / 'root.cert').symlink_to(tmp_path / 'wrong-root.cert')
    elif fault == 'changed':
        (tmp_path / 'case.json').write_bytes(b'changed')
    elif fault == 'receipt':
        (tmp_path / 'preparation.json').write_text('{}')
    elif fault == 'run':
        plan['runId'] = 'another-run'
    else:
        candidate = 'sha256:' + 'b' * 64
    with pytest.raises(ClosureError):
        verify_preparation(tmp_path, plan, expected_receipt_digest=digest, candidate_digest=candidate)
