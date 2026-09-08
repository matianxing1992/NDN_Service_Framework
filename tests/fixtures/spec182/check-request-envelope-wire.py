#!/usr/bin/env python3
"""Offline SDK decoder and independently recomputed request-identity oracle."""
import base64
import hashlib
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core.contracts import (
    DIRequestEnvelopeV2, canonical_digest)

rows = Path(sys.argv[1]).read_bytes().splitlines()
assert len(rows) == 2
transports = set()
for wire in rows:
    request = DIRequestEnvelopeV2.from_bytes(wire)
    assert request.to_bytes() == wire
    transports.add(request.input_transport)
    model = request.model
    identity = canonical_digest({
        "model_name": model["name"], "content_digest": model["content_digest"],
        "semantics_digest": model["semantics_digest"], "source_revision": model["source_revision"]})
    assert request.model_identity_hash == identity
    assert request.invocation_id == "invocation:" + canonical_digest({
        "request_id": request.request_id, "model": identity})[7:39]
    payload = base64.b64decode(request.input_payload_b64, validate=True)
    options = base64.b64decode(request.options_payload_b64, validate=True)
    logical = ("sha256:" + hashlib.sha256(payload).hexdigest()
               if request.input_transport == "INLINE" else canonical_digest(request.input_reference))
    assert request.input_manifest_digest == canonical_digest({
        "input_schema_digest": "sha256:" + hashlib.sha256(b"fixture-input-schema").hexdigest(),
        "options_schema_digest": "sha256:" + hashlib.sha256(b"fixture-options-schema").hexdigest(),
        "input_transport": request.input_transport, "input_digest": logical,
        "input_reference": request.input_reference, "options_digest": hashlib.sha256(options).hexdigest()})
    assert request.task["placement_profile"] == "DI_PLACEMENT_V3"
assert transports == {"INLINE", "REPO_REF"}
print("SPEC182_REQUEST_ENVELOPE_SDK_ORACLE_PASS records=2")
