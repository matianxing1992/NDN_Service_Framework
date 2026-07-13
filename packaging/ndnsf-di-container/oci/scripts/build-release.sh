#!/bin/sh
set -eu

repo=$(cd -P -- "$(dirname -- "$0")/../../../.." && pwd)
exec python3 - "$repo" "$@" <<'PY'
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile


def digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def fail(code: str, detail: str = "", exit_code: int = 4) -> None:
    print(code + ((":" + detail) if detail else ""), file=sys.stderr)
    raise SystemExit(exit_code)


repo = Path(sys.argv[1])
parser = argparse.ArgumentParser()
parser.add_argument("--output", required=True)
parser.add_argument("--release-id", required=True)
parser.add_argument("--candidate-id", required=True)
parser.add_argument("--source-revision", required=True)
parser.add_argument("--created-at", required=True)
parser.add_argument("--image-reference", required=True)
parser.add_argument("--image-digest", required=True)
parser.add_argument("--sbom", required=True)
parser.add_argument("--provenance", required=True)
args = parser.parse_args(sys.argv[2:])

output = Path(args.output)
if output.exists():
    fail("OCI_RELEASE_OUTPUT_EXISTS", str(output), 2)
reference = re.fullmatch(r"\S+@sha256:([a-f0-9]{64})", args.image_reference)
image_digest = re.fullmatch(r"sha256:([a-f0-9]{64})", args.image_digest)
if reference is None or image_digest is None:
    fail("OCI_DIGEST_REQUIRED")
if reference.group(1) != image_digest.group(1):
    fail("OCI_DIGEST_MISMATCH")
if not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", args.created_at):
    fail("OCI_CREATED_AT_INVALID", exit_code=2)

sbom = Path(args.sbom)
provenance = Path(args.provenance)
for path, label in ((sbom, "SBOM"), (provenance, "PROVENANCE")):
    if not path.is_file():
        fail(f"OCI_{label}_MISSING", str(path), 2)
locks = [
    repo / "packaging/ndnsf-di-container/oci/locks/common.lock",
    repo / "packaging/ndnsf-di-container/oci/locks/cpu.lock",
]
for lock in locks:
    if not lock.is_file():
        fail("OCI_LOCK_MISSING", str(lock), 2)

manifest = {
    "schemaVersion": "1.0",
    "releaseId": args.release_id,
    "candidateId": args.candidate_id,
    "sourceRevision": args.source_revision,
    "createdAt": args.created_at,
    "images": {
        "linux-amd64-cpu": {
            "reference": args.image_reference,
            "digest": args.image_digest,
            "platform": "linux/amd64",
            "backend": "cpu",
        }
    },
    "buildInputs": [
        {"path": str(path.relative_to(repo)), "digest": digest(path)} for path in locks
    ],
    "sbom": {"location": "sbom.spdx.json", "digest": digest(sbom)},
    "provenance": {"builder": "ndnsf-di-container/build-release.sh", "digest": digest(provenance)},
    "compatibility": {"architecture": "amd64", "cuda": None, "onnxRuntime": "cpu"},
}
output.parent.mkdir(parents=True, exist_ok=True)
staging = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=str(output.parent)))
try:
    shutil.copyfile(sbom, staging / "sbom.spdx.json")
    shutil.copyfile(provenance, staging / "provenance.json")
    (staging / "release-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(staging, output)
except Exception:
    shutil.rmtree(staging, ignore_errors=True)
    raise
print(json.dumps({"status": "PASS", "releaseId": args.release_id,
                  "imageDigest": args.image_digest}, sort_keys=True))
PY
