"""Thin C05 Python wire codec parity; native C++ owns binding/authentication."""
from pathlib import Path
import json
import os
import sys

import pytest
from ndn.encoding import Name

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.naming import make_invocation_event_name

DIGEST = "aa8c0e2fc57574d2ad7e6b70c25ba8c6b5f563bf7b7c0f87620a02654f3e438d"


def _varnum(value):
    return bytes([value]) if value < 253 else b"\xfd" + value.to_bytes(2, "big")


def _tlv(typ, value):
    return _varnum(typ) + _varnum(len(value)) + value


def test_independent_c05_wire_transcript():
    fields = ("provider", "final", "NDNSF", "event", "REQUEST",
              "name=/8=request/8=id", "ATTEMPT", "1", "SERVICE", "name=/8=LLM/8=Qwen")
    expected = _tlv(7, b"".join(_tlv(8, field.encode()) for field in fields)
                    + _tlv(8, bytes.fromhex(DIGEST)) + _tlv(58, b"\x09"))
    actual = make_invocation_event_name("/provider/final", "/request/id", "/LLM/Qwen", 1, DIGEST, 9)
    assert bytes(Name.encode(Name.from_str(actual))) == expected


@pytest.mark.skipif(not os.environ.get("SPEC201_EVENT_RECORDS"), reason="native records not supplied")
def test_actual_native_event_wire_parity():
    records = json.loads(Path(os.environ["SPEC201_EVENT_RECORDS"]).read_text())
    assert len(records) == 4
    for record in records:
        name = make_invocation_event_name(record["producer"], record["request"], record["service"],
                                          record["attempt"], record["digest"], record["cursor"])
        assert bytes(Name.encode(Name.from_str(name))).hex() == record["wire"]
    # Same long readable request label must still carry different native commitments.
    assert records[2]["wire"] != records[3]["wire"]


@pytest.mark.parametrize("field,value", [("cursor", 0), ("cursor", 2**64), ("cursor", True),
    ("attempt", 0), ("attempt", True), ("digest", "A" * 64), ("producer", "/"),
    ("request", "/"), ("service", "/")])
def test_invalid_event_codec_input(field, value):
    args = dict(producer="/provider/final", request_id="/request/id", service="/LLM/Qwen",
                attempt=1, binding_digest=DIGEST, cursor=257)
    args[{"request": "request_id", "digest": "binding_digest"}.get(field, field)] = value
    with pytest.raises(ValueError):
        make_invocation_event_name(**args)
