#!/usr/bin/env python3
"""Public artifact API smoke with real NDNSF Collaboration control."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from ndnsf import ServiceUser
from py_repoclient import (
    ArtifactRepositoryApi,
    CollaborationArtifactApiBackend,
)


SERVICE = "/NDNSF/DistributedRepo/Artifact/v2/STORE"
PROVIDER = "/example/hello/provider/A"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--destination", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = (
        args.source.resolve() if args.source
        else args.output / "source.bin"
    )
    destination = (
        args.destination.resolve() if args.destination
        else args.output / "destination.bin"
    )
    if args.source:
        payload = source.read_bytes()
    else:
        payload = b"spec164-secure-collaboration-artifact" * 1024
        source.write_bytes(payload)

    service_user = ServiceUser()
    control_backend = CollaborationArtifactApiBackend(
        None,
        service_user,
        SERVICE,
        ack_timeout_ms=3000,
    )
    api = ArtifactRepositoryApi(
        control_backend,
        publisher_identity="/example/hello/user",
        default_timeout_ms=15_000,
    )
    published = api.publish_file(
        source,
        name="/example/hello/user/artifacts/spec164-control",
        expected_sha256=hashlib.sha256(payload).hexdigest(),
        replicas=1,
        idempotency_key="spec164-control-smoke",
    )
    fetched = api.fetch_file(
        published.reference,
        destination,
        idempotency_key="spec164-control-fetch",
    )
    if destination.read_bytes() != payload:
        raise RuntimeError("public artifact fetch differs from source")
    metrics = control_backend.last_control_metrics
    summary = {
        "schema": "ndnsf-repo-spec164-real-network-artifact-v1",
        "verdict": "PASS",
        "operationId": published.operation_id,
        "selectedRepoNodes": [
            item.repo_node for item in published.replicas
        ],
        "controlOperationCount": metrics.control_operation_count,
        "lifecyclePhaseCount": metrics.lifecycle_phase_count,
        "responseCount": metrics.response_count,
        "controlElapsedMs": metrics.elapsed_ms,
        "payloadBytes": len(payload),
        "fetchedBytes": fetched.transferred_bytes,
        "destinationDigest": hashlib.sha256(
            destination.read_bytes()
        ).hexdigest(),
        "receipts": list(control_backend.last_receipts),
    }
    (args.output / "summary.json").write_text(
        json.dumps(summary, sort_keys=True, indent=2) + "\n"
    )
    print("SPEC164_SECURE_ARTIFACT_CONTROL_OK", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
