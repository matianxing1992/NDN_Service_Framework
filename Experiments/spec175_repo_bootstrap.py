#!/usr/bin/env python3
"""Publish or fetch the frozen Spec175 tiny-ONNX bundle through DistributedRepo."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import time

from py_repoclient import (
    ArtifactRepositoryApi,
    CollaborationArtifactApiBackend,
    artifact_reference_from_dict,
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def digest_hex(value: object) -> str:
    text = str(value)
    return text[7:] if text.startswith("sha256:") else text


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("publish", "fetch"))
    parser.add_argument("--config", required=True)
    parser.add_argument("--generated-policy-dir", required=True)
    parser.add_argument("--state-root", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--bootstrap-token-file", required=True)
    parser.add_argument("--registration", required=True)
    parser.add_argument("--stage-manifest")
    parser.add_argument("--object-prefix", default="/NDNSF/Spec175/TinyOnnx")
    parser.add_argument("--role")
    parser.add_argument("--destination")
    parser.add_argument("--timeout-ms", type=int, default=60_000)
    parser.add_argument(
        "--ack-timeout-ms", type=int, default=5_000,
        help=("ACK collection deadline for the repository control-plane "
              "collaboration; distinct from the streamed invocation deadline"),
    )
    parser.add_argument(
        "--test-only-allow-ephemeral-app-state", action="store_true",
        help=("Allow the named volatile state root only for an explicit "
              "real-MiniNDN test; production use must provide persistent state"),
    )
    return parser


def make_backend(args: argparse.Namespace, *, receipts=()):
    token = Path(args.bootstrap_token_file).read_text(encoding="utf-8").strip()
    if not token:
        raise RuntimeError("Spec175 Repo bootstrap token is empty")
    ack_timeout_ms = int(getattr(args, "ack_timeout_ms", 5_000))
    if ack_timeout_ms <= 0:
        raise ValueError("Spec175 Repo ACK timeout must be positive")
    return CollaborationArtifactApiBackend.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        state_root=args.state_root,
        user=args.user,
        bootstrap_token=token,
        committed_receipts=tuple(dict(item) for item in receipts),
        ack_timeout_ms=ack_timeout_ms,
        packet_payload_bytes=7600,
        chunk_bytes=1024 * 1024,
        test_only_allow_ephemeral_state_root=bool(
            getattr(args, "test_only_allow_ephemeral_app_state", False)),
    )


def receipts_for_publish_result(backend, result) -> list[dict]:
    """Return only receipts committed by one publish operation."""
    committed_ids = {
        str(replica.receipt_id)
        for replica in result.replicas
        if str(replica.state) == "COMMITTED" and str(replica.receipt_id)
    }
    return [
        dict(value) for value in backend.last_receipts
        if str(dict(value.get("receipt", {})).get("receiptId", ""))
        in committed_ids
    ]


def publish(args: argparse.Namespace) -> int:
    if not args.stage_manifest:
        raise RuntimeError("publish requires --stage-manifest")
    registration_path = Path(args.registration)
    if registration_path.exists():
        raise FileExistsError(registration_path)
    manifest_path = Path(args.stage_manifest)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    stages = list(manifest.get("stages", ()))
    if len(stages) != 4:
        raise RuntimeError("Spec175 Repo bootstrap requires exactly four stages")
    backend = make_backend(args)
    api = ArtifactRepositoryApi(
        backend, publisher_identity=args.user,
        default_timeout_ms=args.timeout_ms)
    artifacts = []
    for stage in stages:
        path = Path(str(stage["path"]))
        digest = digest_hex(stage["sha256"])
        if not path.is_file() or sha256_file(path) != digest:
            raise RuntimeError(f"Spec175 Repo source digest mismatch: {path}")
        object_name = (
            f"{args.object_prefix.rstrip('/')}/stage-{int(stage['stageIndex'])}-{digest}")
        started = time.perf_counter()
        result = api.publish_file(
            path,
            name=object_name,
            expected_sha256=digest,
            replicas=1,
            policy_epoch=str(manifest["modelDigest"]),
            idempotency_key=(
                f"spec175:{manifest['modelDigest']}:{stage['stageIndex']}:{digest}"),
            timeout_ms=args.timeout_ms,
        )
        receipts = receipts_for_publish_result(backend, result)
        if result.achieved_replicas != 1 or len(receipts) != 1:
            raise RuntimeError("Spec175 Repo publication lacks one committed receipt")
        artifacts.append({
            "role": str(stage["role"]),
            "stageIndex": int(stage["stageIndex"]),
            "fileSha256": "sha256:" + digest,
            "fileBytes": path.stat().st_size,
            "objectName": str(receipts[0].get("dataName", "")),
            "artifactReference": result.reference.to_dict(),
            "operationId": result.operation_id,
            "receipts": receipts,
            "publishMs": (time.perf_counter() - started) * 1000.0,
        })
        if not artifacts[-1]["objectName"]:
            raise RuntimeError("Spec175 Repo receipt omits committed Data name")
        print(
            "NDNSF_DI_SPEC175_REPO_STAGE_PUBLISHED",
            f"role={stage['role']}", f"sha256={digest}", flush=True)
    record = {
        "schema": "ndnsf-di-spec175-repo-registration-v1",
        "modelDigest": manifest["modelDigest"],
        "revision": manifest["revision"],
        "publisher": args.user,
        "artifactCount": len(artifacts),
        "artifacts": artifacts,
    }
    registration_path.parent.mkdir(parents=True, exist_ok=True)
    registration_path.write_text(
        json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    # The native ServiceUser owns an IO thread and pybind-managed Face.  Stop
    # it explicitly before interpreter teardown; relying on Python's object
    # finalizer can double-release native state after a completed publication.
    backend.control.service_user.stop()
    print(
        "NDNSF_DI_SPEC175_REPO_PUBLISH_PASS",
        f"registration={registration_path}", flush=True)
    return 0


def fetch(args: argparse.Namespace) -> int:
    if not args.role or not args.destination:
        raise RuntimeError("fetch requires --role and --destination")
    registration = json.loads(
        Path(args.registration).read_text(encoding="utf-8"))
    if registration.get("schema") != "ndnsf-di-spec175-repo-registration-v1":
        raise RuntimeError("unsupported Spec175 Repo registration")
    matches = [
        dict(item) for item in registration.get("artifacts", ())
        if str(item.get("role", "")) == args.role
    ]
    if len(matches) != 1:
        raise RuntimeError(f"Spec175 Repo role registration is not unique: {args.role}")
    item = matches[0]
    receipts = tuple(
        dict(receipt)
        for artifact in registration.get("artifacts", ())
        for receipt in artifact.get("receipts", ())
    )
    backend = make_backend(args, receipts=receipts)
    api = ArtifactRepositoryApi(
        backend, publisher_identity=args.user,
        default_timeout_ms=args.timeout_ms)
    destination = Path(args.destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    reference = artifact_reference_from_dict(dict(item["artifactReference"]))
    result = api.fetch_file(
        reference, destination, replace=False, timeout_ms=args.timeout_ms)
    expected = digest_hex(item["fileSha256"])
    if (not destination.is_file()
            or destination.stat().st_size != int(item["fileBytes"])
            or sha256_file(destination) != expected):
        raise RuntimeError("Spec175 Repo fetch did not reproduce exact artifact")
    backend.control.service_user.stop()
    print(
        "NDNSF_DI_SPEC175_REPO_FETCH_PASS",
        f"role={args.role}", f"destination={destination}",
        f"sha256={expected}", flush=True)
    return 0


def main() -> int:
    args = build_parser().parse_args()
    return publish(args) if args.mode == "publish" else fetch(args)


if __name__ == "__main__":
    raise SystemExit(main())
