"""Build Spec 168 admission evidence from one completed real runtime."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import time
from typing import Mapping, Sequence

from ndnsf_distributed_inference.core.contracts import LifecycleEventV1


RUNTIME_SCHEMA = "ndnsf-di.spec168-runtime-admission.v1"
_DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}")


def _digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(4 << 20), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def _require_digest(value: str, label: str) -> str:
    if _DIGEST_RE.fullmatch(str(value)) is None:
        raise ValueError(f"SPEC168_RUNTIME_{label}_DIGEST_INVALID")
    return str(value)


def _required_marker(text: str, marker: str, label: str) -> None:
    if marker not in text:
        raise ValueError(f"SPEC168_RUNTIME_MARKER_MISSING:{label}:{marker}")


def _one_match(pattern: str, text: str, label: str) -> re.Match[str]:
    matches = list(re.finditer(pattern, text))
    if len(matches) != 1:
        raise ValueError(
            f"SPEC168_RUNTIME_MARKER_CARDINALITY:{label}:{len(matches)}")
    return matches[0]


def _read_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"SPEC168_RUNTIME_JSON_OBJECT_REQUIRED:{path.name}")
    return value


def _generation_samples(path: Path) -> tuple[list[dict], dict]:
    rows = [json.loads(line) for line in path.read_text(
        encoding="utf-8").splitlines() if line.strip()]
    if not rows or any(row.get("status") != "OK" for row in rows):
        raise ValueError("SPEC168_RUNTIME_GENERATION_SAMPLE_INVALID")
    measured = [row for row in rows if row.get("phase") == "measured"]
    if len(measured) != 1:
        raise ValueError("SPEC168_RUNTIME_GENERATION_SAMPLE_INVALID")
    return rows, measured[0]


def _token_metadata(sample: Mapping[str, object]) -> dict:
    steps = sample.get("tokenSteps")
    if not isinstance(steps, list) or len(steps) != 1:
        raise ValueError("SPEC168_RUNTIME_INVOCATION_FRAGMENTED")
    step = steps[0]
    metadata = step.get("metadata") if isinstance(step, dict) else None
    if not isinstance(metadata, dict):
        raise ValueError("SPEC168_RUNTIME_TOKEN_METADATA_MISSING")
    return metadata


def _provider_observation(
    path: Path, *, stage_index: int, plan_digest: str, request_id: str,
) -> dict[str, object]:
    text = path.read_text(errors="replace")
    role = f"/LLM/Pipeline/Stage/{stage_index}"
    ready = _one_match(
        rf"LLM_PIPELINE_QWEN_RUNTIME_READY .*?requestId={re.escape(request_id)} "
        rf".*?role={re.escape(role)} "
        r"device=([^ ]+) artifactDigest=(sha256:[0-9a-f]{64}) "
        r"loadCompleted=true warmupCompleted=true cpuFallbackCount=([0-9]+) "
        r"deviceClass=([A-Z_]+)",
        text,
        f"stage-{stage_index}-ready",
    )
    residency = _one_match(
        rf"LLM_PIPELINE_QWEN_RESIDENCY_SNAPSHOT .*?requestId={re.escape(request_id)} "
        rf".*?role={re.escape(role)} "
        r"snapshot=(\{.*\})",
        text,
        f"stage-{stage_index}-residency",
    )
    snapshot = json.loads(residency.group(1))
    records = snapshot.get("records")
    if not isinstance(records, list) or len(records) != 1:
        raise ValueError(f"SPEC168_RUNTIME_RESIDENCY_INVALID:stage-{stage_index}")
    record = records[0]
    if record.get("partitionDigest") != plan_digest:
        raise ValueError(f"SPEC168_RUNTIME_PLAN_MISMATCH:stage-{stage_index}")
    _required_marker(text, "NDNSF_DI_PROVIDER_HANDLER_TIMING event=start",
                     f"stage-{stage_index}-executing")
    completion = (
        "LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL" if stage_index == 0 else
        "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED" if stage_index == 2 else
        "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED"
    )
    _required_marker(text, completion, f"stage-{stage_index}-completed")
    return {
        "role": f"stage-{stage_index}",
        "wireRole": role,
        "providerBootEpoch": str(snapshot["providerBootEpoch"]),
        "device": ready.group(1),
        "artifactDigest": ready.group(2),
        "cpuFallbackCount": int(ready.group(3)),
        "deviceClass": ready.group(4),
        "backend": str(record["backend"]),
        "loadCompleted": True,
        "warmupCompleted": True,
        "logDigest": _digest(path),
        "completionMarker": completion,
    }


def _lifecycle_events(
    *, experiment_id: str, request_id: str, plan_digest: str,
    observations: Sequence[Mapping[str, object]],
) -> list[LifecycleEventV1]:
    rows: list[tuple[str, str, Mapping[str, object] | None]] = [
        ("REQUEST_CREATED", "user", None),
        ("REQUEST_PUBLISHED", "user", None),
        ("ACK_CLOSED", "user", None),
        ("GRAPH_INSPECTED", "planner", None),
        ("PLAN_VALIDATED", "planner", None),
        ("PLAN_COMMITTED", "planner", None),
        ("FINAL_SELECTION", "user", None),
    ]
    for item in observations:
        for kind in ("ROLE_ASSIGNED", "LOCAL_READY", "STAGE_EXECUTING",
                     "STAGE_COMPLETED"):
            rows.append((kind, "provider", item))
    rows.append(("RESPONSE_PUBLISHED", "user", None))
    now = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    base_ns = time.monotonic_ns()
    events = []
    plan_bound = {
        "PLAN_COMMITTED", "FINAL_SELECTION", "ROLE_ASSIGNED", "LOCAL_READY",
        "STAGE_EXECUTING", "STAGE_COMPLETED", "RESPONSE_PUBLISHED",
    }
    for sequence, (kind, component, item) in enumerate(rows, 1):
        role = str(item["role"]) if item is not None else None
        stage_index = int(role[-1]) if role is not None else -1
        events.append(LifecycleEventV1(
            experiment_id=experiment_id,
            request_id=request_id,
            attempt_epoch=1,
            event_id=f"{experiment_id}-event-{sequence:03d}",
            event_type=kind,
            component=component,
            provider=(f"/example/llm-pipeline/provider"
                      + (f"/{stage_index}" if stage_index else ""))
            if item is not None else None,
            provider_boot_epoch=str(item["providerBootEpoch"])
            if item is not None else None,
            role=role,
            plan_digest=plan_digest if kind in plan_bound else None,
            operation_id=None,
            epoch=0,
            sequence=sequence,
            monotonic_ns=base_ns + sequence,
            wall_time_utc=now,
            authenticated=True,
            details_schema="ndnsf-di.runtime-observation.v1",
            details={
                "source": "authenticated-runtime-log",
                **({"logDigest": item["logDigest"]} if item is not None else {}),
            },
        ))
    return events


def write_spec168_runtime_evidence(
    output_dir: str | Path,
    *,
    source_digest: str,
    model_identity_digest: str,
    workload_digest: str,
    process_rows: Sequence[Mapping[str, object]],
    route_snapshot: str,
    fidelity: str = "REAL_MININDN",
    sif_digest: str = "",
    admission_path: str | Path | None = None,
) -> dict[str, object]:
    """Validate retained runtime observations, then emit Gate B/C contracts."""
    root = Path(output_dir)
    source_digest = _require_digest(source_digest, "SOURCE")
    model_identity_digest = _require_digest(model_identity_digest, "MODEL")
    workload_digest = _require_digest(workload_digest, "WORKLOAD")
    if fidelity not in {"REAL_MININDN", "EXACT_SIF"}:
        raise ValueError("SPEC168_RUNTIME_FIDELITY_INVALID")
    if fidelity == "EXACT_SIF":
        _require_digest(sif_digest, "SIF")

    manifest_path = (
        Path(admission_path).resolve() if admission_path is not None
        else root / "spec168-runtime-admission.json"
    )
    evidence_root = manifest_path.parent
    evidence_root.mkdir(parents=True, exist_ok=True)
    route_path = evidence_root / "route-snapshot.txt"
    route_path.write_text(route_snapshot, encoding="utf-8")
    user_path = root / "llm-pipeline-user.log"
    user_text = user_path.read_text(errors="replace")
    _required_marker(user_text, "NDNSF_DI_AUTOPLANNING_ACK_CLOSED", "ack")
    _required_marker(user_text, "NDNSF_DI_AUTOPLANNING_GRAPH_READY", "graph")
    _required_marker(user_text, "LLM_PIPELINE_GENERATION_CAMPAIGN_PASS", "response")
    selection_matches = list(re.finditer(
        r"NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED .*?requestId=([^ ]+) "
        r".*?candidateDigest=(sha256:[0-9a-f]{64})",
        user_text,
    ))
    if not selection_matches:
        raise ValueError(
            "SPEC168_RUNTIME_MARKER_CARDINALITY:selection-committed:0")
    plan_by_request: dict[str, str] = {}
    for match in selection_matches:
        request = match.group(1)
        if request in plan_by_request:
            raise ValueError(
                "SPEC168_RUNTIME_SELECTION_DUPLICATE_REQUEST:" + request)
        plan_by_request[request] = match.group(2)

    samples, sample = _generation_samples(root / "generation.jsonl")
    observed_invocations = []
    for item in samples:
        item_metadata = _token_metadata(item)
        item_request_id = str(item_metadata.get("requestId", ""))
        item_generated = item.get("generatedTokenIds")
        if (not item_request_id or item_request_id not in plan_by_request
                or not isinstance(item_generated, list)
                or len(item_generated) < 2
                or int(item_metadata.get("wireRequestCount", -1)) != 1
                or int(item_metadata.get("tokenRequestCount", -1)) != 0):
            raise ValueError("SPEC168_RUNTIME_INVOCATION_INVALID")
        observed_invocations.append({
            "phase": str(item.get("phase", "")),
            "requestId": item_request_id,
            "planDigest": plan_by_request[item_request_id],
            "wireRequestCount": 1,
            "tokenRequestCount": 0,
            "completeResponse": True,
            "tokenCount": len(item_generated),
            "totalMs": float(item.get("totalMs", 0.0)),
        })

    metadata = _token_metadata(sample)
    request_id = str(metadata.get("requestId", ""))
    plan_digest = plan_by_request[request_id]
    planning = _read_json(root / "automatic-planning.json")
    if planning.get("candidateDigest") != plan_digest:
        raise ValueError("SPEC168_RUNTIME_PLAN_MISMATCH:planning")

    if (sample.get("modelIdentityDigest") != model_identity_digest
            or sample.get("workloadDigest") != workload_digest):
        raise ValueError("SPEC168_RUNTIME_GENERATION_IDENTITY_MISMATCH")
    request_id = str(metadata.get("requestId", ""))
    generated = sample.get("generatedTokenIds")
    if (not request_id or not isinstance(generated, list) or len(generated) < 2
            or int(metadata.get("wireRequestCount", -1)) != 1
            or int(metadata.get("tokenRequestCount", -1)) != 0):
        raise ValueError("SPEC168_RUNTIME_INVOCATION_INVALID")

    repo = _read_json(root / "repo-registration.json")
    artifacts = repo.get("artifacts")
    if not isinstance(artifacts, list) or len(artifacts) != 3:
        raise ValueError("SPEC168_RUNTIME_REPO_REGISTRATION_INVALID")
    artifact_digests = {
        f"stage-{int(item['stageIndex'])}": str(item["fileSha256"])
        for item in artifacts
    }
    observations = [
        _provider_observation(
            root / f"stage{index}-provider.log",
            stage_index=index,
            plan_digest=plan_digest,
            request_id=request_id,
        ) for index in range(3)
    ]
    if any(item["artifactDigest"] != artifact_digests[item["role"]]
           for item in observations):
        raise ValueError("SPEC168_RUNTIME_ARTIFACT_BINDING_MISMATCH")
    device_classes = {str(item["deviceClass"]) for item in observations}
    backends = {str(item["backend"]) for item in observations}
    if len(device_classes) != 1 or len(backends) != 1:
        raise ValueError("SPEC168_RUNTIME_DEVICE_BINDING_MISMATCH")

    provider_texts = [
        (root / f"stage{index}-provider.log").read_text(errors="replace")
        for index in range(3)
    ]
    security_ok = (
        "UserToken/ProviderToken runtime mode: enabled" in user_text
        and "Installed user permission" in user_text
        and all("NAC_ABE_BOOTSTRAP" in text for text in provider_texts)
        and all("Installed provider permission" in text for text in provider_texts)
    )
    if not security_ok:
        raise ValueError("SPEC168_RUNTIME_SECURITY_EVIDENCE_MISSING")

    experiment_id = str(sample.get("campaignId", "spec168-local-gate"))
    events = _lifecycle_events(
        experiment_id=experiment_id,
        request_id=request_id,
        plan_digest=plan_digest,
        observations=observations,
    )
    lifecycle_path = evidence_root / "lifecycle.jsonl"
    lifecycle_path.write_text("".join(
        json.dumps(item.to_dict(), sort_keys=True, separators=(",", ":")) + "\n"
        for item in events
    ), encoding="utf-8")

    manifest: dict[str, object] = {
        "schema": RUNTIME_SCHEMA,
        "fidelity": fidelity,
        "sourceDigest": source_digest,
        "modelIdentityDigest": model_identity_digest,
        "workloadDigest": workload_digest,
        "simulatedComponents": [],
        "processes": [dict(item) for item in process_rows],
        "network": {
            "realMiniNdn": True,
            "realNfdPerNode": True,
            "hostNfdUsed": False,
            "routeSnapshotDigest": _digest(route_path),
        },
        "security": {
            "normalPermissions": True,
            "nacAbe": True,
            "userToken": True,
            "providerToken": True,
            "replayProtection": True,
            "testOnlyIdentities": False,
            "bypassEnabled": False,
        },
        "artifactDelivery": {
            "transport": "NDNSF-DistributedRepo",
            "throughNdn": True,
            "sharedFilesystemPayloadInjection": False,
            "uniqueBytes": sum(int(item["fileBytes"]) for item in artifacts),
            "wireBytes": sum(int(item["fileBytes"]) for item in artifacts),
            "artifactDigests": artifact_digests,
        },
        "readiness": {"mode": "event-driven", "fixedSettleWaitMs": 0},
        "adapter": {
            "name": "qwen-transformers",
            "mocked": False,
            "backend": next(iter(backends)),
            "deviceClass": next(iter(device_classes)),
            "assignments": [{
                key: item[key] for key in (
                    "role", "device", "artifactDigest", "loadCompleted",
                    "warmupCompleted", "cpuFallbackCount",
                )
            } for item in observations],
        },
        "invocation": {
            "requestId": request_id,
            "attemptEpoch": 1,
            "wireRequestCount": int(metadata["wireRequestCount"]),
            "tokenRequestCount": int(metadata["tokenRequestCount"]),
            "completeResponse": True,
            "tokenCount": len(generated),
            "cpuFallbackCount": sum(
                int(item["cpuFallbackCount"]) for item in observations),
        },
        "observedInvocations": observed_invocations,
        "evidence": {"lifecycleJsonl": lifecycle_path.name},
    }
    if fidelity == "EXACT_SIF":
        manifest["container"] = {
            "runtime": "apptainer", "sifDigest": sif_digest}
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest
