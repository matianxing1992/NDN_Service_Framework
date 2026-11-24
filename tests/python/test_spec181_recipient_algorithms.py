"""Configured grant key algorithms; these focused tests do not start NFD."""

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.backends import default_backend

from test_spec181_y_b_grant_seam import seam


def replace_recipient(seam, curve):
    key = ec.generate_private_key(curve, default_backend())
    (seam.root / "recipient.key").write_bytes(key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    return key




def test_python_provider_loads_p256_recipient(seam):
    expected = replace_recipient(seam, ec.SECP256R1())
    _, actual, identity = seam.provider_module._load_grant_keys("BackboneNeck")
    assert isinstance(actual, ec.EllipticCurvePrivateKey)
    assert actual.private_numbers() == expected.private_numbers()
    assert identity == "operator-policy-authority"


def test_provider_rejects_unsupported_recipient_curve(seam):
    replace_recipient(seam, ec.SECP384R1())
    with pytest.raises(ValueError):
        seam.provider_module._load_grant_keys("BackboneNeck")
