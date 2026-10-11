#!/usr/bin/env python3
"""Run the NDNSF controller for the YOLO layout + DistributedRepo example."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import signal
import tempfile
import threading
import time
from pathlib import Path

from ndnsf import (FileSegmentedObjectProducer, SegmentedObjectProducer,
                   ServiceUser, make_content_data_prefix)
from ndn.encoding import Component, Name
from ndnsf_distributed_inference.app_sdk.canonical_artifacts import canonical_model_object_name
from ndnsf_distributed_inference.naming import (
    NamingDigestField, canonical_name_uri, compute_naming_digest,
    make_naming_digest_component,
)
from ndnsf_distributed_inference.app_sdk.controller import APPController
from ndnsf_distributed_inference.app_sdk import (
    APPDeployment,
)
from ndnsf_distributed_inference.app_sdk.contracts import ArtifactReference as DIArtifactReference
from ndnsf_distributed_inference.repo_artifact_resolver import binding_from_native_handle
from py_repoclient import (
    ArtifactCapabilityRequirements,
    ArtifactReference,
    RepoClient,
    RepoWriteConsistency,
    SelectedRepoSet,
    SelectedRepoTarget,
)
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
    parser.add_argument("--deploy-artifact-references", default="")
    parser.add_argument("--deploy-only", action="store_true",
                        help="Upload controller-owned artifacts to repo and exit")
    parser.add_argument("--replication-factor", type=int, default=1)
    parser.add_argument(
        "--spec180-runtime-publication-file", default="",
        help=("Controller-owned Spec180 publication batch. The controller "
              "signs and reads back every APP record before publishing the "
              "readiness marker."),
    )
    args = parser.parse_args()
    if args.deploy_only:
        if not args.deploy_artifact_references:
            raise ValueError("--deploy-only requires --deploy-artifact-references")
        _deploy_artifacts_to_repo(
            args.config,
            args.generated_policy_dir,
            args.deploy_artifact_references,
            args.replication_factor,
        )
        print("YOLO_2X2_CONTROLLER_REPO_DEPLOYED", args.deploy_artifact_references, flush=True)
        return 0

    controller = APPController.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    )
    # Spec180's candidate-bound APP publication is a controller-owned runtime
    # operation, not a repository deployment. Keep it usable without a deploy
    # manifest; otherwise the controller would take the ordinary
    # ``controller.run()`` path and the runner would wait forever for a receipt.
    if (not args.deploy_artifact_references
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
            )
            print("SPEC180_RUNTIME_CATALOGUE_PUBLISHED", flush=True)
            # Keep the publishing ServiceUser alive for the whole case: it
            # serves the signed catalogue APP Data that the User fetches by
            # exact name after ACK_CLOSED. Stopping it would close its face
            # and drop the published records from its InMemoryStorage.
            shutdown = threading.Event()

            def request_shutdown(_signum, _frame):
                # Wake the Python owner so it can stop the native Controller
                # explicitly.  Relying on interpreter teardown leaves the
                # background C++ event loop alive until the MiniNDN harness
                # escalates to SIGKILL, which is a cleanup failure.
                shutdown.set()

            signal.signal(signal.SIGINT, request_shutdown)
            signal.signal(signal.SIGTERM, request_shutdown)
            while runtime_publication_user is not None and not shutdown.wait(3600):
                pass
            if shutdown.is_set():
                controller.stop()
                return 0
        if not args.deploy_artifact_references:
            raise RuntimeError(
                "artifact references are required when runtime publication is absent")
        _deploy_artifacts_to_repo(
            args.config,
            args.generated_policy_dir,
            args.deploy_artifact_references,
            args.replication_factor,
        )
        print("YOLO_2X2_CONTROLLER_REPO_DEPLOYED", args.deploy_artifact_references, flush=True)
        while True:
            time.sleep(3600)
    finally:
        controller.stop()


def _publish_spec180_runtime(config: str, generated_policy_dir: str,
                             publication_path: str) -> ServiceUser:
    """Publish and exact-readback the candidate-bound Spec180 APP batch.

    The returned ServiceUser must stay alive for the remainder of the case:
    the User fetches the signed catalogue by exact name after ACK_CLOSED,
    and stopping the publisher would close its face and drop the signed
    APP Data from its InMemoryStorage.
    """
    publication_file = Path(publication_path).expanduser().resolve()
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
    receipt_path = publication_file.with_name("runtime-publication-receipt.json")
    receipt_path.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n",
                            encoding="utf-8")
    return user


def _store_original_repo_file(repo, *, path: Path, publisher: str,
                             object_name: str, object_type: str,
                             model_name: str = "", model_digest: str = "", label: str,
                             replication_factor: int, policy_epoch: str):
    """Publish immutable file packets once and submit their original wire."""
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    profile = json.dumps({"encoding": "raw", "segmentBytes": 4000,
                          "freshnessMs": 60000}, sort_keys=True, separators=(",", ":"))
    if model_name:
        content = canonical_model_object_name(
            publisher=publisher, model_name=model_name, model_digest=model_digest,
            object_kind="model-file", object_label=label, object_digest="sha256:" + digest)
        representation = compute_naming_digest("ndnsf-naming/representation", [
            NamingDigestField(1, 5, profile.encode("utf-8")),
        ])
        data_name = canonical_name_uri(Name.from_str(content) + [
            Component.from_str("REPRESENTATION"), make_naming_digest_component(representation),
            Component.from_str("DATA"),
        ])
    else:
        data_name = make_content_data_prefix(publisher, "blob", label, digest, profile)
    producer = FileSegmentedObjectProducer(
        data_name, str(path), signing_identity=publisher,
        max_segment_size=4000, freshness_ms=60000)
    packets = [producer.packet(i) for i in range(producer.segment_count)]
    packet_index = json.dumps({
        "objectName": object_name, "dataName": data_name,
        "packets": [{"name": packet.name, "segment": packet.segment,
                     "wireSha256": hashlib.sha256(packet.wire).hexdigest()}
                    for packet in packets],
    }, sort_keys=True).encode("utf-8")
    index_digest = hashlib.sha256(packet_index).hexdigest()
    index_profile = json.dumps({"encoding": "json", "segmentBytes": 6000,
                                "freshnessMs": 60000},
                               sort_keys=True, separators=(",", ":"))
    index_name = make_content_data_prefix(
        publisher, "blob", "original-packet-index", index_digest, index_profile)
    index_producer = SegmentedObjectProducer(
        index_name, packet_index, signing_identity=publisher,
        max_segment_size=6000, freshness_ms=60000)
    try:
        producer.start()
        index_producer.start()
        manifest = repo.store_signed_packets(
            object_name=object_name, packets=packets, object_type=object_type,
            object_size=producer.file_size, object_sha256=digest,
            replication_factor=replication_factor, policy_epoch=policy_epoch,
            data_name=data_name, packet_manifest_name=index_producer.versioned_name,
            packet_manifest_sha256=index_digest)
    finally:
        try:
            index_producer.stop()
        finally:
            producer.stop()
    wire_digest = hashlib.sha256()
    for packet in packets:
        wire_digest.update(packet.wire)
    print("YOLO_ORIGINAL_REPO_PACKETS", json.dumps({
        "object": object_name, "dataPrefix": data_name, "packets": len(packets),
        "versionedName": producer.versioned_name,
        "wireSha256": wire_digest.hexdigest(),
    }, sort_keys=True), flush=True)
    return manifest


def _deploy_artifacts_to_repo(config: str, generated_policy_dir: str,
                              artifact_references_path: str,
                              replication_factor: int) -> None:
    print("YOLO_2X2_CONTROLLER_REPO_DEPLOY_BEGIN",
          artifact_references_path, flush=True)
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
    user.start()
    repo = RepoClient(
        user=user,
        repo_service_name=REPO_SERVICE,
        ack_timeout_ms=1500,
        timeout_ms=60000,
    )
    api = repo.artifact_api

    def publish_binding(source: Path, *, logical_name: str, filename: str,
                        role: str, use: str, object_type: str) -> dict:
        payload_digest = hashlib.sha256(source.read_bytes()).hexdigest()
        reference = ArtifactReference(
            logical_name=logical_name,
            content_digest=payload_digest,
            size_bytes=source.stat().st_size,
            root_manifest_name=logical_name + "/root",
            publisher_identity=canonical_name_uri(Name.from_str(deployment.controller)),
            policy_epoch="/Policy/yolo-2x2/v1",
        )
        requirements = ArtifactCapabilityRequirements()
        requirements.artifact = reference
        candidates = api.discover_repo_candidates(requirements, 60000).candidates
        if len(candidates) < replication_factor:
            raise RuntimeError(
                f"Repo discovery returned {len(candidates)} candidates; "
                f"need {replication_factor}")
        selected = SelectedRepoSet()
        targets = []
        for candidate in candidates[:replication_factor]:
            target = SelectedRepoTarget()
            target.repo_node = candidate.repo_node
            target.provider_identity = candidate.provider_identity
            target.service_prefix = candidate.service_prefix
            targets.append(target)
        selected.targets = targets
        selected.seed_repo_id = selected.targets[0].repo_node
        selected.consistency = RepoWriteConsistency.ALL
        result = repo.publish_file(source, reference, selected, timeout_ms=60000)
        if (result.handle is None or result.receipt is None
                or result.achieved_replicas < replication_factor):
            raise RuntimeError(f"native Repo publish failed for {logical_name}")
        uri = "urn:ndnsf:repo-artifact:" + hashlib.sha256(
            result.handle.to_json().encode("utf-8")).hexdigest()
        projection = DIArtifactReference(
            uri=uri,
            digest="sha256:" + payload_digest,
            size_bytes=source.stat().st_size,
            mount_path=filename,
        )
        binding = binding_from_native_handle(
            result.handle, projection, role=role, use=use)
        return {
            "repoBinding": binding.to_dict(),
            "filename": filename,
            "kind": object_type,
            "metadata": {"kind": object_type},
        }

    service = deployment.service_policy(yolo_inference_service(deployment))
    artifacts = {artifact.role: artifact for artifact in service.artifacts}
    # The split model identity binds the actual complete role/file set; an
    # individual shard's payload digest is only its own content identity.
    model_digest = "sha256:" + compute_naming_digest("ndnsf-naming/yolo-split-model", [
        NamingDigestField(1, 2, service.model_name.encode("utf-8")),
        NamingDigestField(2, 5, json.dumps({
            role: hashlib.sha256(Path(artifact.path).read_bytes()).hexdigest()
            for role, artifact in artifacts.items()
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")),
    ])
    runner_payload = build_runner_script()
    runner_hash = hashlib.sha256(runner_payload).hexdigest()[:16]
    artifact_references = {"roles": {}}
    layout = str(service.metadata.get("layout", "2x2"))
    with tempfile.TemporaryDirectory(prefix="ndnsf-di-yolo-repo-publish-") as temp:
        runner_path = Path(temp) / "runner.py"
        runner_path.write_bytes(runner_payload)
        for role in service.roles:
            artifact = artifacts[role]
            model_path = Path(artifact.path)
            model_name = canonical_model_object_name(
                publisher=deployment.controller, model_name=service.model_name,
                model_digest=model_digest, object_kind="model-file",
                object_label=role + "/model.onnx",
                object_digest="sha256:" + hashlib.sha256(model_path.read_bytes()).hexdigest())
            artifact_references["roles"][role] = {
                "model": publish_binding(
                    model_path,
                    logical_name=model_name,
                    filename=model_path.name,
                    role=role,
                    use="model",
                    object_type=artifact.kind or "onnx-model",
                ),
                "runner": publish_binding(
                    runner_path,
                    logical_name=make_content_data_prefix(
                        deployment.controller, "blob", "yolo-" + layout + "-runner.py",
                        hashlib.sha256(runner_payload).hexdigest(),
                        '{"encoding":"raw"}'),
                    filename="runner.py",
                    role=role,
                    use="runtime",
                    object_type="runtime-script",
                ),
            }
            print("YOLO_2X2_CONTROLLER_REPO_OBJECT", role, model_name, flush=True)
    target = Path(artifact_references_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        json.dumps(artifact_references, indent=2, sort_keys=True), encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
