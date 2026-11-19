#!/usr/bin/env python3
"""Publish exact Qwen3.6 stage files as bounded DistributedRepo bundles."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import time

from py_repoclient import (
    ArtifactRepositoryApi,
    CollaborationArtifactApiBackend,
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(16 * 1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--stage-manifest", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--object-prefix", required=True)
    parser.add_argument("--generated-policy-dir", required=True)
    parser.add_argument("--state-root", required=True)
    parser.add_argument("--bootstrap-token-file")
    parser.add_argument("--chunk-mib", type=int, default=16)
    parser.add_argument("--replication-factor", type=int, default=1)
    parser.add_argument("--timeout-ms", type=int, default=600000)
    parser.add_argument(
        "--test-only-allow-ephemeral-app-state", action="store_true",
        help="Allow the named volatile client state root only for a test run.")
    parser.add_argument(
        "--control-mode",
        choices=("normal", "targeted"),
        default="normal",
    )
    args = parser.parse_args()
    if args.chunk_mib < 1 or args.chunk_mib > 64:
        raise RuntimeError("--chunk-mib must be between 1 and 64")
    if args.replication_factor < 1:
        raise RuntimeError("--replication-factor must be positive")

    stage_manifest_path = Path(args.stage_manifest)
    stage_manifest = json.loads(
        stage_manifest_path.read_text(encoding="utf-8"))
    stages = list(stage_manifest.get("stages", []))
    if len(stages) != 3:
        raise RuntimeError("Spec 162 registration requires exactly three stages")
    output_path = Path(args.output)
    if output_path.exists():
        raise FileExistsError(output_path)

    bootstrap_token = (
        Path(args.bootstrap_token_file).read_text(encoding="utf-8").strip()
        if args.bootstrap_token_file else ""
    )
    backend = CollaborationArtifactApiBackend.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        state_root=args.state_root,
        user=args.user,
        bootstrap_token=bootstrap_token,
        ack_timeout_ms=3000,
        packet_payload_bytes=7600,
        chunk_bytes=args.chunk_mib * 1024 * 1024,
        test_only_allow_ephemeral_state_root=(
            args.test_only_allow_ephemeral_app_state),
    )
    if args.control_mode != "normal":
        raise RuntimeError(
            "cold artifact publication requires normal Collaboration control")
    repo = ArtifactRepositoryApi(
        backend,
        publisher_identity=args.user,
        default_timeout_ms=args.timeout_ms,
    )
    readiness = {
        "service": "/NDNSF/DistributedRepo/Artifact/v2/STORE",
        "control": "begin_collaboration->ACK_CLOSED->commit_plan",
        "reservation": "none",
    }
    artifacts = []
    for stage in stages:
        role = str(stage["role"])
        path = Path(stage["path"])
        digest = str(stage["sha256"])
        if digest.startswith("sha256:"):
            digest = digest[7:]
        size = int(stage["bytes"])
        object_name = (
            f"{args.object_prefix.rstrip('/')}/"
            f"stage-{int(stage['stageIndex'])}-{digest}")
        started = time.perf_counter()
        result = repo.publish_file(
            path,
            name=object_name,
            expected_sha256=digest,
            replicas=args.replication_factor,
            policy_epoch=str(stage_manifest["modelDigest"]),
            idempotency_key=(
                f"qwen36:{stage_manifest['modelDigest']}:"
                f"{int(stage['stageIndex'])}:{digest}"
            ),
            timeout_ms=args.timeout_ms,
        )
        receipts = [dict(value) for value in backend.last_receipts]
        if len(receipts) != result.achieved_replicas:
            raise RuntimeError(
                "artifact collaboration receipt count does not match durability")
        reference = result.reference.to_dict()
        data_names = [str(item.get("dataName", "")) for item in receipts]
        if not data_names or any(not value for value in data_names):
            raise RuntimeError("artifact receipt is missing committed Data name")
        artifacts.append({
            "role": role,
            "stageIndex": int(stage["stageIndex"]),
            "fileSha256": "sha256:" + digest,
            "fileBytes": size,
            "objectName": data_names[0],
            "artifactReference": reference,
            "operationId": result.operation_id,
            "requestedReplicas": result.requested_replicas,
            "achievedReplicas": result.achieved_replicas,
            "receipts": receipts,
            "publishMs": (time.perf_counter() - started) * 1000.0,
        })
        print(
            "SPEC162_REPO_STAGE_REGISTERED",
            f"role={role}",
            f"objectName={data_names[0]}",
            f"bytes={size}",
            flush=True,
        )

    registration = {
        "schemaVersion": "ndnsf-di-qwen36-repo-registration-v1",
        "stageManifestSha256": "sha256:" + hashlib.sha256(
            stage_manifest_path.read_bytes()).hexdigest(),
        "modelDigest": stage_manifest["modelDigest"],
        "revision": stage_manifest["revision"],
        "publisher": args.user,
        "repositoryReadiness": readiness,
        "chunkMiB": args.chunk_mib,
        "controlMode": args.control_mode,
        "replicationFactor": args.replication_factor,
        "artifacts": artifacts,
        "completedAtUnixMs": int(time.time() * 1000),
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("x", encoding="utf-8") as output:
        json.dump(registration, output, indent=2, sort_keys=True)
        output.write("\n")
    print(
        "SPEC162_REPO_REGISTRATION_OK",
        f"output={output_path}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
