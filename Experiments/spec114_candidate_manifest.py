#!/usr/bin/env python3
"""Create and inspect an immutable Spec 114 NDN-SVS V3 candidate."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCHEMA = "spec114-svs-v3-candidate-v2"
CELLS = tuple(
    f"loss{loss}-run{run:02d}"
    for loss in ("00", "05")
    for run in range(1, 4)
)


class CandidateError(RuntimeError):
    pass


def _run(args: list[str], cwd: Path) -> str:
    completed = subprocess.run(
        args, cwd=cwd, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False,
    )
    if completed.returncode != 0:
        raise CandidateError(
            f"command failed: {' '.join(args)}: {completed.stderr.strip()}"
        )
    return completed.stdout.strip()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def _identity(ndn_svs: Path, ndnts_lock: Path, campaign_attempt: str) -> dict[str, Any]:
    branch = _run(["git", "branch", "--show-current"], ndn_svs)
    if branch != "Experimental":
        raise CandidateError(f"NDN-SVS must be on Experimental, got {branch!r}")
    status = _run(["git", "status", "--porcelain=v1", "--untracked-files=all"], ndn_svs)
    if status:
        raise CandidateError("NDN-SVS worktree must be clean before candidate freeze")
    if not ndnts_lock.is_file():
        raise CandidateError(f"NDNts lockfile is missing: {ndnts_lock}")

    source_tree = _run(["git", "rev-parse", "HEAD^{tree}"], ndn_svs)
    repo = Path(__file__).resolve().parents[1]
    interop_root = repo / "examples/interop/ndn-svs-v3"
    campaign_inputs = [
        repo / "Experiments/spec114_candidate_manifest.py",
        repo / "Experiments/NDN_SVS_V3_Interop_Minindn.py",
        interop_root / "build-cpp-peer.sh",
        interop_root / "run-standalone.sh",
        interop_root / "cpp/svs3-peer.cpp",
        interop_root / "ndnts/package.json",
        interop_root / "ndnts/svs3-peer.ts",
    ]
    if not campaign_attempt or not all(ch.isalnum() or ch in "._-" for ch in campaign_attempt):
        raise CandidateError("campaign attempt must use only letters, digits, '.', '_' or '-'")
    return {
        "schemaVersion": SCHEMA,
        "ndnSvs": {
            "path": str(ndn_svs.resolve()),
            "branch": branch,
            "head": _run(["git", "rev-parse", "HEAD"], ndn_svs),
            "tree": source_tree,
            "status": "clean",
            "base": "c34c04d766836bba1567a70bae846dfbd9d25b66",
        },
        "ndntsLock": {
            "path": str(ndnts_lock.resolve()),
            "sha256": _sha256(ndnts_lock),
        },
        "interop": {
            "owner": "NDNSF",
            "root": str(interop_root.resolve()),
            "cppSource": str((interop_root / "cpp/svs3-peer.cpp").resolve()),
            "cppPeer": str((interop_root / "build/svs3-peer").resolve()),
            "ndntsSource": str((interop_root / "ndnts/svs3-peer.ts").resolve()),
            "ndntsSourceLanguage": "TypeScript",
        },
        "campaignAttempt": campaign_attempt,
        "campaignInputs": [
            {"path": str(path.resolve()), "sha256": _sha256(path)}
            for path in campaign_inputs
        ],
        "protocol": {
            "version": 3,
            "syncInterestLifetimeMs": 1000,
            "periodicTimeoutMs": 30000,
            "periodicJitter": 0.1,
            "suppressionPeriodMs": 200,
            "compression": "disabled",
            "extensionProfile": "mapping-repair-trailing-v1",
        },
        "workload": {
            "publishCountPerPeer": 20,
            "convergenceTimeoutSeconds": 60,
            "cells": list(CELLS),
        },
    }


def create(ndn_svs: Path, ndnts_lock: Path, output_root: Path,
           campaign_attempt: str) -> str:
    identity = _identity(ndn_svs.resolve(), ndnts_lock.resolve(), campaign_attempt)
    identity_digest = hashlib.sha256(_canonical(identity)).hexdigest()
    candidate_id = f"spec114-{identity_digest[:20]}"
    candidate_dir = output_root.resolve() / candidate_id
    manifest_path = candidate_dir / "candidate-manifest.json"
    if candidate_dir.exists():
        raise CandidateError(f"candidate already exists; refusing overwrite: {candidate_dir}")

    manifest = {
        "candidateId": candidate_id,
        "identitySha256": identity_digest,
        "identity": identity,
        "createdAt": datetime.now(timezone.utc).isoformat(),
        "runOnce": {cell: "pending" for cell in CELLS},
    }
    output_root.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{candidate_id}-", dir=output_root))
    try:
        (temporary / "candidate-manifest.json").write_bytes(
            json.dumps(manifest, indent=2, sort_keys=True).encode() + b"\n"
        )
        os.rename(temporary, candidate_dir)
    except Exception:
        if temporary.exists():
            temporary.rmdir()
        raise
    if not manifest_path.is_file():
        raise CandidateError("atomic candidate creation failed")
    return candidate_id


def inspect(manifest_path: Path) -> dict[str, Any]:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    identity = manifest.get("identity")
    digest = hashlib.sha256(_canonical(identity)).hexdigest()
    if digest != manifest.get("identitySha256"):
        raise CandidateError("candidate identity digest mismatch")
    expected = f"spec114-{digest[:20]}"
    if manifest.get("candidateId") != expected:
        raise CandidateError("candidate identifier mismatch")
    if tuple(identity["workload"]["cells"]) != CELLS:
        raise CandidateError("formal cell declaration mismatch")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    create_parser = sub.add_parser("create")
    create_parser.add_argument("--ndn-svs", type=Path, required=True)
    create_parser.add_argument("--ndnts-lock", type=Path, required=True)
    create_parser.add_argument("--output-root", type=Path, required=True)
    create_parser.add_argument("--campaign-attempt", default="default")
    inspect_parser = sub.add_parser("inspect")
    inspect_parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "create":
            print(create(args.ndn_svs, args.ndnts_lock, args.output_root,
                         args.campaign_attempt))
        else:
            manifest = inspect(args.manifest)
            print(json.dumps({"candidateId": manifest["candidateId"], "valid": True}))
        return 0
    except (CandidateError, OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
