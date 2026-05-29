#!/usr/bin/env python3
"""Store and fetch non-AI objects through an NDNSF-DistributedRepo cluster."""

from __future__ import annotations

import argparse
import hashlib
import json

from ndnsf import make_segmented_data_packets
from ndnsf_distributed_inference import APPDeployment, DistributedRepo


CONFIG_FILE = "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_policy.yaml"
REPO_SERVICE = "/NDNSF/DistributedRepo"
CONFIG_OBJECT = (
    "/example/repo/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/CONFIG/repo_policy.yaml"
)


def deterministic_blob(size: int) -> bytes:
    seed = b"ndnsf-distributed-repo-generic-object-store"
    output = bytearray()
    counter = 0
    while len(output) < size:
        output.extend(hashlib.sha256(seed + counter.to_bytes(8, "big")).digest())
        counter += 1
    return bytes(output[:size])


def build_objects() -> list[tuple[str, str, bytes, int]]:
    config = json.dumps({
        "workflow": "payment-risk-evaluation",
        "version": 3,
        "thresholds": {"review": 0.72, "reject": 0.91},
    }, sort_keys=True).encode()
    log = "\n".join(
        f"2026-05-28T12:{minute:02d}:00Z sensor={minute % 4} value={minute * 17}"
        for minute in range(240)
    ).encode()
    blob = deterministic_blob(192 * 1024)
    return [
        ("APP/Payment/Config/v3", "json-config", config, 2),
        ("APP/UAV/TelemetryLog/window-0007", "telemetry-log", log, 2),
        ("APP/Generic/BinaryBlob/demo-192KiB", "binary-blob", blob, 3),
    ]


def build_app_signed_object(
    repo: DistributedRepo,
    data_prefix: str,
    user_name: str,
) -> tuple[str, str, bytes, list]:
    payload = json.dumps({
        "source": "app-owned-segmenter",
        "note": "segments are signed before they are submitted to the repo",
        "values": [index * index for index in range(64)],
    }, sort_keys=True).encode()
    object_hash = hashlib.sha256(payload).hexdigest()
    data_name = f"{data_prefix}/APP-SIGNED/payment-risk-metadata/{object_hash}"
    packets = make_segmented_data_packets(
        data_name,
        payload,
        signing_identity=user_name,
        max_segment_size=512,
        freshness_ms=60000,
    )
    return (
        repo.object_name("APP/Payment/AppSigned/RiskMetadata"),
        "app-signed-json",
        payload,
        packets,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-distributed-repo-generic-policy")
    parser.add_argument("--use-local-config", action="store_true",
                        help="Load --config directly instead of fetching it through NDNSF")
    parser.add_argument("--controller", default="/example/repo/controller")
    parser.add_argument("--user", default="/example/repo/user")
    parser.add_argument("--group", default="/example/repo/group")
    parser.add_argument("--trust-schema", default="examples/trust-schema.conf")
    parser.add_argument("--config-object", default=CONFIG_OBJECT,
                        help="Repo object name that stores the deployment config")
    parser.add_argument("--ack-timeout-ms", type=int, default=2500)
    parser.add_argument("--timeout-ms", type=int, default=60000)
    args = parser.parse_args()

    if args.use_local_config:
        deployment = APPDeployment.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
        ).deployment
        repo = DistributedRepo.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
            service_name=REPO_SERVICE,
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=args.timeout_ms,
        )
        user_name = deployment.user
    else:
        repo = DistributedRepo.from_repo_config(
            controller=args.controller,
            user=args.user,
            group=args.group,
            trust_schema=args.trust_schema,
            config_object_name=args.config_object,
            generated_policy_dir=args.generated_policy_dir,
            service_name=REPO_SERVICE,
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=args.timeout_ms,
        )
        user_name = args.user

    capability = repo.wait_until_ready(15.0)
    manifests = []
    for object_suffix, object_type, payload, replicas in build_objects():
        object_name = repo.object_name(object_suffix)
        print(f"store object={object_name} type={object_type} size={len(payload)} "
              f"replicas={replicas}", flush=True)
        manifest = repo.put(
            object_suffix,
            payload,
            object_type=object_type,
            replication_factor=replicas,
            policy_epoch="/Policy/generic-repo/v1",
        )
        print(f"stored object={object_name} manifest={manifest.to_dict()}", flush=True)
        fetched = repo.get(object_name, manifest)
        print(f"fetched object={object_name} size={len(fetched)}", flush=True)
        if fetched != payload:
            raise RuntimeError(f"repo fetch mismatch for {object_name}")
        if len(manifest.replica_nodes) != replicas:
            raise RuntimeError(
                f"unexpected replica count for {object_name}: "
                f"{len(manifest.replica_nodes)} != {replicas}")
        manifests.append(manifest)
        print(f"verified object={object_name} replicas={manifest.replica_nodes}",
              flush=True)

    object_name, object_type, payload, packets = build_app_signed_object(
        repo,
        f"{repo.publisher_namespace}/APP-SIGNED-DATA",
        user_name,
    )
    print(f"store pre-signed object={object_name} type={object_type} "
          f"segments={len(packets)} replicas=3", flush=True)
    signed_manifest = repo._client.store_signed_packets(
        object_name=object_name,
        packets=packets,
        object_type=object_type,
        object_size=len(payload),
        object_sha256=hashlib.sha256(payload).hexdigest(),
        replication_factor=3,
        policy_epoch="/Policy/generic-repo/v1",
    )
    print(f"stored pre-signed object={object_name} "
          f"manifest={signed_manifest.to_dict()}", flush=True)
    fetched = repo.get(object_name, signed_manifest)
    print(f"fetched pre-signed object={object_name} size={len(fetched)}",
          flush=True)
    if fetched != payload:
        raise RuntimeError(f"repo fetch mismatch for pre-signed {object_name}")
    manifests.append(signed_manifest)
    print(f"verified pre-signed object={object_name} "
          f"replicas={signed_manifest.replica_nodes}", flush=True)

    inventory = repo.list()
    if not all(manifest.object_name in inventory for manifest in manifests[:3]):
        raise RuntimeError("repo inventory missing stored objects")
    removed = repo.remove(manifests[0].object_name)
    if not removed:
        raise RuntimeError("repo remove reported no deletion")

    print("GENERIC_DISTRIBUTED_REPO_OK")
    print("first_capability:", capability)
    for manifest in manifests:
        print("manifest:", manifest.to_dict())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
