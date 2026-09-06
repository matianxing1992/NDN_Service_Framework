#!/usr/bin/env python3
"""Run a paired one-view versus multi-view evaluation on a frozen registration.

This evaluator is intentionally claim-bounded.  It records a row for every
sample/view-count pair, input/result hashes, stage failures, and deterministic
paired bootstrap metadata.  A functional or one-sample registration can run
the same code, but it cannot silently become a scientific accuracy claim.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Any, Dict, Iterable, List, Mapping, Optional


ROOT = Path(__file__).resolve().parents[2]
RUNNER = Path(__file__).with_name("multiview_recognition_worker.py")
DEFAULT_REGISTRY = ROOT / "NDNSF-UAV-APP/configs/uav_multiview_models.json"


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_text(value: str) -> str:
    return digest_bytes(value.encode("utf-8"))


def digest_file(path: Path) -> str:
    return digest_bytes(path.read_bytes())


def git_value(*args: str) -> str:
    try:
        return subprocess.check_output(["git", *args], cwd=ROOT,
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return "unavailable"


def score(label: str, truth: str) -> int:
    return int(label.strip().lower() == truth.strip().lower())


def macro_f1(rows: Iterable[Mapping[str, Any]]) -> Optional[float]:
    """Compute macro-F1 over observed labels without dropping failed rows."""
    observations = list(rows)
    labels = sorted({str(row.get("groundTruthLabel", "")) for row in observations} |
                    {str(row.get("label", "")) for row in observations})
    labels = [label for label in labels if label]
    if not labels:
        return None
    scores = []
    for label in labels:
        true_positive = sum(str(row.get("groundTruthLabel", "")) == label and
                            str(row.get("label", "")) == label for row in observations)
        false_positive = sum(str(row.get("groundTruthLabel", "")) != label and
                             str(row.get("label", "")) == label for row in observations)
        false_negative = sum(str(row.get("groundTruthLabel", "")) == label and
                             str(row.get("label", "")) != label for row in observations)
        denominator = 2 * true_positive + false_positive + false_negative
        scores.append((2 * true_positive / denominator) if denominator else 0.0)
    return round(statistics.mean(scores), 6)


def bootstrap_ci(values: Iterable[float], seed: int, replicates: int = 2000) -> Dict[str, Any]:
    observations = [float(value) for value in values]
    if len(observations) < 2:
        return {
            "status": "insufficient-samples",
            "sampleCount": len(observations),
            "replicates": 0,
            "ci95": None,
        }
    rng = random.Random(seed)
    draws = []
    for _ in range(replicates):
        draws.append(statistics.mean(rng.choice(observations)
                                     for _ in observations))
    draws.sort()
    return {
        "status": "computed",
        "sampleCount": len(observations),
        "replicates": replicates,
        "ci95": [draws[int(0.025 * (len(draws) - 1))],
                  draws[int(0.975 * (len(draws) - 1))]],
    }


def _profile(registry: Mapping[str, Any], profile_id: str) -> Mapping[str, Any]:
    for profile in registry.get("profiles", []):
        if profile.get("profile_id") == profile_id:
            return profile
    return {}


def _make_subset_manifest(tmp: Path, sample: Mapping[str, Any], views: List[Mapping[str, Any]],
                          count: int) -> Path:
    path = tmp / ("%s-%d.json" % (sample["sampleId"], count))
    value = {
        "schema": "ndnsf-uav-evaluation-input/v1",
        "sampleId": sample["sampleId"],
        "targetId": sample["targetId"],
        "views": [{
            "view_id": view["viewId"],
            "file": view["file"],
            "sha256": view["sha256"],
        } for view in views],
    }
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def _evaluate_row(sample: Mapping[str, Any], views: List[Mapping[str, Any]], count: int,
                  tmp: Path, provider: str, profile: Mapping[str, Any]) -> Dict[str, Any]:
    minimum = 1 if count == 1 else 2
    input_manifest = _make_subset_manifest(tmp, sample, views, count)
    output = tmp / ("%s-output-%d" % (sample["sampleId"], count))
    started = time.perf_counter()
    command = [sys.executable, str(RUNNER), "--manifest", str(input_manifest),
               "--views", str(count), "--minimum-views", str(minimum),
               "--output", str(output), "--provider", provider,
               "--mode", "real",
               "--profile-id", str(profile.get("profile_id", "vehicle-mvcnn-v1")),
               "--model-digest", str(profile.get("model_digest", ""))]
    if count == 1:
        command.append("--paired-baseline")
    completed = subprocess.run(
        command,
        text=True, capture_output=True, check=False,
    )
    elapsed = (time.perf_counter() - started) * 1000
    row: Dict[str, Any] = {
        "sampleId": sample["sampleId"],
        "targetId": sample["targetId"],
        "viewCount": count,
        "inputManifestDigest": digest_file(input_manifest),
        "groundTruthLabel": sample["groundTruthLabel"],
        "status": "failed",
        "failureStage": "worker",
        "label": "",
        "confidence": None,
        "correct": 0,
        "acceptedViewCount": 0,
        "latencyMs": round(elapsed, 3),
        "e2eLatencyMs": round(elapsed, 3),
        "modelLatencyMs": None,
        "peakRssBytes": 0,
        "bytesTransferred": sum(Path(view["file"]).stat().st_size for view in views),
        "resultDigest": None,
        "stderr": completed.stderr[-500:] if completed.returncode else "",
    }
    result_path = output / "result.json"
    if completed.returncode == 0 and result_path.is_file():
        result = json.loads(result_path.read_text(encoding="utf-8"))
        row.update({
            "status": result.get("status", "failed"),
            "failureStage": result.get("failureStage"),
            "label": result.get("fusedDecision", {}).get("label", ""),
            "confidence": result.get("fusedDecision", {}).get("confidence"),
            "correct": score(result.get("fusedDecision", {}).get("label", ""),
                              sample["groundTruthLabel"]),
            "acceptedViewCount": len(result.get("contributingViews", [])),
            "modelLatencyMs": result.get("model", {}).get("cpuInferenceMs"),
            "peakRssBytes": int(result.get("model", {}).get("peakRssBytes", 0) or 0),
            "resultDigest": digest_file(result_path),
            "contributingViews": result.get("contributingViews", []),
            "fusionEvidence": result.get("fusionEvidence", {}),
        })
    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registration", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--provider", default="/provider/evaluator")
    parser.add_argument("--model-registry", default=str(DEFAULT_REGISTRY))
    parser.add_argument("--bootstrap-replicates", type=int, default=2000)
    args = parser.parse_args()

    registration_path = Path(args.registration).resolve()
    registration = json.loads(registration_path.read_text(encoding="utf-8"))
    if registration.get("schema") != "ndnsf-uav-multiview-dataset-registration/v1":
        raise SystemExit("unsupported dataset registration schema")
    registry_path = Path(args.model_registry).resolve()
    registry = json.loads(registry_path.read_text(encoding="utf-8"))
    profile_id = str(registration.get("modelProfileId", "vehicle-mvcnn-v1"))
    profile = _profile(registry, profile_id)
    rows: List[Dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="uav-mv-eval-") as directory:
        tmp = Path(directory)
        for sample in registration.get("samples", []):
            views = list(sample["views"])
            for count in (1, 2, 4, 6):
                if count <= len(views):
                    rows.append(_evaluate_row(sample, views[:count], count, tmp,
                                              args.provider, profile))

    metrics: Dict[str, Any] = {}
    for count in sorted({int(row["viewCount"]) for row in rows}):
        group = [row for row in rows if int(row["viewCount"]) == count]
        metrics[str(count)] = {
            "samples": len(group),
            "accuracy": round(statistics.mean(row["correct"] for row in group), 6) if group else 0.0,
            "completionRate": round(sum(row["status"] == "completed" for row in group) /
                                      max(1, len(group)), 6),
            "meanLatencyMs": round(statistics.mean(row["latencyMs"] for row in group), 3) if group else 0.0,
            "meanE2ELatencyMs": round(statistics.mean(row["e2eLatencyMs"] for row in group), 3) if group else 0.0,
            "meanModelLatencyMs": round(statistics.mean(
                row["modelLatencyMs"] for row in group if row["modelLatencyMs"] is not None), 3)
                if any(row["modelLatencyMs"] is not None for row in group) else None,
            "meanBytesTransferred": round(statistics.mean(row["bytesTransferred"] for row in group), 3) if group else 0.0,
            "meanAcceptedViews": round(statistics.mean(row["acceptedViewCount"] for row in group), 3) if group else 0.0,
            "meanConfidence": round(statistics.mean(
                float(row["confidence"]) for row in group if row["confidence"] is not None), 6)
                if any(row["confidence"] is not None for row in group) else None,
            "meanPeakRssBytes": round(statistics.mean(row["peakRssBytes"] for row in group), 3) if group else 0.0,
            "macroF1": macro_f1(group),
            "failureCount": sum(row["status"] != "completed" for row in group),
            "failureStageCounts": {
                str(stage): sum(row.get("failureStage") == stage for row in group)
                for stage in sorted({str(row.get("failureStage")) for row in group
                                     if row.get("failureStage")})
            },
        }

    by_sample: Dict[str, Dict[int, Dict[str, Any]]] = {}
    for row in rows:
        by_sample.setdefault(str(row["sampleId"]), {})[int(row["viewCount"])] = row
    paired_uncertainty: Dict[str, Any] = {}
    for count in (2, 4, 6):
        deltas_accuracy = []
        deltas_latency = []
        deltas_model_latency = []
        mcnemar = {"baselineCorrectTreatmentIncorrect": 0,
                   "baselineIncorrectTreatmentCorrect": 0,
                   "bothCorrect": 0, "bothIncorrect": 0}
        for sample_id, sample_rows in by_sample.items():
            baseline = sample_rows.get(1)
            treatment = sample_rows.get(count)
            if baseline and treatment:
                deltas_accuracy.append(float(treatment["correct"] - baseline["correct"]))
                deltas_latency.append(float(treatment["e2eLatencyMs"] - baseline["e2eLatencyMs"]))
                if treatment["modelLatencyMs"] is not None and baseline["modelLatencyMs"] is not None:
                    deltas_model_latency.append(float(treatment["modelLatencyMs"] - baseline["modelLatencyMs"]))
                if baseline["correct"] and treatment["correct"]:
                    mcnemar["bothCorrect"] += 1
                elif baseline["correct"] and not treatment["correct"]:
                    mcnemar["baselineCorrectTreatmentIncorrect"] += 1
                elif not baseline["correct"] and treatment["correct"]:
                    mcnemar["baselineIncorrectTreatmentCorrect"] += 1
                else:
                    mcnemar["bothIncorrect"] += 1
        paired_uncertainty[str(count)] = {
            "pairedSampleCount": len(deltas_accuracy),
            "accuracyDeltaMultiMinusOneView": {
                "mean": round(statistics.mean(deltas_accuracy), 6) if deltas_accuracy else None,
                **bootstrap_ci(deltas_accuracy, 177000 + count, args.bootstrap_replicates),
            },
            "latencyDeltaMsMultiMinusOneView": {
                "mean": round(statistics.mean(deltas_latency), 3) if deltas_latency else None,
                **bootstrap_ci(deltas_latency, 178000 + count, args.bootstrap_replicates),
            },
            "modelLatencyDeltaMsMultiMinusOneView": {
                "mean": round(statistics.mean(deltas_model_latency), 3) if deltas_model_latency else None,
                **bootstrap_ci(deltas_model_latency, 179000 + count, args.bootstrap_replicates),
            },
            "mcnemar": mcnemar,
        }

    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    registration_digest = digest_file(registration_path)
    registry_digest = digest_file(registry_path)
    result: Dict[str, Any] = {
        "schema": "ndnsf-uav-multiview-evaluation/v2",
        "datasetId": registration["datasetId"],
        "registrationDigest": registration_digest,
        "modelRegistryDigest": registry_digest,
        "modelProfile": profile,
        "rows": rows,
        "metrics": metrics,
        "pairedUncertainty": paired_uncertainty,
        "uncertainty": {
            "method": "paired bootstrap over sample-level multi-view minus one-view deltas",
            "replicates": args.bootstrap_replicates,
            "status": "insufficient-samples" if len(by_sample) < 2 else "computed",
            "sampleCount": len(by_sample),
        },
        "provenance": {
            "sourceRevision": git_value("rev-parse", "HEAD"),
            "workingTreeStatus": git_value("status", "--short"),
            "python": platform.python_version(),
            "evaluator": str(Path(__file__).resolve()),
        },
        "scientificAccuracyClaimAllowed": bool(
            registration.get("scientificAccuracyClaimAllowed", False)
        ),
        "claimBoundary": "functional point estimates only; no scientific accuracy or multi-view benefit claim",
    }
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    run_manifest = output.parent / "campaign-manifest.json"
    run_manifest.write_text(json.dumps({
        "schema": "ndnsf-uav-multiview-campaign-manifest/v1",
        "registration": str(registration_path),
        "registrationDigest": registration_digest,
        "modelRegistry": str(registry_path),
        "modelRegistryDigest": registry_digest,
        "output": str(output),
        "outputDigest": digest_file(output),
        "command": " ".join(sys.argv),
        "sourceRevision": git_value("rev-parse", "HEAD"),
    }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"datasetId": result["datasetId"], "metrics": metrics,
                      "uncertainty": result["uncertainty"],
                      "evaluationDigest": digest_file(output)},
                     indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
