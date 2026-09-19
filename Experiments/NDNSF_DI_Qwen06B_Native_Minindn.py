#!/usr/bin/env python3
"""Run a real Qwen3-0.6B CPU native request through a MiniNDN topology.

The Python process owns only topology, identities, files, and child-process
lifecycle.  Request planning, grants, Provider admission, ONNX execution,
streaming, and conversation checkpoints stay in the C++ executables.

The staged ONNX files are role artifacts.  A canonical ONNX source object must
be supplied explicitly with ``--canonical-source``; the runner never creates a
JSON metadata payload in its place.
"""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import math
import os
import shlex
import signal
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from cryptography.hazmat.primitives.serialization import (
    Encoding,
    PublicFormat,
    load_pem_public_key,
)
from cryptography.hazmat.backends import default_backend

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "Experiments"))
from native_resource_guard import run_guarded, validate_limits
ROLE_PREFIX = "/LLM/Pipeline/Stage/"
SERVICE = "/AI/LLM/Pipeline/QwenNative"
GROUP = "/example/ndnsf-qwen06b/group"
APP_ROOT = "/example/ndnsf-qwen06b"
CONTROLLER = APP_ROOT + "/controller"
AUTHORITY = APP_ROOT + "/authority"
USER = APP_ROOT + "/user"
NATIVE_CONVERSATION_JOURNAL_MAX_BYTES = 64 * 1024 * 1024
# ModelPreparationCache reserves one source slot for the ONNX graph and one
# for a separate external initializer, then adds the assembled package and
# parser/configuration scratch.  Keep the formula in one place so a large
# real model cannot be rejected by an undersized experiment-generated limit.
PREPARATION_CACHE_SCRATCH_BYTES = 4 * 1024 * 1024
PUBLICATION_ROOT_MARGIN_BYTES = 16 * 1024 * 1024
LOCAL_ASSEMBLY_DISK_MARGIN_BYTES = 256 * 1024 * 1024
LARGE_DATA_SEGMENT_BYTES = 7000
LARGE_DATA_DEFAULT_IMS_LIMIT = 50000
LARGE_DATA_IMS_MARGIN_SEGMENTS = 8192
LARGE_DATA_IMS_MAX_LIMIT = 1000000
LARGE_MODEL_THRESHOLD_BYTES = 256 * 1024 * 1024
# Keep operator-controlled startup waits finite.  These values are generous
# enough for a slow local MiniNDN launch while preventing a malformed CLI
# value from turning the generated service envelope into an unbounded wait.
MAX_STARTUP_TIMEOUT_S = 600.0
MAX_NLSR_WAIT_S = 120.0
MAX_QWEN_ROUNDS = 8
MAX_STAGE_COUNT = 32


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def digest_text(value: str) -> str:
    return digest_bytes(value.encode("utf-8"))


def preparation_cache_budget(max_source_bytes: int,
                             max_assembled_bytes: int) -> int:
    """Return the requester preparation reservation for the native contract.

    ``ModelPreparationCache`` reserves two bounded source objects (canonical
    graph plus optional external initializer), one assembled object, 256 KiB
    of native scratch, and small JSON/configuration overhead.  The additional
    four MiB margin keeps the generated config independent of incidental JSON
    formatting while remaining finite and model-specific.
    """
    if max_source_bytes <= 0 or max_assembled_bytes <= 0:
        raise ValueError("preparation cache limits must be positive")
    return (2 * max_source_bytes + max_assembled_bytes +
            PREPARATION_CACHE_SCRATCH_BYTES)


def large_data_ims_limit(source_bytes: int, initializer_bytes: int) -> int:
    """Reserve enough bounded IMS entries for this run's canonical objects.

    ``ServiceUser`` keeps the historical 50,000-entry limit unless the
    requester explicitly opts in through ``NDNSF_REQUEST_LARGE_IMS_LIMIT``.
    The limit covers the graph, optional external initializer, envelope/TLV
    overhead, and a finite control-packet margin.  Refuse a value above the
    native hard ceiling instead of silently starting a run that will evict
    early segments and fail much later in ``SegmentFetcher``.
    """
    if source_bytes <= 0 or initializer_bytes < 0:
        raise ValueError("large-data object sizes must be non-negative")
    estimated_bytes = (source_bytes + initializer_bytes +
                       16 * 1024 * 1024)
    segments = ((estimated_bytes + LARGE_DATA_SEGMENT_BYTES - 1) //
                LARGE_DATA_SEGMENT_BYTES)
    required = segments + LARGE_DATA_IMS_MARGIN_SEGMENTS
    limit = max(LARGE_DATA_DEFAULT_IMS_LIMIT, required)
    if limit > LARGE_DATA_IMS_MAX_LIMIT:
        raise ValueError("Qwen canonical objects exceed the native IMS hard ceiling")
    return limit


def qwen_runtime_budgets(source_bytes: int, initializer_bytes: int,
                         rounds: int = 2, stage_count: int = 3,
                         startup_timeout_s: float = 60.0,
                         nlsr_wait_s: float = 8.0) -> dict[str, int]:
    """Scale preparation/request deadlines only for genuinely large objects.

    The old 30-second bootstrap is suitable for small fixtures but expires
    while the native ONNX identity loader is parsing a real external
    initializer.  Keep ordinary runs bounded and make the large-model budget
    explicit in the generated config and evidence.
    """
    if (source_bytes <= 0 or initializer_bytes < 0 or rounds < 1 or
            rounds > MAX_QWEN_ROUNDS or stage_count < 1 or
            stage_count > MAX_STAGE_COUNT):
        raise ValueError("Qwen runtime budget inputs are outside supported bounds")
    if (not math.isfinite(startup_timeout_s) or not math.isfinite(nlsr_wait_s) or
            startup_timeout_s < 0 or nlsr_wait_s < 0 or
            startup_timeout_s > MAX_STARTUP_TIMEOUT_S or
            nlsr_wait_s > MAX_NLSR_WAIT_S):
        raise ValueError(
            "Qwen startup budgets must be finite and within the supported bounds")
    total = source_bytes + initializer_bytes
    if total >= LARGE_MODEL_THRESHOLD_BYTES:
        bootstrap_ms, timeout_ms, ack_timeout_ms, no_progress_ms, policy_ms = (
            600000, 900000, 60000, 180000, 60000)
        base_service_ms = 1200000
    else:
        bootstrap_ms, timeout_ms, ack_timeout_ms, no_progress_ms, policy_ms = (
            60000, 180000, 30000, 30000, 5000)
        base_service_ms = 300000
    # The turn deadline is written before MiniNDN startup. It must cover the
    # worst-case startup markers, one preparation window, every configured
    # request round, and a bounded commit/cleanup margin. Provider and
    # Authority lifetimes use the same envelope so they cannot exit midway
    # through a later conversation round.
    # Each fresh requester performs one synchronous Runtime.open/freeze phase
    # and one User.prepare phase before submitting its request. Both phases
    # are bounded by the same large-model bootstrap budget in the native
    # preparation path, so account for both instead of killing a valid run at
    # the first phase boundary.
    per_round_bootstrap_ms = 2 * bootstrap_ms
    process_timeout_s = (per_round_bootstrap_ms + timeout_ms + 120000 + 999) // 1000
    startup_ms = int((nlsr_wait_s + startup_timeout_s * (2 + stage_count)) * 1000) + 999
    per_round_ms = per_round_bootstrap_ms + timeout_ms
    lifecycle_ms = startup_ms + rounds * per_round_ms + 120000
    service_ms = max(base_service_ms, lifecycle_ms)
    return {"bootstrap_ms": bootstrap_ms, "timeout_ms": timeout_ms,
            "ack_timeout_ms": ack_timeout_ms, "no_progress_ms": no_progress_ms,
            "policy_ms": policy_ms,
            "process_timeout_s": process_timeout_s, "provider_run_ms": service_ms,
            "authority_run_ms": service_ms, "retention_ms": service_ms}


def qwen_state_successor_pairs(stages: list[dict]) -> list[tuple[str, str]]:
    """Validate and derive semantic past->present pairs for every stage.

    A positional zip is unsafe: a manifest can keep the same input/output
    sets while swapping a key or value tensor.  Qwen's state contract is
    name-based and every layer must carry one key and one value successor.
    """
    pairs: list[tuple[str, str]] = []
    previous_end = 0
    for stage_index, stage in enumerate(stages):
        layer_range = stage.get("layerRange")
        if isinstance(layer_range, dict):
            layer_start = int(layer_range.get("start", -1))
            layer_end = int(layer_range.get("endExclusive", -1))
        elif isinstance(layer_range, list) and len(layer_range) == 2:
            layer_start, layer_end = map(int, layer_range)
        else:
            layer_start, layer_end = -1, -1
        if layer_start != previous_end or layer_end <= previous_end:
            raise ValueError("QWEN_LAYER_RANGES_NOT_CONTIGUOUS")
        previous_end = layer_end
        inputs = list(stage.get("cacheInputs", []))
        outputs = list(stage.get("cacheOutputs", []))
        if (not inputs or not outputs or len(inputs) != len(set(inputs)) or
                len(outputs) != len(set(outputs))):
            raise ValueError(f"QWEN_STATE_CONTRACT_INVALID:{stage_index}")

        def indexed(names: list[str], prefix: str) -> dict[str, dict[int, str]]:
            result: dict[str, dict[int, str]] = {"key": {}, "value": {}}
            for name in names:
                parts = name.split(".")
                if len(parts) != 2 or parts[0] not in {prefix + "_key", prefix + "_value"}:
                    raise ValueError(f"QWEN_STATE_NAME_INVALID:{stage_index}:{name}")
                try:
                    layer = int(parts[1])
                except ValueError as exc:
                    raise ValueError(f"QWEN_STATE_NAME_INVALID:{stage_index}:{name}") from exc
                family = "key" if parts[0].endswith("_key") else "value"
                if layer < 0 or layer in result[family]:
                    raise ValueError(f"QWEN_STATE_NAME_INVALID:{stage_index}:{name}")
                result[family][layer] = name
            return result

        past = indexed(inputs, "past")
        present = indexed(outputs, "present")
        expected = set(range(layer_start, previous_end))
        # The exported stages use local layer indexes.  Require both key/value
        # families for exactly the same layer set; order is irrelevant.
        past_keys = set(past["key"])
        past_values = set(past["value"])
        present_keys = set(present["key"])
        present_values = set(present["value"])
        if not (past_keys == past_values == present_keys == present_values == expected):
            raise ValueError(f"QWEN_STATE_LAYER_COVER_INVALID:{stage_index}")
        for layer in sorted(expected):
            pairs.extend([(past["key"][layer], present["key"][layer]),
                          (past["value"][layer], present["value"][layer])])
    return pairs


def materialize_input(source: Path, destination: Path, expected_digest: str | None,
                      label: str, free_margin: int = LOCAL_ASSEMBLY_DISK_MARGIN_BYTES) -> dict:
    """Create a run-local immutable view without duplicating large artifacts.

    Read-only inputs on the same filesystem are hard-linked, so a local run
    does not need another 1.5 GiB initializer copy.  A cross-device fallback
    copies through a temporary file only after an explicit free-space check.
    The expected digest and stable inode tuple close the prepare-to-run TOCTOU
    boundary; writable sources are rejected because a hard link would share
    their mutable inode.
    """
    source = source.expanduser().resolve()
    destination = destination.expanduser().resolve()
    stat_before = source.stat()
    if not source.is_file() or stat_before.st_size == 0:
        raise RuntimeError(f"{label}_SOURCE_INVALID")
    if stat_before.st_mode & 0o222:
        raise RuntimeError(f"{label}_SOURCE_NOT_IMMUTABLE")
    actual = digest_file(source)
    if expected_digest and actual != expected_digest:
        raise RuntimeError(f"{label}_DIGEST_MISMATCH")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination_existed = destination.exists()
    method = "hardlink"
    try:
        try:
            os.link(source, destination)
        except OSError as exc:
            if exc.errno != errno.EXDEV:
                raise
            usage = shutil.disk_usage(destination.parent)
            required = stat_before.st_size + free_margin
            if usage.free < required:
                raise RuntimeError(f"{label}_DISK_SPACE_INSUFFICIENT") from exc
            method = "copy"
            temporary = destination.with_name(destination.name + ".partial")
            try:
                flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
                flags |= getattr(os, "O_NOFOLLOW", 0)
                fd = os.open(temporary, flags, 0o600)
                with os.fdopen(fd, "wb") as dst, source.open("rb") as src:
                    while True:
                        chunk = src.read(8 * 1024 * 1024)
                        if not chunk:
                            break
                        dst.write(chunk)
                    dst.flush()
                    os.fsync(dst.fileno())
                os.replace(temporary, destination)
            finally:
                temporary.unlink(missing_ok=True)
        stat_after = source.stat()
        if ((stat_after.st_dev, stat_after.st_ino, stat_after.st_size,
             stat_after.st_mtime_ns) !=
                (stat_before.st_dev, stat_before.st_ino, stat_before.st_size,
                 stat_before.st_mtime_ns)):
            raise RuntimeError(f"{label}_SOURCE_CHANGED")
        destination_digest = digest_file(destination)
        if expected_digest and destination_digest != expected_digest:
            raise RuntimeError(f"{label}_MATERIALIZED_DIGEST_MISMATCH")
        return {"path": str(destination), "sha256": destination_digest, "method": method,
                "bytes": stat_before.st_size}
    except Exception:
        if not destination_existed:
            destination.unlink(missing_ok=True)
        raise


def require_file_digest(path: Path, expected_digest: str | None, label: str) -> str:
    path = path.expanduser().resolve()
    if not path.is_file():
        raise SystemExit(f"{label}_MISSING:{path}")
    if not expected_digest:
        raise SystemExit(f"{label}_DIGEST_REQUIRED")
    actual = digest_file(path)
    if actual != expected_digest:
        raise SystemExit(f"{label}_DIGEST_MISMATCH")
    return actual


def canonical_source_graph_digest(path: Path) -> str:
    """Derive the source graph identity with the maintained ONNX oracle.

    The Qwen planner graph is a semantic, synthetic graph used for layer
    placement.  The catalog's canonical source identity must instead describe
    the actual ONNX bytes supplied to the native preparer.  Keeping this
    calculation in the maintained ONNX identity helper also avoids silently
    reusing the planner digest for a different identity domain.
    """
    package_root = ROOT / "NDNSF-DistributedInference"
    if str(package_root) not in sys.path:
        sys.path.insert(0, str(package_root))
    try:
        from ndnsf_distributed_inference.adapters.onnx.graph import (  # type: ignore
            canonical_onnx_identity,
        )
    except ImportError as exc:
        raise RuntimeError(
            "Qwen canonical source identity requires the maintained ONNX oracle"
        ) from exc
    return canonical_onnx_identity(path).graph_digest


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def qwen_graph_digest(model_name: str, revision: str,
                      precision: str, ranges: list[list[int]]) -> str:
    layers = int(ranges[-1][1])
    nodes = ["embedding"] + [
        f"layer-{index:02d}" for index in range(layers)
    ] + ["final-norm-head"]
    edges = []
    for index in range(1, len(nodes)):
        if index == 1:
            edge = "hidden-embedding-to-layer-00"
        elif index == len(nodes) - 1:
            edge = f"hidden-layer-{layers - 1}-to-final"
        else:
            edge = f"hidden-layer-{index - 2}-to-{index - 1}"
        edges.append(edge)
    return digest_bytes(canonical_bytes({
        "model": model_name,
        "revision": revision,
        "precision": precision,
        "decode_mode": "single-token-autoregressive",
        "modality": "text-only",
        "mtp_enabled": False,
        "thinking_mode": "disabled",
        "layer_ranges": ranges,
        "nodes": nodes,
        "edges": edges,
        "legal_cuts": edges,
    }))


def run_host(command: list[str], *, check: bool = True) -> None:
    subprocess.run(command, cwd=ROOT, check=check,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def make_external_key(directory: Path, stem: str) -> tuple[Path, Path]:
    directory.mkdir(parents=True, exist_ok=True)
    private = directory / f"{stem}-private.pem"
    public = directory / f"{stem}-public.pem"
    run_host(["/usr/bin/openssl", "genpkey", "-algorithm", "ED25519",
              "-out", str(private)])
    run_host(["/usr/bin/openssl", "pkey", "-in", str(private), "-pubout",
              "-out", str(public)])
    private.chmod(0o600)
    public.chmod(0o644)
    return private, public


def public_key_digest(path: Path) -> str:
    key = load_pem_public_key(path.read_bytes(), backend=default_backend())
    raw = key.public_bytes(Encoding.Raw, PublicFormat.Raw)
    return digest_bytes(raw)


def parse_csv_ints(value: str) -> list[int]:
    try:
        result = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as exc:
        raise ValueError(f"invalid integer list: {value}") from exc
    if not result:
        raise ValueError("integer list must not be empty")
    if any(item < 0 for item in result):
        raise ValueError("token IDs must be non-negative")
    return result


def tensor_bundle(token_ids: list[int]) -> bytes:
    payload = b"".join(struct.pack("<q", value) for value in token_ids)
    return (b"NDITB001" + struct.pack("<I", 1) + struct.pack("<I", 9)
            + b"input_ids" + struct.pack("<I", 3) + struct.pack("<I", 2)
            + struct.pack("<q", 1) + struct.pack("<q", len(token_ids))
            + struct.pack("<Q", len(payload))
            + payload)


def load_stage_manifest(path: Path, stage_root: Path | None):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    stages = manifest.get("stages")
    if (not isinstance(stages, list) or len(stages) < 2 or
            len(stages) > MAX_STAGE_COUNT):
        raise ValueError(
            f"Qwen stage manifest requires 2..{MAX_STAGE_COUNT} stages")
    resolved = []
    for stage in stages:
        role = str(stage.get("role", ""))
        expected = f"{ROLE_PREFIX}{len(resolved)}"
        if role != expected:
            raise ValueError(f"stage role must be {expected}, got {role}")
        raw = Path(str(stage.get("path", ""))).expanduser()
        candidates = [raw]
        if not raw.is_absolute():
            candidates.insert(0, path.parent / raw)
            if stage_root:
                candidates.insert(0, stage_root / raw.name)
        artifact = next((item.resolve() for item in candidates if item.is_file()), None)
        if artifact is None:
            raise ValueError(f"stage artifact is missing: {raw}")
        actual = digest_file(artifact)
        expected_digest = str(stage.get("sha256", ""))
        if expected_digest.startswith("sha256:"):
            expected_digest = expected_digest[7:]
        if actual[7:] != expected_digest:
            raise ValueError(f"stage digest mismatch: {artifact}")
        resolved.append({**stage, "path": str(artifact), "sha256": actual,
                         "bytes": artifact.stat().st_size})
    return manifest, resolved


def stage_plan_and_manifest(output: Path, model_manifest: dict, stages: list[dict],
                            max_tokens: int, tokenizer_digest: str):
    state_pairs = qwen_state_successor_pairs(stages)
    roles = [str(stage["role"]) for stage in stages]
    dependencies = []
    for index in range(len(roles) - 1):
        dependencies.append({
            "producers": [roles[index]], "consumers": [roles[index + 1]],
            "keyScope": f"pipeline-stage-{index}-to-{index + 1}",
            "topicPrefix": "/NDNSF/DI/QWEN",
            "objectNameTemplate": (
                "{producerProvider}/NDNSF/DI/QWEN/{sessionId}/"
                "{producerRole}/{consumerRole}/{sequence}"),
            "expectedSegments": 0, "expectedBytes": 0, "required": True,
            "segmentNaming": {"mode": "ndn-segment-component",
                               "staticSegmentCount": 0, "dynamicFallback": True},
            "tensors": ["hidden_states", "attention_mask", "position_ids"],
        })
    artifacts = []
    for index, stage in enumerate(stages):
        cache_inputs = list(stage.get("cacheInputs", []))
        cache_outputs = list(stage.get("cacheOutputs", []))
        input_names = list(stage["inputNames"])
        output_names = list(stage["outputNames"])
        if index > 0 and "hidden_states" in input_names:
            input_names[input_names.index("hidden_states")] = (
                f"qwen_s{index - 1}_hidden_states_out")
        if index < len(stages) - 1 and "hidden_states_out" in output_names:
            output_names[output_names.index("hidden_states_out")] = (
                f"qwen_s{index}_hidden_states_out")
        metadata = {
            "inputNames": ",".join(input_names),
            "outputNames": ",".join(output_names),
            "forceOutputBundle": "true",
            "executionProvider": "cpu",
            "allowCpuFallback": "false",
            "deviceId": "cpu0",
            "dtype": str(model_manifest.get("dtype", "float32")),
            "passthroughTensors": "attention_mask,position_ids",
            "kvTensorMap": ",".join(
                f"{a}={b}" for a, b in state_pairs
                if a in cache_inputs and b in cache_outputs),
            "kvOutputTensors": ",".join(cache_outputs),
            "kvOutputScope": "kv-state",
            "positionInputPolicy": "qwen-causal-position-v1",
            "attentionMaskInputName": "attention_mask",
            "positionIdsInputName": "position_ids",
            "outputBundleScope": "final-response" if index == len(stages) - 1 else f"pipeline-stage-{index}-to-{index + 1}",
            "streamingGeneration": "true",
            "statefulModel": "true",
            "stateInputNames": ",".join(cache_inputs),
            "stateOutputNames": ",".join(cache_outputs),
            "maxGeneratedTokens": str(max_tokens),
            "eosTokenIds": "151645",
            "tokenizerDigest": tokenizer_digest,
        }
        if index > 0:
            for name in (f"qwen_s{index - 1}_hidden_states_out",
                         "attention_mask", "position_ids"):
                metadata[f"inputScope.{name}"] = f"pipeline-stage-{index - 1}-to-{index}"
        artifacts.append({
            "role": stage["role"], "path": stage["path"],
            "artifact": f"/Artifact/Qwen3-0.6B/Stage/{index}",
            "filename": Path(stage["path"]).name, "kind": "model",
            "backend": "onnxruntime", "metadata": metadata,
        })
    model_name = str(model_manifest["model"])
    revision = str(model_manifest.get("modelRevision", ""))
    model_uri = "/Model/Qwen3/0.6B"
    plan = {"version": 2, "services": [{
        "schemaVersion": 2, "service": SERVICE, "model": model_uri,
        "modelRepository": model_name, "modelRevision": revision,
        "dtype": model_manifest.get("dtype", "float32"), "modelFamily": "qwen",
        "modelFormat": "onnx", "plannerKind": "native-qwen-layer",
        "runtimeBackend": "onnxruntime", "executionPolicy": "DATA_DRIVEN_V2",
        "roles": roles, "dependencies": dependencies,
    }]}
    service_manifest = {"services": [{
        "name": SERVICE, "model": model_uri, "modelRepository": model_name,
        "modelRevision": revision, "dtype": model_manifest.get("dtype", "float32"),
        "roles": roles, "dependencies": dependencies, "artifacts": artifacts,
        "modelFamily": "qwen", "modelFormat": "onnx",
        "plannerKind": "native-qwen-layer", "runtimeBackend": "onnxruntime",
    }]}
    plan_path = output / "native-qwen-execution-plan.json"
    manifest_path = output / "native-qwen-service-manifest.json"
    plan_path.write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n")
    manifest_path.write_text(json.dumps(service_manifest, indent=2, sort_keys=True) + "\n")
    return plan_path, manifest_path


def policy_text(provider_names: list[str]) -> str:
    entries = "".join(
        " provider-policy\n {\n  for %s\n  allow { %s\n  %s/ROLE/LLM/Pipeline/Stage/%d }\n }\n"
        % (name, SERVICE, SERVICE, index)
        for index, name in enumerate(provider_names))
    return (f"name {CONTROLLER}/NDNSF/ControllerPolicy/v1\n\n"
            "provider-policies\n{\n provider-policy\n {\n  for " + AUTHORITY
            + "\n  allow { /HELLO }\n }\n" + entries + "}\n\n"
            "user-policies\n{\n user-policy\n {\n  for " + USER
            + "\n  allow { " + SERVICE + "\n  /HELLO }\n }\n}\n")


def env_for(home: Path, node: str) -> dict[str, str]:
    env = {"HOME": str(home), "NDN_CLIENT_CONF": str(home / ".ndn/client.conf"),
           "NDN_CLIENT_PIB": "pib-sqlite3:" + str(home / "pib"),
           "NDN_CLIENT_TPM": "tpm-file:" + str(home / "tpm"),
           "NDN_CLIENT_TRANSPORT": f"unix:///run/nfd/{node}.sock",
           "PATH": "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"}
    diagnostic_log = os.environ.get("NDNSF_NDN_LOG", "")
    if diagnostic_log:
        env["NDN_LOG"] = diagnostic_log
    stream_diagnostic = os.environ.get("NDNSF_STREAM_GRANT_DIAGNOSTIC", "")
    if stream_diagnostic:
        env["NDNSF_STREAM_GRANT_DIAGNOSTIC"] = stream_diagnostic
    protected_dataflow_diagnostic = os.environ.get(
        "NDNSF_DI_PROTECTED_DATAFLOW_DIAGNOSTIC", "")
    if protected_dataflow_diagnostic:
        env["NDNSF_DI_PROTECTED_DATAFLOW_DIAGNOSTIC"] = protected_dataflow_diagnostic
    runtime_timing = os.environ.get("NDNSF_DI_RUNTIME_TIMING", "")
    if runtime_timing:
        env["NDNSF_DI_RUNTIME_TIMING"] = runtime_timing
    dependency_trace = os.environ.get("NDNSF_DI_DEPENDENCY_OBJECT_TRACE", "")
    if dependency_trace:
        env["NDNSF_DI_DEPENDENCY_OBJECT_TRACE"] = dependency_trace
    # Keep provider-side large-data/assignment diagnostics explicit and
    # run-scoped.  These are observation controls only; they must reach every
    # MiniNDN process through the maintained launcher instead of relying on a
    # host environment leak.
    for key in ("NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE",
                "NDNSF_COLLAB_LARGE_FETCH_TIMING"):
        value = os.environ.get(key, "")
        if value:
            env[key] = value
    return env


def env_command(env: dict[str, str], command: str) -> str:
    return "env " + " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items()) + " " + command


def wait_for_marker(process, log: Path, markers: tuple[str, ...], timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text(errors="replace") if log.exists() else ""
        for marker in markers:
            if marker in text:
                return marker
        if process.poll() is not None:
            raise RuntimeError(f"process exited {process.returncode}: {log}")
        time.sleep(0.2)
    raise RuntimeError(f"timeout waiting for {markers}: {log}")


def validate_native_output(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"native output is not JSON: {path}") from exc
    if value.get("schema") != "NDNSF-DI-FINAL-V1":
        raise RuntimeError(f"native output schema is unexpected: {path}")
    token_ids = value.get("tokenIds")
    if not isinstance(token_ids, list) or not token_ids:
        raise RuntimeError(f"native output has no generated token IDs: {path}")
    if any(not isinstance(item, int) for item in token_ids):
        raise RuntimeError(f"native output token IDs are not integers: {path}")
    return {"tokenCount": len(token_ids), "tokenIds": token_ids}


def main(argv=None, *, _supervised=False) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage-manifest", type=Path, required=True)
    parser.add_argument("--stage-manifest-sha256", default=None)
    parser.add_argument("--stage-root", type=Path, default=None)
    parser.add_argument("--canonical-source", type=Path, default=None,
                        help="canonical ONNX graph object for native assembly")
    parser.add_argument("--canonical-source-sha256", default=None,
                        help="expected canonical source digest from the prepared candidate")
    parser.add_argument("--canonical-initializer", type=Path, default=None,
                        help="optional external canonical ONNX initializer object")
    parser.add_argument("--canonical-initializer-sha256", default=None,
                        help="expected external initializer digest from the prepared candidate")
    parser.add_argument("--node-mapping", type=Path, required=True,
                        help="authenticated semantic-to-canonical ONNX node mapping JSON")
    parser.add_argument("--node-mapping-sha256", default=None,
                        help="expected node mapping digest from the prepared candidate")
    parser.add_argument("--topology", type=Path, default=ROOT / "Experiments/Topology/AI_Lab.conf")
    parser.add_argument("--topology-sha256", default=None)
    parser.add_argument("--tokenizer-sha256", default=None)
    parser.add_argument("--stage-nodes", default="ucla,arizona,wustl")
    parser.add_argument("--controller-node", default="memphis")
    parser.add_argument("--user-node", default="neu")
    parser.add_argument("--build", type=Path, default=ROOT / "build-spec184-b5-candidate-r4")
    parser.add_argument("--build-receipt-sha256", default=None)
    parser.add_argument("--controller-binary", type=Path, default=ROOT / "build-spec184-b5-candidate/examples/App_ServiceController")
    parser.add_argument("--controller-binary-sha256", default=None)
    parser.add_argument("--authority-binary-sha256", default=None)
    parser.add_argument("--requester-binary-sha256", default=None)
    parser.add_argument("--provider-binary-sha256", default=None)
    parser.add_argument("--assembly-worker-binary-sha256", default=None)
    parser.add_argument("--oracle-binary", type=Path, default=None,
                        help="Spec189 C++ full-path oracle executable")
    parser.add_argument("--oracle-binary-sha256", default=None)
    parser.add_argument("--run-root", type=Path, default=None)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--max-new-tokens", type=int, default=2)
    parser.add_argument("--input-token-ids", default="")
    parser.add_argument("--delta-token-ids", default="0")
    parser.add_argument("--negative-parent", action="store_true")
    parser.add_argument("--nlsr-wait-s", type=float, default=8.0)
    parser.add_argument("--startup-timeout-s", type=float, default=60.0)
    parser.add_argument("--resource-limits-json", default="{}",
                        help="host resource limits as a JSON object; defaults match LocalExperiment")
    args = parser.parse_args(argv)
    limits = validate_limits(json.loads(args.resource_limits_json))
    if not _supervised:
        run_root = (args.run_root.expanduser().resolve() if args.run_root else
                    Path(tempfile.mkdtemp(prefix="ndnsf-qwen06b-minindn-", dir="/tmp")))
        run_root.mkdir(parents=True, exist_ok=True)
        run_root.chmod(0o700)
        worker_args = list(sys.argv[1:] if argv is None else argv)
        if args.run_root is None:
            worker_args.extend(["--run-root", str(run_root)])
        # Internal worker entry has no public bypass switch. The outer process
        # owns resource admission before model hashing/materialization begins.
        worker = [sys.executable, "-c",
                  "import runpy,sys; path=sys.argv[1]; sys.argv=sys.argv[1:]; "
                  "scope=runpy.run_path(path); "
                  "raise SystemExit(scope['main'](_supervised=True))",
                  str(Path(__file__).resolve()), *worker_args]
        try:
            receipt_fd = os.open(run_root / "supervisor.json",
                                 os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            print("NATIVE_HOST_GUARD_STOP " + json.dumps({
                "boundary": "EVIDENCE_CONFLICT", "cleanup": "NOT_STARTED",
                "returncode": None, "remainingProcesses": []}, sort_keys=True))
            return 1
        with os.fdopen(receipt_fd, "w", encoding="utf-8") as receipt:
            result = run_guarded(worker, cwd=ROOT, stdout=None,
                                 sample_path=run_root / "resource-samples.jsonl", limits=limits)
            json.dump(result, receipt, sort_keys=True, indent=2)
            receipt.write("\n")
        if result["boundary"] or result["cleanup"] != "PASS":
            print("NATIVE_HOST_GUARD_STOP " + json.dumps(result, sort_keys=True))
            return 1
        return result["returncode"] if result["returncode"] is not None else 1
    if os.geteuid() != 0:
        raise SystemExit("MININDN_REQUIRES_ROOT: run this script with sudo -E")
    if args.rounds < 1 or args.rounds > 8:
        raise SystemExit("--rounds must be between 1 and 8")
    if args.negative_parent and args.rounds < 2:
        raise SystemExit("--negative-parent requires --rounds >= 2")
    if args.max_new_tokens < 1 or args.max_new_tokens > 64:
        raise SystemExit("--max-new-tokens must be between 1 and 64")
    stage_manifest_path = args.stage_manifest.expanduser().resolve()
    require_file_digest(stage_manifest_path, args.stage_manifest_sha256, "MODEL_STAGE_MANIFEST")
    model_manifest, stages = load_stage_manifest(stage_manifest_path, args.stage_root)
    qwen_state_successor_pairs(stages)
    stage_nodes = [item.strip() for item in args.stage_nodes.split(",") if item.strip()]
    if len(stage_nodes) != len(stages) or len(set(stage_nodes)) != len(stage_nodes):
        raise SystemExit("--stage-nodes count must match stage manifest and contain no duplicates")
    if not args.topology.expanduser().is_file():
        raise SystemExit(f"topology is missing: {args.topology}")
    require_file_digest(args.topology, args.topology_sha256, "TOPOLOGY")
    tokenizer_raw = Path(str(model_manifest.get("tokenizer", ""))).expanduser()
    tokenizer_candidates = [tokenizer_raw]
    if not tokenizer_raw.is_absolute():
        tokenizer_candidates.extend([
            stage_manifest_path.parent / tokenizer_raw,
            (args.stage_root.expanduser() / tokenizer_raw)
            if args.stage_root else stage_manifest_path.parent / tokenizer_raw,
        ])
    tokenizer = None
    for candidate in tokenizer_candidates:
        resolved = candidate.resolve()
        if resolved.is_dir():
            resolved = resolved / "tokenizer.json"
        if resolved.is_file():
            tokenizer = resolved
            break
    if tokenizer is None:
        raise SystemExit(f"tokenizer.json is missing: {tokenizer_raw}")
    tokenizer_digest = digest_bytes(tokenizer.read_bytes())
    if args.tokenizer_sha256 and tokenizer_digest != args.tokenizer_sha256:
        raise SystemExit("MODEL_TOKENIZER_DIGEST_MISMATCH")
    build = args.build.expanduser().resolve()
    oracle_binary = (args.oracle_binary.expanduser().resolve()
                     if args.oracle_binary is not None
                     else (build / "examples/spec189-two-provider-oracle").resolve())
    receipt = build / "spec180-native-build.json"
    if args.build_receipt_sha256:
        require_file_digest(receipt, args.build_receipt_sha256, "BUILD_RECEIPT")
    for path, expected, label in (
        (args.controller_binary, args.controller_binary_sha256, "CONTROLLER_BINARY"),
        (build / "examples/DI_NativeArtifactAuthority", args.authority_binary_sha256, "AUTHORITY_BINARY"),
        (build / "examples/DI_NativeRequester", args.requester_binary_sha256, "REQUESTER_BINARY"),
        (build / "examples/di-native-provider", args.provider_binary_sha256, "PROVIDER_BINARY"),
        (build / "DI_NativeOnnxAssemblyWorker", args.assembly_worker_binary_sha256, "ASSEMBLY_WORKER_BINARY"),
        (oracle_binary, args.oracle_binary_sha256, "SPEC189_ORACLE_BINARY"),
    ):
        require_file_digest(path, expected, label)
    revision = str(model_manifest.get("modelRevision", ""))
    model_name = "Qwen3-0.6B"
    ranges = []
    for stage in stages:
        layer_range = stage.get("layerRange")
        if isinstance(layer_range, dict):
            ranges.append([int(layer_range["start"]), int(layer_range["endExclusive"])])
        elif isinstance(layer_range, list) and len(layer_range) == 2:
            ranges.append([int(layer_range[0]), int(layer_range[1])])
        else:
            raise ValueError(f"stage layerRange is invalid: {stage.get('role')}")
    precision = str(model_manifest.get("dtype", "float32")).lower()
    if precision not in {"float16", "float32"}:
        raise SystemExit(f"unsupported Qwen precision: {precision}")
    graph_digest = qwen_graph_digest(model_name, revision, precision, ranges)
    adapter = {
        "name": "qwen", "version": "1", "state_digest": digest_text("qwen-native-state"),
        "abi": "qwen-native-onnxruntime-cpu-v1", "model_formats": ["onnx"],
        "tasks": ["text-generation"], "backends": ["onnxruntime"], "precisions": [precision],
        "input_schema_digest": digest_text("qwen-input-schema"),
        "options_schema_digest": digest_text("qwen-options-schema"),
        "result_schema_digest": digest_text("qwen-result-schema"),
        "graph_schema_digest": digest_text("qwen-graph-schema"),
        "split_schema_digest": digest_text("qwen-split-schema"),
        "state_schema_digest": digest_text("qwen-state-schema"),
        "graph_inspectable": True, "splittable": True, "deterministic_analysis": True,
    }
    model_descriptor = {
        "model_name": model_name, "content_digest": digest_text(f"{model_name}:{revision}:weights"),
        "semantics_digest": digest_text(f"{model_name}:{revision}:semantics"),
        "graph_digest": graph_digest, "model_format": "onnx", "precision": precision,
        "adapter": adapter, "source_revision": revision,
    }
    adapter_digest = digest_bytes(canonical_bytes(adapter))
    manifest_digest = digest_bytes(stage_manifest_path.read_bytes())
    if args.canonical_source is None:
        raise SystemExit("MODEL_CANONICAL_SOURCE_REQUIRED")
    if not args.canonical_source_sha256:
        raise SystemExit("MODEL_CANONICAL_SOURCE_DIGEST_REQUIRED")
    canonical_source_input = args.canonical_source.expanduser().resolve()
    if not canonical_source_input.is_file():
        raise SystemExit(f"MODEL_CANONICAL_SOURCE_MISSING:{canonical_source_input}")
    if canonical_source_input.stat().st_size == 0:
        raise SystemExit("MODEL_CANONICAL_SOURCE_SIZE_INVALID")
    mapping_input = args.node_mapping.expanduser().resolve()
    if not mapping_input.is_file():
        raise SystemExit(f"MODEL_NODE_MAPPING_MISSING:{mapping_input}")
    if not args.node_mapping_sha256:
        raise SystemExit("MODEL_NODE_MAPPING_DIGEST_REQUIRED")
    mapping_digest = digest_file(mapping_input)
    if args.node_mapping_sha256 and mapping_digest != args.node_mapping_sha256:
        raise SystemExit("MODEL_NODE_MAPPING_DIGEST_MISMATCH")
    mapping_payload = json.loads(mapping_input.read_text(encoding="utf-8"))
    node_mapping = mapping_payload.get("mapping", mapping_payload)
    expected_mapping_keys = {"embedding", "final-norm-head"} | {
        f"layer-{index:02d}" for index in range(ranges[-1][1])}
    if not isinstance(node_mapping, dict) or set(node_mapping) != expected_mapping_keys:
        raise SystemExit("MODEL_NODE_MAPPING_COVER_INVALID")
    if any(not isinstance(indices, list) or not indices or
           any(not isinstance(index, int) or index < 0 for index in indices)
           for indices in node_mapping.values()):
        raise SystemExit("MODEL_NODE_MAPPING_INDEX_INVALID")
    source_node_count = mapping_payload.get("source_nodes") if isinstance(mapping_payload, dict) else None
    if (not isinstance(source_node_count, int) or source_node_count <= 0):
        raise SystemExit("MODEL_NODE_MAPPING_SOURCE_COUNT_INVALID")
    mapped_indices = [index for indices in node_mapping.values() for index in indices]
    if (len(mapped_indices) != source_node_count or
            len(set(mapped_indices)) != source_node_count or
            set(mapped_indices) != set(range(source_node_count))):
        raise SystemExit("MODEL_NODE_MAPPING_INDEX_COVER_INVALID")
    canonical_initializer_input = None
    if args.canonical_initializer is not None:
        if not args.canonical_initializer_sha256:
            raise SystemExit("MODEL_CANONICAL_INITIALIZER_DIGEST_REQUIRED")
        canonical_initializer_input = args.canonical_initializer.expanduser().resolve()
        if not canonical_initializer_input.is_file():
            raise SystemExit(f"MODEL_CANONICAL_INITIALIZER_MISSING:{canonical_initializer_input}")
        if canonical_initializer_input.stat().st_size == 0:
            raise SystemExit("MODEL_CANONICAL_INITIALIZER_SIZE_INVALID")
    if args.run_root is None:
        run_root = Path(tempfile.mkdtemp(prefix="ndnsf-qwen06b-minindn-", dir="/tmp"))
    else:
        run_root = args.run_root.expanduser().resolve()
        run_root.mkdir(parents=True, exist_ok=True)
    run_root.chmod(0o700)
    (run_root / "requester").mkdir()
    source_path = run_root / "requester/canonical-source.onnx"
    mapping_path = run_root / "requester/node-mapping.json"
    initializer_path = (run_root / "requester/canonical-initializer.bin"
                         if canonical_initializer_input is not None else None)
    materialized = []
    try:
        source_materialized = materialize_input(
            canonical_source_input, source_path, args.canonical_source_sha256, "MODEL_CANONICAL")
        materialized.append(source_path)
        mapping_materialized = materialize_input(
            mapping_input, mapping_path, args.node_mapping_sha256, "MODEL_NODE_MAPPING")
        materialized.append(mapping_path)
        if initializer_path is not None:
            initializer_materialized = materialize_input(
                canonical_initializer_input, initializer_path,
                args.canonical_initializer_sha256, "MODEL_CANONICAL_INITIALIZER")
            materialized.append(initializer_path)
    except (OSError, RuntimeError) as exc:
        for path in materialized:
            path.unlink(missing_ok=True)
        raise SystemExit(str(exc)) from exc
    source_digest = source_materialized["sha256"]
    initializer_path = None
    initializer_digest = None
    if canonical_initializer_input is not None:
        initializer_path = run_root / "requester/canonical-initializer.bin"
        initializer_digest = initializer_materialized["sha256"]
    # The maintained ONNX identity loader resolves external data relative to
    # this copied source object, so compute it only after the initializer has
    # been materialized in the same requester directory.
    canonical_source_digest = canonical_source_graph_digest(source_path)
    stage_plan, service_manifest = stage_plan_and_manifest(
        run_root, model_manifest, stages, args.max_new_tokens, tokenizer_digest)
    provider_names = [f"{APP_ROOT}/provider-{index}" for index in range(len(stages))]
    role_map_digest = digest_bytes(canonical_bytes(list(zip(
        [stage["role"] for stage in stages], provider_names))))
    # The preparation cache reservation is conservative: it includes the
    # largest role assembly plus parser scratch, while the provider cache
    # enforces the same per-role assembly ceiling.  Keep this model-specific
    # instead of reserving an 8 GiB generic ceiling for a 0.6B local run.
    max_source_bytes = max(
        canonical_source_input.stat().st_size,
        canonical_initializer_input.stat().st_size
        if canonical_initializer_input is not None else 0,
    ) + 64 * 1024 * 1024
    # An extracted role can retain every tensor from the inlined canonical
    # model.  Bound assembly by the complete graph plus external initializer,
    # rather than assuming the largest stage is the assembled model size.
    max_assembled_bytes = (canonical_source_input.stat().st_size +
                           (canonical_initializer_input.stat().st_size
                            if canonical_initializer_input is not None else 0) +
                           64 * 1024 * 1024)
    max_prepared_bytes = preparation_cache_budget(
        max_source_bytes, max_assembled_bytes)
    canonical_source_bytes = canonical_source_input.stat().st_size
    canonical_initializer_bytes = (canonical_initializer_input.stat().st_size
                                   if canonical_initializer_input is not None else 0)
    large_data_ims = large_data_ims_limit(
        canonical_source_bytes, canonical_initializer_bytes)
    runtime_budgets = qwen_runtime_budgets(
        canonical_source_bytes, canonical_initializer_bytes,
        rounds=args.rounds, stage_count=len(stages),
        startup_timeout_s=args.startup_timeout_s, nlsr_wait_s=args.nlsr_wait_s)
    state_inputs = {
        stage["role"]: {name: [name] for name in stage.get("cacheInputs", [])}
        for stage in stages
    }
    state_outputs = {
        stage["role"]: {name: [name] for name in stage.get("cacheOutputs", [])}
        for stage in stages
    }
    all_state_inputs = [name for stage in stages for name in stage.get("cacheInputs", [])]
    all_state_outputs = [name for stage in stages for name in stage.get("cacheOutputs", [])]
    catalog = {
        "schema": "ndnsf-di-native-request-catalog-v1", "model": model_descriptor,
        "source": {"data_name": "/catalog/qwen06b/source", "digest": source_digest,
                    "model_manifest_digest": manifest_digest,
                    "canonical_graph_digest": canonical_source_digest,
                    **({"initializer_digest": initializer_digest}
                       if initializer_digest else {})},
        "recipe": {"artifact_profile_digest": digest_text("qwen06b-profile"),
                   "assembler_descriptor_digest": digest_text("qwen06b-assembler"),
                   "backend_abi": "onnxruntime-cpu-v1", "precision": precision,
                   "quantization": "none", "layout": "native", "padding": "none",
                   "protection_epoch": "epoch-1", "max_source_bytes": max_source_bytes,
                   "max_assembled_bytes": max_assembled_bytes, "max_nodes": 10000},
        "publication": {"artifact_root": "/Model/Qwen3-0.6B/artifacts",
                         "package_manifest_digest": manifest_digest,
                         # Prepare publishes one topology-independent material
                         # set plus its authenticated root. This budget is
                         # separate from each selected role's assembly limit.
                         "max_publication_bytes": (max_source_bytes +
                                                    max_assembled_bytes +
                                                    PUBLICATION_ROOT_MARGIN_BYTES)},
        "input_format": "OPAQUE", "max_payload_bytes": 4 << 20,
        # Conversation input is an authenticated tensor bundle.  The native
        # catalog adapter must know how to extract input_ids before it can
        # create the first conversation turn; omitting this contract makes a
        # real request fail at the input boundary before ACK/Selection.
        "conversation_input": {"kind": "TENSOR_BUNDLE_TOKEN_IDS",
                                "tensor_name": "input_ids"},
        "splitter": {"kind": "QWEN", "layer_ranges": ranges,
                     "artifact_digests_by_role": {stage["role"]: stage["sha256"] for stage in stages},
                     "weight_bytes_by_role": {stage["role"]: stage["bytes"] for stage in stages},
                     "roles": [stage["role"] for stage in stages],
                     "tensor_degrees": [1] * len(stages),
                     "input_ingress_role": stages[0]["role"],
                     "result_egress_role": stages[-1]["role"]},
        "node_mapping": node_mapping,
        "state_inputs": state_inputs,
        "state_outputs": state_outputs,
    }
    requester_dir = run_root / "requester"
    catalog_path = requester_dir / "catalog.json"
    catalog_path.write_text(json.dumps(catalog, indent=2, sort_keys=True) + "\n")
    input_ids = parse_csv_ints(args.input_token_ids) if args.input_token_ids else [1]
    (requester_dir / "input.bin").write_bytes(tensor_bundle(input_ids))
    delta_ids = parse_csv_ints(args.delta_token_ids)
    (requester_dir / "delta.bin").write_bytes(tensor_bundle(delta_ids))
    policy = run_root / "policy.conf"
    policy.write_text(policy_text(provider_names))
    (run_root / "trust-schema.conf").write_text((ROOT / "examples/trust-schema.conf").read_text())
    authority_dir = run_root / "authority"; authority_dir.mkdir()
    provider_dirs = []
    for index in range(len(stages)):
        path = run_root / f"provider-{index}"; path.mkdir(); provider_dirs.append(path)
    req_private, req_public = make_external_key(requester_dir, "requester")
    auth_private, auth_public = make_external_key(authority_dir, "authority")
    (authority_dir / "content-key.bin").write_bytes(os.urandom(32))
    (authority_dir / "content-key.bin").chmod(0o600)
    provider_keys = {}
    for index, directory in enumerate(provider_dirs):
        recipient_private, recipient_public = make_external_key(directory, "recipient")
        offer_private, offer_public = make_external_key(directory, "offer")
        provider_keys[index] = (recipient_private, recipient_public, offer_private, offer_public)
    # Build one public certificate set, copy it to each node, then remove
    # private key files for identities that node does not own.  This mirrors
    # a real deployment: validators can resolve peer certificates without
    # turning every MiniNDN host into a shared private-key store.
    nodes = [args.controller_node, args.user_node, *stage_nodes]
    homes = {node: run_root / "homes" / node for node in nodes}
    for node, home in homes.items():
        (home / ".ndn").mkdir(parents=True, exist_ok=True)
        (home / "pib").mkdir(parents=True, exist_ok=True)
        (home / "tpm").mkdir(parents=True, exist_ok=True)
        (home / ".ndn/client.conf").write_text(f"transport=unix:///run/nfd/{node}.sock\n")
    identities = [(args.controller_node, CONTROLLER), (args.controller_node, AUTHORITY),
                  (args.user_node, USER), *zip(stage_nodes, provider_names)]
    bootstrap_home = run_root / "homes/bootstrap"
    (bootstrap_home / ".ndn").mkdir(parents=True, exist_ok=True)
    (bootstrap_home / "pib").mkdir(parents=True, exist_ok=True)
    (bootstrap_home / "tpm").mkdir(parents=True, exist_ok=True)
    (bootstrap_home / ".ndn/client.conf").write_text("transport=unix:///run/nfd/bootstrap.sock\n")
    certs = {}
    for _, identity in identities:
        cert = run_root / "certs" / (identity.strip("/").replace("/", "_") + ".cert")
        cert.parent.mkdir(parents=True, exist_ok=True)
        prefix = env_command(env_for(bootstrap_home, "bootstrap"), "ndnsec")
        subprocess.run(prefix + f" key-gen -t r {shlex.quote(identity)}", shell=True, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        subprocess.run(prefix + f" cert-dump -i {shlex.quote(identity)} > {shlex.quote(str(cert))}",
                       shell=True, check=True)
        certs[identity] = cert
    for node, home in homes.items():
        shutil.copytree(bootstrap_home / "pib", home / "pib", dirs_exist_ok=True)
        shutil.copytree(bootstrap_home / "tpm", home / "tpm", dirs_exist_ok=True)
        with sqlite3.connect(home / "pib/pib.db") as conn:
            conn.execute("update tpmInfo set tpm_locator=?", ("tpm-file:" + str(home / "tpm"),))
            conn.commit()
        keep = {identity for owner, identity in identities if owner == node}
        with sqlite3.connect(home / "pib/pib.db") as conn:
            rows = conn.execute("select identities.identity, keys.key_name from identities join keys on keys.identity_id=identities.id").fetchall()
        for identity_blob, key_name in rows:
            try:
                from ndn.encoding import Component, Name
                parts, _ = Name.decode(bytes(identity_blob))
                identity = "/" + "/".join(Component.to_str(item.tobytes()) for item in parts)
            except Exception:
                continue
            if identity in keep:
                continue
            (home / "tpm/ndnsec-key-file" / (hashlib.sha256(bytes(key_name)).hexdigest() + ".privkey")).unlink(missing_ok=True)
    for node, home in homes.items():
        default_identity = next(identity for owner, identity in identities if owner == node)
        prefix = env_command(env_for(home, node), "ndnsec")
        subprocess.run(prefix + f" set-default -n {shlex.quote(default_identity)}",
                       shell=True, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Find each provider's actual signing key prefix for NativeOfferAdmission.
    def key_prefix(index: int) -> str:
        db = homes[stage_nodes[index]] / "pib/pib.db"
        with sqlite3.connect(db) as conn:
            for (value,) in conn.execute("select key_name from keys"):
                raw = bytes(value)
                try:
                    from ndn.encoding import Component, Name
                    parts, _ = Name.decode(raw)
                    uri = "/" + "/".join(Component.to_str(item.tobytes()) for item in parts)
                except Exception:
                    continue
                if uri.startswith(provider_names[index] + "/KEY/"):
                    return uri
        raise RuntimeError(f"provider key prefix unavailable: {provider_names[index]}")
    offer_entries = []; public_key_files = {}
    for index, provider in enumerate(provider_names):
        _, _, _, offer_public = provider_keys[index]
        offer_id = public_key_digest(offer_public)
        public_key_files[offer_id] = str(offer_public)
        prefix = key_prefix(index)
        offer_entries.append({"provider": provider, "service": SERVICE,
                              "keyLocatorPrefix": prefix,
                              "signerKeyId": offer_id,
                              "certificateName": prefix + "/ID-CERT"})
    candidate_digest = digest_text("qwen06b-offer-policy")
    offer_policy = {"schema": "spec180-provider-offer-trust-v1", "candidateId": "qwen06b-local",
                    "candidateDigest": candidate_digest, "trustSchema": APP_ROOT + "/trust",
                    "entries": offer_entries}
    authority_cfg = {
        "schema": "ndnsf-di-native-authority-v1",
        "run_for_ms": runtime_budgets["authority_run_ms"],
        "permission_bootstrap_ms": 60000,
        "max_grant_ttl_ms": min(3600000, runtime_budgets["retention_ms"]),
        "authority": {"identity": AUTHORITY, "service": "/HELLO", "group": GROUP,
                       "controller_identity": CONTROLLER, "requester_identity": USER,
                       "protection_epoch": "epoch-1", "content_key_id": "model-key",
                       "trust_schema_file": "../trust-schema.conf",
                       "authority_private_key_file": "authority-private.pem",
                       "requester_public_key_file": "../requester/requester-public.pem",
                       "content_key_file": "content-key.bin",
                       "allowed_model_manifests": [manifest_digest],
                       "recipient_public_key_files": {provider: str(provider_dirs[index] / "recipient-public.pem") for index, provider in enumerate(provider_names)},
                       "publication_sources": {manifest_digest: {
                           "model_name": model_descriptor["model_name"],
                           "model_content_digest": model_descriptor["content_digest"],
                           "canonical_source_digest": source_digest,
                           **({"initializer_object_digest": initializer_digest}
                              if initializer_digest else {}),
                           "artifact_profile_digest": catalog["recipe"]["artifact_profile_digest"]}}}}
    (authority_dir / "authority-private.pem").write_bytes(auth_private.read_bytes())
    (authority_dir / "requester-public.pem").write_bytes(req_public.read_bytes())
    for index, provider in enumerate(provider_names):
        provider_dirs[index].joinpath("recipient-public.pem").write_bytes(provider_keys[index][1].read_bytes())
    (authority_dir / "authority.json").write_text(json.dumps(authority_cfg, indent=2, sort_keys=True) + "\n")
    # NativeProtectedGrantCredentials resolves a complete operator-owned
    # registry from the Provider's authority-public-key hint.  Keep the
    # registry and its immutable public-key target in the run root, while
    # each Provider receives its own non-symlink hint and registry file.
    authority_public_bytes = auth_public.read_bytes()
    shared_authority_public = run_root / "authority-public.pem"
    shared_authority_public.write_bytes(authority_public_bytes)
    shared_authority_public.chmod(0o644)
    authority_registry = {
        "schemaVersion": 1,
        "status": "CONFIGURED",
        "artifactPolicyAuthority": {
            "acceptedModelFamilies": ["/Model/Qwen3/0.6B"],
            "authorityId": AUTHORITY,
            "grantSchema": "ndnsf-di-key-grant-v1",
            "keyId": "qwen06b-artifact-policy-ed25519-local",
            "protectionEpochs": ["epoch-1"],
            "publicKeyAlgorithm": "ed25519",
            "publicKeyPath": "authority-public.pem",
            "publicKeySha256": digest_bytes(authority_public_bytes),
            "signatureAlgorithm": "ed25519",
        },
    }
    # Provider maps and requester config are written after key paths are stable.
    for index, provider in enumerate(provider_names):
        directory = provider_dirs[index]
        (directory / "plan.json").write_text(stage_plan.read_text())
        # Serving providers advertise role metadata and wait for the
        # authenticated Selection projection to bind a concrete artifact.
        # Keeping exporter-local `path` fields here would make the native
        # provider reject the manifest as a preassembled startup artifact.
        provider_manifest = json.loads(service_manifest.read_text())
        for service in provider_manifest.get("services", []):
            for artifact in service.get("artifacts", []):
                artifact.pop("path", None)
        (directory / "manifest.json").write_text(
            json.dumps(provider_manifest, indent=2, sort_keys=True) + "\n")
        (directory / "trust-schema.conf").write_text((ROOT / "examples/trust-schema.conf").read_text())
        (directory / "authority-public.pem").write_bytes(auth_public.read_bytes())
        (directory / "trust-root-registry-v1.json").write_text(
            json.dumps(authority_registry, indent=2, sort_keys=True) + "\n")
        (directory / "offer-private.pem").write_bytes(provider_keys[index][2].read_bytes())
        (directory / "recipient-private.pem").write_bytes(provider_keys[index][0].read_bytes())
        (directory / "recipient-key-map.json").write_text(json.dumps({provider: str(directory / "recipient-private.pem")}))
    requester_cfg = {
        "schema": "ndnsf-di-native-requester-v1",
        "catalog": {**catalog, "source": {**catalog["source"], "file": source_path.name,
                                             **({"initializer_file": initializer_path.name}
                                                if initializer_path else {})}},
        "core": {"requester_identity": USER, "authority_identity": CONTROLLER,
                 "group": GROUP, "trust_schema_file": "../trust-schema.conf"},
        "grant": {"authority_identity": AUTHORITY, "authority_service": "/HELLO",
                   "authority_public_key_file": "authority-public.pem",
                   "requester_private_key_file": "requester-private.pem",
                   "protection_epoch": "epoch-1"},
        "offer_admission": {"policy": offer_policy, "public_key_files": public_key_files,
                            "candidate_digest": candidate_digest},
        # The canonical Repo is the prepare-time source owner/cache.  The
        # requester ingests the immutable source once; protected root/material
        # publication remains on Runtime's authenticated NDN LargeData path so
        # remote Providers can fetch the returned references.  Keep ciphertext
        # large data in its separate Repo below so the two ownership contracts
        # do not share cache accounting or cleanup.
        "repository": {"path": str(run_root / "requester/canonical-repo"),
                        "max_bytes": 4 << 30},
        "encrypted_repository": {"path": str(run_root / "requester/encrypted-repo"),
                                 "max_bytes": 4 << 30},
        "limits": {"bootstrap_ms": runtime_budgets["bootstrap_ms"],
                    "large_data_ims_limit": large_data_ims,
                    "max_source_bytes": max_source_bytes,
                    "max_assembled_bytes": max_assembled_bytes,
                    "max_prepared_bytes": max_prepared_bytes,
                    "max_prepared_entries": 8},
        "request": {"service": SERVICE, "task": "text-generation",
                     "adapter_composition_digest": adapter_digest,
                     "task_descriptor_digest": digest_text("qwen06b-task"),
                     "generation_mode": "TOKEN_STREAMING", "tokenizer_digest": tokenizer_digest,
                     "input_layout_digest": digest_text("qwen06b-input-layout"),
                     "security_policy_digest": digest_text("qwen06b-security"),
                     # Assembly and authenticated material fetch happen after
                     # Selection and may be silent before the first token.
                     # Keep the bounded stream wait explicit in the generated
                     # native requester contract.
                     "interest_lifetime_ms": 5000, "max_event_retries": 8,
                     "max_candidates": 1, "max_policy_ms": runtime_budgets["policy_ms"], "provider_names": provider_names,
                     "max_reentries": 1, "no_progress_ms": runtime_budgets["no_progress_ms"],
                     "timeout_ms": runtime_budgets["timeout_ms"],
                     "ack_timeout_ms": runtime_budgets["ack_timeout_ms"],
                     "options_file": "options.json"},
        "conversation": {"schema": "ndnsf-di-native-conversation-v1",
                          "journal": {"state_root": "conversation-state", "identity": "qwen06b-user",
                                      "keys": [{"id": "active", "file": "conversation.key"}],
                                      "quota_bytes": NATIVE_CONVERSATION_JOURNAL_MAX_BYTES},
                          "owner": {"requester_identity": USER, "service_name": SERVICE,
                                    "security_domain_digest": digest_text("qwen06b-security")},
                          "turn": {"conversation_id": "qwen06b-minindn-conversation",
                                   "parent_context_epoch": 0, "service_name": SERVICE,
                                   "plan_role_map_digest": role_map_digest,
                                   "retention_deadline_ms": int(time.time() * 1000) + runtime_budgets["retention_ms"],
                                   "mode": "FULL_CONTEXT", "generation_id": "0123456789abcdef0123456789abcdef",
                                   "canonical_token_ids": input_ids,
                                   "expected_roles": [stage["role"] for stage in stages]},
                          "checkpoint_output_file": "conversation-state.json"},
    }
    (requester_dir / "requester-private.pem").write_bytes(req_private.read_bytes())
    (requester_dir / "authority-public.pem").write_bytes(auth_public.read_bytes())
    (requester_dir / "conversation.key").write_bytes(os.urandom(32))
    (requester_dir / "conversation.key").chmod(0o600)
    (requester_dir / "trust-schema.conf").write_text((ROOT / "examples/trust-schema.conf").read_text())
    (requester_dir / "options.json").write_text(json.dumps({
        "useCache": True, "outputMode": "TOKEN_STREAMING",
        "generationId": "0123456789abcdef0123456789abcdef",
        "maxNewTokens": args.max_new_tokens, "tokenizerDigest": tokenizer_digest,
        "eosTokenIds": [151645], "sampling": {"mode": "Greedy", "temperature": 0.0,
        "topK": 1, "topP": 1.0, "repetitionPenalty": 1.0, "seed": 18406},
        "stopStrings": [], "tokenInputName": "input_ids",
        "stateInputNames": all_state_inputs,
        "stateOutputNames": all_state_outputs,
        "stateSuccessorMap": ",".join(
            f"{input_name}={output_name}"
            for input_name, output_name in qwen_state_successor_pairs(stages)),
        "positionInputPolicy": "qwen-causal-position-v1",
        "attentionMaskInputName": "attention_mask",
        "positionIdsInputName": "position_ids",
    }, indent=2, sort_keys=True) + "\n")
    (requester_dir / "config.json").write_text(json.dumps(requester_cfg, indent=2, sort_keys=True) + "\n")
    # Re-check every identity immediately before creating MiniNDN processes.
    # The earlier preflight protects preparation; this final fence prevents a
    # replacement between preflight/materialization and the actual requester,
    # provider, or controller exec from changing the candidate in flight.
    require_file_digest(stage_manifest_path, args.stage_manifest_sha256, "MODEL_STAGE_MANIFEST")
    load_stage_manifest(stage_manifest_path, args.stage_root)
    require_file_digest(args.topology, args.topology_sha256, "TOPOLOGY")
    require_file_digest(tokenizer, args.tokenizer_sha256, "MODEL_TOKENIZER")
    require_file_digest(receipt, args.build_receipt_sha256, "BUILD_RECEIPT")
    for path, expected, label in (
        (args.controller_binary, args.controller_binary_sha256, "CONTROLLER_BINARY"),
        (build / "examples/DI_NativeArtifactAuthority", args.authority_binary_sha256, "AUTHORITY_BINARY"),
        (build / "examples/DI_NativeRequester", args.requester_binary_sha256, "REQUESTER_BINARY"),
        (build / "examples/di-native-provider", args.provider_binary_sha256, "PROVIDER_BINARY"),
        (build / "DI_NativeOnnxAssemblyWorker", args.assembly_worker_binary_sha256, "ASSEMBLY_WORKER_BINARY"),
        (oracle_binary, args.oracle_binary_sha256, "SPEC189_ORACLE_BINARY"),
    ):
        require_file_digest(path, expected, label)
    require_file_digest(source_path, args.canonical_source_sha256, "MODEL_CANONICAL_SOURCE")
    require_file_digest(mapping_path, args.node_mapping_sha256, "MODEL_NODE_MAPPING")
    if initializer_path is not None:
        require_file_digest(initializer_path, args.canonical_initializer_sha256,
                           "MODEL_CANONICAL_INITIALIZER")
    # MiniNDN and process orchestration starts here.
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.apps.nlsr import Nlsr
    from minindn.helpers.nfdc import Nfdc
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.minindn import Minindn
    from minindn.util import getPopen
    # MiniNDN's constructor parses the process argv itself.  Remove runner
    # options after our parser has consumed them so its CLI never sees them.
    sys.argv = [sys.argv[0]]
    Minindn.cleanUp()
    ndn = Minindn(topoFile=str(args.topology.expanduser().resolve()))
    processes = []
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        AppManager(ndn, ndn.net.hosts, Nlsr, sync="psync", security=False,
                   faceType="udp", nFaces=3, routingType="link-state", logLevel="INFO")
        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin([ndn.net[args.controller_node]], [CONTROLLER, CONTROLLER + "/KEY", AUTHORITY, AUTHORITY + "/KEY"])
        rh.addOrigin([ndn.net[args.user_node]], [USER, USER + "/KEY"])
        for node, provider in zip(stage_nodes, provider_names):
            rh.addOrigin([ndn.net[node]], [provider, provider + "/KEY"])
        rh.addOrigin([ndn.net[args.user_node], *[ndn.net[node] for node in stage_nodes]], [GROUP])
        rh.calculateRoutes()
        time.sleep(max(0.0, args.nlsr_wait_s))
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, APP_ROOT, Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, GROUP, Nfdc.STRATEGY_MULTICAST)
        def start(node_name: str, label: str, command: str, env: dict[str, str]):
            log = run_root / f"{label}.log"
            handle = log.open("wb")
            proc = getPopen(ndn.net[node_name], command, envDict=env, shell=True,
                            stdout=handle, stderr=subprocess.STDOUT)
            processes.append((proc, handle, log))
            return proc, log
        build = args.build.expanduser().resolve()
        authority_env = env_for(homes[args.controller_node], args.controller_node)
        # Bind Controller's fenced generation store to this immutable run.
        # The default global /tmp path can retain a stale writer lock after a
        # manually interrupted MiniNDN run and would poison later candidates.
        authority_env["NDNSF_CONTROLLER_GENERATION_STATE"] = str(
            run_root / "controller-generation.state")
        # App_ServiceController keeps the maintained trust-schema default as a
        # repository-relative path.  MiniNDN launches commands from a host
        # namespace working directory, so make that dependency explicit while
        # keeping all generated run artifacts absolute and portable.
        controller_cmd = (f"cd {shlex.quote(str(ROOT))} && exec "
                          f"{shlex.quote(str(args.controller_binary.expanduser().resolve()))} "
                          f"--controller-prefix {shlex.quote(CONTROLLER)} --policy-file {shlex.quote(str(policy))} "
                          f"--ensure-identities {shlex.quote(AUTHORITY + ',' + ','.join(provider_names) + ',' + USER)} "
                          f"--no-serve-certificates --run-for-ms {runtime_budgets['authority_run_ms']}")
        controller_proc, controller_log = start(args.controller_node, "controller", controller_cmd, authority_env)
        # The banner is emitted before Controller::start() enters its real
        # PUBPARAMS readiness probe.  Wait for that process to reach the
        # banner, then give its registration path a bounded settle window
        # before launching the Authority.  Authority must still start before
        # Controller::start() can finish its reciprocal probe.
        wait_for_marker(controller_proc, controller_log, ("ServiceController started",), args.startup_timeout_s)
        time.sleep(0.5)
        authority_cmd = (f"{shlex.quote(str(build / 'examples/DI_NativeArtifactAuthority'))} "
                         f"--config {shlex.quote(str(authority_dir / 'authority.json'))}")
        authority_proc, authority_log = start(args.controller_node, "authority", authority_cmd, authority_env)
        wait_for_marker(authority_proc, authority_log, ("NATIVE_GRANT_AUTHORITY_READY",), args.startup_timeout_s)
        for index, node in enumerate(stage_nodes):
            provider = provider_names[index]; directory = provider_dirs[index]
            env = env_for(homes[node], node)
            env.update({"SPEC181_GRANT_AUTHORITY_PUBLIC_KEY": str(directory / "authority-public.pem"),
                        "SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(directory / "recipient-key-map.json"),
                        "NDNSF_DI_WORKER_BINARY": str(build / "DI_NativeOnnxAssemblyWorker")})
            command = (f"{shlex.quote(str(build / 'examples/di-native-provider'))} --plan {shlex.quote(str(directory / 'plan.json'))} "
                       f"--manifest {shlex.quote(str(directory / 'manifest.json'))} --service {shlex.quote(SERVICE)} "
                       f"--provider {shlex.quote(provider)} --group {shlex.quote(GROUP)} --controller {shlex.quote(CONTROLLER)} "
                       f"--trust-schema {shlex.quote(str(directory / 'trust-schema.conf'))} --roles {shlex.quote(stages[index]['role'])} "
                       f"--serve --run-for-ms {runtime_budgets['provider_run_ms']} --artifact-cache-dir {shlex.quote(str(directory / 'cache'))} "
                       f"--tokenizer-json {shlex.quote(str(tokenizer))} --selection-offer-key-file {shlex.quote(str(directory / 'offer-private.pem'))} "
                       "--offer-backend onnxruntime-cpu --offer-can-provision --offer-has-model")
            proc, log = start(node, f"provider-{index}", command, env)
            wait_for_marker(proc, log, ("NDNSF_DI_NATIVE_PROVIDER_READY", "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY"), args.startup_timeout_s)
        requester_env = env_for(homes[args.user_node], args.user_node)
        # Large canonical objects require an explicit, finite requester-side
        # IMS reservation.  Providers do not publish the canonical source and
        # retain the historical default, so this opt-in cannot enlarge every
        # process in a deployment.
        requester_env["NDNSF_REQUEST_LARGE_IMS_LIMIT"] = str(large_data_ims)
        requester_cmd = (f"{shlex.quote(str(build / 'examples/DI_NativeRequester'))} --config {shlex.quote(str(requester_dir / 'config.json'))} "
                         f"--input {shlex.quote(str(requester_dir / 'input.bin'))} --output {shlex.quote(str(requester_dir / 'output-0.bin'))}")
        first_proc, first_log = start(args.user_node, "requester-0", requester_cmd, requester_env)
        first_proc.wait(timeout=runtime_budgets["process_timeout_s"])
        first_text = first_log.read_text(errors="replace")
        if first_proc.returncode != 0 or "NATIVE_REQUEST_SUCCEEDED" not in first_text or "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN" not in first_text:
            raise RuntimeError(f"first native Qwen round failed: {first_log}")
        first_output = validate_native_output(requester_dir / "output-0.bin")
        round_records = [{"round": 0, "returncode": first_proc.returncode,
                          "log": str(first_log), "output": str(requester_dir / "output-0.bin"),
                          **first_output}]
        for round_index in range(1, args.rounds):
            cfg = json.loads((requester_dir / "config.json").read_text())
            generation_id = f"{round_index:032x}"
            cfg["request"]["options_file"] = f"options-{round_index}.json"
            cfg["conversation"]["turn"] = {"mode": "APPEND_DELTA", "generation_id": generation_id,
                "parent_state_file": "conversation-state.json", "delta_token_ids": delta_ids}
            cfg["conversation"].pop("checkpoint_output_file", None)
            (requester_dir / f"options-{round_index}.json").write_text(json.dumps({
                "useCache": True, "outputMode": "TOKEN_STREAMING", "generationId": generation_id,
                "maxNewTokens": args.max_new_tokens, "tokenizerDigest": tokenizer_digest,
                "eosTokenIds": [151645], "sampling": {"mode": "Greedy", "temperature": 0.0,
                "topK": 1, "topP": 1.0, "repetitionPenalty": 1.0, "seed": 18406 + round_index},
                "tokenInputName": "input_ids"}, indent=2, sort_keys=True))
            cfg_path = requester_dir / f"config-{round_index}.json"; cfg_path.write_text(json.dumps(cfg, indent=2, sort_keys=True) + "\n")
            proc, log = start(args.user_node, f"requester-{round_index}",
                              requester_cmd.replace("config.json", cfg_path.name).replace("input.bin", "delta.bin").replace("output-0.bin", f"output-{round_index}.bin"), requester_env)
            proc.wait(timeout=runtime_budgets["process_timeout_s"])
            text = log.read_text(errors="replace")
            if proc.returncode != 0 or "NATIVE_REQUEST_SUCCEEDED" not in text:
                raise RuntimeError(f"native Qwen round {round_index} failed: {log}")
            output_summary = validate_native_output(requester_dir / f"output-{round_index}.bin")
            round_records.append({"round": round_index, "returncode": proc.returncode,
                                  "log": str(log), "output": str(requester_dir / f"output-{round_index}.bin"),
                                  **output_summary})
        if args.negative_parent:
            cfg = json.loads((requester_dir / "config-1.json").read_text() if args.rounds > 1 else (requester_dir / "config.json").read_text())
            cfg["conversation"]["turn"]["parent_checkpoint_digest"] = "sha256:" + "0" * 64
            bad = requester_dir / "config-negative.json"; bad.write_text(json.dumps(cfg, indent=2, sort_keys=True) + "\n")
            proc, log = start(args.user_node, "requester-negative",
                              requester_cmd.replace("config.json", bad.name).replace("input.bin", "delta.bin").replace("output-0.bin", "output-negative.bin"), requester_env)
            proc.wait(timeout=120); text = log.read_text(errors="replace")
            if proc.returncode == 0 or "DI_NATIVE_CONVERSATION_PARENT_MISMATCH" not in text:
                raise RuntimeError(f"negative parent case did not fail closed: {log}")
            round_records.append({"round": "negative-parent", "returncode": proc.returncode, "log": str(log)})
        oracle_log = run_root / "spec189-cpp-oracle.log"
        with oracle_log.open("w") as oracle_handle:
            oracle_result = subprocess.run(
                [str(oracle_binary), "--run-root", str(run_root)],
                cwd=str(ROOT), stdout=oracle_handle, stderr=subprocess.STDOUT,
                text=True, timeout=max(30.0, args.startup_timeout_s), check=False)
        oracle_output = oracle_log.read_text(errors="replace")
        if (oracle_result.returncode != 0 or
                "SPEC189_CPP_ORACLE_PASS" not in oracle_output):
            raise RuntimeError(
                f"SPEC189_CPP_ORACLE_FAIL rc={oracle_result.returncode}: {oracle_log}")
        record = {"schema": "ndnsf-di-qwen06b-native-minindn-run-v1", "model": model_name,
                  "revision": revision, "stageManifest": str(stage_manifest_path),
                  "stageManifestDigest": manifest_digest, "tokenizerDigest": tokenizer_digest,
                  "topology": str(args.topology.expanduser().resolve()), "stageNodes": stage_nodes,
                  "largeDataImsLimit": large_data_ims,
                  "runtimeBudgets": runtime_budgets,
                  "rounds": round_records,
                  "cppOracle": {"binary": str(oracle_binary),
                                "log": str(oracle_log),
                                "returncode": oracle_result.returncode,
                                "status": "PASS"},
                  "status": "PASS"}
        (run_root / "run-record.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        print("NDNSF_DI_QWEN06B_NATIVE_MININDN_PASS " + json.dumps(record, sort_keys=True))
        return 0
    finally:
        for proc, handle, _ in reversed(processes):
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
        for proc, handle, _ in reversed(processes):
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(timeout=2)
            handle.close()
        ndn.stop()


if __name__ == "__main__":
    raise SystemExit(main())
