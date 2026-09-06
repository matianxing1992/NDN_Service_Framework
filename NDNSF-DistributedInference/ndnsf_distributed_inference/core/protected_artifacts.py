"""Canonical protected-artifact grant encodings for NDNSF-DI.

Implements the Spec170 ``artifact-assembly-v1`` protected-profile key contract
(KeyGrantV1 / GrantRequestV1) with canonical cross-language wire encodings.
All digests are non-circular: a record's digest and signature are computed
over the canonical bytes of every other field, so a grant digest never hashes
a field containing that same digest.

The revocation subsystem (RevocationStateV1 ledger and its network service)
is intentionally NOT implemented on this branch: the owner develops it on a
separate branch/machine. ``revocationSequence`` stays in the wire encoding as
a passive contract-stable field fixed at 1, so the later integration does not
change the canonical bytes. Expiry is the only time-bound rejection here.

Wire convention (matches the native Provider parser): canonical JSON with
sorted keys, no whitespace, snake_case field names, ``sha256:`` digest prefix,
hex-encoded signatures and envelope components.

Recipient envelopes use ECDH + HKDF-SHA256 + AES-256-GCM:
- ``X25519-AESGCM-SHA256`` for Ed25519 recipient keys (Edwards->Montgomery
  conversion of both the public point and the private seed);
- ``ECDH-P256-AESGCM-SHA256`` for EC P-256 recipient certificates.

The AES-GCM AAD is the canonical binding context (provider identity, request,
attempt, plan core, model manifest, protection epoch), so a grant unwrapped
under a different request/attempt/core/model/epoch fails closed at
authentication, not at policy comparison.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
import hashlib
import hmac
import json
import os
import stat
from pathlib import Path
from threading import RLock
from typing import Any, Mapping, Sequence, Union

from cryptography.hazmat.primitives.asymmetric import ec, ed25519, x25519
from cryptography.hazmat.primitives.asymmetric.ec import (
    EllipticCurvePrivateKey, EllipticCurvePublicKey)
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.backends import default_backend
from cryptography.exceptions import InvalidSignature, InvalidTag

# ---------------------------------------------------------------------------
# Canonical encoding helpers

_P = 2**255 - 19
_GRANT_KDF_INFO = b"NDNSF-DI/key-grant/v1"
_GRANT_PURPOSE = "DISK_CIPHERTEXT_ASSEMBLED"
_GRANT_POLICY = "CANCEL_IMMEDIATELY"


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def _digest_bytes(raw: bytes) -> str:
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def canonical_digest(value: Any) -> str:
    return _digest_bytes(_canonical_bytes(value))


def _require_digest(value: str, name: str) -> None:
    if not isinstance(value, str) or not value.startswith("sha256:"):
        raise ValueError(f"{name} is not canonical")


# ---------------------------------------------------------------------------
# Recipient envelope (ECDH + HKDF + AES-GCM)

@dataclass(frozen=True)
class RecipientEnvelopeV1:
    """Content-key envelope encrypted to one Provider identity certificate."""

    alg: str
    kdf: str = "HKDF-SHA256"
    ephemeral_public_key: str = ""
    nonce: str = ""
    ciphertext: str = ""

    def __post_init__(self) -> None:
        if self.alg not in ("X25519-AESGCM-SHA256", "ECDH-P256-AESGCM-SHA256"):
            raise ValueError("unsupported recipient envelope algorithm")
        for name in ("ephemeral_public_key", "nonce", "ciphertext"):
            if not getattr(self, name):
                raise ValueError(f"recipient envelope is missing {name}")
        bytes.fromhex(self.ephemeral_public_key)
        bytes.fromhex(self.nonce)
        bytes.fromhex(self.ciphertext)

    def to_dict(self) -> dict[str, str]:
        return {
            "alg": self.alg,
            "kdf": self.kdf,
            "ephemeralPublicKey": self.ephemeral_public_key,
            "nonce": self.nonce,
            "ciphertext": self.ciphertext,
        }

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "RecipientEnvelopeV1":
        try:
            return cls(
                alg=str(payload["alg"]),
                kdf=str(payload.get("kdf", "HKDF-SHA256")),
                ephemeral_public_key=str(payload["ephemeralPublicKey"]),
                nonce=str(payload["nonce"]),
                ciphertext=str(payload["ciphertext"]),
            )
        except KeyError as exc:
            raise ValueError("recipient envelope is incomplete") from exc


def _ed25519_public_to_x25519(public: ed25519.Ed25519PublicKey) -> x25519.X25519PublicKey:
    raw = public.public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    y = int.from_bytes(raw, "little") & ((1 << 255) - 1)
    u = (1 + y) * pow(1 - y, _P - 2, _P) % _P
    return x25519.X25519PublicKey.from_public_bytes(u.to_bytes(32, "little"))


def _ed25519_private_to_x25519(private: ed25519.Ed25519PrivateKey) -> x25519.X25519PrivateKey:
    seed = private.private_bytes(
        serialization.Encoding.Raw,
        serialization.PrivateFormat.Raw,
        serialization.NoEncryption())
    derived = hashlib.sha512(seed).digest()
    return x25519.X25519PrivateKey.from_private_bytes(derived[:32])


RecipientPublicKey = Union[
    ed25519.Ed25519PublicKey, EllipticCurvePublicKey, x25519.X25519PublicKey]
RecipientPrivateKey = Union[
    ed25519.Ed25519PrivateKey, EllipticCurvePrivateKey, x25519.X25519PrivateKey]


def _binding_context(*, provider_identity: str, request_id: str, attempt: int,
                     plan_core_digest: str, model_manifest_digest: str,
                     protection_epoch: str) -> bytes:
    return _canonical_bytes({
        "providerIdentity": provider_identity,
        "requestId": request_id,
        "attempt": attempt,
        "planCoreDigest": plan_core_digest,
        "modelManifestDigest": model_manifest_digest,
        "protectionEpoch": protection_epoch,
    })


def wrap_content_key(recipient_public_key: RecipientPublicKey, *,
                     provider_identity: str, request_id: str, attempt: int,
                     plan_core_digest: str, model_manifest_digest: str,
                     protection_epoch: str, content_key: bytes) -> RecipientEnvelopeV1:
    """Encrypt a content key to one Provider identity with a bound context."""
    if not content_key or len(content_key) > 256:
        raise ValueError("content key is empty or oversized")
    context = _binding_context(
        provider_identity=provider_identity, request_id=request_id,
        attempt=attempt, plan_core_digest=plan_core_digest,
        model_manifest_digest=model_manifest_digest,
        protection_epoch=protection_epoch)
    if isinstance(recipient_public_key, ed25519.Ed25519PublicKey):
        peer = _ed25519_public_to_x25519(recipient_public_key)
        ephemeral = x25519.X25519PrivateKey.generate()
        shared = ephemeral.exchange(peer)
        alg = "X25519-AESGCM-SHA256"
        ephemeral_public = ephemeral.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    elif isinstance(recipient_public_key, x25519.X25519PublicKey):
        ephemeral = x25519.X25519PrivateKey.generate()
        shared = ephemeral.exchange(recipient_public_key)
        alg = "X25519-AESGCM-SHA256"
        ephemeral_public = ephemeral.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    elif isinstance(recipient_public_key, EllipticCurvePublicKey):
        ephemeral = ec.generate_private_key(ec.SECP256R1(), default_backend())
        shared = ephemeral.exchange(ec.ECDH(), recipient_public_key)
        alg = "ECDH-P256-AESGCM-SHA256"
        ephemeral_public = ephemeral.public_key().public_bytes(
            serialization.Encoding.X962,
            serialization.PublicFormat.UncompressedPoint)
    else:
        raise TypeError("unsupported recipient public key type")
    derived = HKDF(
        algorithm=hashes.SHA256(), length=32, salt=None,
        info=_GRANT_KDF_INFO + context,
        backend=default_backend(),
    ).derive(shared)
    nonce = os.urandom(12)
    ciphertext = AESGCM(derived).encrypt(
        nonce, content_key, associated_data=context)
    return RecipientEnvelopeV1(
        alg=alg, ephemeral_public_key=ephemeral_public.hex(),
        nonce=nonce.hex(), ciphertext=ciphertext.hex())


def unwrap_content_key(recipient_private_key: RecipientPrivateKey,
                       envelope: RecipientEnvelopeV1, *,
                       provider_identity: str, request_id: str, attempt: int,
                       plan_core_digest: str, model_manifest_digest: str,
                       protection_epoch: str) -> bytes:
    """Decrypt a bound content-key envelope under one Provider identity."""
    context = _binding_context(
        provider_identity=provider_identity, request_id=request_id,
        attempt=attempt, plan_core_digest=plan_core_digest,
        model_manifest_digest=model_manifest_digest,
        protection_epoch=protection_epoch)
    ephemeral_public = bytes.fromhex(envelope.ephemeral_public_key)
    nonce = bytes.fromhex(envelope.nonce)
    ciphertext = bytes.fromhex(envelope.ciphertext)
    if envelope.alg == "X25519-AESGCM-SHA256":
        if isinstance(recipient_private_key, x25519.X25519PrivateKey):
            private = recipient_private_key
        elif isinstance(recipient_private_key, ed25519.Ed25519PrivateKey):
            private = _ed25519_private_to_x25519(recipient_private_key)
        else:
            raise TypeError("X25519 envelope requires an X25519/Ed25519 key")
        peer = x25519.X25519PublicKey.from_public_bytes(ephemeral_public)
        shared = private.exchange(peer)
    elif envelope.alg == "ECDH-P256-AESGCM-SHA256":
        if not isinstance(recipient_private_key, EllipticCurvePrivateKey):
            raise TypeError("P-256 envelope requires an EC private key")
        peer = EllipticCurvePublicKey.from_encoded_point(
            ec.SECP256R1(), ephemeral_public)
        shared = recipient_private_key.exchange(ec.ECDH(), peer)
    else:
        raise ValueError("unsupported recipient envelope algorithm")
    derived = HKDF(
        algorithm=hashes.SHA256(), length=32, salt=None,
        info=_GRANT_KDF_INFO + context,
        backend=default_backend(),
    ).derive(shared)
    try:
        return AESGCM(derived).decrypt(
            nonce, ciphertext, associated_data=context)
    except InvalidTag as exc:
        raise ValueError("content-key envelope failed authentication") from exc


# ---------------------------------------------------------------------------
# GrantRequestV1 — requester-signed request to the ArtifactPolicyAuthority

@dataclass(frozen=True)
class GrantRequestV1:
    provider_identity: str
    request_id: str
    attempt: int
    plan_core_digest: str
    grant_view_digest: str
    model_manifest_digest: str
    protection_epoch: str
    allowed_residency_tiers: tuple[str, ...] = (_GRANT_PURPOSE,)
    purpose: str = _GRANT_PURPOSE
    requester_identity: str = ""
    issued_at_ms: int = 0
    requester_signature: str = ""

    def __post_init__(self) -> None:
        if (not self.provider_identity or not self.request_id
                or self.attempt <= 0 or self.purpose != _GRANT_PURPOSE):
            raise ValueError("invalid protected artifact grant request")
        for name in ("plan_core_digest", "grant_view_digest",
                     "model_manifest_digest"):
            _require_digest(str(getattr(self, name)), name)
        if not self.protection_epoch:
            raise ValueError("grant request protection epoch is empty")
        object.__setattr__(
            self, "allowed_residency_tiers", tuple(self.allowed_residency_tiers))
        if (not self.allowed_residency_tiers
                or any(not tier for tier in self.allowed_residency_tiers)):
            raise ValueError("grant request residency tiers are empty")

    def payload(self) -> dict[str, Any]:
        """Canonical signed payload: every field except the signature."""
        return {
            "providerIdentity": self.provider_identity,
            "requestId": self.request_id,
            "attempt": self.attempt,
            "planCoreDigest": self.plan_core_digest,
            "grantViewDigest": self.grant_view_digest,
            "modelManifestDigest": self.model_manifest_digest,
            "protectionEpoch": self.protection_epoch,
            "allowedResidencyTiers": list(self.allowed_residency_tiers),
            "purpose": self.purpose,
            "requesterIdentity": self.requester_identity,
            "issuedAtMs": self.issued_at_ms,
        }

    def signing_bytes(self) -> bytes:
        return _canonical_bytes(self.payload())

    def digest(self) -> str:
        return _digest_bytes(self.signing_bytes())

    def sign(self, requester_private_key: ed25519.Ed25519PrivateKey) -> "GrantRequestV1":
        signature = requester_private_key.sign(self.signing_bytes()).hex()
        return GrantRequestV1(
            provider_identity=self.provider_identity,
            request_id=self.request_id, attempt=self.attempt,
            plan_core_digest=self.plan_core_digest,
            grant_view_digest=self.grant_view_digest,
            model_manifest_digest=self.model_manifest_digest,
            protection_epoch=self.protection_epoch,
            allowed_residency_tiers=self.allowed_residency_tiers,
            purpose=self.purpose,
            requester_identity=self.requester_identity,
            issued_at_ms=self.issued_at_ms,
            requester_signature=signature,
        )

    def verify(self, requester_public_key: ed25519.Ed25519PublicKey) -> None:
        """Fail closed unless the requester signature covers every field."""
        if not self.requester_identity:
            raise ValueError("grant request has no requester identity")
        if not self.requester_signature:
            raise ValueError("grant request is unsigned")
        try:
            requester_public_key.verify(
                bytes.fromhex(self.requester_signature), self.signing_bytes())
        except InvalidSignature as exc:
            raise ValueError("grant request signature is invalid") from exc


# ---------------------------------------------------------------------------
# KeyGrantV1 — authority-signed, Provider-bound content-key grant

@dataclass(frozen=True)
class KeyGrantV1:
    policy_authority: str
    provider_identity: str
    request_id: str
    attempt: int
    plan_core_digest: str
    model_manifest_digest: str
    protection_epoch: str
    key_id: str
    wrapped_content_key: RecipientEnvelopeV1
    allowed_residency_tiers: tuple[str, ...] = (_GRANT_PURPOSE,)
    issued_at_ms: int = 0
    expires_at_ms: int = 0
    # Passive contract-stable field. The revocation system is owned by a
    # separate branch; on this branch it is fixed at 1 and never advances.
    revocation_sequence: int = 1
    active_request_policy: str = _GRANT_POLICY
    grant_digest: str = ""
    authority_signature: str = ""

    def __post_init__(self) -> None:
        if (not self.policy_authority or not self.provider_identity
                or not self.request_id or self.attempt <= 0
                or not self.protection_epoch or not self.key_id
                or self.active_request_policy != _GRANT_POLICY
                or self.expires_at_ms <= 0
                or self.revocation_sequence <= 0
                or not isinstance(self.wrapped_content_key, RecipientEnvelopeV1)):
            raise ValueError("invalid protected artifact key grant")
        for name in ("plan_core_digest", "model_manifest_digest"):
            _require_digest(str(getattr(self, name)), name)
        object.__setattr__(
            self, "allowed_residency_tiers", tuple(self.allowed_residency_tiers))

    def payload(self) -> dict[str, Any]:
        """Canonical signed payload: every field except grant digest/signature."""
        return {
            "policyAuthority": self.policy_authority,
            "providerIdentity": self.provider_identity,
            "requestId": self.request_id,
            "attempt": self.attempt,
            "planCoreDigest": self.plan_core_digest,
            "modelManifestDigest": self.model_manifest_digest,
            "protectionEpoch": self.protection_epoch,
            "keyId": self.key_id,
            "wrappedContentKey": self.wrapped_content_key.to_dict(),
            "allowedResidencyTiers": list(self.allowed_residency_tiers),
            "issuedAtMs": self.issued_at_ms,
            "expiresAtMs": self.expires_at_ms,
            "revocationSequence": self.revocation_sequence,
            "activeRequestPolicy": self.active_request_policy,
        }

    def signing_bytes(self) -> bytes:
        return _canonical_bytes(self.payload())

    def computed_grant_digest(self) -> str:
        """Non-circular digest over the signed payload only."""
        return _digest_bytes(self.signing_bytes())

    def verify(self, authority_public_key: ed25519.Ed25519PublicKey, *,
               now_ms: int) -> None:
        """Fail closed on bad authority, bad digest, or expiry."""
        if not self.authority_signature:
            raise ValueError("key grant is unsigned")
        if self.grant_digest != self.computed_grant_digest():
            raise ValueError("key grant digest is inconsistent")
        if int(now_ms) >= self.expires_at_ms:
            raise ValueError("key grant is expired")
        try:
            authority_public_key.verify(
                bytes.fromhex(self.authority_signature), self.signing_bytes())
        except InvalidSignature as exc:
            raise ValueError("key grant authority signature is invalid") from exc

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "KeyGrantV1":
        try:
            return cls(
                policy_authority=str(payload["policyAuthority"]),
                provider_identity=str(payload["providerIdentity"]),
                request_id=str(payload["requestId"]),
                attempt=int(payload["attempt"]),
                plan_core_digest=str(payload["planCoreDigest"]),
                model_manifest_digest=str(payload["modelManifestDigest"]),
                protection_epoch=str(payload["protectionEpoch"]),
                key_id=str(payload["keyId"]),
                wrapped_content_key=RecipientEnvelopeV1.from_dict(
                    payload["wrappedContentKey"]),
                allowed_residency_tiers=tuple(
                    str(tier) for tier in payload["allowedResidencyTiers"]),
                issued_at_ms=int(payload["issuedAtMs"]),
                expires_at_ms=int(payload["expiresAtMs"]),
                revocation_sequence=int(payload["revocationSequence"]),
                active_request_policy=str(payload["activeRequestPolicy"]),
                grant_digest=str(payload.get("grantDigest", "")),
                authority_signature=str(payload.get("authoritySignature", "")),
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError("key grant payload is incomplete") from exc


class ProtectedGrantRejected(Exception):
    """A protected grant failed verification at the authorization boundary.

    Raised with the reason that the verifier registered (expiry, binding
    mismatch, bad authority signature, AEAD authentication failure, ...).
    The Provider boundary maps it to the DI_PROTECTED_GRANT_REJECTED error
    family, never to a synthetic rejection reason.
    """


def grant_to_wire(grant: KeyGrantV1) -> bytes:
    """Canonical wire bytes for one KeyGrantV1 (published as signed APP Data).

    The published bytes are the canonical payload plus the non-secret
    digest/signature pair; verification recomputes the digest and checks the
    signature over the payload (non-circular by construction).
    """
    return _canonical_bytes({
        **grant.payload(),
        "grantDigest": grant.grant_digest,
        "authoritySignature": grant.authority_signature,
    })


def grant_from_wire(raw: bytes) -> KeyGrantV1:
    """Parse canonical wire bytes back into a KeyGrantV1 (fields validated)."""
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ValueError("key grant wire bytes are malformed") from exc
    if not isinstance(payload, Mapping):
        raise ValueError("key grant wire payload is not an object")
    return KeyGrantV1.from_dict(payload)


# ---------------------------------------------------------------------------
# Provider-side verification and unwrap

def verify_and_unwrap_grant(
    grant: KeyGrantV1, *,
    authority_public_key: ed25519.Ed25519PublicKey,
    recipient_private_key: RecipientPrivateKey,
    expected_provider_identity: str,
    expected_request_id: str,
    expected_attempt: int,
    expected_plan_core_digest: str,
    expected_model_manifest_digest: str,
    expected_protection_epoch: str,
    now_ms: int,
) -> bytes:
    """Verify a fetched grant and unwrap its content key inside the Provider.

    Every binding mismatch, an expired grant, or an envelope that fails
    authentication fails closed before any content key is exposed.
    Revocation checks are owned by a separate branch and are not part of
    this branch's verification path; expiry is the time-bound rejection.
    """
    if (grant.provider_identity != expected_provider_identity
            or grant.request_id != expected_request_id
            or grant.attempt != expected_attempt
            or grant.plan_core_digest != expected_plan_core_digest
            or grant.model_manifest_digest != expected_model_manifest_digest
            or grant.protection_epoch != expected_protection_epoch):
        raise ValueError("key grant binding does not match the assignment")
    grant.verify(authority_public_key, now_ms=now_ms)
    return unwrap_content_key(
        recipient_private_key, grant.wrapped_content_key,
        provider_identity=expected_provider_identity,
        request_id=expected_request_id, attempt=expected_attempt,
        plan_core_digest=expected_plan_core_digest,
        model_manifest_digest=expected_model_manifest_digest,
        protection_epoch=expected_protection_epoch)


# ---------------------------------------------------------------------------
# Assembled-entry AEAD (spec181 FR-013: the unwrapped content key must be
# consumed by a real cryptographic operation). Derivation follows the
# Spec170 artifact-assembly-v1 contract:
#
#   K_bundle = HKDF(epochContentKey,
#     "NDNSF-DI/assembled/v1" || modelManifestDigest
#     || roleAssemblySpecDigest || storageProfileDigest)
#   K_entry  = HKDF(K_bundle, entryKind)
#
# A (K_entry, nonce) pair is used exactly once; wrong keys or tampered
# ciphertext fail closed at AEAD authentication.

_ASSEMBLED_KDF_PREFIX = b"NDNSF-DI/assembled/v1"
_ASSEMBLED_CIPHERTEXT_SCHEMA = "ndnsf-di-assembled-ciphertext-v1"
_ASSEMBLED_ENTRY_KINDS = ("MODEL_PROTO", "EXTERNAL_DATA")


def assembled_kdf_context(*, model_manifest_digest: str,
                          role_assembly_spec_digest: str,
                          storage_profile_digest: str) -> bytes:
    """Deterministic KDF info bytes for the assembled-bundle key."""
    for name in ("model_manifest_digest", "role_assembly_spec_digest",
                 "storage_profile_digest"):
        _require_digest(str(locals()[name]), name)
    return (_ASSEMBLED_KDF_PREFIX
            + model_manifest_digest.encode("utf-8")
            + role_assembly_spec_digest.encode("utf-8")
            + storage_profile_digest.encode("utf-8"))


def derive_assembled_bundle_key(epoch_content_key: bytes, *,
                                model_manifest_digest: str,
                                role_assembly_spec_digest: str,
                                storage_profile_digest: str) -> bytes:
    """K_bundle = HKDF(epochContentKey, assembled-kdf-context)."""
    if not epoch_content_key or len(epoch_content_key) > 256:
        raise ValueError("epoch content key is empty or oversized")
    return HKDF(
        algorithm=hashes.SHA256(), length=32, salt=None,
        info=assembled_kdf_context(
            model_manifest_digest=model_manifest_digest,
            role_assembly_spec_digest=role_assembly_spec_digest,
            storage_profile_digest=storage_profile_digest),
        backend=default_backend(),
    ).derive(epoch_content_key)


def derive_assembled_entry_key(bundle_key: bytes, entry_kind: str) -> bytes:
    """K_entry = HKDF(K_bundle, entryKind)."""
    if entry_kind not in _ASSEMBLED_ENTRY_KINDS:
        raise ValueError("assembled entry kind is not allowlisted")
    if len(bundle_key) != 32:
        raise ValueError("assembled bundle key is malformed")
    return HKDF(
        algorithm=hashes.SHA256(), length=32, salt=None,
        info=entry_kind.encode("utf-8"),
        backend=default_backend(),
    ).derive(bundle_key)


@dataclass(frozen=True)
class AssembledCiphertextV1:
    """AEAD-sealed assembled entry (DISK_CIPHERTEXT_ASSEMBLED semantics).

    The manifest binds the AEAD/KDF identifiers, KDF-context digest, entry
    nonce, ciphertext length, and ciphertext digest; the raw ciphertext
    travels alongside the manifest and is never exposed inside it.
    """

    schema: str
    entry_kind: str
    kdf_context_digest: str
    model_manifest_digest: str
    role_assembly_spec_digest: str
    storage_profile_digest: str
    nonce: str
    ciphertext: bytes = b""

    def __post_init__(self) -> None:
        if self.schema != _ASSEMBLED_CIPHERTEXT_SCHEMA:
            raise ValueError("assembled ciphertext schema is not recognized")
        if self.entry_kind not in _ASSEMBLED_ENTRY_KINDS:
            raise ValueError("assembled entry kind is not allowlisted")
        _require_digest(self.kdf_context_digest, "kdf_context_digest")
        for name in ("model_manifest_digest", "role_assembly_spec_digest",
                     "storage_profile_digest"):
            _require_digest(str(getattr(self, name)), name)
        bytes.fromhex(self.nonce)
        if not self.ciphertext:
            raise ValueError("assembled ciphertext is empty")

    @property
    def ciphertext_digest(self) -> str:
        return _digest_bytes(self.ciphertext)

    def manifest(self) -> dict[str, Any]:
        """Canonical manifest: every field except the raw ciphertext."""
        return {
            "schema": self.schema,
            "entryKind": self.entry_kind,
            "kdf": "HKDF-SHA256",
            "aead": "AES-256-GCM",
            "kdfContextDigest": self.kdf_context_digest,
            "modelManifestDigest": self.model_manifest_digest,
            "roleAssemblySpecDigest": self.role_assembly_spec_digest,
            "storageProfileDigest": self.storage_profile_digest,
            "nonce": self.nonce,
            "ciphertextLength": len(self.ciphertext),
            "ciphertextDigest": self.ciphertext_digest,
        }

    def manifest_bytes(self) -> bytes:
        return _canonical_bytes(self.manifest())

    def to_bytes(self) -> bytes:
        """Deterministic framing: 8-byte big-endian manifest length, manifest
        JSON, then raw ciphertext."""
        manifest = self.manifest_bytes()
        return len(manifest).to_bytes(8, "big") + manifest + self.ciphertext

    @classmethod
    def from_bytes(cls, raw: bytes) -> "AssembledCiphertextV1":
        """Parse the deterministic framing back into a sealed entry."""
        if len(raw) < 9:
            raise ValueError("assembled ciphertext framing is malformed")
        manifest_length = int.from_bytes(raw[:8], "big")
        if (manifest_length < 1 or manifest_length > 65536
                or len(raw) < 8 + manifest_length + 1):
            raise ValueError("assembled ciphertext framing is malformed")
        ciphertext = raw[8 + manifest_length:]
        try:
            payload = json.loads(raw[8:8 + manifest_length].decode("utf-8"))
        except (UnicodeDecodeError, ValueError) as exc:
            raise ValueError("assembled ciphertext framing is malformed") from exc
        try:
            sealed = cls(
                schema=str(payload["schema"]),
                entry_kind=str(payload["entryKind"]),
                kdf_context_digest=str(payload["kdfContextDigest"]),
                model_manifest_digest=str(payload["modelManifestDigest"]),
                role_assembly_spec_digest=str(payload["roleAssemblySpecDigest"]),
                storage_profile_digest=str(payload["storageProfileDigest"]),
                nonce=str(payload["nonce"]),
                ciphertext=ciphertext,
            )
        except KeyError as exc:
            raise ValueError("assembled ciphertext manifest is incomplete") from exc
        if (int(payload.get("ciphertextLength", -1)) != len(ciphertext)
                or payload.get("ciphertextDigest") != sealed.ciphertext_digest):
            raise ValueError("assembled ciphertext framing does not match its manifest")
        if raw[8:8 + manifest_length] != sealed.manifest_bytes():
            raise ValueError("assembled ciphertext manifest is not the canonical contract")
        return sealed


def encrypt_assembled_entry(epoch_content_key: bytes, plaintext: bytes, *,
                            entry_kind: str,
                            model_manifest_digest: str,
                            role_assembly_spec_digest: str,
                            storage_profile_digest: str) -> AssembledCiphertextV1:
    """AEAD-seal one assembled entry under the derived entry key."""
    if not plaintext:
        raise ValueError("assembled entry plaintext is empty")
    context = assembled_kdf_context(
        model_manifest_digest=model_manifest_digest,
        role_assembly_spec_digest=role_assembly_spec_digest,
        storage_profile_digest=storage_profile_digest)
    bundle_key = derive_assembled_bundle_key(
        epoch_content_key, model_manifest_digest=model_manifest_digest,
        role_assembly_spec_digest=role_assembly_spec_digest,
        storage_profile_digest=storage_profile_digest)
    entry_key = derive_assembled_entry_key(bundle_key, entry_kind)
    nonce = os.urandom(12)
    ciphertext = AESGCM(entry_key).encrypt(nonce, plaintext, associated_data=context)
    return AssembledCiphertextV1(
        schema=_ASSEMBLED_CIPHERTEXT_SCHEMA,
        entry_kind=entry_kind,
        kdf_context_digest=_digest_bytes(context),
        model_manifest_digest=model_manifest_digest,
        role_assembly_spec_digest=role_assembly_spec_digest,
        storage_profile_digest=storage_profile_digest,
        nonce=nonce.hex(),
        ciphertext=ciphertext,
    )


def decrypt_assembled_entry(epoch_content_key: bytes,
                            sealed: AssembledCiphertextV1, *,
                            entry_kind: str | None = None) -> bytes:
    """Decrypt one sealed assembled entry; any mismatch fails authentication.

    Raises ValueError on wrong content key, tampered ciphertext, tampered
    context fields, or a wrong entry kind (DI_PROTECTED_GRANT_REJECTED at
    the call boundary).
    """
    if entry_kind is not None and entry_kind != sealed.entry_kind:
        raise ValueError("assembled entry kind does not match")
    context = assembled_kdf_context(
        model_manifest_digest=sealed.model_manifest_digest,
        role_assembly_spec_digest=sealed.role_assembly_spec_digest,
        storage_profile_digest=sealed.storage_profile_digest)
    if sealed.kdf_context_digest != _digest_bytes(context):
        raise ValueError("assembled KDF context digest does not match")
    entry_key = derive_assembled_entry_key(
        derive_assembled_bundle_key(
            epoch_content_key,
            model_manifest_digest=sealed.model_manifest_digest,
            role_assembly_spec_digest=sealed.role_assembly_spec_digest,
            storage_profile_digest=sealed.storage_profile_digest),
        sealed.entry_kind)
    try:
        return AESGCM(entry_key).decrypt(
            bytes.fromhex(sealed.nonce), sealed.ciphertext,
            associated_data=context)
    except InvalidTag as exc:
        raise ValueError("assembled entry failed authentication") from exc


# ---------------------------------------------------------------------------
# Plaintext lease registry (unchanged behavior)

class ProtectionState(str, Enum):
    NO_GRANT = "NO_GRANT"
    GRANTED = "GRANTED"
    MATERIALIZED = "MATERIALIZED"
    REVOKED = "REVOKED"
    ZEROIZED = "ZEROIZED"
    FAILED_CLOSED = "FAILED_CLOSED"


@dataclass
class _PlaintextLease:
    secret: bytearray
    path: Path | None = None
    fd: int | None = None


class PlaintextLeaseRegistry:
    """Owns mutable secrets and private file leases until cleanup succeeds.

    File descriptors pin the allocation being erased, so replacing a path
    cannot redirect zeroization into an unrelated file. Failed cleanup stays
    registered and does not prevent the remaining leases from being erased.
    """

    def __init__(self) -> None:
        self._leases: dict[str, _PlaintextLease] = {}
        self._lock = RLock()

    def _check_new_lease(self, lease_id: str, plaintext: bytes) -> None:
        if not lease_id or not plaintext:
            raise ValueError("plaintext lease is incomplete")
        if lease_id in self._leases:
            raise ValueError("duplicate plaintext lease ID")

    def register_secret(self, lease_id: str, plaintext: bytes) -> bytearray:
        """Return the owned mutable allocation without creating a disk copy."""
        with self._lock:
            self._check_new_lease(lease_id, plaintext)
            secret = bytearray(plaintext)
            self._leases[lease_id] = _PlaintextLease(secret)
            return secret

    def register(self, lease_id: str, path: str | Path, plaintext: bytes) -> None:
        with self._lock:
            self._check_new_lease(lease_id, plaintext)
            target = Path(path)
            target.parent.mkdir(parents=True, exist_ok=True)
            fd = os.open(target, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW |
                         os.O_NONBLOCK, 0o600)
            try:
                if not stat.S_ISREG(os.fstat(fd).st_mode):
                    raise ValueError("plaintext lease requires a regular file")
                os.fchmod(fd, 0o600)
            except BaseException:
                os.close(fd)
                raise
            secret = bytearray(plaintext)
            self._leases[lease_id] = _PlaintextLease(secret, target, fd)
            # Register ownership before writing: a partial write is still
            # discoverable by the enclosing runtime's cleanup boundary.
            try:
                os.ftruncate(fd, 0)
                offset = 0
                while offset < len(secret):
                    count = os.write(fd, memoryview(secret)[offset:])
                    if count <= 0:
                        raise OSError("plaintext lease write made no progress")
                    offset += count
            except BaseException:
                self.zeroize(lease_id)
                raise

    def zeroize(self, lease_id: str) -> None:
        with self._lock:
            item = self._leases.get(lease_id)
            if item is None:
                return
            item.secret[:] = b"\x00" * len(item.secret)
            try:
                if item.fd is not None:
                    size = os.fstat(item.fd).st_size
                    os.lseek(item.fd, 0, os.SEEK_SET)
                    zeros = bytes(min(64 * 1024, size))
                    while size:
                        count = os.write(item.fd, zeros[:min(len(zeros), size)])
                        if count <= 0:
                            raise OSError("plaintext zeroization made no progress")
                        size -= count
                    os.fsync(item.fd)
                    try:
                        current = item.path.lstat()
                    except FileNotFoundError:
                        current = None
                    original = os.fstat(item.fd)
                    if current is not None:
                        if (current.st_dev, current.st_ino) != (
                                original.st_dev, original.st_ino):
                            raise OSError("plaintext lease path was replaced")
                        item.path.unlink()
                    os.close(item.fd)
                    item.fd = None
            except (OSError, ValueError) as exc:
                raise RuntimeError("plaintext zeroization failed") from exc
            del self._leases[lease_id]

    def zeroize_all(self) -> None:
        with self._lock:
            errors = []
            for lease_id in tuple(self._leases):
                try:
                    self.zeroize(lease_id)
                except RuntimeError as exc:
                    errors.append(exc)
            if errors:
                raise RuntimeError(
                    f"plaintext zeroization failed for {len(errors)} lease(s)") from errors[0]

    def __enter__(self) -> "PlaintextLeaseRegistry":
        return self

    def __contains__(self, lease_id: str) -> bool:
        with self._lock:
            return lease_id in self._leases

    def __exit__(self, *_exc) -> None:
        self.zeroize_all()


__all__ = [
    "ProtectionState",
    "GrantRequestV1",
    "KeyGrantV1",
    "RecipientEnvelopeV1",
    "PlaintextLeaseRegistry",
    "AssembledCiphertextV1",
    "canonical_digest",
    "assembled_kdf_context",
    "derive_assembled_bundle_key",
    "derive_assembled_entry_key",
    "encrypt_assembled_entry",
    "decrypt_assembled_entry",
    "wrap_content_key",
    "unwrap_content_key",
    "verify_and_unwrap_grant",
    "grant_to_wire",
    "grant_from_wire",
    "ProtectedGrantRejected",
]
