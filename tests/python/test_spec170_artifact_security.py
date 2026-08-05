from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core import (  # noqa: E402
    GrantRequestV1, PlaintextLeaseRegistry, RevocationStateV1,
)
from ndnsf_distributed_inference.security import ArtifactPolicyAuthority  # noqa: E402


class ArtifactSecurityTest(unittest.TestCase):
    def request(self):
        return GrantRequestV1(
            provider="/provider/p0", request_id="req-1", attempt=1,
            plan_core_digest="sha256:" + "a" * 64,
            grant_view_digest="sha256:" + "b" * 64,
            artifact_digest="sha256:" + "c" * 64,
            recipient="/provider/p0")

    def test_grant_is_bound_to_core_provider_and_revocation(self):
        authority = ArtifactPolicyAuthority("/authority", b"k" * 32)
        grant = authority.issue(self.request(), wrapped_key=b"wrapped", expires_at_ms=500)
        grant.verify(authority="/authority", key=b"k" * 32, now_ms=100)
        state = RevocationStateV1("/authority", "epoch-1", 1,
                                  revoked_grants=(grant.request_digest,),
                                  next_check_at_ms=500)
        self.assertTrue(state.is_revoked(grant, now_ms=100))
        with self.assertRaises(ValueError):
            grant.verify(authority="/other", key=b"k" * 32, now_ms=100)

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
