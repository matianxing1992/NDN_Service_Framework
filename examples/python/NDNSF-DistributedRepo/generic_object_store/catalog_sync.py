#!/usr/bin/env python3
"""Periodic catalog-delta sidecar for the generic DistributedRepo example."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import time

from ndnsf import AckCandidate, ServiceUser
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
    executed_actions: set[tuple[str, str, str]],
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
            if key in executed_actions:
                continue
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
            executed_actions.add(key)
            print(
                f"catalog_sync repaired repo={repo_node} "
                f"object={result.get('objectName', key[0])} "
                f"source={result.get('sourceRepo', key[1])}",
                flush=True,
            )


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
    args = parser.parse_args()

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
    executed_actions: set[tuple[str, str, str]] = set()
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
        merged_any = False
        merged_object_names: set[str] = set()
        for peer in args.peer_repo_node:
            since = peer_epochs.get(peer, 0)
            try:
                delta = request_repo(
                    user,
                    peer,
                    encode_repo_request("CATALOG_DELTA", sinceEpoch=since),
                )
                entries = delta.get("entries", [])
                merge_entries = [
                    entry for entry in entries
                    if isinstance(entry, dict)
                ]
                source_status = delta.get("repoStatus", {})
                if not isinstance(source_status, dict):
                    source_status = {}
                for batch in catalog_merge_batches(
                    merge_entries,
                    source_status,
                    CATALOG_MERGE_MAX_REQUEST_BYTES,
                ):
                    request_repo(
                        user,
                        args.repo_node,
                        encode_repo_request(
                            "CATALOG_MERGE",
                            entries=batch,
                            sourceStatus=source_status,
                        ),
                    )
                peer_epochs[peer] = int(delta.get("catalogEpoch", since))
                if entries:
                    merged_any = True
                    for entry in entries:
                        if isinstance(entry, dict):
                            object_name = str(entry.get("objectName", ""))
                            if object_name:
                                merged_object_names.add(object_name)
                    print(
                        f"catalog_sync merged repo={args.repo_node} "
                        f"peer={peer} entries={len(entries)}",
                        flush=True,
                    )
            except Exception as exc:  # noqa: BLE001
                print(
                    f"catalog_sync warning repo={args.repo_node} peer={peer}: {exc}",
                    flush=True,
                )
        if merged_any:
            try:
                maybe_repair(
                    user,
                    args.repo_node,
                    auto_repair=auto_repair,
                    repair_repo=repair_repo,
                    executed_actions=executed_actions,
                    object_names=(repair_filter or merged_object_names),
                )
            except Exception as exc:  # noqa: BLE001
                print(
                    f"catalog_sync repair-check warning repo={args.repo_node}: {exc}",
                    flush=True,
                )
        time.sleep(interval)


if __name__ == "__main__":
    raise SystemExit(main())
