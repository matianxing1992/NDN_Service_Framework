#!/usr/bin/env python3
"""Prepare portable inputs for the installed Spec 242 MiniNDN deployment."""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

import onnx
from ndnsf_distributed_inference.adapters.onnx.graph import canonical_onnx_identity


def digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def prepare(output, requester, protection_epoch, source=None, source_sha256=None,
            tokenizer=None, layer_cut=None, rank_weights=None):
    repo = Path(__file__).resolve().parents[3]
    # Use the maintained host-side encoder and semantic graph identity.
    sys.path.insert(0, str(repo / "Experiments"))
    from NDNSF_DI_Qwen06B_Native_Minindn import digest_file, qwen_graph_digest, tensor_bundle

    real_model = source is not None
    if not real_model:
        source = Path(__file__).with_name("four-layer-conversation.onnx").resolve()
        source_sha256 = "sha256:52fc9b4abbc04ef28c9ca80c9e2dd40e31573998220a325acf281e1a87860fbd"
    source_digest = digest_file(source)
    if source_digest != source_sha256:
        raise ValueError("ONNX source identity changed")
    source_bytes = source.stat().st_size
    graph_digest = canonical_onnx_identity(source).graph_digest
    graph = onnx.load(source, load_external_data=False).graph
    if any(item.data_location == onnx.TensorProto.EXTERNAL for item in graph.initializer):
        raise ValueError("this workload requires one self-contained ONNX model")
    layers = {int(match[1]) for value in graph.input
              if (match := re.fullmatch(r"past_key_values\.(\d+)\.key", value.name))}
    layer_count = len(layers)
    cut = layer_cut if real_model else 1
    if layers != set(range(layer_count)) or not 0 < cut < layer_count:
        raise ValueError("expected contiguous decoder layers and a strict interior layer cut")
    partition = [[0, cut], [cut, layer_count]]
    # The Controller represents each absolute role as an NDN hierarchy.
    # Relative ranks with embedded slashes would become one escaped component.
    stages = ["/Stage/First", "/Stage/Last"]
    degrees = [len(rank_weights) if rank_weights is not None else 2, 1]
    schema_digest = digest(b"spec242-planning-graph")
    manifest_digest = (digest(b"spec234-qwen3-0.6b-int4:" + source_digest.encode())
                       if real_model else digest(b"spec242-four-layer-conversation:" + source.read_bytes()))
    adapter = dict(name="qwen", version="1", abi="onnxruntime", model_formats=["onnx"],
                   tasks=["text-generation"], backends=["onnxruntime"], precisions=["int4"],
                   graph_inspectable=True, splittable=True, deterministic_analysis=True,
                   state_digest=schema_digest)
    for field in ("input", "options", "result", "graph", "split", "state"):
        adapter[field + "_schema_digest"] = schema_digest
    model_name, revision = ("Qwen3-0.6B", source_digest[7:]) if real_model else (
        "qwen-fixture", "fixture-v1")
    model = dict(model_name=model_name, source_revision=revision,
                 content_digest=source_digest, semantics_digest=schema_digest,
                 graph_digest=qwen_graph_digest(model_name, revision, "int4", partition),
                 model_format="onnx", precision="int4", quantization_subtype="weight_only_int4",
                 adapter=adapter)
    bound = source_bytes + (128 << 20) if real_model else 8 << 20
    state_budget = 16 << 20 if real_model else 128 << 10
    profiles = {stage: dict(backends=["onnxruntime"], weight_bytes=bound, workspace_bytes=bound,
                            kv_bytes=state_budget, activation_bytes=state_budget, transient_bytes=bound,
                            safety_margin=1.0) for stage in stages}
    splitter = dict(kind="QWEN", roles=stages, layer_ranges=partition, tensor_degrees=degrees,
                    artifact_digests_by_role={stage: digest(stage.encode()) for stage in stages},
                    rank_artifact_digests_by_role={stages[0]: [
                        digest(f"first-r{rank}".encode()) for rank in range(degrees[0])]},
                    weight_bytes_by_role={stage: bound for stage in stages},
                    resources_by_role=profiles, input_ingress_role=stages[0],
                    result_egress_role=stages[-1])
    if rank_weights is not None:
        splitter["tensor_rank_weights_by_role"] = {stages[0]: rank_weights}
    node_labels = []
    consumers = {}
    for index, node in enumerate(graph.node):
        match = re.search(r"layers\.(\d+)(?:/|$)", node.name)
        if match and int(match[1]) < partition[-1][1]:
            label = f"layer-{int(match[1]):02d}"
        elif match or "lm_head" in node.name:
            label = "final-norm-head"
        else:
            label = "embedding"
        node_labels.append(label)
        for name in node.input:
            if name:
                consumers.setdefault(name, set()).add(index)
    # The frozen export transposes the tied embedding table solely for lm_head.
    # Keep that initializer transform with the head, as the native entry classifier does.
    initializer_names = {item.name for item in graph.initializer}
    for index, node in enumerate(graph.node):
        if node_labels[index] != "embedding" or node.op_type != "Transpose":
            continue
        if len(node.input) != 1 or node.input[0] not in initializer_names:
            continue
        readers = {reader for name in node.output for reader in consumers.get(name, ())}
        if len(readers) == 1 and node_labels[next(iter(readers))] == "final-norm-head":
            node_labels[index] = "final-norm-head"
    node_mapping = {}
    for index, label in enumerate(node_labels):
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
    model_prefix = "/Model/Spec234" if real_model else "/Model/Spec242"
    catalog = dict(schema="ndnsf-di-native-request-catalog-v1", model=model, splitter=splitter,
                   source=dict(file=str(source), bytes=source_bytes, initializer_bytes=0,
                               data_name=requester + model_prefix + "/source", digest=source_digest,
                               model_manifest_digest=manifest_digest,
                               canonical_graph_digest=graph_digest),
                   recipe=dict(artifact_profile_digest=schema_digest,
                               assembler_descriptor_digest=schema_digest,
                               backend_abi="onnxruntime", precision="int4",
                               quantization="weight_only_int4", layout="BNSH", padding="none",
                               protection_epoch=protection_epoch, max_source_bytes=bound,
                               max_assembled_bytes=bound, max_nodes=max(512, len(graph.node))),
                   publication=dict(artifact_root=model_prefix + "/artifacts",
                                    package_manifest_digest=manifest_digest,
                                    max_publication_bytes=2 * bound if real_model else bound),
                   node_mapping=node_mapping, state_inputs=state_inputs, state_outputs=state_outputs,
                   conversation_input=dict(kind="TENSOR_BUNDLE_TOKEN_IDS", tensor_name="input_ids"),
                   input_format="OPAQUE", max_payload_bytes=1024 * 1024)
    if not real_model:
        from ndnsf import _ndnsf

        validated = _ndnsf.NativeRequestCatalog.load(json.dumps(catalog), source.read_bytes(),
                                                     max_source_bytes=bound,
                                                     max_assembled_bytes=bound)
        if validated.canonical_source_digest != source_digest:
            raise ValueError("native catalog returned a different source identity")

    output.mkdir(mode=0o700)
    write_json(output / "catalog.json", catalog)
    tokenizer_path = output / "tokenizer.json"
    if real_model:
        tokenizer_path.write_bytes(tokenizer.read_bytes())
    else:
        write_json(tokenizer_path, dict(
            version="1.0", truncation=None, padding=None, added_tokens=[], normalizer=None,
            pre_tokenizer={"type": "Whitespace"}, post_processor=None, decoder=None,
            model=dict(type="WordLevel", vocab={f"token{i}": i for i in range(64)},
                       unk_token="token0")))
    (output / "prompt.bin").write_bytes(tensor_bundle([1, 2, 3]))
    (output / "delta.bin").write_bytes(tensor_bundle([4]))
    write_json(output / "generation.json", dict(
        sampling=dict(mode="Greedy", repetitionPenalty=1.0),
        maxNewTokens=9 if real_model else 32, outputMode="TOKEN_STREAMING", useCache=True,
        tokenizerDigest=digest(tokenizer_path.read_bytes()),
        tokenInputName="input_ids", logitsOutputName="logits",
        eosTokenIds=[151645] if real_model else [63], stopStrings=[], attentionMaskInputName="attention_mask",
        positionIdsInputName="position_ids", positionInputPolicy="qwen-causal-position-v1",
        stateInputNames=state_input_names, stateOutputNames=state_output_names,
        stateSuccessorMap=",".join(successors)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="Absent directory under a target-machine workspace")
    parser.add_argument("--requester", required=True, help="Complete Requester NDN identity")
    parser.add_argument("--protection-epoch", required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--source-sha256")
    parser.add_argument("--tokenizer", type=Path)
    parser.add_argument("--layer-cut", type=int)
    parser.add_argument("--rank-weights", nargs="+", type=int)
    args = parser.parse_args()
    if not args.requester.startswith("/") or args.requester.endswith("/"):
        parser.error("--requester must be an absolute non-root NDN name")
    source_options = (args.source_sha256, args.tokenizer, args.layer_cut)
    if args.source and any(value is None for value in source_options):
        parser.error("--source requires --source-sha256, --tokenizer and --layer-cut")
    if not args.source and (any(value is not None for value in source_options) or args.rank_weights):
        parser.error("model source options require --source")
    if args.rank_weights is not None and len(args.rank_weights) not in (2, 3):
        parser.error("the MiniNDN qualification layout requires two or three rank weights")
    prepare(args.output.resolve(), args.requester, args.protection_epoch,
            args.source.resolve(strict=True) if args.source else None, args.source_sha256,
            args.tokenizer, args.layer_cut, args.rank_weights)
