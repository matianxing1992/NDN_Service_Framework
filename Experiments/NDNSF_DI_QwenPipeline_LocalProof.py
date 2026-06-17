#!/usr/bin/env python3
"""Strict local proof for a real Qwen/HF transformer pipeline split.

The proof loads a HuggingFace decoder-only model, exports one stage-weight
package per contiguous layer range, executes those packages stage by stage, and
compares the final logits with a normal full-model forward pass.

This is intentionally local. It proves the model-splitting runtime invariant
before MiniNDN distribution: per-stage artifacts plus hidden-state exchange can
produce the same next-token result as the original model.
"""

from __future__ import annotations

import argparse
import inspect
import json
import sys
import time
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Any


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "examples/python/NDNSF-DistributedInference/llm_pipeline"))

from llm_pipeline_lib import split_layer_ranges  # noqa: E402


OK_MARKER = "NDNSF_DI_QWEN_PIPELINE_PROOF_OK"
SKIP_MARKER = "NDNSF_DI_QWEN_PIPELINE_PROOF_SKIPPED"
FAIL_MARKER = "NDNSF_DI_QWEN_PIPELINE_PROOF_FAILED"


@dataclass(frozen=True)
class StagePackage:
    index: int
    start: int
    end: int
    path: Path


def _call_with_supported_kwargs(fn: Any, **kwargs: Any) -> Any:
    signature = inspect.signature(fn)
    return fn(**{
        name: value
        for name, value in kwargs.items()
        if name in signature.parameters
    })


def _position_ids_like(input_ids: Any) -> Any:
    import torch

    return torch.arange(
        int(input_ids.shape[1]),
        device=input_ids.device,
        dtype=torch.long,
    ).unsqueeze(0)


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


def _rotary_embeddings(base_model: Any, hidden_states: Any, position_ids: Any) -> Any:
    rotary = getattr(base_model, "rotary_emb", None)
    if rotary is None:
        return None
    try:
        return rotary(hidden_states, position_ids)
    except TypeError:
        return None


def _run_layer(layer: Any, hidden_states: Any, *,
               attention_mask: Any,
               position_ids: Any,
               position_embeddings: Any) -> Any:
    output = _call_with_supported_kwargs(
        layer.forward,
        hidden_states=hidden_states,
        attention_mask=attention_mask,
        position_ids=position_ids,
        past_key_value=None,
        output_attentions=False,
        use_cache=False,
        cache_position=position_ids[0],
        position_embeddings=position_embeddings,
    )
    if isinstance(output, tuple):
        return output[0]
    return output


def _validate_model_shape(model: Any) -> None:
    base = getattr(model, "model", None)
    layers = list(getattr(base, "layers", [])) if base is not None else []
    if base is None or not layers:
        raise RuntimeError("expected a Qwen/Llama-style model.model.layers decoder")
    if getattr(base, "embed_tokens", None) is None:
        raise RuntimeError("model.model.embed_tokens is required")
    if getattr(base, "norm", None) is None:
        raise RuntimeError("model.model.norm is required")
    if getattr(model, "lm_head", None) is None:
        raise RuntimeError("model.lm_head is required")


def _stage_state_dict(full_state: dict[str, Any], *,
                      stage_index: int,
                      stage_count: int,
                      start: int,
                      end: int) -> dict[str, Any]:
    prefixes = [f"model.layers.{index}." for index in range(start, end)]
    if stage_index == 0:
        prefixes.append("model.embed_tokens.")
    if stage_index == stage_count - 1:
        prefixes.extend(["model.norm.", "lm_head."])
    return {
        key: value.detach().cpu()
        for key, value in full_state.items()
        if any(key.startswith(prefix) for prefix in prefixes)
    }


def export_stage_packages(model: Any, output_dir: Path, *,
                          stages: int) -> list[StagePackage]:
    import torch

    _validate_model_shape(model)
    output_dir.mkdir(parents=True, exist_ok=True)
    layers = list(model.model.layers)
    ranges = split_layer_ranges(len(layers), stages)
    full_state = model.state_dict()
    packages: list[StagePackage] = []
    for index, (start, end) in enumerate(ranges):
        path = output_dir / f"qwen-stage-{index}.pt"
        package = {
            "schema": "ndnsf-di-qwen-stage-weights-v1",
            "stageIndex": index,
            "stageCount": stages,
            "layerRange": {"start": start, "endExclusive": end},
            "config": model.config.to_dict(),
            "state_dict": _stage_state_dict(
                full_state,
                stage_index=index,
                stage_count=stages,
                start=start,
                end=end,
            ),
        }
        torch.save(package, path)
        packages.append(StagePackage(index=index, start=start, end=end, path=path))
    return packages


def _load_package(path: Path) -> dict[str, Any]:
    import torch

    try:
        return torch.load(path, map_location="cpu", weights_only=True)
    except TypeError:
        return torch.load(path, map_location="cpu")


def _load_stage_model(model_cls: Any, config: Any, package: dict[str, Any]) -> Any:
    model = model_cls(config)
    model.load_state_dict(package.get("state_dict", {}), strict=False)
    model.eval()
    return model


def _encode_hidden_state(*, hidden_states: Any, input_ids: Any, next_layer: int) -> bytes:
    import torch

    buffer = BytesIO()
    torch.save({
        "schema": "ndnsf-di-qwen-hidden-state-v1",
        "hidden_states": hidden_states.cpu(),
        "input_ids": input_ids.cpu(),
        "next_layer": int(next_layer),
    }, buffer)
    return buffer.getvalue()


def _decode_hidden_state(payload: bytes) -> dict[str, Any]:
    import torch

    try:
        return torch.load(BytesIO(payload), map_location="cpu", weights_only=True)
    except TypeError:
        return torch.load(BytesIO(payload), map_location="cpu")


def run_stage_package(model_cls: Any, config: Any, package: dict[str, Any],
                      input_payload: bytes | None,
                      input_ids: Any | None) -> bytes | Any:
    import torch

    stage_index = int(package["stageIndex"])
    stage_count = int(package["stageCount"])
    start = int(package["layerRange"]["start"])
    end = int(package["layerRange"]["endExclusive"])
    model = _load_stage_model(model_cls, config, package)
    base = model.model
    position_ids = None

    with torch.no_grad():
        if stage_index == 0:
            if input_ids is None:
                raise RuntimeError("stage 0 requires input_ids")
            hidden_states = base.embed_tokens(input_ids)
            expected_start = 0
        else:
            if input_payload is None:
                raise RuntimeError(f"stage {stage_index} requires hidden-state input")
            incoming = _decode_hidden_state(input_payload)
            if incoming.get("schema") != "ndnsf-di-qwen-hidden-state-v1":
                raise RuntimeError("unexpected hidden-state payload schema")
            input_ids = incoming["input_ids"]
            hidden_states = incoming["hidden_states"]
            expected_start = int(incoming["next_layer"])
        if expected_start != start:
            raise RuntimeError(f"stage {stage_index} expected layer {start}, got {expected_start}")
        position_ids = _position_ids_like(input_ids)
        attention_mask = _build_causal_mask(base, input_ids, hidden_states)
        for layer in list(base.layers)[start:end]:
            hidden_states = _run_layer(
                layer,
                hidden_states,
                attention_mask=attention_mask,
                position_ids=position_ids,
                position_embeddings=_rotary_embeddings(base, hidden_states, position_ids),
            )
        if stage_index < stage_count - 1:
            return _encode_hidden_state(
                hidden_states=hidden_states,
                input_ids=input_ids,
                next_layer=end,
            )
        return model.lm_head(base.norm(hidden_states))


def run_artifact_pipeline(model: Any, packages: list[StagePackage], input_ids: Any) -> Any:
    payload: bytes | None = None
    logits = None
    for package_ref in packages:
        package = _load_package(package_ref.path)
        output = run_stage_package(
            model.__class__,
            model.config,
            package,
            input_payload=payload,
            input_ids=input_ids if package_ref.index == 0 else None,
        )
        if isinstance(output, bytes):
            payload = output
        else:
            logits = output
    if logits is None:
        raise RuntimeError("artifact pipeline did not produce logits")
    return logits


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model",
        default="Qwen/Qwen2.5-0.5B-Instruct",
        help="Local HuggingFace path or model id for the Qwen proof.",
    )
    parser.add_argument("--prompt", default="Explain NDNSF in one sentence.")
    parser.add_argument("--stages", type=int, default=3)
    parser.add_argument("--output-dir", default="results/qwen_pipeline_local_proof_latest")
    parser.add_argument("--allow-download", action="store_true")
    parser.add_argument("--max-diff", type=float, default=5e-3)
    parser.add_argument("--dtype", choices=("float32", "auto"), default="float32")
    args = parser.parse_args()

    try:
        import torch
        from transformers import AutoModelForCausalLM, AutoTokenizer
    except Exception as exc:  # noqa: BLE001
        print(SKIP_MARKER, f"reason=missing-dependency detail={type(exc).__name__}:{exc}")
        return 0

    output_dir = Path(args.output_dir).expanduser().resolve()
    stage_dir = output_dir / "qwen-stage-artifacts"
    output_dir.mkdir(parents=True, exist_ok=True)
    local_files_only = not args.allow_download
    dtype = torch.float32 if args.dtype == "float32" else "auto"

    started_load = time.perf_counter()
    try:
        tokenizer = AutoTokenizer.from_pretrained(
            args.model,
            local_files_only=local_files_only,
            trust_remote_code=True,
        )
        model = AutoModelForCausalLM.from_pretrained(
            args.model,
            local_files_only=local_files_only,
            trust_remote_code=True,
            torch_dtype=dtype,
        )
    except Exception as exc:  # noqa: BLE001
        if not args.allow_download:
            print(SKIP_MARKER, f"reason=model-not-local model={args.model} detail={type(exc).__name__}:{exc}")
            return 0
        raise
    model.eval()
    _validate_model_shape(model)
    load_ms = (time.perf_counter() - started_load) * 1000.0

    input_ids = tokenizer(args.prompt, return_tensors="pt")["input_ids"]
    with torch.no_grad():
        started_full = time.perf_counter()
        full_logits = model(input_ids=input_ids, use_cache=False).logits
        full_ms = (time.perf_counter() - started_full) * 1000.0

    started_export = time.perf_counter()
    packages = export_stage_packages(model, stage_dir, stages=args.stages)
    export_ms = (time.perf_counter() - started_export) * 1000.0

    started_pipeline = time.perf_counter()
    artifact_logits = run_artifact_pipeline(model, packages, input_ids)
    artifact_ms = (time.perf_counter() - started_pipeline) * 1000.0

    diff = torch.max(torch.abs(full_logits[:, -1, :] - artifact_logits[:, -1, :])).item()
    full_top = int(torch.argmax(full_logits[:, -1, :], dim=-1).item())
    artifact_top = int(torch.argmax(artifact_logits[:, -1, :], dim=-1).item())
    summary = {
        "model": args.model,
        "prompt": args.prompt,
        "stages": args.stages,
        "layerRanges": [[item.start, item.end] for item in packages],
        "loadMs": load_ms,
        "fullMs": full_ms,
        "exportMs": export_ms,
        "artifactPipelineMs": artifact_ms,
        "maxDiff": diff,
        "fullTop": full_top,
        "artifactTop": artifact_top,
        "stageArtifacts": [str(item.path) for item in packages],
    }
    (output_dir / "qwen-pipeline-proof-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    if full_top != artifact_top or diff > args.max_diff:
        print(
            FAIL_MARKER,
            f"model={args.model}",
            f"stages={args.stages}",
            f"full_top={full_top}",
            f"artifact_top={artifact_top}",
            f"max_diff={diff:.6g}",
            f"max_allowed={args.max_diff}",
            f"summary={output_dir / 'qwen-pipeline-proof-summary.json'}",
        )
        return 2
    print(
        OK_MARKER,
        f"model={args.model}",
        f"stages={args.stages}",
        f"ranges={summary['layerRanges']}",
        f"load_ms={load_ms:.2f}",
        f"full_ms={full_ms:.2f}",
        f"export_ms={export_ms:.2f}",
        f"artifact_pipeline_ms={artifact_ms:.2f}",
        f"max_diff={diff:.6g}",
        f"top_token={full_top}",
        f"summary={output_dir / 'qwen-pipeline-proof-summary.json'}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
