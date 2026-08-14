"""Validation LLM pipeline example helpers.

This example is intentionally a small, deterministic runtime.  It proves that
NDNSF-DI can coordinate multiple LLM pipeline stages across providers without
claiming that a real Qwen/Llama transformer has been partitioned at layer
boundaries.
"""

from __future__ import annotations

import argparse
import gc
import hashlib
import inspect
import json
import os
import shutil
import struct
import time
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

from ndnsf_distributed_inference.plan import PlannerKind
from ndnsf_distributed_inference.splitter import (
    SplitArtifact,
    SplitServiceSpec,
    SplitterOutput,
)
from ndnsf_distributed_inference.llm_stub_planner import (
    llm_planner_registry,
    llm_planner_request,
    llm_splitter_output_from_result,
)


SERVICE = "/AI/LLM/Pipeline/Fake"
MODEL_NAME = "/Model/LLM/Pipeline/Fake"
DEFAULT_CONTROLLER = "/NDNSF-DistributeInference/example/controller"
DEFAULT_GROUP = "/NDNSF-DistributeInference/example/group"
DEFAULT_USER = "/NDNSF-DistributeInference/example/user"
DEFAULT_PROVIDER_PREFIX = "/NDNSF-DistributeInference/example/provider"
TINY_TRANSFORMERS_RUNTIME = "tiny-transformers"
QWEN_TRANSFORMERS_RUNTIME = "qwen-transformers"
QWEN_ONNX_RUNTIME = "qwen-onnx"
MAX_QWEN_GENERATED_TOKENS = 64


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _is_loadable_torch_file(torch: Any, path: Path) -> bool:
    if not path.exists() or path.stat().st_size == 0:
        return False
    try:
        torch.load(path, map_location="cpu")
        return True
    except Exception:
        return False


def _safe_torch_save(torch: Any, package: dict[str, Any], path: Path) -> None:
    """Atomically write a torch artifact and avoid reusing partial files."""
    path.parent.mkdir(parents=True, exist_ok=True)
    if _is_loadable_torch_file(torch, path):
        return
    if path.exists():
        path.unlink()

    tmp_path = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    if tmp_path.exists():
        tmp_path.unlink()
    try:
        torch.save(package, tmp_path)
        os.replace(tmp_path, path)
    except Exception as exc:
        tmp_path.unlink(missing_ok=True)
        usage = shutil.disk_usage(path.parent)
        free_gib = usage.free / (1024 ** 3)
        raise RuntimeError(
            f"failed to write torch artifact {path}; "
            f"free space in {path.parent} is {free_gib:.2f} GiB"
        ) from exc


def _content_addressed_torch_save(
    torch: Any,
    package: dict[str, Any],
    run_path: Path,
    content_store: str | Path,
    *,
    identity: Mapping[str, Any],
) -> str:
    """Persist one immutable stage and expose only a run-local symlink.

    The semantic identity index avoids rewriting the same stage on later
    requests. The object filename is still a SHA-256 digest, so an interrupted
    or corrupted entry cannot be accepted merely because its identity matches.
    """

    store = Path(content_store).expanduser().resolve()
    objects = store / "sha256"
    objects.mkdir(parents=True, exist_ok=True)
    index_path = store / "index-v1.json"
    index: dict[str, Any] = {
        "schema": "ndnsf-di-content-store-v1",
        "entries": {},
    }
    if index_path.exists():
        try:
            loaded = json.loads(index_path.read_text(encoding="utf-8"))
            if loaded.get("schema") == index["schema"]:
                index["entries"] = dict(loaded.get("entries") or {})
        except (OSError, TypeError, ValueError):
            # Object names are hashes, so a damaged index is recoverable.
            pass

    identity_key = hashlib.sha256(json.dumps(
        dict(identity), sort_keys=True, separators=(",", ":")
    ).encode("utf-8")).hexdigest()
    digest = str(index["entries"].get(identity_key, ""))
    object_path = (
        objects / f"{digest[7:]}.pt"
        if digest.startswith("sha256:") and len(digest) == 71 else None
    )
    if object_path is None or not _is_loadable_torch_file(torch, object_path):
        tmp_path = objects / f".{identity_key}.tmp-{os.getpid()}"
        tmp_path.unlink(missing_ok=True)
        _safe_torch_save(torch, package, tmp_path)
        raw_digest = _sha256_file(tmp_path)
        digest = f"sha256:{raw_digest}"
        object_path = objects / f"{raw_digest}.pt"
        if object_path.exists() and _is_loadable_torch_file(torch, object_path):
            tmp_path.unlink(missing_ok=True)
        else:
            os.replace(tmp_path, object_path)

    index["entries"][identity_key] = digest
    index_tmp = index_path.with_name(f".{index_path.name}.tmp-{os.getpid()}")
    index_tmp.write_text(
        json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(index_tmp, index_path)

    run_path.parent.mkdir(parents=True, exist_ok=True)
    if run_path.exists() or run_path.is_symlink():
        run_path.unlink()
    run_path.symlink_to(os.path.relpath(object_path, run_path.parent))
    return digest


def role_name(index: int) -> str:
    return f"/LLM/Pipeline/Stage/{index}"


def role_index(role: str) -> int:
    marker = "/LLM/Pipeline/Stage/"
    if not role.startswith(marker):
        raise ValueError(f"not an LLM pipeline stage role: {role}")
    return int(role[len(marker):])


def split_layer_ranges(layer_count: int, stages: int) -> list[tuple[int, int]]:
    if layer_count <= 0:
        raise ValueError("layer_count must be positive")
    if stages <= 0:
        raise ValueError("stages must be positive")
    if stages > layer_count:
        raise ValueError("stages cannot exceed layer_count")
    return [
        ((index * layer_count) // stages, ((index + 1) * layer_count) // stages)
        for index in range(stages)
    ]


def encode_prompt(prompt: str, *, request_id: str = "manual") -> bytes:
    return json.dumps({
        "schema": "ndnsf-di-llm-pipeline-input-v1",
        "requestId": request_id,
        "prompt": prompt,
    }, sort_keys=True).encode("utf-8")


def decode_payload(payload: bytes) -> dict[str, Any]:
    return json.loads(payload.decode("utf-8"))


@dataclass(frozen=True)
class BoundedQwenGenerationResult:
    status: str
    stop_reason: str
    generation_id: str
    generated_token_ids: tuple[int, ...]
    decoded_text: str
    exact_reference_match: bool
    ttft_ms: float
    inter_token_ms: tuple[float, ...]
    total_ms: float
    tokens_per_second: float
    token_steps: tuple[dict[str, Any], ...]
    error: str = ""
    reference_acceptance: str = "EXACT"
    reference_evidence_digest: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "schemaVersion": "ndnsf-di-qwen-bounded-generation-v1",
            "status": self.status,
            "stopReason": self.stop_reason,
            "generationId": self.generation_id,
            "generatedTokenIds": list(self.generated_token_ids),
            "decodedText": self.decoded_text,
            "exactReferenceMatch": self.exact_reference_match,
            "referenceAcceptance": self.reference_acceptance,
            "referenceEvidenceDigest": self.reference_evidence_digest,
            "ttftMs": self.ttft_ms,
            "interTokenMs": list(self.inter_token_ms),
            "totalMs": self.total_ms,
            "tokensPerSecond": self.tokens_per_second,
            "tokenSteps": list(self.token_steps),
            "error": self.error,
        }


def _qwen_step_token(value: Any) -> tuple[int, dict[str, Any]]:
    if isinstance(value, bool):
        raise ValueError("Qwen token step returned a boolean")
    if isinstance(value, int):
        token = value
        metadata = {}
    elif isinstance(value, dict):
        if "tokenId" in value:
            token = value["tokenId"]
        elif "topToken" in value:
            token = value["topToken"]
        else:
            raise ValueError("Qwen token step result lacks tokenId/topToken")
        metadata = {
            key: item
            for key, item in value.items()
            if key not in {"tokenId", "topToken"}
        }
    else:
        raise TypeError("Qwen token step must return an integer or dictionary")
    token = int(token)
    if token < 0:
        raise ValueError("Qwen token step returned a negative token ID")
    return token, metadata


def run_bounded_qwen_generation(
    *,
    input_token_ids: Sequence[int],
    max_new_tokens: int,
    eos_token_ids: Iterable[int],
    generation_id: str,
    token_step: Callable[[tuple[int, ...], int, str], Any],
    expected_token_ids: Sequence[int] | None = None,
    require_eos: bool = True,
    decode: Callable[[Sequence[int]], str] | None = None,
    clock: Callable[[], float] = time.perf_counter,
) -> BoundedQwenGenerationResult:
    """Run one callback-driven greedy generation with exact evidence.

    The callback owns one real or fixture token step. This function owns only
    application generation semantics: context growth, stable request IDs,
    reference comparison, EOS/limit classification, decoding, and timing.
    """

    initial = tuple(int(token) for token in input_token_ids)
    if not initial or any(token < 0 for token in initial):
        raise ValueError("Qwen generation input token IDs must be non-empty and non-negative")
    if max_new_tokens < 1 or max_new_tokens > MAX_QWEN_GENERATED_TOKENS:
        raise ValueError(
            f"max_new_tokens must be between 1 and {MAX_QWEN_GENERATED_TOKENS}")
    eos = frozenset(int(token) for token in eos_token_ids)
    if not eos or any(token < 0 for token in eos):
        raise ValueError("Qwen generation EOS token IDs must be non-empty and non-negative")
    if not generation_id:
        raise ValueError("Qwen generation ID must not be empty")
    expected = (
        tuple(int(token) for token in expected_token_ids)
        if expected_token_ids is not None else None
    )
    if expected is not None and any(token < 0 for token in expected):
        raise ValueError("Qwen expected token IDs must be non-negative")

    generated: list[int] = []
    steps: list[dict[str, Any]] = []
    completion_times: list[float] = []
    started = clock()
    status = "FAILED"
    stop_reason = "REQUEST_FAILURE"
    error = ""

    for token_epoch in range(max_new_tokens):
        context = initial + tuple(generated)
        request_id = f"{generation_id}-token-{token_epoch}"
        step_started = clock()
        try:
            token, metadata = _qwen_step_token(
                token_step(context, token_epoch, request_id))
        except Exception as exc:
            step_ended = clock()
            steps.append({
                "tokenEpoch": token_epoch,
                "requestId": request_id,
                "contextTokenCount": len(context),
                "contextSha256": hashlib.sha256(json.dumps(
                    list(context), separators=(",", ":")).encode("utf-8")
                ).hexdigest(),
                "durationMs": max(0.0, (step_ended - step_started) * 1000.0),
                "status": "FAILED",
                "error": str(exc),
            })
            error = str(exc)
            break

        step_ended = clock()
        completion_times.append(step_ended)
        expected_token = (
            expected[token_epoch]
            if expected is not None and token_epoch < len(expected) else None
        )
        step_record = {
            "tokenEpoch": token_epoch,
            "requestId": request_id,
            "contextTokenCount": len(context),
            "contextSha256": hashlib.sha256(json.dumps(
                list(context), separators=(",", ":")).encode("utf-8")
            ).hexdigest(),
            "expectedTokenId": expected_token,
            "actualTokenId": token,
            "durationMs": max(0.0, (step_ended - step_started) * 1000.0),
            "status": "OK",
            "error": "",
        }
        if metadata:
            step_record["transport"] = metadata
        generated.append(token)

        if expected is not None and (
            expected_token is None or token != expected_token
        ):
            step_record["status"] = "FAILED"
            step_record["error"] = (
                f"TOKEN_MISMATCH index={token_epoch} "
                f"expected={expected_token} actual={token}"
            )
            steps.append(step_record)
            status = "FAILED"
            stop_reason = "TOKEN_MISMATCH"
            error = step_record["error"]
            break

        steps.append(step_record)
        if token in eos:
            non_eos = [item for item in generated if item not in eos]
            if not non_eos:
                status = "FAILED"
                stop_reason = "EMPTY_OUTPUT"
                error = "generation emitted EOS before a non-special token"
            elif expected is not None and tuple(generated) != expected:
                status = "FAILED"
                stop_reason = "TOKEN_MISMATCH"
                error = (
                    "TOKEN_MISMATCH generation ended before the complete "
                    f"reference sequence expected={len(expected)} "
                    f"actual={len(generated)}"
                )
            else:
                status = "OK"
                stop_reason = "EOS"
            break
    else:
        if (
            not require_eos
            and expected is not None
            and tuple(generated) == expected
        ):
            status = "OK"
            stop_reason = "MAX_NEW_TOKENS"
            error = ""
        else:
            status = "TRUNCATED"
            stop_reason = "TOKEN_LIMIT"
            error = f"generation reached {max_new_tokens} tokens without EOS"

    ended = clock()
    total_ms = max(0.0, (ended - started) * 1000.0)
    ttft_ms = (
        max(0.0, (completion_times[0] - started) * 1000.0)
        if completion_times else 0.0
    )
    inter_token_ms = tuple(
        max(0.0, (right - left) * 1000.0)
        for left, right in zip(completion_times, completion_times[1:])
    )
    decoded_text = ""
    if decode is not None and generated:
        try:
            decoded_text = str(decode(tuple(generated)))
        except Exception as exc:
            status = "FAILED"
            stop_reason = "REQUEST_FAILURE"
            error = f"generation decode failed: {exc}"
    if status == "OK" and not decoded_text.strip():
        status = "FAILED"
        stop_reason = "EMPTY_OUTPUT"
        error = "decoded generation text is empty"
    exact_reference_match = (
        status == "OK"
        and expected is not None
        and tuple(generated) == expected
    )
    tokens_per_second = (
        len(generated) / (total_ms / 1000.0)
        if generated and total_ms > 0 else 0.0
    )
    return BoundedQwenGenerationResult(
        status=status,
        stop_reason=stop_reason,
        generation_id=generation_id,
        generated_token_ids=tuple(generated),
        decoded_text=decoded_text,
        exact_reference_match=exact_reference_match,
        ttft_ms=ttft_ms,
        inter_token_ms=inter_token_ms,
        total_ms=total_ms,
        tokens_per_second=tokens_per_second,
        token_steps=tuple(steps),
        error=error,
    )


def run_full_qwen_generation(
    *,
    input_token_ids: Sequence[int],
    max_new_tokens: int,
    eos_token_ids: Iterable[int],
    generation_id: str,
    generation_call: Callable[[tuple[int, ...], int, str], Any],
    expected_token_ids: Sequence[int] | None = None,
    require_eos: bool = True,
    decode: Callable[[Sequence[int]], str] | None = None,
    numeric_equivalence: Mapping[str, Any] | None = None,
    clock: Callable[[], float] = time.perf_counter,
) -> BoundedQwenGenerationResult:
    """Validate one durable full-generation response.

    ``generation_call`` is invoked exactly once.  Provider-side autoregressive
    work, including any inter-stage control traffic, is internal to that one
    invocation and must return ``generatedTokenIds`` as a complete sequence.
    This deliberately keeps the old :func:`run_bounded_qwen_generation`
    token-step helper separate for diagnostics and compatibility experiments.
    """

    initial = tuple(int(token) for token in input_token_ids)
    if not initial or any(token < 0 for token in initial):
        raise ValueError("Qwen generation input token IDs must be non-empty and non-negative")
    if max_new_tokens < 1 or max_new_tokens > MAX_QWEN_GENERATED_TOKENS:
        raise ValueError(
            f"max_new_tokens must be between 1 and {MAX_QWEN_GENERATED_TOKENS}")
    eos = frozenset(int(token) for token in eos_token_ids)
    if not eos or any(token < 0 for token in eos):
        raise ValueError("Qwen generation EOS token IDs must be non-empty and non-negative")
    if not generation_id:
        raise ValueError("Qwen generation ID must not be empty")
    expected = (
        tuple(int(token) for token in expected_token_ids)
        if expected_token_ids is not None else None
    )
    started = clock()
    status = "FAILED"
    stop_reason = "REQUEST_FAILURE"
    error = ""
    generated: tuple[int, ...] = ()
    metadata: dict[str, Any] = {}
    completion_monotonic_ms: tuple[float, ...] = ()
    reference_acceptance = "EXACT"
    reference_evidence_digest = ""
    try:
        value = generation_call(initial, max_new_tokens, generation_id)
        if not isinstance(value, Mapping):
            raise TypeError("full-generation response must be a mapping")
        raw = value.get("generatedTokenIds")
        if not isinstance(raw, (list, tuple)):
            raise ValueError("full-generation response lacks generatedTokenIds")
        generated = tuple(int(token) for token in raw)
        if any(token < 0 for token in generated):
            raise ValueError("full-generation response contains a negative token ID")
        metadata = {
            str(key): item for key, item in value.items()
            if key not in {"generatedTokenIds", "topToken"}
        }
        raw_completion_times = value.get("tokenCompletionMonotonicMs", ())
        if raw_completion_times:
            if (value.get("tokenCompletionClock") != "CLOCK_MONOTONIC"
                    or not isinstance(raw_completion_times, (list, tuple))):
                raise ValueError(
                    "full-generation token completion clock is invalid")
            completion_monotonic_ms = tuple(
                float(item) for item in raw_completion_times)
            if (len(completion_monotonic_ms) != len(generated)
                    or any(right < left for left, right in zip(
                        completion_monotonic_ms,
                        completion_monotonic_ms[1:]))):
                raise ValueError(
                    "full-generation token completion timestamps are invalid")
    except Exception as exc:
        error = str(exc)

    if not error:
        if not generated:
            status, stop_reason, error = "FAILED", "EMPTY_OUTPUT", "full generation returned no tokens"
        elif len(generated) > max_new_tokens:
            status, stop_reason, error = "FAILED", "TOKEN_LIMIT", (
                f"full generation returned {len(generated)} tokens, limit {max_new_tokens}")
        elif require_eos and generated[-1] not in eos:
            status, stop_reason, error = "TRUNCATED", "TOKEN_LIMIT", (
                "full generation did not terminate with EOS")
        elif expected is not None and generated != expected:
            policy = dict(numeric_equivalence or {})
            divergence = dict(policy.get("firstDivergence") or {})
            allowed = {
                int(item) for item in policy.get("allowedTokenIds", ())
            }
            index = divergence.get("tokenIndex")
            equivalent = (
                policy.get("classification") ==
                "NUMERICALLY_EQUIVALENT_DIVERGENCE"
                and isinstance(index, int)
                and index >= 0
                and index < len(expected)
                and index < len(generated)
                and generated[:index] == expected[:index]
                and generated[index] != expected[index]
                and generated[index] in allowed
                and policy.get("evidenceDigest", "")
            )
            if equivalent:
                status = "OK"
                stop_reason = "EOS" if generated[-1] in eos else "MAX_NEW_TOKENS"
                error = ""
                reference_acceptance = "NUMERICALLY_EQUIVALENT_DIVERGENCE"
                reference_evidence_digest = str(policy["evidenceDigest"])
            else:
                status, stop_reason, error = "FAILED", "TOKEN_MISMATCH", (
                    f"TOKEN_MISMATCH expected={len(expected)} actual={len(generated)}")
        else:
            status = "OK"
            stop_reason = "EOS" if generated[-1] in eos else "MAX_NEW_TOKENS"

    decoded_text = ""
    if decode is not None and generated:
        try:
            decoded_text = str(decode(generated))
        except Exception as exc:
            status, stop_reason, error = "FAILED", "REQUEST_FAILURE", f"generation decode failed: {exc}"
    if status == "OK" and not decoded_text.strip():
        status, stop_reason, error = "FAILED", "EMPTY_OUTPUT", "decoded generation text is empty"
    ended = clock()
    total_ms = max(0.0, (ended - started) * 1000.0)
    ttft_ms = total_ms
    inter_token_ms: tuple[float, ...] = ()
    if completion_monotonic_ms:
        ttft_ms = max(0.0, completion_monotonic_ms[0] - started * 1000.0)
        inter_token_ms = tuple(
            max(0.0, right - left)
            for left, right in zip(
                completion_monotonic_ms, completion_monotonic_ms[1:]))
    return BoundedQwenGenerationResult(
        status=status,
        stop_reason=stop_reason,
        generation_id=generation_id,
        generated_token_ids=generated,
        decoded_text=decoded_text,
        exact_reference_match=(status == "OK" and expected is not None and generated == expected),
        ttft_ms=ttft_ms,
        inter_token_ms=inter_token_ms,
        total_ms=total_ms,
        tokens_per_second=(len(generated) / (total_ms / 1000.0)
                           if generated and total_ms > 0 else 0.0),
        token_steps=({"mode": "FULL", "metadata": metadata},) if metadata else (),
        error=error,
        reference_acceptance=reference_acceptance,
        reference_evidence_digest=reference_evidence_digest,
    )


def _call_with_supported_kwargs(fn: Any, **kwargs: Any) -> Any:
    signature = inspect.signature(fn)
    return fn(**{
        name: value
        for name, value in kwargs.items()
        if name in signature.parameters
    })


def create_tiny_transformer_model(layer_count: int):
    import torch
    from transformers import LlamaConfig, LlamaForCausalLM

    torch.manual_seed(7)
    config = LlamaConfig(
        vocab_size=257,
        hidden_size=32,
        intermediate_size=64,
        num_hidden_layers=int(layer_count),
        num_attention_heads=4,
        num_key_value_heads=4,
        max_position_embeddings=64,
        rope_theta=10000.0,
        pad_token_id=0,
        bos_token_id=1,
        eos_token_id=2,
    )
    model = LlamaForCausalLM(config)
    model.eval()
    return model


def _tiny_transformer_config_dict(layer_count: int, seed: int = 7) -> dict[str, Any]:
    return {
        "vocab_size": 257,
        "hidden_size": 32,
        "intermediate_size": 64,
        "num_hidden_layers": int(layer_count),
        "num_attention_heads": 4,
        "num_key_value_heads": 4,
        "max_position_embeddings": 64,
        "rope_theta": 10000.0,
        "pad_token_id": 0,
        "bos_token_id": 1,
        "eos_token_id": 2,
        "seed": int(seed),
    }


def _stage_state_dict(full_state: dict[str, Any], spec: dict[str, Any]) -> dict[str, Any]:
    start = int(spec["layerRange"]["start"])
    end = int(spec["layerRange"]["endExclusive"])
    stage_index = int(spec["stageIndex"])
    stage_count = int(spec["stageCount"])
    prefixes = [
        prefix
        for index in range(start, end)
        for prefix in (
            f"model.layers.{index}.",
            f"model.language_model.layers.{index}.",
        )
    ]
    if stage_index == 0:
        prefixes.extend((
            "model.embed_tokens.",
            "model.language_model.embed_tokens.",
        ))
    if stage_index == stage_count - 1:
        prefixes.extend((
            "model.norm.",
            "model.language_model.norm.",
            "lm_head.",
        ))
    return {
        key: value.detach().cpu()
        for key, value in full_state.items()
        if any(key.startswith(prefix) for prefix in prefixes)
    }


def _torch_state_identity(torch: Any, state: Mapping[str, Any]) -> str:
    """Hash model weights and metadata for immutable model identity binding."""

    digest = hashlib.sha256()
    for name in sorted(state):
        tensor = state[name].detach().cpu().contiguous()
        digest.update(name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(tensor.dtype).encode("ascii"))
        digest.update(repr(tuple(tensor.shape)).encode("ascii"))
        # A byte view avoids dtype-specific NumPy conversions such as bfloat16.
        digest.update(tensor.view(torch.uint8).numpy().tobytes())
    return "sha256:" + digest.hexdigest()


def _model_layer_count(model: Any) -> int:
    base = getattr(model, "model", None)
    layers = list(getattr(base, "layers", [])) if base is not None else []
    if not layers:
        raise RuntimeError("expected a decoder model with model.layers")
    return len(layers)


def tiny_transformer_stage_spec(*, role: str, stages: int,
                                layer_count: int, seed: int = 7) -> dict[str, Any]:
    index = role_index(role)
    ranges = split_layer_ranges(layer_count, stages)
    return {
        "schema": "ndnsf-di-llm-stage-artifact-v1",
        "runtime": TINY_TRANSFORMERS_RUNTIME,
        "role": role,
        "stageIndex": index,
        "stageCount": int(stages),
        "layerCount": int(layer_count),
        "layerRange": {
            "start": ranges[index][0],
            "endExclusive": ranges[index][1],
        },
        "seed": int(seed),
        "modelFamily": "llm",
        "modelFormat": "hf-transformers",
        "runtimeBackend": "transformers",
    }


def write_tiny_transformer_stage_artifacts(
    output_dir: str | Path,
    *,
    roles: list[str],
    stages: int,
    layer_count: int,
    content_store: str | Path = "",
) -> list[SplitArtifact]:
    import torch

    root = Path(output_dir) / "tiny-transformers-stage-artifacts"
    root.mkdir(parents=True, exist_ok=True)
    full_model = create_tiny_transformer_model(layer_count)
    full_state = full_model.state_dict()
    model_identity_digest = _torch_state_identity(torch, full_state)
    artifacts: list[SplitArtifact] = []
    for role in roles:
        spec = tiny_transformer_stage_spec(
            role=role,
            stages=stages,
            layer_count=layer_count,
        )
        filename = f"stage-{spec['stageIndex']}-tiny-transformers.pt"
        path = root / filename
        package = {
            "schema": "ndnsf-di-llm-stage-weights-v1",
            "spec": spec,
            "config": _tiny_transformer_config_dict(layer_count, spec.get("seed", 7)),
            "state_dict": _stage_state_dict(full_state, spec),
        }
        content_digest = ""
        if content_store:
            content_digest = _content_addressed_torch_save(
                torch, package, path, content_store,
                identity={
                    "model": "NDNSF/TinyTransformers-Fixture",
                    "revision": "seed-7-config-v1",
                    "modelIdentityDigest": model_identity_digest,
                    "spec": spec,
                    "config": _tiny_transformer_config_dict(
                        layer_count, spec.get("seed", 7)),
                })
        else:
            _safe_torch_save(torch, package, path)
        artifacts.append(SplitArtifact(
            role=role,
            path=str(path),
            artifact_name=f"/Model/LLM/Pipeline/TinyTransformers/{role.strip('/')}",
            filename=filename,
            kind="llm-stage-weights",
            backend="transformers",
            metadata={
                "runtime": TINY_TRANSFORMERS_RUNTIME,
                "stageIndex": spec["stageIndex"],
                "stageCount": spec["stageCount"],
                "layerCount": spec["layerCount"],
                "layerRange": dict(spec["layerRange"]),
                "modelFamily": "llm",
                "runtimeBackend": "transformers",
                **({"contentAddress": content_digest}
                   if content_digest else {}),
                "modelIdentityDigest": model_identity_digest,
            },
        ))
    return artifacts


def with_tiny_transformer_artifacts(
    splitter: SplitterOutput,
    *,
    output_dir: str | Path,
    stages: int,
    layer_count: int,
    content_store: str | Path = "",
) -> SplitterOutput:
    services: list[SplitServiceSpec] = []
    for service in splitter.services:
        if service.name != SERVICE:
            services.append(service)
            continue
        services.append(SplitServiceSpec(
            name=service.name,
            model_name=service.model_name,
            roles=list(service.roles),
            dependencies=list(service.dependencies),
            artifacts=write_tiny_transformer_stage_artifacts(
                output_dir,
                roles=list(service.roles),
                stages=stages,
                layer_count=layer_count,
                content_store=content_store,
            ),
            input_schema=dict(service.input_schema),
            output_schema=dict(service.output_schema),
            users=list(service.users),
            providers=list(service.providers),
            metadata={
                **dict(service.metadata),
                "execution_implemented": True,
                "runtime": TINY_TRANSFORMERS_RUNTIME,
            },
        ))
    return SplitterOutput(
        application=splitter.application,
        controller=splitter.controller,
        group=splitter.group,
        user=splitter.user,
        provider_prefix=splitter.provider_prefix,
        services=services,
        provider_identities=list(splitter.provider_identities),
        trust_app_roots=list(splitter.trust_app_roots),
        trust_anchor_file=splitter.trust_anchor_file,
        artifact_allowlist=list(splitter.artifact_allowlist),
        artifact_sandbox=dict(splitter.artifact_sandbox),
        metadata=dict(splitter.metadata),
    )


def qwen_transformer_stage_spec(*, role: str,
                                stages: int,
                                layer_count: int,
                                model_name: str) -> dict[str, Any]:
    index = role_index(role)
    ranges = split_layer_ranges(layer_count, stages)
    return {
        "schema": "ndnsf-di-qwen-stage-artifact-v1",
        "runtime": QWEN_TRANSFORMERS_RUNTIME,
        "role": role,
        "stageIndex": index,
        "stageCount": int(stages),
        "layerCount": int(layer_count),
        "layerRange": {
            "start": ranges[index][0],
            "endExclusive": ranges[index][1],
        },
        "model": model_name,
        "modelFamily": "llm",
        "modelFormat": "hf-transformers",
        "runtimeBackend": "transformers",
    }


def qwen_onnx_stage_spec(*, role: str,
                         stages: int,
                         layer_count: int,
                         model_name: str) -> dict[str, Any]:
    spec = qwen_transformer_stage_spec(
        role=role,
        stages=stages,
        layer_count=layer_count,
        model_name=model_name,
    )
    spec.update({
        "schema": "ndnsf-di-qwen-onnx-stage-artifact-v1",
        "runtime": QWEN_ONNX_RUNTIME,
        "modelFormat": "onnx",
        "runtimeBackend": "onnxruntime",
    })
    start = int(spec["layerRange"]["start"])
    end = int(spec["layerRange"]["endExclusive"])
    cache_inputs = [
        name
        for layer in range(start, end)
        for name in (f"past_key.{layer}", f"past_value.{layer}")
    ]
    cache_outputs = [
        name
        for layer in range(start, end)
        for name in (f"present_key.{layer}", f"present_value.{layer}")
    ]
    primary_inputs = (
        ["input_ids", "attention_mask", "position_ids"]
        if int(spec["stageIndex"]) == 0
        else ["attention_mask", "hidden_states", "position_ids"]
    )
    spec.update({
        "inputNames": [*primary_inputs, *cache_inputs],
        "outputNames": [
            "logits" if int(spec["stageIndex"]) == int(spec["stageCount"]) - 1
            else "hidden_states_out",
            *cache_outputs,
        ],
        "cacheInputs": cache_inputs,
        "cacheOutputs": cache_outputs,
    })
    return spec


def _serializable_tensor(value: Any) -> Any:
    if hasattr(value, "detach"):
        return value.detach().cpu().tolist()
    return value


def _ones_like_nested(value: Any) -> Any:
    if hasattr(value, "detach"):
        import torch

        return torch.ones_like(value, dtype=torch.long).cpu().tolist()
    if not value:
        return value
    if isinstance(value[0], list):
        return [[1 for _ in row] for row in value]
    return [1 for _ in value]


def _position_ids_for_nested(value: Any) -> Any:
    serializable = _serializable_tensor(value)
    if not serializable:
        return serializable
    if isinstance(serializable[0], list):
        return [list(range(len(row))) for row in serializable]
    return list(range(len(serializable)))


def encode_qwen_pipeline_context(
    input_ids: Any,
    *,
    attention_mask: Any = None,
    position_ids: Any = None,
    request_id: str = "manual",
    session_id: str = "",
    context_epoch: int = 0,
    generation: Mapping[str, Any] | None = None,
) -> bytes:
    """Encode the formal Qwen ONNX full-context input object.

    This object is the DI-level context contract.  Transport is intentionally
    separate: callers may pass the returned bytes inline when small, or publish
    it through NDNSF large-data and pass the standard reference payload.
    """

    serializable_ids = _serializable_tensor(input_ids)
    serializable_attention = (
        _serializable_tensor(attention_mask)
        if attention_mask is not None else
        _ones_like_nested(serializable_ids)
    )
    serializable_position = (
        _serializable_tensor(position_ids)
        if position_ids is not None else
        _position_ids_for_nested(serializable_ids)
    )
    return json.dumps({
        "schema": "ndnsf-di-qwen-pipeline-context-v1",
        "requestId": request_id,
        "sessionId": session_id,
        "contextEpoch": int(context_epoch),
        "inputIds": serializable_ids,
        "attentionMask": serializable_attention,
        "positionIds": serializable_position,
        "contextMode": "full",
        "delta": None,
        "kvCacheReference": None,
        "generation": dict(generation or {}),
    }, sort_keys=True).encode("utf-8")


def encode_qwen_pipeline_delta(
    delta_input_ids: Any,
    *,
    delta_attention_mask: Any = None,
    request_id: str = "manual",
    session_id: str,
    base_context_epoch: int,
    context_epoch: int,
    kv_cache_reference: dict[str, Any] | None = None,
) -> bytes:
    """Encode an append-only context delta for a cached Qwen ONNX session.

    The delta is not a standalone inference input.  Stage 0 must have a cached
    full context for ``session_id`` at ``base_context_epoch`` and expands this
    payload into a full-context object before running the ONNX stage.
    """

    serializable_delta_ids = _serializable_tensor(delta_input_ids)
    serializable_delta_attention = (
        _serializable_tensor(delta_attention_mask)
        if delta_attention_mask is not None else
        _ones_like_nested(serializable_delta_ids)
    )
    return json.dumps({
        "schema": "ndnsf-di-qwen-pipeline-context-v1",
        "requestId": request_id,
        "sessionId": session_id,
        "contextEpoch": int(context_epoch),
        "baseContextEpoch": int(base_context_epoch),
        "contextMode": "append-delta",
        "inputIds": None,
        "attentionMask": None,
        "positionIds": None,
        "delta": {
            "inputIds": serializable_delta_ids,
            "attentionMask": serializable_delta_attention,
        },
        "kvCacheReference": kv_cache_reference,
    }, sort_keys=True).encode("utf-8")


def encode_qwen_input_ids(input_ids: Any, *, request_id: str = "manual") -> bytes:
    """Compatibility wrapper for older Qwen pipeline callers."""

    return encode_qwen_pipeline_context(input_ids, request_id=request_id)


def decode_qwen_pipeline_context(payload: bytes) -> dict[str, Any]:
    doc = decode_payload(payload)
    mode = doc.get("contextMode", "full")
    if mode == "append-delta":
        delta = doc.get("delta") or {}
        if not doc.get("sessionId"):
            raise ValueError("append-delta Qwen context requires sessionId")
        if "inputIds" not in delta:
            raise ValueError("append-delta Qwen context requires delta.inputIds")
        delta_attention = delta.get("attentionMask")
        if delta_attention is None:
            delta_attention = _ones_like_nested(delta["inputIds"])
        return {
            **doc,
            "contextMode": "append-delta",
            "sessionId": doc.get("sessionId", ""),
            "contextEpoch": int(doc.get("contextEpoch", 0) or 0),
            "baseContextEpoch": int(doc.get("baseContextEpoch", 0) or 0),
            "delta": {
                **delta,
                "attentionMask": delta_attention,
            },
        }
    if mode != "full":
        raise ValueError(f"unsupported Qwen context mode: {mode}")
    if "inputIds" not in doc:
        raise ValueError("Qwen pipeline context requires inputIds")
    input_ids = doc["inputIds"]
    attention_mask = doc.get("attentionMask")
    position_ids = doc.get("positionIds")
    if attention_mask is None:
        attention_mask = _ones_like_nested(input_ids)
    if position_ids is None:
        position_ids = _position_ids_for_nested(input_ids)
    return {
        **doc,
        "attentionMask": attention_mask,
        "positionIds": position_ids,
        "contextMode": doc.get("contextMode", "full"),
        "sessionId": doc.get("sessionId", ""),
        "contextEpoch": int(doc.get("contextEpoch", 0) or 0),
        "generation": dict(doc.get("generation") or {}),
    }


def _concat_nested_rows(left: Any, right: Any) -> Any:
    if not left:
        return right
    if not right:
        return left
    if isinstance(left[0], list):
        if not isinstance(right[0], list) or len(left) != len(right):
            raise ValueError("append-delta batch shape does not match cached context")
        return [list(base) + list(delta) for base, delta in zip(left, right)]
    if isinstance(right[0], list):
        raise ValueError("append-delta rank does not match cached context")
    return list(left) + list(right)


def merge_qwen_pipeline_delta(base_doc: dict[str, Any],
                              delta_doc: dict[str, Any]) -> dict[str, Any]:
    """Merge an append-only delta into a cached full Qwen context document."""

    if base_doc.get("contextMode", "full") != "full":
        raise ValueError("base Qwen context cache must contain a full context")
    if delta_doc.get("contextMode") != "append-delta":
        raise ValueError("delta Qwen context must use append-delta mode")
    base_epoch = int(base_doc.get("contextEpoch", 0) or 0)
    expected_epoch = int(delta_doc.get("baseContextEpoch", 0) or 0)
    if base_epoch != expected_epoch:
        raise ValueError(
            f"Qwen context epoch mismatch: cached {base_epoch}, delta expects {expected_epoch}")
    delta = delta_doc.get("delta") or {}
    input_ids = _concat_nested_rows(base_doc["inputIds"], delta["inputIds"])
    attention_mask = _concat_nested_rows(
        base_doc["attentionMask"],
        delta.get("attentionMask") or _ones_like_nested(delta["inputIds"]),
    )
    position_ids = _position_ids_for_nested(input_ids)
    return {
        **base_doc,
        "requestId": delta_doc.get("requestId", base_doc.get("requestId", "")),
        "sessionId": delta_doc.get("sessionId", base_doc.get("sessionId", "")),
        "contextEpoch": int(delta_doc.get("contextEpoch", expected_epoch + 1) or 0),
        "inputIds": input_ids,
        "attentionMask": attention_mask,
        "positionIds": position_ids,
        "contextMode": "full",
        "delta": None,
        "kvCacheReference": delta_doc.get("kvCacheReference"),
    }


def write_tiny_qwen3_transformer_stage_artifacts(
    output_dir: str | Path,
    *,
    roles: list[str],
    stages: int,
    layer_count: int = 3,
    vocab_size: int = 151936,
    hidden_size: int = 64,
    intermediate_size: int = 128,
    attention_heads: int = 4,
    key_value_heads: int = 2,
    seed: int = 168,
    content_store: str | Path = "",
) -> list[SplitArtifact]:
    """Write a bounded, random-weight Qwen3 fixture for local lifecycle gates.

    This is a real Qwen3 architecture executed by the production stage loader;
    it is not a pretrained quality or GPU-capacity subject.  Its distinct model
    identity prevents the local fixture from being confused with Qwen3-0.6B.
    """

    import torch
    from transformers.models.qwen3.configuration_qwen3 import Qwen3Config
    from transformers.models.qwen3.modeling_qwen3 import Qwen3ForCausalLM

    if (stages <= 0 or layer_count < stages or len(roles) != stages
            or hidden_size <= 0 or intermediate_size <= 0
            or attention_heads <= 0 or key_value_heads <= 0
            or hidden_size % attention_heads != 0
            or attention_heads % key_value_heads != 0
            or vocab_size <= 151645):
        raise ValueError("invalid tiny Qwen3 fixture configuration")
    expected_roles = [role_name(index) for index in range(stages)]
    if list(roles) != expected_roles:
        raise ValueError("tiny Qwen3 fixture roles must use canonical stage order")

    config = Qwen3Config(
        vocab_size=int(vocab_size),
        hidden_size=int(hidden_size),
        intermediate_size=int(intermediate_size),
        num_hidden_layers=int(layer_count),
        num_attention_heads=int(attention_heads),
        num_key_value_heads=int(key_value_heads),
        head_dim=int(hidden_size // attention_heads),
        max_position_embeddings=256,
        rope_theta=10000.0,
        pad_token_id=151643,
        bos_token_id=151643,
        eos_token_id=151645,
        tie_word_embeddings=False,
    )
    with torch.random.fork_rng(devices=[]):
        torch.manual_seed(int(seed))
        full_model = Qwen3ForCausalLM(config)
    full_model.eval()
    full_state = full_model.state_dict()
    model_identity_digest = _torch_state_identity(torch, full_state)
    model_name = "NDNSF/TinyQwen3-Fixture"
    root = Path(output_dir) / "qwen-transformers-stage-artifacts"
    root.mkdir(parents=True, exist_ok=True)
    artifacts: list[SplitArtifact] = []
    for role in roles:
        spec = qwen_transformer_stage_spec(
            role=role,
            stages=stages,
            layer_count=layer_count,
            model_name=model_name,
        )
        spec["fixtureProfile"] = "spec168-tiny-qwen3-v1"
        spec["fixtureSeed"] = int(seed)
        filename = f"stage-{spec['stageIndex']}-qwen-transformers.pt"
        path = root / filename
        package = {
            "schema": "ndnsf-di-qwen-stage-weights-v1",
            "spec": spec,
            "config": config.to_dict(),
            "attnImplementation": "sdpa",
            "state_dict": _stage_state_dict(full_state, spec),
        }
        content_digest = ""
        if content_store:
            content_digest = _content_addressed_torch_save(
                torch, package, path, content_store,
                identity={
                    "model": model_name,
                    "revision": "spec168-tiny-qwen3-v1",
                    "seed": int(seed),
                    "modelIdentityDigest": model_identity_digest,
                    "spec": spec,
                    "config": config.to_dict(),
                })
        else:
            _safe_torch_save(torch, package, path)
        artifacts.append(SplitArtifact(
            role=role,
            path=str(path),
            artifact_name=(
                f"/Model/LLM/Pipeline/TinyQwen3/{role.strip('/')}"),
            filename=filename,
            kind="llm-stage-weights",
            backend="transformers",
            metadata={
                "runtime": QWEN_TRANSFORMERS_RUNTIME,
                "stageIndex": spec["stageIndex"],
                "stageCount": spec["stageCount"],
                "layerCount": spec["layerCount"],
                "layerRange": dict(spec["layerRange"]),
                "modelFamily": "llm",
                "modelType": "qwen3",
                "runtimeBackend": "transformers",
                "fixtureProfile": "spec168-tiny-qwen3-v1",
                "fixtureSeed": int(seed),
                **({"contentAddress": content_digest}
                   if content_digest else {}),
                "modelIdentityDigest": model_identity_digest,
            },
        ))
    return artifacts


def write_qwen_transformer_stage_artifacts(
    output_dir: str | Path,
    *,
    roles: list[str],
    stages: int,
    model_name: str,
    model_revision: str = "main",
    prompt: str = "",
    allow_download: bool = False,
    dtype: str = "float32",
    content_store: str | Path = "",
) -> list[SplitArtifact]:
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    root = Path(output_dir) / "qwen-transformers-stage-artifacts"
    root.mkdir(parents=True, exist_ok=True)
    local_files_only = not allow_download
    torch_dtype = (torch.float32 if dtype == "float32" else
                   torch.float16 if dtype == "float16" else "auto")
    tokenizer = AutoTokenizer.from_pretrained(
        model_name,
        revision=model_revision,
        local_files_only=local_files_only,
        trust_remote_code=True,
    )
    full_model = AutoModelForCausalLM.from_pretrained(
        model_name,
        revision=model_revision,
        local_files_only=local_files_only,
        trust_remote_code=True,
        torch_dtype=torch_dtype,
    )
    full_model.eval()
    layer_count = _model_layer_count(full_model)
    full_state = full_model.state_dict()
    model_identity_digest = _torch_state_identity(torch, full_state)
    artifacts: list[SplitArtifact] = []
    for role in roles:
        spec = qwen_transformer_stage_spec(
            role=role,
            stages=stages,
            layer_count=layer_count,
            model_name=model_name,
        )
        filename = f"stage-{spec['stageIndex']}-qwen-transformers.pt"
        path = root / filename
        package = {
            "schema": "ndnsf-di-qwen-stage-weights-v1",
            "spec": spec,
            "config": full_model.config.to_dict(),
            "attnImplementation": getattr(full_model.config, "_attn_implementation", ""),
            "state_dict": _stage_state_dict(full_state, spec),
        }
        content_digest = ""
        if content_store:
            content_digest = _content_addressed_torch_save(
                torch, package, path, content_store,
                identity={
                    "model": model_name,
                    "revision": model_revision,
                    "dtype": dtype,
                    "modelIdentityDigest": model_identity_digest,
                    "spec": spec,
                    "config": full_model.config.to_dict(),
                })
        else:
            _safe_torch_save(torch, package, path)
        artifacts.append(SplitArtifact(
            role=role,
            path=str(path),
            artifact_name=f"/Model/LLM/Pipeline/Qwen/{role.strip('/')}",
            filename=filename,
            kind="llm-stage-weights",
            backend="transformers",
            metadata={
                "runtime": QWEN_TRANSFORMERS_RUNTIME,
                "stageIndex": spec["stageIndex"],
                "stageCount": spec["stageCount"],
                "layerCount": spec["layerCount"],
                "layerRange": dict(spec["layerRange"]),
                "modelFamily": "llm",
                "runtimeBackend": "transformers",
                **({"contentAddress": content_digest}
                   if content_digest else {}),
                "modelIdentityDigest": model_identity_digest,
            },
        ))
    if prompt:
        with torch.no_grad():
            tokens = tokenizer(prompt, return_tensors="pt")
            input_ids = tokens["input_ids"]
            attention_mask = tokens.get("attention_mask", torch.ones_like(input_ids))
            started = time.perf_counter()
            logits = full_model(
                input_ids=input_ids,
                attention_mask=attention_mask,
                use_cache=False,
            ).logits
            full_ms = (time.perf_counter() - started) * 1000.0
            top_token = int(torch.argmax(logits[:, -1, :], dim=-1).item())
        runtime_summary = {
            "schema": "ndnsf-di-qwen-pipeline-runtime-v1",
            "model": model_name,
            "prompt": prompt,
            "stages": stages,
            "layerCount": layer_count,
            "layerRanges": [list(item) for item in split_layer_ranges(layer_count, stages)],
            "inputIds": input_ids.cpu().tolist(),
            "attentionMask": attention_mask.cpu().tolist(),
            "expectedTopToken": top_token,
            "fullMs": full_ms,
        }
        (Path(output_dir) / "qwen-pipeline-runtime.json").write_text(
            json.dumps(runtime_summary, indent=2, sort_keys=True),
            encoding="utf-8",
        )
    return artifacts


def with_qwen_transformer_artifacts(
    splitter: SplitterOutput,
    *,
    output_dir: str | Path,
    stages: int,
    model_name: str,
    model_revision: str = "main",
    prompt: str = "",
    allow_download: bool = False,
    dtype: str = "float32",
    content_store: str | Path = "",
) -> SplitterOutput:
    services: list[SplitServiceSpec] = []
    for service in splitter.services:
        if service.name != SERVICE:
            services.append(service)
            continue
        services.append(SplitServiceSpec(
            name=service.name,
            model_name=model_name,
            roles=list(service.roles),
            dependencies=list(service.dependencies),
            artifacts=write_qwen_transformer_stage_artifacts(
                output_dir,
                roles=list(service.roles),
                stages=stages,
                model_name=model_name,
                model_revision=model_revision,
                prompt=prompt,
                allow_download=allow_download,
                dtype=dtype,
                content_store=content_store,
            ),
            input_schema=dict(service.input_schema),
            output_schema=dict(service.output_schema),
            users=list(service.users),
            providers=list(service.providers),
            metadata={
                **dict(service.metadata),
                "execution_implemented": True,
                "runtime": QWEN_TRANSFORMERS_RUNTIME,
                "model": model_name,
                "modelRevision": model_revision,
                "dtype": dtype,
            },
        ))
    return SplitterOutput(
        application=splitter.application,
        controller=splitter.controller,
        group=splitter.group,
        user=splitter.user,
        provider_prefix=splitter.provider_prefix,
        services=services,
        provider_identities=list(splitter.provider_identities),
        trust_app_roots=list(splitter.trust_app_roots),
        trust_anchor_file=splitter.trust_anchor_file,
        artifact_allowlist=list(splitter.artifact_allowlist),
        artifact_sandbox=dict(splitter.artifact_sandbox),
        metadata=dict(splitter.metadata),
    )


def _npz_payload(values: dict[str, Any]) -> bytes:
    import numpy as np

    buffer = BytesIO()
    np.savez_compressed(buffer, **values)
    return buffer.getvalue()


_TENSOR_DTYPE_TO_CODE = {
    "float32": 1,
    "float16": 2,
    "int64": 3,
    "bool": 4,
}
_TENSOR_CODE_TO_DTYPE = {value: key for key, value in _TENSOR_DTYPE_TO_CODE.items()}


def _native_tensor_bundle_payload(tensors: dict[str, Any]) -> bytes:
    import numpy as np

    if len(tensors) > 256:
        raise ValueError("native tensor bundle exceeds tensor-count limit")
    output = bytearray(b"NDITB001")
    output.extend(struct.pack("<I", len(tensors)))
    for name, value in tensors.items():
        name_bytes = str(name).encode("utf-8")
        array = np.ascontiguousarray(value)
        dtype_name = str(array.dtype)
        if not name_bytes or len(name_bytes) > 1024 or dtype_name not in _TENSOR_DTYPE_TO_CODE:
            raise ValueError(f"unsupported native tensor: {name!r} dtype={dtype_name}")
        if array.ndim > 16 or any(int(dim) < 0 for dim in array.shape):
            raise ValueError(f"invalid native tensor shape: {name!r} {array.shape}")
        payload = array.tobytes(order="C")
        output.extend(struct.pack("<I", len(name_bytes)))
        output.extend(name_bytes)
        output.extend(struct.pack("<II", _TENSOR_DTYPE_TO_CODE[dtype_name], array.ndim))
        for dim in array.shape:
            output.extend(struct.pack("<q", int(dim)))
        output.extend(struct.pack("<Q", len(payload)))
        output.extend(payload)
    return bytes(output)


def _decode_native_tensor_bundle(payload: bytes) -> dict[str, Any]:
    import numpy as np

    if not payload.startswith(b"NDITB001"):
        raise ValueError("payload is not an NDNSF-DI native tensor bundle")
    offset = 8

    def take(fmt: str):
        nonlocal offset
        size = struct.calcsize(fmt)
        if offset + size > len(payload):
            raise ValueError("truncated native tensor bundle")
        value = struct.unpack_from(fmt, payload, offset)
        offset += size
        return value[0] if len(value) == 1 else value

    count = take("<I")
    if count > 256:
        raise ValueError("native tensor bundle exceeds tensor-count limit")
    result = {}
    for _ in range(count):
        name_size = take("<I")
        if name_size < 1 or name_size > 1024 or offset + name_size > len(payload):
            raise ValueError("invalid native tensor name")
        name = payload[offset:offset + name_size].decode("utf-8")
        offset += name_size
        dtype_code, rank = take("<II")
        if dtype_code not in _TENSOR_CODE_TO_DTYPE or rank > 16:
            raise ValueError("unsupported native tensor dtype or rank")
        shape = tuple(int(take("<q")) for _ in range(rank))
        if any(dim < 0 for dim in shape):
            raise ValueError("invalid native tensor dimension")
        size = take("<Q")
        dtype = np.dtype(_TENSOR_CODE_TO_DTYPE[dtype_code])
        expected = int(np.prod(shape, dtype=np.int64)) * dtype.itemsize
        if size != expected or offset + size > len(payload):
            raise ValueError("native tensor payload size mismatch")
        result[name] = np.frombuffer(payload, dtype=dtype, count=expected // dtype.itemsize,
                                     offset=offset).reshape(shape).copy()
        offset += size
    if offset != len(payload):
        raise ValueError("native tensor bundle has trailing bytes")
    return result


def _load_qwen_hidden_payload(payload: bytes) -> tuple[dict[str, Any], str]:
    if payload.startswith(b"NDITB001"):
        return _decode_native_tensor_bundle(payload), "typed-tensor-bundle"
    import numpy as np

    with np.load(BytesIO(payload), allow_pickle=False) as data:
        return {name: data[name] for name in data.files}, "legacy-npz-comparison"


def _safe_array_text(value: Any) -> str:
    if getattr(value, "ndim", None) == 1 and hasattr(value, "tolist"):
        items = value.tolist()
        if all(isinstance(item, int) and 0 <= item <= 255 for item in items):
            return bytes(items).decode("utf-8")
    if hasattr(value, "item"):
        value = value.item()
    if isinstance(value, bytes):
        return value.decode("utf-8")
    return str(value)


def _utf8_text_tensor(value: str) -> Any:
    import numpy as np

    return np.frombuffer(str(value).encode("utf-8"), dtype=np.uint8).astype(
        np.int64)


def _onnx_stage_wrapper(model: Any):
    import torch
    from torch import nn

    stage_index = int(getattr(model, "ndnsf_stage_index"))
    stage_count = int(getattr(model, "ndnsf_stage_count"))
    start = int(getattr(model, "ndnsf_stage_start"))
    end = int(getattr(model, "ndnsf_stage_end"))

    layer_indices = list(range(start, end))

    class _ExportCache:
        def __init__(self, values):
            self.values = {
                layer: [values[index * 2], values[index * 2 + 1]]
                for index, layer in enumerate(layer_indices)
            }

        def update(self, key_states, value_states, layer_idx, cache_kwargs=None):
            del cache_kwargs
            past_key, past_value = self.values[int(layer_idx)]
            present_key = torch.cat((past_key, key_states), dim=2)
            present_value = torch.cat((past_value, value_states), dim=2)
            self.values[int(layer_idx)] = [present_key, present_value]
            return present_key, present_value

    class _QwenOnnxStage(nn.Module):
        def __init__(self):
            super().__init__()
            self.model = model

        def forward(self, input_ids, attention_mask, hidden_states, position_ids, *past_values):
            base = self.model.model
            if stage_index == 0:
                hidden_states = base.embed_tokens(input_ids)
            cache = _ExportCache(past_values)
            query_length = hidden_states.shape[1]
            past_length = past_values[0].shape[2]
            key_length = past_length + query_length
            query_positions = past_length + torch.arange(
                query_length, device=hidden_states.device)
            key_positions = torch.arange(key_length, device=hidden_states.device)
            allowed = key_positions.unsqueeze(0) <= query_positions.unsqueeze(1)
            minimum = torch.finfo(hidden_states.dtype).min
            causal_mask = torch.where(
                allowed,
                torch.zeros((), dtype=hidden_states.dtype, device=hidden_states.device),
                torch.full((), minimum, dtype=hidden_states.dtype,
                           device=hidden_states.device),
            ).unsqueeze(0).unsqueeze(0)
            padding_mask = (1 - attention_mask.to(hidden_states.dtype)).unsqueeze(1).unsqueeze(1)
            causal_mask = causal_mask + padding_mask * minimum
            position_embeddings = _rotary_embeddings(base, hidden_states, position_ids)
            for layer in list(base.layers):
                output = _call_with_supported_kwargs(
                    layer.forward,
                    hidden_states=hidden_states,
                    position_ids=position_ids,
                    position_embeddings=position_embeddings,
                    attention_mask=causal_mask,
                    past_key_value=cache,
                    use_cache=True,
                    cache_position=position_ids[0],
                    output_attentions=False,
                )
                hidden_states = output[0] if isinstance(output, tuple) else output
            primary = hidden_states
            if stage_index < stage_count - 1:
                primary = hidden_states
            else:
                primary = self.model.lm_head(base.norm(hidden_states))
            cache_outputs = tuple(
                value
                for layer in layer_indices
                for value in cache.values[layer]
            )
            return (primary, *cache_outputs)

    wrapper = _QwenOnnxStage()
    wrapper.eval()
    return wrapper, stage_index, stage_count, start, end


def _export_qwen_onnx_stage(model: Any, onnx_path: Path,
                            *, sample_input_ids: Any) -> dict[str, Any]:
    import torch

    wrapper, stage_index, stage_count, start, end = _onnx_stage_wrapper(model)
    hidden_size = int(model.config.hidden_size)
    seq_len = int(sample_input_ids.shape[1])
    dummy_hidden = torch.zeros(
        (int(sample_input_ids.shape[0]), seq_len, hidden_size),
        dtype=torch.float32,
    )
    position_ids = torch.arange(seq_len, dtype=torch.long).unsqueeze(0)
    layer_indices = list(range(start, end))
    kv_heads = int(getattr(model.config, "num_key_value_heads",
                           model.config.num_attention_heads))
    head_dim = int(hidden_size // model.config.num_attention_heads)
    past_values = tuple(
        torch.empty((int(sample_input_ids.shape[0]), kv_heads, 0, head_dim),
                    dtype=torch.float32)
        for _ in range(len(layer_indices) * 2)
    )
    attention_mask = torch.ones(
        (int(sample_input_ids.shape[0]), seq_len), dtype=torch.long)
    input_names = ["input_ids", "attention_mask", "hidden_states", "position_ids"] + [
        name for layer in layer_indices
        for name in (f"past_key.{layer}", f"past_value.{layer}")
    ]
    output_names = ["logits" if stage_index == stage_count - 1 else "hidden_states_out"] + [
        name for layer in layer_indices
        for name in (f"present_key.{layer}", f"present_value.{layer}")
    ]
    dynamic_axes = {
        "input_ids": {1: "seq"},
        "hidden_states": {1: "seq"},
        "position_ids": {1: "seq"},
        "attention_mask": {1: "total_seq"},
        output_names[0]: {1: "seq"},
    }
    for layer in layer_indices:
        dynamic_axes[f"past_key.{layer}"] = {2: "past_seq"}
        dynamic_axes[f"past_value.{layer}"] = {2: "past_seq"}
        dynamic_axes[f"present_key.{layer}"] = {2: "total_seq"}
        dynamic_axes[f"present_value.{layer}"] = {2: "total_seq"}
    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(
        wrapper,
        (sample_input_ids, attention_mask, dummy_hidden, position_ids, *past_values),
        str(onnx_path),
        input_names=input_names,
        output_names=output_names,
        dynamic_axes=dynamic_axes,
        opset_version=17,
        do_constant_folding=True,
    )
    import onnx

    graph = onnx.load(str(onnx_path), load_external_data=False).graph
    actual_input_names = [value.name for value in graph.input]
    actual_output_names = [value.name for value in graph.output]

    def contract(value):
        tensor = value.type.tensor_type
        dimensions = []
        for dim in tensor.shape.dim:
            dimensions.append(
                int(dim.dim_value) if dim.HasField("dim_value") else str(dim.dim_param)
            )
        return {"elementType": int(tensor.elem_type), "shape": dimensions}

    return {
        "stageIndex": stage_index,
        "stageCount": stage_count,
        "layerRange": {"start": start, "endExclusive": end},
        "inputNames": actual_input_names,
        "outputNames": actual_output_names,
        "cacheInputs": [name for name in actual_input_names if name.startswith("past_")],
        "cacheOutputs": [name for name in actual_output_names if name.startswith("present_")],
        "tensorContracts": {
            value.name: contract(value)
            for value in [*graph.input, *graph.output]
        },
    }


def _validate_qwen_onnx_stages(artifacts: list[SplitArtifact], *,
                               input_ids: Any, attention_mask: Any,
                               config: Any) -> dict[str, Any]:
    import numpy as np
    import onnxruntime as ort

    hidden = np.zeros(
        (int(input_ids.shape[0]), int(input_ids.shape[1]), int(config.hidden_size)),
        dtype=np.float32,
    )
    ids = input_ids.detach().cpu().numpy().astype(np.int64)
    mask = attention_mask.detach().cpu().numpy().astype(np.int64)
    position_ids = np.arange(ids.shape[1], dtype=np.int64).reshape(1, -1)
    kv_heads = int(getattr(config, "num_key_value_heads", config.num_attention_heads))
    head_dim = int(config.hidden_size // config.num_attention_heads)
    stage_records = []
    logits = None
    for artifact in artifacts:
        options = ort.SessionOptions()
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
        session = ort.InferenceSession(
            artifact.path, sess_options=options, providers=["CPUExecutionProvider"])
        feed = {}
        for item in session.get_inputs():
            if item.name == "input_ids":
                feed[item.name] = ids
            elif item.name == "attention_mask":
                feed[item.name] = mask
            elif item.name == "hidden_states":
                feed[item.name] = hidden
            elif item.name == "position_ids":
                feed[item.name] = position_ids
            elif item.name.startswith(("past_key.", "past_value.")):
                feed[item.name] = np.empty(
                    (ids.shape[0], kv_heads, 0, head_dim), dtype=np.float32)
            else:
                raise RuntimeError(f"unrecognized exported Qwen input: {item.name}")
        outputs = session.run(None, feed)
        primary = np.asarray(outputs[0])
        if artifact.metadata["stageIndex"] < artifact.metadata["stageCount"] - 1:
            hidden = primary.astype(np.float32, copy=False)
        else:
            logits = primary
        stage_records.append({
            "role": artifact.role,
            "primaryShape": list(primary.shape),
            "cacheOutputCount": len(outputs) - 1,
        })
        del session
    if logits is None:
        raise RuntimeError("Qwen stage validation produced no logits")
    return {
        "topToken": int(np.argmax(logits[:, -1, :], axis=-1)[0]),
        "logitsShape": list(logits.shape),
        "stages": stage_records,
    }


def write_qwen_onnx_stage_artifacts(
    output_dir: str | Path,
    *,
    roles: list[str],
    stages: int,
    model_name: str,
    model_revision: str = "main",
    prompt: str = "",
    allow_download: bool = False,
    dtype: str = "float32",
) -> list[SplitArtifact]:
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    root = Path(output_dir) / "qwen-onnx-stage-artifacts"
    root.mkdir(parents=True, exist_ok=True)
    local_files_only = not allow_download
    torch_dtype = (torch.float32 if dtype == "float32" else
                   torch.float16 if dtype == "float16" else "auto")
    tokenizer = AutoTokenizer.from_pretrained(
        model_name,
        revision=model_revision,
        local_files_only=local_files_only,
        trust_remote_code=True,
    )
    full_model = AutoModelForCausalLM.from_pretrained(
        model_name,
        revision=model_revision,
        local_files_only=local_files_only,
        trust_remote_code=True,
        torch_dtype=torch_dtype,
    )
    full_model.eval()
    layer_count = _model_layer_count(full_model)
    prompt_for_sample = prompt or "Explain NDNSF-DI pipeline inference."
    sample_tokens = tokenizer(prompt_for_sample, return_tensors="pt")
    sample_input_ids = sample_tokens["input_ids"]
    validation = None
    expected_top_token = None
    input_ids = None
    attention_mask = None
    full_ms = None
    if prompt:
        with torch.no_grad():
            tokens = tokenizer(prompt, return_tensors="pt")
            input_ids = tokens["input_ids"]
            attention_mask = tokens.get(
                "attention_mask", torch.ones_like(input_ids))
            started = time.perf_counter()
            logits = full_model(
                input_ids=input_ids,
                attention_mask=attention_mask,
                use_cache=False,
            ).logits
            full_ms = (time.perf_counter() - started) * 1000.0
            expected_top_token = int(
                torch.argmax(logits[:, -1, :], dim=-1).item())
        del logits

    model_config = full_model.config
    config_dict = model_config.to_dict()
    attn_implementation = getattr(model_config, "_attn_implementation", "")
    hidden_size = int(model_config.hidden_size)
    resolved_revision = str(
        getattr(model_config, "_commit_hash", "") or model_revision)
    full_state = full_model.state_dict()
    stage_packages: list[tuple[str, dict[str, Any], Path]] = []
    for role in roles:
        spec = qwen_onnx_stage_spec(
            role=role,
            stages=stages,
            layer_count=layer_count,
            model_name=model_name,
        )
        pt_path = root / f"stage-{spec['stageIndex']}-qwen-onnx-export.pt"
        package = {
            "schema": "ndnsf-di-qwen-stage-weights-v1",
            "spec": {
                **spec,
                "runtime": QWEN_TRANSFORMERS_RUNTIME,
                "modelFormat": "hf-transformers",
                "runtimeBackend": "transformers",
            },
            "config": config_dict,
            "attnImplementation": attn_implementation,
            "state_dict": _stage_state_dict(full_state, spec),
        }
        _safe_torch_save(torch, package, pt_path)
        stage_packages.append((role, spec, pt_path))
        del package

    # Stage packages are the immutable handoff.  Drop the full FP32 model
    # before ONNX tracing so the largest stage does not overlap the complete
    # model, its reconstructed stage model, and the tracing graph.
    del full_state
    del full_model
    gc.collect()

    artifacts: list[SplitArtifact] = []
    for role, spec, pt_path in stage_packages:
        stage_model = qwen_transformer_model_from_stage_package(pt_path)
        filename = f"stage-{spec['stageIndex']}-qwen.onnx"
        onnx_path = root / filename
        export_info = _export_qwen_onnx_stage(
            stage_model,
            onnx_path,
            sample_input_ids=sample_input_ids,
        )
        artifacts.append(SplitArtifact(
            role=role,
            path=str(onnx_path),
            artifact_name=f"/Model/LLM/Pipeline/QwenOnnx/{role.strip('/')}",
            filename=filename,
            kind="onnx-model",
            backend="onnxruntime",
            metadata={
                "runtime": QWEN_ONNX_RUNTIME,
                "stageIndex": spec["stageIndex"],
                "stageCount": spec["stageCount"],
                "layerCount": spec["layerCount"],
                "layerRange": dict(spec["layerRange"]),
                "modelFamily": "llm",
                "modelFormat": "onnx",
                "runtimeBackend": "onnxruntime",
                "hiddenSize": hidden_size,
                "inputNames": export_info["inputNames"],
                "outputNames": export_info["outputNames"],
                "cacheInputs": export_info["cacheInputs"],
                "cacheOutputs": export_info["cacheOutputs"],
                "tensorContracts": export_info["tensorContracts"],
            },
        ))
        del stage_model
        gc.collect()
    if prompt:
        assert input_ids is not None
        assert attention_mask is not None
        assert expected_top_token is not None
        assert full_ms is not None
        validation = _validate_qwen_onnx_stages(
            artifacts,
            input_ids=input_ids,
            attention_mask=attention_mask,
            config=model_config,
        )
        if validation["topToken"] != expected_top_token:
            raise RuntimeError(
                "staged Qwen ONNX top token differs from frozen full-model baseline: "
                f"{validation['topToken']} != {expected_top_token}")
        runtime_summary = {
            "schema": "ndnsf-di-qwen-onnx-pipeline-runtime-v1",
            "model": model_name,
            "modelRevision": model_revision,
            "dtype": dtype,
            "prompt": prompt,
            "runtime": QWEN_ONNX_RUNTIME,
            "stages": stages,
            "layerCount": layer_count,
            "layerRanges": [list(item) for item in split_layer_ranges(layer_count, stages)],
            "inputIds": input_ids.cpu().tolist(),
            "attentionMask": attention_mask.cpu().tolist(),
            "expectedTopToken": expected_top_token,
            "stagedValidation": validation,
            "fullMs": full_ms,
        }
        (Path(output_dir) / "qwen-pipeline-runtime.json").write_text(
            json.dumps(runtime_summary, indent=2, sort_keys=True),
            encoding="utf-8",
        )
    manifest = {
        "schema": "ndnsf-di-qwen-onnx-service-manifest-v1",
        "model": model_name,
        "modelRevision": resolved_revision,
        "dtype": dtype,
        "tokenizer": str(getattr(tokenizer, "name_or_path", model_name)),
        "stageCount": stages,
        "layerCount": layer_count,
        "expectedTopToken": expected_top_token,
        "stagedValidation": validation,
        "stages": [],
    }
    for artifact in artifacts:
        path = Path(artifact.path)
        manifest["stages"].append({
            "role": artifact.role,
            "stageIndex": artifact.metadata["stageIndex"],
            "layerRange": artifact.metadata["layerRange"],
            "path": str(path),
            "bytes": path.stat().st_size,
            "sha256": _sha256_file(path),
            "inputNames": artifact.metadata["inputNames"],
            "outputNames": artifact.metadata["outputNames"],
            "cacheInputs": artifact.metadata["cacheInputs"],
            "cacheOutputs": artifact.metadata["cacheOutputs"],
            "tensorContracts": artifact.metadata["tensorContracts"],
        })
    (Path(output_dir) / "qwen-onnx-service-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return artifacts


def with_qwen_onnx_artifacts(
    splitter: SplitterOutput,
    *,
    output_dir: str | Path,
    stages: int,
    model_name: str,
    model_revision: str = "main",
    prompt: str = "",
    allow_download: bool = False,
    dtype: str = "float32",
) -> SplitterOutput:
    services: list[SplitServiceSpec] = []
    for service in splitter.services:
        if service.name != SERVICE:
            services.append(service)
            continue
        services.append(SplitServiceSpec(
            name=service.name,
            model_name=model_name,
            roles=list(service.roles),
            dependencies=list(service.dependencies),
            artifacts=write_qwen_onnx_stage_artifacts(
                output_dir,
                roles=list(service.roles),
                stages=stages,
                model_name=model_name,
                model_revision=model_revision,
                prompt=prompt,
                allow_download=allow_download,
                dtype=dtype,
            ),
            input_schema=dict(service.input_schema),
            output_schema=dict(service.output_schema),
            users=list(service.users),
            providers=list(service.providers),
            metadata={
                **dict(service.metadata),
                "execution_implemented": True,
                "runtime": QWEN_ONNX_RUNTIME,
                "model": model_name,
                "modelRevision": model_revision,
                "dtype": dtype,
            },
        ))
    return SplitterOutput(
        application=splitter.application,
        controller=splitter.controller,
        group=splitter.group,
        user=splitter.user,
        provider_prefix=splitter.provider_prefix,
        services=services,
        provider_identities=list(splitter.provider_identities),
        trust_app_roots=list(splitter.trust_app_roots),
        trust_anchor_file=splitter.trust_anchor_file,
        artifact_allowlist=list(splitter.artifact_allowlist),
        artifact_sandbox=dict(splitter.artifact_sandbox),
        metadata=dict(splitter.metadata),
    )


def reuse_qwen_onnx_stage_artifacts(
    output_dir: str | Path,
    *,
    roles: list[str],
    artifact_store: str | Path,
    service_manifest_path: str | Path,
    runtime_manifest_path: str | Path,
) -> list[SplitArtifact]:
    """Bind reviewed Qwen metadata to a verified read-only Spec 107 store."""

    store = Path(artifact_store).resolve()
    store_manifest_path = store / "artifact-set.json"
    try:
        store_manifest = json.loads(store_manifest_path.read_text(encoding="utf-8"))
        service_manifest = json.loads(
            Path(service_manifest_path).read_text(encoding="utf-8"))
        runtime_manifest = json.loads(
            Path(runtime_manifest_path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"invalid reusable Qwen manifest: {exc}") from exc
    if (
        store_manifest.get("schema") != "ndnsf-di-spec107-artifact-set-v1"
        or store_manifest.get("retention") != "content-addressed-read-only"
        or not store.is_dir()
        or store.stat().st_mode & 0o222
    ):
        raise RuntimeError("Qwen artifact store is not a sealed Spec 107 store")
    expected_digest = str(store_manifest.get("artifactSetDigest", ""))
    if not expected_digest.startswith("sha256:") or store.name != expected_digest[7:]:
        raise RuntimeError("Qwen artifact store content address mismatch")
    store_rows = store_manifest.get("artifacts")
    service_rows = service_manifest.get("stages")
    if (
        not isinstance(store_rows, list) or len(store_rows) != 3
        or not isinstance(service_rows, list) or len(service_rows) != 3
        or service_manifest.get("schema") != "ndnsf-di-qwen-onnx-service-manifest-v1"
        or int(service_manifest.get("stageCount", 0)) != 3
    ):
        raise RuntimeError("Qwen reusable manifests require exactly three stages")
    store_by_role = {str(row.get("role", "")): row for row in store_rows}
    service_by_role = {str(row.get("role", "")): row for row in service_rows}
    if set(store_by_role) != set(roles) or set(service_by_role) != set(roles):
        raise RuntimeError("Qwen reusable manifest role set mismatch")

    artifacts: list[SplitArtifact] = []
    rebound_rows = []
    for role in roles:
        store_row = store_by_role[role]
        metadata_row = service_by_role[role]
        artifact_path = (store / str(store_row.get("path", ""))).resolve()
        try:
            artifact_path.relative_to(store)
        except ValueError as exc:
            raise RuntimeError("Qwen artifact escaped the sealed store") from exc
        expected_sha = str(store_row.get("sha256", ""))
        metadata_sha = str(metadata_row.get("sha256", ""))
        if metadata_sha.startswith("sha256:"):
            metadata_sha = metadata_sha[7:]
        actual_sha = _sha256_file(artifact_path) if artifact_path.is_file() else ""
        if (
            not artifact_path.is_file()
            or artifact_path.stat().st_mode & 0o222
            or artifact_path.stat().st_size != int(store_row.get("bytes", -1))
            or artifact_path.stat().st_size != int(metadata_row.get("bytes", -2))
            or "sha256:" + actual_sha != expected_sha
            or actual_sha != metadata_sha
        ):
            raise RuntimeError(f"Qwen artifact verification failed for {role}")
        rebound = dict(metadata_row)
        rebound["path"] = str(artifact_path)
        rebound["bytes"] = artifact_path.stat().st_size
        rebound["sha256"] = expected_sha[7:]
        rebound_rows.append(rebound)
        artifacts.append(SplitArtifact(
            role=role,
            path=str(artifact_path),
            artifact_name=f"/Model/LLM/Pipeline/QwenOnnx/{role.strip('/')}",
            filename=artifact_path.name,
            kind="onnx-model",
            backend="onnxruntime",
            metadata={
                "runtime": QWEN_ONNX_RUNTIME,
                "stageIndex": int(metadata_row["stageIndex"]),
                "stageCount": 3,
                "layerCount": int(service_manifest.get("layerCount", 0)),
                "layerRange": dict(metadata_row["layerRange"]),
                "modelFamily": "llm",
                "modelFormat": "onnx",
                "runtimeBackend": "onnxruntime",
                "inputNames": list(metadata_row["inputNames"]),
                "outputNames": list(metadata_row["outputNames"]),
                "cacheInputs": list(metadata_row["cacheInputs"]),
                "cacheOutputs": list(metadata_row["cacheOutputs"]),
                "tensorContracts": dict(metadata_row["tensorContracts"]),
            },
        ))
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    rebound_manifest = dict(service_manifest)
    rebound_manifest["stages"] = rebound_rows
    (output / "qwen-onnx-service-manifest.json").write_text(
        json.dumps(rebound_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    (output / "qwen-pipeline-runtime.json").write_text(
        json.dumps(runtime_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    return artifacts


def with_reused_qwen_onnx_artifacts(
    splitter: SplitterOutput,
    *,
    output_dir: str | Path,
    artifact_store: str | Path,
    service_manifest_path: str | Path,
    runtime_manifest_path: str | Path,
) -> SplitterOutput:
    services: list[SplitServiceSpec] = []
    for service in splitter.services:
        if service.name != SERVICE:
            services.append(service)
            continue
        services.append(SplitServiceSpec(
            name=service.name,
            model_name=service.model_name,
            roles=list(service.roles),
            dependencies=list(service.dependencies),
            artifacts=reuse_qwen_onnx_stage_artifacts(
                output_dir,
                roles=list(service.roles),
                artifact_store=artifact_store,
                service_manifest_path=service_manifest_path,
                runtime_manifest_path=runtime_manifest_path,
            ),
            input_schema=dict(service.input_schema),
            output_schema=dict(service.output_schema),
            users=list(service.users),
            providers=list(service.providers),
            metadata={
                **dict(service.metadata),
                "execution_implemented": True,
                "runtime": QWEN_ONNX_RUNTIME,
                "artifactRetention": "content-addressed-read-only",
            },
        ))
    return SplitterOutput(
        application=splitter.application,
        controller=splitter.controller,
        group=splitter.group,
        user=splitter.user,
        provider_prefix=splitter.provider_prefix,
        services=services,
        provider_identities=list(splitter.provider_identities),
        trust_app_roots=list(splitter.trust_app_roots),
        trust_anchor_file=splitter.trust_anchor_file,
        artifact_allowlist=list(splitter.artifact_allowlist),
        artifact_sandbox=dict(splitter.artifact_sandbox),
        metadata=dict(splitter.metadata),
    )


def tiny_transformer_stage_spec_from_execution(
    execution: Any,
    *,
    fallback_role: str,
    fallback_stages: int,
    fallback_layer_count: int,
) -> dict[str, Any]:
    artifact_paths = getattr(execution, "artifact_paths", {}) or {}
    model_path = artifact_paths.get("model")
    if model_path:
        path = Path(model_path)
        if path.suffix == ".pt":
            import torch

            package = torch.load(path, map_location="cpu", weights_only=True)
            spec = dict(package.get("spec", {}))
        else:
            spec = json.loads(path.read_text(encoding="utf-8"))
        if spec.get("runtime") != TINY_TRANSFORMERS_RUNTIME:
            raise ValueError(f"unexpected LLM stage artifact runtime: {spec.get('runtime')}")
        return spec
    metadata = getattr(getattr(execution, "spec", None), "metadata", {}) or {}
    if metadata.get("runtime") == TINY_TRANSFORMERS_RUNTIME:
        layer_range = dict(metadata.get("layerRange", {}) or {})
        return {
            "schema": "ndnsf-di-llm-stage-artifact-v1",
            "runtime": TINY_TRANSFORMERS_RUNTIME,
            "role": fallback_role,
            "stageIndex": int(metadata.get("stageIndex", role_index(fallback_role))),
            "stageCount": int(metadata.get("stageCount", fallback_stages)),
            "layerCount": int(metadata.get("layerCount", fallback_layer_count)),
            "layerRange": {
                "start": int(layer_range.get("start", 0)),
                "endExclusive": int(layer_range.get("endExclusive", fallback_layer_count)),
            },
            "seed": int(metadata.get("seed", 7)),
        }
    return tiny_transformer_stage_spec(
        role=fallback_role,
        stages=fallback_stages,
        layer_count=fallback_layer_count,
    )


def tiny_transformer_model_from_execution(
    execution: Any,
    *,
    fallback_layer_count: int,
) -> Any | None:
    artifact_paths = getattr(execution, "artifact_paths", {}) or {}
    model_path = artifact_paths.get("model")
    if not model_path or Path(model_path).suffix != ".pt":
        return None
    return tiny_transformer_model_from_stage_package(model_path, fallback_layer_count)


def tiny_transformer_model_from_stage_package(
    model_path: str | Path,
    fallback_layer_count: int,
) -> Any:
    import torch
    from transformers import LlamaConfig, LlamaForCausalLM

    package = torch.load(Path(model_path), map_location="cpu", weights_only=True)
    config = dict(package.get("config", {}))
    seed = int(config.pop("seed", 7))
    if not config:
        config = _tiny_transformer_config_dict(fallback_layer_count, seed)
        config.pop("seed", None)
    torch.manual_seed(seed)
    model = LlamaForCausalLM(LlamaConfig(**config))
    state_dict = package.get("state_dict", {})
    model.load_state_dict(state_dict, strict=False)
    model.eval()
    return model


def qwen_transformer_stage_spec_from_execution(execution: Any, *,
                                               fallback_role: str,
                                               fallback_stages: int) -> dict[str, Any]:
    artifact_paths = getattr(execution, "artifact_paths", {}) or {}
    model_path = artifact_paths.get("model")
    if not model_path:
        metadata = getattr(getattr(execution, "spec", None), "metadata", {}) or {}
        if metadata.get("runtime") == QWEN_TRANSFORMERS_RUNTIME:
            layer_range = dict(metadata.get("layerRange", {}) or {})
            return {
                "schema": "ndnsf-di-qwen-stage-artifact-v1",
                "runtime": QWEN_TRANSFORMERS_RUNTIME,
                "role": fallback_role,
                "stageIndex": int(metadata.get("stageIndex", role_index(fallback_role))),
                "stageCount": int(metadata.get("stageCount", fallback_stages)),
                "layerCount": int(metadata.get("layerCount", 0)),
                "layerRange": {
                    "start": int(layer_range.get("start", 0)),
                    "endExclusive": int(layer_range.get("endExclusive", 0)),
                },
            }
        raise RuntimeError("Qwen runtime requires a stage artifact path")
    import torch

    package = torch.load(Path(model_path), map_location="cpu", weights_only=True)
    spec = dict(package.get("spec", {}))
    if spec.get("runtime") != QWEN_TRANSFORMERS_RUNTIME:
        raise ValueError(f"unexpected Qwen stage runtime: {spec.get('runtime')}")
    return spec


def resolve_qwen_execution_device(device: str = "cpu", *,
                                  require_cuda: bool = False) -> Any:
    """Resolve an explicit Qwen stage device and fail closed when required."""
    import torch

    requested = str(device or "cpu").strip().lower()
    if requested == "auto":
        requested = "cuda:0" if torch.cuda.is_available() else "cpu"
    resolved = torch.device(requested)
    if resolved.type == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("QWEN_STAGE_CUDA_UNAVAILABLE")
    if require_cuda and resolved.type != "cuda":
        raise RuntimeError("QWEN_STAGE_CPU_FALLBACK_FORBIDDEN")
    return resolved


def _normalize_qwen_stage_config(
    config_dict: dict[str, Any],
) -> tuple[str, dict[str, Any]]:
    """Return the runtime family and text-only config stored in a stage."""
    config_copy = dict(config_dict)
    model_type = str(config_copy.get("model_type", ""))
    if not model_type:
        raise ValueError("Qwen stage package config is missing model_type")
    if model_type == "qwen3_5":
        text_config = config_copy.get("text_config")
        if not isinstance(text_config, dict):
            raise ValueError("qwen3_5 stage package config is missing text_config")
        normalized = dict(text_config)
        normalized.setdefault("model_type", "qwen3_5_text")
        return model_type, normalized
    if model_type == "qwen3_5_text":
        return "qwen3_5", config_copy
    return model_type, config_copy


def _require_qwen_transformers_runtime(
    model_type: str,
    *,
    installed_version: str | None = None,
) -> None:
    """Reject runtimes that cannot construct the frozen Qwen3.6 architecture."""
    if model_type != "qwen3_5":
        return
    if installed_version is None:
        from importlib.metadata import version

        installed_version = version("transformers")
    from packaging.version import Version

    if Version(installed_version) < Version("5.14.1"):
        raise RuntimeError(
            "QWEN3_5_TRANSFORMERS_TOO_OLD: "
            f"need transformers>=5.14.1, got {installed_version}"
        )


def probe_qwen_transformers_model_type(model_type: str) -> None:
    """Prove the configured runtime can construct a model family before ACK."""

    from importlib import import_module

    modules = {
        "qwen2": (
            "transformers.models.qwen2.configuration_qwen2",
            "transformers.models.qwen2.modeling_qwen2",
        ),
        "qwen3": (
            "transformers.models.qwen3.configuration_qwen3",
            "transformers.models.qwen3.modeling_qwen3",
        ),
        "qwen3_5": (
            "transformers.models.qwen3_5.configuration_qwen3_5",
            "transformers.models.qwen3_5.modeling_qwen3_5",
        ),
    }
    model_type = str(model_type or "")
    if model_type not in modules:
        raise RuntimeError(
            f"QWEN_TRANSFORMERS_MODEL_TYPE_UNSUPPORTED:{model_type}")
    _require_qwen_transformers_runtime(model_type)
    try:
        for module_name in modules[model_type]:
            import_module(module_name)
    except (ImportError, ModuleNotFoundError) as exc:
        raise RuntimeError(
            "QWEN_TRANSFORMERS_MODEL_TYPE_UNAVAILABLE:"
            f"{model_type}:{type(exc).__name__}:{exc}"
        ) from exc


def _remap_qwen_stage_state_dict(
    state_dict: dict[str, Any],
    *,
    start: int,
    local_layer_count: int,
    include_embedding: bool,
    include_final: bool,
) -> dict[str, Any]:
    """Map full-model Qwen2/Qwen3.6 keys into a stage-local strict state dict."""
    remapped: dict[str, Any] = {}
    embed_prefixes = (
        "model.embed_tokens.",
        "model.language_model.embed_tokens.",
    )
    norm_prefixes = (
        "model.norm.",
        "model.language_model.norm.",
    )
    layer_prefixes = (
        "model.layers.",
        "model.language_model.layers.",
    )
    for key, value in state_dict.items():
        embed_prefix = next(
            (prefix for prefix in embed_prefixes if key.startswith(prefix)),
            None,
        )
        if embed_prefix is not None:
            if include_embedding:
                remapped[f"model.embed_tokens.{key[len(embed_prefix):]}"] = value
            continue
        norm_prefix = next(
            (prefix for prefix in norm_prefixes if key.startswith(prefix)),
            None,
        )
        if norm_prefix is not None:
            if include_final:
                remapped[f"model.norm.{key[len(norm_prefix):]}"] = value
            continue
        if key.startswith("lm_head."):
            if include_final:
                remapped[key] = value
            continue
        layer_prefix = next(
            (prefix for prefix in layer_prefixes if key.startswith(prefix)),
            None,
        )
        if layer_prefix is None:
            continue
        rest = key[len(layer_prefix):]
        layer_text, suffix = rest.split(".", 1)
        local_index = int(layer_text) - start
        if 0 <= local_index < local_layer_count:
            remapped[f"model.layers.{local_index}.{suffix}"] = value
    return remapped


def format_qwen_chat_prompt(
    tokenizer: Any,
    prompt: str,
    *,
    enable_thinking: bool = False,
) -> Any:
    """Tokenize a text-only Qwen chat prompt with thinking mode explicit."""
    return tokenizer.apply_chat_template(
        [{"role": "user", "content": str(prompt)}],
        tokenize=True,
        return_dict=False,
        add_generation_prompt=True,
        enable_thinking=bool(enable_thinking),
    )


def qwen_transformer_model_from_stage_package(
    model_path: str | Path,
    *,
    device: str = "cpu",
    require_cuda: bool = False,
) -> Any:
    import torch
    from torch import nn

    # Stage packages are loaded concurrently by the three deployment-faithful
    # Providers.  A normal ``torch.load`` followed by a normally initialized
    # model briefly keeps both the serialized state dict and the model's
    # freshly allocated parameters alive.  On the fixed 8-GiB Gate-B host that
    # peak is enough to push the final stage into swap before it can publish
    # ``QWEN_RUNTIME_READY``.  Local stage artifacts are immutable regular
    # files, so use PyTorch's file-backed storage when available.  Older
    # PyTorch releases do not accept ``mmap`` and retain the previous path.
    try:
        package = torch.load(
            Path(model_path),
            map_location="cpu",
            weights_only=True,
            mmap=True,
        )
    except (TypeError, RuntimeError):
        package = torch.load(Path(model_path), map_location="cpu", weights_only=True)
    spec = dict(package.get("spec", {}))
    model_type, config_dict = _normalize_qwen_stage_config(
        dict(package.get("config", {})))
    _require_qwen_transformers_runtime(model_type)
    if model_type == "qwen2":
        from transformers import AutoConfig
        from transformers.models.qwen2.modeling_qwen2 import (
            Qwen2DecoderLayer as DecoderLayer,
            Qwen2RMSNorm as RMSNorm,
            Qwen2RotaryEmbedding as RotaryEmbedding,
        )

        config_payload = dict(config_dict)
        config_payload.pop("model_type", None)
        config = AutoConfig.for_model("qwen2", **config_payload)
    elif model_type == "qwen3":
        from transformers.models.qwen3.configuration_qwen3 import Qwen3Config
        from transformers.models.qwen3.modeling_qwen3 import (
            Qwen3DecoderLayer as DecoderLayer,
            Qwen3RMSNorm as RMSNorm,
            Qwen3RotaryEmbedding as RotaryEmbedding,
        )

        config = Qwen3Config.from_dict(config_dict)
    elif model_type == "qwen3_5":
        from transformers.models.qwen3_5.configuration_qwen3_5 import (
            Qwen3_5TextConfig,
        )
        from transformers.models.qwen3_5.modeling_qwen3_5 import (
            Qwen3_5DecoderLayer as DecoderLayer,
            Qwen3_5RMSNorm as RMSNorm,
            Qwen3_5TextRotaryEmbedding as RotaryEmbedding,
        )

        config = Qwen3_5TextConfig.from_dict(config_dict)
    else:
        raise ValueError(
            "lightweight Qwen stage only supports qwen2, qwen3, or "
            f"qwen3_5, got {model_type}"
        )
    attn_impl = package.get("attnImplementation") or "sdpa"
    try:
        config._attn_implementation = attn_impl
    except Exception:
        pass
    stage_index = int(spec["stageIndex"])
    stage_count = int(spec["stageCount"])
    start = int(spec["layerRange"]["start"])
    end = int(spec["layerRange"]["endExclusive"])

    class _StageBackbone(nn.Module):
        def __init__(self):
            super().__init__()
            self.config = config
            if stage_index == 0:
                self.embed_tokens = nn.Embedding(
                    config.vocab_size,
                    config.hidden_size,
                    getattr(config, "pad_token_id", None),
                )
            self.layers = nn.ModuleList([
                DecoderLayer(config, layer_idx=layer_idx)
                for layer_idx in range(start, end)
            ])
            self.rotary_emb = RotaryEmbedding(config=config)
            if stage_index == stage_count - 1:
                self.norm = RMSNorm(config.hidden_size, eps=config.rms_norm_eps)

    class _StageCausalLM(nn.Module):
        def __init__(self):
            super().__init__()
            self.config = config
            self.model = _StageBackbone()
            if stage_index == stage_count - 1:
                self.lm_head = nn.Linear(config.hidden_size, config.vocab_size, bias=False)
            self.ndnsf_stage_index = stage_index
            self.ndnsf_stage_count = stage_count
            self.ndnsf_stage_start = start
            self.ndnsf_stage_end = end
            self.ndnsf_layer_count = int(spec["layerCount"])
            self.ndnsf_model_type = model_type

    remapped = _remap_qwen_stage_state_dict(
        dict(package.get("state_dict", {})),
        start=start,
        local_layer_count=end - start,
        include_embedding=stage_index == 0,
        include_final=stage_index == stage_count - 1,
    )
    floating_dtypes = {
        value.dtype
        for value in remapped.values()
        if getattr(value, "is_floating_point", lambda: False)()
    }
    if len(floating_dtypes) != 1:
        raise ValueError(
            "Qwen stage package must use exactly one floating-point dtype, "
            f"got {sorted(map(str, floating_dtypes))}")
    stage_dtype = next(iter(floating_dtypes))

    # Construct on the meta device when supported.  ``assign=True`` then
    # attaches the already-loaded tensors instead of copying them into a
    # second full parameter allocation.  Keep a compatibility fallback for
    # PyTorch versions without meta/assign support and for unusual custom
    # modules that reject the low-memory load contract.
    try:
        with torch.device("meta"):
            model = _StageCausalLM()
        model.to(dtype=stage_dtype)
        model.load_state_dict(remapped, strict=True, assign=True)
        if any(
            tensor.is_meta
            for tensor in tuple(model.parameters()) + tuple(model.buffers())
        ):
            raise RuntimeError(
                "meta Qwen stage load left an unassigned parameter or buffer")
    except (TypeError, RuntimeError, NotImplementedError):
        model = _StageCausalLM()
        model.to(dtype=stage_dtype)
        model.load_state_dict(remapped, strict=True)
    # The model now owns the state tensors (or copied them in the compatibility
    # path); release the package graph before moving to CUDA or warming so the
    # serialized state dict cannot keep a second copy resident.
    del remapped
    del package
    execution_device = resolve_qwen_execution_device(
        device, require_cuda=require_cuda)
    model.to(execution_device)
    model.eval()
    model.ndnsf_execution_device = str(execution_device)
    model.ndnsf_cpu_fallback = (
        str(device or "cpu").strip().lower() == "auto"
        and execution_device.type != "cuda"
    )
    return model


def _prompt_input_ids(prompt_payload: bytes):
    import torch

    try:
        doc = decode_payload(prompt_payload)
        if "inputIds" in doc:
            return torch.tensor(doc["inputIds"], dtype=torch.long)
        prompt = str(doc.get("prompt", ""))
    except Exception:
        prompt = prompt_payload.decode("utf-8", errors="replace")
    token_values = [1]
    token_values.extend(((ord(ch) % 240) + 3) for ch in prompt[:14])
    token_values.append(2)
    return torch.tensor([token_values], dtype=torch.long)


def _position_ids_like(input_ids: Any) -> Any:
    return _qwen_position_ids(input_ids, "qwen2")


def _qwen_position_ids(input_ids: Any, model_type: str) -> Any:
    import torch

    positions = torch.arange(
        int(input_ids.shape[1]),
        device=input_ids.device,
        dtype=torch.long,
    ).unsqueeze(0)
    if model_type == "qwen3_5":
        return positions.view(1, 1, -1).expand(
            4, int(input_ids.shape[0]), -1)
    return positions


def _qwen_layer_position_inputs(
    position_ids: Any,
    model_type: str,
) -> tuple[Any, Any]:
    if model_type == "qwen3_5":
        if position_ids.ndim != 3 or int(position_ids.shape[0]) != 4:
            raise ValueError("qwen3_5 position_ids must have shape [4,batch,seq]")
        return position_ids[0], position_ids[1:]
    return position_ids, position_ids


def _build_causal_mask(base_model: Any, input_ids: Any, hidden_states: Any) -> Any:
    position_ids = _position_ids_like(input_ids)
    updater = getattr(base_model, "_update_causal_mask", None)
    if updater is None:
        return None
    try:
        return _call_with_supported_kwargs(
            updater,
            attention_mask=None,
            input_tensor=hidden_states,
            inputs_embeds=hidden_states,
            cache_position=position_ids[0],
            past_key_values=None,
            output_attentions=False,
        )
    except Exception:
        return None


def _build_qwen_attention_masks(
    base_model: Any,
    input_ids: Any,
    hidden_states: Any,
    position_ids: Any,
    model_type: str,
) -> Any:
    if model_type == "qwen3":
        # Qwen3Model.forward constructs an explicit mask through
        # create_causal_mask even when the caller supplies an all-one
        # attention mask.  Passing None here silently selects SDPA's inline
        # is_causal path, which is mathematically equivalent but can produce
        # different bfloat16 reductions for near-tied vocabulary logits.
        from transformers.masking_utils import create_causal_mask

        import torch

        return create_causal_mask(
            config=base_model.config,
            inputs_embeds=hidden_states,
            attention_mask=torch.ones_like(input_ids),
            past_key_values=None,
            position_ids=position_ids,
        )
    if model_type != "qwen3_5":
        return _build_causal_mask(base_model, input_ids, hidden_states)
    from transformers.masking_utils import (
        create_causal_mask,
        create_recurrent_attention_mask,
    )

    text_position_ids, _ = _qwen_layer_position_inputs(
        position_ids, model_type)
    mask_kwargs = {
        "config": base_model.config,
        "inputs_embeds": hidden_states,
        "attention_mask": None,
        "past_key_values": None,
        "position_ids": text_position_ids,
    }
    return {
        "full_attention": create_causal_mask(**mask_kwargs),
        "linear_attention": create_recurrent_attention_mask(**mask_kwargs),
    }


def _qwen_attention_mask_for_layer(attention_masks: Any, layer: Any) -> Any:
    if not isinstance(attention_masks, dict):
        return attention_masks
    block_type = getattr(layer, "block_type", None)
    if block_type is None:
        block_type = getattr(layer, "layer_type", None)
    if block_type not in attention_masks:
        raise ValueError(f"missing attention mask for layer type {block_type!r}")
    return attention_masks[block_type]


def _rotary_embeddings(base_model: Any, hidden_states: Any, position_ids: Any) -> Any:
    rotary = getattr(base_model, "rotary_emb", None)
    if rotary is None:
        return None
    try:
        return rotary(hidden_states, position_ids)
    except TypeError:
        return None


def _run_transformer_layer(layer: Any, hidden_states: Any, *,
                           position_ids: Any,
                           position_embeddings: Any,
                           attention_mask: Any = None) -> Any:
    output = _call_with_supported_kwargs(
        layer.forward,
        hidden_states=hidden_states,
        attention_mask=attention_mask,
        position_ids=position_ids,
        past_key_value=None,
        past_key_values=None,
        output_attentions=False,
        use_cache=False,
        cache_position=position_ids[0],
        position_embeddings=position_embeddings,
    )
    if isinstance(output, tuple):
        return output[0]
    return output


def _encode_hidden_state(*, hidden_states: Any, input_ids: Any, next_layer: int,
                         stage_index: int, ranges: list[tuple[int, int]],
                         request_id: str = "") -> bytes:
    import torch

    buffer = BytesIO()
    torch.save({
        "schema": "ndnsf-di-llm-transformer-hidden-v1",
        "hidden_states": hidden_states.cpu(),
        "input_ids": input_ids.cpu(),
        "next_layer": int(next_layer),
        "stage_index": int(stage_index),
        "layer_ranges": list(ranges),
        "request_id": request_id,
    }, buffer)
    return buffer.getvalue()


def _decode_hidden_state(payload: bytes) -> dict[str, Any]:
    import torch

    try:
        return torch.load(BytesIO(payload), map_location="cpu", weights_only=True)
    except TypeError:
        return torch.load(BytesIO(payload), map_location="cpu")


def run_tiny_transformer_stage(
    input_payload: bytes,
    *,
    role: str,
    stages: int,
    layer_count: int,
    compute_delay_ms: float = 0.0,
    model: Any = None,
) -> bytes:
    """Run one deterministic tiny Llama stage and return hidden/final bytes.

    The tiny model is intentionally reconstructed from the same seed by every
    provider. Real deployment will materialize stage-specific model artifacts;
    this smoke focuses on the DI stage/dependency dataflow first.
    """

    import torch

    if compute_delay_ms > 0:
        time.sleep(compute_delay_ms / 1000.0)
    stage_index = role_index(role)
    ranges = split_layer_ranges(layer_count, stages)
    if stage_index >= len(ranges):
        raise ValueError(f"stage_index {stage_index} outside ranges {ranges}")

    model = model if model is not None else create_tiny_transformer_model(layer_count)
    base = model.model
    try:
        execution_device = next(model.parameters()).device
    except StopIteration:
        execution_device = torch.device("cpu")
    layers = list(base.layers)
    start, end = ranges[stage_index]

    with torch.no_grad():
        if stage_index == 0:
            input_ids = _prompt_input_ids(input_payload).to(execution_device)
            hidden_states = base.embed_tokens(input_ids)
            expected_start = 0
        else:
            incoming = _decode_hidden_state(input_payload)
            if incoming.get("schema") != "ndnsf-di-llm-transformer-hidden-v1":
                raise ValueError("unexpected hidden-state payload schema")
            input_ids = incoming["input_ids"].to(execution_device)
            hidden_states = incoming["hidden_states"].to(execution_device)
            expected_start = int(incoming.get("next_layer", -1))
        if expected_start != start:
            raise ValueError(
                f"stage {stage_index} expected layer {start}, got {expected_start}")
        position_ids = _position_ids_like(input_ids)
        attention_mask = _build_causal_mask(base, input_ids, hidden_states)
        for layer in layers[start:end]:
            hidden_states = _run_transformer_layer(
                layer,
                hidden_states,
                position_ids=position_ids,
                position_embeddings=_rotary_embeddings(base, hidden_states, position_ids),
                attention_mask=attention_mask,
            )
        if stage_index < stages - 1:
            return _encode_hidden_state(
                hidden_states=hidden_states,
                input_ids=input_ids,
                next_layer=end,
                stage_index=stage_index,
                ranges=ranges,
            )
        logits = model.lm_head(base.norm(hidden_states))
        top_token = int(torch.argmax(logits[:, -1, :], dim=-1).item())
        return json.dumps({
            "schema": "ndnsf-di-llm-transformer-response-v1",
            "runtime": "tiny-transformers",
            "finalRole": role,
            "stageCount": stages,
            "layerCount": layer_count,
            "layerRanges": [list(item) for item in ranges],
            "topToken": top_token,
            "logitsShape": list(logits.shape),
        }, sort_keys=True).encode("utf-8")


def warm_qwen_transformer_stage(
    model: Any,
    assigned_device: str | None = None,
) -> bool:
    """Execute one real token on the stage's committed execution device."""

    import torch

    base = model.model
    try:
        parameter = next(model.parameters())
    except StopIteration as exc:
        raise RuntimeError("Qwen stage has no model parameters") from exc
    device = parameter.device
    expected_device = torch.device(
        assigned_device
        if assigned_device is not None
        else getattr(model, "ndnsf_execution_device", str(device))
    )
    if device != expected_device:
        raise RuntimeError(
            "Qwen stage warmup device does not match committed assignment: "
            f"loaded={device} assigned={expected_device}"
        )
    input_ids = torch.zeros((1, 1), dtype=torch.long, device=device)
    stage_index = int(getattr(model, "ndnsf_stage_index", -1))
    stage_count = int(getattr(model, "ndnsf_stage_count", 0))
    if stage_index < 0 or stage_count <= 0:
        raise RuntimeError("Qwen stage warmup lacks stage identity")
    with torch.no_grad():
        if stage_index == 0:
            hidden_states = base.embed_tokens(input_ids)
        else:
            hidden_states = torch.zeros(
                (1, 1, int(model.config.hidden_size)),
                dtype=parameter.dtype,
                device=device,
            )
        position_ids = _position_ids_like(input_ids)
        attention_mask = _build_causal_mask(base, input_ids, hidden_states)
        for layer in list(base.layers):
            hidden_states = _run_transformer_layer(
                layer,
                hidden_states,
                position_ids=position_ids,
                position_embeddings=_rotary_embeddings(
                    base, hidden_states, position_ids),
                attention_mask=attention_mask,
            )
        if stage_index == stage_count - 1:
            model.lm_head(base.norm(hidden_states))
    if device.type == "cuda":
        torch.cuda.synchronize(device)
    return True


def run_qwen_transformer_stage(
    input_payload: bytes,
    *,
    role: str,
    stages: int,
    model: Any,
    compute_delay_ms: float = 0.0,
    timing: dict[str, float | int | str] | None = None,
) -> bytes:
    import torch

    def record(name: str, value: float | int | str) -> None:
        if timing is not None:
            timing[name] = value

    total_start = time.perf_counter()
    if compute_delay_ms > 0:
        sleep_start = time.perf_counter()
        time.sleep(compute_delay_ms / 1000.0)
        record("artificial_delay_ms", (time.perf_counter() - sleep_start) * 1000.0)
    else:
        record("artificial_delay_ms", 0.0)
    stage_index = role_index(role)
    base = model.model
    try:
        execution_device = next(model.parameters()).device
    except StopIteration:
        execution_device = torch.device("cpu")
    record("device", str(execution_device))
    record("cpu_fallback", int(execution_device.type != "cuda"))
    layer_count = int(getattr(model, "ndnsf_layer_count", len(list(base.layers))))
    ranges = split_layer_ranges(layer_count, stages)
    if stage_index >= len(ranges):
        raise ValueError(f"stage_index {stage_index} outside ranges {ranges}")
    start = int(getattr(model, "ndnsf_stage_start", ranges[stage_index][0]))
    end = int(getattr(model, "ndnsf_stage_end", ranges[stage_index][1]))
    if (start, end) != tuple(ranges[stage_index]):
        raise ValueError(
            f"stage {stage_index} package range {(start, end)} does not match plan {ranges[stage_index]}")
    layers_to_run = list(base.layers)
    if not hasattr(model, "ndnsf_stage_start"):
        layers_to_run = layers_to_run[start:end]
    with torch.no_grad():
        if stage_index == 0:
            decode_start = time.perf_counter()
            input_ids = _prompt_input_ids(input_payload).to(execution_device)
            try:
                input_doc = decode_payload(input_payload)
                request_id = str(input_doc.get("requestId", ""))
            except Exception:
                request_id = ""
            record("decode_ms", (time.perf_counter() - decode_start) * 1000.0)
            embed_start = time.perf_counter()
            hidden_states = base.embed_tokens(input_ids)
            record("embed_ms", (time.perf_counter() - embed_start) * 1000.0)
            expected_start = 0
        else:
            decode_start = time.perf_counter()
            incoming = _decode_hidden_state(input_payload)
            if incoming.get("schema") != "ndnsf-di-llm-transformer-hidden-v1":
                raise ValueError("unexpected hidden-state payload schema")
            input_ids = incoming["input_ids"].to(execution_device)
            hidden_states = incoming["hidden_states"].to(execution_device)
            expected_start = int(incoming.get("next_layer", -1))
            request_id = str(incoming.get("request_id", ""))
            record("decode_ms", (time.perf_counter() - decode_start) * 1000.0)
            record("embed_ms", 0.0)
        if expected_start != start:
            raise ValueError(
                f"stage {stage_index} expected layer {start}, got {expected_start}")
        record("request_id", request_id)
        model_type = str(getattr(model, "ndnsf_model_type", "qwen2"))
        position_ids = _qwen_position_ids(input_ids, model_type)
        layer_position_ids, rotary_position_ids = (
            _qwen_layer_position_inputs(position_ids, model_type)
        )
        mask_start = time.perf_counter()
        attention_masks = _build_qwen_attention_masks(
            base,
            input_ids,
            hidden_states,
            position_ids,
            model_type,
        )
        position_embeddings = _rotary_embeddings(
            base, hidden_states, rotary_position_ids)
        record("mask_ms", (time.perf_counter() - mask_start) * 1000.0)
        layers_start = time.perf_counter()
        for layer in layers_to_run:
            hidden_states = _run_transformer_layer(
                layer,
                hidden_states,
                position_ids=layer_position_ids,
                position_embeddings=position_embeddings,
                attention_mask=_qwen_attention_mask_for_layer(
                    attention_masks, layer),
            )
        record("layers_ms", (time.perf_counter() - layers_start) * 1000.0)
        if stage_index < stages - 1:
            encode_start = time.perf_counter()
            payload = _encode_hidden_state(
                hidden_states=hidden_states,
                input_ids=input_ids,
                next_layer=end,
                stage_index=stage_index,
                ranges=ranges,
                request_id=request_id,
            )
            record("encode_ms", (time.perf_counter() - encode_start) * 1000.0)
            record("final_head_ms", 0.0)
            record("total_ms", (time.perf_counter() - total_start) * 1000.0)
            return payload
        head_start = time.perf_counter()
        logits = model.lm_head(base.norm(hidden_states))
        top_token = int(torch.argmax(logits[:, -1, :], dim=-1).item())
        record("final_head_ms", (time.perf_counter() - head_start) * 1000.0)
        encode_start = time.perf_counter()
        payload = json.dumps({
            "schema": "ndnsf-di-qwen-transformer-response-v1",
            "runtime": QWEN_TRANSFORMERS_RUNTIME,
            "finalRole": role,
            "stageCount": stages,
            "layerCount": layer_count,
            "layerRanges": [list(item) for item in ranges],
            "topToken": top_token,
            "logitsShape": list(logits.shape),
        }, sort_keys=True).encode("utf-8")
        record("encode_ms", (time.perf_counter() - encode_start) * 1000.0)
        record("total_ms", (time.perf_counter() - total_start) * 1000.0)
        return payload


def run_qwen_onnx_stage(
    input_payload: bytes,
    *,
    role: str,
    stages: int,
    session: Any,
    metadata: dict[str, Any],
    compute_delay_ms: float = 0.0,
    timing: dict[str, float | int | str] | None = None,
) -> bytes:
    import numpy as np

    def record(name: str, value: float | int | str) -> None:
        if timing is not None:
            timing[name] = value

    total_start = time.perf_counter()
    if compute_delay_ms > 0:
        sleep_start = time.perf_counter()
        time.sleep(compute_delay_ms / 1000.0)
        record("artificial_delay_ms", (time.perf_counter() - sleep_start) * 1000.0)
    else:
        record("artificial_delay_ms", 0.0)
    stage_index = int(metadata.get("stageIndex", role_index(role)))
    layer_range = dict(metadata.get("layerRange", {}) or {})
    start = int(layer_range.get("start", 0))
    end = int(layer_range.get("endExclusive", 0))
    stage_count = int(metadata.get("stageCount", stages))
    hidden_size = 0
    request_id = ""

    decode_start = time.perf_counter()
    if stage_index == 0:
        try:
            input_doc = decode_qwen_pipeline_context(input_payload)
            input_ids = np.asarray(input_doc["inputIds"], dtype=np.int64)
            attention_mask = np.asarray(input_doc["attentionMask"], dtype=np.int64)
            position_ids = np.asarray(input_doc["positionIds"], dtype=np.int64)
            request_id = str(input_doc.get("requestId", ""))
            session_id = str(input_doc.get("sessionId", ""))
            context_epoch = int(input_doc.get("contextEpoch", 0) or 0)
        except Exception:
            try:
                input_doc = decode_payload(input_payload)
                token_values = [1]
                token_values.extend(((ord(ch) % 240) + 3)
                                    for ch in str(input_doc.get("prompt", ""))[:14])
                token_values.append(2)
                input_ids = np.asarray([token_values], dtype=np.int64)
                request_id = str(input_doc.get("requestId", ""))
            except Exception:
                token_values = [1]
                token_values.extend(((byte % 240) + 3) for byte in input_payload[:14])
                token_values.append(2)
                input_ids = np.asarray([token_values], dtype=np.int64)
                request_id = ""
            attention_mask = np.ones_like(input_ids, dtype=np.int64)
            position_ids = np.arange(input_ids.shape[1], dtype=np.int64).reshape(1, -1)
            session_id = ""
            context_epoch = 0
        hidden_size = int(metadata.get("hiddenSize", 0) or 0)
        if hidden_size <= 0:
            hidden_shape = session.get_inputs()[1].shape
            hidden_size = int(hidden_shape[2]) if len(hidden_shape) >= 3 and isinstance(hidden_shape[2], int) else 896
        hidden_states = np.zeros(
            (int(input_ids.shape[0]), int(input_ids.shape[1]), hidden_size),
            dtype=np.float32,
        )
        expected_start = 0
    else:
        incoming, transport = _load_qwen_hidden_payload(input_payload)
        record("tensor_transport", transport)
        input_ids = incoming["input_ids"].astype(np.int64)
        attention_mask = incoming.get(
            "attention_mask",
            np.ones_like(input_ids, dtype=np.int64),
        ).astype(np.int64)
        position_ids = incoming.get(
            "position_ids",
            np.arange(input_ids.shape[1], dtype=np.int64).reshape(1, -1),
        ).astype(np.int64)
        hidden_states = incoming["hidden_states"].astype(np.float32)
        expected_start = int(incoming["next_layer"].item())
        request_id = _safe_array_text(incoming.get("request_id", ""))
        session_id = _safe_array_text(incoming.get("session_id", ""))
        context_epoch = int(incoming.get("context_epoch", np.asarray([0])).reshape(-1)[0])
    record("decode_ms", (time.perf_counter() - decode_start) * 1000.0)
    record("embed_ms", 0.0)
    if expected_start != start:
        raise ValueError(
            f"stage {stage_index} expected layer {start}, got {expected_start}")
    record("request_id", request_id)
    run_start = time.perf_counter()
    available_inputs = {item.name for item in session.get_inputs()}
    feed = {}
    if "input_ids" in available_inputs:
        feed["input_ids"] = input_ids
    if "hidden_states" in available_inputs:
        feed["hidden_states"] = hidden_states
    if "position_ids" in available_inputs:
        feed["position_ids"] = position_ids
    if "attention_mask" in available_inputs:
        feed["attention_mask"] = attention_mask
    for item in session.get_inputs():
        if not item.name.startswith(("past_key.", "past_value.")):
            continue
        shape = item.shape
        if len(shape) != 4 or not isinstance(shape[1], int) or not isinstance(shape[3], int):
            raise ValueError(f"unsupported Qwen KV input shape for {item.name}: {shape}")
        feed[item.name] = np.empty(
            (int(input_ids.shape[0]), int(shape[1]), 0, int(shape[3])),
            dtype=np.float32,
        )
    output_names = tuple(item.name for item in session.get_outputs())
    output_values = session.run(None, feed)
    if len(output_names) != len(output_values):
        raise ValueError(
            "Qwen ONNX output metadata/value count mismatch: "
            f"{len(output_names)} != {len(output_values)}")
    outputs = dict(zip(output_names, output_values))
    primary_name = (
        "logits" if stage_index == stage_count - 1 else "hidden_states_out")
    if primary_name not in outputs:
        raise ValueError(
            f"Qwen ONNX stage output is missing required {primary_name!r}")
    record("layers_ms", (time.perf_counter() - run_start) * 1000.0)
    record("mask_ms", 0.0)
    if stage_index < stage_count - 1:
        encode_start = time.perf_counter()
        payload = _native_tensor_bundle_payload({
            "hidden_states": np.asarray(outputs[primary_name], dtype=np.float32),
            "input_ids": input_ids,
            "attention_mask": attention_mask,
            "position_ids": position_ids,
            "next_layer": np.asarray([end], dtype=np.int64),
            "stage_index": np.asarray([stage_index], dtype=np.int64),
            "context_epoch": np.asarray([context_epoch], dtype=np.int64),
            "request_id": _utf8_text_tensor(request_id),
            "session_id": _utf8_text_tensor(session_id),
        })
        record("encode_ms", (time.perf_counter() - encode_start) * 1000.0)
        record("final_head_ms", 0.0)
        record("total_ms", (time.perf_counter() - total_start) * 1000.0)
        return payload
    logits = np.asarray(outputs[primary_name])
    top_token = int(np.argmax(logits[:, -1, :], axis=-1)[0])
    record("final_head_ms", 0.0)
    encode_start = time.perf_counter()
    payload = json.dumps({
        "schema": "ndnsf-di-qwen-onnx-response-v1",
        "runtime": QWEN_ONNX_RUNTIME,
        "finalRole": role,
        "stageCount": stage_count,
        "layerRangeEnd": end,
        "topToken": top_token,
        "logitsShape": list(logits.shape),
    }, sort_keys=True).encode("utf-8")
    record("encode_ms", (time.perf_counter() - encode_start) * 1000.0)
    record("total_ms", (time.perf_counter() - total_start) * 1000.0)
    return payload


def run_local_tiny_transformer_pipeline(
    prompt_payload: bytes,
    *,
    stages: int,
    layer_count: int,
    compute_delay_ms: float = 0.0,
) -> LocalPipelineResult:
    model = create_tiny_transformer_model(layer_count)
    started = time.perf_counter()
    payload = prompt_payload
    for index in range(stages):
        payload = run_tiny_transformer_stage(
            payload,
            role=role_name(index),
            stages=stages,
            layer_count=layer_count,
            compute_delay_ms=compute_delay_ms,
            model=model,
        )
    return LocalPipelineResult(
        payload=payload,
        elapsed_ms=(time.perf_counter() - started) * 1000.0,
    )


def run_local_tiny_transformer_artifact_pipeline(
    prompt_payload: bytes,
    *,
    stages: int,
    layer_count: int,
    artifact_dir: str | Path,
    compute_delay_ms: float = 0.0,
) -> LocalPipelineResult:
    """Run the tiny transformer pipeline from per-stage artifact packages.

    This mirrors the MiniNDN provider path without starting MiniNDN: the planner
    exports one stage-weight package per role, each stage loads only its package,
    and the hidden-state bytes flow through the same stage runner.
    """

    roles = [role_name(index) for index in range(stages)]
    artifacts = write_tiny_transformer_stage_artifacts(
        artifact_dir,
        roles=roles,
        stages=stages,
        layer_count=layer_count,
    )
    models = {
        artifact.role: tiny_transformer_model_from_stage_package(
            artifact.path,
            fallback_layer_count=layer_count,
        )
        for artifact in artifacts
    }
    started = time.perf_counter()
    payload = prompt_payload
    for index, role in enumerate(roles):
        payload = run_tiny_transformer_stage(
            payload,
            role=role,
            stages=stages,
            layer_count=layer_count,
            compute_delay_ms=compute_delay_ms,
            model=models[role],
        )
    return LocalPipelineResult(
        payload=payload,
        elapsed_ms=(time.perf_counter() - started) * 1000.0,
    )


def encode_stage_payload(
    *,
    role: str,
    stage_index: int,
    input_payload: bytes,
    compute_delay_ms: float,
) -> bytes:
    if compute_delay_ms > 0:
        time.sleep(compute_delay_ms / 1000.0)
    incoming = decode_payload(input_payload)
    lineage = list(incoming.get("lineage", []))
    if not lineage:
        lineage = ["prompt"]
    lineage.append(role)
    return json.dumps({
        "schema": "ndnsf-di-llm-hidden-state-v1",
        "stageIndex": stage_index,
        "role": role,
        "lineage": lineage,
        "inputBytes": len(input_payload),
        "promptDigest": incoming.get("promptDigest") or _prompt_digest(incoming),
    }, sort_keys=True).encode("utf-8")


def encode_final_response(
    *,
    role: str,
    stage_index: int,
    input_payload: bytes,
    compute_delay_ms: float,
) -> bytes:
    hidden = decode_payload(encode_stage_payload(
        role=role,
        stage_index=stage_index,
        input_payload=input_payload,
        compute_delay_ms=compute_delay_ms,
    ))
    return json.dumps({
        "schema": "ndnsf-di-llm-pipeline-response-v1",
        "finalRole": role,
        "stageCount": stage_index + 1,
        "lineage": hidden["lineage"],
        "text": "fake distributed LLM response",
        "promptDigest": hidden["promptDigest"],
    }, sort_keys=True).encode("utf-8")


def _prompt_digest(doc: dict[str, Any]) -> str:
    import hashlib

    prompt = str(doc.get("prompt", ""))
    return hashlib.sha256(prompt.encode("utf-8")).hexdigest()[:16]


@dataclass(frozen=True)
class LocalPipelineResult:
    payload: bytes
    elapsed_ms: float


def run_local_pipeline(
    prompt_payload: bytes,
    *,
    stages: int,
    compute_delay_ms: float = 1.0,
) -> LocalPipelineResult:
    started = time.perf_counter()
    payload = prompt_payload
    for index in range(stages):
        role = role_name(index)
        if index == stages - 1:
            payload = encode_final_response(
                role=role,
                stage_index=index,
                input_payload=payload,
                compute_delay_ms=compute_delay_ms,
            )
        else:
            payload = encode_stage_payload(
                role=role,
                stage_index=index,
                input_payload=payload,
                compute_delay_ms=compute_delay_ms,
            )
    return LocalPipelineResult(
        payload=payload,
        elapsed_ms=(time.perf_counter() - started) * 1000.0,
    )


def write_policy(
    path: str | Path,
    *,
    service: str = SERVICE,
    model: str = MODEL_NAME,
    stages: int = 3,
    layers: int = 24,
    controller: str = DEFAULT_CONTROLLER,
    group: str = DEFAULT_GROUP,
    user: str = DEFAULT_USER,
    provider_prefix: str = DEFAULT_PROVIDER_PREFIX,
    runtime: str = "fake",
    transformer_layers: int = 4,
    qwen_model: str = "Qwen/Qwen2.5-0.5B-Instruct",
    qwen_revision: str = "main",
    qwen_prompt: str = "",
    qwen_allow_download: bool = False,
    qwen_dtype: str = "float32",
    qwen_content_store: str = "",
    qwen_artifact_store: str = "",
    qwen_service_manifest: str = "",
    qwen_runtime_manifest: str = "",
) -> Path:
    output_dir = Path(path).parent
    request = llm_planner_request(
        planner_kind=PlannerKind.LLM_PIPELINE,
        model_path=model,
        output_dir=output_dir,
        model_format="custom",
        runtime_backend="custom",
        service=service,
        stages=stages,
        layers=layers,
    )
    result = llm_planner_registry().plan(request)
    splitter = llm_splitter_output_from_result(
        result,
        application="llm-pipeline-fake-demo",
        controller=controller,
        group=group,
        user=user,
        provider_prefix=provider_prefix,
    )
    if runtime == TINY_TRANSFORMERS_RUNTIME:
        splitter = with_tiny_transformer_artifacts(
            splitter,
            output_dir=output_dir,
            stages=stages,
            layer_count=transformer_layers,
            content_store=qwen_content_store,
        )
    elif runtime == QWEN_TRANSFORMERS_RUNTIME:
        splitter = with_qwen_transformer_artifacts(
            splitter,
            output_dir=output_dir,
            stages=stages,
            model_name=qwen_model,
            model_revision=qwen_revision,
            prompt=qwen_prompt,
            allow_download=qwen_allow_download,
            dtype=qwen_dtype,
            content_store=qwen_content_store,
        )
    elif runtime == QWEN_ONNX_RUNTIME:
        reuse_inputs = (
            qwen_artifact_store, qwen_service_manifest, qwen_runtime_manifest)
        if any(reuse_inputs) and not all(reuse_inputs):
            raise ValueError(
                "Qwen artifact reuse requires store, service manifest, and runtime manifest")
        if all(reuse_inputs):
            splitter = with_reused_qwen_onnx_artifacts(
                splitter,
                output_dir=output_dir,
                artifact_store=qwen_artifact_store,
                service_manifest_path=qwen_service_manifest,
                runtime_manifest_path=qwen_runtime_manifest,
            )
        else:
            splitter = with_qwen_onnx_artifacts(
                splitter,
                output_dir=output_dir,
                stages=stages,
                model_name=qwen_model,
                model_revision=qwen_revision,
                prompt=qwen_prompt,
                allow_download=qwen_allow_download,
                dtype=qwen_dtype,
            )
    policy = Path(path)
    splitter.write_policy_config(policy)
    _pin_stage_providers(
        policy,
        service=service,
        provider_prefix=provider_prefix,
        stages=stages,
    )
    return policy


def _pin_stage_providers(
    policy: Path,
    *,
    service: str,
    provider_prefix: str,
    stages: int,
) -> None:
    try:
        import yaml  # type: ignore
    except ImportError as exc:
        raise RuntimeError("LLM pipeline policy role pinning requires PyYAML") from exc
    config = yaml.safe_load(policy.read_text(encoding="utf-8"))
    providers = [
        {
            "identity": provider_prefix.rstrip("/") if index == 0
            else f"{provider_prefix.rstrip('/')}/{index}",
            "roles": [role_name(index)],
        }
        for index in range(stages)
    ]
    for service_entry in config.get("services", []):
        if service_entry.get("name") == service:
            service_entry["providers"] = providers
    policy.write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")


def parse_common_args(description: str) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--config", default="/tmp/ndnsf-di-llm-pipeline-policy.yaml")
    parser.add_argument("--generated-policy-dir", default="/tmp/ndnsf-di-llm-pipeline-generated")
    parser.add_argument("--group", default="")
    parser.add_argument("--dry-run", action="store_true")
    return parser
