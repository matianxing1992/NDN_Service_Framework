"""Real grant-key generation/decoding; NDN PIBs are layout fixtures only."""
import json
import stat
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import pytest
from cryptography.hazmat.primitives import serialization
from runtime.identities import issue_yolo_recipients
from ndnsf_distributed_inference.security.registry_keys import (
    load_grant_recipient_private_key, load_grant_recipient_public_map)
from test_yolo_worker import prepared


def setup(tmp_path):
    inputs = prepared(tmp_path, rank=0, mode='local-cpu')
    names = {role: '/run/' + role for role in inputs['homes']}
    return inputs, names


def test_generated_keys_are_publicly_loadable_and_private_to_each_role(tmp_path):
    inputs, names = setup(tmp_path)
    issue_yolo_recipients('/run', inputs['homes'], inputs['public'], names)
    public = load_grant_recipient_public_map(inputs['public'] / 'recipient-public-keys.json')
    seen = set()
    for role in ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge'):
        home = inputs['homes'][role]
        key_file = home / 'recipient.pem'
        key = load_grant_recipient_private_key(key_file)
        wire = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        assert wire == public[names[role]].public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        assert wire not in seen
        seen.add(wire)
        assert stat.S_IMODE(key_file.stat().st_mode) == 0o600
        assert json.loads((home / 'recipient-map.json').read_text()) == {
            names[role]: '/identities/' + role + '/recipient.pem'}
    assert len((inputs['homes']['user'] / 'requester.key').read_bytes()) == 32
    assert not (inputs['homes']['user'] / 'recipient.pem').exists()
    assert not list(inputs['public'].rglob('*.pem'))
    with pytest.raises(ValueError, match='REUSE'):
        issue_yolo_recipients('/run', inputs['homes'], inputs['public'], names)


@pytest.mark.parametrize('fault', ['missing-role', 'duplicate-identity', 'existing-secret'])
def test_bad_preparation_does_not_create_public_outputs(tmp_path, fault):
    inputs, names = setup(tmp_path)
    if fault == 'missing-role':
        del names['Merge']
    elif fault == 'duplicate-identity':
        names['Merge'] = names['BackboneNeck']
    else:
        (inputs['homes']['user'] / 'requester.key').write_bytes(b'preserve')
    with pytest.raises(ValueError):
        issue_yolo_recipients('/run', inputs['homes'], inputs['public'], names)
    assert not (inputs['public'] / 'recipients').exists()
