#!/usr/bin/env python3
"""Minimal public-API artifact publisher for local development."""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
from pathlib import Path

from py_repoclient import (
    ArtifactDescriptor,
    ArtifactReference,
    ArtifactRepositoryApi,
    FilesystemArtifactApiBackend,
)


def _result_dict(result, progress_count: int) -> dict:
    return {
        "schema": "ndnsf-repo-public-artifact-publish-v1",
        "operationId": result.operation_id,
        "artifact": result.reference.to_dict(),
        "requestedReplicas": result.requested_replicas,
        "achievedReplicas": result.achieved_replicas,
        "receiptIds": [item.receipt_id for item in result.replicas],
        "deduplicated": result.deduplicated,
        "resumed": result.resumed,
        "progressEvents": progress_count,
    }


def _file_sha256(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            size += len(block)
            digest.update(block)
    return size, digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--store-dir", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--publisher", default="/example/artifact/publisher")
    parser.add_argument("--policy-epoch", default="example-epoch-1")
    parser.add_argument("--idempotency-key", default="")
    parser.add_argument("--reference-out", type=Path, required=True)
    parser.add_argument("--result-out", type=Path)
    parser.add_argument("--async", dest="use_async", action="store_true")
    parser.add_argument("--advanced", action="store_true")
    args = parser.parse_args()

    backend = FilesystemArtifactApiBackend(args.store_dir)
    repo = ArtifactRepositoryApi(
        backend, publisher_identity=args.publisher
    )
    progress_count = [0]

    def on_progress(_event) -> None:
        progress_count[0] += 1

    if args.advanced:
        size, digest = _file_sha256(args.source)
        if digest != args.expected_sha256.lower():
            raise ValueError("source does not match --expected-sha256")
        reference = ArtifactReference(
            logical_name=args.name,
            content_digest=digest,
            size_bytes=size,
            root_manifest_name=f"{args.name.rstrip('/')}/root",
            publisher_identity=args.publisher,
            policy_epoch=args.policy_epoch,
        )
        descriptor = ArtifactDescriptor(
            reference,
            requested_replicas=1,
            idempotency_key=(
                args.idempotency_key or f"example-publish:{digest}"
            ),
        )
        session = repo.begin_upload(
            descriptor, on_progress=on_progress
        )
        session.upload_file(args.source)
        result = session.commit()
    elif args.use_async:
        result = asyncio.run(repo.publish_file_async(
            args.source,
            name=args.name,
            expected_sha256=args.expected_sha256,
            replicas=1,
            policy_epoch=args.policy_epoch,
            idempotency_key=args.idempotency_key,
            on_progress=on_progress,
        ))
    else:
        result = repo.publish_file(
            args.source,
            name=args.name,
            expected_sha256=args.expected_sha256,
            replicas=1,
            policy_epoch=args.policy_epoch,
            idempotency_key=args.idempotency_key,
            on_progress=on_progress,
        )

    args.reference_out.parent.mkdir(parents=True, exist_ok=True)
    args.reference_out.write_text(
        json.dumps(result.reference.to_dict(), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    summary = _result_dict(result, progress_count[0])
    if args.result_out:
        args.result_out.parent.mkdir(parents=True, exist_ok=True)
        args.result_out.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
