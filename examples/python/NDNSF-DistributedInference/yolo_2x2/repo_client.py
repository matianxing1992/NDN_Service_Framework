#!/usr/bin/env python3
"""Store and fetch real YOLO 2x2 artifacts through a shared-service repo cluster."""

from __future__ import annotations

import argparse
from pathlib import Path

from ndnsf import ServiceUser
from ndnsf_distributed_inference import APPDeployment, NetworkDistributedRepoClient


CONFIG_FILE = "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"
REPO_SERVICE = "/NDNSF/DistributedRepo"


def split_payload(payload: bytes, count: int) -> list[bytes]:
    step = (len(payload) + count - 1) // count
    return [payload[index:index + step] for index in range(0, len(payload), step)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-yolo-2x2-policy")
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--replication-factor", type=int, default=2)
    parser.add_argument("--max-bytes", type=int, default=0,
                        help="Optional smoke-test limit; 0 stores the full file")
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    args = parser.parse_args()

    deployment = APPDeployment.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    ).deployment
    user = ServiceUser(
        group=deployment.group,
        controller=deployment.controller,
        user=deployment.user,
        trust_schema=deployment.trust_schema,
        permission_wait_ms=6000,
        adaptive_admission=False,
    )
    repo = NetworkDistributedRepoClient(
        user=user,
        service_name=REPO_SERVICE,
        upload_prefix=f"{deployment.user}/NDNSF-DISTRIBUTED-REPO/UPLOAD",
        ack_timeout_ms=args.ack_timeout_ms,
        timeout_ms=args.timeout_ms,
    )
    capability = repo.wait_until_ready(15.0)
    model_path = Path(args.model)
    payload = model_path.read_bytes()
    if args.max_bytes > 0:
        payload = payload[:args.max_bytes]
    chunks = split_payload(payload, 4)
    manifests = []
    for index, chunk in enumerate(chunks):
        print(f"store chunk {index} size={len(chunk)}", flush=True)
        manifest = repo.store_object(
            object_name=repo.publisher_object_name(
                f"NDNSF-DI/ARTIFACT/AI/YOLO/2x2/real-model/chunk/{index}"
            ),
            payload=chunk,
            object_type="model-shard",
            replication_factor=args.replication_factor,
            policy_epoch="/Policy/yolo-2x2/v1",
        )
        print(f"fetch chunk {index}", flush=True)
        fetched = repo.fetch_object(manifest.object_name, manifest)
        if fetched != chunk:
            raise RuntimeError(f"repo fetch mismatch for {manifest.object_name}")
        manifests.append(manifest)
        print(f"verified chunk {index}", flush=True)

    print("YOLO_2X2_REAL_REPO_OK")
    print("first_capability:", capability)
    for manifest in manifests:
        print("manifest:", manifest.to_dict())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
