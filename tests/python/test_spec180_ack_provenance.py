#!/usr/bin/env python3
"""Spec180: preserve Trust-Schema ACK provenance through the Python facade."""

from __future__ import annotations

from types import SimpleNamespace
from pathlib import Path

from ndnsf.service import AckCandidate, _from_native_ack_candidate


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
    marker = ('std::string regex_str = "^(<>*)" + '
              'NameToRegexString(ndn::Name("/NDNSF/ACK")) + "(<>*)$";')
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
