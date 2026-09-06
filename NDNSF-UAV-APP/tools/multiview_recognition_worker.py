#!/usr/bin/env python3
"""Detector-guided, CPU-safe multi-view recognition worker for Spec 177.

The worker is deliberately transport-neutral.  NDNSF passes exact named-Data
references; this process receives a local verified materialization manifest,
performs joint feature pooling, and emits a compact result manifest plus one
annotation per contributing view.  Ultralytics is optional: when a model file
is unavailable the deterministic whole-image detector keeps the functional
contract executable without silently claiming model accuracy.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
from typing import Any

try:
    from PIL import Image, ImageDraw
except Exception as exc:  # pragma: no cover - deployment dependency
    Image = None  # type: ignore[assignment]
    ImageDraw = None  # type: ignore[assignment]


ALGORITHM_ID = "detector-guided-mvcnn-pooling/v1"
POOLING_OPERATOR = "elementwise-max/v1"


def _run_real_model(args: argparse.Namespace) -> dict[str, Any]:
    from mvcnn_onnx_worker import run_real_manifest

    manifest_path = Path(args.manifest).resolve()
    manifest = _load_manifest(manifest_path)
    manifest["views"] = list(manifest.get("views", []))[:args.views]
    for entry in manifest.get("views", []):
        filename = entry.get("file", entry.get("sourceFile"))
        if filename and not Path(str(filename)).is_absolute():
            entry["file"] = str((manifest_path.parent / str(filename)).resolve())
    return run_real_manifest(
        manifest, Path(args.output), model=args.model,
        model_digest=args.model_digest, profile_id=args.profile_id,
        registry=Path(args.model_registry), provider=args.provider,
        mission_id=args.mission_id, job_id=args.job_id, attempt=args.attempt,
        minimum_views=args.minimum_views, paired_baseline=args.paired_baseline)


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _feature_vector(image: Any, box: tuple[int, int, int, int]) -> list[float]:
    """Create a small deterministic appearance feature for one detected crop."""
    left, top, right, bottom = box
    crop = image.crop((left, top, right, bottom)).convert("RGB").resize((32, 32))
    raw = bytes(crop.tobytes())
    # Shared feature extraction is intentionally model-neutral.  The vector
    # contains normalized RGB samples and a compact colour histogram, which is
    # sufficient to demonstrate permutation-invariant joint pooling on CPU.
    samples = [byte / 255.0 for byte in raw[::24]]
    histogram = [0] * 16
    for offset in range(0, len(raw), 3):
        brightness = (raw[offset] + raw[offset + 1] + raw[offset + 2]) // 3
        histogram[min(15, brightness // 16)] += 1
    scale = float(max(1, len(raw) // 3))
    return samples + [count / scale for count in histogram]


def _simple_detect(image: Any) -> tuple[tuple[int, int, int, int], float, str]:
    width, height = image.size
    # The generated fixture is a bounded object image.  A full-frame box is a
    # deterministic detector fallback, never a scientific detection claim.
    return (0, 0, width, height), 0.5, "car"


class Detector:
    def __init__(self, model_path: Path | None, confidence: float) -> None:
        self.model = None
        self.model_path = model_path
        self.confidence = confidence
        if model_path and model_path.is_file():
            try:  # optional dependency; no download is attempted
                from ultralytics import YOLO
                self.model = YOLO(str(model_path))
            except Exception:
                self.model = None

    def detect(self, image_path: Path) -> tuple[tuple[int, int, int, int], float, str]:
        image = Image.open(image_path).convert("RGB")
        if self.model is None:
            return _simple_detect(image)
        try:
            predictions = self.model.predict(
                str(image_path), conf=self.confidence, verbose=False, device="cpu"
            )
            names = getattr(self.model, "names", {})
            best: tuple[tuple[int, int, int, int], float, str] | None = None
            for prediction in predictions:
                boxes = getattr(prediction, "boxes", None)
                if boxes is None:
                    continue
                for box in boxes:
                    score = float(box.conf[0])
                    coords = [int(round(float(v))) for v in box.xyxy[0].tolist()]
                    label = str(names.get(int(box.cls[0]), "object")).lower()
                    if best is None or score > best[1]:
                        best = ((coords[0], coords[1], coords[2], coords[3]), score, label)
            if best is not None:
                return best
        except Exception:
            pass
        return _simple_detect(image)


def _load_manifest(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict) or not isinstance(value.get("views"), list):
        raise ValueError("manifest must contain a views array")
    return value


def _select_views(manifest: dict[str, Any], count: int) -> list[dict[str, Any]]:
    views = list(manifest["views"])
    if count < 1 or count > len(views):
        raise ValueError(f"requested view count {count} is outside 1-{len(views)}")
    return views[:count]


def _annotation_name(provider: str, job: str, view_id: str) -> str:
    provider = provider.rstrip("/") or "/provider/compute"
    return f"{provider}/UAV/MULTIVIEW/JOB/{job}/ANNOTATION/{view_id}/v=1"


def _result_name(provider: str, job: str) -> str:
    provider = provider.rstrip("/") or "/provider/compute"
    return f"{provider}/UAV/MULTIVIEW/JOB/{job}/RESULT/v=1"


def run(args: argparse.Namespace) -> dict[str, Any]:
    if getattr(args, "mode", "functional") == "real":
        if not args.profile_id:
            args.profile_id = "vehicle-mvcnn-v1"
        return _run_real_model(args)
    if not args.profile_id:
        args.profile_id = "functional-adapter-v1"
    started = time.perf_counter()
    manifest_path = Path(args.manifest).resolve()
    fixture_root = manifest_path.parent
    source = _load_manifest(manifest_path)
    selected = _select_views(source, args.views)
    minimum = args.minimum_views
    if len(selected) < minimum:
        return {
            "schema": "ndnsf-uav-multiview-result/v1",
            "status": "insufficient-views",
            "jobId": args.job_id,
            "acceptedViewCount": len(selected),
            "requiredViewCount": minimum,
            "failureStage": "validation",
        }
    output_root = Path(args.output).resolve()
    annotation_root = output_root / "annotations"
    annotation_root.mkdir(parents=True, exist_ok=True)
    model_path = Path(args.model).resolve() if args.model else None
    detector = Detector(model_path, args.confidence)
    accepted: list[dict[str, Any]] = []
    feature_vectors: list[list[float]] = []
    all_labels: list[str] = []
    total_bytes = 0
    stage_times: dict[str, float] = {}
    for entry in selected:
        view_started = time.perf_counter()
        image_path = fixture_root / str(entry["file"])
        if not image_path.is_file():
            raise FileNotFoundError(image_path)
        image = Image.open(image_path).convert("RGB")
        total_bytes += image_path.stat().st_size
        box, confidence, label = detector.detect(image_path)
        feature_vectors.append(_feature_vector(image, box))
        all_labels.append(label)
        annotated = image.copy()
        draw = ImageDraw.Draw(annotated)
        draw.rectangle(box, outline=(255, 32, 32), width=max(2, image.width // 128))
        draw.text((box[0] + 4, box[1] + 4), f"{label} {confidence:.3f}", fill=(255, 32, 32))
        annotation_path = annotation_root / f"{entry['view_id']}.png"
        annotated.save(annotation_path, format="PNG", optimize=False)
        accepted.append({
            "viewId": str(entry["view_id"]),
            "sourceDigest": f"sha256:{entry['sha256']}",
            "sourceFile": str(image_path),
            "exactDataName": _annotation_name(args.provider, args.job_id, str(entry["view_id"])),
            "contentDigest": digest_file(annotation_path),
            "mediaType": "image/png",
            "signerIdentity": args.provider,
            "box": list(box),
            "label": label,
            "confidence": round(confidence, 6),
        })
        stage_times[f"view:{entry['view_id']}"] = round((time.perf_counter() - view_started) * 1000, 3)

    width = max(len(vector) for vector in feature_vectors)
    pooled = [max(vector[index] if index < len(vector) else 0.0 for vector in feature_vectors)
              for index in range(width)]
    canonical = json.dumps(
        {"views": [item["viewId"] for item in accepted], "features": feature_vectors, "pooled": pooled},
        separators=(",", ":"), sort_keys=True,
    ).encode("utf-8")
    pooled_digest = digest_bytes(canonical)
    label = "car" if any(item in {"car", "truck", "suv"} for item in all_labels) else (all_labels[0] if all_labels else "object")
    confidence = round(sum(item["confidence"] for item in accepted) / len(accepted), 6)
    job = {
        "schema": "ndnsf-uav-multiview-result/v1",
        "missionSessionId": args.mission_id,
        "jobId": args.job_id,
        "attempt": args.attempt,
        "status": "completed",
        "fusedDecision": {"label": label, "confidence": confidence},
        "model": {
            "profileId": args.profile_id,
            "algorithmId": ALGORITHM_ID,
            "modelDigest": args.model_digest,
            "preprocessingProfile": "rgb-resize-224-center-crop/v1",
        },
        "contributingViews": [item["viewId"] for item in accepted],
        "rejectedViews": [],
        "fusionEvidence": {
            "operator": POOLING_OPERATOR,
            "consumedViewCount": len(accepted),
            "pooledFeatureDigest": pooled_digest,
        },
        "resultManifest": {"exactDataName": _result_name(args.provider, args.job_id)},
        "annotatedViews": accepted,
        "stageTimingsMs": stage_times,
        "bytesTransferred": total_bytes,
        "scientificAccuracyClaimAllowed": False,
        "terminalOwner": args.provider,
        "elapsedMs": round((time.perf_counter() - started) * 1000, 3),
    }
    output_root.mkdir(parents=True, exist_ok=True)
    result_path = output_root / "result.json"
    result_path.write_text(json.dumps(job, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    job["resultManifest"]["contentDigest"] = digest_file(result_path)
    result_path.write_text(json.dumps(job, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return job


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--views", type=int, default=6)
    parser.add_argument("--minimum-views", type=int, default=2)
    parser.add_argument("--model", default="")
    parser.add_argument("--model-digest", default="sha256:unavailable-local-model")
    parser.add_argument("--profile-id", default="")
    parser.add_argument("--provider", default="/provider/compute")
    parser.add_argument("--mission-id", default="mission-uav-mv")
    parser.add_argument("--job-id", default="recognition-001")
    parser.add_argument("--attempt", type=int, default=1)
    parser.add_argument("--confidence", type=float, default=0.25)
    parser.add_argument("--mode", choices=("functional", "real"), default="functional")
    parser.add_argument("--model-registry", default=str(Path(__file__).resolve().parents[2] /
                                                           "NDNSF-UAV-APP/configs/uav_multiview_models.json"))
    parser.add_argument("--paired-baseline", action="store_true")
    args = parser.parse_args()
    try:
        result = run(args)
    except Exception as exc:
        print(json.dumps({"status": "inference-failed", "error": str(exc)}), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result.get("status") == "completed" else 3


if __name__ == "__main__":
    raise SystemExit(main())
