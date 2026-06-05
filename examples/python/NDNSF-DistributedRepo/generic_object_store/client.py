#!/usr/bin/env python3
"""Store and fetch non-AI objects through an NDNSF-DistributedRepo cluster."""

from __future__ import annotations

import argparse
import hashlib
import json
import time

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
    parser.add_argument("--test-delete", action="store_true",
                        help="Also exercise deletion after store/fetch/catalog checks")
    parser.add_argument("--catalog-health-smoke", action="store_true",
                        help="Only verify catalog stale detection and repair-plan generation")
    parser.add_argument("--catalog-snapshot-large-response-smoke", action="store_true",
                        help="Fetch a full catalog snapshot so NDNSF core can exercise large response references")
    parser.add_argument("--catalog-snapshot-repo-node",
                        default="/example/repo/provider/repoA")
    parser.add_argument("--catalog-health-repo-node", default="/example/repo/provider/repoA")
    parser.add_argument("--catalog-health-stale-repo", default="/example/repo/provider/repoC")
    parser.add_argument("--catalog-health-object-suffix",
                        default="APP/UAV/TelemetryLog/window-0007")
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

    if args.catalog_health_smoke:
        object_name = repo.object_name(args.catalog_health_object_suffix)
        lookup = repo.catalog_lookup(object_name, args.catalog_health_repo_node)
        stale_repos = set(str(value) for value in lookup.get("staleRepos", []))
        if args.catalog_health_stale_repo not in stale_repos:
            raise RuntimeError(
                f"catalog health did not mark stale repo={args.catalog_health_stale_repo} "
                f"lookup={lookup}"
            )
        if "repairPlan" not in lookup:
            raise RuntimeError(f"catalog health lookup missing repair plan: {lookup}")

        repair_object = repo.object_name("APP/CatalogHealth/RepairPlanSynthetic")
        now_ms = int(time.time() * 1000)
        repair_entry = {
            "objectName": repair_object,
            "objectSha256": "synthetic-object-sha256",
            "manifestSha256": "synthetic-manifest-sha256",
            "objectType": "catalog-health-synthetic",
            "size": 128,
            "segmentCount": 1,
            "sourceRepo": "/example/repo/provider/repoB",
            "repoMode": "persistent",
            "state": "AVAILABLE",
            "catalogEpoch": now_ms,
            "lastSeenMs": now_ms,
            "updatedAtMs": now_ms,
            "minReplicationFactor": 2,
            "maxReplicationFactor": 2,
            "desiredReplicationFactor": 2,
            "replicaNodes": ["/example/repo/provider/repoB"],
            "manifest": {
                "objectName": repair_object,
                "objectType": "catalog-health-synthetic",
                "sha256": "synthetic-object-sha256",
                "size": 128,
                "segmentCount": 1,
                "minReplicationFactor": 2,
                "maxReplicationFactor": 2,
                "replicationFactor": 2,
                "replicaNodes": ["/example/repo/provider/repoB"],
            },
        }
        repo.catalog_merge(
            args.catalog_health_repo_node,
            [repair_entry],
            {
                "repoNode": "/example/repo/provider/repoD",
                "repoMode": "persistent",
                "catalogEpoch": now_ms,
                "acceptsBackupReplica": True,
            },
        )
        repair_lookup = repo.catalog_lookup(
            repair_object, args.catalog_health_repo_node)
        repair_plan = repair_lookup.get("repairPlan", {})
        if not isinstance(repair_plan, dict) or not repair_plan.get("needed"):
            raise RuntimeError(
                f"catalog health synthetic object missing repair plan: {repair_lookup}"
            )
        actions = repair_plan.get("actions", [])
        if not isinstance(actions, list) or not actions:
            raise RuntimeError(
                f"catalog health synthetic object missing repair actions: {repair_lookup}"
            )
        first_action = actions[0]
        if not first_action.get("sourceRepo") or not first_action.get("targetRepo"):
            raise RuntimeError(
                f"catalog health repair action is incomplete: {repair_lookup}"
            )
        if first_action.get("sourceRepo") != "/example/repo/provider/repoB":
            raise RuntimeError(f"catalog health repair source is wrong: {repair_lookup}")
        if first_action.get("targetRepo") == first_action.get("sourceRepo"):
            raise RuntimeError(f"catalog health repair target equals source: {repair_lookup}")
        print("GENERIC_DISTRIBUTED_REPO_CATALOG_HEALTH_OK", flush=True)
        return 0

    if args.catalog_snapshot_large_response_smoke:
        snapshot, raw_payload = repo.catalog_snapshot_with_payload(
            args.catalog_snapshot_repo_node)
        entries = snapshot.get("entries", [])
        objects = snapshot.get("objects", [])
        if not isinstance(entries, list) or not entries:
            raise RuntimeError(f"catalog snapshot missing entries: {snapshot}")
        if not isinstance(objects, list) or not objects:
            raise RuntimeError(f"catalog snapshot missing object summaries: {snapshot}")
        if not snapshot.get("repoStatus"):
            raise RuntimeError(f"catalog snapshot missing repo status: {snapshot}")
        if len(raw_payload) < 10 * 1024:
            raise RuntimeError(
                "catalog snapshot response did not exercise 10KB+ Core payload "
                f"path: bytes={len(raw_payload)}"
            )
        print(
            "GENERIC_DISTRIBUTED_REPO_CATALOG_SNAPSHOT_LARGE_RESPONSE_OK "
            f"repo={args.catalog_snapshot_repo_node} "
            f"entries={len(entries)} objects={len(objects)} "
            f"payloadBytes={len(raw_payload)}",
            flush=True,
        )
        print(
            "GENERIC_DISTRIBUTED_REPO_CORE_10KB_RESPONSE_CALLBACK_OK "
            f"payloadBytes={len(raw_payload)}",
            flush=True,
        )
        return 0

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
    repo_nodes = (
        "/example/repo/provider/repoA",
        "/example/repo/provider/repoB",
        "/example/repo/provider/repoC",
    )
    time.sleep(15.0)
    catalog_manifest = manifests[-2]
    for repo_node in repo_nodes:
        lookup = repo.catalog_lookup(catalog_manifest.object_name, repo_node)
        if str(lookup.get("objectSha256", "")) != catalog_manifest.sha256:
            raise RuntimeError(
                f"catalog object hash mismatch repo={repo_node} "
                f"object={catalog_manifest.object_name}"
            )
        if not lookup.get("manifestSha256"):
            raise RuntimeError(
                f"catalog manifest hash missing repo={repo_node} "
                f"object={catalog_manifest.object_name}"
            )
        min_replicas = int(lookup.get("minReplicationFactor", 1) or 1)
        max_replicas = int(
            lookup.get("maxReplicationFactor", catalog_manifest.replication_factor) or
            catalog_manifest.replication_factor
        )
        if min_replicas < 1:
            raise RuntimeError(
                f"catalog min replication too small repo={repo_node} "
                f"object={catalog_manifest.object_name}"
            )
        if max_replicas < min_replicas:
            raise RuntimeError(
                f"catalog max replication below min repo={repo_node} "
                f"object={catalog_manifest.object_name} lookup={lookup}"
            )
        if int(lookup.get("availableReplicaCount", 0)) < min_replicas:
            raise RuntimeError(
                f"catalog below minimum replicas unexpectedly repo={repo_node} "
                f"object={catalog_manifest.object_name} lookup={lookup}"
            )
        if lookup.get("underReplicated"):
            raise RuntimeError(
                f"catalog repair hint unexpectedly active repo={repo_node} "
                f"object={catalog_manifest.object_name} lookup={lookup}"
            )
        if not lookup.get("candidateReplicas"):
            raise RuntimeError(
                f"catalog lookup has no candidate replicas repo={repo_node} "
                f"object={catalog_manifest.object_name}"
            )
        if "repairPlan" not in lookup:
            raise RuntimeError(
                f"catalog lookup missing repair plan repo={repo_node} "
                f"object={catalog_manifest.object_name}"
            )
    print("GENERIC_DISTRIBUTED_REPO_CATALOG_GOSSIP_OK", flush=True)
    if args.test_delete:
        deleted_manifest = manifests[0]
        removed = repo.remove(deleted_manifest.object_name)
        if not removed:
            raise RuntimeError("repo remove reported no deletion")
        lookup_repo = (
            deleted_manifest.replica_nodes[0]
            if deleted_manifest.replica_nodes else
            args.catalog_snapshot_repo_node
        )
        lookup = repo.catalog_lookup(deleted_manifest.object_name, lookup_repo)
        states = {str(entry.get("state", "")) for entry in lookup.get("entries", [])}
        if lookup.get("state") != "DELETED" and "DELETED" not in states:
            raise RuntimeError(
                "repo delete did not leave a catalog tombstone: "
                f"repo={lookup_repo} lookup={lookup}"
            )
        print(
            "GENERIC_DISTRIBUTED_REPO_CATALOG_TOMBSTONE_OK "
            f"repo={lookup_repo} object={deleted_manifest.object_name}",
            flush=True,
        )

    print("GENERIC_DISTRIBUTED_REPO_OK")
    print("first_capability:", capability)
    for manifest in manifests:
        print("manifest:", manifest.to_dict())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
