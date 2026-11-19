#!/usr/bin/env python3
"""Prepare portable inputs for the installed Spec 242 MiniNDN deployment."""

import argparse
import hashlib
import json
import re
import runpy
import sys
from pathlib import Path

import onnx
from ndnsf import _ndnsf
from ndnsf_distributed_inference.adapters.onnx.graph import canonical_onnx_identity


def digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def prepare(output, requester, protection_epoch, scenario="hybrid"):
    repo = Path(__file__).resolve().parents[3]
    # Use the maintained host-side encoder and semantic graph identity.
    sys.path.insert(0, str(repo / "Experiments"))
    from NDNSF_DI_Qwen06B_Native_Minindn import qwen_graph_digest, tensor_bundle

    output.mkdir(mode=0o700)
    if scenario == "tp3":
        source = output / "one-layer-tp3.onnx"
        generate = runpy.run_path(str(Path(__file__).with_name("make-four-layer-qwen.py")))["build"]
        generate(str(source), layers=1, q_heads=6, kv_heads=3, ffn=192, decode=True)
        partition, stages, degrees = [[0, 1]], ["/Stage/First"], [3]
    else:
        source = Path(__file__).with_name("four-layer-conversation.onnx").resolve()
        partition, stages, degrees = [[0, 1], [1, 4]], ["/Stage/First", "/Stage/Last"], [2, 1]
    model_bytes = source.read_bytes()
    source_digest = digest(model_bytes)
    if scenario == "hybrid" and source_digest != "sha256:52fc9b4abbc04ef28c9ca80c9e2dd40e31573998220a325acf281e1a87860fbd":
        raise ValueError("Spec 242 ONNX fixture identity changed")
    graph_digest = canonical_onnx_identity(source).graph_digest
    schema_digest = digest(b"spec242-planning-graph")
    manifest_digest = digest(b"spec242-four-layer-conversation:" + model_bytes)
    adapter = dict(name="qwen", version="1", abi="onnxruntime", model_formats=["onnx"],
                   tasks=["text-generation"], backends=["onnxruntime"], precisions=["int4"],
                   graph_inspectable=True, splittable=True, deterministic_analysis=True,
                   state_digest=schema_digest)
    for field in ("input", "options", "result", "graph", "split", "state"):
        adapter[field + "_schema_digest"] = schema_digest
    model = dict(model_name="qwen-fixture", source_revision="fixture-v1",
                 content_digest=source_digest, semantics_digest=schema_digest,
                 graph_digest=qwen_graph_digest("qwen-fixture", "fixture-v1", "int4", partition),
                 model_format="onnx", precision="int4", quantization_subtype="weight_only_int4",
                 adapter=adapter)
    bound = 8 * 1024 * 1024
    # Admission budgets for this small CPU fixture. The full 68-position KV
    # extent fits in 128 KiB per role. Measure actual process usage in MiniNDN.
    profiles = {stage: dict(backends=["onnxruntime"], weight_bytes=bound, workspace_bytes=bound,
                            kv_bytes=128 * 1024, activation_bytes=128 * 1024, transient_bytes=bound,
                            safety_margin=1.0) for stage in stages}
    splitter = dict(kind="QWEN", roles=stages, layer_ranges=partition, tensor_degrees=degrees,
                    artifact_digests_by_role={stage: digest(stage.encode()) for stage in stages},
                    rank_artifact_digests_by_role={
                        stages[0]: [digest(f"first-r{rank}".encode()) for rank in range(degrees[0])]},
                    weight_bytes_by_role={stage: bound for stage in stages},
                    resources_by_role=profiles, input_ingress_role=stages[0],
                    result_egress_role=stages[-1])
    node_mapping = {}
    for index, node in enumerate(onnx.load_model_from_string(model_bytes).graph.node):
        match = re.search(r"layers\.(\d+)(?:/|$)", node.name)
        if match and int(match[1]) < partition[-1][1]:
            label = f"layer-{int(match[1]):02d}"
        elif match or "lm_head" in node.name:
            label = "final-norm-head"
        else:
            label = "embedding"
        node_mapping.setdefault(label, []).append(index)
    state_inputs = {stage: {} for stage in stages}
    state_outputs = {stage: {} for stage in stages}
    state_input_names = []
    state_output_names = []
    successors = []
    for stage, (begin, end) in zip(stages, partition):
        for layer in range(begin, end):
            for component in ("key", "value"):
                slot = f"{layer}.{component}"
                state_input, state_output = f"past_key_values.{slot}", f"present.{slot}"
                state_inputs[stage][slot] = [state_input]
                state_outputs[stage][slot] = [state_output]
                state_input_names.append(state_input)
                state_output_names.append(state_output)
                successors.append(f"{state_input}={state_output}")
    catalog = dict(schema="ndnsf-di-native-request-catalog-v1", model=model, splitter=splitter,
                   source=dict(file=str(source), bytes=len(model_bytes), initializer_bytes=0,
                               data_name=requester + "/Model/Spec242/source", digest=source_digest,
                               model_manifest_digest=manifest_digest,
                               canonical_graph_digest=graph_digest),
                   recipe=dict(artifact_profile_digest=schema_digest,
                               assembler_descriptor_digest=schema_digest,
                               backend_abi="onnxruntime", precision="int4",
                               quantization="weight_only_int4", layout="BNSH", padding="none",
                               protection_epoch=protection_epoch, max_source_bytes=bound,
                               max_assembled_bytes=bound, max_nodes=512),
                   publication=dict(artifact_root="/Model/Spec242/artifacts",
                                    package_manifest_digest=manifest_digest,
                                    max_publication_bytes=bound),
                   node_mapping=node_mapping, state_inputs=state_inputs, state_outputs=state_outputs,
                   conversation_input=dict(kind="TENSOR_BUNDLE_TOKEN_IDS", tensor_name="input_ids"),
                   input_format="OPAQUE", max_payload_bytes=1024 * 1024)
    validated = _ndnsf.NativeRequestCatalog.load(json.dumps(catalog), model_bytes,
                                                  max_source_bytes=bound,
                                                  max_assembled_bytes=bound)
    if validated.canonical_source_digest != source_digest:
        raise ValueError("native catalog returned a different source identity")

    write_json(output / "catalog.json", catalog)
    tokenizer_path = output / "tokenizer.json"
    write_json(tokenizer_path, dict(
        version="1.0", truncation=None, padding=None, added_tokens=[], normalizer=None,
        pre_tokenizer={"type": "Whitespace"}, post_processor=None, decoder=None,
        model=dict(type="WordLevel", vocab={f"token{i}": i for i in range(64)},
                   unk_token="token0")))
    (output / "prompt.bin").write_bytes(tensor_bundle([1, 2, 3]))
    (output / "delta.bin").write_bytes(tensor_bundle([4]))
    write_json(output / "generation.json", dict(
        sampling=dict(mode="Greedy", repetitionPenalty=1.0),
        maxNewTokens=2 if scenario == "tp3" else 32, outputMode="TOKEN_STREAMING", useCache=True,
        tokenizerDigest=digest(tokenizer_path.read_bytes()),
        tokenInputName="input_ids", logitsOutputName="logits",
        eosTokenIds=[63], stopStrings=[], attentionMaskInputName="attention_mask",
        positionIdsInputName="position_ids", positionInputPolicy="qwen-causal-position-v1",
        stateInputNames=state_input_names, stateOutputNames=state_output_names,
        stateSuccessorMap=",".join(successors)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="Absent directory under a target-machine workspace")
    parser.add_argument("--requester", required=True, help="Complete Requester NDN identity")
    parser.add_argument("--protection-epoch", required=True)
    parser.add_argument("--scenario", choices=("hybrid", "tp3"), default="hybrid")
    args = parser.parse_args()
    if not args.requester.startswith("/") or args.requester.endswith("/"):
        parser.error("--requester must be an absolute non-root NDN name")
    prepare(args.output.resolve(), args.requester, args.protection_epoch, args.scenario)
