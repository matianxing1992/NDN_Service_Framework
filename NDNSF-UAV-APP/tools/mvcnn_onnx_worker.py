#!/usr/bin/env python3
"""Strict CPU ONNX Runtime worker for the Spec 178 MVCNN subject.

This module is intentionally separate from the Spec 177 deterministic
functional adapter.  A real-model invocation requires a registered artifact,
exact digest, explicit CPUExecutionProvider, and a bounded 2--6-view input.
There is no download, detector fallback, or per-view voting path here.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import resource
import sys
import time
from typing import Any, Mapping

import numpy as np
from PIL import Image, ImageDraw, ImageOps


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_REGISTRY = ROOT / "NDNSF-UAV-APP/configs/uav_multiview_models.json"
MAX_VIEWS = 6
IMAGE_SIZE = 224
ALGORITHM_ID = "mvcnn-onnx-maxpool/v1"
SCHEMA_RESULT = "ndnsf-uav-multiview-result/v2"


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("expected JSON object: %s" % path)
    return value


def load_profile(registry_path: Path, profile_id: str) -> dict[str, Any]:
    registry = load_json(registry_path)
    for profile in registry.get("profiles", []):
        if profile.get("profile_id") == profile_id:
            if profile.get("algorithm_id") != ALGORITHM_ID:
                raise ValueError("profile is not the Spec 178 MVCNN ONNX profile")
            return profile
    raise ValueError("model profile not found: %s" % profile_id)


def resolve_artifact(profile: Mapping[str, Any], model: str) -> Path:
    candidate = Path(model) if model else ROOT / str(profile.get("model_artifact", ""))
    if not candidate.is_absolute():
        candidate = ROOT / candidate
    candidate = candidate.resolve()
    if not candidate.is_file():
        raise FileNotFoundError("registered ONNX artifact is missing: %s" % candidate)
    expected = str(profile.get("model_digest", ""))
    actual = digest_file(candidate)
    if expected != actual:
        raise ValueError("model digest mismatch: expected %s actual %s" % (expected, actual))
    return candidate


def _preprocess(path: Path) -> np.ndarray:
    image = Image.open(path).convert("RGB")
    image = ImageOps.fit(image, (IMAGE_SIZE, IMAGE_SIZE), method=Image.Resampling.BILINEAR,
                         centering=(0.5, 0.5))
    array = np.asarray(image, dtype=np.float32) / np.float32(255.0)
    return np.transpose(array, (2, 0, 1))


def _view_fields(entry: Mapping[str, Any]) -> tuple[str, Path, str]:
    view_id = str(entry.get("viewId", entry.get("view_id", "")))
    filename = str(entry.get("file", entry.get("sourceFile", "")))
    digest = str(entry.get("sha256", entry.get("contentDigest", "")))
    if not view_id or not filename or not digest:
        raise ValueError("view requires viewId, file, and sha256/contentDigest")
    path = Path(filename).resolve()
    if not path.is_file():
        raise FileNotFoundError("view image is missing: %s" % path)
    expected = digest[len("sha256:"):] if digest.startswith("sha256:") else digest
    actual = digest_file(path)
    actual_hex = actual[len("sha256:"):] if actual.startswith("sha256:") else actual
    if actual_hex != expected:
        raise ValueError("view digest mismatch: %s" % view_id)
    return view_id, path, "sha256:" + expected


def _make_session(artifact: Path) -> tuple[Any, dict[str, Any]]:
    import onnxruntime as ort

    # Explicit list prevents ONNX Runtime from silently selecting Azure/CUDA
    # or appending another provider from the installed wheel.
    session = ort.InferenceSession(str(artifact), providers=["CPUExecutionProvider"])
    providers = session.get_providers()
    if providers != ["CPUExecutionProvider"]:
        raise RuntimeError("unexpected execution providers: %s" % providers)
    inputs = {item.name: item for item in session.get_inputs()}
    outputs = {item.name: item for item in session.get_outputs()}
    if set(inputs) != {"images", "viewMask"}:
        raise ValueError("ONNX input contract mismatch: %s" % sorted(inputs))
    if set(outputs) != {"logits", "pooledFeatures"}:
        raise ValueError("ONNX output contract mismatch: %s" % sorted(outputs))
    image_shape = list(inputs["images"].shape)
    mask_shape = list(inputs["viewMask"].shape)
    if image_shape[1:] != [MAX_VIEWS, 3, IMAGE_SIZE, IMAGE_SIZE]:
        raise ValueError("ONNX image shape mismatch: %s" % image_shape)
    if mask_shape[1:] != [MAX_VIEWS]:
        raise ValueError("ONNX mask shape mismatch: %s" % mask_shape)
    return session, {"providers": providers, "inputs": image_shape, "mask": mask_shape,
                     "outputs": sorted(outputs), "runtime": ort.__version__}


def run_real_manifest(manifest: Mapping[str, Any], output: Path, *, model: str = "",
                      model_digest: str = "", profile_id: str = "vehicle-mvcnn-v1",
                      registry: Path = DEFAULT_REGISTRY, provider: str = "/provider/cpu",
                      mission_id: str = "mission-uav-mv", job_id: str = "recognition-001",
                      attempt: int = 1, minimum_views: int = 2,
                      paired_baseline: bool = False) -> dict[str, Any]:
    """Execute one verified local materialization through the real ONNX graph."""
    started = time.perf_counter()
    profile = load_profile(registry, profile_id)
    artifact = resolve_artifact(profile, model)
    if model_digest and model_digest != digest_file(artifact):
        raise ValueError("requested model digest does not match artifact")
    views = list(manifest.get("views", []))
    if not 1 <= len(views) <= MAX_VIEWS:
        raise ValueError("view count must be within 1-6")
    unique_views = []
    seen_exact_names: set[str] = set()
    for entry in views:
        exact_name = str(entry.get("exactDataName", entry.get("exact_data_name", "")))
        dedupe_key = exact_name or "view:" + str(entry.get("viewId", entry.get("view_id", "")))
        if dedupe_key in seen_exact_names:
            continue
        seen_exact_names.add(dedupe_key)
        unique_views.append(entry)
    duplicate_view_count = len(views) - len(unique_views)
    views = unique_views
    if len(views) < minimum_views and not (paired_baseline and len(views) == 1):
        return {"schema": SCHEMA_RESULT, "status": "insufficient-views",
                "failureStage": "validation", "acceptedViewCount": len(views),
                "requiredViewCount": minimum_views, "jobId": job_id}
    if len(views) == 1 and not paired_baseline:
        return {"schema": SCHEMA_RESULT, "status": "insufficient-views",
                "failureStage": "validation", "acceptedViewCount": 1,
                "requiredViewCount": minimum_views, "jobId": job_id}
    session, runtime = _make_session(artifact)
    tensors = np.zeros((1, MAX_VIEWS, 3, IMAGE_SIZE, IMAGE_SIZE), dtype=np.float32)
    mask = np.zeros((1, MAX_VIEWS), dtype=np.bool_)
    accepted: list[dict[str, Any]] = []
    for index, entry in enumerate(views):
        view_id, path, digest = _view_fields(entry)
        tensors[0, index] = _preprocess(path)
        mask[0, index] = True
        accepted.append({"viewId": view_id, "sourceDigest": digest,
                         "sourceFile": str(path),
                         "exactDataName": str(entry.get("exactDataName", "/uav/unknown")),
                         "mediaType": str(entry.get("mediaType", "image/png")),
                         "signerIdentity": str(entry.get("producerIdentity", "/uav/unknown")),
                         "sourceProducerIdentity": str(entry.get("producerIdentity", "/uav/unknown"))})
    mask_digest = digest_bytes(mask.tobytes())
    input_digest = digest_bytes(tensors.tobytes())
    inference_started = time.perf_counter()
    outputs = session.run(["logits", "pooledFeatures"], {"images": tensors, "viewMask": mask})
    inference_ms = (time.perf_counter() - inference_started) * 1000.0
    logits = np.asarray(outputs[0], dtype=np.float32)
    pooled = np.asarray(outputs[1], dtype=np.float32)
    if not np.isfinite(logits).all() or not np.isfinite(pooled).all():
        raise ValueError("ONNX output contains NaN or Inf")
    class_map = list(profile.get("class_map", ["car", "truck", "person"]))
    prediction = int(np.argmax(logits[0]))
    label = class_map[prediction] if prediction < len(class_map) else "unknown"
    probabilities = np.exp(logits[0] - np.max(logits[0]))
    probabilities = probabilities / np.sum(probabilities)
    output.mkdir(parents=True, exist_ok=True)
    annotation_dir = output / "annotations"
    annotation_dir.mkdir(parents=True, exist_ok=True)
    annotation_refs: list[dict[str, Any]] = []
    for item in accepted:
        image = Image.open(item["sourceFile"]).convert("RGB")
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, image.width - 1, image.height - 1), outline=(255, 32, 32), width=max(2, image.width // 128))
        draw.text((4, 4), "%s %.3f" % (label, float(probabilities[prediction])), fill=(255, 32, 32))
        path = annotation_dir / (item["viewId"] + ".png")
        image.save(path, format="PNG", optimize=False)
        annotation_refs.append({**item, "exactDataName": "%s/UAV/MULTIVIEW/JOB/%s/ANNOTATION/%s/v=1" %
                                (provider.rstrip("/"), job_id, item["viewId"]),
                                "contentDigest": digest_file(path), "mediaType": "image/png",
                                "signerIdentity": provider,
                                "annotationFile": str(path)})
    peak_rss_bytes = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss) * 1024
    evidence = {
        "profileId": profile_id,
        "modelProfileId": profile_id,
        "modelDigest": digest_file(artifact),
        "artifactDigest": digest_file(artifact),
        "checkpointDigest": profile.get("checkpoint_digest"),
        "algorithmId": ALGORITHM_ID,
        "runtimeVersion": runtime["runtime"],
        "executionProviders": runtime["providers"],
        "activeProviders": runtime["providers"],
        "inputTensorContract": runtime["inputs"],
        "inputTensorDigest": input_digest,
        "viewMaskDigest": mask_digest,
        "logitsDigest": digest_bytes(logits.tobytes()),
        "pooledFeatureDigest": digest_bytes(pooled.tobytes()),
        "pooledRepresentationDigest": digest_bytes(pooled.tobytes()),
        "acceptedViewCount": len(accepted),
        "duplicateViewCount": duplicate_view_count,
        "cpuInferenceMs": round(inference_ms, 3),
        "inferenceMs": round(inference_ms, 3),
        "peakRssBytes": peak_rss_bytes,
        "predictedClass": label,
        "confidence": round(float(probabilities[prediction]), 6),
        "fallbackUsed": False,
        "terminalOwner": provider,
    }
    result: dict[str, Any] = {
        "schema": SCHEMA_RESULT,
        "missionSessionId": mission_id,
        "jobId": job_id,
        "attempt": attempt,
        "status": "completed",
        "fusedDecision": {"label": label, "confidence": round(float(probabilities[prediction]), 6)},
        "model": evidence,
        "contributingViews": [item["viewId"] for item in accepted],
        "rejectedViews": [],
        "fusionEvidence": {"operator": "masked-elementwise-max/v1",
                            "consumedViewCount": len(accepted),
                            "pooledFeatureDigest": evidence["pooledFeatureDigest"],
                            "viewMaskDigest": mask_digest},
        "annotatedViews": annotation_refs,
        "terminalOwner": provider,
        "bytesTransferred": sum(Path(item["sourceFile"]).stat().st_size for item in accepted),
        "elapsedMs": round((time.perf_counter() - started) * 1000.0, 3),
        "scientificAccuracyClaimAllowed": bool(profile.get("scientific_accuracy_claim_allowed", False)),
        "pairedBaseline": bool(paired_baseline and len(accepted) == 1),
        "runtime": {"python": platform.python_version(), **runtime},
    }
    result_path = output / "result.json"
    result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    result["resultDigest"] = digest_file(result_path)
    result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--model", default="")
    parser.add_argument("--model-digest", default="")
    parser.add_argument("--profile-id", default="vehicle-mvcnn-v1")
    parser.add_argument("--model-registry", default=str(DEFAULT_REGISTRY))
    parser.add_argument("--provider", default="/provider/cpu")
    parser.add_argument("--mission-id", default="mission-uav-mv")
    parser.add_argument("--job-id", default="recognition-001")
    parser.add_argument("--attempt", type=int, default=1)
    parser.add_argument("--minimum-views", type=int, default=2)
    parser.add_argument("--paired-baseline", action="store_true")
    args = parser.parse_args()
    try:
        manifest_path = Path(args.manifest).resolve()
        manifest = load_json(manifest_path)
        # Fixture manifests use paths relative to their own directory; the
        # evaluator uses absolute frozen paths.  Normalize both forms before
        # hashing so the worker never depends on its caller's cwd.
        for entry in manifest.get("views", []):
            filename = entry.get("file", entry.get("sourceFile"))
            if filename and not Path(str(filename)).is_absolute():
                entry["file"] = str((manifest_path.parent / str(filename)).resolve())
        result = run_real_manifest(manifest, Path(args.output),
                                   model=args.model, model_digest=args.model_digest,
                                   profile_id=args.profile_id, registry=Path(args.model_registry),
                                   provider=args.provider, mission_id=args.mission_id,
                                   job_id=args.job_id, attempt=args.attempt,
                                   minimum_views=args.minimum_views,
                                   paired_baseline=args.paired_baseline)
    except Exception as exc:
        print(json.dumps({"schema": SCHEMA_RESULT, "status": "inference-failed",
                          "failureStage": "model-or-input", "error": str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result.get("status") == "completed" else 3


if __name__ == "__main__":
    raise SystemExit(main())
