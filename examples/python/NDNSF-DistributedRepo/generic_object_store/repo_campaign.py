#!/usr/bin/env python3
"""Headless NDNSF-REPO read/write campaign driver."""

from __future__ import annotations

import argparse
import csv
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import random
import statistics
import threading
import time

from ndnsf import ServiceUser, make_segmented_data_packets
from ndnsf_distributed_inference import APPDeployment
from ndnsf_distributed_inference.repo import (
    NetworkDistributedRepoClient,
    RepoIncompleteWriteError,
    RepoObjectManifest,
    WriteConsistency,
    encode_repo_request,
)


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(
        quantile * (len(ordered) - 1)))))
    return float(ordered[index])


def deterministic_payload(seed: int, index: int, size: int) -> bytes:
    block = hashlib.sha256(f"{seed}:{index}".encode()).digest()
    return (block * ((size + len(block) - 1) // len(block)))[:size]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--generated-policy-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--ready-file", default="",
                        help="Write a marker after seed replication succeeds.")
    parser.add_argument("--duration-s", type=float, default=60.0)
    parser.add_argument("--rps", type=float, default=1.0)
    parser.add_argument("--concurrency", type=int, default=4)
    parser.add_argument("--read-ratio", type=float, default=0.8)
    parser.add_argument("--object-bytes", type=int, default=2048)
    parser.add_argument("--object-mode", choices=("opaque", "exact"),
                        default="opaque")
    parser.add_argument("--replication-factor", type=int, default=2)
    parser.add_argument("--write-consistency",
                        choices=tuple(item.value for item in WriteConsistency),
                        default=WriteConsistency.ALL.value)
    parser.add_argument("--repo-node", action="append", default=[])
    parser.add_argument("--seed", type=int, default=77001)
    parser.add_argument("--seed-attempts", type=int, default=3)
    parser.add_argument("--ack-timeout-ms", type=int, default=1000)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--handler-threads", type=int, default=2)
    parser.add_argument("--control-mode", choices=("normal", "targeted"),
                        default="targeted")
    parser.add_argument("--disable-targeted-fallback", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if not 0.0 <= args.read_ratio <= 1.0:
        raise ValueError("--read-ratio must be between 0 and 1")
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    deployment = APPDeployment.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    ).deployment
    def make_client() -> NetworkDistributedRepoClient:
        return NetworkDistributedRepoClient(
            user=ServiceUser(
                group=deployment.group,
                controller=deployment.controller,
                user=deployment.user,
                trust_schema=deployment.trust_schema,
                permission_wait_ms=6000,
                handler_threads=max(0, args.handler_threads),
                adaptive_admission=False,
            ),
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=args.timeout_ms,
            placement_cache_ttl_ms=5000,
            replica_cooldown_ms=3000,
            control_mode=args.control_mode,
            enable_targeted_fallback=not args.disable_targeted_fallback,
        )

    client = make_client()
    client.wait_until_ready(30.0)
    def worker_client() -> NetworkDistributedRepoClient:
        return client
    replicas = tuple(str(value) for value in args.repo_node if str(value))
    if replicas and len(replicas) < args.replication_factor:
        raise ValueError("fewer --repo-node values than replication factor")

    rng = random.Random(args.seed)
    rng_lock = threading.Lock()
    manifest_lock = threading.Lock()
    manifests: list[RepoObjectManifest] = []
    rows: list[dict[str, object]] = []
    rows_lock = threading.Lock()

    def write(index: int, active_client: NetworkDistributedRepoClient = client
              ) -> RepoObjectManifest:
        payload = deterministic_payload(args.seed, index, args.object_bytes)
        object_name = active_client.publisher_object_name(
            f"CAMPAIGN/{args.seed}/{index}")
        if args.object_mode == "exact":
            data_name = f"/data/ndnsf-repo/campaign/{args.seed}/{index}"
            packets = make_segmented_data_packets(
                data_name, payload, signing_identity=deployment.user,
                max_segment_size=2048)
            return active_client.store_signed_packets(
                object_name=object_name, packets=packets,
                object_type="campaign-exact-data",
                object_size=len(payload),
                object_sha256=hashlib.sha256(payload).hexdigest(),
                replication_factor=args.replication_factor,
                replica_nodes=replicas,
                policy_epoch="/Policy/repo-campaign/v1",
                data_name=data_name,
                metadata={"campaignSeed": args.seed},
            )
        return active_client.store_versioned(
            object_name=object_name, payload=payload,
            object_type="campaign-opaque-object",
            generation=0, expected_generation=-1,
            write_consistency=args.write_consistency,
            replication_factor=args.replication_factor,
            replica_nodes=replicas,
            policy_epoch="/Policy/repo-campaign/v1",
            metadata={"campaignSeed": args.seed},
        )

    seed_manifest = None
    seed_errors = []
    seed_attempt_count = max(1, int(args.seed_attempts))
    for seed_attempt in range(1, seed_attempt_count + 1):
        try:
            seed_manifest = write(-seed_attempt)
            break
        except Exception as exc:  # noqa: BLE001
            seed_errors.append({
                "attempt": seed_attempt,
                "error": str(exc),
            })
    if seed_manifest is None:
        raise RuntimeError(
            f"repo campaign seed failed after {seed_attempt_count} attempts: "
            f"{seed_errors}")
    manifests.append(seed_manifest)
    if args.ready_file:
        ready_file = Path(args.ready_file)
        ready_file.parent.mkdir(parents=True, exist_ok=True)
        ready_file.write_text(json.dumps({
            "ready": True,
            "timestampMs": int(time.time() * 1000),
            "seedObject": seed_manifest.object_name,
            "seedAttempts": len(seed_errors) + 1,
            "seedErrors": seed_errors,
            "confirmedReplicas": list(seed_manifest.confirmed_replica_nodes),
        }, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    client.reset_control_metrics()

    def one(index: int) -> None:
        active_client = worker_client()
        with rng_lock:
            read = rng.random() < args.read_ratio and bool(manifests)
            manifest = rng.choice(manifests) if read else None
        operation = "read" if read else "write"
        started_epoch_ms = int(time.time() * 1000)
        started = time.monotonic()
        success = False
        error = ""
        confirmed = 0
        object_name = (
            manifest.object_name if manifest is not None else
            active_client.publisher_object_name(f"CAMPAIGN/{args.seed}/{index}")
        )
        phase_metrics: dict[str, float] = {}
        active_client.begin_operation_metrics()
        try:
            if manifest is not None:
                if manifest.packet_names:
                    active_client.fetch_signed_packets(manifest)
                else:
                    active_client.fetch_object(manifest.object_name, manifest)
            else:
                created = write(index, active_client)
                object_name = created.object_name
                confirmed = len(created.confirmed_replica_nodes)
                with manifest_lock:
                    manifests.append(created)
            success = True
        except Exception as exc:  # noqa: BLE001
            error = str(exc)
            if isinstance(exc, RepoIncompleteWriteError):
                confirmed = len(exc.confirmed_replicas)
        finally:
            phase_metrics = active_client.end_operation_metrics()
        elapsed_ms = (time.monotonic() - started) * 1000.0
        completed_epoch_ms = int(time.time() * 1000)
        with rows_lock:
            rows.append({
                "request": index,
                "operation": operation,
                "objectName": object_name,
                "success": int(success),
                "startedEpochMs": started_epoch_ms,
                "completedEpochMs": completed_epoch_ms,
                "latencyMs": round(elapsed_ms, 3),
                "reserveMs": round(phase_metrics.get("reserveMs", 0.0), 3),
                "storeMs": round(phase_metrics.get("storeMs", 0.0), 3),
                "confirmedReplicas": confirmed,
                "error": error,
            })

    started = time.monotonic()
    deadline = started + max(0.1, args.duration_s)
    interval = 1.0 / max(0.01, args.rps)
    next_submit = started
    futures = set()
    with ThreadPoolExecutor(max_workers=max(1, args.concurrency)) as executor:
        index = 0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now < next_submit:
                time.sleep(min(0.01, next_submit - now))
                continue
            completed = {future for future in futures if future.done()}
            for future in completed:
                future.result()
            futures.difference_update(completed)
            if len(futures) >= max(1, args.concurrency):
                rejected_epoch_ms = int(time.time() * 1000)
                with rows_lock:
                    rows.append({
                        "request": index,
                        "operation": "admission",
                        "objectName": "",
                        "success": 0,
                        "startedEpochMs": rejected_epoch_ms,
                        "completedEpochMs": rejected_epoch_ms,
                        "latencyMs": 0.0,
                        "reserveMs": 0.0,
                        "storeMs": 0.0,
                        "confirmedReplicas": 0,
                        "error": "client-admission-overloaded",
                    })
            else:
                futures.add(executor.submit(one, index))
            index += 1
            next_submit += interval
        for future in futures:
            future.result()
    wall_s = time.monotonic() - started

    csv_path = output_dir / "request-lifecycle.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=(
            "request", "operation", "objectName", "success", "latencyMs",
            "startedEpochMs", "completedEpochMs",
            "reserveMs", "storeMs", "confirmedReplicas", "error"))
        writer.writeheader()
        writer.writerows(rows)
    successful_latencies = [
        float(row["latencyMs"]) for row in rows if int(row["success"])]
    failures = [row for row in rows if not int(row["success"])]
    rejection_count = sum(
        "repo-overloaded" in str(row["error"]) or
        "repo-capacity-rejected" in str(row["error"]) or
        "client-admission-overloaded" in str(row["error"])
        for row in failures)
    successful_write_receipts = [
        int(row["confirmedReplicas"])
        for row in rows
        if int(row["success"]) and row["operation"] == "write"]
    capabilities = []
    for repo_node in replicas:
        try:
            response = client._request_specific_repo(
                repo_node=repo_node,
                payload=encode_repo_request("CAPABILITY"))
            capabilities.append(json.loads(response.payload.decode()))
        except Exception as exc:  # noqa: BLE001
            capabilities.append({"repoNode": repo_node, "error": str(exc)})
    summary = {
        "seed": args.seed,
        "seedAttempts": len(seed_errors) + 1,
        "seedErrors": seed_errors,
        "objectMode": args.object_mode,
        "durationSeconds": args.duration_s,
        "wallSeconds": wall_s,
        "offeredRps": args.rps,
        "concurrency": args.concurrency,
        "readRatio": args.read_ratio,
        "replicationFactor": args.replication_factor,
        "writeConsistency": args.write_consistency,
        "controlMode": args.control_mode,
        "campaignStartEpochMs": min(
            (int(row["startedEpochMs"]) for row in rows), default=0),
        "campaignEndEpochMs": max(
            (int(row["completedEpochMs"]) for row in rows), default=0),
        "attempted": len(rows),
        "succeeded": len(rows) - len(failures),
        "failed": len(failures),
        "failureRate": len(failures) / max(1, len(rows)),
        "rejectionCount": rejection_count,
        "successfulWriteReceiptCounts": successful_write_receipts,
        "minimumSuccessfulWriteReceipts": min(
            successful_write_receipts, default=0),
        "achievedRps": (len(rows) - len(failures)) / max(0.001, wall_s),
        "latencyMs": {
            "p50": percentile(successful_latencies, 0.50),
            "p95": percentile(successful_latencies, 0.95),
            "p99": percentile(successful_latencies, 0.99),
            "mean": statistics.fmean(successful_latencies)
            if successful_latencies else 0.0,
            "stddev": statistics.pstdev(successful_latencies)
            if len(successful_latencies) > 1 else 0.0,
        },
        "capabilities": capabilities,
        "controlMetrics": client.control_metrics(),
        "lifecycleCsv": str(csv_path),
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(json.dumps(summary, sort_keys=True), flush=True)
    client.close()
    return 0 if not failures else 2


if __name__ == "__main__":
    raise SystemExit(main())
