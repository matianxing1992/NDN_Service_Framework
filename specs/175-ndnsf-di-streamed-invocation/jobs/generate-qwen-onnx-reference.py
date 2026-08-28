#!/usr/bin/env python3
"""Generate a deterministic greedy reference sequence from Qwen ONNX stages.

This utility is intentionally ONNX Runtime-only.  It is used after export to
create the campaign oracle; it must not import PyTorch or Transformers.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import time

import numpy as np
import onnxruntime as ort


STATE_FAMILIES = ("attention_kv", "recurrent_state", "convolution_state")
SEQUENCE_POLICY = "stateful-prefill-decode-v1"


def numpy_dtype(type_name: str):
    normalized = str(type_name or "").lower()
    if "bfloat16" in normalized:
        raise RuntimeError("QWEN_ONNX_BFLOAT16_UNSUPPORTED")
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
    raise RuntimeError(f"unsupported ONNX tensor type: {type_name}")


def integer_dimension(value, *, name: str, fallback: int | None = None) -> int:
    if isinstance(value, int) and value > 0:
        return value
    if fallback is not None:
        return fallback
    raise RuntimeError(f"missing positive dimension for {name}: {value!r}")


def make_session(path: Path, *, device_id: int, execution_provider: str):
    print(
        f"QWEN_ONNX_SESSION_BEGIN path={path.name} device={device_id} "
        f"executionProvider={execution_provider}", flush=True)
    options = ort.SessionOptions()
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    options.enable_mem_pattern = False
    # Reuse non-overlapping intermediate buffers.  The previous diagnostic
    # setting disabled reuse and made stage 2 run out of GPU memory on a
    # 32-GiB RTX 5000 while allocating a 60-MiB transpose buffer.
    options.enable_mem_reuse = True
    if execution_provider == "cuda":
        providers = [
            ("CUDAExecutionProvider", {"device_id": device_id}),
            "CPUExecutionProvider",
        ]
    else:
        providers = ["CPUExecutionProvider"]
    session = ort.InferenceSession(str(path), sess_options=options,
                                   providers=providers)
    providers = tuple(session.get_providers())
    if execution_provider == "cuda" and providers[:1] != ("CUDAExecutionProvider",):
        raise RuntimeError(f"unexpected ORT providers for {path}: {providers}")
    print(
        f"QWEN_ONNX_SESSION_READY path={path.name} device={device_id} "
        f"providers={providers}",
        flush=True,
    )
    return session


def _state_names(metadata: dict) -> tuple[tuple[str, ...], tuple[str, ...]]:
    inputs = tuple(str(value) for value in metadata.get("stateInputNames", ()))
    outputs = tuple(str(value) for value in metadata.get("stateOutputNames", ()))
    expected_inputs = tuple(f"{family}_in" for family in STATE_FAMILIES)
    expected_outputs = tuple(f"{family}_out" for family in STATE_FAMILIES)
    if (metadata.get("sequencePolicy") != SEQUENCE_POLICY
            or any(value not in inputs for value in expected_inputs)
            or any(value not in outputs for value in expected_outputs)
            or len(inputs) != len(outputs)):
        raise RuntimeError("QWEN_ONNX_STATEFUL_SEQUENCE_CONTRACT_REQUIRED")
    return inputs, outputs


def _initial_state(item, contract: dict, *, batch: int) -> np.ndarray:
    dtype = numpy_dtype(str(getattr(item, "type", "")))
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
            raise RuntimeError(
                f"state input {item.name} requires tensorContracts.initialShape")
    if not shape:
        raise RuntimeError(f"state input {item.name} has no initial shape")
    return np.zeros(tuple(shape), dtype=dtype)


def stage_feed(session, *, input_ids: np.ndarray, attention: np.ndarray,
               positions: np.ndarray, hidden: np.ndarray, metadata: dict,
               state: dict[str, np.ndarray]):
    contracts = dict(metadata.get("tensorContracts", {}))
    state_inputs, _ = _state_names(metadata)
    feed = {}
    for item in session.get_inputs():
        name = str(item.name)
        type_name = str(getattr(item, "type", ""))
        dtype = numpy_dtype(type_name)
        if name == "input_ids":
            feed[name] = input_ids.astype(dtype, copy=False)
        elif name == "attention_mask":
            feed[name] = attention.astype(dtype, copy=False)
        elif name == "position_ids":
            feed[name] = positions.astype(dtype, copy=False)
        elif name == "hidden_states":
            feed[name] = hidden.astype(dtype, copy=False)
        elif name in state_inputs:
            value = state.get(name)
            if value is None:
                value = _initial_state(
                    item, dict(contracts.get(name, {})),
                    batch=int(input_ids.shape[0]))
            feed[name] = np.asarray(value).astype(dtype, copy=False)
        else:
            raise RuntimeError(f"unrecognized Qwen ONNX input: {name}")
    return feed


def update_stage_state(session, outputs: list[np.ndarray], metadata: dict,
                       state: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    state_inputs, state_outputs = _state_names(metadata)
    output_by_name = {
        str(item.name): np.asarray(value)
        for item, value in zip(session.get_outputs(), outputs)
    }
    successor = dict(state)
    for input_name, output_name in zip(state_inputs, state_outputs):
        if output_name not in output_by_name:
            raise RuntimeError(
                f"Qwen ONNX stage omitted state output: {output_name}")
        successor[input_name] = output_by_name[output_name]
    return successor


def output_tensor(session, outputs: list[np.ndarray], metadata: dict,
                  *, terminal: bool) -> np.ndarray:
    _, state_outputs = _state_names(metadata)
    values = {
        str(item.name): np.asarray(value)
        for item, value in zip(session.get_outputs(), outputs)
    }
    if terminal:
        if "logits" not in values:
            raise RuntimeError("terminal Qwen ONNX stage omitted logits")
        return values["logits"]
    for name in ("hidden_states", "hidden_states_out", "last_hidden_state"):
        if name in values:
            return values[name]
    candidates = [value for name, value in values.items()
                  if name not in state_outputs and name != "logits"]
    if len(candidates) != 1:
        raise RuntimeError("Qwen ONNX stage has ambiguous activation output")
    return candidates[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--service-manifest", required=True)
    parser.add_argument("--artifact-root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--max-new-tokens", type=int, default=64)
    parser.add_argument(
        "--execution-provider", choices=("cuda", "cpu"), default="cuda",
        help="ORT execution provider; CPU is diagnostic only.",
    )
    parser.add_argument(
        "--padding-mode",
        choices=("stateful",),
        default="stateful",
        help=(
            "Compatibility spelling for the mandatory stateful prefill/decode "
            "sequence policy."),
    )
    parser.add_argument(
        "--allow-first-token-mismatch",
        action="store_true",
        help="Write a diagnostic result even when the frozen oracle differs.",
    )
    parser.add_argument(
        "--stage-device-ids",
        default="0,1,2",
        help="Comma-separated CUDA device IDs for stage 0, stage 1, and stage 2.",
    )
    parser.add_argument("--eos-token-id", type=int, action="append", default=[])
    args = parser.parse_args()

    if args.max_new_tokens < 1 or args.max_new_tokens > 256:
        raise SystemExit("max-new-tokens out of range")
    service = json.loads(Path(args.service_manifest).read_text(encoding="utf-8"))
    try:
        stage_device_ids = tuple(
            int(value.strip()) for value in str(args.stage_device_ids).split(",")
            if value.strip()
        )
    except ValueError as exc:
        raise SystemExit("--stage-device-ids must be comma-separated integers") from exc
    if len(stage_device_ids) != 3 or any(value < 0 for value in stage_device_ids):
        raise SystemExit("--stage-device-ids must contain exactly three non-negative IDs")
    root = Path(args.artifact_root).resolve()
    stages = sorted(service["stages"], key=lambda item: int(item["stageIndex"]))
    if len(stages) != 3 or service.get("dtype") not in {"float16", "float32"}:
        raise SystemExit("reference requires three FP16/FP32 ONNX stages")
    model_type = str(service.get("modelType", "qwen2"))
    if model_type == "qwen3_5_text":
        model_type = "qwen3_5"
    sessions = []
    for index, item in enumerate(stages):
        sessions.append(make_session(
            root / "qwen-onnx-stage-artifacts" / Path(item["path"]).name,
            device_id=stage_device_ids[index],
            execution_provider=args.execution_provider,
        ))
    prompt_ids = [int(value) for value in service["promptIds"]]
    prompt = np.asarray([prompt_ids], dtype=np.int64)
    sequence = list(prompt_ids)
    context_length = int(service.get("contextLength", 0))
    if context_length <= len(sequence):
        raise RuntimeError("QWEN_ONNX_CONTEXT_LENGTH_INVALID")
    if context_length < len(sequence) + args.max_new_tokens:
        raise RuntimeError(
            "QWEN_ONNX_CONTEXT_TOO_SHORT_FOR_REQUEST: "
            f"{context_length} < {len(sequence) + args.max_new_tokens}")
    print(
        f"QWEN_ONNX_CONTEXT prompt={len(sequence)} context={context_length} "
        f"sequencePolicy={SEQUENCE_POLICY}",
        flush=True,
    )
    generated = []
    completion_ms = []
    stage_states: list[dict[str, np.ndarray]] = [
        {} for _ in sessions]
    started = time.perf_counter()
    for generation_index in range(args.max_new_tokens):
        actual_length = len(sequence)
        token_chunk = sequence if generation_index == 0 else [sequence[-1]]
        ids = np.asarray([token_chunk], dtype=np.int64)
        attention = np.ones((1, actual_length), dtype=np.int64)
        first_position = actual_length - len(token_chunk)
        positions = np.arange(
            first_position, actual_length, dtype=np.int64).reshape(1, -1)
        if model_type == "qwen3_5":
            positions = np.broadcast_to(
                positions.reshape(1, 1, -1),
                (4, int(ids.shape[0]), int(ids.shape[1])),
            )
        hidden_size = int(
            dict(stages[0].get("tensorContracts", {}))
            .get("hidden_states", {}).get("shape", [1, "seq", 896])[2]
            if isinstance(dict(stages[0].get("tensorContracts", {}))
                          .get("hidden_states", {}).get("shape", [1, "seq", 896])[2], int)
            else 896
        )
        hidden = np.zeros((1, len(token_chunk), hidden_size), dtype=np.float32)
        outputs = None
        for index, session in enumerate(sessions):
            feed = stage_feed(
                session,
                input_ids=ids,
                attention=attention,
                positions=positions,
                hidden=hidden,
                metadata=stages[index],
                state=stage_states[index],
            )
            outputs = session.run(None, feed)
            stage_states[index] = update_stage_state(
                session, outputs, stages[index], stage_states[index])
            if index < len(sessions) - 1:
                hidden = output_tensor(
                    session, outputs, stages[index], terminal=False)
        assert outputs is not None
        logits = output_tensor(
            sessions[-1], outputs, stages[-1], terminal=True)
        token = int(np.argmax(logits[:, -1, :], axis=-1)[0])
        generated.append(token)
        sequence.append(token)
        completion_ms.append((time.perf_counter() - started) * 1000.0)
        if token in set(args.eos_token_id):
            break

    expected_value = service.get("referenceTopToken")
    expected_first = int(expected_value) if expected_value is not None else -1
    pytorch_reference = service.get("pytorchReferenceTopToken")
    if (generated and expected_first >= 0 and generated[0] != expected_first
            and not args.allow_first_token_mismatch):
        raise RuntimeError(
            f"reference first token mismatch: {generated[0]} != {expected_first}")
    result = {
        "schemaVersion": "ndnsf-di-qwen-onnx-reference-v1",
        "runtime": "onnxruntime",
        "executionProvider": (
            "CUDAExecutionProvider" if args.execution_provider == "cuda"
            else "CPUExecutionProvider"),
        "stageProviders": [list(session.get_providers()) for session in sessions],
        "dtype": service["dtype"],
        "promptIds": [int(value) for value in prompt[0]],
        "generatedTokenIds": generated,
        "tokenCompletionMonotonicMs": completion_ms,
        # When the manifest has no frozen runtime token yet, the first token
        # produced by this exact ORT stage chain becomes the canonical oracle;
        # the exporter-time PyTorch value remains a parity diagnostic only.
        "referenceTopToken": (
            expected_first if expected_first >= 0
            else (generated[0] if generated else None)),
        "pytorchReferenceTopToken": pytorch_reference,
        "referencePolicy": service.get(
            "referencePolicy", "onnxruntime-canonical-v1"),
        "model": service["model"],
        "modelRevision": service["modelRevision"],
        "modelType": model_type,
        "sequencePolicy": SEQUENCE_POLICY,
    }
    output = Path(args.output)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print("QWEN_ONNX_REFERENCE_PASS", json.dumps({
        "tokens": len(generated),
        "firstToken": generated[0] if generated else None,
        "output": str(output),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
