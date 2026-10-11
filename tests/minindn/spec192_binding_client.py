#!/usr/bin/env python3
"""Independent Python-binding peer for the Spec192 C++ Repo MiniNDN gate."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from ndnsf import ServiceUser
from py_repoclient import (
    ArtifactHandle,
    ArtifactReference,
    ArtifactRepositoryApi,
    PersistentRepoClientConfig,
    RepoWriteConsistency,
    SelectedRepoSet,
    SelectedRepoTarget,
)

PERSISTENT_ACK_TIMEOUT_MS = 30000
PERSISTENT_REQUEST_TIMEOUT_MS = 60000


def _read_handle(path: Path) -> ArtifactHandle:
    return ArtifactHandle.from_json(path.read_text(encoding="utf-8"))


def _make_selected(args: argparse.Namespace) -> SelectedRepoSet:
    selected = SelectedRepoSet()
    selected.seed_repo_id = args.repo_a
    selected.consistency = RepoWriteConsistency.ALL
    repo_a = SelectedRepoTarget()
    repo_a.repo_node = args.repo_a
    repo_a.provider_identity = args.provider_a
    repo_a.service_prefix = args.service_prefix
    repo_b = SelectedRepoTarget()
    repo_b.repo_node = args.repo_b
    repo_b.provider_identity = args.provider_b
    repo_b.service_prefix = args.service_prefix
    selected.targets = [repo_a, repo_b]
    return selected


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("fetch", "publish", "receipt"), required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--trust-schema", type=Path, required=True)
    parser.add_argument("--service-prefix", default="/NDNSF/DistributedRepo/Object/v1")
    parser.add_argument("--repo-a", required=True)
    parser.add_argument("--provider-a", required=True)
    parser.add_argument("--repo-b", required=True)
    parser.add_argument("--provider-b", required=True)
    parser.add_argument("--handle", type=Path)
    parser.add_argument("--receipt", type=Path)
    parser.add_argument("--handle-out", type=Path)
    parser.add_argument("--receipt-out", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--timeout-ms", type=int, default=300000)
    args = parser.parse_args()

    user = ServiceUser(
        group=args.group,
        controller=args.controller,
        user=args.user,
        trust_schema=str(args.trust_schema),
        permission_wait_ms=20000,
    )
    user.start()
    try:
        # Keep each provider request bounded so replica fallback can use the
        # separate whole-operation budget passed to fetch_file/publish_file.
        # Cold five-host MiniNDN STATUS discovery needs more than the default
        # ACK window, so allow 30 seconds for authenticated ACK closure and a
        # separate 60-second request response deadline.
        config = PersistentRepoClientConfig()
        config.ack_collection_time_ms = PERSISTENT_ACK_TIMEOUT_MS
        config.request_timeout_ms = PERSISTENT_REQUEST_TIMEOUT_MS
        config.operation_wait_timeout_ms = args.timeout_ms
        api = ArtifactRepositoryApi.make_persistent(user, config)
        if args.mode == "receipt":
            if args.handle is None or args.receipt is None:
                parser.error("receipt mode requires --handle and --receipt")
            handle = _read_handle(args.handle)
            if not handle.commit_receipt_ids:
                raise RuntimeError("Persistent handle carries no commit receipt")
            receipt = api.get_receipt(
                handle.commit_receipt_ids[0], handle,
                PERSISTENT_REQUEST_TIMEOUT_MS,
            )
            expected = json.loads(args.receipt.read_text(encoding="utf-8"))
            if json.loads(receipt.to_json()) != expected:
                raise RuntimeError("Python binding receipt differs from C++ receipt file")
            print(json.dumps({
                "mode": "receipt",
                "receipt_id": receipt.receipt_id,
                "receipt_json": receipt.to_json(),
            }, sort_keys=True))
            return 0

        if args.mode == "fetch":
            if args.handle is None:
                parser.error("--handle is required in fetch mode")
            if args.destination is None:
                parser.error("fetch mode requires --destination")
            handle = _read_handle(args.handle)
            fetch = api.fetch_file(handle, args.destination, args.timeout_ms)
            payload = args.destination.read_bytes()
            digest = hashlib.sha256(payload).hexdigest()
            if len(payload) != handle.reference.size_bytes or digest != handle.reference.content_digest:
                raise RuntimeError("Python binding fetch digest/size mismatch")
            receipt_ids = list(handle.commit_receipt_ids)
            receipts = []
            if args.receipt is not None:
                if not receipt_ids:
                    raise RuntimeError("Persistent handle carries no commit receipt")
                receipts = [api.get_receipt(
                    receipt_ids[0], handle, args.timeout_ms
                )]
                expected = json.loads(args.receipt.read_text(encoding="utf-8"))
                if receipts[0].to_json() != json.dumps(expected, sort_keys=True, separators=(",", ":")):
                    # The C++ serializer is canonical but intentionally does
                    # not promise key ordering in the JSON interchange file.
                    if json.loads(receipts[0].to_json()) != expected:
                        raise RuntimeError("Python binding receipt differs from C++ receipt file")
            print(json.dumps({
                "mode": "fetch",
                "digest": digest,
                "size": len(payload),
                "repo_node": fetch.repo_node,
                "provider_identity": fetch.provider_identity,
                "receipt_ids": receipt_ids,
                "receipts_verified": len(receipts),
                "receipt_json": [item.to_json() for item in receipts],
            }, sort_keys=True))
            return 0

        if (args.source is None or args.destination is None or
                args.handle_out is None or args.receipt_out is None):
            parser.error("publish mode requires --source, --destination, --handle-out and --receipt-out")
        payload = args.source.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        reference = ArtifactReference(
            f"/spec192/e2e/{args.source.name}",
            digest,
            len(payload),
            f"{args.user}/NDNSF/ARTIFACT/{digest}/root",
            args.user,
            "repo-policy-v1",
        )
        result = api.publish_file(
            args.source, reference, _make_selected(args), args.timeout_ms
        )
        if result.handle is None or result.receipt is None:
            raise RuntimeError("Python native publish returned no handle/receipt")
        args.handle_out.parent.mkdir(parents=True, exist_ok=True)
        args.receipt_out.parent.mkdir(parents=True, exist_ok=True)
        args.handle_out.write_text(result.handle.to_json() + "\n", encoding="utf-8")
        args.receipt_out.write_text(result.receipt.to_json() + "\n", encoding="utf-8")
        fetched_receipt = api.get_receipt(
            result.receipt.receipt_id, result.handle,
            PERSISTENT_REQUEST_TIMEOUT_MS,
        )
        if fetched_receipt.to_json() != result.receipt.to_json():
            raise RuntimeError("Python native status receipt differs from publish receipt")
        api.fetch_file(result.handle, args.destination, args.timeout_ms)
        fetched_payload = args.destination.read_bytes()
        if hashlib.sha256(fetched_payload).hexdigest() != digest:
            raise RuntimeError("Python native publish/fetch round-trip mismatch")
        print(json.dumps({
            "mode": "publish",
            "digest": digest,
            "size": len(fetched_payload),
            "receipt_id": result.receipt.receipt_id,
            "receipt_json": result.receipt.to_json(),
        }, sort_keys=True))
        return 0
    finally:
        user.stop()


if __name__ == "__main__":
    raise SystemExit(main())
