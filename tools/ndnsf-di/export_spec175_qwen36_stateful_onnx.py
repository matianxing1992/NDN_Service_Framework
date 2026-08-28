#!/usr/bin/env python3
"""Seal an offline Qwen3.6 stateful-ONNX stage manifest.

The deployment image consumes the resulting ONNX and manifest; it never
imports PyTorch or Transformers.  Model conversion remains an explicitly
offline operation owned by the caller.  This utility performs the final
content-addressed, state-I/O contract check and emits the immutable manifest
used by the NDNSF-DI provider assembler.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


STATE_FAMILIES = ("attention_kv", "recurrent_state", "convolution_state")
DECODE_MODE = "single-token-autoregressive"
MODALITY = "text-only"
MTP_ENABLED = False
THINKING_MODE = "disabled"
POSITION_INPUT_POLICY = "qwen-causal-position-v1"
SEQUENCE_POLICY = "stateful-prefill-decode-v1"
FORBIDDEN_COMPONENT_MARKERS = ("vision", "image", "video", "mtp", "speculative")


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def _names_from_manifest(value: object, label: str) -> tuple[str, ...]:
    names = tuple(str(item) for item in (value or ()))
    if not names or len(set(names)) != len(names):
        raise ValueError(f"{label} must contain unique non-empty names")
    if any(not item for item in names):
        raise ValueError(f"{label} contains an empty name")
    return names


def seal(args: argparse.Namespace) -> dict[str, object]:
    model = Path(args.model).resolve()
    if not model.is_file():
        raise ValueError(f"ONNX model does not exist: {model}")
    if model.suffix.lower() != ".onnx":
        raise ValueError("stateful deployment input must be an ONNX graph")
    raw = json.loads(Path(args.io_manifest).read_text(encoding="utf-8"))
    inputs = _names_from_manifest(raw.get("inputNames"), "inputNames")
    outputs = _names_from_manifest(raw.get("outputNames"), "outputNames")
    state_inputs = _names_from_manifest(raw.get("stateInputNames"), "stateInputNames")
    state_outputs = _names_from_manifest(raw.get("stateOutputNames"), "stateOutputNames")
    if raw.get("sequencePolicy") != SEQUENCE_POLICY:
        raise ValueError(f"sequencePolicy must be {SEQUENCE_POLICY}")
    if raw.get("positionInputPolicy") != POSITION_INPUT_POLICY:
        raise ValueError(
            f"positionInputPolicy must be {POSITION_INPUT_POLICY}")
    attention_mask_input = str(raw.get(
        "attentionMaskInputName", "attention_mask"))
    position_ids_input = str(raw.get(
        "positionIdsInputName", "position_ids"))
    cache_position_input = str(raw.get("cachePositionInputName", ""))
    required_position_inputs = {
        attention_mask_input, position_ids_input,
        *({cache_position_input} if cache_position_input else set()),
    }
    if ("" in required_position_inputs
            or not required_position_inputs.issubset(set(inputs))):
        raise ValueError(
            "adapter-certified position inputs are absent from inputNames")
    if raw.get("decodeMode") != DECODE_MODE:
        raise ValueError(f"decodeMode must be {DECODE_MODE}")
    if raw.get("modality") != MODALITY:
        raise ValueError(f"modality must be {MODALITY}")
    if raw.get("mtpEnabled") is not MTP_ENABLED:
        raise ValueError("mtpEnabled must be false")
    if raw.get("thinkingMode") != THINKING_MODE:
        raise ValueError(f"thinkingMode must be {THINKING_MODE}")
    chat_template_digest = str(raw.get("chatTemplateDigest", ""))
    if (not chat_template_digest.startswith("sha256:")
            or len(chat_template_digest) != 71):
        raise ValueError("chatTemplateDigest must be a canonical sha256 digest")
    graph_components = _names_from_manifest(
        raw.get("graphComponents"), "graphComponents")
    forbidden = tuple(
        item for item in graph_components
        if any(marker in item.lower() for marker in FORBIDDEN_COMPONENT_MARKERS)
    )
    if forbidden:
        raise ValueError(
            "language-model-only graph contains forbidden components: "
            + ", ".join(forbidden))
    for family in STATE_FAMILIES:
        if f"{family}_in" not in state_inputs or f"{family}_out" not in state_outputs:
            raise ValueError(f"missing complete state family: {family}")
    result = {
        "schema": "ndnsf-di-qwen36-stateful-onnx-manifest-v1",
        "modelDigest": digest(model),
        "graphSemanticDigest": str(raw.get("graphSemanticDigest", "")),
        "adapterDigest": str(raw.get("adapterDigest", "")),
        "tokenizerDigest": str(raw.get("tokenizerDigest", "")),
        "runnerDigest": str(raw.get("runnerDigest", "")),
        "stage": str(args.stage),
        "layerRange": [int(args.layer_begin), int(args.layer_end)],
        "inputNames": list(inputs),
        "outputNames": list(outputs),
        "stateInputNames": list(state_inputs),
        "stateOutputNames": list(state_outputs),
        "stateFamilies": list(STATE_FAMILIES),
        "sequencePolicy": SEQUENCE_POLICY,
        "positionInputPolicy": POSITION_INPUT_POLICY,
        "attentionMaskInputName": attention_mask_input,
        "positionIdsInputName": position_ids_input,
        "cachePositionInputName": cache_position_input,
        "decodeMode": DECODE_MODE,
        "modality": MODALITY,
        "mtpEnabled": MTP_ENABLED,
        "thinkingMode": THINKING_MODE,
        "chatTemplateDigest": chat_template_digest,
        "graphComponents": list(graph_components),
        "visionEncoderOrProjectorPresent": False,
        "mtpOrSpeculativeHeadPresent": False,
        "deploymentRuntime": "onnxruntime",
        "offlineExporter": "external-canonical-exporter",
        "pytorchOrTransformersInDeployment": False,
    }
    for key in ("graphSemanticDigest", "adapterDigest", "tokenizerDigest", "runnerDigest"):
        if not result[key].startswith("sha256:") or len(result[key]) != 71:
            raise ValueError(f"{key} must be a canonical sha256 digest")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--io-manifest", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--layer-begin", type=int, required=True)
    parser.add_argument("--layer-end", type=int, required=True)
    args = parser.parse_args()
    if args.layer_begin < 0 or args.layer_end <= args.layer_begin:
        raise SystemExit("invalid layer range")
    manifest = seal(args)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.tmp")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
    temporary.replace(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
