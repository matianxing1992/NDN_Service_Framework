"""Publish immutable Qwen stage artifacts through an injected Repo backend.

The caller owns the NDNSF ``ServiceUser`` behind ``backend``.  This is
intentional: request-scoped publication must remain on the same durable user
session as the inference Request and final Selection.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import time
from typing import Any

from py_repoclient import ArtifactRepositoryApi

from ...app_sdk.canonical_artifacts import canonical_layer_name


def publish_qwen_stage_manifest(
    *,
    backend: Any,
    stage_manifest_path: str | Path,
    output_path: str | Path,
    publisher_identity: str,
    object_prefix: str,
    deadline_ms: int,
    chunk_mib: int = 16,
    replication_factor: int = 1,
    control_mode: str = "normal",
    canonical_model_name: str = "",
    canonical_graph_digest: str = "",
    canonical_profile: str = "",
    canonical_publisher: str = "",
    canonical_role_kind: str = "PIPELINE_RANGE",
) -> dict[str, Any]:
    """Publish all stages and persist their committed Repo registration."""
    if int(chunk_mib) < 1 or int(chunk_mib) > 64:
        raise ValueError("chunk_mib must be between 1 and 64")
    if int(replication_factor) < 1:
        raise ValueError("replication_factor must be positive")
    if str(control_mode) != "normal":
        raise ValueError(
            "cold artifact publication requires normal Collaboration control")

    manifest_path = Path(stage_manifest_path)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    stages = list(manifest.get("stages", ()))
    if len(stages) != 3:
        raise RuntimeError("Qwen Repo registration requires exactly three stages")
    registration_path = Path(output_path)
    if registration_path.exists():
        raise FileExistsError(registration_path)

    def remaining_ms() -> int:
        remaining = int(deadline_ms) - int(time.time() * 1000)
        if remaining <= 0:
            raise TimeoutError("Qwen Repo publication deadline expired")
        return remaining

    repo = ArtifactRepositoryApi(
        backend,
        publisher_identity=str(publisher_identity),
        default_timeout_ms=max(1000, remaining_ms()),
    )
    use_canonical_namespace = bool(
        canonical_model_name and canonical_graph_digest and canonical_profile)
    if use_canonical_namespace and not canonical_publisher:
        canonical_publisher = "/ndnsf-di"
    artifacts = []
    for stage in stages:
        role = str(stage["role"])
        path = Path(stage["path"])
        digest = str(stage["sha256"])
        if digest.startswith("sha256:"):
            digest = digest[7:]
        size = int(stage["bytes"])
        legacy_object_name = (
            f"{str(object_prefix).rstrip('/')}/"
            f"stage-{int(stage['stageIndex'])}-{digest}"
        )
        object_name = legacy_object_name
        canonical_name = ""
        if use_canonical_namespace:
            layer_range = dict(stage.get("layerRange") or {})
            if not layer_range:
                ranges = list(manifest.get("layerRanges", ()))
                index = int(stage["stageIndex"])
                if index >= len(ranges) or len(ranges[index]) != 2:
                    raise ValueError(
                        f"stage {index} has no canonical layer range")
                layer_begin, layer_end = (int(ranges[index][0]),
                                          int(ranges[index][1]))
            else:
                layer_begin = int(layer_range["start"])
                layer_end = int(layer_range["endExclusive"])
            recipe_digest = "sha256:" + hashlib.sha256(json.dumps({
                "modelDigest": str(manifest["modelDigest"]),
                "graphDigest": str(canonical_graph_digest),
                "role": role,
                "roleKind": str(canonical_role_kind),
                "layerBegin": layer_begin,
                "layerEnd": layer_end,
                "profile": str(canonical_profile),
                "runtime": str(manifest.get("runtime", "")),
                "dtype": str(manifest.get("dtype", manifest.get("modelProfile", ""))),
            }, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            canonical_name = canonical_layer_name(
                publisher=str(canonical_publisher),
                model_name=str(canonical_model_name),
                model_digest=str(manifest["modelDigest"]),
                profile=str(canonical_profile),
                graph_digest=str(canonical_graph_digest),
                role_kind=str(canonical_role_kind),
                layer_begin=layer_begin,
                layer_end=layer_end,
                rank=0,
                recipe_digest=recipe_digest,
                object_digest="sha256:" + digest,
                segment_number=0,
            )
            # The canonical logical name is the Repo identity.  The receipt's
            # dataName remains a transport-serving name and is retained below
            # for fetch diagnostics; it must never replace the content-bound
            # identity carried in Selection.
            object_name = canonical_name
        started = time.perf_counter()
        result = repo.publish_file(
            path,
            name=object_name,
            expected_sha256=digest,
            replicas=int(replication_factor),
            policy_epoch=str(manifest["modelDigest"]),
            idempotency_key=(
                f"qwen36:{manifest['modelDigest']}:"
                f"{int(stage['stageIndex'])}:{digest}"
            ),
            timeout_ms=max(1000, remaining_ms()),
        )
        receipts = [dict(value) for value in backend.last_receipts]
        if len(receipts) != result.achieved_replicas:
            raise RuntimeError(
                "artifact collaboration receipt count does not match durability")
        data_names = [str(item.get("dataName", "")) for item in receipts]
        if not data_names or any(not value for value in data_names):
            raise RuntimeError("artifact receipt is missing committed Data name")
        artifacts.append({
            "role": role,
            "stageIndex": int(stage["stageIndex"]),
            "fileSha256": "sha256:" + digest,
            "fileBytes": size,
            "canonicalName": canonical_name,
            "objectName": data_names[0],
            "artifactReference": result.reference.to_dict(),
            "operationId": result.operation_id,
            "requestedReplicas": result.requested_replicas,
            "achievedReplicas": result.achieved_replicas,
            "receipts": receipts,
            "publishMs": (time.perf_counter() - started) * 1000.0,
        })
        print(
            "QWEN_REPO_STAGE_REGISTERED",
            f"role={role}",
            f"canonicalName={canonical_name or object_name}",
            f"dataName={data_names[0]}",
            f"bytes={size}",
            flush=True,
        )

    registration = {
        "schemaVersion": "ndnsf-di-qwen36-repo-registration-v1",
        "stageManifestSha256": "sha256:" + hashlib.sha256(
            manifest_path.read_bytes()).hexdigest(),
        "modelDigest": manifest["modelDigest"],
        "revision": manifest["revision"],
        "publisher": str(publisher_identity),
        "repositoryReadiness": {
            "service": "/NDNSF/DistributedRepo/Artifact/v2/STORE",
            "control": "begin_collaboration->ACK_CLOSED->commit_plan",
            "reservation": "none",
        },
        "chunkMiB": int(chunk_mib),
        "controlMode": str(control_mode),
        "replicationFactor": int(replication_factor),
        "artifacts": artifacts,
        "completedAtUnixMs": int(time.time() * 1000),
    }
    registration_path.parent.mkdir(parents=True, exist_ok=True)
    with registration_path.open("x", encoding="utf-8") as output:
        json.dump(registration, output, indent=2, sort_keys=True)
        output.write("\n")
    return registration


__all__ = ["publish_qwen_stage_manifest"]
