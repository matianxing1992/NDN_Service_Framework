#!/usr/bin/env python3
"""Spec180: candidate-bound ProviderOfferV3 trust verification."""

from __future__ import annotations

import base64
import hashlib
from types import SimpleNamespace
import time

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from ndnsf_distributed_inference.app_sdk.provider import (
    ProviderOfferTrustVerifier,
)
from ndnsf_distributed_inference.app_sdk.placement import (
    v3_provider_view_factory,
)
from ndnsf_distributed_inference.sdk.placement import (
    DeviceTopologyProfile,
    ExecutionDisposition,
    ProviderOfferV3,
)


MODEL = "sha256:" + "1" * 64
GRAPH = "sha256:" + "2" * 64
WIRE = "sha256:" + "a" * 64
NOW = int(time.time() * 1000)


def _fixture():
    private = Ed25519PrivateKey.generate()
    public = private.public_key()
    raw = public.public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    key_id = "sha256:" + hashlib.sha256(raw).hexdigest()
    unsigned = ProviderOfferV3(
        request_id="req-1", attempt=1,
        service="/ObjectDetection/YOLO26n", provider="/provider/A",
        model_digest=MODEL, graph_digest=GRAPH, status=True,
        execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
        preparation_accepted=True,
        topology=DeviceTopologyProfile("/provider/A", (), "cpu"),
        accepted_roles=("FullModel",), backends=("onnxruntime-cpu",),
        boot_epoch="boot-1", captured_at_ms=NOW - 1_000,
        expires_at_ms=NOW + 60_000, signer_key_id=key_id,
        signature="placeholder",
    )
    signature = base64.b64encode(
        private.sign(unsigned.digest().encode("utf-8"))).decode("ascii")
    offer = ProviderOfferV3(**{
        **unsigned.__dict__, "signature": signature,
    })
    policy = {
        "schema": "spec180-provider-offer-trust-v1",
        "candidateId": "yolo-atomic-v1",
        "candidateDigest": "sha256:" + "c" * 64,
        "trustSchema": "/ndnsf/trust-schema",
        "entries": [{
            "provider": "/provider/A",
            "service": "/ObjectDetection/YOLO26n",
            "keyLocatorPrefix": "/provider/A/KEY/",
            "signerKeyId": key_id,
            "certificateName": "/provider/A/KEY/k1/self/v1",
        }],
    }
    ack = SimpleNamespace(
        status=True, provider_name="/provider/A",
        service_name="/ObjectDetection/YOLO26n", request_id="req-1",
        attempt=1, signer_identity="/provider/A",
        signer_key_locator="/provider/A/KEY/k1",
        validated_wire_digest=WIRE, payload=offer.to_bytes(),
        trust_schema_validated=True,
    )
    verifier = ProviderOfferTrustVerifier(
        policy, {key_id: public.public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo)},
        trust_schema_verifier=lambda _ack: True,
        clock_ms=lambda: NOW,
    )
    public_pem = public.public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo)
    return verifier, offer, ack, key_id, public_pem


def test_provider_offer_verifier_binds_policy_and_ack_provenance():
    verifier, offer, ack, _, _ = _fixture()
    assert verifier.verify_ack(
        offer, ack, model_digest=MODEL, graph_digest=GRAPH,
        request_id="req-1", deadline_ms=NOW + 30_000)


def test_v3_factory_invokes_ack_aware_production_verifier():
    verifier, offer, ack, _, _ = _fixture()
    view = v3_provider_view_factory(verifier)(
        ack, MODEL, NOW + 30_000, GRAPH)
    assert view.provider == offer.provider
    assert view.offer_digest == offer.digest()


@pytest.mark.parametrize("field,value", [
    ("provider_name", "/provider/B"),
    ("signer_key_locator", "/other/KEY/k1"),
    ("validated_wire_digest", "sha256:not-a-digest"),
])
def test_provider_offer_verifier_rejects_bad_ack_provenance(field, value):
    verifier, offer, ack, _, _ = _fixture()
    setattr(ack, field, value)
    with pytest.raises(ValueError):
        verifier.verify_ack(offer, ack, model_digest=MODEL, graph_digest=GRAPH)


def test_provider_offer_verifier_rejects_bad_signature():
    verifier, offer, ack, key_id, _ = _fixture()
    tampered = ProviderOfferV3(**{
        **offer.__dict__, "signer_key_id": key_id,
        "signature": base64.b64encode(b"bad").decode("ascii"),
    })
    with pytest.raises(ValueError, match="signature"):
        verifier.verify_ack(tampered, ack, model_digest=MODEL, graph_digest=GRAPH)


def test_provider_offer_verifier_requires_trust_schema_callback():
    _, offer, ack, key_id, public_pem = _fixture()
    no_trust = ProviderOfferTrustVerifier(
        {
            "schema": "spec180-provider-offer-trust-v1",
            "candidateId": "yolo-atomic-v1",
            "candidateDigest": "sha256:" + "c" * 64,
            "trustSchema": "/ndnsf/trust-schema",
            "entries": [{
                "provider": "/provider/A",
                "service": "/ObjectDetection/YOLO26n",
                "keyLocatorPrefix": "/provider/A/KEY/",
                "signerKeyId": key_id,
                "certificateName": "/provider/A/KEY/k1/self/v1",
            }],
        },
        {key_id: public_pem},
        trust_schema_verifier=lambda _ack: False,
        clock_ms=lambda: NOW,
    )
    with pytest.raises(ValueError, match="Trust Schema"):
        no_trust.verify_ack(offer, ack)


def test_provider_offer_verifier_rejects_unmarked_ack_even_if_callback_says_true():
    verifier, offer, ack, _, _ = _fixture()
    ack.trust_schema_validated = False
    with pytest.raises(ValueError, match="not marked Trust-Schema"):
        verifier.verify_ack(offer, ack, model_digest=MODEL, graph_digest=GRAPH)
