#!/usr/bin/env python3
"""Spec180: preserve Trust-Schema ACK provenance through the Python facade."""

from __future__ import annotations

import pytest
from types import SimpleNamespace
import time
from pathlib import Path

from ndnsf.service import AckCandidate, _from_native_ack_candidate
from ndnsf_distributed_inference.app_sdk.client import APPClient
from ndnsf_distributed_inference.app_sdk.placement import (
    _validate_v3_ack_offer_provenance,
    v3_provider_view_factory,
)
from ndnsf_distributed_inference.app_sdk.runtime_journal import RuntimeJournal
from ndnsf_distributed_inference.sdk.placement import (  # noqa: E402
    DeviceTopologyProfile,
    ExecutionDisposition,
    ProviderOfferV3,
)


MODEL = "sha256:" + "1" * 64
GRAPH = "sha256:" + "2" * 64


def _native_candidate(**overrides):
    values = {
        "provider_name": "/provider/A",
        "service_name": "/Inference/YOLO",
        "request_id": "req-1",
        "status": True,
        "message": "ready",
        "payload": b"offer",
        "telemetry": None,
        "selection_input_key_offer": {},
        "signer_identity": "/provider/A",
        "signer_key_locator": "/provider/A/KEY/k1",
        "validated_wire_digest": "sha256:" + "a" * 64,
        "trust_schema_validated": True,
    }
    values.update(overrides)
    return SimpleNamespace(**values)


def test_native_ack_provenance_is_preserved_without_reconstruction():
    candidate = _from_native_ack_candidate(_native_candidate())

    assert isinstance(candidate, AckCandidate)
    assert candidate.signer_identity == "/provider/A"
    assert candidate.signer_key_locator == "/provider/A/KEY/k1"
    assert candidate.validated_wire_digest == "sha256:" + "a" * 64
    assert candidate.trust_schema_validated is True


def test_ack_subscription_retains_validated_data_packet():
    """The production ACK subscription must request packet-backed callbacks."""
    source = (Path(__file__).resolve().parents[2]
              / "ndn-service-framework" / "ServiceUser.cpp").read_text(
                  encoding="utf-8")
    marker = 'std::string regex_str = "^(<>*)<NDNSF><ACK>(<>*)$";'
    start = source.index(marker)
    end = source.index('std::string regex_str2 =', start)
    block = source[start:end]
    assert "std::bind(&ServiceUser::OnRequestAck, this, _1)" in block
    assert "true, true);" in block


def test_old_native_fixture_defaults_missing_provenance_to_empty():
    candidate = _from_native_ack_candidate(_native_candidate())
    legacy = SimpleNamespace(
        provider_name=candidate.provider_name,
        service_name=candidate.service_name,
        request_id=candidate.request_id,
        status=candidate.status,
        message=candidate.message,
        payload=candidate.payload,
        telemetry=None,
        selection_input_key_offer={},
    )

    projected = _from_native_ack_candidate(legacy)
    assert projected.signer_identity == ""
    assert projected.signer_key_locator == ""
    assert projected.validated_wire_digest == ""
    assert projected.trust_schema_validated is False


def test_v3_offer_gate_requires_packet_identity_binding():
    offer = SimpleNamespace(
        provider="/provider/A",
        service="/Inference/YOLO",
        request_id="req-1",
    )
    ack = _native_candidate()
    _validate_v3_ack_offer_provenance(ack, offer)

    ack.signer_identity = "/provider/B"
    try:
        _validate_v3_ack_offer_provenance(ack, offer)
    except ValueError as exc:
        assert "does not match Provider" in str(exc)
    else:  # pragma: no cover - assertion keeps the fail-closed contract clear
        raise AssertionError("mismatched ACK identity was accepted")


def test_v3_offer_gate_rejects_missing_or_malformed_provenance():
    offer = SimpleNamespace(
        provider="/provider/A",
        service="/Inference/YOLO",
        request_id="req-1",
    )
    for digest in ("", "sha256:not-a-digest"):
        ack = _native_candidate(validated_wire_digest=digest)
        try:
            _validate_v3_ack_offer_provenance(ack, offer)
        except ValueError as exc:
            assert "missing or malformed" in str(exc)
        else:  # pragma: no cover
            raise AssertionError("invalid ACK provenance was accepted")


def test_v3_configuration_rejects_caller_ack_coverage(tmp_path):
    journal = RuntimeJournal.for_test(tmp_path, "requester")
    network = SimpleNamespace(service_user=object())
    strategy = SimpleNamespace(placement_profile="DI_PLACEMENT_V3")
    adapter = SimpleNamespace(descriptor=SimpleNamespace(name="fixture"))
    client = APPClient(journal, network_client=network)
    with pytest.raises(ValueError, match="does not permit caller ACK coverage"):
        client.configure_automatic_planning(
            service_name="/ObjectDetection/YOLO26n",
            adapters=[adapter],
            strategy=strategy,
            catalog_snapshot_provider=lambda: (),
            verify_offer_signature=lambda _offer: True,
            ack_coverage_roles=("BackboneNeck",),
        )
    with pytest.raises(ValueError, match="does not permit caller ACK coverage"):
        client.configure_automatic_planning(
            service_name="/ObjectDetection/YOLO26n",
            adapters=[adapter],
            strategy=strategy,
            catalog_snapshot_provider=lambda: (),
            verify_offer_signature=lambda _offer: True,
            ack_coverage_predicate=lambda _candidates: True,
        )


def test_v3_factory_defaults_to_strict_ack_provenance():
    now_ms = int(time.time() * 1000)
    offer = ProviderOfferV3(
        request_id="req-1",
        attempt=1,
        service="/Inference/YOLO",
        provider="/provider/A",
        model_digest=MODEL,
        graph_digest=GRAPH,
        status=True,
        execution_disposition=ExecutionDisposition.ACCEPT_WITH_PREPARATION,
        preparation_accepted=True,
        topology=DeviceTopologyProfile("/provider/A", (), "cpu"),
        accepted_roles=("FullModel",),
        backends=("onnxruntime-cpu",),
        boot_epoch="boot-1",
        captured_at_ms=now_ms - 1,
        expires_at_ms=now_ms + 60000,
        signer_key_id="ack-key",
        signature="ack-signature",
    )
    ack = SimpleNamespace(
        status=True,
        payload=offer.to_bytes(),
        provider_name="/provider/A",
        service_name="/Inference/YOLO",
        request_id="req-1",
    )
    factory = v3_provider_view_factory(lambda _offer: True)
    with pytest.raises(ValueError, match="authenticated provenance"):
        factory(ack, MODEL, now_ms + 1000, GRAPH)
