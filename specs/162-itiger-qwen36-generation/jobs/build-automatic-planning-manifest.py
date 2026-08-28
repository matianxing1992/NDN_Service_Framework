#!/usr/bin/env python3
"""Build the exact Spec 162 deferred-planning manifest from sealed artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import time

from ndnsf_distributed_inference.adapters.qwen import (
    build_qwen_three_stage_adapter,
)


def canonical_digest(value) -> str:
    wire = json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False,
    ).encode()
    return "sha256:" + hashlib.sha256(wire).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage-manifest", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--repository-prefix", required=True)
    parser.add_argument("--repo-registration")
    args = parser.parse_args()

    stage_manifest = json.loads(
        Path(args.stage_manifest).read_text(encoding="utf-8"))
    stages = tuple(stage_manifest["stages"])
    if len(stages) != 3:
        raise RuntimeError("Spec 162 requires exactly three stage artifacts")
    artifact_digests = {
        str(item["role"]): "sha256:" + str(item["sha256"])
        for item in stages
    }
    weight_bytes = {
        str(item["role"]): int(item["bytes"]) for item in stages
    }
    adapter = build_qwen_three_stage_adapter(
        model_name=str(stage_manifest["repository"]),
        revision=str(stage_manifest["revision"]),
        layer_ranges=tuple(
            (int(item[0]), int(item[1]))
            for item in stage_manifest["layerRanges"]
        ),
        artifact_digests_by_role=artifact_digests,
        weight_bytes_by_role=weight_bytes,
        precision=str(stage_manifest["dtype"]),
    )
    semantics_digest = canonical_digest({
        "model": stage_manifest["repository"],
        "revision": stage_manifest["revision"],
        "dtype": stage_manifest["dtype"],
        "quantization": stage_manifest["quantization"],
        "layerRanges": stage_manifest["layerRanges"],
        "tokenizerDigest": stage_manifest["tokenizer"]["digest"],
        "generation": {
            "enableThinking": False,
            "greedy": True,
            "maxNewTokens": 64,
            "useCache": False,
        },
    })
    model = adapter.describe_model(
        stage_manifest["repository"],
        stage_manifest["modelDigest"],
        semantics_digest,
        source_revision=stage_manifest["revision"],
    )
    graph = adapter.graph.inspect(model)
    candidate = adapter.splitter.enumerate_candidates(model, graph)[0]
    prefix = args.repository_prefix.rstrip("/")
    registration = None
    if args.repo_registration:
        registration = json.loads(
            Path(args.repo_registration).read_text(encoding="utf-8"))
        if registration.get("schemaVersion") != (
                "ndnsf-di-qwen36-repo-registration-v1"):
            raise RuntimeError("unsupported DistributedRepo registration schema")
        if registration.get("stageManifestSha256") != (
                "sha256:" + hashlib.sha256(
                    Path(args.stage_manifest).read_bytes()).hexdigest()):
            raise RuntimeError("DistributedRepo registration is not stage-manifest bound")
        registration_by_role = {
            str(item["role"]): item
            for item in registration.get("artifacts", [])
        }
        if len(registration_by_role) != 3:
            raise RuntimeError("DistributedRepo registration requires three roles")
    else:
        registration_by_role = {}
    stage_rows = []
    for item in stages:
        role = str(item["role"])
        digest = artifact_digests[role]
        registration_item = registration_by_role.get(role)
        if registration_item is not None:
            if (registration_item.get("fileSha256") != digest
                    or int(registration_item.get("fileBytes", -1))
                    != int(item["bytes"])
                    or not str(registration_item.get("objectName", ""))):
                raise RuntimeError(
                    f"DistributedRepo registration mismatch for {role}")
            data_name = str(registration_item["objectName"])
        else:
            data_name = f"{prefix}/segments/{digest[7:]}"
        stage_rows.append({
            **item,
            "sha256": digest,
            "dataName": data_name,
            "requiredGpuMiB": (
                candidate.requirements_by_role[role]
                .estimated_peak_gpu_memory_bytes
                + 1024 * 1024 - 1
            ) // (1024 * 1024),
        })
    created_at_ms = int(time.time() * 1000)
    catalog_body = {
        "modelContentDigest": model.content_digest,
        "semanticsDigest": model.semantics_digest,
        "graphDigest": graph.graph_digest,
        "candidateDigest": candidate.candidate_digest,
        "artifacts": [
            {
                "role": item["role"],
                "digest": item["sha256"],
                "dataName": item["dataName"],
                "bytes": item["bytes"],
            }
            for item in stage_rows
        ],
    }
    output = {
        "schemaVersion": "ndnsf-di-spec162-automatic-planning-v1",
        "model": {
            "name": model.model_name,
            "contentDigest": model.content_digest,
            "semanticsDigest": model.semantics_digest,
            "revision": model.source_revision,
        },
        "adapterDescriptorDigest": adapter.descriptor.descriptor_digest,
        "adapterCompositionDigest": adapter.composition_digest,
        "adapterId": adapter.descriptor.name,
        "adapterVersion": adapter.descriptor.version,
        "graphDigest": graph.graph_digest,
        "candidateDigest": candidate.candidate_digest,
        "dtype": stage_manifest["dtype"],
        "layerRanges": stage_manifest["layerRanges"],
        "stages": stage_rows,
        "preSplitCatalog": {
            "alias": (
                str(stage_manifest["repository"])
                .replace("/", "-")
                .lower()
                + "-three-stage"
            ),
            "manifestDigest": canonical_digest(catalog_body),
            "candidateDigest": candidate.candidate_digest,
            "createdAtMs": created_at_ms,
            "publicationState": (
                "ACTIVE" if registration is not None
                else "REQUIRES_DISTRIBUTED_REPO_REGISTRATION"
            ),
            "registrationDigest": (
                canonical_digest(registration)
                if registration is not None else ""
            ),
        },
    }
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(output, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        "SPEC162_AUTOMATIC_PLANNING_MANIFEST_READY",
        f"candidateDigest={candidate.candidate_digest}",
        f"graphDigest={graph.graph_digest}",
        f"output={output_path}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
