from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.runtime_v1_evidence import (  # noqa: E402
    SPEC175_STREAM_EVIDENCE_SCHEMA,
    Spec175StreamEvidenceRecorder,
    validate_spec175_stream_evidence,
)


REQUEST = "/user/1/request/spec175"
GENERATION = "generation-1"
EXPECTED = (
    "REQUEST_CREATED", "ACK_CLOSED", "PLAN_COMMITTED", "SELECTION_ACCEPTED",
    "PREPARATION", "PREFILL", "DECODE_EPOCH",
    "INTERNAL_FEEDBACK_PUBLISHED", "INTERNAL_FEEDBACK_FETCHED",
    "EXTERNAL_EVENT_PUBLISHED", "EXTERNAL_EVENT_FETCHED",
    "EXTERNAL_EVENT_DELIVERED", "END", "RESPONSE",
)


def record(sequence: int, event_type: str, **extra: object) -> dict[str, object]:
    value: dict[str, object] = {
        "schema": SPEC175_STREAM_EVIDENCE_SCHEMA,
        "requestId": REQUEST,
        "generationId": GENERATION,
        "attemptEpoch": 1,
        "sequence": sequence,
        "eventType": event_type,
        "timestampUs": sequence * 100,
    }
    if event_type in {
        "PREPARATION", "PREFILL", "DECODE_EPOCH",
        "INTERNAL_FEEDBACK_PUBLISHED", "INTERNAL_FEEDBACK_FETCHED",
        "EXTERNAL_EVENT_PUBLISHED", "EXTERNAL_EVENT_FETCHED",
        "EXTERNAL_EVENT_DELIVERED",
    }:
        value.update({"providerIdentity": "/provider/0", "roleName": "/LLM/Pipeline/Stage/0"})
    value.update(extra)
    return value


def trace() -> list[dict[str, object]]:
    return [record(index, event_type) for index, event_type in enumerate(EXPECTED, 1)]


def swap_decode_and_prefill(rows: list[dict[str, object]]) -> None:
    rows[5], rows[6] = rows[6], rows[5]


def test_valid_trace_reports_bounded_lineage_without_plaintext() -> None:
    summary = validate_spec175_stream_evidence(
        trace(), request_id=REQUEST, expected_event_types=EXPECTED)
    assert summary["eventCount"] == len(EXPECTED)
    assert summary["decodeEpochCount"] == 1
    assert summary["startTimestampUs"] == 100


@pytest.mark.parametrize("mutation,code", [
    (lambda rows: rows.pop(5), "SEQUENCE_MISMATCH"),
    (lambda rows: rows.insert(5, record(6, "RETRY")), "SEQUENCE_MISMATCH"),
    (swap_decode_and_prefill, "SEQUENCE_MISMATCH"),
])
def test_missing_extra_or_reordered_span_is_rejected(mutation, code: str) -> None:
    rows = trace()
    mutation(rows)
    with pytest.raises(ValueError, match="SPEC175_EVIDENCE_EVENT_SEQUENCE_MISMATCH|SPEC175_EVIDENCE_SEQUENCE_INVALID"):
        validate_spec175_stream_evidence(
            rows, request_id=REQUEST, expected_event_types=EXPECTED)


def test_wrong_provider_attribution_and_plaintext_are_rejected() -> None:
    rows = trace()
    rows[9]["providerIdentity"] = ""
    with pytest.raises(ValueError, match="ATTRIBUTION_MISSING"):
        validate_spec175_stream_evidence(rows, request_id=REQUEST, expected_event_types=EXPECTED)
    rows = trace()
    rows[0]["prompt"] = "secret prompt"
    with pytest.raises(ValueError, match="FORBIDDEN_FIELD"):
        validate_spec175_stream_evidence(rows, request_id=REQUEST, expected_event_types=EXPECTED)


def test_bounded_recorder_writes_metadata_only_jsonl(tmp_path: Path) -> None:
    output = tmp_path / "events.jsonl"
    recorder = Spec175StreamEvidenceRecorder(
        REQUEST, GENERATION, output_path=output, max_records=len(EXPECTED))
    for event_type in EXPECTED:
        kwargs = {}
        if event_type in {
            "PREPARATION", "PREFILL", "DECODE_EPOCH",
            "INTERNAL_FEEDBACK_PUBLISHED", "INTERNAL_FEEDBACK_FETCHED",
            "EXTERNAL_EVENT_PUBLISHED", "EXTERNAL_EVENT_FETCHED",
            "EXTERNAL_EVENT_DELIVERED",
        }:
            kwargs = {"provider_identity": "/provider/0", "role_name": "/LLM/Pipeline/Stage/0"}
        recorder.append(event_type, timestamp_us=len(recorder.records) + 1, **kwargs)
    summary = recorder.close(expected_event_types=EXPECTED)
    assert summary["eventCount"] == len(EXPECTED)
    assert output.read_text(encoding="utf-8").count("\n") == len(EXPECTED)
    assert "prompt" not in output.read_text(encoding="utf-8")
    with pytest.raises(RuntimeError, match="ALREADY_CLOSED"):
        recorder.append("RESPONSE", timestamp_us=99)
