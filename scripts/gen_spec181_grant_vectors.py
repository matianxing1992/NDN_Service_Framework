"""Generate the spec181 cross-language grant parity vectors (T003).

One fixed grant byte stream (canonical JSON) is unwrapped by BOTH the
Python verifier and the native verifier; both must derive the same content
key.  The vectors cover the positive case and every negative case (wrong
recipient, cross request/attempt/plan-core/model/epoch binding, expiry,
forged authority signature).

Regenerate with:

    python3 scripts/gen_spec181_grant_vectors.py > tests/fixtures/spec181/grant-vectors-v1.json

Any change to the canonical encodings MUST regenerate this file and
re-verify both sides (T003 acceptance).
"""

from __future__ import annotations

import hashlib
import json
import sys
import time

sys.path.insert(0, "NDNSF-DistributedInference")

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519

from ndnsf_distributed_inference.core.protected_artifacts import (
    GrantRequestV1, KeyGrantV1, grant_from_wire, grant_to_wire,
    verify_and_unwrap_grant)
from ndnsf_distributed_inference.security.artifact_policy_authority import (
    ArtifactPolicyAuthority)

PROVIDER = "/provider/p0"
REQUEST_ID = "req-vector-1"
ATTEMPT = 7
PLAN_CORE = "sha256:" + "cd" * 32
MODEL_MANIFEST = "sha256:" + "11" * 32
EPOCH = "spec180-yolo-protected-v1"


def derive_seed(label: str) -> bytes:
    return hashlib.sha256(
        ("spec181-grant-vectors-v1:" + label).encode("utf-8")).digest()


def derive_key(label: str) -> ed25519.Ed25519PrivateKey:
    return ed25519.Ed25519PrivateKey.from_private_bytes(derive_seed(label))


def main() -> int:
    authority_key = derive_key("authority")
    recipient_key = derive_key("recipient")
    requester_key = derive_key("requester")
    wrong_recipient_key = derive_key("wrong-recipient")
    content_key = derive_seed("content-key")[:32]
    now_ms = 1730000000000
    ttl_ms = 3_600_000
    authority = ArtifactPolicyAuthority(
        "/authority/artifact-policy", authority_key,
        protection_epoch=EPOCH,
        allowed_model_manifests=frozenset({MODEL_MANIFEST}),
    )

    def signed_request(*, request_id=REQUEST_ID, attempt=ATTEMPT,
                       plan_core=PLAN_CORE, model=MODEL_MANIFEST, epoch=EPOCH):
        return GrantRequestV1(
            provider_identity=PROVIDER, request_id=request_id, attempt=attempt,
            plan_core_digest=plan_core,
            grant_view_digest="sha256:" + "55" * 32,
            model_manifest_digest=model, protection_epoch=epoch,
            requester_identity="/user/u0", issued_at_ms=now_ms,
        ).sign(requester_key)

    def issue(request, *, recipient=recipient_key, expires_at_ms=now_ms + ttl_ms):
        return authority.issue(
            request,
            requester_public_key=requester_key.public_key(),
            recipient_public_key=recipient.public_key(),
            content_key=content_key, key_id="key-vector-1",
            expires_at_ms=expires_at_ms, now_ms=now_ms)

    def expect_unwrap(wire, *, request_id=REQUEST_ID, attempt=ATTEMPT,
                      plan_core=PLAN_CORE, model=MODEL_MANIFEST, epoch=EPOCH,
                      recipient=recipient_key, verify_now=now_ms + 1000):
        """Unwrap the given wire bytes under the stated binding expectations.

        This is the same production call the Provider makes; the wire under
        test is exactly the wire in the vector case (never re-issued here).
        """
        try:
            grant = grant_from_wire(wire.encode("utf-8"))
            key = verify_and_unwrap_grant(
                grant,
                authority_public_key=authority_key.public_key(),
                recipient_private_key=recipient,
                expected_provider_identity=PROVIDER,
                expected_request_id=request_id,
                expected_attempt=attempt,
                expected_plan_core_digest=plan_core,
                expected_model_manifest_digest=model,
                expected_protection_epoch=epoch,
                now_ms=verify_now,
            )
            return {"ok": True, "contentKey": key.hex(), "reason": ""}
        except ValueError as exc:
            return {"ok": False, "contentKey": "", "reason": str(exc)}

    def binding_for(*, request_id=REQUEST_ID, attempt=ATTEMPT,
                    plan_core=PLAN_CORE, model=MODEL_MANIFEST, epoch=EPOCH,
                    verify_now=now_ms + 1000):
        return {
            "requestId": request_id, "attempt": attempt,
            "planCoreDigest": plan_core, "modelManifestDigest": model,
            "protectionEpoch": epoch, "nowMs": verify_now,
        }

    cases = []
    positive = issue(signed_request())
    positive_wire = grant_to_wire(positive).decode("utf-8")
    cases.append({
        "name": "positive-roundtrip",
        "wire": positive_wire,
        "binding": binding_for(),
        "expected": expect_unwrap(positive_wire),
    })
    wrong_recipient_wire = grant_to_wire(issue(
        signed_request(), recipient=wrong_recipient_key)).decode("utf-8")
    cases.append({
        "name": "wrong-recipient",
        "wire": wrong_recipient_wire,
        "binding": binding_for(),
        "expected": expect_unwrap(wrong_recipient_wire),
    })
    cases.append({
        "name": "cross-request",
        "wire": positive_wire,
        "binding": binding_for(request_id="req-vector-OTHER"),
        "expected": expect_unwrap(positive_wire, request_id="req-vector-OTHER"),
    })
    cases.append({
        "name": "cross-attempt",
        "wire": positive_wire,
        "binding": binding_for(attempt=8),
        "expected": expect_unwrap(positive_wire, attempt=8),
    })
    cases.append({
        "name": "cross-plan-core",
        "wire": positive_wire,
        "binding": binding_for(plan_core="sha256:" + "ce" * 32),
        "expected": expect_unwrap(positive_wire, plan_core="sha256:" + "ce" * 32),
    })
    cases.append({
        "name": "cross-model-manifest",
        "wire": positive_wire,
        "binding": binding_for(model="sha256:" + "12" * 32),
        "expected": expect_unwrap(positive_wire, model="sha256:" + "12" * 32),
    })
    cases.append({
        "name": "cross-epoch",
        "wire": positive_wire,
        "binding": binding_for(epoch="spec180-yolo-protected-v2"),
        "expected": expect_unwrap(positive_wire, epoch="spec180-yolo-protected-v2"),
    })
    # Expired: same payload, expired timestamp, digest/signature rebuilt.
    expired = KeyGrantV1(
        policy_authority=positive.policy_authority,
        provider_identity=positive.provider_identity,
        request_id=positive.request_id, attempt=positive.attempt,
        plan_core_digest=positive.plan_core_digest,
        model_manifest_digest=positive.model_manifest_digest,
        protection_epoch=positive.protection_epoch,
        key_id=positive.key_id,
        wrapped_content_key=positive.wrapped_content_key,
        allowed_residency_tiers=positive.allowed_residency_tiers,
        issued_at_ms=positive.issued_at_ms,
        expires_at_ms=now_ms - 1,
        revocation_sequence=positive.revocation_sequence,
        grant_digest="",
        authority_signature="",
    )
    expired = KeyGrantV1(
        policy_authority=expired.policy_authority,
        provider_identity=expired.provider_identity,
        request_id=expired.request_id, attempt=expired.attempt,
        plan_core_digest=expired.plan_core_digest,
        model_manifest_digest=expired.model_manifest_digest,
        protection_epoch=expired.protection_epoch,
        key_id=expired.key_id,
        wrapped_content_key=expired.wrapped_content_key,
        allowed_residency_tiers=expired.allowed_residency_tiers,
        issued_at_ms=expired.issued_at_ms,
        expires_at_ms=expired.expires_at_ms,
        revocation_sequence=expired.revocation_sequence,
        grant_digest=expired.computed_grant_digest(),
        authority_signature=authority_key.sign(
            expired.signing_bytes()).hex(),
    )
    expired_wire = grant_to_wire(expired).decode("utf-8")
    cases.append({
        "name": "expired",
        "wire": expired_wire,
        "binding": binding_for(verify_now=now_ms + ttl_ms + 1000),
        "expected": expect_unwrap(
            expired_wire, verify_now=now_ms + ttl_ms + 1000),
    })
    # Forged authority signature over the same payload.
    evil_key = derive_key("evil-authority")
    forged = issue(signed_request())
    forged = KeyGrantV1(
        policy_authority=forged.policy_authority,
        provider_identity=forged.provider_identity,
        request_id=forged.request_id, attempt=forged.attempt,
        plan_core_digest=forged.plan_core_digest,
        model_manifest_digest=forged.model_manifest_digest,
        protection_epoch=forged.protection_epoch,
        key_id=forged.key_id,
        wrapped_content_key=forged.wrapped_content_key,
        allowed_residency_tiers=forged.allowed_residency_tiers,
        issued_at_ms=forged.issued_at_ms,
        expires_at_ms=forged.expires_at_ms,
        revocation_sequence=forged.revocation_sequence,
        grant_digest=forged.computed_grant_digest(),
        authority_signature=evil_key.sign(forged.signing_bytes()).hex(),
    )
    forged_wire = grant_to_wire(forged).decode("utf-8")
    cases.append({
        "name": "forged-authority-signature",
        "wire": forged_wire,
        "binding": binding_for(),
        "expected": expect_unwrap(forged_wire),
    })

    vectors = {
        "schema": "spec181-grant-vectors-v1",
        "authorityPublicKeyRaw": authority_key.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw).hex(),
        "recipientSeed": derive_seed("recipient").hex(),
        "providerIdentity": PROVIDER,
        "requestId": REQUEST_ID,
        "attempt": ATTEMPT,
        "planCoreDigest": PLAN_CORE,
        "modelManifestDigest": MODEL_MANIFEST,
        "protectionEpoch": EPOCH,
        "nowMs": now_ms,
        "cases": cases,
    }
    json.dump(vectors, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
