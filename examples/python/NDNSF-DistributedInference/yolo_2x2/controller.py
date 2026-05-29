#!/usr/bin/env python3
"""Run the NDNSF controller for the YOLO 2x2 + DistributedRepo example."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
from pathlib import Path

from ndnsf import ServiceUser
from ndnsf_distributed_inference import (
    APPController,
    APPDeployment,
    NetworkDistributedRepoClient,
)
from yolo_2x2_lib import REPO_SERVICE, ROLES, SERVICE, build_runner_script


CONFIG_FILE = "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-yolo-2x2-policy")
    parser.add_argument("--deploy-to-repo-manifest", default="")
    parser.add_argument("--deploy-only", action="store_true",
                        help="Upload controller-owned artifacts to repo and exit")
    parser.add_argument("--replication-factor", type=int, default=1)
    args = parser.parse_args()
    if args.deploy_only:
        if not args.deploy_to_repo_manifest:
            raise ValueError("--deploy-only requires --deploy-to-repo-manifest")
        _deploy_artifacts_to_repo(
            args.config,
            args.generated_policy_dir,
            args.deploy_to_repo_manifest,
            args.replication_factor,
        )
        print("YOLO_2X2_CONTROLLER_REPO_DEPLOYED", args.deploy_to_repo_manifest, flush=True)
        return 0

    controller = APPController.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    )
    if not args.deploy_to_repo_manifest:
        return controller.run()

    controller._controller.start_background()
    try:
        _deploy_artifacts_to_repo(
            args.config,
            args.generated_policy_dir,
            args.deploy_to_repo_manifest,
            args.replication_factor,
        )
        print("YOLO_2X2_CONTROLLER_REPO_DEPLOYED", args.deploy_to_repo_manifest, flush=True)
        while True:
            time.sleep(3600)
    finally:
        controller.stop()


def _deploy_artifacts_to_repo(config: str, generated_policy_dir: str,
                              manifest_path: str, replication_factor: int) -> None:
    deployment = APPDeployment.from_config(
        config,
        generated_policy_dir=generated_policy_dir,
    ).deployment
    user = ServiceUser(
        group=deployment.group,
        controller=deployment.controller,
        user=deployment.controller,
        trust_schema=deployment.trust_schema,
        permission_wait_ms=6000,
        adaptive_admission=False,
    )
    repo = NetworkDistributedRepoClient(
        user=user,
        service_name=REPO_SERVICE,
        upload_prefix=f"{deployment.controller}/NDNSF-DISTRIBUTED-REPO/UPLOAD",
        ack_timeout_ms=1500,
        timeout_ms=60000,
        verbose=True,
    )
    repo.wait_until_ready(60.0)
    service = deployment.service_policy(SERVICE)
    artifacts = {artifact.role: artifact for artifact in service.artifacts}
    runner_payload = build_runner_script()
    runner_hash = hashlib.sha256(runner_payload).hexdigest()[:16]
    manifests = {"roles": {}}
    for role in ROLES:
        artifact = artifacts[role]
        model_payload = Path(artifact.path).read_bytes()
        model_object = repo.publisher_object_name(
            "NDNSF-DI/ARTIFACT/AI/YOLO/2x2" + role + "/model"
        )
        runner_object = repo.publisher_object_name(
            "NDNSF-DI/RUNTIME/AI/YOLO/2x2/runner/" + runner_hash
        )
        model_manifest = repo.store_object(
            object_name=model_object,
            payload=model_payload,
            object_type=artifact.kind or "model",
            replication_factor=replication_factor,
            policy_epoch="/Policy/yolo-2x2/v1",
        )
        runner_manifest = repo.store_object(
            object_name=runner_object,
            payload=runner_payload,
            object_type="runtime-script",
            replication_factor=replication_factor,
            policy_epoch="/Policy/yolo-2x2/v1",
        )
        manifests["roles"][role] = {
            "model": model_manifest.to_dict(),
            "runner": runner_manifest.to_dict(),
        }
        print("YOLO_2X2_CONTROLLER_REPO_OBJECT", role, model_object, flush=True)
    target = Path(manifest_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(manifests, indent=2, sort_keys=True), encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
