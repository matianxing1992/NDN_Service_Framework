#!/usr/bin/env python3
"""Offline SDK oracle for request wires emitted by the native stream fixture."""
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "NDNSF-DistributedInference"))
from ndnsf_distributed_inference.core.contracts import DIRequestEnvelopeV2, GenerationRecoveryV1

rows = Path(sys.argv[1]).read_bytes().splitlines()
assert len(rows) == 2
prefixes = set()
for wire in rows:
    request = DIRequestEnvelopeV2.from_bytes(wire)
    assert request.to_bytes() == wire
    recovery = GenerationRecoveryV1.from_dict(request.task["generation_recovery"])
    assert recovery.to_dict() == request.task["generation_recovery"]
    assert request.attempt == recovery.attempt == 2
    assert request.request_id == recovery.recovery_request_id
    assert recovery.original_request_id != request.request_id
    assert recovery.original_input_manifest_digest == request.input_manifest_digest
    assert recovery.failed_provider == "/provider/a"
    prefix = tuple(recovery.committed_token_ids)
    assert recovery.committed_prefix_digest == "sha256:" + hashlib.sha256(
        ",".join(map(str, prefix)).encode("ascii")).hexdigest()
    prefixes.add(prefix)
assert prefixes == {(), (1,)}
print("SPEC182_STREAM_RECOVERY_SDK_ORACLE_PASS records=2")
