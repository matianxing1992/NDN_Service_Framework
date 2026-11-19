#!/usr/bin/env python3
"""Convert the exporter manifest into the NDNSF-DI stage-manifest contract.

The exporter manifest describes canonical ONNX files.  The NDNSF-DI request
planner additionally needs role names, content identities, and the immutable
runtime/source identities used by the ACK/Selection path.  This conversion is
metadata-only: it never creates a Provider-role assignment.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


STATE_FAMILIES = ("attention_kv", "recurrent_state", "convolution_state")
DECODE_MODE = "single-token-autoregressive"
MODALITY = "text-only"
THINKING_MODE = "disabled"
POSITION_INPUT_POLICY = "qwen-causal-position-v1"
SEQUENCE_POLICY = "stateful-prefill-decode-v1"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _required_state_names(item: dict[str, object], key: str) -> list[str]:
    """Return the adapter-certified state names, failing closed on old graphs.

    The former Qwen export advertised ``past_key``/``present_key`` only.  That
    is not the Spec175 state contract: Qwen3.6 must carry attention KV plus the
    linear-attention recurrent and convolution state through every stage.
    """
    values = item.get(key)
    if not isinstance(values, list) or not values:
        raise SystemExit(f"QWEN_STATE_{key.upper()}_REQUIRED")
    names = [str(value) for value in values]
    if len(set(names)) != len(names) or any(not value for value in names):
        raise SystemExit(f"QWEN_STATE_{key.upper()}_INVALID")
    missing = [f"{family}_{'in' if key == 'stateInputNames' else 'out'}"
               for family in STATE_FAMILIES if f"{family}_{'in' if key == 'stateInputNames' else 'out'}" not in names]
    if missing:
        raise SystemExit(
            f"QWEN_STATE_{key.upper()}_INCOMPLETE:" + ",".join(missing))
    return names


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--service-manifest", required=True)
    parser.add_argument("--artifact-root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--runtime-sif-sha256", required=True)
    parser.add_argument("--source-bundle-sha256", required=True)
    parser.add_argument("--capacity-decision-sha256", required=True)
    parser.add_argument("--policy-path", default="")
    args = parser.parse_args()

    service_path = Path(args.service_manifest).resolve()
    root = Path(args.artifact_root).resolve()
    source = json.loads(service_path.read_text(encoding="utf-8"))
    stages = source.get("stages")
    if source.get("schema") != "ndnsf-di-qwen-onnx-service-manifest-v1":
        raise SystemExit("unsupported Qwen ONNX service manifest")
    if source.get("runtime") != "onnxruntime":
        raise SystemExit("stage manifest requires ONNX Runtime")
    model_type = str(source.get("modelType", "qwen2"))
    if model_type == "qwen3_5_text":
        model_type = "qwen3_5"
    dtype = str(source.get("dtype", "")).lower()
    if dtype not in {"float16", "float32"}:
        raise SystemExit(
            "unsupported ONNX deployment dtype; export float16 or float32: "
            + dtype)
    if not isinstance(stages, list) or len(stages) != 3:
        raise SystemExit("Qwen3.6 stage manifest requires exactly three stages")
    sequence_policy = str(source.get("sequencePolicy", ""))
    context_length = int(source.get("contextLength", 0) or 0)
    prompt_length = int(source.get("promptLength", 0) or 0)
    prompt_ids = source.get("promptIds")
    if (not isinstance(prompt_ids, list) or len(prompt_ids) != prompt_length
            or any(not isinstance(value, int) or value < 0
                   for value in prompt_ids)):
        raise SystemExit("QWEN_ONNX_PROMPT_IDS_REQUIRED")
    pad_token_id = source.get("padTokenId")
    if sequence_policy != SEQUENCE_POLICY:
        raise SystemExit("QWEN_ONNX_SEQUENCE_POLICY_REQUIRED")
    if context_length <= prompt_length or context_length < prompt_length + 64:
        raise SystemExit("QWEN_ONNX_CONTEXT_LENGTH_INVALID")
    if not isinstance(pad_token_id, int) or pad_token_id < 0:
        raise SystemExit("QWEN_ONNX_PAD_TOKEN_INVALID")
    if source.get("decodeMode") != DECODE_MODE:
        raise SystemExit("QWEN_ONNX_DECODE_MODE_REQUIRED")
    if source.get("modality") != MODALITY:
        raise SystemExit("QWEN_ONNX_MODALITY_REQUIRED")
    if source.get("mtpEnabled") is not False:
        raise SystemExit("QWEN_ONNX_MTP_MUST_BE_DISABLED")
    if source.get("thinkingMode") != THINKING_MODE:
        raise SystemExit("QWEN_ONNX_THINKING_MODE_REQUIRED")
    components = source.get("graphComponents")
    if not isinstance(components, list) or not components:
        raise SystemExit("QWEN_ONNX_GRAPH_COMPONENTS_REQUIRED")
    forbidden = ("vision", "image", "video", "mtp", "speculative")
    if any(any(marker in str(component).lower() for marker in forbidden)
           for component in components):
        raise SystemExit("QWEN_ONNX_FORBIDDEN_GRAPH_COMPONENT")

    manifest_digest = digest(service_path)
    rows = []
    for index, item in enumerate(stages):
        filename = Path(str(item.get("path", ""))).name
        path = root / "qwen-onnx-stage-artifacts" / filename
        if not path.is_file():
            raise SystemExit(f"missing ONNX stage: {path}")
        actual = digest(path)
        declared = str(item.get("sha256", ""))
        if declared.startswith("sha256:"):
            declared = declared[7:]
        if actual != declared:
            raise SystemExit(f"ONNX stage digest mismatch: {path}")
        state_inputs = _required_state_names(item, "stateInputNames")
        state_outputs = _required_state_names(item, "stateOutputNames")
        input_names = [str(value) for value in item.get("inputNames", [])]
        if (source.get("positionInputPolicy") != POSITION_INPUT_POLICY
                or "attention_mask" not in input_names
                or "position_ids" not in input_names):
            raise SystemExit("QWEN_ONNX_POSITION_INPUT_POLICY_REQUIRED")
        contracts = dict(item.get("tensorContracts", {}))
        for family in STATE_FAMILIES:
            if (f"{family}_in" not in contracts
                    or f"{family}_out" not in contracts):
                raise SystemExit(
                    "QWEN_ONNX_STATE_CONTRACT_MISSING:" + family)
            state_input = contracts.get(f"{family}_in", {})
            state_output = contracts.get(f"{family}_out", {})
            if (state_input.get("elementType")
                    != state_output.get("elementType")):
                raise SystemExit(
                    "QWEN_ONNX_STATE_DTYPE_MISMATCH:" + family)
        role = f"/LLM/Pipeline/Stage/{index}"
        rows.append({
            "role": role,
            "stageIndex": int(item.get("stageIndex", index)),
            "stageCount": 3,
            "layerRange": dict(item["layerRange"]),
            "filename": filename,
            "path": str(path),
            "bytes": path.stat().st_size,
            "sha256": "sha256:" + actual,
            "kind": "onnx-model",
            "backend": "onnxruntime",
            "runtime": "onnxruntime",
            "metadata": {
                "runtime": "onnxruntime",
                "modelType": model_type,
                "runtimeExecutionProvider": "CUDAExecutionProvider",
                "dtype": dtype,
                "sequencePolicy": sequence_policy,
                "promptLength": prompt_length,
                "contextLength": context_length,
                "padTokenId": int(pad_token_id),
                "stageIndex": int(item.get("stageIndex", index)),
                "stageCount": 3,
                "layerRange": dict(item["layerRange"]),
                "inputNames": input_names,
                "outputNames": list(item.get("outputNames", [])),
                "cacheInputs": list(item.get("cacheInputs", [])),
                "cacheOutputs": list(item.get("cacheOutputs", [])),
                "stateInputNames": state_inputs,
                "stateOutputNames": state_outputs,
                "stateFamilies": list(STATE_FAMILIES),
                "positionInputPolicy": POSITION_INPUT_POLICY,
                "attentionMaskInputName": "attention_mask",
                "positionIdsInputName": "position_ids",
                "cachePositionInputName": (
                    "cache_position" if "cache_position" in input_names else ""),
                "tensorContracts": contracts,
            },
        })

    tokenizer = root / "qwen-onnx-tokenizer"
    tokenizer_json = tokenizer / "tokenizer.json"
    if not tokenizer_json.is_file():
        raise SystemExit(f"missing standalone tokenizer: {tokenizer_json}")
    tokenizer_digest = digest(tokenizer_json)
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    doc = {
        "schemaVersion": "ndnsf-di-qwen36-onnx-stage-manifest-v1",
        "modelProfile": "qwen3.6-27b-onnx-cuda",
        "repository": "Qwen/Qwen3.6-27B",
        "modelType": model_type,
        "revision": str(source["modelRevision"]),
        "modelDigest": "sha256:" + manifest_digest,
        "dtype": dtype,
        "sequencePolicy": sequence_policy,
        "decodeMode": DECODE_MODE,
        "modality": MODALITY,
        "mtpEnabled": False,
        "thinkingMode": THINKING_MODE,
        "graphComponents": [str(component) for component in components],
        "promptLength": prompt_length,
        "promptIds": [int(value) for value in prompt_ids],
        "contextLength": context_length,
        "padTokenId": int(pad_token_id),
        "quantization": "none",
        "layerCount": int(source["layerCount"]),
        "layerRanges": source["layerRanges"],
        # CUDA EP executes model kernels.  Dynamic KV-cache shape/control
        # nodes may use CPU EP; that is not model-compute fallback.
        "cpuFallbackAllowed": False,
        "cpuControlOpsAllowed": True,
        "runtimeSifSha256": args.runtime_sif_sha256,
        "sourceBundleSha256": args.source_bundle_sha256,
        "capacityDecisionSha256": args.capacity_decision_sha256,
        "runtime": {
            "backend": "onnxruntime",
            "executionProvider": "CUDAExecutionProvider",
            "cpuControlExecutionProvider": "CPUExecutionProvider",
            "cpuComputeFallbackAllowed": False,
        },
        "tokenizer": {
            "path": str(tokenizer),
            "digest": "sha256:" + tokenizer_digest,
        },
        "policy": {"path": args.policy_path},
        "reference": {
            "referenceTopToken": int(source["referenceTopToken"]),
            "pytorchReferenceTopToken": (
                None if source.get("pytorchReferenceTopToken") is None
                else int(source["pytorchReferenceTopToken"])),
            "referencePolicy": source.get(
                "referencePolicy", "onnxruntime-canonical-v1"),
            "manifestSha256": "sha256:" + manifest_digest,
        },
        "stages": rows,
    }
    output.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(json.dumps({
        "schemaVersion": doc["schemaVersion"],
        "manifestSha256": "sha256:" + digest(output),
        "stageCount": len(rows),
        "backend": doc["runtime"]["backend"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
