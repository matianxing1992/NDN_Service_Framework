#!/usr/bin/env python3
"""Launch the maintained native Persistent Repo for the YOLO example."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil

from ndnsf_distributed_inference.app_sdk import APPDeployment

CONFIG_FILE = "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir", default="/tmp/ndnsf-di-yolo-2x2-policy")
    parser.add_argument("--provider-id", required=True)
    parser.add_argument("--repo-node", required=True)
    parser.add_argument("--storage-dir", required=True)
    parser.add_argument("--memory-cache-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--failure-domain", default="")
    parser.add_argument("--handler-threads", type=int, default=1)
    parser.add_argument("--ack-threads", type=int, default=1)
    parser.add_argument("--server-binary", default="DistributedRepoServer")
    args = parser.parse_args()
    deployment = APPDeployment.from_config(
        args.config, generated_policy_dir=args.generated_policy_dir).deployment
    binary = shutil.which(args.server_binary)
    if binary is None:
        raise RuntimeError("installed native DistributedRepoServer is unavailable")
    expected = os.environ.get("NDNSF_REPO_SERVER_SHA256", "")
    if expected:
        with open(binary, "rb") as stream:
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != expected:
            raise RuntimeError("installed native DistributedRepoServer differs from candidate")
    storage = Path(args.storage_dir).expanduser().resolve()
    os.environ["NDNSF_HANDLER_THREADS"] = str(args.handler_threads)
    os.environ["NDNSF_ACK_THREADS"] = str(args.ack_threads)
    command = [binary,
        "--repo-node", args.repo_node,
        "--database", str(storage / "repo.sqlite3"),
        "--group-prefix", deployment.group,
        "--provider-identity", deployment.provider_prefix.rstrip("/") + "/" + args.provider_id,
        "--authority-identity", deployment.controller,
        "--trust-schema", str(Path(deployment.trust_schema).resolve()),
        "--cache-bytes", str(args.memory_cache_bytes),
        "--failure-domain", args.failure_domain]
    os.execv(binary, command)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
