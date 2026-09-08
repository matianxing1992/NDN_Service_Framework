#!/usr/bin/env python3
"""Offline independent SDK/cryptography check of C++ grant issuer output.

All private key values here are public deterministic test fixtures, never operator keys.
"""
import json
from pathlib import Path
import sys
from cryptography.hazmat.primitives.asymmetric import ed25519, ec
from cryptography.hazmat.backends import default_backend

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core.protected_artifacts import (
    GrantRequestV1, grant_from_wire, grant_to_wire, unwrap_content_key)

rows = [json.loads(line) for line in Path(sys.argv[1]).read_text().splitlines()]
assert {r["kind"] for r in rows} == {"ed25519", "unicode", "p256", "x25519"}
assert len(rows) == 4
authority = ed25519.Ed25519PrivateKey.from_private_bytes(b"a" * 32)
requester = ed25519.Ed25519PrivateKey.from_private_bytes(b"b" * 32)
for row in rows:
    payload = json.loads(row["request"])
    request = GrantRequestV1(
        provider_identity=payload["providerIdentity"], request_id=payload["requestId"],
        attempt=payload["attempt"], plan_core_digest=payload["planCoreDigest"],
        grant_view_digest=payload["grantViewDigest"], model_manifest_digest=payload["modelManifestDigest"],
        protection_epoch=payload["protectionEpoch"], allowed_residency_tiers=payload["allowedResidencyTiers"],
        purpose=payload["purpose"], requester_identity=payload["requesterIdentity"],
        issued_at_ms=payload["issuedAtMs"], requester_signature=row["signature"])
    assert request.signing_bytes() == row["request"].encode()
    assert request.sign(requester).requester_signature == row["signature"]
    request.verify(requester.public_key())
    grant = grant_from_wire(row["wire"].encode())
    assert grant_to_wire(grant) == row["wire"].encode()
    grant.verify(authority.public_key(), now_ms=1001)
    recipient = (ec.derive_private_key(7, ec.SECP256R1(), default_backend()) if row["kind"] == "p256"
                 else ed25519.Ed25519PrivateKey.from_private_bytes(b"c" * 32))
    key = unwrap_content_key(recipient, grant.wrapped_content_key,
        provider_identity=grant.provider_identity, request_id=grant.request_id,
        attempt=grant.attempt, plan_core_digest=grant.plan_core_digest,
        model_manifest_digest=grant.model_manifest_digest, protection_epoch=grant.protection_epoch)
    assert key == bytes([42]) * 32
print(json.dumps({"ok": True, "grants": len(rows), "request_signatures": len(rows),
                  "independent_unwraps": len(rows)}))
