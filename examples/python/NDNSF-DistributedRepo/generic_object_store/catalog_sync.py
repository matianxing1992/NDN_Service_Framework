#!/usr/bin/env python3
"""Periodic catalog-delta sidecar for the generic DistributedRepo example."""

from __future__ import annotations

import argparse
import json
import time

from ndnsf import AckCandidate, ServiceUser
from ndnsf_distributed_inference import APPDeployment
from ndnsf_distributed_inference.repo import encode_repo_request


CONFIG_FILE = "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_policy.yaml"
REPO_SERVICE = "/NDNSF/DistributedRepo"


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-distributed-repo-generic-policy")
    parser.add_argument("--repo-node", required=True)
    parser.add_argument("--peer-repo-node", action="append", default=[])
    parser.add_argument("--interval-s", type=float, default=10.0)
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
    print(
        f"catalog_sync ready repo={args.repo_node} peers={args.peer_repo_node}",
        flush=True,
    )
    while True:
        for peer in args.peer_repo_node:
            since = peer_epochs.get(peer, 0)
            try:
                delta = request_repo(
                    user,
                    peer,
                    encode_repo_request("CATALOG_DELTA", sinceEpoch=since),
                )
                entries = delta.get("entries", [])
                request_repo(
                    user,
                    args.repo_node,
                    encode_repo_request(
                        "CATALOG_MERGE",
                        entries=entries,
                        sourceStatus=delta.get("repoStatus", {}),
                    ),
                )
                peer_epochs[peer] = int(delta.get("catalogEpoch", since))
                if entries:
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
        time.sleep(interval)


if __name__ == "__main__":
    raise SystemExit(main())
