#!/usr/bin/env python3
"""Default fail-closed local deployment gate for NDNSF-DI (Spec 165)."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from .ndnsf_validation.evidence import read_jsonl, validate_generation_evidence
    from .ndnsf_validation.fidelity import (
        FidelityTier,
        GatePolicy,
        aggregate_records,
    )
    from .ndnsf_validation.workload import (
        DEFAULT_MODEL_SNAPSHOT,
        canonical_workload,
        digest_value,
        write_workload,
    )
except ImportError:
    from ndnsf_validation.evidence import read_jsonl, validate_generation_evidence
    from ndnsf_validation.fidelity import (
        FidelityTier,
        GatePolicy,
        aggregate_records,
    )
    from ndnsf_validation.workload import (
        DEFAULT_MODEL_SNAPSHOT,
        canonical_workload,
        digest_value,
        write_workload,
    )

REPO = Path(__file__).resolve().parents[1]
SOURCE_FILES = (
    "Experiments/NDNSF_DI_Run_Local_Deployment_Gates.py",
    "Experiments/NDNSF_DI_Prepare_Spec165_Workload.py",
    "Experiments/NDNSF_DI_LlmPipeline_Minindn.py",
    "Experiments/NDNSF_Run_Minindn_Quick_Checks.py",
    "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py",
    "examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py",
    "examples/python/NDNSF-DistributedInference/llm_pipeline/provider.py",
    "NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py",
)
MANDATORY_TIERS = {
    "gate-a-fidelity": FidelityTier.UNIT,
    "gate-b-minindn": FidelityTier.REAL_MININDN_MODEL,
    "gate-c-container": FidelityTier.REAL_CANDIDATE_CONTAINER_MODEL,
    "gate-d-deadline": FidelityTier.UNIT,
}
ARTIFACT_BUNDLE_SCHEMA = "ndnsf-experiment-artifact-bundle-v2"
DEFAULT_ARTIFACT_STORE_ROOT = REPO / "results" / "_artifacts"


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def source_identity() -> tuple[str, dict[str, str]]:
    head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=True,
    ).stdout.strip()
    manifest = {}
    for relative in SOURCE_FILES:
        path = REPO / relative
        if not path.is_file():
            raise RuntimeError(f"required source file is absent: {relative}")
        manifest[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    digest = hashlib.sha256(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    return f"{head}+spec165:{digest}", manifest


def command_text(command: list[str]) -> str:
    return shlex.join(command)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def _stage_bundle_manifest(
    stage_dir: Path, *, model_content_digest: str
) -> dict[str, Any]:
    prior_manifest = stage_dir / "bundle-manifest.json"
    files = []
    if prior_manifest.is_file():
        prior = json.loads(prior_manifest.read_text(encoding="utf-8"))
        files = [dict(item) for item in prior.get("files", [])]
        for item in files:
            path = stage_dir / item["path"]
            if (
                not path.is_file()
                or path.is_symlink()
                or path.stat().st_size != item["size"]
            ):
                raise RuntimeError(f"prior artifact bundle member is malformed: {path}")
    else:
        for path in sorted(item for item in stage_dir.rglob("*") if item.is_file()):
            if path.relative_to(stage_dir).as_posix() == "bundle-manifest.json":
                continue
            if path.is_symlink():
                raise RuntimeError(f"stage artifact bundle cannot contain symlinks: {path}")
            files.append(
                {
                    "path": path.relative_to(stage_dir).as_posix(),
                    "size": path.stat().st_size,
                    "sha256": _sha256_file(path),
                }
            )
    if not files:
        raise RuntimeError(f"stage artifact directory is empty: {stage_dir}")
    identity = {
        "schema": ARTIFACT_BUNDLE_SCHEMA,
        "kind": "qwen-onnx-stage-bundle",
        "modelContentDigest": model_content_digest,
        "files": files,
    }
    return {
        **identity,
        "bundleDigest": "sha256:"
        + hashlib.sha256(_canonical_json_bytes(identity)).hexdigest(),
        "payloadBytes": sum(item["size"] for item in files),
    }


def _verify_stage_bundle(
    bundle_dir: Path, manifest: dict[str, Any], *, verify_payload: bool
) -> None:
    manifest_path = bundle_dir / "bundle-manifest.json"
    if not manifest_path.is_file():
        raise RuntimeError(f"artifact store manifest is missing: {manifest_path}")
    stored = json.loads(manifest_path.read_text(encoding="utf-8"))
    if stored != manifest:
        raise RuntimeError(f"artifact store manifest conflicts with identity: {bundle_dir}")
    for item in manifest["files"]:
        path = bundle_dir / item["path"]
        if (
            not path.is_file()
            or path.is_symlink()
            or path.stat().st_size != item["size"]
        ):
            raise RuntimeError(f"artifact store member is absent or malformed: {path}")
        if verify_payload and _sha256_file(path) != item["sha256"]:
            raise RuntimeError(f"artifact store member digest mismatch: {path}")


def retain_stage_artifacts(
    *,
    stage_dir: Path,
    artifact_store_root: Path,
    model_content_digest: str,
    workload_digest: str,
    replace_source: bool,
) -> dict[str, Any]:
    """Install a stage bundle by hard link, then leave only a run-local link.

    The content store must share the repository filesystem. This makes import
    write zero duplicate payload bytes and keeps a bundle alive after its
    originating run directory is pruned.
    """
    source_dir = stage_dir.resolve(strict=True)
    if not source_dir.is_dir():
        raise RuntimeError(f"stage artifact directory is absent: {stage_dir}")
    store_root = artifact_store_root.expanduser().resolve()
    try:
        store_root.relative_to(REPO)
    except ValueError as exc:
        raise RuntimeError(
            "artifact store must be inside the repository so candidate containers can mount it"
        ) from exc
    manifest = _stage_bundle_manifest(
        source_dir,
        model_content_digest=model_content_digest,
    )
    digest = manifest["bundleDigest"].split(":", 1)[1]
    bundle_parent = store_root / "qwen-stage-bundles" / "sha256"
    bundle_dir = bundle_parent / digest
    bundle_parent.mkdir(parents=True, exist_ok=True)
    installed = False
    if bundle_dir.exists():
        _verify_stage_bundle(bundle_dir, manifest, verify_payload=False)
    else:
        staging = bundle_parent / f".{digest}.{uuid.uuid4().hex}.partial"
        staging.mkdir(parents=False, exist_ok=False)
        try:
            for item in manifest["files"]:
                source = source_dir / item["path"]
                target = staging / item["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                try:
                    os.link(source, target)
                except OSError as exc:
                    raise RuntimeError(
                        "artifact store must share a filesystem with prepared artifacts; "
                        "copy fallback is intentionally disabled"
                    ) from exc
            (staging / "bundle-manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            _verify_stage_bundle(staging, manifest, verify_payload=False)
            try:
                staging.rename(bundle_dir)
                installed = True
            except FileExistsError:
                _verify_stage_bundle(bundle_dir, manifest, verify_payload=False)
        finally:
            if staging.exists():
                shutil.rmtree(staging)

    if replace_source:
        backup = stage_dir.with_name(
            f".{stage_dir.name}.{uuid.uuid4().hex}.retention-source"
        )
        prior_link_target = None
        if stage_dir.is_symlink():
            prior_link_target = os.readlink(stage_dir)
            stage_dir.unlink()
            backup = None
        else:
            stage_dir.rename(backup)
        try:
            relative_target = os.path.relpath(bundle_dir, start=stage_dir.parent)
            stage_dir.symlink_to(relative_target, target_is_directory=True)
        except Exception:
            if stage_dir.is_symlink():
                stage_dir.unlink()
            if backup is not None and backup.exists():
                backup.rename(stage_dir)
            elif prior_link_target is not None:
                stage_dir.symlink_to(prior_link_target, target_is_directory=True)
            raise
        if backup is not None:
            shutil.rmtree(backup)

    return {
        "schemaVersion": 1,
        "bundleDigest": manifest["bundleDigest"],
        "bundlePath": str(bundle_dir),
        "payloadBytes": manifest["payloadBytes"],
        "fileCount": len(manifest["files"]),
        "installed": installed,
        "duplicatePayloadBytes": 0,
        "storeMode": "content-addressed-hardlink",
        "runReferenceMode": "relative-symlink" if replace_source else "none",
        "workloadDigest": workload_digest,
    }


def write_artifact_retention(run_dir: Path, record: dict[str, Any]) -> None:
    (run_dir / "artifact-retention.json").write_text(
        json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def run_command(
    command: list[str], *, log_path: Path, timeout_s: int, env: dict[str, str] | None = None
) -> tuple[int, str]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    try:
        proc = subprocess.run(
            command,
            cwd=REPO,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout_s,
        )
        output = proc.stdout
        returncode = proc.returncode
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") + (
            "\n" if exc.stdout else ""
        ) + f"SPEC165_HARD_TIMEOUT timeout_s={timeout_s}\n"
        returncode = 124
    output += f"SPEC165_COMMAND_ELAPSED_S={time.monotonic() - started:.3f}\n"
    log_path.write_text(output, encoding="utf-8")
    return returncode, output


def base_record(
    *,
    case_id: str,
    gate_id: str,
    run_id: str,
    source_revision: str,
    tier: FidelityTier,
    command: list[str],
    started_at: str,
    status: str,
    failure_reason: str,
    real_components: list[str],
    simulated_components: list[str],
    network_mode: str,
    container_mode: str,
    model_identity: dict[str, Any],
    workload_digest: str,
    backend: str,
    evidence_paths: list[str],
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    record = {
        "schemaVersion": 1,
        "caseId": case_id,
        "gateId": gate_id,
        "runId": run_id,
        "startedAt": started_at,
        "completedAt": now_iso(),
        "status": status,
        "failureReason": failure_reason,
        "exactCommand": command_text(command),
        "sourceRevision": source_revision,
        "fidelityTier": tier.name,
        "realComponents": real_components,
        "simulatedComponents": simulated_components,
        "networkMode": network_mode,
        "containerMode": container_mode,
        "modelIdentity": model_identity,
        "workloadDigest": workload_digest,
        "hardwareProfile": {
            "hostname": platform.node(),
            "machine": platform.machine(),
            "kernel": platform.release(),
            "requestedBackend": backend,
        },
        "skipIsFailure": True,
        "evidencePaths": evidence_paths,
    }
    if extra:
        record.update(extra)
    return record


def write_record(path: Path, record: dict[str, Any]) -> dict[str, Any]:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return record


def unit_gate(
    *,
    case_id: str,
    gate_id: str,
    patterns: tuple[str, ...],
    run_dir: Path,
    run_id: str,
    source_revision: str,
    workload: dict[str, Any],
    backend: str,
) -> dict[str, Any]:
    command = [
        sys.executable,
        "-m",
        "unittest",
        "discover",
        "-v",
        "-s",
        "tests/python",
        "-p",
        patterns[0],
    ]
    # unittest accepts one glob. Run multiple patterns through an explicit
    # shell-free Python loader when a gate spans several files.
    if len(patterns) > 1:
        names = ",".join(patterns)
        code = (
            "import pathlib,unittest,sys;"
            f"p={names!r}.split(',');"
            "s=unittest.TestSuite();l=unittest.defaultTestLoader;"
            "[s.addTests(l.discover('tests/python',pattern=x)) for x in p];"
            "r=unittest.TextTestRunner(verbosity=2).run(s);"
            "sys.exit(0 if r.wasSuccessful() else 1)"
        )
        command = [sys.executable, "-c", code]
    started = now_iso()
    log = run_dir / gate_id.lower() / "test.log"
    returncode, _ = run_command(command, log_path=log, timeout_s=120)
    status = "PASS" if returncode == 0 else "FAIL"
    return write_record(
        run_dir / gate_id.lower() / "fidelity-record.json",
        base_record(
            case_id=case_id,
            gate_id=gate_id,
            run_id=run_id,
            source_revision=source_revision,
            tier=FidelityTier.UNIT,
            command=command,
            started_at=started,
            status=status,
            failure_reason="" if status == "PASS" else f"unit gate exit {returncode}",
            real_components=["Python", "Spec165 contract implementation"],
            simulated_components=["deterministic fixtures"],
            network_mode="none",
            container_mode="host",
            model_identity=workload["modelIdentity"],
            workload_digest=workload["workloadDigest"],
            backend=backend,
            evidence_paths=[str(log)],
        ),
    )


def minindn_command(
    *, run_dir: Path, snapshot: Path, workload: dict[str, Any], use_sudo: bool,
    routing_mode: str = "nlsr",
) -> list[str]:
    command = [
        sys.executable,
        "Experiments/NDNSF_DI_LlmPipeline_Minindn.py",
        "--topology-file",
        str(REPO / "Experiments/Topology/AI_Lab.conf"),
        "--output-dir",
        str(run_dir / "minindn" / "runtime"),
        "--stages",
        "3",
        "--runtime",
        "qwen-onnx",
        "--qwen-model",
        "Qwen/Qwen3-0.6B",
        "--qwen-revision",
        str(workload["modelIdentity"]["revision"]),
        "--qwen-dtype",
        "float32",
        "--qwen-execution-provider",
        str(workload["requestedBackend"]),
        "--nlsr-wait-s",
        "5",
        "--controller-wait-s",
        "5",
        "--provider-wait-s",
        "8",
        "--provider-start-timeout-s",
        "120",
        "--ack-timeout-ms",
        "1500",
        "--generation-campaign-manifest",
        str(run_dir / "generation-campaign.json"),
        "--generation-jsonl",
        str(run_dir / "minindn" / "generation.jsonl"),
        "--qwen-tokenizer-dir",
        str(snapshot),
        "--workload-digest",
        str(workload["workloadDigest"]),
        "--model-identity-digest",
        str(workload["modelIdentity"]["contentDigest"]),
        "--require-real-model",
        "--max-new-tokens",
        str(workload["maximumGeneratedTokens"]),
        "--timeout-ms",
        "600000",
        "--app-state-root",
        str(run_dir / "minindn" / "app-state"),
        "--test-only-allow-ephemeral-app-state",
        "--reuse-existing-policy",
    ]
    if routing_mode == "static":
        command.append("--static-routing-only")
    elif routing_mode != "nlsr":
        raise ValueError(f"unsupported MiniNDN routing mode: {routing_mode}")
    return ["sudo", "-E", *command] if use_sudo else command


def prepare_workload(
    *, run_dir: Path, snapshot: Path, backend: str, tooling_image: str
) -> dict[str, Any]:
    model_repo = snapshot.parents[1]
    inner_snapshot = (
        Path("/models/repo") / "snapshots" / snapshot.name
    )
    inner_run = Path("/workspace") / run_dir.relative_to(REPO)
    command = [
        "docker",
        "run",
        "--rm",
        "--entrypoint",
        "python3",
        "--user",
        f"{os.getuid()}:{os.getgid()}",
        "--memory=6g",
        "--memory-swap=8g",
        "-v",
        f"{REPO}:/workspace",
        "-v",
        f"{model_repo}:/models/repo:ro",
        "-w",
        "/workspace",
        "-e",
        (
            "PYTHONPATH=/workspace/NDNSF-DistributedInference:"
            "/workspace/pythonWrapper:/workspace/Experiments:"
            "/workspace/tools/ndnsf-di"
        ),
        "-e",
        "HOME=/tmp",
        tooling_image,
        "Experiments/NDNSF_DI_Prepare_Spec165_Workload.py",
        "--output-dir",
        str(inner_run),
        "--model-snapshot",
        str(inner_snapshot),
        "--backend",
        backend,
        "--minimum-generated-tokens",
        "8",
        "--maximum-generated-tokens",
        "8",
    ]
    returncode, _ = run_command(
        command, log_path=run_dir / "prepare.log", timeout_s=1800
    )
    if returncode:
        raise RuntimeError(f"workload preparation failed with exit {returncode}")
    workload = json.loads(
        (run_dir / "workload.json").read_text(encoding="utf-8"))
    # The container path is an execution detail. Bind evidence to the
    # operator-visible immutable snapshot used by the host and container.
    workload["modelIdentity"]["localSnapshot"] = str(snapshot)
    body = {key: value for key, value in workload.items()
            if key not in {"workloadDigest", "modelManifest"}}
    workload["workloadDigest"] = digest_value(body)
    write_workload(run_dir / "workload.json", workload)
    campaign_path = run_dir / "generation-campaign.json"
    campaign = json.loads(campaign_path.read_text(encoding="utf-8"))
    campaign["workloadDigest"] = workload["workloadDigest"]
    campaign["model"] = workload["modelIdentity"]
    campaign_path.write_text(
        json.dumps(campaign, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return workload


def prepare_policy_in_container(
    *,
    run_dir: Path,
    snapshot: Path,
    tooling_image: str,
    workload: dict[str, Any],
) -> None:
    model_repo = snapshot.parents[1]
    inner_snapshot = Path("/models/repo") / "snapshots" / snapshot.name
    inner_run = Path("/workspace") / run_dir.relative_to(REPO)
    output = inner_run / "minindn" / "runtime"
    command = [
        "docker",
        "run",
        "--rm",
        "--entrypoint",
        "python3",
        "--user",
        f"{os.getuid()}:{os.getgid()}",
        "--memory=8g",
        "--memory-swap=12g",
        "-v",
        f"{REPO}:/workspace",
        "-v",
        f"{model_repo}:/models/repo:ro",
        "-w",
        "/workspace",
        "-e",
        (
            "PYTHONPATH=/workspace/NDNSF-DistributedInference:"
            "/workspace/pythonWrapper:/workspace/Experiments:"
            "/workspace/tools/ndnsf-di"
        ),
        "-e",
        "HOME=/tmp",
        tooling_image,
        "examples/python/NDNSF-DistributedInference/llm_pipeline/plan_pipeline.py",
        "--policy",
        str(output / "llm_pipeline_policy.yaml"),
        "--service",
        "/AI/LLM/Pipeline/Fake",
        "--stages",
        "3",
        "--layers",
        "24",
        "--controller",
        "/example/llm-pipeline/controller",
        "--group",
        "/example/llm-pipeline/group",
        "--user",
        "/example/llm-pipeline/user",
        "--provider-prefix",
        "/example/llm-pipeline/provider",
        "--runtime",
        "qwen-onnx",
        "--transformer-layers",
        "4",
        "--qwen-model",
        str(inner_snapshot),
        "--qwen-revision",
        str(workload["modelIdentity"]["revision"]),
        "--qwen-prompt",
        str(workload["prompts"][0]["text"]),
        "--qwen-dtype",
        "float32",
        "--trust-app-root",
        "/example/llm-pipeline",
    ]
    returncode, _ = run_command(
        command, log_path=run_dir / "policy-prepare.log", timeout_s=1800
    )
    if returncode:
        raise RuntimeError(
            f"Qwen stage preparation failed with exit {returncode}")
    output_dir = run_dir / "minindn" / "runtime"
    for generated in (
        output_dir / "llm_pipeline_policy.yaml",
        output_dir / "qwen-pipeline-runtime.json",
        output_dir / "qwen-onnx-service-manifest.json",
    ):
        if generated.is_file():
            generated.write_text(
                generated.read_text(encoding="utf-8").replace(
                    "/workspace/", str(REPO) + "/"
                ),
                encoding="utf-8",
            )


def reuse_prepared_run(
    *,
    source_run: Path,
    run_dir: Path,
    snapshot: Path,
    artifact_store_root: Path,
) -> dict[str, Any]:
    source_run = source_run.resolve()
    source_runtime = source_run / "minindn" / "runtime"
    source_stages = source_runtime / "qwen-onnx-stage-artifacts"
    required = (
        source_run / "workload.json",
        source_run / "generation-campaign.json",
        source_runtime / "llm_pipeline_policy.yaml",
        source_runtime / "qwen-pipeline-runtime.json",
        source_runtime / "qwen-onnx-service-manifest.json",
    )
    if any(not path.is_file() for path in required) or not source_stages.is_dir():
        raise RuntimeError(
            f"prepared run is incomplete and cannot be reused: {source_run}")
    shutil.copy2(source_run / "workload.json", run_dir / "workload.json")
    shutil.copy2(
        source_run / "generation-campaign.json",
        run_dir / "generation-campaign.json",
    )
    workload = json.loads(
        (run_dir / "workload.json").read_text(encoding="utf-8"))
    if workload["modelIdentity"]["localSnapshot"] != str(snapshot):
        raise RuntimeError("prepared run model snapshot identity mismatch")
    retention = retain_stage_artifacts(
        stage_dir=source_stages,
        artifact_store_root=artifact_store_root,
        model_content_digest=workload["modelIdentity"]["contentDigest"],
        workload_digest=workload["workloadDigest"],
        replace_source=False,
    )
    target_runtime = run_dir / "minindn" / "runtime"
    target_runtime.mkdir(parents=True, exist_ok=True)
    target_stages = target_runtime / "qwen-onnx-stage-artifacts"
    bundle_dir = Path(retention["bundlePath"])
    target_stages.symlink_to(
        os.path.relpath(bundle_dir, start=target_stages.parent),
        target_is_directory=True,
    )
    for name in (
        "llm_pipeline_policy.yaml",
        "qwen-pipeline-runtime.json",
        "qwen-onnx-service-manifest.json",
    ):
        source = source_runtime / name
        text = source.read_text(encoding="utf-8")
        text = text.replace("/workspace/", str(REPO) + "/")
        text = text.replace(str(source_runtime), str(target_runtime))
        (target_runtime / name).write_text(text, encoding="utf-8")
    retention["runReferenceMode"] = "relative-symlink"
    write_artifact_retention(run_dir, retention)
    (run_dir / "preparation-reuse.json").write_text(
        json.dumps(
            {
                "schemaVersion": 2,
                "sourceRun": str(source_run),
                "stageArtifactDirectory": str(bundle_dir),
                "bundleDigest": retention["bundleDigest"],
                "duplicatePayloadBytes": 0,
                "modelContentDigest": workload["modelIdentity"]["contentDigest"],
                "workloadDigest": workload["workloadDigest"],
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )
    return workload


def run_real_gate(
    *,
    case_id: str,
    gate_id: str,
    tier: FidelityTier,
    command: list[str],
    run_dir: Path,
    run_id: str,
    source_revision: str,
    workload: dict[str, Any],
    backend: str,
    generation_path: Path,
    timeout_s: int,
    container_mode: str,
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    started = now_iso()
    log = run_dir / gate_id.lower() / "command.log"
    returncode, _ = run_command(command, log_path=log, timeout_s=timeout_s)
    failure = ""
    summary: dict[str, Any] = {}
    if returncode == 0:
        try:
            summary = validate_generation_evidence(
                read_jsonl(generation_path), workload=workload
            )
            if gate_id == "B":
                runtime_dir = generation_path.parent / "runtime"
                user_log = runtime_dir / "llm-pipeline-user.log"
                if not user_log.is_file():
                    raise RuntimeError(f"MiniNDN user evidence is absent: {user_log}")
                user_text = user_log.read_text(encoding="utf-8", errors="replace")
                if "LLM_PIPELINE_GENERATION_CAMPAIGN_PASS" not in user_text:
                    raise RuntimeError(
                        "MiniNDN real-model campaign completion marker is absent"
                    )
                roles = {
                    match.group(1)
                    for match in re.finditer(
                        r"NDNSF_COLLAB_ASSIGNMENT_SELECTED .*?role=(/LLM/Pipeline/Stage/\d+)",
                        user_text,
                    )
                }
                expected_roles = {
                    f"/LLM/Pipeline/Stage/{index}" for index in range(3)
                }
                if roles != expected_roles:
                    raise RuntimeError(
                        "MiniNDN selection evidence does not cover all roles: "
                        f"expected={sorted(expected_roles)} observed={sorted(roles)}"
                    )
                stage_outputs: dict[str, int] = {}
                for index in range(3):
                    stage_log = runtime_dir / f"stage{index}-provider.log"
                    if not stage_log.is_file():
                        raise RuntimeError(f"MiniNDN provider evidence is absent: {stage_log}")
                    stage_text = stage_log.read_text(
                        encoding="utf-8", errors="replace"
                    )
                    if "LLM_PIPELINE_QWEN_ONNX_STAGE_ARTIFACT_READY" not in stage_text:
                        raise RuntimeError(
                            f"MiniNDN stage {index} lacks artifact readiness evidence"
                        )
                    full_generation = (
                        "LLM_PIPELINE_QWEN_FULL_STAGE_START" in stage_text
                    )
                    marker = (
                        (
                            "LLM_PIPELINE_QWEN_FULL_GENERATION_FINAL"
                            if index == 0
                            else "LLM_PIPELINE_QWEN_FULL_HIDDEN_PUBLISHED"
                            if index == 1
                            else "LLM_PIPELINE_QWEN_FULL_TOKEN_PUBLISHED"
                        )
                        if full_generation else
                        "LLM_PIPELINE_QWEN_ONNX_STAGE_FINAL"
                        if index == 2
                        else "LLM_PIPELINE_QWEN_ONNX_STAGE_OUTPUT"
                    )
                    count = stage_text.count(marker)
                    if count == 0:
                        raise RuntimeError(
                            f"MiniNDN stage {index} lacks execution output evidence"
                        )
                    stage_outputs[str(index)] = count
                summary["minindnRealModelContract"] = {
                    "selectedRoles": sorted(roles),
                    "stageOutputEvents": stage_outputs,
                    "campaignMarker": True,
                }
        except Exception as exc:
            failure = str(exc)
    else:
        failure = f"command exit {returncode}"
    status = "PASS" if not failure else "FAIL"
    return write_record(
        run_dir / gate_id.lower() / "fidelity-record.json",
        base_record(
            case_id=case_id,
            gate_id=gate_id,
            run_id=run_id,
            source_revision=source_revision,
            tier=tier,
            command=command,
            started_at=started,
            status=status,
            failure_reason=failure,
            real_components=[
                "Qwen3 weights",
                "NDNSF collaboration",
                "MiniNDN",
                "NFD",
                "three Provider roles",
            ],
            simulated_components=[],
            network_mode="minindn",
            container_mode=container_mode,
            model_identity=workload["modelIdentity"],
            workload_digest=workload["workloadDigest"],
            backend=backend,
            evidence_paths=[str(log), str(generation_path)],
            extra={**(extra or {}), "generationSummary": summary},
        ),
    )


def image_identity(image: str) -> str:
    proc = subprocess.run(
        ["docker", "image", "inspect", "--format", "{{.Id}}", image],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode or not proc.stdout.strip().startswith("sha256:"):
        raise RuntimeError(f"candidate image is unavailable: {image}")
    return proc.stdout.strip()


def prepare_container_policy(run_dir: Path) -> None:
    """Map the host-prepared immutable ONNX policy into the candidate path."""
    source_runtime = run_dir / "minindn" / "runtime"
    target_runtime = run_dir / "container" / "runtime"
    target_runtime.mkdir(parents=True, exist_ok=True)
    source_artifacts = (
        source_runtime / "qwen-onnx-stage-artifacts").resolve(strict=True)
    container_artifacts = (
        Path("/workspace") / source_artifacts.relative_to(REPO))
    (target_runtime / "qwen-onnx-stage-artifacts").symlink_to(
        container_artifacts, target_is_directory=True)
    inner_target = Path("/workspace") / target_runtime.relative_to(REPO)
    for name in (
        "llm_pipeline_policy.yaml",
        "qwen-pipeline-runtime.json",
        "qwen-onnx-service-manifest.json",
    ):
        source = source_runtime / name
        if not source.is_file():
            raise RuntimeError(
                f"container policy source is missing: {source}")
        text = source.read_text(encoding="utf-8")
        text = text.replace(str(source_runtime), str(inner_target))
        text = text.replace(str(REPO), "/workspace")
        (target_runtime / name).write_text(text, encoding="utf-8")


def container_command(
    *,
    image: str,
    run_dir: Path,
    snapshot: Path,
    workload: dict[str, Any],
    memory: str,
    memory_swap: str,
) -> list[str]:
    relative_run = run_dir.relative_to(REPO)
    inner_run = Path("/workspace") / relative_run
    model_repo = snapshot.parents[1]
    inner_snapshot = Path("/models/repo") / "snapshots" / snapshot.name
    inner = minindn_command(
        run_dir=inner_run,
        snapshot=inner_snapshot,
        workload=workload,
        use_sudo=False,
    )
    # The host runner uses /usr/bin/python3, while the sealed candidate keeps
    # its compatible NDNSF/Qwen stack in /opt/venv.  Resolve through the image
    # PATH and enter the role-based image entrypoint explicitly.
    inner[0] = "python3"
    # The host command is constructed from the repository's absolute path.
    # Inside the candidate image the repository is mounted at /workspace, so
    # translate the topology path as well; otherwise MiniNDN parses a missing
    # file and reports the misleading "No section: 'nodes'" error.
    topology_index = inner.index("--topology-file") + 1
    inner[topology_index] = "/workspace/Experiments/Topology/AI_Lab.conf"
    # Keep container evidence distinct while consuming the same campaign bytes.
    generation_index = inner.index("--generation-jsonl") + 1
    inner[generation_index] = str(inner_run / "container" / "generation.jsonl")
    output_index = inner.index("--output-dir") + 1
    inner[output_index] = str(inner_run / "container" / "runtime")
    state_index = inner.index("--app-state-root") + 1
    inner[state_index] = str(inner_run / "container" / "app-state")
    # The sealed CPU candidate has materially higher scheduling/ONNX startup
    # variance than the host profile.  Keep the same workload and deadline,
    # but give control-plane ACK collection a bounded 5 s window so a slow
    # candidate is not rejected before it can advertise all three roles.
    ack_index = inner.index("--ack-timeout-ms") + 1
    inner[ack_index] = "5000"
    inner.extend(("--static-routing-only", "--nlsr-wait-s", "1"))
    return [
        "docker",
        "run",
        "--rm",
        "--privileged",
        "--user",
        "0:0",
        f"--memory={memory}",
        f"--memory-swap={memory_swap}",
        "-v",
        f"{REPO}:/workspace",
        "-v",
        f"{model_repo}:/models/repo:ro",
        "-v",
        "/home/tianxing/NDN/mini-ndn:/opt/mini-ndn:ro",
        "-w",
        "/workspace",
        "-e",
        (
            "PYTHONPATH=/workspace/NDNSF-DistributedInference:"
            "/opt/ndnsf-app/python:/workspace/pythonWrapper:"
            "/workspace/Experiments:"
            "/workspace/tools/ndnsf-di:/opt/mini-ndn"
        ),
        "-e",
        "HOME=/tmp/spec165",
        "-e",
        "SHELL=/bin/bash",
        "-e",
        "NDNSF_ALLOW_CPU_FALLBACK=1",
        "-e",
        "NDNSF_PREFER_INSTALLED_NATIVE=1",
        image,
        "exec",
        "bash",
        "-lc",
        (
            "export PATH=/opt/mini-ndn/dl/infoedit:$PATH; "
            "service openvswitch-switch start "
            ">/tmp/spec165-openvswitch.log 2>&1 && exec "
            + command_text(inner)
        ),
    ]


def write_summary(run_dir: Path, verdict: dict[str, Any]) -> None:
    (run_dir / "aggregate-verdict.json").write_text(
        json.dumps(verdict, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    lines = [
        "# Spec 165 Local Gate Summary",
        "",
        f"- Run: `{verdict['runId']}`",
        f"- Passed: `{verdict['passed']}`",
        "- TigerCluster submitted: `false`",
        f"- External validation authorized: `{verdict['externalValidationAuthorized']}`",
    ]
    retention_path = run_dir / "artifact-retention.json"
    if retention_path.is_file():
        retention = json.loads(retention_path.read_text(encoding="utf-8"))
        lines.extend(
            (
                f"- Artifact bundle: `{retention['bundleDigest']}`",
                f"- Duplicate model payload bytes: `{retention['duplicatePayloadBytes']}`",
            )
        )
    lines.extend(
        (
            "",
            "| Case | Required fidelity | Status | Passed | Reasons |",
            "|---|---|---|---:|---|",
        )
    )
    for item in verdict["caseResults"]:
        lines.append(
            f"| {item['caseId']} | {item['requiredTier']} | {item['status']} | "
            f"{str(item['passed']).lower()} | {', '.join(item['reasons'])} |"
        )
    if verdict["errors"]:
        lines.extend(("", "## Integrity errors", ""))
        lines.extend(f"- {error}" for error in verdict["errors"])
    (run_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--gate",
        choices=("all", "fidelity", "deadline", "minindn", "container"),
        default="all",
    )
    parser.add_argument("--output-root", default="results/spec165-local-gates")
    parser.add_argument("--model-snapshot", default=str(DEFAULT_MODEL_SNAPSHOT))
    parser.add_argument("--backend", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument(
        "--minindn-routing",
        choices=("nlsr", "static"),
        default="nlsr",
        help="Real MiniNDN control-plane profile used by Gate B.",
    )
    parser.add_argument(
        "--candidate-image", default=os.environ.get("NDNSF_DI_CANDIDATE_IMAGE", "")
    )
    parser.add_argument(
        "--tooling-image",
        default=os.environ.get(
            "NDNSF_DI_TOOLING_IMAGE", "ndnsf-di:spec164-native-75a614424271"
        ),
        help="Local image used only for Qwen3 reference/stage preparation.",
    )
    parser.add_argument("--container-memory", default="8g")
    parser.add_argument("--container-memory-swap", default="10g")
    parser.add_argument(
        "--reuse-prepared-run",
        default="",
        help="Reuse immutable Qwen stage artifacts from an earlier local run.",
    )
    parser.add_argument(
        "--artifact-store-root",
        default=str(DEFAULT_ARTIFACT_STORE_ROOT.relative_to(REPO)),
        help=(
            "Repository-local content-addressed store. Prepared model payloads "
            "are hard-linked once; run directories retain relative links only."
        ),
    )
    args = parser.parse_args()

    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    run_dir = (REPO / args.output_root / run_id).resolve()
    run_dir.mkdir(parents=True, exist_ok=False)
    source_revision, source_manifest = source_identity()
    (run_dir / "source-manifest.json").write_text(
        json.dumps(source_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    snapshot = Path(args.model_snapshot).expanduser().resolve()
    artifact_store_root = Path(args.artifact_store_root).expanduser()
    if not artifact_store_root.is_absolute():
        artifact_store_root = REPO / artifact_store_root
    artifact_store_root = artifact_store_root.resolve()
    # A lightweight identity is sufficient for unit-only subsets. Real gates
    # replace it with the byte-resolved manifest produced by preparation.
    workload = canonical_workload(
        snapshot=snapshot,
        backend=args.backend,
        include_snapshot_manifest=False,
    )
    write_workload(run_dir / "workload.json", workload)
    records: list[dict[str, Any]] = []
    requested = (
        ("fidelity", "deadline", "minindn", "container")
        if args.gate == "all"
        else (args.gate,)
    )

    if "minindn" in requested or "container" in requested:
        try:
            if args.reuse_prepared_run:
                workload = reuse_prepared_run(
                    source_run=(REPO / args.reuse_prepared_run),
                    run_dir=run_dir,
                    snapshot=snapshot,
                    artifact_store_root=artifact_store_root,
                )
            else:
                workload = prepare_workload(
                    run_dir=run_dir,
                    snapshot=snapshot,
                    backend=args.backend,
                    tooling_image=args.tooling_image,
                )
                prepare_policy_in_container(
                    run_dir=run_dir,
                    snapshot=snapshot,
                    tooling_image=args.tooling_image,
                    workload=workload,
                )
                retention = retain_stage_artifacts(
                    stage_dir=run_dir
                    / "minindn"
                    / "runtime"
                    / "qwen-onnx-stage-artifacts",
                    artifact_store_root=artifact_store_root,
                    model_content_digest=workload["modelIdentity"]["contentDigest"],
                    workload_digest=workload["workloadDigest"],
                    replace_source=True,
                )
                write_artifact_retention(run_dir, retention)
        except Exception as exc:
            # Do not skip mandatory real gates. Emit explicit failed records for
            # every requested real case so aggregate accounting stays complete.
            for name, gate, tier in (
                ("gate-b-minindn", "B", FidelityTier.REAL_MININDN_MODEL),
                (
                    "gate-c-container",
                    "C",
                    FidelityTier.REAL_CANDIDATE_CONTAINER_MODEL,
                ),
            ):
                if (gate == "B" and "minindn" not in requested) or (
                    gate == "C" and "container" not in requested
                ):
                    continue
                records.append(
                    write_record(
                        run_dir / gate.lower() / "fidelity-record.json",
                        base_record(
                            case_id=name,
                            gate_id=gate,
                            run_id=run_id,
                            source_revision=source_revision,
                            tier=tier,
                            command=["workload-preparation"],
                            started_at=now_iso(),
                            status="FAIL",
                            failure_reason=str(exc),
                            real_components=["workload preparation"],
                            simulated_components=[],
                            network_mode="minindn",
                            container_mode="host" if gate == "B" else "docker",
                            model_identity=workload["modelIdentity"],
                            workload_digest=workload["workloadDigest"],
                            backend=args.backend,
                            evidence_paths=[str(run_dir / "prepare.log")],
                        ),
                    )
                )
        else:
            if "minindn" in requested:
                command = minindn_command(
                    run_dir=run_dir,
                    snapshot=snapshot,
                    workload=workload,
                    use_sudo=True,
                    routing_mode=args.minindn_routing,
                )
                records.append(
                    run_real_gate(
                        case_id="gate-b-minindn",
                        gate_id="B",
                        tier=FidelityTier.REAL_MININDN_MODEL,
                        command=command,
                        run_dir=run_dir,
                        run_id=run_id,
                        source_revision=source_revision,
                        workload=workload,
                        backend=args.backend,
                        generation_path=run_dir / "minindn" / "generation.jsonl",
                        timeout_s=3600,
                        container_mode="host",
                    )
                )
            if "container" in requested:
                failure = ""
                image_digest = ""
                try:
                    if not args.candidate_image:
                        raise RuntimeError(
                            "--candidate-image or NDNSF_DI_CANDIDATE_IMAGE is required"
                        )
                    image_digest = image_identity(args.candidate_image)
                    prepare_container_policy(run_dir)
                    command = container_command(
                        image=args.candidate_image,
                        run_dir=run_dir,
                        snapshot=snapshot,
                        workload=workload,
                        memory=args.container_memory,
                        memory_swap=args.container_memory_swap,
                    )
                    records.append(
                        run_real_gate(
                            case_id="gate-c-container",
                            gate_id="C",
                            tier=FidelityTier.REAL_CANDIDATE_CONTAINER_MODEL,
                            command=command,
                            run_dir=run_dir,
                            run_id=run_id,
                            source_revision=source_revision,
                            workload=workload,
                            backend=args.backend,
                            generation_path=run_dir
                            / "container"
                            / "generation.jsonl",
                            timeout_s=3600,
                            container_mode="docker",
                            extra={
                                "imageIdentity": image_digest,
                                "resourceLimits": {
                                    "memory": args.container_memory,
                                    "memorySwap": args.container_memory_swap,
                                },
                                "oomKilled": False,
                            },
                        )
                    )
                except Exception as exc:
                    failure = str(exc)
                if failure:
                    records.append(
                        write_record(
                            run_dir / "c" / "fidelity-record.json",
                            base_record(
                                case_id="gate-c-container",
                                gate_id="C",
                                run_id=run_id,
                                source_revision=source_revision,
                                tier=FidelityTier.REAL_CANDIDATE_CONTAINER_MODEL,
                                command=["candidate-container-preflight"],
                                started_at=now_iso(),
                                status="FAIL",
                                failure_reason=failure,
                                real_components=["Docker candidate preflight"],
                                simulated_components=[],
                                network_mode="minindn",
                                container_mode="docker",
                                model_identity=workload["modelIdentity"],
                                workload_digest=workload["workloadDigest"],
                                backend=args.backend,
                                evidence_paths=[],
                                extra={
                                    "imageIdentity": image_digest,
                                    "resourceLimits": {
                                        "memory": args.container_memory,
                                        "memorySwap": args.container_memory_swap,
                                    },
                                    "oomKilled": False,
                                },
                            ),
                        )
                    )

    # Unit-contract evidence must bind to the same byte-resolved workload as
    # the real gates.  Run it after successful preparation/reuse so an all-gate
    # aggregate cannot mix placeholder and resolved model identities.
    if "fidelity" in requested:
        records.append(
            unit_gate(
                case_id="gate-a-fidelity",
                gate_id="A",
                patterns=(
                    "test_spec165_validation_fidelity.py",
                    "test_spec165_workload.py",
                    "test_spec165_generation_evidence.py",
                ),
                run_dir=run_dir,
                run_id=run_id,
                source_revision=source_revision,
                workload=workload,
                backend=args.backend,
            )
        )
    if "deadline" in requested:
        records.append(
            unit_gate(
                case_id="gate-d-deadline",
                gate_id="D",
                patterns=(
                    "test_spec165_progress_deadline.py",
                    "test_spec165_lineage.py",
                ),
                run_dir=run_dir,
                run_id=run_id,
                source_revision=source_revision,
                workload=workload,
                backend=args.backend,
            )
        )

    selected_policy = {
        case: tier
        for case, tier in MANDATORY_TIERS.items()
        if args.gate == "all"
        or case
        == {
            "fidelity": "gate-a-fidelity",
            "minindn": "gate-b-minindn",
            "container": "gate-c-container",
            "deadline": "gate-d-deadline",
        }[args.gate]
    }
    verdict = aggregate_records(
        records,
        GatePolicy(
            schema_version=1,
            source_revision=source_revision,
            run_id=run_id,
            mandatory_cases=selected_policy,
            model_identity_digest=workload["modelIdentity"]["contentDigest"],
            workload_digest=workload["workloadDigest"],
            authorization_case_ids=frozenset(MANDATORY_TIERS),
        ),
    )
    verdict["tigerClusterSubmitted"] = False
    write_summary(run_dir, verdict)
    print(
        "NDNSF_SPEC165_LOCAL_GATE_" + ("PASS" if verdict["passed"] else "FAIL"),
        f"run={run_id}",
        f"output={run_dir}",
        "tigerClusterSubmitted=false",
        flush=True,
    )
    return 0 if verdict["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
