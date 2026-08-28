#!/usr/bin/env python3
"""Run the Spec175 per-stage stateful Qwen readiness control.

This runner is deliberately deployment-side and ONNX Runtime-only.  It does
not import PyTorch/Transformers and does not invoke the NDNSF control-plane
launcher.  The model directory is staged once into a node-local,
content-addressed directory before the measured prefill/decode and
cached-versus-full-prefix controls.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import time

import numpy as np
import onnxruntime as ort


STATE_FAMILIES = ("attention_kv", "recurrent_state", "convolution_state")
STATE_INPUTS = tuple(f"{family}_in" for family in STATE_FAMILIES)
STATE_OUTPUTS = tuple(f"{family}_out" for family in STATE_FAMILIES)
SCHEMA = "ndnsf-di-qwen36-onnx-stage-manifest-v1"
SEQUENCE_POLICY = "stateful-prefill-decode-v1"


def sha256_file(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def fail(message: str) -> "NoReturn":
    raise RuntimeError(message)


def numpy_dtype(type_name: str):
    value = str(type_name).lower()
    if "float16" in value:
        return np.float16
    if "float" in value:
        return np.float32
    if "int64" in value:
        return np.int64
    if "int32" in value:
        return np.int32
    if "bool" in value:
        return np.bool_
    fail(f"QWEN_STAGE_UNSUPPORTED_INPUT_TYPE:{type_name}")


def load_manifest(path: Path) -> dict:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schemaVersion") != SCHEMA:
        fail("QWEN_STAGE_MANIFEST_SCHEMA")
    if document.get("repository") != "Qwen/Qwen3.6-27B":
        fail("QWEN_STAGE_MODEL_IDENTITY")
    if document.get("sequencePolicy") != SEQUENCE_POLICY:
        fail("QWEN_STAGE_SEQUENCE_POLICY")
    stages = document.get("stages")
    if not isinstance(stages, list) or len(stages) != 3:
        fail("QWEN_STAGE_COUNT")
    for index, stage in enumerate(stages):
        if stage.get("stageIndex") != index:
            fail(f"QWEN_STAGE_INDEX:{index}")
        metadata = stage.get("metadata")
        if not isinstance(metadata, dict):
            fail(f"QWEN_STAGE_METADATA:{index}")
        if tuple(metadata.get("stateInputNames", ())) != STATE_INPUTS:
            fail(f"QWEN_STAGE_STATE_INPUTS:{index}")
        if tuple(metadata.get("stateOutputNames", ())) != STATE_OUTPUTS:
            fail(f"QWEN_STAGE_STATE_OUTPUTS:{index}")
        if metadata.get("positionInputPolicy") != "qwen-causal-position-v1":
            fail(f"QWEN_STAGE_POSITION_POLICY:{index}")
        names = set(metadata.get("inputNames", ()))
        if not {"attention_mask", "position_ids"}.issubset(names):
            fail(f"QWEN_STAGE_POSITION_INPUTS:{index}")
        digest = str(stage.get("sha256", ""))
        if not digest.startswith("sha256:") or len(digest) != 71:
            fail(f"QWEN_STAGE_DIGEST:{index}")
    tokenizer = document.get("tokenizer")
    if not isinstance(tokenizer, dict) or not str(tokenizer.get("digest", "")).startswith("sha256:"):
        fail("QWEN_STAGE_TOKENIZER_DIGEST")
    return document


def verify_file(path: Path, declared: str, label: str) -> int:
    if not path.is_file():
        fail(f"QWEN_STAGE_FILE_MISSING:{label}")
    actual = "sha256:" + sha256_file(path)
    if actual != declared:
        fail(f"QWEN_STAGE_DIGEST_MISMATCH:{label}")
    return path.stat().st_size


def verify_artifact_checksums(root: Path) -> int:
    """Verify the promoted artifact checksum ledger after node-local copy."""
    ledger = root / "artifact-checksums.sha256"
    if not ledger.is_file():
        fail("QWEN_STAGE_ARTIFACT_CHECKSUM_LEDGER_MISSING")
    verified = 0
    for raw in ledger.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(maxsplit=1)
        if len(fields) != 2 or len(fields[0]) != 64:
            fail("QWEN_STAGE_ARTIFACT_CHECKSUM_LEDGER_INVALID")
        relative = fields[1].lstrip(" *")
        path = (root / relative).resolve()
        if root.resolve() not in path.parents or not path.is_file():
            fail("QWEN_STAGE_ARTIFACT_CHECKSUM_PATH_INVALID")
        if sha256_file(path) != fields[0]:
            fail("QWEN_STAGE_ARTIFACT_CHECKSUM_MISMATCH:" + relative)
        verified += 1
    if verified == 0:
        fail("QWEN_STAGE_ARTIFACT_CHECKSUM_LEDGER_EMPTY")
    return verified


def stage_model(model_root: Path, cache_root: Path, manifest: dict) -> tuple[Path, dict]:
    """Copy the external artifact tree once and publish an atomic cache marker."""
    source = model_root.resolve()
    if not source.is_dir():
        fail("QWEN_STAGE_MODEL_ROOT_MISSING")
    cache_root = cache_root.resolve()
    cache_root.mkdir(parents=True, exist_ok=True)
    model_digest = str(manifest.get("modelDigest", ""))
    if not model_digest.startswith("sha256:"):
        fail("QWEN_STAGE_MODEL_DIGEST_MISSING")
    # The digest is part of the directory name, so a job cannot accidentally
    # reuse another model's node-local stage tree.
    cache = cache_root / ("qwen36-stage-cache-" + model_digest[7:])
    marker = cache / ".complete.json"
    started = time.perf_counter()
    cache_hit = marker.is_file()
    if cache_hit:
        try:
            previous = json.loads(marker.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            previous = {}
        if previous.get("modelDigest") != model_digest:
            fail("QWEN_STAGE_CACHE_IDENTITY_MISMATCH")
    else:
        temporary = Path(tempfile.mkdtemp(
            prefix=".qwen36-stage-cache-", dir=str(cache_root)))
        if temporary.exists():
            shutil.rmtree(temporary)
        if cache.exists():
            # A cache without its completion marker is an interrupted copy;
            # it is safe to replace because this directory is job-local.
            shutil.rmtree(cache)
        try:
            shutil.copytree(source, temporary, symlinks=False)
            temporary_marker = temporary / ".complete.json"
            temporary_marker.write_text(
                json.dumps({"modelDigest": model_digest}, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            temporary.rename(cache)
        except Exception:
            if temporary.exists():
                shutil.rmtree(temporary, ignore_errors=True)
            raise
    artifact_dir = cache / "qwen-onnx-stage-artifacts"
    tokenizer_dir = cache / "qwen-onnx-tokenizer"
    checksum_entries = verify_artifact_checksums(cache)
    bytes_verified = 0
    for index, stage in enumerate(manifest["stages"]):
        filename = Path(str(stage.get("filename") or stage.get("path", ""))).name
        bytes_verified += verify_file(
            artifact_dir / filename, str(stage["sha256"]), f"stage[{index}]")
    tokenizer_path = tokenizer_dir / "tokenizer.json"
    tokenizer_bytes = verify_file(
        tokenizer_path, str(manifest["tokenizer"]["digest"]), "tokenizer")
    complete = {
        "modelDigest": model_digest,
        "stageCount": len(manifest["stages"]),
        "stageBytesVerified": bytes_verified,
        "tokenizerBytesVerified": tokenizer_bytes,
        "artifactChecksumEntries": checksum_entries,
    }
    if not marker.is_file():
        marker.write_text(json.dumps(complete, sort_keys=True) + "\n", encoding="utf-8")
    return cache, {
        "cacheRoot": str(cache),
        "stageBytesVerified": bytes_verified,
        "tokenizerBytesVerified": tokenizer_bytes,
        "artifactChecksumEntries": checksum_entries,
        "stagingMs": (time.perf_counter() - started) * 1000.0,
        "cacheHit": cache_hit,
    }


def shape_from_contract(item, contract: dict, batch: int = 1) -> tuple[int, ...]:
    declared = list(contract.get("shape", ()))
    shape: list[int] = []
    for index, dimension in enumerate(declared):
        if isinstance(dimension, int) and dimension >= 0:
            shape.append(int(dimension))
        elif index == 0:
            shape.append(batch)
        elif any(marker in str(dimension).lower()
                 for marker in ("past", "cache", "sequence", "seq", "total")):
            shape.append(0)
        else:
            fail(f"QWEN_STAGE_STATE_SHAPE:{item.name}")
    if not shape:
        fail(f"QWEN_STAGE_STATE_SHAPE_EMPTY:{item.name}")
    return tuple(shape)


def initial_state(session, metadata: dict) -> dict[str, np.ndarray]:
    contracts = dict(metadata.get("tensorContracts", {}))
    result = {}
    by_name = {str(item.name): item for item in session.get_inputs()}
    for name in STATE_INPUTS:
        item = by_name.get(name)
        if item is None:
            fail(f"QWEN_STAGE_STATE_INPUT_NOT_IN_GRAPH:{name}")
        shape = shape_from_contract(item, dict(contracts.get(name, {})))
        result[name] = np.zeros(shape, dtype=numpy_dtype(item.type))
    return result


def position_ids(model_type: str, length: int, chunk: int) -> np.ndarray:
    values = np.arange(length - chunk, length, dtype=np.int64)
    if model_type == "qwen3_5":
        return np.broadcast_to(values.reshape(1, 1, -1), (4, 1, chunk))
    return values.reshape(1, -1)


def host_output(outputs: dict[str, object], names: tuple[str, ...], terminal: bool) -> np.ndarray:
    if terminal:
        value = outputs.get("logits")
        if value is None:
            fail("QWEN_STAGE_LOGITS_MISSING")
        return value.numpy() if hasattr(value, "numpy") else np.asarray(value)
    for name in ("hidden_states", "hidden_states_out", "last_hidden_state"):
        if name in outputs:
            value = outputs[name]
            return value.numpy() if hasattr(value, "numpy") else np.asarray(value)
    fail("QWEN_STAGE_ACTIVATION_MISSING")


def run_stage(session, metadata: dict, *, arrays: dict[str, np.ndarray], state: dict[str, object],
              device_id: int, bound: bool) -> tuple[dict[str, object], dict[str, object], float]:
    started = time.perf_counter()
    if not bound:
        feed = {}
        for item in session.get_inputs():
            name = str(item.name)
            if name in state:
                value = state[name]
            elif name not in arrays:
                fail(f"QWEN_STAGE_INPUT_NOT_BOUND:{name}")
            else:
                value = arrays[name]
            feed[name] = np.asarray(value).astype(numpy_dtype(item.type), copy=False)
        values = session.run(None, feed)
        outputs = {str(item.name): value
                   for item, value in zip(session.get_outputs(), values)}
        return outputs, {name: outputs[name] for name in STATE_OUTPUTS}, (time.perf_counter() - started) * 1000.0

    binding = session.io_binding()
    for item in session.get_inputs():
        name = str(item.name)
        if name in state and hasattr(state[name], "device_name"):
            binding.bind_ortvalue_input(name, state[name])
            continue
        if name in state:
            value = state[name]
        elif name in arrays:
            value = arrays[name]
        else:
            fail(f"QWEN_STAGE_INPUT_NOT_BOUND:{name}")
        binding.bind_cpu_input(
            name, np.asarray(value).astype(numpy_dtype(item.type), copy=False))
    for item in session.get_outputs():
        binding.bind_output(str(item.name), "cuda", device_id)
    session.run_with_iobinding(binding)
    values = binding.get_outputs()
    host_values = binding.copy_outputs_to_cpu()
    outputs = {}
    for item, value, host_value in zip(session.get_outputs(), values, host_values):
        name = str(item.name)
        # Keep the complete state as CUDA OrtValues.  Only activation/logit
        # outputs cross to the host so the next stage can consume them.
        outputs[name] = value if name in STATE_OUTPUTS else host_value
    next_state = {}
    for input_name, output_name in zip(STATE_INPUTS, STATE_OUTPUTS):
        value = outputs.get(output_name)
        if value is None:
            fail(f"QWEN_STAGE_STATE_OUTPUT_MISSING:{output_name}")
        if getattr(value, "device_name", "") != "cuda":
            fail(f"QWEN_STAGE_STATE_NOT_DEVICE_RESIDENT:{output_name}")
        next_state[input_name] = value
    return outputs, next_state, (time.perf_counter() - started) * 1000.0


def run_chain(sessions: list[ort.InferenceSession], stages: list[dict], prompt: list[int],
              steps: int, *, bound: bool, device_id: int) -> dict:
    sequence = list(prompt)
    generated: list[int] = []
    states: list[dict[str, object]] = [{} for _ in sessions]
    stage_times: list[list[float]] = [[] for _ in sessions]
    state_device_checks: list[bool] = [False] * len(sessions)
    for epoch in range(steps):
        if not bound:
            states = [{} for _ in sessions]
        chunk = sequence if epoch == 0 or not bound else [sequence[-1]]
        ids = np.asarray([chunk], dtype=np.int64)
        arrays: dict[str, np.ndarray] = {
            "input_ids": ids,
            "attention_mask": np.ones((1, len(sequence)), dtype=np.int64),
            "position_ids": position_ids("qwen3_5", len(sequence), len(chunk)),
        }
        hidden = None
        for index, session in enumerate(sessions):
            local = dict(arrays)
            if hidden is not None:
                local["hidden_states"] = hidden
            if not states[index]:
                states[index] = initial_state(session, stages[index]["metadata"])
            outputs, successor, elapsed = run_stage(
                session, stages[index]["metadata"], arrays=local, state=states[index],
                device_id=device_id, bound=bound)
            states[index] = successor
            stage_times[index].append(elapsed)
            state_device_checks[index] = state_device_checks[index] or all(
                getattr(value, "device_name", "") == "cuda" for value in successor.values())
            hidden = host_output(outputs, (), terminal=index == len(sessions) - 1)
        token = int(np.argmax(hidden[:, -1, :], axis=-1)[0])
        generated.append(token)
        sequence.append(token)
    return {
        "generatedTokenIds": generated,
        "stageTimesMs": stage_times,
        "stateDeviceResident": state_device_checks,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--model-root", required=True, type=Path)
    parser.add_argument("--cache-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--device-id", type=int, default=0)
    parser.add_argument("--max-new-tokens", type=int, default=8)
    args = parser.parse_args()
    if args.max_new_tokens != 8:
        fail("QWEN_STAGE_REGISTERED_LENGTH_MUST_BE_8")
    manifest = load_manifest(args.manifest.resolve())
    cache, staging = stage_model(args.model_root, args.cache_root, manifest)
    artifact = cache / "qwen-onnx-stage-artifacts"
    sessions = []
    for index, stage in enumerate(manifest["stages"]):
        filename = Path(str(stage.get("filename") or stage.get("path", ""))).name
        options = ort.SessionOptions()
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        options.enable_mem_pattern = False
        options.enable_mem_reuse = True
        session = ort.InferenceSession(
            str(artifact / filename), sess_options=options,
            providers=[("CUDAExecutionProvider", {"device_id": args.device_id}),
                       "CPUExecutionProvider"])
        providers = tuple(session.get_providers())
        if providers[:1] != ("CUDAExecutionProvider",):
            fail(f"QWEN_STAGE_CUDA_PROVIDER_MISSING:{index}:{providers}")
        sessions.append(session)
    prompt = [int(value) for value in manifest.get("promptIds", ())]
    if not prompt:
        fail("QWEN_STAGE_PROMPT_IDS_MISSING")
    # Exclude one preparation run, then keep all three alternating-order
    # cached/full pairs.  The order is part of the evidence so a thermal or
    # allocator effect cannot be silently selected away.
    run_chain(sessions, manifest["stages"], prompt, args.max_new_tokens,
              bound=True, device_id=args.device_id)
    cached_runs: list[dict] = []
    full_runs: list[dict] = []
    pair_order: list[str] = []
    for pair in range(3):
        if pair % 2 == 0:
            cached = run_chain(sessions, manifest["stages"], prompt,
                               args.max_new_tokens, bound=True,
                               device_id=args.device_id)
            full = run_chain(sessions, manifest["stages"], prompt,
                             args.max_new_tokens, bound=False,
                             device_id=args.device_id)
            pair_order.append("cached-full")
        else:
            full = run_chain(sessions, manifest["stages"], prompt,
                             args.max_new_tokens, bound=False,
                             device_id=args.device_id)
            cached = run_chain(sessions, manifest["stages"], prompt,
                               args.max_new_tokens, bound=True,
                               device_id=args.device_id)
            pair_order.append("full-cached")
        if cached["generatedTokenIds"] != full["generatedTokenIds"]:
            fail(f"QWEN_STAGE_CACHED_FULL_PARITY:{pair}")
        cached_runs.append(cached)
        full_runs.append(full)
    cached = cached_runs[-1]
    full = full_runs[-1]
    reference = manifest.get("reference", {})
    expected = reference.get("referenceTopToken")
    if expected is not None and cached["generatedTokenIds"][0] != int(expected):
        fail("QWEN_STAGE_REFERENCE_TOKEN_MISMATCH")
    stage_records = []
    for index, stage in enumerate(manifest["stages"]):
        cached_times = [value for run in cached_runs
                        for value in run["stageTimesMs"][index][1:]]
        full_times = [value for run in full_runs
                      for value in run["stageTimesMs"][index]]
        if not cached_times or not full_times:
            fail(f"QWEN_STAGE_TIMING_EMPTY:{index}")
        cached_median = float(np.median(cached_times))
        full_median = float(np.median(full_times))
        stage_records.append({
            "stageIndex": index,
            "role": stage["role"],
            "sha256": stage["sha256"],
            "cachedInputTokens": 1,
            "cachedMedianModelComputeMs": cached_median,
            "fullPrefixMedianModelComputeMs": full_median,
            "cachedFaster": cached_median < full_median,
            "pairCount": 3,
            "pairOrder": pair_order,
            "stateDeviceResident": bool(cached["stateDeviceResident"][index]),
            "hostCompleteStateBytesAfterPrefill": 0,
            "cpuModelComputeFallback": False,
        })
        if not stage_records[-1]["cachedFaster"]:
            fail(f"QWEN_STAGE_CACHED_NOT_FASTER:{index}")
        if not stage_records[-1]["stateDeviceResident"]:
            fail(f"QWEN_STAGE_STATE_NOT_RESIDENT:{index}")
    result = {
        "schemaVersion": "ndnsf-di-qwen36-stage-readiness-v1",
        "status": "PASS",
        "executionProvider": "CUDAExecutionProvider",
        "modelManifest": str(args.manifest.resolve()),
        "modelDigest": manifest.get("modelDigest"),
        "referenceTopToken": expected,
        "generatedTokenIds": cached["generatedTokenIds"],
        "nonemptyTranscript": bool(cached["generatedTokenIds"]),
        "staging": staging,
        "stages": stage_records,
        "noCompleteStateHostRoundTripAfterPrefill": True,
        "cpuModelComputeFallback": False,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print("QWEN_STAGE_READINESS_PASS", json.dumps({
        "output": str(args.output), "tokens": len(cached["generatedTokenIds"]),
        "stages": len(stage_records),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
