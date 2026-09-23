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
TINY_ONNX_RUNTIME = "tiny-onnx"
QWEN_TRANSFORMERS_RUNTIME = "qwen-transformers"
QWEN_ONNX_RUNTIME = "qwen-onnx"
MAX_QWEN_GENERATED_TOKENS = 64


def _onnx_input_numpy_dtype(type_name: str, default):
    """Map an ORT tensor type without silently coercing BF16 to FP32."""
    import numpy as np

    normalized = str(type_name or "").lower()
    if "bfloat16" in normalized:
        raise RuntimeError(
            "QWEN_ONNX_BFLOAT16_UNSUPPORTED: export FP16 or provide an "
            "explicit packed-bfloat16 binding")
    if "float16" in normalized:
        return np.float16
    if "float" in normalized:
        return np.float32
    if "int64" in normalized:
        return np.int64
    if "int32" in normalized:
        return np.int32
    if "bool" in normalized:
        return np.bool_
    return default


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
    # Opaque native-owner checkpoint for a caller that will issue the next
    # APPEND_DELTA turn.  It is intentionally omitted from the public JSON
    # projection; the native coordinator remains the durable owner.
    native_conversation_checkpoint: bytes | None = None

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


def with_tiny_onnx_artifacts(
    splitter: SplitterOutput,
    *,
    fixture_root: str | Path,
    stages: int,
) -> SplitterOutput:
    """Bind the checked-in deterministic ONNX fixture to pipeline roles.

    The fixture is deliberately referenced read-only from the repository. It
    is small enough for the host/MiniNDN gate and exercises the same ORT
    session loading and stateful stage boundary as the later sealed runtime.
    No framework model package is imported by this path.
    """
    root = Path(fixture_root).expanduser().resolve()
    manifest_path = root / "manifest.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"tiny ONNX fixture manifest is missing: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    fixture_schema = str(manifest.get("schemaVersion", manifest.get("schema", "")))
    if fixture_schema != "spec175-tiny-causal-lm-v1":
        raise ValueError("unsupported Spec175 tiny ONNX fixture schema")
    partition_name = {1: "one-role", 2: "two-role", 4: "four-role"}.get(int(stages), "")
    partition = (manifest.get("partitions", {}) or {}).get(partition_name)
    if not isinstance(partition, list) or len(partition) != int(stages):
        raise ValueError("tiny ONNX fixture does not provide the requested stage count")
    by_role = {str(row["role"]): dict(row) for row in partition}
    services: list[SplitServiceSpec] = []
    for service in splitter.services:
        if service.name != SERVICE:
            services.append(service)
            continue
        artifacts: list[SplitArtifact] = []
        for role in service.roles:
            row = by_role.get(str(role))
            if row is None:
                raise ValueError(f"tiny ONNX fixture lacks role {role}")
            relative = Path(str(row["path"]))
            path = (root / relative).resolve()
            try:
                path.relative_to(root)
            except ValueError as exc:
                raise ValueError("tiny ONNX fixture path escaped fixture root") from exc
            expected = str(row.get("sha256", manifest.get("content", {}).get(str(row["path"]), "")))
            if expected.startswith("sha256:"):
                expected = expected[7:]
            actual = _sha256_file(path) if path.is_file() else ""
            if not path.is_file() or (expected and actual != expected):
                raise ValueError(f"tiny ONNX fixture digest mismatch for {role}")
            artifacts.append(SplitArtifact(
                role=str(role),
                path=str(path),
                artifact_name=f"/Model/LLM/Pipeline/TinyOnnx/{str(role).strip('/')}",
                filename=path.name,
                kind="onnx-model",
                backend="onnxruntime",
                metadata={
                    "runtime": TINY_ONNX_RUNTIME,
                    "runtimeBackend": "onnxruntime",
                    "modelFormat": "onnx",
                    "fixtureSchema": fixture_schema,
                    "fixtureRoot": str(root),
                    "stageIndex": int(row["role"].rsplit("/", 1)[-1]),
                    "stageCount": int(stages),
                    "sha256": actual,
                    "stateful": True,
                },
            ))
        services.append(SplitServiceSpec(
            name=service.name,
            model_name=service.model_name,
            roles=list(service.roles),
            dependencies=list(service.dependencies),
            artifacts=artifacts,
            input_schema={**dict(service.input_schema), "codec": "tiny-onnx-context-v1"},
            output_schema={**dict(service.output_schema), "codec": "tiny-onnx-stream-v1"},
            users=list(service.users),
            providers=list(service.providers),
            metadata={
                **dict(service.metadata),
                "execution_implemented": True,
                "runtime": TINY_ONNX_RUNTIME,
                "runtimeBackend": "onnxruntime",
                "fixtureSchema": fixture_schema,
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
        metadata={**dict(splitter.metadata), "runtime": TINY_ONNX_RUNTIME},
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
                         model_name: str,
                         stateful: bool = False,
                         model_family: str = "qwen") -> dict[str, Any]:
    model_family = str(model_family or "qwen").strip().lower()
    if model_family not in {"qwen", "llama"}:
        raise ValueError(f"unsupported ONNX model family: {model_family}")
    supported_llama_models = {
        "HuggingFaceTB/SmolLM2-135M",
        "HuggingFaceTB/SmolLM2-360M",
    }
    if model_family == "llama" and model_name not in supported_llama_models:
        raise ValueError(
            "llama ONNX export supports only the maintained SmolLM2 checkpoints: "
            f"{sorted(supported_llama_models)}")
    if model_family == "qwen" and "smollm" in model_name.lower():
        raise ValueError("qwen ONNX export cannot bind a SmolLM2 checkpoint")
    spec = qwen_transformer_stage_spec(
        role=role,
        stages=stages,
        layer_count=layer_count,
        model_name=model_name,
    )
    spec.update({
        "schema": f"ndnsf-di-{model_family}-onnx-stage-artifact-v1",
        "runtime": QWEN_ONNX_RUNTIME,
        "modelFamily": model_family,
        "modelFormat": "onnx",
        "runtimeBackend": "onnxruntime",
    })
    start = int(spec["layerRange"]["start"])
    end = int(spec["layerRange"]["endExclusive"])
    primary_inputs = (
        ["input_ids", "attention_mask", "position_ids"]
        if int(spec["stageIndex"]) == 0
        else ["attention_mask", "hidden_states", "position_ids"]
    )
    primary_output = (
        "logits" if int(spec["stageIndex"]) == int(spec["stageCount"]) - 1
        else "hidden_states_out"
    )
    if stateful:
        state_inputs = [
            "attention_kv_in", "recurrent_state_in", "convolution_state_in",
        ]
        state_outputs = [
            "attention_kv_out", "recurrent_state_out", "convolution_state_out",
        ]
        spec.update({
            "inputNames": [*primary_inputs, *state_inputs],
            "outputNames": [primary_output, *state_outputs],
            "cacheInputs": [],
            "cacheOutputs": [],
            "stateInputNames": state_inputs,
            "stateOutputNames": state_outputs,
            "sequencePolicy": "stateful-prefill-decode-v1",
        })
    else:
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
        spec.update({
            "inputNames": [*primary_inputs, *cache_inputs],
            "outputNames": [primary_output, *cache_outputs],
            "cacheInputs": cache_inputs,
            "cacheOutputs": cache_outputs,
            "stateInputNames": [],
            "stateOutputNames": [],
            "sequencePolicy": "dynamic-past-key-v1",
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


def _qwen_position_ids_for_nested(value: Any, model_type: str) -> Any:
    """Build the serialized position-id contract for a Qwen model family.

    Qwen3.5 is a hybrid text/vision architecture.  Its text model still
    expects the four-plane ``[4, batch, seq]`` position contract even for a
    text-only request; the first plane is used by the decoder and the other
    three are consumed by the rotary embedding.  Qwen2/Qwen3 retain the
    ordinary ``[batch, seq]`` contract.
    """
    rows = _position_ids_for_nested(value)
    if model_type != "qwen3_5":
        return rows
    if not rows:
        return rows
    if isinstance(rows[0], list):
        return [[list(row) for row in rows] for _ in range(4)]
    return [[list(rows)] for _ in range(4)]


def encode_qwen_pipeline_context(
    input_ids: Any,
    *,
    attention_mask: Any = None,
    position_ids: Any = None,
    request_id: str = "manual",
    session_id: str = "",
    context_epoch: int = 0,
    generation: Mapping[str, Any] | None = None,
    model_type: str = "qwen2",
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
        _qwen_position_ids_for_nested(serializable_ids, str(model_type))
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


def _qwen_head_dim(config: Any) -> int:
    """Return the model-declared attention projection width.

    Qwen3.5-27B deliberately has a non-divisible hidden size (5120) and
    attention-head count (24); deriving this value by integer division yields
    213 instead of the configured 256 and corrupts the persistent KV contract.
    """
    configured = getattr(config, "head_dim", None)
    if configured is not None:
        value = int(configured)
        if value <= 0:
            raise ValueError("Qwen attention head_dim must be positive")
        return value
    hidden_size = int(config.hidden_size)
    heads = int(config.num_attention_heads)
    if heads <= 0 or hidden_size % heads:
        raise ValueError("Qwen config has no valid attention head dimension")
    return hidden_size // heads


def _onnx_stage_wrapper(model: Any, *, stateful: bool = False):
    import torch
    import torch.nn.functional as F
    from torch import nn

    stage_index = int(getattr(model, "ndnsf_stage_index"))
    stage_count = int(getattr(model, "ndnsf_stage_count"))
    start = int(getattr(model, "ndnsf_stage_start"))
    end = int(getattr(model, "ndnsf_stage_end"))
    model_type = str(
        getattr(model, "ndnsf_model_type", "")
        or getattr(getattr(model, "config", None), "model_type", "")
        or "qwen2"
    )
    if model_type == "qwen3_5_text":
        model_type = "qwen3_5"

    layer_indices = list(range(start, end))
    layer_by_index = {
        index: layer
        for index, layer in zip(layer_indices, list(model.model.layers))
    }
    full_layer_indices = [
        index for index in layer_indices
        if getattr(layer_by_index[index], "block_type", "") == "full_attention"
    ]
    linear_layer_indices = [
        index for index in layer_indices
        if getattr(layer_by_index[index], "block_type", "") == "linear_attention"
    ]

    def _onnx_qwen35_rotary_embeddings(hidden_states, position_ids):
        """Functional Qwen3.5 MRoPE encoding for the ONNX graph.

        ``Qwen3_5TextRotaryEmbedding.apply_interleaved_mrope`` writes the
        height/width slices into the temporal slice in place.  The exporter
        lowers those writes to ScatterND; ORT CUDA warns about the resulting
        duplicate-index contract.  Selective ``where`` updates are equivalent
        and keep the positional encoding mutation-free.
        """
        rotary = model.model.rotary_emb
        if position_ids.ndim == 2:
            position_ids = position_ids[None, ...].expand(3, position_ids.shape[0], -1)
        inv_freq = rotary.inv_freq
        inv_freq_expanded = (
            inv_freq[None, None, :, None]
            .float()
            .expand(3, position_ids.shape[1], -1, 1)
            .to(hidden_states.device)
        )
        position_ids_expanded = position_ids[:, :, None, :].float()
        freqs = (
            inv_freq_expanded.float() @ position_ids_expanded.float()
        ).transpose(2, 3)
        freqs_t = freqs[0]
        indices = torch.arange(
            freqs_t.shape[-1], device=freqs_t.device, dtype=torch.long)
        for dim, offset in enumerate((1, 2), start=1):
            length = int(rotary.mrope_section[dim]) * 3
            selected = (
                (indices >= offset)
                & (indices < length)
                & (((indices - offset) % 3) == 0)
            ).view(1, 1, -1)
            freqs_t = torch.where(selected, freqs[dim], freqs_t)
        emb = torch.cat((freqs_t, freqs_t), dim=-1)
        cos = emb.cos() * rotary.attention_scaling
        sin = emb.sin() * rotary.attention_scaling
        return cos.to(dtype=hidden_states.dtype), sin.to(dtype=hidden_states.dtype)

    def _onnx_chunk_gated_delta_rule(
        query,
        key,
        value,
        g,
        beta,
        chunk_size=64,
        initial_state=None,
        output_final_state=False,
        use_qk_l2norm_in_kernel=False,
        **kwargs,
    ):
        """Functional Qwen3.5 recurrent attention path for ONNX export.

        Transformers' reference fallback updates two tensors in place while
        resolving the within-chunk recurrence.  The legacy exporter lowers
        those writes to ScatterND.  On the large FP16 graph ORT CUDA warns
        about potentially duplicated indices and produced a different first
        token, even though the PyTorch stage pipeline was exact.  Building
        the rows/chunks with concatenation and stacking expresses the same
        recurrence without mutation or ScatterND updates.
        """
        # Qwen3.5 applies the two-dimensional padding mask before the
        # projections, but the recurrent kernel itself also has to ignore
        # masked suffix tokens.  Without this, a fixed-context export treats
        # the zeroed suffix as real time steps (the conv bias and recurrent
        # decay still update state), which changes the first generated token.
        attention_mask = kwargs.pop("attention_mask", None)
        initial_dtype = query.dtype
        if use_qk_l2norm_in_kernel:
            query = query * torch.rsqrt(
                (query * query).sum(dim=-1, keepdim=True) + 1e-6)
            key = key * torch.rsqrt(
                (key * key).sum(dim=-1, keepdim=True) + 1e-6)
        query, key, value, beta, g = [
            item.transpose(1, 2).contiguous().to(torch.float32)
            for item in (query, key, value, beta, g)
        ]

        if attention_mask is not None:
            mask = attention_mask.to(query.dtype)
            query = query * mask[:, None, :, None]
            key = key * mask[:, None, :, None]
            value = value * mask[:, None, :, None]
            beta = beta * mask[:, None, :]
            g = g * mask[:, None, :]

        batch_size, num_heads, sequence_length, key_dim = key.shape
        value_dim = value.shape[-1]
        pad_size = (chunk_size - sequence_length % chunk_size) % chunk_size
        if pad_size:
            query = F.pad(query, (0, 0, 0, pad_size))
            key = F.pad(key, (0, 0, 0, pad_size))
            value = F.pad(value, (0, 0, 0, pad_size))
            beta = F.pad(beta, (0, pad_size))
            g = F.pad(g, (0, pad_size))
        total_sequence_length = sequence_length + pad_size
        query = query * (1.0 / (key_dim ** 0.5))
        v_beta = value * beta.unsqueeze(-1)
        k_beta = key * beta.unsqueeze(-1)
        query, key, value, k_beta, v_beta = [
            item.reshape(item.shape[0], item.shape[1], -1, chunk_size, item.shape[-1])
            for item in (query, key, value, k_beta, v_beta)
        ]
        g = g.reshape(g.shape[0], g.shape[1], -1, chunk_size)
        g = g.cumsum(dim=-1)
        decay_mask = (
            (g.unsqueeze(-1) - g.unsqueeze(-2)).tril().exp().float()
        ).tril()

        causal = torch.triu(
            torch.ones(chunk_size, chunk_size, dtype=torch.bool, device=query.device),
            diagonal=0,
        )
        attn = -(
            (k_beta @ key.transpose(-1, -2)) * decay_mask
        ).masked_fill(causal, 0)

        # Resolve the triangular recurrence without in-place indexed writes.
        rows = [attn[..., 0, :].unsqueeze(-2)]
        for row_index in range(1, chunk_size):
            row = attn[..., row_index, :row_index]
            sub = torch.cat(rows, dim=-2)[..., :row_index, :row_index]
            updated = row + (row.unsqueeze(-1) * sub).sum(-2)
            rows.append(torch.cat([
                updated,
                torch.zeros(
                    (*updated.shape[:-1], chunk_size - row_index),
                    dtype=updated.dtype,
                    device=updated.device,
                ),
            ], dim=-1).unsqueeze(-2))
        attn = torch.cat(rows, dim=-2) + torch.eye(
            chunk_size, dtype=attn.dtype, device=attn.device)
        value = attn @ v_beta
        k_cumdecay = attn @ (k_beta * g.exp().unsqueeze(-1))
        last_recurrent_state = (
            torch.zeros(
                batch_size,
                num_heads,
                key_dim,
                value_dim,
                dtype=value.dtype,
                device=value.device,
            )
            if initial_state is None else initial_state.to(value)
        )
        output_chunks = []
        for chunk_index in range(total_sequence_length // chunk_size):
            q_i, k_i, v_i = (
                query[:, :, chunk_index],
                key[:, :, chunk_index],
                value[:, :, chunk_index],
            )
            local_attn = q_i @ k_i.transpose(-1, -2) * decay_mask[:, :, chunk_index]
            v_prime = k_cumdecay[:, :, chunk_index] @ last_recurrent_state
            v_new = v_i - v_prime
            attn_inter = (
                q_i * g[:, :, chunk_index, :, None].exp()
            ) @ last_recurrent_state
            output_chunks.append(attn_inter + local_attn @ v_new)
            last_recurrent_state = (
                last_recurrent_state * g[:, :, chunk_index, -1, None, None].exp()
                + (
                    k_i
                    * (
                        g[:, :, chunk_index, -1, None]
                        - g[:, :, chunk_index]
                    ).exp()[..., None]
                ).transpose(-1, -2) @ v_new
            )
        core_attn_out = torch.cat(output_chunks, dim=-2)
        core_attn_out = core_attn_out[:, :, :sequence_length]
        core_attn_out = core_attn_out.transpose(1, 2).contiguous().to(initial_dtype)
        if not output_final_state:
            last_recurrent_state = None
        return core_attn_out, last_recurrent_state

    def _stateful_recurrent_rule(query, key, value, g, beta, initial_state):
        """Dynamic-length recurrent rule used by stateful ONNX exports.

        The legacy chunk implementation specializes Python padding/chunk
        branches to the sample length.  A scripted loop lowers the recurrence
        as an ONNX ``Loop`` so the same stage accepts prompt prefill and
        one-token cached decode.
        """
        initial_dtype = query.dtype
        batch_size, sequence_length, num_heads, key_dim = query.shape
        value_dim = value.shape[-1]
        last_recurrent_state = initial_state.to(torch.float32)
        # Accumulate into a tensor rather than a TorchScript list.  A list of
        # tensors lowers to ONNX SequenceConstruct/SequenceAt values.  The
        # deployment ORT baseline (1.20) intentionally accepts tensor-only
        # state contracts and rejects those sequence TypeProto values while
        # loading an otherwise valid Qwen graph.
        output_steps = torch.zeros(
            (batch_size, num_heads, 0, value_dim),
            dtype=torch.float32,
            device=query.device,
        )
        for index in range(sequence_length):
            q_t = query[:, index]
            k_t = key[:, index]
            v_t = value[:, index]
            q_t = q_t * torch.rsqrt(
                (q_t * q_t).sum(dim=-1, keepdim=True) + 1e-6)
            k_t = k_t * torch.rsqrt(
                (k_t * k_t).sum(dim=-1, keepdim=True) + 1e-6)
            q_t = q_t.to(torch.float32) * (1.0 / (key_dim ** 0.5))
            k_t = k_t.to(torch.float32)
            v_t = v_t.to(torch.float32)
            g_t = g[:, index].to(torch.float32).exp().unsqueeze(-1).unsqueeze(-1)
            beta_t = beta[:, index].to(torch.float32).unsqueeze(-1)
            last_recurrent_state = last_recurrent_state * g_t
            kv_mem = (last_recurrent_state * k_t.unsqueeze(-1)).sum(dim=-2)
            delta = (v_t - kv_mem) * beta_t
            last_recurrent_state = (
                last_recurrent_state
                + k_t.unsqueeze(-1) * delta.unsqueeze(-2)
            )
            output_step = (last_recurrent_state * q_t.unsqueeze(-1)).sum(dim=-2)
            output_steps = torch.cat(
                (output_steps, output_step.unsqueeze(2)), dim=2)
        core_attn_out = output_steps
        return (
            core_attn_out.transpose(1, 2).contiguous().to(initial_dtype),
            last_recurrent_state,
        )

    stateful_recurrent_rule = (
        torch.jit.script(_stateful_recurrent_rule) if stateful else None
    )

    def _onnx_qwen35_gated_delta_forward(
        module,
        hidden_states,
        cache_params=None,
        attention_mask=None,
        **kwargs,
    ):
        """Export-only Qwen3.5 linear-attention forward.

        The stock Qwen3.5 module forwards attention_mask only to
        apply_mask_to_padding_states and not to the chunk kernel.  That is
        sufficient for ordinary variable-length inputs, but not for the
        fixed-context ONNX contract: masked suffix positions would still
        advance the recurrent state.  This no-cache export path preserves
        the model equations while passing the mask to the functional kernel
        above.  It is installed only on the temporary export wrapper; the
        Transformers model implementation is not modified for deployment.
        """
        if cache_params is not None and not stateful:
            raise ValueError("Qwen3.5 ONNX export does not support cache_params")
        if attention_mask is not None:
            hidden_states = hidden_states * attention_mask.to(
                hidden_states.dtype
            ).unsqueeze(-1)
        batch_size, sequence_length, _ = hidden_states.shape
        use_precomputed_states = stateful and cache_params is not None
        if use_precomputed_states:
            conv_state = cache_params.layers[module.layer_idx].conv_states[0]
            recurrent_state = cache_params.layers[
                module.layer_idx
            ].recurrent_states[0]
        mixed_qkv = module.in_proj_qkv(hidden_states).transpose(1, 2)
        z = module.in_proj_z(hidden_states)
        z = z.reshape(batch_size, sequence_length, -1, module.head_v_dim)
        b = module.in_proj_b(hidden_states)
        a = module.in_proj_a(hidden_states)

        if use_precomputed_states:
            # Always include the cached left context.  This is equivalent to
            # the stock single-token fast path for a one-token decode while
            # also supporting a variable-length prefill in the same graph.
            mixed_qkv = torch.cat((conv_state, mixed_qkv), dim=-1)
            new_conv_state = F.pad(
                mixed_qkv,
                (module.conv_kernel_size - mixed_qkv.shape[-1], 0),
            )
            cache_params.update_conv_state(new_conv_state, module.layer_idx)
            if module.causal_conv1d_fn is not None:
                mixed_qkv = module.causal_conv1d_fn(
                    x=mixed_qkv,
                    weight=module.conv1d.weight.squeeze(1),
                    bias=module.conv1d.bias,
                    activation=module.activation,
                    seq_idx=kwargs.get("seq_idx"),
                )
            else:
                mixed_qkv = torch.nn.functional.silu(
                    module.conv1d(mixed_qkv)[:, :, :mixed_qkv.shape[-1]]
                )
            mixed_qkv = mixed_qkv[:, :, -sequence_length:]
        elif module.causal_conv1d_fn is not None:
            mixed_qkv = module.causal_conv1d_fn(
                x=mixed_qkv,
                weight=module.conv1d.weight.squeeze(1),
                bias=module.conv1d.bias,
                activation=module.activation,
                seq_idx=kwargs.get("seq_idx"),
            )
        else:
            mixed_qkv = torch.nn.functional.silu(
                module.conv1d(mixed_qkv)[:, :, :mixed_qkv.shape[-1]]
            )
        mixed_qkv = mixed_qkv.transpose(1, 2)
        query, key, value = torch.split(
            mixed_qkv,
            [module.key_dim, module.key_dim, module.value_dim],
            dim=-1,
        )
        query = query.reshape(batch_size, sequence_length, -1, module.head_k_dim)
        key = key.reshape(batch_size, sequence_length, -1, module.head_k_dim)
        value = value.reshape(batch_size, sequence_length, -1, module.head_v_dim)
        beta = b.sigmoid()
        g = -module.A_log.float().exp() * torch.nn.functional.softplus(
            a.float() + module.dt_bias
        )
        if module.num_v_heads // module.num_k_heads > 1:
            repeat = module.num_v_heads // module.num_k_heads
            query = query.repeat_interleave(repeat, dim=2)
            key = key.repeat_interleave(repeat, dim=2)

        if use_precomputed_states:
            if stateful_recurrent_rule is None:
                raise RuntimeError("stateful recurrent rule was not initialized")
            core_attn_out, last_recurrent_state = stateful_recurrent_rule(
                query, key, value, g, beta, recurrent_state)
            cache_params.update_recurrent_state(
                last_recurrent_state, module.layer_idx)
        else:
            core_attn_out, _ = module.chunk_gated_delta_rule(
                query,
                key,
                value,
                g=g,
                beta=beta,
                initial_state=None,
                output_final_state=False,
                use_qk_l2norm_in_kernel=True,
                cu_seqlens=kwargs.get("cu_seq_lens_q"),
                attention_mask=attention_mask,
            )
        core_attn_out = core_attn_out.reshape(-1, module.head_v_dim)
        z = z.reshape(-1, module.head_v_dim)
        core_attn_out = module.norm(core_attn_out, z)
        core_attn_out = core_attn_out.reshape(batch_size, sequence_length, -1)
        return module.out_proj(core_attn_out)

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

    class _StatefulLayerView:
        def __init__(self, convolution_state, recurrent_state):
            self.conv_states = {0: convolution_state}
            self.recurrent_states = {0: recurrent_state}

    class _StatefulExportCache:
        """Pure tensor cache bridge for one exported stage.

        Transformers' runtime cache mutates CUDA buffers in place.  During
        export we instead replace tensor references and return the complete
        successor state as graph outputs.  The deployment adapter owns the
        equivalent state transitions after ONNX Runtime execution.
        """

        def __init__(self, attention_kv, recurrent_state, convolution_state):
            if len(full_layer_indices) == 0 or len(linear_layer_indices) == 0:
                raise ValueError("Qwen3.5 stateful stage must contain both layer types")
            # The recurrent update is accumulated in float32 for numerical
            # stability, but the public state boundary must retain the
            # exported model dtype.  Otherwise a FP16 graph emits a FP32
            # recurrent_state_out that cannot be fed to recurrent_state_in on
            # the next decode epoch.
            self.state_dtype = attention_kv.dtype
            self.values = {}
            self.layers = {}
            for cursor, layer in enumerate(full_layer_indices):
                self.values[layer] = [
                    attention_kv[0, cursor], attention_kv[1, cursor]
                ]
            for cursor, layer in enumerate(linear_layer_indices):
                self.layers[layer] = _StatefulLayerView(
                    convolution_state[cursor], recurrent_state[cursor]
                )

        def has_previous_state(self, layer_idx=None, state_idx=None):
            del state_idx
            if layer_idx is None:
                return True
            return int(layer_idx) in self.layers

        def update(self, key_states, value_states, layer_idx, cache_kwargs=None):
            del cache_kwargs
            past_key, past_value = self.values[int(layer_idx)]
            present_key = torch.cat((past_key, key_states), dim=2)
            present_value = torch.cat((past_value, value_states), dim=2)
            self.values[int(layer_idx)] = [present_key, present_value]
            return present_key, present_value

        def update_conv_state(self, convolution_state, layer_idx,
                             state_idx=0, **kwargs):
            del kwargs
            view = self.layers[int(layer_idx)]
            kernel = view.conv_states[state_idx].shape[-1]
            successor = convolution_state[..., -kernel:]
            view.conv_states[state_idx] = successor
            return successor

        def update_recurrent_state(self, recurrent_state, layer_idx,
                                   state_idx=0, **kwargs):
            del kwargs
            view = self.layers[int(layer_idx)]
            view.recurrent_states[state_idx] = recurrent_state
            return recurrent_state

        def state_outputs(self):
            attention = torch.stack([
                torch.stack([
                    self.values[layer][0], self.values[layer][1]
                ], dim=0)
                for layer in full_layer_indices
            ], dim=1)
            recurrent = torch.stack([
                self.layers[layer].recurrent_states[0]
                for layer in linear_layer_indices
            ], dim=0).to(self.state_dtype)
            convolution = torch.stack([
                self.layers[layer].conv_states[0]
                for layer in linear_layer_indices
            ], dim=0).to(self.state_dtype)
            return attention, recurrent, convolution

    class _QwenOnnxStage(nn.Module):
        def __init__(self):
            super().__init__()
            self.model = model
            if model_type == "qwen3_5":
                # Keep the normal Transformers implementation untouched.  The
                # export wrapper alone uses the functional form above.
                for layer in self.model.model.layers:
                    linear_attention = getattr(layer, "linear_attn", None)
                    if linear_attention is not None:
                        linear_attention.chunk_gated_delta_rule = (
                            _onnx_chunk_gated_delta_rule
                        )
                        # The stock module does not pass its padding mask into
                        # the recurrent kernel.  Bind this export-only method
                        # to the temporary stage model.
                        import types
                        linear_attention.forward = types.MethodType(
                            _onnx_qwen35_gated_delta_forward,
                            linear_attention,
                        )

        def forward(self, input_ids, attention_mask, hidden_states, position_ids, *state_values):
            base = self.model.model
            if stage_index == 0:
                hidden_states = base.embed_tokens(input_ids)
            if stateful:
                if len(state_values) != 3:
                    raise ValueError(
                        "stateful Qwen3.5 stages require three state tensors")
                cache = _StatefulExportCache(*state_values)
            else:
                cache = _ExportCache(state_values)
            query_length = hidden_states.shape[1]
            past_length = (
                state_values[0].shape[4] if stateful
                else state_values[0].shape[2]
            )
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
            layer_position_ids, rotary_position_ids = (
                _qwen_layer_position_inputs(position_ids, model_type)
            )
            if model_type == "qwen3_5":
                # Qwen3.5 has two token mixers.  Full-attention layers use the
                # usual causal mask; linear-attention layers only need the
                # 2-D padding mask (an all-one mask is semantically a no-op).
                linear_mask = attention_mask
                if stateful:
                    linear_mask = attention_mask[:, -query_length:]
                layer_attention_mask = {
                    "full_attention": causal_mask,
                    "linear_attention": linear_mask,
                }
            else:
                layer_attention_mask = causal_mask
            if model_type == "qwen3_5":
                position_embeddings = _onnx_qwen35_rotary_embeddings(
                    hidden_states, rotary_position_ids)
            else:
                position_embeddings = _rotary_embeddings(
                    base, hidden_states, rotary_position_ids)
            for layer in list(base.layers):
                attention_for_layer = _qwen_attention_mask_for_layer(
                    layer_attention_mask, layer)
                output = _call_with_supported_kwargs(
                    layer.forward,
                    hidden_states=hidden_states,
                    position_ids=layer_position_ids,
                    position_embeddings=position_embeddings,
                    attention_mask=attention_for_layer,
                    past_key_value=(
                        cache if (model_type == "qwen3_5" and stateful)
                        else (None if model_type == "qwen3_5" else cache)
                    ),
                    past_key_values=(
                        cache if (model_type == "qwen3_5" and stateful)
                        else (None if model_type == "qwen3_5" else cache)
                    ),
                    use_cache=(model_type != "qwen3_5"),
                    cache_position=layer_position_ids[0],
                    output_attentions=False,
                )
                hidden_states = output[0] if isinstance(output, tuple) else output
            primary = hidden_states
            if stage_index < stage_count - 1:
                primary = hidden_states
            else:
                primary = self.model.lm_head(base.norm(hidden_states))
            if stateful:
                return (primary, *cache.state_outputs())
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
                            *, sample_input_ids: Any,
                            export_dtype: str = "auto",
                            fixed_context: bool = False,
                            stateful: bool = False) -> dict[str, Any]:
    import torch

    hidden_size = int(model.config.hidden_size)
    seq_len = int(sample_input_ids.shape[1])
    model_type = str(
        getattr(model, "ndnsf_model_type", "")
        or getattr(getattr(model, "config", None), "model_type", "")
        or "qwen2"
    )
    if model_type == "qwen3_5_text":
        model_type = "qwen3_5"

    # Torch < 2.6's legacy ONNX symbolic cannot serialize the SDPA
    # floating-point scale used by Qwen3 and Llama-family checkpoints.
    # Eager attention is mathematically equivalent for this export and keeps
    # the compatibility decision explicit; it does not alter the native ONNX
    # Runtime execution contract.
    export_attention_implementation = str(
        getattr(getattr(model, "config", None), "_attn_implementation", "")
        or "sdpa"
    )
    torch_version = []
    for component in str(getattr(torch, "__version__", "0.0")).split(".")[:2]:
        digits = "".join(character for character in component if character.isdigit())
        torch_version.append(int(digits or 0))
    if (model_type in {"qwen3", "llama"}
            and export_attention_implementation == "sdpa"
            and tuple(torch_version) < (2, 6)):
        model.config._attn_implementation = "eager"
        export_attention_implementation = "eager"

    wrapper, stage_index, stage_count, start, end = _onnx_stage_wrapper(
        model, stateful=stateful)
    if export_dtype == "float16":
        tensor_dtype = torch.float16
    elif export_dtype == "float32":
        tensor_dtype = torch.float32
    else:
        tensor_dtype = next(
            (parameter.dtype for parameter in model.parameters()
             if parameter.is_floating_point()),
            torch.float32,
        )
        if tensor_dtype not in (torch.float16, torch.float32, torch.bfloat16):
            tensor_dtype = torch.float32
    dummy_hidden = torch.zeros(
        (int(sample_input_ids.shape[0]), seq_len, hidden_size),
        dtype=tensor_dtype,
    )
    position_ids = torch.arange(seq_len, dtype=torch.long).unsqueeze(0)
    if model_type == "qwen3_5":
        position_ids = position_ids.view(1, 1, -1).expand(
            4, int(sample_input_ids.shape[0]), -1
        )
    layer_indices = list(range(start, end))
    kv_heads = int(getattr(model.config, "num_key_value_heads",
                           model.config.num_attention_heads))
    # Qwen3.5 configurations may use a head dimension that is not equal to
    # hidden_size // num_attention_heads (Qwen3.6-27B uses 5120/24 != 256).
    # The explicit configuration value is the projection/cache contract.
    head_dim = _qwen_head_dim(model.config)
    layer_by_index = {
        index: layer
        for index, layer in zip(layer_indices, list(model.model.layers))
    }
    full_layers = [
        index for index in layer_indices
        if getattr(layer_by_index[index], "block_type", "") == "full_attention"
    ]
    linear_layers = [
        index for index in layer_indices
        if getattr(layer_by_index[index], "block_type", "") == "linear_attention"
    ]
    if stateful:
        if model_type != "qwen3_5":
            raise ValueError("stateful ONNX export requires Qwen3.5")
        linear_heads = int(model.config.linear_num_value_heads)
        linear_key_dim = int(model.config.linear_key_head_dim)
        linear_value_dim = int(model.config.linear_value_head_dim)
        linear_conv_dim = (
            int(model.config.linear_num_key_heads) * linear_key_dim * 2
            + linear_heads * linear_value_dim
        )
        state_values = (
            torch.empty(
                (2, len(full_layers), int(sample_input_ids.shape[0]), kv_heads,
                 0, head_dim), dtype=tensor_dtype
            ),
            torch.zeros(
                (len(linear_layers), int(sample_input_ids.shape[0]),
                 linear_heads, linear_key_dim, linear_value_dim),
                dtype=tensor_dtype,
            ),
            torch.zeros(
                (len(linear_layers), int(sample_input_ids.shape[0]),
                 linear_conv_dim, int(model.config.linear_conv_kernel_dim)),
                dtype=tensor_dtype,
            ),
        )
    else:
        state_values = tuple(
            torch.empty((int(sample_input_ids.shape[0]), kv_heads, 0, head_dim),
                        dtype=tensor_dtype)
            for _ in range(len(layer_indices) * 2)
        )
    attention_mask = torch.ones(
        (int(sample_input_ids.shape[0]), seq_len), dtype=torch.long)
    input_names = ["input_ids", "attention_mask", "hidden_states", "position_ids"]
    output_names = ["logits" if stage_index == stage_count - 1 else "hidden_states_out"]
    if stateful:
        input_names += ["attention_kv_in", "recurrent_state_in", "convolution_state_in"]
        output_names += ["attention_kv_out", "recurrent_state_out", "convolution_state_out"]
    else:
        input_names += [
            name for layer in layer_indices
            for name in (f"past_key.{layer}", f"past_value.{layer}")
        ]
        output_names += [
            name for layer in layer_indices
            for name in (f"present_key.{layer}", f"present_value.{layer}")
        ]
    # Qwen3.5's linear-attention implementation specializes chunk/padding
    # branches during tracing.  A fixed-context export must therefore keep
    # every sequence dimension static; runtime padding alone is insufficient
    # because a dynamic ONNX axis still permits the traced 20-token branch.
    dynamic_axes = None
    if not fixed_context:
        dynamic_axes = {
            "input_ids": {1: "seq"},
            "hidden_states": {1: "seq"},
            "position_ids": (
                {1: "batch", 2: "seq"}
                if model_type == "qwen3_5" else {1: "seq"}
            ),
            "attention_mask": {1: "total_seq"},
            output_names[0]: {1: "seq"},
        }
        if stateful:
            dynamic_axes["attention_kv_in"] = {4: "past_seq"}
            dynamic_axes["attention_kv_out"] = {4: "total_seq"}
        else:
            for layer in layer_indices:
                dynamic_axes[f"past_key.{layer}"] = {2: "past_seq"}
                dynamic_axes[f"past_value.{layer}"] = {2: "past_seq"}
                dynamic_axes[f"present_key.{layer}"] = {2: "total_seq"}
                dynamic_axes[f"present_value.{layer}"] = {2: "total_seq"}
    # The legacy Torch exporter writes external-data files relative to the
    # process cwd rather than the directory containing ``f``.  A large stage
    # therefore looked valid but left its weight files beside the launcher;
    # ORT resolves external locations relative to the ONNX file and could not
    # load the resulting stage.  Export from the artifact directory and pass
    # only the basename so every external tensor is colocated with its graph.
    onnx_path = onnx_path.resolve()
    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    previous_cwd = Path.cwd()
    try:
        os.chdir(onnx_path.parent)
        torch.onnx.export(
            wrapper,
            (sample_input_ids, attention_mask, dummy_hidden, position_ids, *state_values),
            onnx_path.name,
            input_names=input_names,
            output_names=output_names,
            dynamic_axes=dynamic_axes,
            opset_version=17,
            # Qwen3.5's hybrid linear-attention graph contains exporter-side
            # scalar expressions that Torch 2.6 may misclassify as ComplexDouble
            # during legacy constant folding.  Leave folding to ONNX Runtime,
            # which also keeps the exported graph faithful to the runtime path.
            do_constant_folding=False,
        )
    finally:
        os.chdir(previous_cwd)
    import onnx

    if stateful:
        # PyTorch 2.6's legacy exporter may emit TorchScript Loop-body nodes
        # out of dependency order when a scripted recurrence captures tensor
        # intermediates.  ORT accepts the top-level graph but rejects the
        # body at execution time (typically ``Missing Input: <id>``).  Sort
        # each exported subgraph without changing operators or tensors.
        def sort_subgraphs(graph):
            produced = {output for node in graph.node for output in node.output}
            known = {value.name for value in graph.input}
            known.update(value.name for value in graph.initializer)
            remaining = list(graph.node)
            ordered = []
            while remaining:
                ready = [
                    node for node in remaining
                    if all(
                        not value
                        or value in known
                        or value not in produced
                        for value in node.input
                    )
                ]
                if not ready:
                    raise RuntimeError(
                        "stateful ONNX export produced a cyclic subgraph")
                for node in ready:
                    ordered.append(node)
                    known.update(value for value in node.output if value)
                    remaining.remove(node)
            graph.ClearField("node")
            graph.node.extend(ordered)
            for node in graph.node:
                for attribute in node.attribute:
                    if attribute.type == onnx.AttributeProto.GRAPH:
                        sort_subgraphs(attribute.g)
                    elif attribute.type == onnx.AttributeProto.GRAPHS:
                        for subgraph in attribute.graphs:
                            sort_subgraphs(subgraph)

    onnx_model = onnx.load(str(onnx_path), load_external_data=False)

    def iter_graphs(graph):
        yield graph
        for node in graph.node:
            for attribute in node.attribute:
                if attribute.type == onnx.AttributeProto.GRAPH:
                    yield from iter_graphs(attribute.g)
                elif attribute.type == onnx.AttributeProto.GRAPHS:
                    for subgraph in attribute.graphs:
                        yield from iter_graphs(subgraph)

    def iter_tensors(graph):
        yield from graph.initializer
        for node in graph.node:
            for attribute in node.attribute:
                if attribute.type == onnx.AttributeProto.TENSOR:
                    yield attribute.t

    # The deployed stateful contract is tensor-only.  In particular, a
    # scripted recurrence must not leak a Python list into the graph: Torch
    # lowers such a list to SequenceConstruct/SequenceAt and ORT 1.20 rejects
    # the resulting Sequence TypeProto while loading the model.  Fail with a
    # precise exporter error if a future model/wrapper reintroduces one.
    non_tensor_types = []
    for graph in iter_graphs(onnx_model.graph):
        for values in (graph.input, graph.output, graph.value_info):
            for value in values:
                value_case = value.type.WhichOneof("value")
                if value_case not in (None, "tensor_type"):
                    non_tensor_types.append(f"{value.name}:{value_case}")
    if non_tensor_types:
        raise RuntimeError(
            "QWEN_ONNX_NON_TENSOR_TYPE: "
            + ",".join(non_tensor_types[:8])
            + ("..." if len(non_tensor_types) > 8 else "")
        )

    external_locations = []
    missing_external = []
    artifact_dir = onnx_path.parent.resolve()
    for graph in iter_graphs(onnx_model.graph):
        for tensor in iter_tensors(graph):
            if tensor.data_location != onnx.TensorProto.EXTERNAL:
                continue
            location = next(
                (entry.value for entry in tensor.external_data
                 if entry.key == "location"),
                "",
            )
            if not location:
                missing_external.append(f"{tensor.name}:empty-location")
                continue
            resolved = (artifact_dir / location).resolve()
            try:
                resolved.relative_to(artifact_dir)
            except ValueError:
                missing_external.append(f"{tensor.name}:path-escape:{location}")
                continue
            external_locations.append(location)
            if not resolved.is_file() or resolved.stat().st_size == 0:
                missing_external.append(f"{tensor.name}:{location}")
    if missing_external:
        raise RuntimeError(
            "QWEN_ONNX_EXTERNAL_DATA_MISSING: "
            + ",".join(missing_external[:8])
            + ("..." if len(missing_external) > 8 else "")
        )
    if stateful:
        sort_subgraphs(onnx_model.graph)
        onnx.save(onnx_model, str(onnx_path))
    graph = onnx_model.graph
    actual_input_names = [value.name for value in graph.input]
    actual_output_names = [value.name for value in graph.output]

    # ONNX exporters are free to reorder graph inputs/outputs.  The native
    # state transaction pairs each state output with the corresponding state
    # input by vector position, so preserving graph order here would silently
    # bind (for example) recurrent state to the attention-KV slot.  Publish a
    # canonical family order while retaining the graph's complete input/output
    # signatures separately.
    canonical_state_inputs = [
        "attention_kv_in", "recurrent_state_in", "convolution_state_in"]
    canonical_state_outputs = [
        "attention_kv_out", "recurrent_state_out", "convolution_state_out"]
    state_input_names = [
        name for name in actual_input_names if name.endswith("_in")]
    state_output_names = [
        name for name in actual_output_names if name.endswith("_out")]
    if stateful:
        # ``hidden_states_out`` is a regular stage activation, not a
        # persistent state component.  Select only the three adapter-owned
        # families rather than treating every ``*_out`` graph value as state.
        state_input_names = [
            name for name in canonical_state_inputs if name in actual_input_names]
        state_output_names = [
            name for name in canonical_state_outputs if name in actual_output_names]
        if (len(state_input_names) != len(canonical_state_inputs)
                or len(state_output_names) != len(canonical_state_outputs)):
            raise RuntimeError(
                "QWEN_ONNX_STATE_IO_INCOMPLETE: "
                f"inputs={actual_input_names} outputs={actual_output_names}")

    def contract(value):
        tensor = value.type.tensor_type
        dimensions = []
        for dim in tensor.shape.dim:
            dimensions.append(
                int(dim.dim_value) if dim.HasField("dim_value") else str(dim.dim_param)
            )
        return {"elementType": int(tensor.elem_type), "shape": dimensions}

    if stateful:
        contracts_by_name = {
            value.name: contract(value)
            for value in [*graph.input, *graph.output]
        }
        for input_name, output_name in zip(
                canonical_state_inputs, canonical_state_outputs):
            if (contracts_by_name[input_name]["elementType"]
                    != contracts_by_name[output_name]["elementType"]):
                raise RuntimeError(
                    "QWEN_ONNX_STATE_DTYPE_MISMATCH: "
                    f"{input_name}!={output_name}")

    return {
        "stageIndex": stage_index,
        "stageCount": stage_count,
        "modelType": model_type,
        "layerRange": {"start": start, "endExclusive": end},
        "inputNames": actual_input_names,
        "outputNames": actual_output_names,
        "cacheInputs": [name for name in actual_input_names if name.startswith("past_")],
        "cacheOutputs": [name for name in actual_output_names if name.startswith("present_")],
        "stateInputNames": state_input_names,
        "stateOutputNames": state_output_names,
        "sequencePolicy": (
            "stateful-prefill-decode-v1" if stateful
            else "fixed-context-padded-v1" if fixed_context
            else "dynamic-past-key-v1"
        ),
        "exportAttentionImplementation": export_attention_implementation,
        "tensorContracts": {
            value.name: contract(value)
            for value in [*graph.input, *graph.output]
        },
    }


def _validate_qwen_onnx_stages(artifacts: list[SplitArtifact], *,
                               input_ids: Any, attention_mask: Any,
                               config: Any,
                               stateful: bool = False) -> dict[str, Any]:
    import numpy as np
    import onnxruntime as ort

    hidden = np.zeros(
        (int(input_ids.shape[0]), int(input_ids.shape[1]), int(config.hidden_size)),
        dtype=np.float32,
    )
    ids = input_ids.detach().cpu().numpy().astype(np.int64)
    mask = attention_mask.detach().cpu().numpy().astype(np.int64)
    model_type = str(getattr(config, "model_type", "qwen2"))
    if model_type == "qwen3_5_text":
        model_type = "qwen3_5"
    position_ids = np.arange(ids.shape[1], dtype=np.int64).reshape(1, -1)
    if model_type == "qwen3_5":
        position_ids = np.broadcast_to(
            position_ids.reshape(1, 1, -1),
            (4, int(ids.shape[0]), int(ids.shape[1])),
        )
    kv_heads = int(getattr(config, "num_key_value_heads", config.num_attention_heads))
    head_dim = _qwen_head_dim(config)
    stage_records = []
    logits = None
    for artifact in artifacts:
        options = ort.SessionOptions()
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
        session = ort.InferenceSession(
            artifact.path, sess_options=options, providers=["CPUExecutionProvider"])
        metadata = dict(artifact.metadata or {})
        artifact_stateful = bool(
            stateful
            or metadata.get("stateInputNames")
            or metadata.get("sequencePolicy") == "stateful-prefill-decode-v1"
        )

        def state_shape(value):
            dimensions = []
            for axis, dimension in enumerate(getattr(value, "shape", ())):
                if isinstance(dimension, int):
                    dimensions.append(dimension)
                    continue
                text = str(dimension)
                if text.isdigit():
                    dimensions.append(int(text))
                elif text in {"batch", "B"}:
                    dimensions.append(int(ids.shape[0]))
                elif text in {"seq", "total_seq"}:
                    dimensions.append(int(ids.shape[1]))
                elif text in {"past", "past_seq"}:
                    dimensions.append(0)
                else:
                    raise RuntimeError(
                        f"unresolved exported Qwen state dimension: {value.name}:{dimension}")
            return tuple(dimensions)

        feed = {}
        for item in session.get_inputs():
            if item.name == "input_ids":
                feed[item.name] = ids
            elif item.name == "attention_mask":
                feed[item.name] = mask
            elif item.name == "hidden_states":
                feed[item.name] = hidden.astype(
                    _onnx_input_numpy_dtype(getattr(item, "type", ""), np.float32),
                    copy=False,
                )
            elif item.name == "position_ids":
                feed[item.name] = position_ids
            elif artifact_stateful and item.name in {
                    "attention_kv_in", "recurrent_state_in",
                    "convolution_state_in"}:
                shape = state_shape(item)
                dtype = _onnx_input_numpy_dtype(
                    getattr(item, "type", ""), np.float32)
                feed[item.name] = np.empty(shape, dtype=dtype)
                if item.name != "attention_kv_in":
                    feed[item.name].fill(0)
            elif item.name.startswith(("past_key.", "past_value.")):
                feed[item.name] = np.empty(
                    (ids.shape[0], kv_heads, 0, head_dim),
                    dtype=_onnx_input_numpy_dtype(
                        getattr(item, "type", ""), np.float32),
                )
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
            "cacheOutputCount": (
                0 if artifact_stateful else len(outputs) - 1),
            "stateOutputCount": (
                len(outputs) - 1 if artifact_stateful else 0),
            "sequencePolicy": metadata.get(
                "sequencePolicy",
                "stateful-prefill-decode-v1" if artifact_stateful
                else "dynamic-past-key-v1"),
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
    stateful: bool = False,
    model_family: str = "qwen",
) -> list[SplitArtifact]:
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    model_family = str(model_family or "qwen").strip().lower()
    if model_family not in {"qwen", "llama"}:
        raise ValueError(f"unsupported ONNX model family: {model_family}")
    supported_llama_models = {
        "HuggingFaceTB/SmolLM2-135M",
        "HuggingFaceTB/SmolLM2-360M",
    }
    if model_family == "llama" and model_name not in supported_llama_models:
        raise ValueError(
            "llama ONNX export supports only the maintained SmolLM2 checkpoints: "
            f"{sorted(supported_llama_models)}")
    if model_family == "qwen" and "smollm" in model_name.lower():
        raise ValueError("qwen ONNX export cannot bind a SmolLM2 checkpoint")
    root = Path(output_dir) / f"{model_family}-onnx-stage-artifacts"
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
    tokenizer_dir = Path(output_dir) / f"{model_family}-onnx-tokenizer"
    tokenizer_dir.mkdir(parents=True, exist_ok=True)
    tokenizer.save_pretrained(str(tokenizer_dir))
    tokenizer_json = tokenizer_dir / "tokenizer.json"
    if not tokenizer_json.is_file():
        raise RuntimeError(
            "Qwen ONNX export must produce standalone tokenizer.json; "
            f"missing {tokenizer_json}")
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
    eos_token_ids = getattr(tokenizer, "eos_token_id", None)
    if eos_token_ids is None:
        eos_token_ids = getattr(model_config, "eos_token_id", None)
    if isinstance(eos_token_ids, int):
        eos_token_ids = [eos_token_ids]
    if not isinstance(eos_token_ids, (list, tuple)):
        raise RuntimeError(
            f"{model_family.upper()}_EOS_TOKEN_IDS_MISSING: tokenizer/config has no eos token")
    eos_token_ids = [int(item) for item in eos_token_ids]
    if not eos_token_ids or any(item < 0 for item in eos_token_ids):
        raise RuntimeError(f"{model_family.upper()}_EOS_TOKEN_IDS_INVALID")
    full_state = full_model.state_dict()
    stage_packages: list[tuple[str, dict[str, Any], Path]] = []
    for role in roles:
        spec = qwen_onnx_stage_spec(
            role=role,
            stages=stages,
            layer_count=layer_count,
            model_name=model_name,
            stateful=stateful,
            model_family=model_family,
        )
        pt_path = root / f"stage-{spec['stageIndex']}-{model_family}-onnx-export.pt"
        package = {
            "schema": f"ndnsf-di-{model_family}-stage-weights-v1",
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
        filename = f"stage-{spec['stageIndex']}-{model_family}.onnx"
        onnx_path = root / filename
        export_info = _export_qwen_onnx_stage(
            stage_model,
            onnx_path,
            sample_input_ids=sample_input_ids,
            export_dtype=dtype,
            stateful=stateful,
        )
        artifacts.append(SplitArtifact(
            role=role,
            path=str(onnx_path),
            artifact_name=f"/Model/LLM/Pipeline/{model_family.title()}Onnx/{role.strip('/')}",
            filename=filename,
            kind="onnx-model",
            backend="onnxruntime",
            metadata={
                "runtime": QWEN_ONNX_RUNTIME,
                "stageIndex": spec["stageIndex"],
                "stageCount": spec["stageCount"],
                "layerCount": spec["layerCount"],
                "layerRange": dict(spec["layerRange"]),
                "modelFamily": model_family,
                "modelFormat": "onnx",
                "runtimeBackend": "onnxruntime",
                "materialization": "exporter-contract-only",
                "hiddenSize": hidden_size,
                "inputNames": export_info["inputNames"],
                "outputNames": export_info["outputNames"],
                "cacheInputs": export_info["cacheInputs"],
                "cacheOutputs": export_info["cacheOutputs"],
                "stateInputNames": export_info["stateInputNames"],
                "stateOutputNames": export_info["stateOutputNames"],
                "sequencePolicy": export_info["sequencePolicy"],
                "exportAttentionImplementation": export_info[
                    "exportAttentionImplementation"],
                "tensorContracts": export_info["tensorContracts"],
            },
        ))
        # Stage packages are exporter-only inputs.  Never leave them beside
        # the canonical ONNX artifacts where a deployment packager could
        # accidentally promote a Transformers/PyTorch dependency.
        pt_path.unlink(missing_ok=True)
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
            stateful=stateful,
        )
        if validation["topToken"] != expected_top_token:
            raise RuntimeError(
                "staged Qwen ONNX top token differs from frozen full-model baseline: "
                f"{validation['topToken']} != {expected_top_token}")
        runtime_summary = {
            "schema": f"ndnsf-di-{model_family}-onnx-pipeline-runtime-v1",
            "modelFamily": model_family,
            "model": model_name,
            # Keep the runtime summary on the canonical revision discovered by
            # the loader; the service manifest uses this same value.
            "modelRevision": resolved_revision,
            "dtype": dtype,
            "prompt": prompt,
            "runtime": QWEN_ONNX_RUNTIME,
            "stages": stages,
            "layerCount": layer_count,
            "layerRanges": [list(item) for item in split_layer_ranges(layer_count, stages)],
            "inputIds": input_ids.cpu().tolist(),
            "attentionMask": attention_mask.cpu().tolist(),
            "expectedTopToken": expected_top_token,
            "eosTokenIds": eos_token_ids,
            "stagedValidation": validation,
            "fullMs": full_ms,
        }
        (Path(output_dir) / f"{model_family}-pipeline-runtime.json").write_text(
            json.dumps(runtime_summary, indent=2, sort_keys=True),
            encoding="utf-8",
        )
    manifest = {
        "schema": f"ndnsf-di-{model_family}-onnx-service-manifest-v1",
        "modelFamily": model_family,
        "model": model_name,
        "modelRevision": resolved_revision,
        "dtype": dtype,
        "tokenizer": str(tokenizer_dir),
        "tokenizerSha256": _sha256_file(tokenizer_json),
        "stageCount": stages,
        "layerCount": layer_count,
        "expectedTopToken": expected_top_token,
        "stagedValidation": validation,
        "sequencePolicy": (
            "stateful-prefill-decode-v1" if stateful
            else "dynamic-past-key-v1"),
        "stages": [],
    }
    manifest["eosTokenIds"] = eos_token_ids
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
            "stateInputNames": artifact.metadata["stateInputNames"],
            "stateOutputNames": artifact.metadata["stateOutputNames"],
            "sequencePolicy": artifact.metadata["sequencePolicy"],
            "materialization": artifact.metadata["materialization"],
            "exportAttentionImplementation": artifact.metadata[
                "exportAttentionImplementation"],
            "tensorContracts": artifact.metadata["tensorContracts"],
        })
    (Path(output_dir) / f"{model_family}-onnx-service-manifest.json").write_text(
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
    stateful: bool = False,
    model_family: str = "qwen",
    target_service: str | None = None,
) -> SplitterOutput:
    target_service = target_service or SERVICE
    services: list[SplitServiceSpec] = []
    for service in splitter.services:
        if service.name != target_service:
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
                stateful=stateful,
                model_family=model_family,
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
                "modelFamily": model_family,
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
    model_family: str = "qwen",
    expected_model_name: str | None = None,
    expected_model_revision: str | None = None,
    expected_stage_count: int | None = None,
) -> list[SplitArtifact]:
    """Bind reviewed Qwen metadata to a verified read-only Spec 107 store."""

    model_family = str(model_family or "qwen").strip().lower()
    if model_family not in {"qwen", "llama"}:
        raise ValueError(f"unsupported ONNX model family: {model_family}")
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
    service_eos = service_manifest.get("eosTokenIds")
    runtime_rows = runtime_manifest.get("stages")
    if (
        not isinstance(store_rows, list) or len(store_rows) != len(roles)
        or not isinstance(service_rows, list) or len(service_rows) != len(roles)
        or service_manifest.get("schema") != f"ndnsf-di-{model_family}-onnx-service-manifest-v1"
        or service_manifest.get("modelFamily") != model_family
        or int(service_manifest.get("stageCount", 0)) != len(roles)
        or not isinstance(service_eos, list) or not service_eos
        or any(isinstance(item, bool) or not isinstance(item, int) or item < 0
               for item in service_eos)
    ):
        raise RuntimeError(
            "reusable ONNX manifests must contain exactly the requested stage count")
    expected_runtime_schema = f"ndnsf-di-{model_family}-onnx-pipeline-runtime-v1"
    if (runtime_manifest.get("schema") != expected_runtime_schema or
            runtime_manifest.get("modelFamily") != model_family):
        raise RuntimeError("reusable ONNX runtime manifest family/schema mismatch")
    runtime_eos = runtime_manifest.get("eosTokenIds")
    if (not isinstance(runtime_eos, list) or not runtime_eos or
            any(isinstance(item, bool) or not isinstance(item, int) or item < 0
                for item in runtime_eos)):
        raise RuntimeError("reusable ONNX runtime manifest EOS contract is invalid")
    if service_eos != runtime_eos:
        raise RuntimeError("reusable ONNX manifest EOS contracts disagree")
    if (isinstance(runtime_rows, bool) or not isinstance(runtime_rows, int) or
            runtime_rows != len(roles)):
        raise RuntimeError("reusable ONNX runtime stage count does not match roles")
    service_model = str(service_manifest.get("model", ""))
    runtime_model = str(runtime_manifest.get("model", ""))
    service_revision = str(service_manifest.get("modelRevision", ""))
    runtime_revision = str(runtime_manifest.get("modelRevision", ""))
    if (not service_model or service_model != runtime_model or
            service_revision != runtime_revision):
        raise RuntimeError("reusable ONNX model identity manifests disagree")
    if expected_model_name is not None and service_model != expected_model_name:
        raise RuntimeError("reusable ONNX model does not match requested checkpoint")
    # ``main`` is resolved to a commit hash by the loader.  Enforce explicit
    # immutable revisions while accepting the documented symbolic default.
    if (expected_model_revision not in (None, "", "main") and
            service_revision != expected_model_revision):
        raise RuntimeError("reusable ONNX revision does not match requested revision")
    if (expected_stage_count is not None and
            len(roles) != int(expected_stage_count)):
        raise RuntimeError("reusable ONNX stage count does not match requested policy")
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
            artifact_name=f"/Model/LLM/Pipeline/{model_family.title()}Onnx/{role.strip('/')}",
            filename=artifact_path.name,
            kind="onnx-model",
            backend="onnxruntime",
            metadata={
                "runtime": QWEN_ONNX_RUNTIME,
                "stageIndex": int(metadata_row["stageIndex"]),
                "stageCount": len(roles),
                "layerCount": int(service_manifest.get("layerCount", 0)),
                "layerRange": dict(metadata_row["layerRange"]),
                "modelFamily": model_family,
                "modelFormat": "onnx",
                "runtimeBackend": "onnxruntime",
                "materialization": "exporter-contract-only",
                "inputNames": list(metadata_row["inputNames"]),
                "outputNames": list(metadata_row["outputNames"]),
                "cacheInputs": list(metadata_row["cacheInputs"]),
                "cacheOutputs": list(metadata_row["cacheOutputs"]),
                "stateInputNames": list(metadata_row.get("stateInputNames", [])),
                "stateOutputNames": list(metadata_row.get("stateOutputNames", [])),
                "sequencePolicy": metadata_row.get(
                    "sequencePolicy", service_manifest.get(
                        "sequencePolicy", "dynamic-past-key-v1")),
                "tensorContracts": dict(metadata_row["tensorContracts"]),
            },
        ))
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    rebound_manifest = dict(service_manifest)
    rebound_manifest["stages"] = rebound_rows
    (output / f"{model_family}-onnx-service-manifest.json").write_text(
        json.dumps(rebound_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    (output / f"{model_family}-pipeline-runtime.json").write_text(
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
    model_family: str = "qwen",
    target_service: str | None = None,
    expected_model_name: str | None = None,
    expected_model_revision: str | None = None,
    expected_stage_count: int | None = None,
) -> SplitterOutput:
    target_service = target_service or SERVICE
    services: list[SplitServiceSpec] = []
    for service in splitter.services:
        if service.name != target_service:
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
                model_family=model_family,
                expected_model_name=expected_model_name,
                expected_model_revision=expected_model_revision,
                expected_stage_count=expected_stage_count,
            ),
            input_schema=dict(service.input_schema),
            output_schema=dict(service.output_schema),
            users=list(service.users),
            providers=list(service.providers),
            metadata={
                **dict(service.metadata),
                "execution_implemented": True,
                "runtime": QWEN_ONNX_RUNTIME,
                "modelFamily": model_family,
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
        # SmolLM2 uses the standard Llama decoder contract.  Keep this
        # branch explicit so a real SmolLM2 stage cannot silently fall back to
        # the Qwen fixture path.
        "llama": (
            "transformers.models.llama.configuration_llama",
            "transformers.models.llama.modeling_llama",
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
    attn_impl = str(package.get("attnImplementation") or "sdpa")
    torch_version = tuple(
        int("".join(character for character in component
                     if character.isdigit()) or 0)
        for component in str(getattr(torch, "__version__", "0.0")).split(".")[:2]
    )
    # Select the implementation before constructing decoder layers.  Llama
    # and Qwen3 bind their attention class in DecoderLayer.__init__; changing
    # only config after construction leaves an already-created SDPA module in
    # place and does not fix Torch < 2.6 legacy ONNX export.
    if (model_type in {"qwen3", "llama"} and attn_impl == "sdpa"
            and torch_version < (2, 6)):
        attn_impl = "eager"
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
    elif model_type == "llama":
        from transformers.models.llama.configuration_llama import LlamaConfig
        from transformers.models.llama.modeling_llama import (
            LlamaDecoderLayer as DecoderLayer,
            LlamaRMSNorm as RMSNorm,
            LlamaRotaryEmbedding as RotaryEmbedding,
        )

        config = LlamaConfig.from_dict(config_dict)
    else:
        raise ValueError(
            "lightweight transformer stage only supports qwen2, qwen3, "
            "qwen3_5, or llama, got "
            f"{model_type}"
        )
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


_QWEN_STATEFUL_SEQUENCE_POLICY = "stateful-prefill-decode-v1"
_QWEN_STATE_FAMILIES = (
    "attention_kv", "recurrent_state", "convolution_state")
_QWEN_PREFIX_TOKEN_COUNT = "__ndnsf_prefix_token_count__"


def _qwen_stateful_names(metadata: dict[str, Any]) -> tuple[
        tuple[str, ...], tuple[str, ...]]:
    inputs = tuple(str(value) for value in metadata.get(
        "stateInputNames", ()))
    outputs = tuple(str(value) for value in metadata.get(
        "stateOutputNames", ()))
    required_inputs = tuple(f"{family}_in" for family in _QWEN_STATE_FAMILIES)
    required_outputs = tuple(f"{family}_out" for family in _QWEN_STATE_FAMILIES)
    if (metadata.get("sequencePolicy") != _QWEN_STATEFUL_SEQUENCE_POLICY
            or any(value not in inputs for value in required_inputs)
            or any(value not in outputs for value in required_outputs)
            or len(inputs) != len(outputs)):
        raise ValueError("QWEN_ONNX_STATEFUL_SEQUENCE_CONTRACT_REQUIRED")
    return inputs, outputs


def _qwen_initial_state(item: Any, contract: Mapping[str, Any], *,
                        batch: int, np: Any) -> Any:
    declared = list(contract.get("initialShape", ()))
    if not declared:
        declared = list(getattr(item, "shape", ()))
    shape: list[int] = []
    for index, dimension in enumerate(declared):
        if isinstance(dimension, int) and dimension >= 0:
            shape.append(int(dimension))
        elif index == 0:
            shape.append(int(batch))
        elif any(marker in str(dimension).lower()
                 for marker in ("past", "cache", "sequence", "seq")):
            shape.append(0)
        else:
            raise ValueError(
                f"state input {item.name} requires tensorContracts.initialShape")
    if not shape:
        raise ValueError(f"state input {item.name} has no initial shape")
    return np.zeros(
        tuple(shape),
        dtype=_onnx_input_numpy_dtype(
            getattr(item, "type", ""), np.float32),
    )


def _qwen_cuda_device_id(metadata: Mapping[str, Any]) -> int:
    value = str(metadata.get("deviceId", "0") or "0").strip().lower()
    if value.startswith("cuda:"):
        value = value.split(":", 1)[1]
    try:
        device_id = int(value)
    except ValueError as exc:
        raise ValueError("Qwen ONNX deviceId must be a CUDA device index") from exc
    if device_id < 0:
        raise ValueError("Qwen ONNX deviceId must be non-negative")
    return device_id


def _qwen_is_device_ortvalue(value: Any) -> bool:
    device_name = getattr(value, "device_name", None)
    if not callable(device_name):
        return False
    return str(device_name()).lower() != "cpu"


def _qwen_run_stateful_cuda_bound(
        session: Any, feed: Mapping[str, Any], output_names: tuple[str, ...],
        state_inputs: tuple[str, ...], state_outputs: tuple[str, ...],
        *, device_id: int) -> dict[str, Any]:
    """Execute one state transition while retaining opaque state OrtValues.

    Application inputs and inter-Provider activations intentionally originate
    on the host because they cross the NDN boundary.  Only the three declared
    model-state families are rebound as CUDA OrtValues and allocated back onto
    CUDA.  Primary activation/logit outputs are bound to CPU because those are
    serialized onto NDN or sampled by the final Provider.
    """

    binding = session.io_binding()
    for name, value in feed.items():
        if name in state_inputs and _qwen_is_device_ortvalue(value):
            binding.bind_ortvalue_input(name, value)
        else:
            binding.bind_cpu_input(name, value)
    state_output_set = set(state_outputs)
    for name in output_names:
        if name in state_output_set:
            binding.bind_output(name, device_type="cuda", device_id=device_id)
        else:
            binding.bind_output(name, device_type="cpu", device_id=0)
    session.run_with_iobinding(binding)
    values = list(binding.get_outputs())
    if len(values) != len(output_names):
        raise ValueError(
            "Qwen ONNX I/O binding output metadata/value count mismatch: "
            f"{len(output_names)} != {len(values)}")
    outputs: dict[str, Any] = {}
    for name, value in zip(output_names, values):
        outputs[name] = value if name in state_output_set else value.numpy()
    return outputs


def run_qwen_onnx_stage(
    input_payload: bytes,
    *,
    role: str,
    stages: int,
    session: Any,
    metadata: dict[str, Any],
    compute_delay_ms: float = 0.0,
    timing: dict[str, float | int | str] | None = None,
    state: dict[str, Any] | None = None,
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
    model_type = str(metadata.get("modelType", "qwen2"))
    if model_type == "qwen3_5_text":
        model_type = "qwen3_5"
    layer_range = dict(metadata.get("layerRange", {}) or {})
    start = int(layer_range.get("start", 0))
    end = int(layer_range.get("endExclusive", 0))
    stage_count = int(metadata.get("stageCount", stages))
    context_length = int(metadata.get("contextLength", 0) or 0)
    stateful = metadata.get("sequencePolicy") == _QWEN_STATEFUL_SEQUENCE_POLICY
    if stateful and state is None:
        raise ValueError("stateful Qwen ONNX execution requires Provider state")
    state_inputs: tuple[str, ...] = ()
    state_outputs: tuple[str, ...] = ()
    if stateful:
        state_inputs, state_outputs = _qwen_stateful_names(metadata)
    pad_token_id = metadata.get("padTokenId")
    if context_length < 0:
        raise ValueError("Qwen ONNX contextLength must be non-negative")
    if (context_length and not stateful
            and (not isinstance(pad_token_id, int) or pad_token_id < 0)):
        raise ValueError("Qwen ONNX fixed-context metadata lacks padTokenId")
    hidden_size = 0
    request_id = ""

    decode_start = time.perf_counter()
    if stage_index == 0:
        try:
            input_doc = decode_qwen_pipeline_context(input_payload)
            full_input_ids = np.asarray(input_doc["inputIds"], dtype=np.int64)
            full_attention_mask = np.asarray(
                input_doc["attentionMask"], dtype=np.int64)
            request_id = str(input_doc.get("requestId", ""))
            session_id = str(input_doc.get("sessionId", ""))
            context_epoch = int(input_doc.get("contextEpoch", 0) or 0)
            if stateful:
                prefix_count = int((state or {}).get(
                    _QWEN_PREFIX_TOKEN_COUNT, 0) or 0)
                active_length = int(full_attention_mask[0].sum())
                if prefix_count < 0 or prefix_count >= active_length:
                    raise ValueError(
                        "Qwen ONNX state prefix does not precede new input")
                input_ids = full_input_ids[:, prefix_count:active_length]
                attention_mask = full_attention_mask[:, :active_length]
                position_rows = np.arange(
                    prefix_count, active_length, dtype=np.int64)
                if model_type == "qwen3_5":
                    position_ids = np.broadcast_to(
                        position_rows.reshape(1, 1, -1),
                        (4, int(input_ids.shape[0]), int(input_ids.shape[1])),
                    )
                else:
                    position_ids = position_rows.reshape(1, -1)
            else:
                input_ids = full_input_ids
                attention_mask = full_attention_mask
                position_ids = np.asarray(
                    input_doc["positionIds"], dtype=np.int64)
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
            position_ids = np.arange(
                input_ids.shape[1], dtype=np.int64).reshape(1, -1)
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

    # Legacy fixed-context graphs are padded to their export shape. Stateful
    # Spec175 graphs instead receive the full attention prefix plus only the
    # newly appended token chunk, exactly like the frozen G5 CUDA oracle.
    # Padding a stateful graph would repeat the prefix and invalidate its KV/
    # recurrent/convolution state transition.
    #
    # Qwen3.5's exported hybrid linear-attention graph contains Python shape
    # branches (chunk padding and chunk count).  Dynamic ONNX axes do not
    # make those branches dynamic: exporting a 20-token sample specializes a
    # 64-token padded chunk, then a 21-token request fails inside the gated
    # norm.  The Spec175 manifest therefore declares one fixed context window
    # and the runtime pads its active prefix with an explicit attention mask.
    # This keeps every stage's internal shape stable while preserving the
    # logits at the last active token.
    active_length = int(attention_mask[0].sum()) if attention_mask.size else 0
    if context_length and stateful:
        if active_length <= 0 or active_length > context_length:
            raise ValueError(
                "Qwen ONNX active context exceeds declared contextLength")
        if input_ids.ndim != 2 or input_ids.shape[1] <= 0:
            raise ValueError("Qwen ONNX stateful token chunk is empty")
    elif context_length:
        if input_ids.ndim != 2 or attention_mask.ndim != 2:
            raise ValueError("Qwen ONNX fixed-context tensors must be rank 2")
        if input_ids.shape[0] != attention_mask.shape[0]:
            raise ValueError("Qwen ONNX input/mask batch mismatch")
        if active_length <= 0 or active_length > context_length:
            raise ValueError(
                "Qwen ONNX active context exceeds declared contextLength")
        if input_ids.shape[1] > context_length:
            raise ValueError(
                "Qwen ONNX input sequence exceeds declared contextLength")
        padded_ids = np.full(
            (int(input_ids.shape[0]), context_length),
            int(pad_token_id), dtype=np.int64)
        padded_mask = np.zeros_like(padded_ids, dtype=np.int64)
        padded_ids[:, :input_ids.shape[1]] = input_ids
        padded_mask[:, :attention_mask.shape[1]] = attention_mask
        input_ids = padded_ids
        attention_mask = padded_mask
        if hidden_states.shape[1] != context_length:
            padded_hidden = np.zeros(
                (int(hidden_states.shape[0]), context_length,
                 int(hidden_states.shape[2])),
                dtype=hidden_states.dtype)
            padded_hidden[:, :hidden_states.shape[1], :] = hidden_states
            hidden_states = padded_hidden
        position_rows = np.arange(context_length, dtype=np.int64)
        if model_type == "qwen3_5":
            position_ids = np.broadcast_to(
                position_rows.reshape(1, 1, -1),
                (4, int(input_ids.shape[0]), context_length),
            )
        else:
            position_ids = position_rows.reshape(1, -1)
    elif active_length <= 0:
        raise ValueError("Qwen ONNX attention mask has no active token")
    record("decode_ms", (time.perf_counter() - decode_start) * 1000.0)
    record("embed_ms", 0.0)
    if expected_start != start:
        raise ValueError(
            f"stage {stage_index} expected layer {start}, got {expected_start}")
    record("request_id", request_id)
    run_start = time.perf_counter()
    available_inputs = {item.name for item in session.get_inputs()}
    input_types = {
        item.name: getattr(item, "type", "")
        for item in session.get_inputs()
    }
    feed = {}
    if "input_ids" in available_inputs:
        feed["input_ids"] = input_ids
    if "hidden_states" in available_inputs:
        hidden_type = str(input_types.get("hidden_states", ""))
        hidden_dtype = _onnx_input_numpy_dtype(hidden_type, np.float32)
        feed["hidden_states"] = hidden_states.astype(
            hidden_dtype, copy=False)
    if "position_ids" in available_inputs:
        if model_type == "qwen3_5" and position_ids.ndim == 2:
            position_ids = np.broadcast_to(
                position_ids.reshape(1, *position_ids.shape),
                (4, int(position_ids.shape[0]), int(position_ids.shape[1])),
            )
        feed["position_ids"] = position_ids
    if "attention_mask" in available_inputs:
        feed["attention_mask"] = attention_mask
    contracts = dict(metadata.get("tensorContracts", {}) or {})
    for item in session.get_inputs():
        if item.name in feed:
            continue
        if stateful and item.name in state_inputs:
            value = (state or {}).get(item.name)
            if value is None:
                value = _qwen_initial_state(
                    item, dict(contracts.get(item.name, {}) or {}),
                    batch=int(input_ids.shape[0]), np=np)
            if _qwen_is_device_ortvalue(value):
                feed[item.name] = value
            else:
                feed[item.name] = np.asarray(value).astype(
                    _onnx_input_numpy_dtype(
                        getattr(item, "type", ""), np.float32),
                    copy=False,
                )
            continue
        if item.name == "cache_position":
            feed[item.name] = np.arange(
                active_length - int(input_ids.shape[1]), active_length,
                dtype=_onnx_input_numpy_dtype(
                    getattr(item, "type", ""), np.int64),
            )
            continue
        if item.name.startswith(("past_key.", "past_value.")):
            shape = item.shape
            if len(shape) != 4 or not isinstance(shape[1], int) or not isinstance(shape[3], int):
                raise ValueError(f"unsupported Qwen KV input shape for {item.name}: {shape}")
            feed[item.name] = np.empty(
                (int(input_ids.shape[0]), int(shape[1]), 0, int(shape[3])),
                dtype=_onnx_input_numpy_dtype(getattr(item, "type", ""), np.float32),
            )
            continue
        raise ValueError(f"unrecognized Qwen ONNX input: {item.name}")
    output_names = tuple(item.name for item in session.get_outputs())
    providers = tuple(session.get_providers()) \
        if callable(getattr(session, "get_providers", None)) else ()
    use_cuda_state_binding = bool(
        stateful
        and providers
        and providers[0] == "CUDAExecutionProvider"
        and callable(getattr(session, "io_binding", None))
        and callable(getattr(session, "run_with_iobinding", None)))
    if use_cuda_state_binding:
        outputs = _qwen_run_stateful_cuda_bound(
            session, feed, output_names, state_inputs, state_outputs,
            device_id=_qwen_cuda_device_id(metadata))
        record("state_storage", "device")
        record("state_host_round_trip_bytes", 0)
    else:
        output_values = session.run(None, feed)
        if len(output_names) != len(output_values):
            raise ValueError(
                "Qwen ONNX output metadata/value count mismatch: "
                f"{len(output_names)} != {len(output_values)}")
        outputs = dict(zip(output_names, output_values))
        if stateful:
            record("state_storage", "host")
            record("state_host_round_trip_bytes", sum(
                int(np.asarray(outputs[name]).nbytes)
                for name in state_outputs if name in outputs))
    if stateful:
        assert state is not None
        for input_name, output_name in zip(state_inputs, state_outputs):
            if output_name not in outputs:
                raise ValueError(
                    f"Qwen ONNX stage omitted state output: {output_name}")
            state[input_name] = outputs[output_name]
        state[_QWEN_PREFIX_TOKEN_COUNT] = int(active_length)
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
    # Stateful decode logits have the token-chunk sequence dimension, not the
    # full attention-prefix length.
    logit_index = -1 if stateful else active_length - 1
    top_token = int(np.argmax(logits[:, logit_index, :], axis=-1)[0])
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


def run_tiny_onnx_stage(
    input_payload: bytes,
    *,
    role: str,
    stages: int,
    session: Any,
    state_cache: dict[tuple[str, int], dict[str, Any]],
    request_id: str,
    compute_delay_ms: float = 0.0,
) -> bytes:
    """Run one stateful step of the checked-in Spec175 tiny ONNX graph."""
    import base64
    import io
    import numpy as np
    from ndnsf_distributed_inference.adapters.qwen.stateful_onnx import (
        DecodeStateContractError,
        StatefulOnnxIOContractV1,
    )

    stage_index = role_index(role)
    if stage_index >= int(stages):
        raise ValueError("tiny ONNX role is outside the requested stage count")
    if compute_delay_ms > 0:
        time.sleep(float(compute_delay_ms) / 1000.0)
    key = (str(request_id), int(stage_index))
    state = state_cache.setdefault(key, {})
    # The tiny fixture is the CPU executable of the same stateful contract
    # used by the native Provider.  Validate the graph signature on every
    # direct adapter call as well as on the long-lived Provider session.  This
    # prevents an orphaned ``*_out`` tensor or a missing recurrent state from
    # being silently treated as a valid next-token transition.
    raw_session = getattr(session, "session", session)
    contract = getattr(session, "contract", None)
    if contract is None:
        contract = StatefulOnnxIOContractV1.from_session(raw_session)
    else:
        contract.validate()
    inputs = {value.name: value for value in raw_session.get_inputs()}
    feed: dict[str, Any] = {}
    if stage_index == 0:
        context = decode_qwen_pipeline_context(input_payload)
        rows = context.get("inputIds")
        if not isinstance(rows, list) or not rows:
            raise ValueError("tiny ONNX context has no inputIds")
        token = int((rows[0] if isinstance(rows[0], list) else rows)[-1])
        feed["input_ids"] = np.asarray([[token]], dtype=np.int64)
    else:
        doc = json.loads(bytes(input_payload).decode("utf-8"))
        if doc.get("schema") != "ndnsf-di-tiny-onnx-hidden-v1":
            raise ValueError("tiny ONNX stage received an invalid hidden payload")
        raw = base64.b64decode(str(doc["hidden"]).encode("ascii"), validate=True)
        with io.BytesIO(raw) as stream:
            hidden = np.load(stream, allow_pickle=False)
        feed["hidden_in"] = np.asarray(hidden, dtype=np.float32)
    for name in contract.state_input_names:
        value = inputs[name]
        if name != "hidden_in":
            if name not in state:
                shape = tuple(int(d) for d in value.shape)
                if any(d <= 0 for d in shape):
                    raise ValueError(f"tiny ONNX state shape is not static: {name}")
                state[name] = np.zeros(shape, dtype=np.float32)
            feed[name] = state[name]
    names = list(contract.output_names)
    if hasattr(session, "run") and hasattr(session, "contract"):
        outputs = session.run(feed, incremental=bool(state))
    else:
        values = raw_session.run(names, feed)
        if len(values) != len(names):
            raise DecodeStateContractError(
                "stateful ONNX output count does not match the contract")
        outputs = dict(zip(names, values))
    for name in contract.state_output_names:
        value = outputs.get(name)
        if value is None:
            raise DecodeStateContractError(
                "stateful ONNX output is missing: " + name)
        state[name[:-4] + "_in"] = np.asarray(value)
    if stage_index == int(stages) - 1:
        logits = outputs.get("logits")
        if logits is None:
            raise ValueError("tiny ONNX final role did not produce logits")
        token = int(np.argmax(np.asarray(logits)[0, -1]))
        return json.dumps({
            "schema": "ndnsf-di-tiny-onnx-response-v1",
            "runtime": TINY_ONNX_RUNTIME,
            "topToken": token,
            "stageIndex": stage_index,
            "stageCount": int(stages),
        }, sort_keys=True, separators=(",", ":")).encode("utf-8")
    hidden = outputs.get("hidden_out")
    if hidden is None:
        raise ValueError("tiny ONNX non-final role did not produce hidden_out")
    with io.BytesIO() as stream:
        np.save(stream, np.asarray(hidden, dtype=np.float32), allow_pickle=False)
        encoded = base64.b64encode(stream.getvalue()).decode("ascii")
    return json.dumps({
        "schema": "ndnsf-di-tiny-onnx-hidden-v1",
        "runtime": TINY_ONNX_RUNTIME,
        "requestId": str(request_id),
        "stageIndex": stage_index,
        "stageCount": int(stages),
        "hidden": encoded,
    }, sort_keys=True, separators=(",", ":")).encode("utf-8")


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
    qwen_stateful: bool = False,
    qwen_model_family: str = "qwen",
    qwen_content_store: str = "",
    qwen_artifact_store: str = "",
    qwen_service_manifest: str = "",
    qwen_runtime_manifest: str = "",
    tiny_onnx_fixture_root: str = "",
) -> Path:
    if qwen_model_family not in {"qwen", "llama"}:
        raise ValueError(f"unsupported ONNX model family: {qwen_model_family}")
    if qwen_model_family == "llama":
        policy_models = {
            "HuggingFaceTB/SmolLM2-135M": "/Model/SmolLM2/135M",
            "HuggingFaceTB/SmolLM2-360M": "/Model/SmolLM2/360M",
        }
        if qwen_model not in policy_models or model != policy_models[qwen_model]:
            raise ValueError(
                "llama policy must bind the supported SmolLM2 checkpoint and URI: "
                f"checkpoint={qwen_model} model={model}")
    elif "smollm" in qwen_model.lower() or str(model).startswith("/Model/SmolLM2/"):
        raise ValueError("qwen policy cannot bind a SmolLM2 checkpoint or model URI")
    output_dir = Path(path).parent
    request = llm_planner_request(
        planner_kind=PlannerKind.LLM_PIPELINE,
        model_path=model,
        output_dir=output_dir,
        model_format=("onnx" if runtime == TINY_ONNX_RUNTIME else "custom"),
        runtime_backend=("onnxruntime" if runtime == TINY_ONNX_RUNTIME else "custom"),
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
    elif runtime == TINY_ONNX_RUNTIME:
        if not tiny_onnx_fixture_root:
            raise ValueError("tiny-onnx runtime requires --tiny-onnx-fixture-root")
        splitter = with_tiny_onnx_artifacts(
            splitter,
            fixture_root=tiny_onnx_fixture_root,
            stages=stages,
        )
    elif runtime == QWEN_TRANSFORMERS_RUNTIME:
        if qwen_model_family != "qwen":
            raise ValueError(
                "llama family requires the ONNX exporter; the maintained "
                "transformers path is Qwen-only")
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
        if qwen_model_family == "qwen" and "smollm" in qwen_model.lower():
            raise ValueError("qwen family cannot bind a SmolLM2 checkpoint")
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
                model_family=qwen_model_family,
                target_service=service,
                expected_model_name=qwen_model,
                expected_model_revision=qwen_revision,
                expected_stage_count=stages,
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
                stateful=qwen_stateful,
                model_family=qwen_model_family,
                target_service=service,
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
