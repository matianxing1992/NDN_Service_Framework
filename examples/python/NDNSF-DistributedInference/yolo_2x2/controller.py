#!/usr/bin/env python3
"""Run the NDNSF controller for the YOLO layout + DistributedRepo example."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import time
from pathlib import Path

from ndnsf import ServiceUser
from ndnsf_distributed_inference.app_sdk.controller import APPController
from ndnsf_distributed_inference.app_sdk import (
    APPDeployment,
)
from ndnsf_distributed_inference.repo_reference import repo_artifact_reference
from py_repoclient.orchestration import NetworkDistributedRepoClient
from yolo_2x2_lib import REPO_SERVICE, build_runner_script, yolo_inference_service


CONFIG_FILE = "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"
_SPEC180_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_SPEC180_CASES = {"Y-A", "Y-B", "Y-N"}


def _decode_spec180_publication(document: object) -> tuple[str, str, bytes, list[dict]]:
    """Validate and decode one candidate-bound publication batch.

    The publication file is produced by the runner, but it crosses a process
    boundary before the Controller signs anything. Treat it as untrusted input:
    a matching catalogue digest alone is insufficient if an artifact name or
    payload was replaced after the runner created the file.
    """
    if not isinstance(document, dict):
        raise RuntimeError("Spec180 runtime publication must be an object")
    if document.get("schema") != "spec180-runtime-publication-v1":
        raise RuntimeError("unsupported Spec180 runtime publication schema")
    case = str(document.get("case", ""))
    if case not in _SPEC180_CASES:
        raise RuntimeError("invalid Spec180 runtime publication case")
    data_name = str(document.get("catalogueDataName", ""))
    signer = str(document.get("catalogueSigner", ""))
    if (not data_name.startswith("/") or not signer.startswith("/")
            or "//" in data_name or "//" in signer
            or "/.." in data_name or "/.." in signer):
        raise RuntimeError("invalid Spec180 catalogue name or signer")
    di_prefix = signer.rstrip("/") + "/NDNSF/DI/"
    if not data_name.startswith(di_prefix):
        raise RuntimeError("Spec180 catalogue is outside signer DI namespace")
    catalogue_b64 = document.get("cataloguePayloadB64")
    if not isinstance(catalogue_b64, str) or not catalogue_b64:
        raise RuntimeError("missing Spec180 catalogue payload")
    try:
        catalogue_payload = base64.b64decode(catalogue_b64, validate=True)
    except (ValueError, TypeError) as exc:
        raise RuntimeError("invalid Spec180 catalogue payload encoding") from exc
    catalogue_digest = str(document.get("cataloguePayloadDigest", ""))
    if (not _SPEC180_DIGEST_RE.fullmatch(catalogue_digest)
            or "sha256:" + hashlib.sha256(catalogue_payload).hexdigest()
            != catalogue_digest):
        raise RuntimeError("Spec180 catalogue payload digest mismatch")
    manifest_digest = str(document.get("packageManifestSha256", ""))
    if not _SPEC180_DIGEST_RE.fullmatch(manifest_digest):
        raise RuntimeError("invalid Spec180 package manifest digest")
    raw_artifacts = document.get("artifacts")
    if not isinstance(raw_artifacts, list) or not raw_artifacts:
        raise RuntimeError("incomplete Spec180 runtime publication")
    artifact_prefix = signer.rstrip("/") + "/NDNSF/DI/ARTIFACT/"
    artifacts: list[dict] = []
    seen: set[str] = set()
    for item in raw_artifacts:
        if not isinstance(item, dict):
            raise RuntimeError("malformed Spec180 artifact publication")
        name = str(item.get("dataName", ""))
        if (not name.startswith(artifact_prefix) or name in seen
                or "//" in name or "/.." in name):
            raise RuntimeError("invalid or duplicate Spec180 artifact name")
        payload_b64 = item.get("payloadB64")
        if not isinstance(payload_b64, str) or not payload_b64:
            raise RuntimeError("missing Spec180 artifact payload")
        try:
            payload = base64.b64decode(payload_b64, validate=True)
        except (ValueError, TypeError) as exc:
            raise RuntimeError("invalid Spec180 artifact payload encoding") from exc
        if len(payload) > 1024 * 1024:
            raise RuntimeError("Spec180 artifact metadata is oversized")
        payload_digest = str(item.get("payloadDigest", ""))
        expected_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
        if (not _SPEC180_DIGEST_RE.fullmatch(payload_digest)
                or payload_digest != expected_digest):
            raise RuntimeError("Spec180 artifact payload digest mismatch")
        seen.add(name)
        artifacts.append({"dataName": name, "payload": payload,
                          "payloadDigest": payload_digest})
    return data_name, signer, catalogue_payload, artifacts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-yolo-2x2-policy")
    parser.add_argument("--deploy-to-repo-manifest", default="")
    parser.add_argument("--deploy-only", action="store_true",
                        help="Upload controller-owned artifacts to repo and exit")
    parser.add_argument("--replication-factor", type=int, default=1)
    parser.add_argument(
        "--spec180-runtime-publication-file", default="",
        help=("Controller-owned Spec180 publication batch. The controller "
              "signs and reads back every APP record before publishing the "
              "readiness marker."),
    )
    parser.add_argument('--spec180-runtime-receipt-file', default='',
                        help='Writable receipt output; defaults beside publication input for legacy runners.')
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
    # Spec180's candidate-bound APP publication is a controller-owned runtime
    # operation, not a repository deployment. Keep it usable without a deploy
    # manifest; otherwise the controller would take the ordinary
    # ``controller.run()`` path and the runner would wait forever for a receipt.
    if (not args.deploy_to_repo_manifest
            and not args.spec180_runtime_publication_file):
        return controller.run()

    controller._controller.start_background()
    # This marker means the controller process has entered its serving loop;
    # the Spec180 runner uses it to release the repository startup barrier.
    # The signed catalogue publication has a separate receipt barrier below.
    print("ServiceController listening", flush=True)
    # Keep the Spec180 fallback marker distinct from the generic C++ startup
    # log above.  The fallback is used only when no publication receipt is
    # configured, and must not be released by a pre-readiness substring.
    print("SPEC180_CONTROLLER_READY", flush=True)
    try:
        if args.spec180_runtime_publication_file:
            # spec181 T005 repair: the readiness probe's PUBPARAMS Data (named
            # /example/controller/PUBPARAMS/readiness/<nonce>/KP-ABE/...) sits
            # in the NFD ContentStore for the NAC-ABE freshness window (5 s).
            # The publication ServiceUser's NAC fetch uses CanBePrefix and
            # matches that cached Data, whose abeType component reads
            # "readiness" instead of KP-ABE — the verifier accepts the
            # signature and the success callback then fails the type parse
            # (masked as "did not invoke" during unwinding).  Wait out the
            # freshness window so the fetch reaches the AA and receives the
            # canonical /PUBPARAMS/KP-ABE Data.
            time.sleep(6.0)
            runtime_publication_user = _publish_spec180_runtime(
                args.config,
                args.generated_policy_dir,
                args.spec180_runtime_publication_file,
                receipt_path=args.spec180_runtime_receipt_file,
            )
            print("SPEC180_RUNTIME_CATALOGUE_PUBLISHED", flush=True)
            # Keep the publishing ServiceUser alive for the whole case: it
            # serves the signed catalogue APP Data that the User fetches by
            # exact name after ACK_CLOSED. Stopping it would close its face
            # and drop the published records from its InMemoryStorage.
            while runtime_publication_user is not None:
                time.sleep(3600)
        if not args.deploy_to_repo_manifest:
            raise RuntimeError(
                "repository deployment manifest is required when runtime publication is absent")
        _deploy_artifacts_to_repo(
            args.config,
            args.generated_policy_dir,
            args.deploy_to_repo_manifest,
            args.replication_factor,
        )
        print("YOLO_2X2_CONTROLLER_REPO_DEPLOYED", args.deploy_to_repo_manifest, flush=True)
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        # SIGINT is the bounded owner shutdown used by MiniNDN.  Treat it as
        # a clean serving-loop exit so the C++ controller is stopped from the
        # normal finally path without surfacing an application traceback.
        pass
    finally:
        controller.stop()


def _publish_spec180_runtime(config: str, generated_policy_dir: str,
                             publication_path: str, *, receipt_path: str = '') -> ServiceUser:
    """Publish and exact-readback the candidate-bound Spec180 APP batch.

    The returned ServiceUser must stay alive for the remainder of the case:
    the User fetches the signed catalogue by exact name after ACK_CLOSED,
    and stopping the publisher would close its face and drop the signed
    APP Data from its InMemoryStorage.
    """
    publication_file = Path(publication_path).expanduser().resolve()
    receipt_output = (Path(receipt_path).expanduser().absolute() if receipt_path else
                      publication_file.with_name("runtime-publication-receipt.json"))
    if (receipt_output == publication_file or '..' in receipt_output.parts
            or any(p.is_symlink() for p in (receipt_output, *receipt_output.parents))
            or not receipt_output.parent.is_dir() or receipt_path and receipt_output.exists()):
        raise RuntimeError('unsafe runtime publication receipt path')
    try:
        document = json.loads(publication_file.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError("invalid Spec180 runtime publication file") from exc
    data_name, signer, catalogue_payload, artifacts = \
        _decode_spec180_publication(document)
    deployment = APPDeployment.from_config(
        config, generated_policy_dir=generated_policy_dir).deployment
    if signer != deployment.controller:
        raise RuntimeError("Spec180 catalogue signer is not controller identity")
    user = ServiceUser(
        group=deployment.group,
        controller=deployment.controller,
        user=deployment.controller,
        trust_schema=deployment.trust_schema,
        permission_wait_ms=6000,
        handler_threads=1,
        ack_threads=1,
        adaptive_admission=False,
    )
    user.start()
    receipts = []
    for item in artifacts:
        name = item["dataName"]
        payload = item["payload"]
        result = user.publish_signed_app_data(name, payload, freshness_ms=600000)
        if not result.success or result.data_name != name:
            raise RuntimeError(f"Spec180 artifact publication failed: {result.error}")
        fetched = user.fetch_signed_app_data(name, deployment.controller,
                                             timeout_ms=5000)
        if (not fetched.success or fetched.data_name != name
                or bytes(fetched.payload) != payload
                or not str(fetched.signer_certificate).startswith(
                    deployment.controller)):
            raise RuntimeError("Spec180 artifact APP readback mismatch")
        receipts.append({"dataName": name,
                         "payloadDigest": item["payloadDigest"]})
    result = user.publish_signed_app_data(
        data_name, catalogue_payload, freshness_ms=600000)
    if not result.success or result.data_name != data_name:
        raise RuntimeError(f"Spec180 catalogue publication failed: {result.error}")
    fetched = user.fetch_signed_app_data(data_name, deployment.controller,
                                         timeout_ms=5000)
    if (not fetched.success or fetched.data_name != data_name
            or bytes(fetched.payload) != catalogue_payload
            or not str(fetched.signer_certificate).startswith(
                deployment.controller)):
        raise RuntimeError("Spec180 catalogue APP readback mismatch")
    receipt = {
        "schema": "spec180-runtime-publication-receipt-v1",
        "catalogueDataName": data_name,
        "catalogueSigner": deployment.controller,
        "cataloguePayloadDigest": "sha256:" + hashlib.sha256(catalogue_payload).hexdigest(),
        "artifacts": receipts,
    }
    with receipt_output.open('x' if receipt_path else 'w', encoding='utf-8') as stream:
        stream.write(json.dumps(receipt, sort_keys=True, indent=2) + "\n")
    return user


def _deploy_artifacts_to_repo(config: str, generated_policy_dir: str,
                              manifest_path: str, replication_factor: int) -> None:
    print("YOLO_2X2_CONTROLLER_REPO_DEPLOY_BEGIN", manifest_path, flush=True)
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
        handler_threads=1,
        ack_threads=1,
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
    print("YOLO_2X2_CONTROLLER_REPO_WAIT_READY", flush=True)
    repo.wait_until_ready(60.0)
    print("YOLO_2X2_CONTROLLER_REPO_READY", flush=True)
    service = deployment.service_policy(yolo_inference_service(deployment))
    artifacts = {artifact.role: artifact for artifact in service.artifacts}
    runner_payload = build_runner_script()
    runner_hash = hashlib.sha256(runner_payload).hexdigest()[:16]
    manifests = {"roles": {}}
    layout = str(service.metadata.get("layout", "2x2"))
    for role in service.roles:
        artifact = artifacts[role]
        model_payload = Path(artifact.path).read_bytes()
        model_object = repo.publisher_object_name(
            "NDNSF-DI/ARTIFACT/AI/YOLO/" + layout + role + "/model"
        )
        runner_object = repo.publisher_object_name(
            "NDNSF-DI/RUNTIME/AI/YOLO/" + layout + "/runner/" + runner_hash
        )
        model_manifest = repo.store_object(
            object_name=model_object,
            payload=model_payload,
            object_type=artifact.kind or "model",
            replication_factor=replication_factor,
            policy_epoch="/Policy/yolo-" + layout + "/v1",
        )
        runner_manifest = repo.store_object(
            object_name=runner_object,
            payload=runner_payload,
            object_type="runtime-script",
            replication_factor=replication_factor,
            policy_epoch="/Policy/yolo-" + layout + "/v1",
        )
        manifests["roles"][role] = {
            "model": repo_artifact_reference(
                model_manifest,
                object_type=artifact.kind or "model",
                object_id=artifact.artifact_name or role + "/model",
            ),
            "runner": repo_artifact_reference(
                runner_manifest,
                object_type="runtime-script",
                object_id="runner/" + runner_hash,
            ),
        }
        print("YOLO_2X2_CONTROLLER_REPO_OBJECT", role, model_object, flush=True)
    target = Path(manifest_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(manifests, indent=2, sort_keys=True), encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
