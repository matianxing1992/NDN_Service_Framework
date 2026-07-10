#!/usr/bin/env python3
"""Store and fetch non-AI objects through an NDNSF-DistributedRepo cluster."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import time

from ndnsf import DataPacket, make_segmented_data_packets
from ndnsf_distributed_inference import APPDeployment, DistributedRepo
from ndnsf_distributed_inference.repo import RepoObjectManifest, RepoRepairAction


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


def deterministic_tiered_blob(index: int, size: int) -> bytes:
    seed = f"ndnsf-repo-tiered-cache-{index}".encode()
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
    parser.add_argument("--quick-core-smoke", action="store_true",
                        help="Only verify one small single-replica put/get through the repo service")
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
    parser.add_argument("--uav-browse-smoke", action="store_true",
                        help="Query and fetch UAV recording/log objects from the catalog")
    parser.add_argument("--uav-browse-repo-node",
                        default="/example/repo/provider/repoA")
    parser.add_argument("--uav-browse-object-class", default="",
                        help="Optional objectClass filter for UAV browsing")
    parser.add_argument("--uav-browse-mission-id", default="mission-demo")
    parser.add_argument("--tiered-cache-seed-smoke", action="store_true")
    parser.add_argument("--tiered-cache-verify-smoke", action="store_true")
    parser.add_argument("--tiered-cache-state-file", default="")
    parser.add_argument("--tiered-cache-summary-file", default="")
    parser.add_argument("--tiered-cache-repo-node",
                        default="/example/repo/provider/repoA")
    parser.add_argument("--tiered-cache-object-bytes", type=int, default=4096)
    parser.add_argument("--exact-packet-seed-smoke", action="store_true")
    parser.add_argument("--exact-packet-verify-smoke", action="store_true")
    parser.add_argument("--exact-packet-state-file", default="")
    parser.add_argument("--exact-packet-summary-file", default="")
    parser.add_argument("--exact-packet-repo-node",
                        default="/example/repo/provider/repoA")
    parser.add_argument("--exact-packet-secondary-repo-node", default="")
    parser.add_argument("--exact-packet-failover-trigger-file", default="")
    parser.add_argument("--exact-packet-failover-resume-file", default="")
    parser.add_argument("--exact-packet-failover-wait-s", type=float, default=60.0)
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

    if args.exact_packet_seed_smoke:
        if not args.exact_packet_state_file:
            raise ValueError("--exact-packet-state-file is required")
        repo.wait_until_ready(20.0)
        suffix = str(int(time.time() * 1000))
        payload = deterministic_blob(8 * 1024)
        data_prefix = f"/data/ndnsf-repo/exact/{suffix}"
        packets = make_segmented_data_packets(
            data_prefix,
            payload,
            signing_identity=user_name,
            # Keep a directly pushed packet plus base64/NDNSF security envelope
            # below ndn-cxx's 8800-byte packet limit. Larger application objects
            # still use any number of these exact immutable Data packets.
            max_segment_size=2048,
        )
        replica_nodes = tuple(filter(None, (
            args.exact_packet_repo_node,
            args.exact_packet_secondary_repo_node,
        )))
        manifest = repo.put_signed_packets(
            f"APP/ExactPackets/{suffix}",
            packets,
            object_type="exact-ndn-data-packets",
            object_size=len(payload),
            object_sha256=hashlib.sha256(payload).hexdigest(),
            replication_factor=len(replica_nodes),
            replica_nodes=replica_nodes,
            policy_epoch="/Policy/generic-repo/exact-packets-v1",
            data_name=data_prefix,
        )
        state = {
            "schema": "ndnsf-repo-exact-packets-minindn-v1",
            "repoNode": args.exact_packet_repo_node,
            "repoNodes": list(replica_nodes),
            "payloadSha256": hashlib.sha256(payload).hexdigest(),
            "payloadSize": len(payload),
            "manifest": manifest.to_dict(),
            "packets": [
                {
                    "name": packet.name,
                    "segment": packet.segment,
                    "wireSha256": hashlib.sha256(packet.wire).hexdigest(),
                }
                for packet in packets
            ],
        }
        state_path = Path(args.exact_packet_state_file)
        state_path.parent.mkdir(parents=True, exist_ok=True)
        state_path.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n",
                              encoding="utf-8")
        print(
            "GENERIC_DISTRIBUTED_REPO_EXACT_PACKET_SEED_OK "
            f"repos={','.join(replica_nodes)} packets={len(packets)} "
            f"state={state_path}",
            flush=True,
        )
        if not args.exact_packet_verify_smoke:
            return 0

    if args.exact_packet_verify_smoke:
        if not args.exact_packet_state_file or not args.exact_packet_summary_file:
            raise ValueError(
                "--exact-packet-state-file and --exact-packet-summary-file are required")
        state = json.loads(Path(args.exact_packet_state_file).read_text(
            encoding="utf-8"))
        manifest = RepoObjectManifest.from_dict(state["manifest"])
        repo_node = str(state["repoNode"])
        repo_nodes = tuple(str(value) for value in state.get(
            "repoNodes", [repo_node]))
        expected_packets = list(state["packets"])
        packet_names = [str(item["name"]) for item in expected_packets]
        trigger_file = Path(args.exact_packet_failover_trigger_file) \
            if args.exact_packet_failover_trigger_file else None
        resume_file = Path(args.exact_packet_failover_resume_file) \
            if args.exact_packet_failover_resume_file else None
        failover_enabled = trigger_file is not None or resume_file is not None
        if failover_enabled and (trigger_file is None or resume_file is None):
            raise ValueError(
                "exact packet failover requires both trigger and resume files")

        attempts: list[dict[str, object]] = []
        barrier_triggered_at_ms = 0
        if failover_enabled:
            original_fetch_packet = repo._client.fetch_packet
            barrier_fired = False

            def controlled_fetch_packet(target_repo: str, data_name: str) -> DataPacket:
                nonlocal barrier_fired, barrier_triggered_at_ms
                started_ms = time.time_ns() // 1_000_000
                try:
                    packet = original_fetch_packet(target_repo, data_name)
                except Exception as exc:
                    attempts.append({
                        "repoNode": target_repo,
                        "packetName": data_name,
                        "success": False,
                        "error": str(exc),
                        "timestampMs": started_ms,
                    })
                    raise
                attempts.append({
                    "repoNode": target_repo,
                    "packetName": data_name,
                    "success": True,
                    "error": "",
                    "timestampMs": started_ms,
                })
                if target_repo == repo_node and not barrier_fired:
                    barrier_fired = True
                    barrier_triggered_at_ms = time.time_ns() // 1_000_000
                    trigger_file.parent.mkdir(parents=True, exist_ok=True)
                    trigger_file.write_text(json.dumps({
                        "repoNode": target_repo,
                        "packetName": data_name,
                        "timestampMs": barrier_triggered_at_ms,
                    }, sort_keys=True) + "\n", encoding="utf-8")
                    deadline = time.monotonic() + max(
                        1.0, args.exact_packet_failover_wait_s)
                    while not resume_file.exists():
                        if time.monotonic() >= deadline:
                            raise TimeoutError(
                                "timed out waiting for exact packet failover resume")
                        time.sleep(0.05)
                return packet

            repo._client.fetch_packet = controlled_fetch_packet

        fetch_started_ms = time.time_ns() // 1_000_000
        fetched_packets = repo.get_signed_packets(
            manifest.object_name,
            manifest,
            repo_node="" if failover_enabled else repo_node,
        )
        fetch_completed_ms = time.time_ns() // 1_000_000
        fetched_names = [packet.name for packet in fetched_packets]
        expected_wire_sha256 = [
            str(packet["wireSha256"]) for packet in expected_packets
        ]
        actual_wire_sha256 = [
            hashlib.sha256(packet.wire).hexdigest()
            for packet in fetched_packets
        ]
        wire_matches = [
            actual == expected
            for actual, expected in zip(actual_wire_sha256, expected_wire_sha256)
        ]
        checks = {
            "exactNames": fetched_names == packet_names,
            "wireIdentity": all(wire_matches),
            "manifestPacketIndex": list(manifest.packet_names) == packet_names,
            "batchPacketConsumer": len(fetched_packets) == len(expected_packets),
            "noRepoPacketAliases": not any(
                "/ndn-data/" in name or f"{manifest.object_name}/seg/" in name
                for name in packet_names
            ),
        }
        if failover_enabled:
            if len(repo_nodes) < 2:
                raise ValueError("exact packet failover requires two replicas")
            secondary_repo = repo_nodes[1]
            primary_successes = [
                str(item["packetName"]) for item in attempts
                if item["repoNode"] == repo_node and item["success"]
            ]
            primary_failures = [
                item for item in attempts
                if item["repoNode"] == repo_node and not item["success"]
            ]
            secondary_successes = [
                str(item["packetName"]) for item in attempts
                if item["repoNode"] == secondary_repo and item["success"]
            ]
            checks.update({
                "primaryOnePacketBeforeFailure": primary_successes == packet_names[:1],
                "primaryFailureObserved": bool(primary_failures),
                "secondaryRestartedWholeSet": secondary_successes == packet_names,
            })
        else:
            payload = repo.get(manifest.object_name, manifest)
            checks.update({
                "restartPersistence": True,
                "payloadReassembly": (
                    len(payload) == int(state["payloadSize"]) and
                    hashlib.sha256(payload).hexdigest() ==
                    str(state["payloadSha256"])
                ),
            })
        summary = {
            "schema": (
                "ndnsf-repo-exact-packet-failover-minindn-v1"
                if failover_enabled else
                "ndnsf-repo-exact-packets-minindn-result-v1"
            ),
            "repoNode": repo_node,
            "replicaNodes": list(repo_nodes),
            "objectName": manifest.object_name,
            "packetCount": len(expected_packets),
            "packetNames": packet_names,
            "expectedPacketWireSha256": expected_wire_sha256,
            "actualPacketWireSha256": actual_wire_sha256,
            "attempts": attempts,
            "latencyMs": fetch_completed_ms - fetch_started_ms,
            "failoverLatencyMs": (
                fetch_completed_ms - barrier_triggered_at_ms
                if barrier_triggered_at_ms else 0
            ),
            "checks": checks,
            "passed": all(checks.values()),
        }
        summary_path = Path(args.exact_packet_summary_file)
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                                encoding="utf-8")
        if not summary["passed"]:
            raise RuntimeError(f"exact packet MiniNDN checks failed: {summary}")
        print(
            ("GENERIC_DISTRIBUTED_REPO_EXACT_PACKET_FAILOVER_VERIFY_OK "
             if failover_enabled else
             "GENERIC_DISTRIBUTED_REPO_EXACT_PACKET_VERIFY_OK ") +
            f"repo={repo_node} packets={len(expected_packets)} "
            f"summary={summary_path}",
            flush=True,
        )
        return 0

    if args.tiered_cache_seed_smoke:
        if not args.tiered_cache_state_file:
            raise ValueError("--tiered-cache-state-file is required for seed smoke")
        repo.wait_until_ready(20.0)
        suffix = str(int(time.time() * 1000))
        objects = []
        for index in range(3):
            payload = deterministic_tiered_blob(index, args.tiered_cache_object_bytes)
            manifest = repo.put(
                f"APP/TieredCache/{suffix}/object-{index}",
                payload,
                object_type="tiered-cache-smoke",
                replication_factor=1,
                replica_nodes=(args.tiered_cache_repo_node,),
                policy_epoch="/Policy/generic-repo/tiered-cache-v1",
            )
            objects.append({
                "index": index,
                "objectName": manifest.object_name,
                "sha256": hashlib.sha256(payload).hexdigest(),
                "size": len(payload),
                "manifest": manifest.to_dict(),
            })
        state = {
            "schema": "ndnsf-repo-tiered-cache-minindn-v1",
            "repoNode": args.tiered_cache_repo_node,
            "objects": objects,
            "seedCacheStatus": repo.cache_status(args.tiered_cache_repo_node),
        }
        state_path = Path(args.tiered_cache_state_file)
        state_path.parent.mkdir(parents=True, exist_ok=True)
        state_path.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n",
                              encoding="utf-8")
        print(
            "GENERIC_DISTRIBUTED_REPO_TIERED_CACHE_SEED_OK "
            f"repo={args.tiered_cache_repo_node} objects={len(objects)} "
            f"state={state_path}",
            flush=True,
        )
        return 0

    if args.tiered_cache_verify_smoke:
        if not args.tiered_cache_state_file or not args.tiered_cache_summary_file:
            raise ValueError(
                "--tiered-cache-state-file and --tiered-cache-summary-file are required")
        state = json.loads(Path(args.tiered_cache_state_file).read_text(encoding="utf-8"))
        objects = list(state.get("objects", []))
        if len(objects) != 3:
            raise RuntimeError(f"tiered cache state needs three objects: {state}")
        repo_node = str(state.get("repoNode", args.tiered_cache_repo_node))
        baseline = repo.cache_status(repo_node)

        def fetch_and_verify(item: dict) -> bytes:
            manifest = RepoObjectManifest.from_dict(item["manifest"])
            fetched = repo.get(manifest.object_name, manifest)
            actual = hashlib.sha256(fetched).hexdigest()
            if actual != str(item["sha256"]) or len(fetched) != int(item["size"]):
                raise RuntimeError(
                    f"tiered cache payload mismatch object={manifest.object_name} "
                    f"sha256={actual} bytes={len(fetched)}")
            return fetched

        fetch_and_verify(objects[0])
        after_cold = repo.cache_status(repo_node)
        fetch_and_verify(objects[0])
        after_hot = repo.cache_status(repo_node)
        fetch_and_verify(objects[1])
        fetch_and_verify(objects[2])
        before_fallback = repo.cache_status(repo_node)
        fetch_and_verify(objects[0])
        final_status = repo.cache_status(repo_node)

        checks = {
            "restartPersistence": True,
            "coldMiss": int(after_cold.get("misses", 0)) > int(baseline.get("misses", 0)),
            "coldBackingRead": (
                int(after_cold.get("backingReads", 0)) >
                int(baseline.get("backingReads", 0))
            ),
            "repeatHit": int(after_hot.get("hits", 0)) > int(after_cold.get("hits", 0)),
            "repeatAvoidedBackingRead": (
                int(after_hot.get("backingReads", 0)) ==
                int(after_cold.get("backingReads", 0))
            ),
            "evictionObserved": (
                int(final_status.get("evictions", 0)) >
                int(baseline.get("evictions", 0))
            ),
            "fallbackReadObserved": (
                int(final_status.get("backingReads", 0)) >
                int(before_fallback.get("backingReads", 0))
            ),
            "budgetRespected": (
                int(final_status.get("usedBytes", 0)) <=
                int(final_status.get("budgetBytes", 0))
            ),
            "sqliteAuthority": final_status.get("authoritativeBackend") == "sqlite",
        }
        summary = {
            "schema": "ndnsf-repo-tiered-cache-minindn-result-v1",
            "repoNode": repo_node,
            "objectCount": len(objects),
            "checks": checks,
            "baseline": baseline,
            "afterCold": after_cold,
            "afterHot": after_hot,
            "beforeFallback": before_fallback,
            "final": final_status,
            "passed": all(checks.values()),
        }
        summary_path = Path(args.tiered_cache_summary_file)
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                                encoding="utf-8")
        if not summary["passed"]:
            raise RuntimeError(f"tiered cache MiniNDN checks failed: {summary}")
        print(
            "GENERIC_DISTRIBUTED_REPO_TIERED_CACHE_VERIFY_OK "
            f"repo={repo_node} hits={final_status.get('hits', 0)} "
            f"misses={final_status.get('misses', 0)} "
            f"evictions={final_status.get('evictions', 0)} "
            f"summary={summary_path}",
            flush=True,
        )
        return 0

    if args.quick_core_smoke:
        capability = repo.wait_until_ready(15.0)
        payload = json.dumps({
            "smoke": "quick-core",
            "timestampMs": int(time.time() * 1000),
        }, sort_keys=True).encode()
        manifest = repo.put(
            f"APP/QuickSmoke/Object/{int(time.time() * 1000)}",
            payload,
            object_type="quick-smoke",
            replication_factor=1,
            replica_nodes=("/example/repo/provider/repoA",),
            policy_epoch="/Policy/generic-repo/v1",
        )
        fetched = repo.get(manifest.object_name, manifest)
        if fetched != payload:
            raise RuntimeError(
                f"quick core smoke fetch mismatch object={manifest.object_name}"
            )
        print(
            "GENERIC_DISTRIBUTED_REPO_QUICK_CORE_OK "
            f"repoMode={capability.get('repoMode', 'unknown')} "
            f"object={manifest.object_name} replica={manifest.replica_nodes[0]} "
            f"bytes={len(payload)}",
            flush=True,
        )
        return 0

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
            ("Configured/object-0001", "configured-demo", 2, 2, False),
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
            if object_type == "configured-demo":
                if int(lookup.get("ttlMs", 0) or 0) != 12345:
                    raise RuntimeError(
                        f"configured object policy ttl mismatch lookup={lookup}"
                    )
                if not bool(lookup.get("autoDelete", False)):
                    raise RuntimeError(
                        f"configured object policy autoDelete missing lookup={lookup}"
                    )
                if str(lookup.get("deletePolicy", "")) != "ttl-expiry":
                    raise RuntimeError(
                        f"configured object policy deletePolicy mismatch lookup={lookup}"
                    )
        expiring_manifest = repo.put(
            f"APP/ObjectPolicy/TTL/expire/{int(time.time() * 1000)}",
            b"short lived object",
            object_type="mission-log",
            replication_factor=1,
            policy_epoch="/Policy/generic-repo/v1",
        )
        now_ms = int(time.time() * 1000)
        expiring_entry = {
            "objectName": expiring_manifest.object_name,
            "objectSha256": expiring_manifest.sha256,
            "manifestSha256": hashlib.sha256(expiring_manifest.to_bytes()).hexdigest(),
            "objectType": expiring_manifest.object_type,
            "objectClass": expiring_manifest.to_dict().get(
                "objectClass", expiring_manifest.object_type),
            "size": expiring_manifest.size,
            "segmentCount": expiring_manifest.segment_count,
            "sourceRepo": expiring_manifest.replica_nodes[0],
            "repoMode": "persistent",
            "state": "AVAILABLE",
            "catalogEpoch": now_ms,
            "lastSeenMs": now_ms,
            "updatedAtMs": now_ms,
            "minReplicationFactor": 2,
            "maxReplicationFactor": 2,
            "desiredReplicationFactor": 2,
            "ttlMs": 1,
            "repairAllowed": True,
            "replicaNodes": list(expiring_manifest.replica_nodes),
            "manifest": {
                **expiring_manifest.to_dict(),
                "minReplicationFactor": 2,
                "maxReplicationFactor": 2,
                "replicationFactor": 2,
                "ttlMs": 1,
                "repairAllowed": True,
            },
        }
        repo.catalog_merge(
            expiring_manifest.replica_nodes[0],
            [expiring_entry],
            {
                "repoNode": expiring_manifest.replica_nodes[0],
                "repoMode": "persistent",
                "catalogEpoch": now_ms,
                "acceptsBackupReplica": True,
            },
        )
        time.sleep(0.05)
        expired_lookup = repo.catalog_lookup(
            expiring_manifest.object_name,
            expiring_manifest.replica_nodes[0],
        )
        if expired_lookup.get("state") != "EXPIRED":
            raise RuntimeError(f"expired object state mismatch: {expired_lookup}")
        if not expired_lookup.get("expired"):
            raise RuntimeError(f"expired object flag missing: {expired_lookup}")
        if expired_lookup.get("eligibleForRepair"):
            raise RuntimeError(f"expired object should not be repair eligible: {expired_lookup}")
        if expired_lookup.get("underReplicated"):
            raise RuntimeError(f"expired object should not be under-replicated: {expired_lookup}")
        repair_plan = expired_lookup.get("repairPlan", {})
        if repair_plan.get("reason") != "expired":
            raise RuntimeError(f"expired object repair reason mismatch: {expired_lookup}")
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
            metadata = {
                "tags": ["uav", object_type, "mission-demo"],
                "missionId": "mission-demo",
                "droneId": "drone-A",
            }
            manifest = repo.put(
                f"APP/{suffix}",
                payload,
                object_type=object_type,
                replication_factor=replicas,
                policy_epoch="/Policy/generic-repo/v1",
                metadata=metadata,
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
            query = repo.catalog_query(
                manifest.replica_nodes[0],
                {
                    "objectClass": object_type,
                    "publisher": "/example/repo/user",
                    "tags": ["uav", object_type],
                    "metadata": {"missionId": "mission-demo"},
                },
            )
            query_names = {
                str(item.get("objectName", ""))
                for item in query.get("objects", [])
            }
            if manifest.object_name not in query_names:
                raise RuntimeError(
                    f"UAV data product catalog query missed object: "
                    f"type={object_type} object={manifest.object_name} query={query}"
                )
            fetched += 1
        print(
            "GENERIC_DISTRIBUTED_REPO_UAV_DATA_PRODUCT_OK "
            f"objects={fetched}",
            flush=True,
        )
        print(
            "GENERIC_DISTRIBUTED_REPO_CATALOG_QUERY_OK "
            f"objects={fetched}",
            flush=True,
        )
        return 0

    if args.uav_browse_smoke:
        query = {
            "tags": ["uav"],
            "metadata": {"missionId": args.uav_browse_mission_id},
        }
        if args.uav_browse_object_class:
            query["objectClass"] = args.uav_browse_object_class
        result = repo.catalog_query(args.uav_browse_repo_node, query)
        objects = [
            item for item in result.get("objects", [])
            if isinstance(item, dict)
        ]
        if not objects:
            raise RuntimeError(f"UAV browse query returned no objects: {result}")
        fetched = 0
        for item in objects:
            object_name = str(item.get("objectName", ""))
            if not object_name:
                continue
            lookup = repo.catalog_lookup(object_name, args.uav_browse_repo_node)
            candidates = [
                candidate for candidate in lookup.get("candidateReplicas", [])
                if isinstance(candidate, dict) and
                str(candidate.get("state", "")) == "AVAILABLE"
            ]
            if not candidates:
                raise RuntimeError(f"UAV browse object has no available replica: {lookup}")
            manifest_dict = candidates[0].get("manifest", {})
            if not isinstance(manifest_dict, dict):
                raise RuntimeError(f"UAV browse object has no manifest: {lookup}")
            manifest = RepoObjectManifest.from_dict(manifest_dict)
            payload = repo.get(manifest.object_name, manifest)
            if not payload:
                raise RuntimeError(f"UAV browse fetched empty object: {manifest.object_name}")
            fetched += 1
        print(
            "GENERIC_DISTRIBUTED_REPO_UAV_BROWSE_OK "
            f"objects={len(objects)} fetched={fetched}",
            flush=True,
        )
        return 0

    if args.catalog_health_smoke:
        object_name = repo.object_name(args.catalog_health_object_suffix)
        lookup = repo.catalog_lookup(object_name, args.catalog_health_repo_node)
        stale_repos = set(str(value) for value in lookup.get("staleRepos", []))
        if not stale_repos:
            raise RuntimeError(
                f"catalog health did not mark any stale repo "
                f"preferred={args.catalog_health_stale_repo} "
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
        first_repair_action = RepoRepairAction.from_dict(first_action)
        if first_repair_action.to_dict().get("schemaVersion") != 1:
            raise RuntimeError(
                f"catalog health repair action missing schema version: {repair_lookup}"
            )
        if first_repair_action.to_dict().get("actionType") != "copy-replica":
            raise RuntimeError(
                f"catalog health repair action type is wrong: {repair_lookup}"
            )
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
        RepoRepairAction.from_dict(manual_action, target_repo_node=target_repo)
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
    signed_manifest = repo.put_signed_packets(
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
    fetched_packets = repo.get_signed_packets(object_name, signed_manifest)
    if [packet.wire for packet in fetched_packets] != [packet.wire for packet in packets]:
        raise RuntimeError(f"repo exact packet fetch mismatch for {object_name}")
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
