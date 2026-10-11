#!/usr/bin/env python3
"""Publish an immutable artifact through the native In-App Repo API."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from py_repoclient import ArtifactReference, ArtifactRepositoryApi


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--store-dir", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--publisher", default="/example/artifact/publisher")
    parser.add_argument("--policy-epoch", default="example-epoch-1")
    parser.add_argument("--reference-out", type=Path, required=True)
    args = parser.parse_args()

    digest = hashlib.sha256(args.source.read_bytes()).hexdigest()
    size = args.source.stat().st_size
    reference = ArtifactReference(
        args.name,
        digest,
        size,
        f"{args.publisher.rstrip('/')}/NDNSF/ARTIFACT/{digest}/root",
        args.publisher,
        args.policy_epoch,
    )

    api = ArtifactRepositoryApi.make_in_app(
        durable_root=args.store_dir,
        owner_id=args.publisher,
    )
    result = api.publish_file(args.source, reference)
    args.reference_out.parent.mkdir(parents=True, exist_ok=True)
    args.reference_out.write_text(result.reference.to_json() + "\n", encoding="utf-8")

    summary = {
        "operationId": result.operation_id,
        "reference": result.reference.to_json(),
        "receipt": result.receipt.to_json() if result.receipt else None,
        "handle": result.handle.to_json() if result.handle else None,
        "achievedReplicas": result.achieved_replicas,
        "deduplicated": result.deduplicated,
    }
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
