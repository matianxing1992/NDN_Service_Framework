#!/usr/bin/env python3
"""Store and fetch non-AI objects through an NDNSF-DistributedRepo cluster."""

from __future__ import annotations

import argparse
import hashlib
import json

from ndnsf import ServiceUser, make_segmented_data_packets
from ndnsf_distributed_inference import APPDeployment, NetworkDistributedRepoClient


CONFIG_FILE = "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_policy.yaml"
REPO_SERVICE = "/NDNSF/DistributedRepo"


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
        ("/APP/Payment/Config/v3", "json-config", config, 2),
        ("/APP/UAV/TelemetryLog/window-0007", "telemetry-log", log, 2),
        ("/APP/Generic/BinaryBlob/demo-192KiB", "binary-blob", blob, 3),
    ]


def build_app_signed_object(data_prefix: str, user_name: str) -> tuple[str, str, bytes, list]:
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
        "/APP/Payment/AppSigned/RiskMetadata",
        "app-signed-json",
        payload,
        packets,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-distributed-repo-generic-policy")
    parser.add_argument("--ack-timeout-ms", type=int, default=2500)
    parser.add_argument("--timeout-ms", type=int, default=60000)
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
    manifests = []
    for object_name, object_type, payload, replicas in build_objects():
        print(f"store object={object_name} type={object_type} size={len(payload)} "
              f"replicas={replicas}", flush=True)
        manifest = repo.store_object(
            object_name=object_name,
            payload=payload,
            object_type=object_type,
            replication_factor=replicas,
            policy_epoch="/Policy/generic-repo/v1",
        )
        print(f"stored object={object_name} manifest={manifest.to_dict()}", flush=True)
        fetched = repo.fetch_object(object_name, manifest)
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
        f"{deployment.user}/APP-SIGNED-DATA",
        deployment.user,
    )
    print(f"store pre-signed object={object_name} type={object_type} "
          f"segments={len(packets)} replicas=3", flush=True)
    signed_manifest = repo.store_signed_packets(
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
    fetched = repo.fetch_object(object_name, signed_manifest)
    print(f"fetched pre-signed object={object_name} size={len(fetched)}",
          flush=True)
    if fetched != payload:
        raise RuntimeError(f"repo fetch mismatch for pre-signed {object_name}")
    manifests.append(signed_manifest)
    print(f"verified pre-signed object={object_name} "
          f"replicas={signed_manifest.replica_nodes}", flush=True)

    print("GENERIC_DISTRIBUTED_REPO_OK")
    print("first_capability:", capability)
    for manifest in manifests:
        print("manifest:", manifest.to_dict())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
