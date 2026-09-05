from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from cryptography.hazmat.primitives.asymmetric import ed25519  # noqa: E402
from ndnsf_distributed_inference.core import (  # noqa: E402
    GrantRequestV1, PlaintextLeaseRegistry, RevocationStateV1,
)
from ndnsf_distributed_inference.security import ArtifactPolicyAuthority  # noqa: E402


def _now_ms() -> int:
    return int(time.time() * 1000)


class ArtifactSecurityTest(unittest.TestCase):
    def setUp(self):
        self.requester_key = ed25519.Ed25519PrivateKey.generate()
        self.recipient_key = ed25519.Ed25519PrivateKey.generate()
        self.authority = ArtifactPolicyAuthority(
            "/authority/artifact-policy",
            ed25519.Ed25519PrivateKey.generate(),
            protection_epoch="epoch-1",
        )

    def request(self, **overrides):
        fields = dict(
            provider_identity="/provider/p0",
            request_id="req-1",
            attempt=1,
            plan_core_digest="sha256:" + "a" * 64,
            grant_view_digest="sha256:" + "b" * 64,
            model_manifest_digest="sha256:" + "c" * 64,
            protection_epoch="epoch-1",
            requester_identity="/user/u0",
            issued_at_ms=_now_ms(),
        )
        fields.update(overrides)
        return GrantRequestV1(**fields).sign(self.requester_key)

    def test_grant_is_bound_to_core_provider_and_revocation(self):
        request = self.request()
        grant = self.authority.issue(
            request,
            requester_public_key=self.requester_key.public_key(),
            recipient_public_key=self.recipient_key.public_key(),
            content_key=b"content-key-32-bytes-000000000000",
            key_id="key-1",
            expires_at_ms=_now_ms() + 60_000,
            now_ms=_now_ms(),
        )
        grant.verify(self.authority.public_key, now_ms=_now_ms())
        state = self.authority.revoke(
            frozenset({grant.grant_digest}), now_ms=_now_ms(),
            next_check_at_ms=_now_ms() + 60_000)
        self.assertTrue(state.is_revoked(grant.grant_digest, now_ms=_now_ms()))
        with self.assertRaises(ValueError):
            other = ed25519.Ed25519PrivateKey.generate()
            grant.verify(other.public_key(), now_ms=_now_ms())

    def test_plaintext_lease_zeroizes_and_removes_file(self):
        registry = PlaintextLeaseRegistry()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "plaintext.onnx"
            registry.register("lease-1", path, b"secret-model")
            self.assertTrue(path.exists())
            registry.zeroize("lease-1")
            self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main()
