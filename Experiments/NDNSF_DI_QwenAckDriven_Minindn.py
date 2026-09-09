#!/usr/bin/env python3
"""Run the Spec180 QWEN-F request-first ONNX workload.

This entrypoint is intentionally separate from the frozen Spec175 runner.  It
accepts only the image-mounted, signed Qwen3.6-27B manifest and model objects,
checks the complete in-image identity before starting MiniNDN, and then
delegates the protocol work to the maintained request-first pipeline runner.
It never downloads a model, falls back to Transformers/CPU, or treats a tiny
fixture as QWEN-F evidence.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
from typing import Any, Mapping


REPO = Path(__file__).resolve().parents[1]
MANIFEST_SCHEMA = "spec180-model-manifest-v1"
MODEL_ID = "Qwen/Qwen3.6-27B"
MODEL_FAMILY = "Qwen3.6-27B"
_DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")
_SAFE_ID_RE = re.compile(r"^[A-Za-z0-9._:/-]+$")


class QwenInputError(ValueError):
    """Raised before any NFD/SVS or Provider process is created."""


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_file(path: Path) -> str:
    """Hash a mounted model object incrementally; never load a stage in RAM."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _read_json(path: Path, label: str) -> Mapping[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise QwenInputError(f"{label}_INVALID") from exc
    if not isinstance(value, Mapping):
        raise QwenInputError(f"{label}_NOT_OBJECT")
    return value


def _digest(value: Any, label: str) -> str:
    if not isinstance(value, str) or not _DIGEST_RE.fullmatch(value):
        raise QwenInputError(f"{label}_DIGEST_INVALID")
    return value


def _descriptor_path(value: Any, label: str) -> str:
    if isinstance(value, str):
        path = value
    elif isinstance(value, Mapping):
        path = next((value.get(key) for key in
                     ("path", "root", "name", "file", "relativePath")
                     if isinstance(value.get(key), str)), None)
        if not path:
            raise QwenInputError(f"{label}_PATH_MISSING")
    else:
        raise QwenInputError(f"{label}_DESCRIPTOR_INVALID")
    if not path or "\\" in path or "\x00" in path:
        raise QwenInputError(f"{label}_PATH_INVALID")
    return path


def _resolve_model_path(model_root: Path, raw: str, label: str) -> Path:
    root = model_root.resolve()
    candidate = Path(raw)
    if candidate.is_absolute():
        resolved = candidate.resolve()
    else:
        resolved = (root / candidate).resolve()
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise QwenInputError(f"{label}_PATH_ESCAPE") from exc
    return resolved


def _check_object(model_root: Path, descriptor: Any, label: str) -> Path:
    raw_path = _descriptor_path(descriptor, label)
    path = _resolve_model_path(model_root, raw_path, label)
    if not path.is_file():
        raise QwenInputError(f"{label}_MISSING")
    expected = None
    expected_size = None
    if isinstance(descriptor, Mapping):
        expected = descriptor.get("sha256", descriptor.get("digest"))
        expected_size = descriptor.get("bytes", descriptor.get("size"))
    if expected is None:
        raise QwenInputError(f"{label}_DIGEST_MISSING")
    expected = _digest(expected, label)
    if expected_size is not None:
        if (isinstance(expected_size, bool) or
                not isinstance(expected_size, int) or expected_size < 0):
            raise QwenInputError(f"{label}_SIZE_INVALID")
        if path.stat().st_size != expected_size:
            raise QwenInputError(f"{label}_SIZE_MISMATCH")
    if digest_file(path) != expected:
        raise QwenInputError(f"{label}_DIGEST_MISMATCH")
    return path


def verify_external_manifest(path: Path) -> None:
    """Use the registered release verifier; do not duplicate signature rules."""
    verifier_path = REPO / "scripts/spec180_release.py"
    spec = importlib.util.spec_from_file_location(
        "spec180_qwen_release_verifier", verifier_path)
    if spec is None or spec.loader is None:
        raise QwenInputError("MODEL_MANIFEST_VERIFIER_UNAVAILABLE")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
        module._validate_qwen_model_manifest(str(path))
    except QwenInputError:
        raise
    except Exception as exc:
        reason = str(exc) or exc.__class__.__name__
        raise QwenInputError(f"MODEL_MANIFEST_REJECTED:{reason}") from exc


def _prompt_text(manifest: Mapping[str, Any], model_root: Path,
                 expected_digest: str) -> str:
    prompt = manifest.get("prompt")
    if isinstance(prompt, str):
        text = prompt
        actual = digest_bytes(text.encode("utf-8"))
    elif isinstance(prompt, Mapping):
        text = prompt.get("text", prompt.get("value"))
        if isinstance(text, str):
            actual = digest_bytes(text.encode("utf-8"))
        else:
            raw_path = prompt.get("path")
            if not isinstance(raw_path, str):
                raise QwenInputError("PROMPT_VALUE_MISSING")
            path = _resolve_model_path(model_root, raw_path, "PROMPT")
            if not path.is_file():
                raise QwenInputError("PROMPT_MISSING")
            text = path.read_text(encoding="utf-8")
            actual = digest_bytes(text.encode("utf-8"))
    else:
        raise QwenInputError("PROMPT_INVALID")
    if not text.strip():
        raise QwenInputError("PROMPT_EMPTY")
    if actual != expected_digest:
        raise QwenInputError("PROMPT_DIGEST_MISMATCH")
    return text


def _legacy_stage_manifest(manifest: Mapping[str, Any], model_root: Path,
                           output_root: Path) -> tuple[Path, Mapping[str, Any]]:
    """Resolve the signed stage manifest into the maintained runner's format."""
    source = manifest.get("stageManifest")
    if isinstance(source, Mapping) and "stages" in source:
        document = dict(source)
        source_digest = None
    else:
        raw_path = _descriptor_path(source, "STAGE_MANIFEST")
        path = _resolve_model_path(model_root, raw_path, "STAGE_MANIFEST")
        if not path.is_file():
            raise QwenInputError("STAGE_MANIFEST_MISSING")
        descriptor = source if isinstance(source, Mapping) else {}
        if isinstance(descriptor, Mapping):
            expected = descriptor.get("sha256", descriptor.get("digest"))
            if expected is not None and digest_file(path) != _digest(
                    expected, "STAGE_MANIFEST"):
                raise QwenInputError("STAGE_MANIFEST_DIGEST_MISMATCH")
        document = _read_json(path, "STAGE_MANIFEST")
        source_digest = digest_file(path)
    stages = document.get("stages")
    if not isinstance(stages, list) or len(stages) != 3:
        raise QwenInputError("STAGE_MANIFEST_STAGE_COUNT_INVALID")
    normalized: list[dict[str, Any]] = []
    for index, stage in enumerate(stages):
        if not isinstance(stage, Mapping):
            raise QwenInputError(f"STAGE_{index}_INVALID")
        raw_path = _descriptor_path(stage, f"STAGE_{index}")
        path = _resolve_model_path(model_root, raw_path, f"STAGE_{index}")
        if not path.is_file():
            raise QwenInputError(f"STAGE_{index}_MISSING")
        expected = stage.get("sha256", stage.get("digest"))
        expected_size = stage.get("bytes", stage.get("size"))
        if expected is None:
            raise QwenInputError(f"STAGE_{index}_DIGEST_MISSING")
        expected = _digest(expected, f"STAGE_{index}")
        if isinstance(expected_size, int) and path.stat().st_size != expected_size:
            raise QwenInputError(f"STAGE_{index}_SIZE_MISMATCH")
        if digest_file(path) != expected:
            raise QwenInputError(f"STAGE_{index}_DIGEST_MISMATCH")
        role = stage.get("role", f"/LLM/Pipeline/Stage/{index}")
        if not isinstance(role, str) or not role:
            raise QwenInputError(f"STAGE_{index}_ROLE_INVALID")
        normalized.append({
            "role": role,
            "stageIndex": index,
            "path": path.name,
            "sha256": expected,
            "bytes": path.stat().st_size,
        })
    document = dict(document)
    document["stages"] = normalized
    if source_digest:
        document["sourceManifestSha256"] = source_digest
    output_root.mkdir(parents=True, exist_ok=True)
    target = output_root / "qwen-stage-manifest.runtime.json"
    target.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    return target, document


def validate_manifest(manifest_path: Path, model_root: Path, *,
                      expected_model: str, expected_revision: str,
                      expected_model_digest: str,
                      expected_prompt_digest: str,
                      verifier=verify_external_manifest
                      ) -> dict[str, Any]:
    """Validate the complete mounted QWEN-F identity before protocol startup."""
    if not manifest_path.is_file():
        raise QwenInputError("MODEL_MANIFEST_MISSING")
    if not model_root.is_dir():
        raise QwenInputError("MODEL_ROOT_MISSING")
    manifest = _read_json(manifest_path, "MODEL_MANIFEST")
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise QwenInputError("MODEL_MANIFEST_SCHEMA_UNSUPPORTED")
    if manifest.get("modelFamily") != MODEL_FAMILY:
        raise QwenInputError("MODEL_MANIFEST_MODEL_FAMILY_MISMATCH")
    model = manifest.get("model")
    model_id = model.get("id") if isinstance(model, Mapping) else model
    if model_id != expected_model or model_id != MODEL_ID:
        raise QwenInputError("MODEL_MANIFEST_MODEL_ID_MISMATCH")
    if manifest.get("modelRevision") != expected_revision:
        raise QwenInputError("MODEL_MANIFEST_REVISION_MISMATCH")
    expected_model_digest = _digest(expected_model_digest, "MODEL_IDENTITY")
    if manifest.get("modelIdentityDigest") != expected_model_digest:
        raise QwenInputError("MODEL_MANIFEST_IDENTITY_DIGEST_MISMATCH")
    runtime = manifest.get("runtime")
    if not isinstance(runtime, Mapping):
        raise QwenInputError("MODEL_MANIFEST_RUNTIME_INVALID")
    if runtime.get("backend") not in {"onnxruntime", "onnxruntime-cuda"}:
        raise QwenInputError("MODEL_MANIFEST_BACKEND_INVALID")
    if runtime.get("executionProvider") != "cuda":
        raise QwenInputError("MODEL_MANIFEST_EXECUTION_PROVIDER_INVALID")
    if runtime.get("cpuFallback") is not False:
        raise QwenInputError("MODEL_MANIFEST_CPU_FALLBACK_ENABLED")
    stages = manifest.get("stages")
    if not isinstance(stages, list) or len(stages) != 3:
        raise QwenInputError("MODEL_MANIFEST_STAGE_COUNT_INVALID")
    # Signature verification is deliberately before object enumeration.
    verifier(manifest_path)
    graph = manifest.get("graph")
    _check_object(model_root, graph, "GRAPH")
    initializers = manifest.get("initializers")
    if not isinstance(initializers, list) or not initializers:
        raise QwenInputError("INITIALIZERS_INVALID")
    for index, item in enumerate(initializers):
        _check_object(model_root, item, f"INITIALIZER_{index}")
    tokenizer = manifest.get("tokenizer")
    tokenizer_path = _resolve_model_path(
        model_root, _descriptor_path(tokenizer, "TOKENIZER"), "TOKENIZER")
    if not tokenizer_path.is_dir():
        raise QwenInputError("TOKENIZER_ROOT_MISSING")
    for name in ("tokenizer.json", "tokenizer_config.json"):
        if not (tokenizer_path / name).is_file():
            raise QwenInputError("TOKENIZER_FILE_MISSING:" + name)
    prompt_digest = _digest(expected_prompt_digest, "PROMPT")
    prompt = _prompt_text(manifest, model_root, prompt_digest)
    # Validate stage manifest and each stage object before writing runtime state.
    stage_manifest_source = manifest.get("stageManifest")
    if not isinstance(stage_manifest_source, Mapping) or "stages" not in stage_manifest_source:
        _descriptor_path(stage_manifest_source, "STAGE_MANIFEST")
    return {
        "manifest": manifest,
        "prompt": prompt,
        "tokenizerRoot": tokenizer_path,
        "stageManifestSource": stage_manifest_source,
    }


def build_delegate_argv(*, stage_manifest: Path, model_root: Path,
                        tokenizer_root: Path, output_root: Path,
                        model: str, revision: str,
                        prompt: str, request_id: str,
                        workload_digest: str, model_digest: str,
                        native_requester_config: str = "") -> list[str]:
    """Build the fixed request-first Qwen command; caller supplies no options."""
    if not request_id or not _SAFE_ID_RE.fullmatch(request_id):
        raise QwenInputError("REQUEST_ID_INVALID")
    native_requester = bool(native_requester_config)
    argv = [
        sys.executable,
        str(REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"),
        "--runtime", (
            "qwen-onnx-cpu-native" if native_requester else "qwen-onnx"
        ),
        "--stages", "3",
        "--qwen-model", model,
        "--qwen-revision", revision,
        "--qwen-execution-provider", "cuda",
        "--qwen-device-ids", "0,1,2",
        "--qwen-stage-manifest", str(stage_manifest),
        "--qwen-stage-root", str(model_root),
        "--request-id", request_id,
        "--max-new-tokens", "8",
        "--initial-sync-settle-s", "5",
        "--ack-timeout-ms", "1500",
        "--timeout-ms", "60000",
        "--warmup-requests", "0",
        "--measured-requests", "2",
        "--prompt", prompt,
        "--qwen-tokenizer-dir", str(tokenizer_root),
        "--workload-digest", workload_digest,
        "--model-identity-digest", model_digest,
        "--output-dir", str(output_root),
    ]
    if not native_requester:
        argv.insert(argv.index("--request-id"), "--selection-dataflow-v3")
    if native_requester_config:
        argv += ["--native-requester-config", native_requester_config]
    return argv


def run_from_environment() -> int:
    """Validate mounted inputs and replace this process with the maintained runner."""
    if sys.argv[1:] != ["--case", "QWEN-F"]:
        raise QwenInputError("CASE_ARGUMENT_MISMATCH")
    manifest_path = Path(os.environ.get("SPEC180_MODEL_MANIFEST", ""))
    model_root = Path(os.environ.get("SPEC180_MODEL_ROOT", ""))
    output_root = Path(os.environ.get("SPEC180_OUTPUT_ROOT", ""))
    model = os.environ.get("SPEC180_QWEN_MODEL", "")
    revision = os.environ.get("SPEC180_QWEN_REVISION", "")
    model_digest = os.environ.get("SPEC180_QWEN_MODEL_IDENTITY_DIGEST", "")
    native_requester_config = os.environ.get(
        "SPEC180_NATIVE_REQUESTER_CONFIG", "")
    prompt_digest = os.environ.get("SPEC180_QWEN_PROMPT_DIGEST", "")
    workload_digest_raw = os.environ.get("SPEC180_WORKLOAD_SHA256", "")
    if not output_root.is_absolute() or not output_root.is_dir():
        raise QwenInputError("OUTPUT_ROOT_MISSING")
    if (not model or not revision or not model_digest or not prompt_digest
            or not workload_digest_raw):
        raise QwenInputError("QWEN_IDENTITY_ENVIRONMENT_MISSING")
    if native_requester_config:
        config_path = Path(native_requester_config).expanduser()
        if not config_path.is_file():
            raise QwenInputError("NATIVE_REQUESTER_CONFIG_MISSING")
    workload_digest = _digest(workload_digest_raw, "WORKLOAD")
    validated = validate_manifest(
        manifest_path, model_root, expected_model=model,
        expected_revision=revision, expected_model_digest=model_digest,
        expected_prompt_digest=prompt_digest)
    stage_manifest, _ = _legacy_stage_manifest(
        validated["manifest"], model_root, output_root)
    request_id = os.environ.get("SPEC180_RUN_ID", "")
    argv = build_delegate_argv(
        stage_manifest=stage_manifest, model_root=model_root,
        tokenizer_root=validated["tokenizerRoot"], output_root=output_root,
        model=model, revision=revision,
        prompt=validated["prompt"], request_id=request_id,
        workload_digest=workload_digest, model_digest=model_digest,
        native_requester_config=native_requester_config)
    print("SPEC180_QWEN_INPUTS_VALIDATED backend=onnxruntime-cuda "
          "stages=3 cpuFallback=false", flush=True)
    os.execvpe(argv[0], argv, os.environ.copy())
    return 0  # pragma: no cover


def main() -> int:
    try:
        return run_from_environment()
    except QwenInputError as exc:
        print("SPEC180_QWEN status=WAITING_EXTERNAL_INPUT error=" + str(exc),
              file=sys.stderr, flush=True)
        return 78


if __name__ == "__main__":
    raise SystemExit(main())
