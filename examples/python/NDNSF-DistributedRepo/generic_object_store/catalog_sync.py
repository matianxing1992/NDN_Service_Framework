#!/usr/bin/env python3
"""Periodic catalog-delta sidecar for the generic DistributedRepo example."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import time

from ndnsf import AckCandidate, SegmentedObjectProducer, ServiceUser
from ndnsf_distributed_inference import APPDeployment
from ndnsf_distributed_inference.repo import (
    DistributedRepo,
    NetworkDistributedRepoClient,
    RepoRepairAction,
    encode_repo_request,
)


CONFIG_FILE = "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_policy.yaml"
REPO_SERVICE = "/NDNSF/DistributedRepo"
CATALOG_MERGE_MAX_REQUEST_BYTES = 6000


def merge_catalog_delta(
    user: ServiceUser,
    repo_node: str,
    entries: list[dict],
    source_status: dict,
    *,
    max_request_bytes: int = CATALOG_MERGE_MAX_REQUEST_BYTES,
) -> dict[str, object]:
    """Merge one peer delta inline or through exact-name segmented pull."""

    inline_request = encode_repo_request(
        "CATALOG_MERGE", entries=entries, sourceStatus=source_status)
    payload_bytes = len(inline_request)
    if payload_bytes <= max_request_bytes:
        request_repo(user, repo_node, inline_request)
        return {
            "mode": "inline", "batches": 1, "segments": 0,
            "payloadBytes": payload_bytes, "fallback": 0,
        }

    merge_payload = json.dumps({
        "schemaVersion": 1,
        "entries": entries,
        "sourceStatus": source_status,
    }, sort_keys=True, separators=(",", ":")).encode()
    payload_sha256 = hashlib.sha256(merge_payload).hexdigest()
    producer = SegmentedObjectProducer(
        f"{repo_node.rstrip('/')}/NDNSF-DISTRIBUTED-REPO/"
        f"CATALOG-MERGE/{payload_sha256}",
        merge_payload,
        signing_identity=str(getattr(user, "user", repo_node)),
        max_segment_size=6000,
        freshness_ms=60_000,
    ).start()
    try:
        time.sleep(0.2)
        request_repo(
            user,
            repo_node,
            encode_repo_request(
                "CATALOG_MERGE_PULL",
                schemaVersion=1,
                sourceName=producer.versioned_name,
                payloadSha256=payload_sha256,
                payloadBytes=len(merge_payload),
                entryCount=len(entries),
            ),
            timeout_ms=60_000,
        )
        return {
            "mode": "pull", "batches": 1,
            "segments": producer.segment_count,
            "payloadBytes": len(merge_payload), "fallback": 0,
        }
    except Exception as exc:  # noqa: BLE001
        print(
            f"catalog_sync merge_pull warning repo={repo_node} "
            f"error={str(exc).replace(' ', '_')}",
            flush=True,
        )
        batches = catalog_merge_batches(
            entries, source_status, max_request_bytes)
        if not batches:
            batches = [[]]
        for batch in batches:
            request_repo(
                user,
                repo_node,
                encode_repo_request(
                    "CATALOG_MERGE", entries=batch,
                    sourceStatus=source_status),
            )
        return {
            "mode": "fallback", "batches": len(batches),
            "segments": producer.segment_count,
            "payloadBytes": len(merge_payload), "fallback": 1,
        }
    finally:
        producer.stop()


def parse_ack_payload(payload: bytes) -> dict[str, str]:
    fields: dict[str, str] = {}
    for item in payload.decode(errors="replace").split(";"):
        if not item or "=" not in item:
            continue
        key, value = item.split("=", 1)
        fields[key] = value
    return fields


def request_repo(
    user: ServiceUser,
    repo_node: str,
    payload: bytes,
    *,
    timeout_ms: int = 15000,
) -> dict:
    def selector(candidates: list[AckCandidate]) -> list[str]:
        for candidate in candidates:
            fields = parse_ack_payload(candidate.payload)
            if fields.get("repoNode") == repo_node:
                return [candidate.provider_name]
        return []

    response = user.request_service_select(
        REPO_SERVICE,
        payload,
        selector,
        ack_timeout_ms=1000,
        timeout_ms=timeout_ms,
        request_strategy="all-selected",
    )
    if not response.status:
        raise RuntimeError(response.error)
    decoded = json.loads(response.payload.decode())
    if not isinstance(decoded, dict):
        raise ValueError("repo catalog response must be a JSON object")
    return decoded


def catalog_merge_batches(
    entries: list[dict],
    source_status: dict,
    max_request_bytes: int,
) -> list[list[dict]]:
    batches: list[list[dict]] = []
    current: list[dict] = []
    for entry in entries:
        tentative = current + [entry]
        encoded = encode_repo_request(
            "CATALOG_MERGE",
            entries=tentative,
            sourceStatus=source_status,
        )
        if current and len(encoded) > max_request_bytes:
            batches.append(current)
            current = [entry]
        else:
            current = tentative
    if current:
        batches.append(current)
    return batches


def config_auto_repair(config_path: str) -> bool:
    try:
        import yaml  # type: ignore
    except ImportError:
        return False
    try:
        loaded = yaml.safe_load(Path(config_path).read_text(encoding="utf-8"))
    except Exception:
        return False
    if not isinstance(loaded, dict):
        return False
    control = loaded.get("repo_control_plane", {})
    if not isinstance(control, dict):
        return False
    repair = control.get("repair", {})
    if not isinstance(repair, dict):
        return False
    return bool(repair.get("auto_execute", False))


def maybe_repair(
    user: ServiceUser,
    repo_node: str,
    *,
    auto_repair: bool,
    repair_repo: DistributedRepo | None,
    object_names: set[str] | None = None,
) -> None:
    objects: list[dict] = []
    if object_names:
        for object_name in sorted(object_names):
            lookup = request_repo(
                user,
                repo_node,
                encode_repo_request("CATALOG_LOOKUP", objectName=object_name),
                timeout_ms=30000,
            )
            objects.append(lookup)
    else:
        snapshot = request_repo(
            user,
            repo_node,
            encode_repo_request("CATALOG_SNAPSHOT"),
            timeout_ms=30000,
        )
        objects = [
            obj for obj in snapshot.get("objects", [])
            if isinstance(obj, dict)
        ]
    for obj in objects:
        if not isinstance(obj, dict):
            continue
        repair_plan = obj.get("repairPlan", {})
        if not isinstance(repair_plan, dict) or not repair_plan.get("needed"):
            continue
        actions = repair_plan.get("actions", [])
        if not isinstance(actions, list):
            continue
        for action in actions:
            if not isinstance(action, dict):
                continue
            try:
                repair_action = RepoRepairAction.from_dict(action)
            except ValueError as exc:
                print(
                    f"catalog_sync repair action invalid repo={repo_node}: {exc}",
                    flush=True,
                )
                continue
            action = repair_action.to_dict()
            if repair_action.target_repo != repo_node:
                continue
            key = (
                repair_action.object_name,
                repair_action.source_repo,
                repair_action.target_repo,
            )
            if not auto_repair:
                print(
                    f"catalog_sync repair warning repo={repo_node} "
                    f"actionType={repair_action.action_type} "
                    f"object={key[0]} source={key[1]} target={key[2]}",
                    flush=True,
                )
                continue
            if repair_repo is None:
                raise RuntimeError("auto repair requested without repair client")
            result = repair_repo.catalog_repair(repo_node, action)
            print(
                f"catalog_sync repaired repo={repo_node} "
                f"object={result.get('objectName', key[0])} "
                f"source={result.get('sourceRepo', key[1])}",
                flush=True,
            )


def process_durable_repair_jobs(
    user: ServiceUser,
    repo_node: str,
    *,
    auto_repair: bool,
    repair_repo: DistributedRepo | None,
    max_jobs: int = 4,
    repair_workers: int = 1,
    cycle_phase: str = "repair",
) -> dict[str, object]:
    cycle_started = time.monotonic()
    scan_started = time.monotonic()
    scan = (
        repair_repo.repair_scan(repo_node)
        if auto_repair and repair_repo is not None else
        request_repo(user, repo_node, encode_repo_request("REPAIR_SCAN"))
    )
    scan_ms = (time.monotonic() - scan_started) * 1000.0
    if scan.get("createdCount", 0):
        print(
            f"catalog_sync repair jobs repo={repo_node} "
            f"created={scan.get('createdCount', 0)}",
            flush=True,
        )
    if not auto_repair:
        return {
            "phase": cycle_phase,
            "created": int(scan.get("createdCount", 0) or 0),
            "claimable": int(scan.get("claimableCount", 0) or 0),
            "claimed": 0,
            "completed": 0,
            "failed": 0,
            "scanMs": scan_ms,
            "claimMs": 0.0,
            "transferMs": 0.0,
            "cycleMs": (time.monotonic() - cycle_started) * 1000.0,
        }
    if repair_repo is None:
        raise RuntimeError("auto repair requested without repair client")
    workers = max(1, min(8, int(repair_workers)))
    job_limit = max(workers, int(max_jobs))
    jobs: list[dict] = []
    claim_started = time.monotonic()
    for _ in range(job_limit):
        claimed = repair_repo.repair_claim(
            repo_node,
            lease_owner=f"catalog-sidecar:{repo_node}",
            lease_ms=60_000,
        )
        job = claimed.get("job")
        if not isinstance(job, dict):
            break
        jobs.append(job)
    claim_ms = (time.monotonic() - claim_started) * 1000.0

    def transfer(job: dict) -> tuple[dict, dict | None, str, float]:
        action = job.get("action", {})
        started = time.monotonic()
        try:
            if not isinstance(action, dict):
                raise ValueError("durable repair job has no action")
            result = repair_repo.catalog_repair(repo_node, action)
            return job, result, "", (time.monotonic() - started) * 1000.0
        except Exception as exc:
            return job, None, str(exc), (time.monotonic() - started) * 1000.0

    completed = 0
    failed = 0
    transfer_started = time.monotonic()
    with ThreadPoolExecutor(
            max_workers=workers,
            thread_name_prefix="ndnsf-repo-repair") as executor:
        futures = [executor.submit(transfer, job) for job in jobs]
        for future in as_completed(futures):
            job, result, error, duration_ms = future.result()
            repair_id = str(job.get("repairId", ""))
            if not error and result is not None:
                try:
                    repair_repo.repair_complete(
                        repo_node, repair_id=repair_id, result=result)
                    completed += 1
                    print(
                        f"catalog_sync repaired repo={repo_node} "
                        f"object={result.get('objectName', job.get('objectName', ''))} "
                        f"source={result.get('sourceRepo', job.get('sourceRepo', ''))} "
                        f"durationMs={duration_ms:.3f} workers={workers} "
                        f"timestampMs={int(time.time() * 1000)}",
                        flush=True,
                    )
                    continue
                except Exception as exc:  # noqa: BLE001
                    error = f"repair-complete-control-failed: {exc}"
            try:
                repair_repo.repair_fail(
                    repo_node, repair_id=repair_id, error=error)
            except Exception as fail_exc:  # noqa: BLE001
                error = f"{error}; repair-fail-control-failed: {fail_exc}"
            failed += 1
            print(
                f"catalog_sync repair failed repo={repo_node} "
                f"repairId={repair_id} durationMs={duration_ms:.3f} "
                f"workers={workers} error={error}",
                flush=True,
            )
    transfer_ms = (time.monotonic() - transfer_started) * 1000.0
    metrics: dict[str, object] = {
        "repo": repo_node,
        "phase": cycle_phase,
        "created": int(scan.get("createdCount", 0) or 0),
        "jobCount": int(scan.get("jobCount", 0) or 0),
        "claimable": int(scan.get("claimableCount", 0) or 0),
        "claimed": len(jobs),
        "completed": completed,
        "failed": failed,
        "scanMs": scan_ms,
        "claimMs": claim_ms,
        "transferMs": transfer_ms,
        "cycleMs": (time.monotonic() - cycle_started) * 1000.0,
        "workers": workers,
        "timestampMs": int(time.time() * 1000),
    }
    print(
        "catalog_sync repair_cycle " + " ".join(
            f"{key}={value:.3f}" if isinstance(value, float) else f"{key}={value}"
            for key, value in metrics.items()
        ),
        flush=True,
    )
    return metrics


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-distributed-repo-generic-policy")
    parser.add_argument("--repo-node", required=True)
    parser.add_argument("--peer-repo-node", action="append", default=[])
    parser.add_argument("--interval-s", type=float, default=10.0)
    parser.add_argument("--auto-repair", dest="auto_repair",
                        action="store_true", default=None)
    parser.add_argument("--no-auto-repair", dest="auto_repair",
                        action="store_false")
    parser.add_argument("--repair-object-name", action="append", default=[],
                        help="If set, only check/execute repair for this object")
    parser.add_argument("--repair-workers", type=int, default=1)
    parser.add_argument("--repair-max-jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.repair_workers <= 8:
        raise ValueError("--repair-workers must be between 1 and 8")
    if args.repair_max_jobs < args.repair_workers:
        raise ValueError("--repair-max-jobs must be >= --repair-workers")

    deployment = APPDeployment.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    ).deployment
    user = ServiceUser(
        group=deployment.group,
        controller=deployment.controller,
        user=args.repo_node,
        trust_schema=deployment.trust_schema,
        permission_wait_ms=6000,
        adaptive_admission=False,
    )
    peer_epochs: dict[str, int] = {}
    interval = max(0.5, args.interval_s)
    auto_repair = (
        config_auto_repair(args.config)
        if args.auto_repair is None else bool(args.auto_repair)
    )
    repair_repo = None
    if auto_repair:
        repair_repo = DistributedRepo(NetworkDistributedRepoClient(
            user=user,
            service_name=REPO_SERVICE,
            upload_prefix=f"{args.repo_node}/NDNSF-DISTRIBUTED-REPO/UPLOAD",
            ack_timeout_ms=1000,
            timeout_ms=60000,
        ))
    print(
        f"catalog_sync ready repo={args.repo_node} peers={args.peer_repo_node} "
        f"autoRepair={auto_repair}",
        flush=True,
    )
    repair_filter = {
        str(name) for name in args.repair_object_name
        if str(name)
    }
    while True:
        merged_object_names: set[str] = set()
        # Local durable work must not wait behind a dead peer's network
        # timeout. Scan before anti-entropy, then scan again after merges.
        if auto_repair:
            try:
                process_durable_repair_jobs(
                    user,
                    args.repo_node,
                    auto_repair=True,
                    repair_repo=repair_repo,
                    max_jobs=args.repair_max_jobs,
                    repair_workers=args.repair_workers,
                    cycle_phase="pre-merge",
                )
            except Exception as exc:  # noqa: BLE001
                print(
                    f"catalog_sync repair-check warning repo={args.repo_node}: {exc}",
                    flush=True,
                )
        for peer in args.peer_repo_node:
            since = peer_epochs.get(peer, 0)
            merge_started = time.monotonic()
            try:
                delta = request_repo(
                    user,
                    peer,
                    encode_repo_request("CATALOG_DELTA", sinceEpoch=since),
                    timeout_ms=5000,
                )
                entries = delta.get("entries", [])
                merge_entries = [
                    entry for entry in entries
                    if isinstance(entry, dict)
                ]
                source_status = delta.get("repoStatus", {})
                if not isinstance(source_status, dict):
                    source_status = {}
                merge_result = merge_catalog_delta(
                    user, args.repo_node, merge_entries, source_status)
                peer_epochs[peer] = int(delta.get("catalogEpoch", since))
                if entries:
                    for entry in entries:
                        if isinstance(entry, dict):
                            object_name = str(entry.get("objectName", ""))
                            if object_name:
                                merged_object_names.add(object_name)
                    print(
                        f"catalog_sync merged repo={args.repo_node} "
                        f"peer={peer} entries={len(entries)} "
                        f"mode={merge_result['mode']} "
                        f"batches={merge_result['batches']} "
                        f"segments={merge_result['segments']} "
                        f"payloadBytes={merge_result['payloadBytes']} "
                        f"fallback={merge_result['fallback']} "
                        f"durationMs={(time.monotonic() - merge_started) * 1000.0:.3f} "
                        f"timestampMs={int(time.time() * 1000)}",
                        flush=True,
                    )
            except Exception as exc:  # noqa: BLE001
                print(
                    f"catalog_sync warning repo={args.repo_node} peer={peer} "
                    f"durationMs={(time.monotonic() - merge_started) * 1000.0:.3f}: {exc}",
                    flush=True,
                )
        try:
            process_durable_repair_jobs(
                user,
                args.repo_node,
                auto_repair=auto_repair,
                repair_repo=repair_repo,
                max_jobs=args.repair_max_jobs,
                repair_workers=args.repair_workers,
                cycle_phase="post-merge",
            )
            if not auto_repair:
                maybe_repair(
                    user,
                    args.repo_node,
                    auto_repair=False,
                    repair_repo=None,
                    object_names=(repair_filter or merged_object_names or None),
                )
        except Exception as exc:  # noqa: BLE001
            print(
                f"catalog_sync repair-check warning repo={args.repo_node}: {exc}",
                flush=True,
            )
        time.sleep(interval)


if __name__ == "__main__":
    raise SystemExit(main())
