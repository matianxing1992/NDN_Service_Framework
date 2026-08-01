#!/usr/bin/env python3
"""Minimal public-API artifact retriever for local development."""

from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path

from py_repoclient import (
    ArtifactReference,
    ArtifactRepositoryApi,
    FilesystemArtifactApiBackend,
)


def _load_reference(path: Path) -> ArtifactReference:
    value = json.loads(path.read_text(encoding="utf-8"))
    return ArtifactReference(
        logical_name=value["logicalName"],
        content_digest=value["contentDigest"],
        size_bytes=int(value["sizeBytes"]),
        root_manifest_name=value["rootManifestName"],
        publisher_identity=value["publisherIdentity"],
        policy_epoch=value["policyEpoch"],
        digest_algorithm=value.get("digestAlgorithm", "sha256"),
        format_version=value.get("formatVersion", "artifact-manifest-v2"),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--store-dir", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--result-out", type=Path)
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("--no-resume", action="store_true")
    parser.add_argument("--async", dest="use_async", action="store_true")
    parser.add_argument("--advanced", action="store_true")
    args = parser.parse_args()

    reference = _load_reference(args.reference)
    repo = ArtifactRepositoryApi(
        FilesystemArtifactApiBackend(args.store_dir),
        publisher_identity=reference.publisher_identity,
    )
    progress_count = [0]

    def on_progress(_event) -> None:
        progress_count[0] += 1

    options = {
        "resume": not args.no_resume,
        "replace": args.replace,
        "on_progress": on_progress,
    }
    if args.advanced:
        session = repo.begin_fetch(
            reference, args.destination, **options
        )
        session.transfer()
        result = session.commit()
    elif args.use_async:
        result = asyncio.run(repo.fetch_file_async(
            reference, args.destination, **options
        ))
    else:
        result = repo.fetch_file(
            reference, args.destination, **options
        )

    summary = {
        "schema": "ndnsf-repo-public-artifact-fetch-v1",
        "operationId": result.operation_id,
        "artifact": result.reference.to_dict(),
        "destination": str(result.destination),
        "reusedBytes": result.reused_bytes,
        "transferredBytes": result.transferred_bytes,
        "sourceReplicas": list(result.source_replicas),
        "progressEvents": progress_count[0],
    }
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
