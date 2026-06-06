#!/usr/bin/env python3
"""Store and fetch non-AI objects through an NDNSF-DistributedRepo cluster."""

from __future__ import annotations

import argparse
import hashlib
import json
import time

from ndnsf import make_segmented_data_packets
from ndnsf_distributed_inference import APPDeployment, DistributedRepo
from ndnsf_distributed_inference.repo import RepoObjectManifest


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
    parser.add_argument("--catalog-auto-repair-seed-smoke", action="store_true",
                        help="Seed an under-replicated object for sidecar auto-repair")
    parser.add_argument("--catalog-auto-repair-verify-object", default="",
                        help="Verify that sidecar auto-repair fixed this object")
    parser.add_argument("--catalog-auto-source-repo-node",
                        default="/example/repo/provider/repoB")
    parser.add_argument("--catalog-auto-target-repo-node",
                        default="/example/repo/provider/repoA")
    parser.add_argument("--catalog-tombstone-gossip-smoke", action="store_true",
                        help="Verify that tombstones gossip to peer repo catalogs")
    parser.add_argument("--catalog-tombstone-source-repo-node",
                        default="/example/repo/provider/repoA")
    parser.add_argument("--catalog-tombstone-peer-repo-node", action="append",
                        default=[])
    parser.add_argument("--catalog-tombstone-epoch-conflict-smoke", action="store_true",
                        help="Verify that stale AVAILABLE entries with higher catalog epochs cannot revive tombstoned objects")
    parser.add_argument("--object-policy-smoke", action="store_true",
                        help="Verify default object class and retention metadata")
    parser.add_argument("--uav-data-product-smoke", action="store_true",
                        help="Verify UAV recording/log store, catalog lookup, and fetch")
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

    if args.catalog_auto_repair_seed_smoke:
        payload = b"auto catalog repair object"
        suffix = str(int(time.time() * 1000))
        manifest = repo.put(
            f"APP/CatalogHealth/AutoRepairObject/{suffix}",
            payload,
            object_type="catalog-health-auto-repair",
            replication_factor=1,
            replica_nodes=(args.catalog_auto_source_repo_node,),
            policy_epoch="/Policy/generic-repo/v1",
        )
        if manifest.replica_nodes[0] != args.catalog_auto_source_repo_node:
            raise RuntimeError(f"auto repair seed stored on wrong repo: {manifest}")
        now_ms = int(time.time() * 1000)
        entry = {
            "objectName": manifest.object_name,
            "objectSha256": manifest.sha256,
            "manifestSha256": hashlib.sha256(manifest.to_bytes()).hexdigest(),
            "objectType": manifest.object_type,
            "size": manifest.size,
            "segmentCount": manifest.segment_count,
            "sourceRepo": args.catalog_auto_source_repo_node,
            "repoMode": "persistent",
            "state": "AVAILABLE",
            "catalogEpoch": now_ms,
            "lastSeenMs": now_ms,
            "updatedAtMs": now_ms,
            "minReplicationFactor": 2,
            "maxReplicationFactor": 2,
            "desiredReplicationFactor": 2,
            "replicaNodes": [args.catalog_auto_source_repo_node],
            "manifest": {
                **manifest.to_dict(),
                "minReplicationFactor": 2,
                "maxReplicationFactor": 2,
                "replicationFactor": 2,
                "replicaNodes": [args.catalog_auto_source_repo_node],
            },
        }
        repo.catalog_merge(
            args.catalog_auto_source_repo_node,
            [entry],
            {
                "repoNode": args.catalog_auto_source_repo_node,
                "repoMode": "persistent",
                "catalogEpoch": now_ms,
                "acceptsBackupReplica": True,
            },
        )
        print(
            "GENERIC_DISTRIBUTED_REPO_AUTO_REPAIR_SEED_OK "
            f"object={manifest.object_name} source={args.catalog_auto_source_repo_node} "
            f"target={args.catalog_auto_target_repo_node}",
            flush=True,
        )
        return 0

    if args.catalog_auto_repair_verify_object:
        object_name = args.catalog_auto_repair_verify_object
        source_status = repo.catalog_status(args.catalog_auto_source_repo_node)
        repo.catalog_merge(
            args.catalog_auto_target_repo_node,
            [],
            source_status,
        )
        lookup = repo.catalog_lookup(object_name, args.catalog_auto_target_repo_node)
        if lookup.get("underReplicated"):
            raise RuntimeError(f"auto repair object is still under-replicated: {lookup}")
        if int(lookup.get("availableReplicaCount", 0) or 0) < 2:
            raise RuntimeError(f"auto repair object has too few replicas: {lookup}")
        target_replicas = {
            str(candidate.get("sourceRepo", candidate.get("repoNode", "")))
            for candidate in lookup.get("candidateReplicas", [])
            if isinstance(candidate, dict)
        }
        if args.catalog_auto_target_repo_node not in target_replicas:
            raise RuntimeError(f"auto repair target repo missing from lookup: {lookup}")
        target_manifest = None
        for candidate in lookup.get("candidateReplicas", []):
            if not isinstance(candidate, dict):
                continue
            if str(candidate.get("sourceRepo", "")) != args.catalog_auto_target_repo_node:
                continue
            manifest_obj = candidate.get("manifest", {})
            if isinstance(manifest_obj, dict):
                target_manifest = RepoObjectManifest.from_dict(manifest_obj)
                break
        if target_manifest is None:
            raise RuntimeError(f"auto repair target manifest missing from lookup: {lookup}")
        if repo.get(object_name, target_manifest) != b"auto catalog repair object":
            raise RuntimeError("auto repair object payload mismatch")
        print(
            "GENERIC_DISTRIBUTED_REPO_AUTO_REPAIR_OK "
            f"object={object_name} target={args.catalog_auto_target_repo_node}",
            flush=True,
        )
        return 0

    if args.catalog_tombstone_gossip_smoke:
        source_repo = args.catalog_tombstone_source_repo_node
        peer_repos = (
            args.catalog_tombstone_peer_repo_node or
            ["/example/repo/provider/repoB", "/example/repo/provider/repoC"]
        )
        payload = b"tombstone gossip object"
        suffix = str(int(time.time() * 1000))
        manifest = repo.put(
            f"APP/CatalogHealth/TombstoneGossip/{suffix}",
            payload,
            object_type="mission-log",
            replication_factor=1,
            replica_nodes=(source_repo,),
            policy_epoch="/Policy/generic-repo/v1",
        )
        now_ms = int(time.time() * 1000)
        old_updated_ms = max(1, now_ms - 60000)
        stale_available_entry = {
            "objectName": manifest.object_name,
            "objectSha256": manifest.sha256,
            "manifestSha256": hashlib.sha256(manifest.to_bytes()).hexdigest(),
            "objectType": manifest.object_type,
            "objectClass": manifest.to_dict().get("objectClass", manifest.object_type),
            "size": manifest.size,
            "segmentCount": manifest.segment_count,
            "sourceRepo": source_repo,
            "repoMode": "persistent",
            "state": "AVAILABLE",
            "catalogEpoch": 1,
            "lastSeenMs": now_ms,
            "updatedAtMs": old_updated_ms,
            "minReplicationFactor": manifest.to_dict().get("minReplicationFactor", 1),
            "maxReplicationFactor": manifest.to_dict().get("maxReplicationFactor", 1),
            "desiredReplicationFactor": manifest.replication_factor,
            "repairAllowed": manifest.to_dict().get("repairAllowed", True),
            "replicaNodes": [source_repo],
            "manifest": manifest.to_dict(),
        }
        for peer_repo in peer_repos:
            repo.catalog_merge(
                peer_repo,
                [stale_available_entry],
                {
                    "repoNode": source_repo,
                    "repoMode": "persistent",
                    "catalogEpoch": 1,
                    "acceptsBackupReplica": True,
                },
            )
        if not repo.remove(manifest.object_name):
            raise RuntimeError("tombstone gossip remove reported no deletion")
        deadline = time.time() + 80.0
        final_lookups: dict[str, dict] = {}
        while time.time() < deadline:
            deleted_everywhere = True
            final_lookups.clear()
            for peer_repo in peer_repos:
                try:
                    lookup = repo.catalog_lookup(manifest.object_name, peer_repo)
                    final_lookups[peer_repo] = lookup
                    if lookup.get("state") != "DELETED":
                        deleted_everywhere = False
                        break
                    if int(lookup.get("availableReplicaCount", 0) or 0) != 0:
                        deleted_everywhere = False
                        break
                    unshadowed_available = [
                        entry for entry in lookup.get("entries", [])
                        if (str(entry.get("state", "")) == "AVAILABLE" and
                            not entry.get("shadowedByTombstone"))
                    ]
                    if unshadowed_available:
                        deleted_everywhere = False
                        break
                except Exception:
                    deleted_everywhere = False
                    break
            if deleted_everywhere:
                break
            time.sleep(2.0)
        else:
            raise RuntimeError(
                "tombstone did not gossip cleanly to peer catalogs: "
                f"object={manifest.object_name} lookups={final_lookups}"
            )
        print(
            "GENERIC_DISTRIBUTED_REPO_TOMBSTONE_GOSSIP_OK "
            f"object={manifest.object_name} peers={','.join(peer_repos)}",
            flush=True,
        )
        return 0

    if args.catalog_tombstone_epoch_conflict_smoke:
        source_repo = args.catalog_tombstone_source_repo_node
        peer_repos = (
            args.catalog_tombstone_peer_repo_node or
            ["/example/repo/provider/repoB", "/example/repo/provider/repoC"]
        )
        payload = b"tombstone epoch conflict object"
        suffix = str(int(time.time() * 1000))
        manifest = repo.put(
            f"APP/CatalogHealth/TombstoneEpochConflict/{suffix}",
            payload,
            object_type="mission-log",
            replication_factor=1,
            replica_nodes=(source_repo,),
            policy_epoch="/Policy/generic-repo/v1",
        )
        stale_updated_ms = max(1, int(time.time() * 1000) - 120000)
        stale_available_entry = {
            "objectName": manifest.object_name,
            "objectSha256": manifest.sha256,
            "manifestSha256": hashlib.sha256(manifest.to_bytes()).hexdigest(),
            "objectType": manifest.object_type,
            "objectClass": manifest.to_dict().get("objectClass", manifest.object_type),
            "size": manifest.size,
            "segmentCount": manifest.segment_count,
            "sourceRepo": source_repo,
            "repoMode": "persistent",
            "state": "AVAILABLE",
            "catalogEpoch": 999999999,
            "lastSeenMs": int(time.time() * 1000),
            "updatedAtMs": stale_updated_ms,
            "minReplicationFactor": manifest.to_dict().get("minReplicationFactor", 1),
            "maxReplicationFactor": manifest.to_dict().get("maxReplicationFactor", 1),
            "desiredReplicationFactor": manifest.replication_factor,
            "repairAllowed": manifest.to_dict().get("repairAllowed", True),
            "replicaNodes": [source_repo],
            "manifest": manifest.to_dict(),
        }
        if not repo.remove(manifest.object_name):
            raise RuntimeError("tombstone epoch conflict remove reported no deletion")

        deadline = time.time() + 80.0
        deleted_peers: set[str] = set()
        while time.time() < deadline and len(deleted_peers) < len(peer_repos):
            for peer_repo in peer_repos:
                if peer_repo in deleted_peers:
                    continue
                try:
                    lookup = repo.catalog_lookup(manifest.object_name, peer_repo)
                    if lookup.get("state") == "DELETED":
                        deleted_peers.add(peer_repo)
                except Exception:
                    pass
            if len(deleted_peers) == len(peer_repos):
                break
            time.sleep(2.0)
        if len(deleted_peers) != len(peer_repos):
            lookups = {}
            for peer_repo in peer_repos:
                try:
                    lookups[peer_repo] = repo.catalog_lookup(manifest.object_name, peer_repo)
                except Exception as exc:
                    lookups[peer_repo] = {"error": str(exc)}
            raise RuntimeError(
                "tombstone did not reach all peers before epoch conflict check: "
                f"object={manifest.object_name} lookups={lookups}"
            )

        final_lookups: dict[str, dict] = {}
        for peer_repo in peer_repos:
            repo.catalog_merge(
                peer_repo,
                [stale_available_entry],
                {
                    "repoNode": source_repo,
                    "repoMode": "persistent",
                    "catalogEpoch": stale_available_entry["catalogEpoch"],
                    "acceptsBackupReplica": True,
                },
            )
            lookup = repo.catalog_lookup(manifest.object_name, peer_repo)
            final_lookups[peer_repo] = lookup
            if lookup.get("state") != "DELETED":
                raise RuntimeError(
                    "stale high-epoch AVAILABLE entry revived tombstoned object: "
                    f"peer={peer_repo} lookup={lookup}"
                )
            if int(lookup.get("availableReplicaCount", 0) or 0) != 0:
                raise RuntimeError(
                    "stale high-epoch AVAILABLE entry left visible replicas: "
                    f"peer={peer_repo} lookup={lookup}"
                )
            unshadowed_available = [
                entry for entry in lookup.get("entries", [])
                if (str(entry.get("state", "")) == "AVAILABLE" and
                    not entry.get("shadowedByTombstone"))
            ]
            if unshadowed_available:
                raise RuntimeError(
                    "stale high-epoch AVAILABLE entry was not shadowed by tombstone: "
                    f"peer={peer_repo} entries={unshadowed_available}"
                )
        print(
            "GENERIC_DISTRIBUTED_REPO_TOMBSTONE_EPOCH_CONFLICT_OK "
            f"object={manifest.object_name} peers={','.join(peer_repos)}",
            flush=True,
        )
        return 0

    if args.object_policy_smoke:
        cases = [
            ("DI/Activation/tmp", "temporary-activation", 1, 1, False),
            ("DI/Model/yolo-shard", "model-artifact", 2, 3, True),
            ("UAV/Recording/session-0001", "uav-recording", 2, 3, True),
            ("UAV/Telemetry/window-0001", "telemetry-log", 1, 2, True),
            ("UAV/Mission/mission-0001", "mission-log", 2, 3, True),
        ]
        for suffix, object_type, expected_min, expected_max, expected_repair in cases:
            manifest = repo.put(
                f"APP/ObjectPolicy/{suffix}/{int(time.time() * 1000)}",
                f"policy object {object_type}".encode(),
                object_type=object_type,
                replication_factor=max(1, expected_min),
                policy_epoch="/Policy/generic-repo/v1",
            )
            lookup = repo.catalog_lookup(
                manifest.object_name,
                manifest.replica_nodes[0],
            )
            if lookup.get("objectClass") != object_type:
                raise RuntimeError(
                    f"object policy class mismatch type={object_type} lookup={lookup}"
                )
            if int(lookup.get("minReplicationFactor", 0) or 0) != expected_min:
                raise RuntimeError(
                    f"object policy min replica mismatch type={object_type} lookup={lookup}"
                )
            if int(lookup.get("maxReplicationFactor", 0) or 0) != expected_max:
                raise RuntimeError(
                    f"object policy max replica mismatch type={object_type} lookup={lookup}"
                )
            if bool(lookup.get("repairAllowed", True)) != expected_repair:
                raise RuntimeError(
                    f"object policy repair flag mismatch type={object_type} lookup={lookup}"
                )
            if int(lookup.get("ttlMs", 0) or 0) < 0:
                raise RuntimeError(
                    f"object policy ttl invalid type={object_type} lookup={lookup}"
                )
        print("GENERIC_DISTRIBUTED_REPO_OBJECT_POLICY_OK", flush=True)
        return 0

    if args.uav_data_product_smoke:
        timestamp = int(time.time() * 1000)
        objects = [
            (
                f"UAV/Drone/A/Recording/session-{timestamp}",
                "uav-recording",
                b"ndnsf-uav-recording-manifest\nframe=0\nframe=1\n",
                2,
            ),
            (
                f"UAV/Drone/A/TelemetryLog/mission-{timestamp}",
                "telemetry-log",
                b"lat=35.118 lon=-89.937 alt=61.0 battery=11.8\n",
                1,
            ),
            (
                f"UAV/Drone/A/MissionLog/mission-{timestamp}",
                "mission-log",
                b"mission=survey state=DONE completed=4 total=4\n",
                2,
            ),
        ]
        fetched = 0
        for suffix, object_type, payload, replicas in objects:
            manifest = repo.put(
                f"APP/{suffix}",
                payload,
                object_type=object_type,
                replication_factor=replicas,
                policy_epoch="/Policy/generic-repo/v1",
            )
            deadline = time.time() + 60.0
            lookup = {}
            while time.time() < deadline:
                lookup = repo.catalog_lookup(
                    manifest.object_name,
                    manifest.replica_nodes[0],
                )
                if (lookup.get("objectClass") == object_type and
                        int(lookup.get("availableReplicaCount", 0) or 0) >= 1):
                    break
                time.sleep(2.0)
            else:
                raise RuntimeError(
                    f"UAV data product did not enter catalog: "
                    f"type={object_type} object={manifest.object_name} lookup={lookup}"
                )
            if repo.get(manifest.object_name, manifest) != payload:
                raise RuntimeError(
                    f"UAV data product payload mismatch: "
                    f"type={object_type} object={manifest.object_name}"
                )
            fetched += 1
        print(
            "GENERIC_DISTRIBUTED_REPO_UAV_DATA_PRODUCT_OK "
            f"objects={fetched}",
            flush=True,
        )
        return 0

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

        manual_payload = b"manual catalog repair object"
        manual_suffix = str(int(time.time() * 1000))
        manual_manifest = repo.put(
            f"APP/CatalogHealth/ManualRepairObject/{manual_suffix}",
            manual_payload,
            object_type="catalog-health-manual-repair",
            replication_factor=1,
            replica_nodes=("/example/repo/provider/repoB",),
            policy_epoch="/Policy/generic-repo/v1",
        )
        source_repo = manual_manifest.replica_nodes[0]
        target_repo = "/example/repo/provider/repoA"
        if target_repo == source_repo:
            target_repo = "/example/repo/provider/repoB"
        now_ms = int(time.time() * 1000)
        manual_entry = {
            "objectName": manual_manifest.object_name,
            "objectSha256": manual_manifest.sha256,
            "manifestSha256": hashlib.sha256(manual_manifest.to_bytes()).hexdigest(),
            "objectType": manual_manifest.object_type,
            "size": manual_manifest.size,
            "segmentCount": manual_manifest.segment_count,
            "sourceRepo": source_repo,
            "repoMode": "persistent",
            "state": "AVAILABLE",
            "catalogEpoch": now_ms,
            "lastSeenMs": now_ms,
            "updatedAtMs": now_ms,
            "minReplicationFactor": 2,
            "maxReplicationFactor": 2,
            "desiredReplicationFactor": 2,
            "replicaNodes": [source_repo],
            "manifest": {
                **manual_manifest.to_dict(),
                "minReplicationFactor": 2,
                "maxReplicationFactor": 2,
                "replicationFactor": 2,
                "replicaNodes": [source_repo],
            },
        }
        repo.catalog_merge(
            args.catalog_health_repo_node,
            [manual_entry],
            {
                "repoNode": source_repo,
                "repoMode": "persistent",
                "catalogEpoch": now_ms,
                "acceptsBackupReplica": True,
            },
        )
        repo.catalog_merge(
            args.catalog_health_repo_node,
            [],
            {
                "repoNode": target_repo,
                "repoMode": "persistent",
                "catalogEpoch": now_ms,
                "acceptsBackupReplica": True,
            },
        )
        manual_lookup = repo.catalog_lookup(
            manual_manifest.object_name, args.catalog_health_repo_node)
        manual_plan = manual_lookup.get("repairPlan", {})
        manual_actions = manual_plan.get("actions", [])
        if not isinstance(manual_actions, list) or not manual_actions:
            raise RuntimeError(
                f"manual catalog repair object missing repair actions: {manual_lookup}"
            )
        manual_action = dict(manual_actions[0])
        if manual_action.get("sourceRepo") != source_repo:
            raise RuntimeError(f"manual catalog repair source is wrong: {manual_lookup}")
        if manual_action.get("targetRepo") != target_repo:
            raise RuntimeError(f"manual catalog repair target is wrong: {manual_lookup}")
        repair_result = repo.catalog_repair(target_repo, manual_action)
        if str(repair_result.get("status", "")) != "repaired":
            raise RuntimeError(f"manual catalog repair failed: {repair_result}")
        repaired_entry = repair_result.get("catalogEntry", {})
        if not isinstance(repaired_entry, dict):
            raise RuntimeError(f"manual catalog repair missing catalog entry: {repair_result}")
        repo.catalog_merge(
            args.catalog_health_repo_node,
            [repaired_entry],
            {
                "repoNode": target_repo,
                "repoMode": "persistent",
                "catalogEpoch": now_ms + 1,
                "acceptsBackupReplica": True,
            },
        )
        repaired_lookup = repo.catalog_lookup(
            manual_manifest.object_name, args.catalog_health_repo_node)
        repaired_candidates = repaired_lookup.get("candidateReplicas", [])
        if not isinstance(repaired_candidates, list):
            raise RuntimeError(
                f"manual catalog repair candidates malformed: {repaired_lookup}"
            )
        has_target_replica = any(
            isinstance(candidate, dict) and
            str(candidate.get("sourceRepo", "")) == target_repo
            for candidate in repaired_candidates
        )
        if not has_target_replica:
            raise RuntimeError(
                f"manual catalog repair did not add target replica: {repaired_lookup}"
            )
        if repo.get(manual_manifest.object_name, manual_manifest) != manual_payload:
            raise RuntimeError("manual catalog repair corrupted object payload")
        print("GENERIC_DISTRIBUTED_REPO_CATALOG_REPAIR_OK", flush=True)
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
        lookup_repo = (
            deleted_manifest.replica_nodes[0]
            if deleted_manifest.replica_nodes else
            args.catalog_snapshot_repo_node
        )
        pre_delete_lookup = repo.catalog_lookup(deleted_manifest.object_name, lookup_repo)
        old_available_entries = [
            dict(entry)
            for entry in pre_delete_lookup.get("entries", [])
            if str(entry.get("state", "")) == "AVAILABLE"
        ]
        if not old_available_entries:
            raise RuntimeError(
                "repo delete regression could not find pre-delete catalog replicas: "
                f"repo={lookup_repo} lookup={pre_delete_lookup}"
            )
        removed = repo.remove(deleted_manifest.object_name)
        if not removed:
            raise RuntimeError("repo remove reported no deletion")
        lookup = repo.catalog_lookup(deleted_manifest.object_name, lookup_repo)
        states = {str(entry.get("state", "")) for entry in lookup.get("entries", [])}
        if lookup.get("state") != "DELETED" and "DELETED" not in states:
            raise RuntimeError(
                "repo delete did not leave a catalog tombstone: "
                f"repo={lookup_repo} lookup={lookup}"
            )
        repo.catalog_merge(
            lookup_repo,
            old_available_entries,
            {
                "repoNode": str(old_available_entries[0].get("sourceRepo", "")),
                "repoMode": "persistent",
            },
        )
        revived_lookup = repo.catalog_lookup(deleted_manifest.object_name, lookup_repo)
        if revived_lookup.get("state") != "DELETED":
            raise RuntimeError(
                "repo tombstone allowed old catalog entries to revive object: "
                f"repo={lookup_repo} lookup={revived_lookup}"
            )
        if int(revived_lookup.get("availableReplicaCount", 0) or 0) != 0:
            raise RuntimeError(
                "repo tombstone left old replicas available after stale merge: "
                f"repo={lookup_repo} lookup={revived_lookup}"
            )
        unshadowed_available = [
            entry for entry in revived_lookup.get("entries", [])
            if (str(entry.get("state", "")) == "AVAILABLE" and
                not entry.get("shadowedByTombstone"))
        ]
        if unshadowed_available:
            raise RuntimeError(
                "repo tombstone left unshadowed AVAILABLE entries after stale merge: "
                f"repo={lookup_repo} lookup={revived_lookup}"
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
