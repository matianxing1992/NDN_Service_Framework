#!/usr/bin/env python3
"""Real queued whole-artifact RepoNode provider for Spec 164."""

from __future__ import annotations

import argparse
from pathlib import Path

from py_repoclient.orchestration import RepoNodeApp


PROVIDER = "/example/hello/provider/A"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--storage-dir", type=Path, required=True)
    args = parser.parse_args()
    repo = RepoNodeApp(
        repo_node=PROVIDER,
        provider_id="A",
        group="/example/hello/group",
        controller="/example/hello/controller",
        provider_prefix="/example/hello/provider",
        storage_dir=args.storage_dir,
        free_bytes=4 * 1024 * 1024 * 1024,
        artifact_format_versions=(
            "artifact-manifest-v2", "exact-packet-v1",
        ),
        artifact_supports_resume=True,
        artifact_supports_replica_receipts=True,
    )
    print("SPEC164_ARTIFACT_CONTROL_PROVIDER_READY", flush=True)
    return repo.run()


if __name__ == "__main__":
    raise SystemExit(main())
