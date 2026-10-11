#!/usr/bin/env python3
"""Fetch an immutable artifact through the native In-App Repo API."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from py_repoclient import ArtifactReference, ArtifactRepositoryApi


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--store-dir", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()

    reference = ArtifactReference.from_json(
        args.reference.read_text(encoding="utf-8")
    )
    api = ArtifactRepositoryApi.make_in_app(
        durable_root=args.store_dir,
        owner_id=reference.publisher_identity,
    )
    api.fetch_file(reference, args.destination)

    summary = {
        "reference": reference.to_json(),
        "destination": str(args.destination),
        "sha256": hashlib.sha256(args.destination.read_bytes()).hexdigest(),
    }
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
