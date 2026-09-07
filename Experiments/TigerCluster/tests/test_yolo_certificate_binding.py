"""Real NDN wire parsing; synthetic Data are NOT authenticated certificates."""
import base64
from pathlib import Path
import sys

import pytest
from ndn.encoding import make_data, MetaInfo
from ndn.security import DigestSha256Signer

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.identities import certificate_binding


def wire(name):
    return base64.b64encode(make_data(name, MetaInfo(), b'fixture', DigestSha256Signer()))


def test_metadata_uses_actual_encoded_certificate_name():
    assert certificate_binding(wire('/run/BackboneNeck/KEY/key1/issuer/v=7'),
                               '/run/BackboneNeck') == {
        'certificateName': '/run/BackboneNeck/KEY/key1/issuer/v=7',
        'keyLocatorPrefix': '/run/BackboneNeck/KEY/key1'}


@pytest.mark.parametrize('name', ['/run/Merge/KEY/key1/issuer/v=7',
    '/run/BackboneNeck/key1/issuer/v=7', '/run/BackboneNeck/NOTKEY/key1/issuer/v=7',
    '/run/BackboneNeck/KEY/key1/issuer/not-version'])
def test_wrong_identity_or_certificate_structure_rejected(name):
    with pytest.raises(ValueError, match='CERTIFICATE'):
        certificate_binding(wire(name), '/run/BackboneNeck')
