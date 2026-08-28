#!/usr/bin/env python3
"""Build the deterministic stateful CPU ONNX oracle for Spec175.

The fixture is deliberately tiny but exercises the same contract shape as the
Qwen adapter: incremental token input, full-attention KV state, and
linear-attention recurrent plus convolution state.  It is not a fake runner;
all token decisions are produced by ONNX Runtime from these generated graphs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


SCHEMA = "spec175-tiny-causal-lm-v1"
VOCABULARY_SIZE = 32
HIDDEN_SIZE = 8
BLOCK_COUNT = 4
OPSET = 17

EXPECTED_OUTPUTS = {
    "normal": [4, 5, 6, 7, 8, 9, 10, 2],
    "early-eos": [14, 15, 2],
    "max-eight": [20, 21, 22, 23, 24, 25, 26, 27],
}
PROMPTS = {
    "normal": {"text": "normal", "inputIds": [3], "maxGeneratedTokens": 8},
    "early-eos": {"text": "early eos", "inputIds": [13], "maxGeneratedTokens": 8},
    "max-eight": {"text": "bounded max", "inputIds": [19], "maxGeneratedTokens": 8},
}
PARTITIONS = {
    "one-role": [(0, 4)],
    "two-role": [(0, 2), (2, 4)],
    "four-role": [(0, 1), (1, 2), (2, 3), (3, 4)],
}


def _value(name: str, data_type: int, shape: list[object]):
    return helper.make_tensor_value_info(name, data_type, shape)


def _initializer(name: str, value: np.ndarray):
    return numpy_helper.from_array(value, name=name)


def _transition_table() -> np.ndarray:
    table = np.asarray([(value + 1) % VOCABULARY_SIZE for value in range(VOCABULARY_SIZE)], dtype=np.int64)
    table[2] = 2
    table[10] = 2
    table[15] = 2
    return table


def _make_stage(
    *,
    partition: str,
    role_index: int,
    role_count: int,
    block_start: int,
    block_end: int,
) -> onnx.ModelProto:
    block_count = block_end - block_start
    first = role_index == 0
    final = role_index == role_count - 1
    state_shape = [block_count, HIDDEN_SIZE]
    nodes = []
    initializers = [
        _initializer("state_increment", np.ones(state_shape, dtype=np.float32)),
        _initializer("hidden_token_index", np.asarray([0], dtype=np.int64)),
        _initializer("hidden_valid_index", np.asarray([1], dtype=np.int64)),
        _initializer("hidden_rest_indices", np.arange(2, HIDDEN_SIZE, dtype=np.int64)),
        _initializer("float_zero", np.asarray(0.0, dtype=np.float32)),
        _initializer("float_one", np.asarray(1.0, dtype=np.float32)),
    ]
    inputs = []
    if first:
        inputs.append(_value("input_ids", TensorProto.INT64, [1, "sequence"]))
        nodes.extend([
            helper.make_node("Cast", ["input_ids"], ["token_float_2d"], to=TensorProto.FLOAT, name="embed_cast"),
            helper.make_node("Unsqueeze", ["token_float_2d", "unsqueeze_axis"], ["token_hidden"], name="embed_unsqueeze"),
            helper.make_node("Mul", ["token_hidden", "float_zero"], ["hidden_zero"], name="embed_zero"),
            helper.make_node("Add", ["hidden_zero", "float_one"], ["hidden_valid"], name="embed_valid"),
            helper.make_node(
                "Concat",
                ["token_hidden", "hidden_valid"] + ["hidden_zero"] * (HIDDEN_SIZE - 2),
                ["hidden_before_state"],
                axis=2,
                name="token_embedding",
            ),
        ])
        initializers.append(_initializer("unsqueeze_axis", np.asarray([2], dtype=np.int64)))
    else:
        inputs.append(_value("hidden_in", TensorProto.FLOAT, [1, "sequence", HIDDEN_SIZE]))
        nodes.append(helper.make_node("Identity", ["hidden_in"], ["hidden_before_state"], name="receive_hidden"))

    for family in ("attention_kv", "recurrent_state", "convolution_state"):
        inputs.append(_value(f"{family}_in", TensorProto.FLOAT, state_shape))
        nodes.extend([
            helper.make_node(
                "ReduceMean", [f"{family}_in"], [f"{family}_mean"],
                axes=[0, 1], keepdims=0, name=f"{family}_mean_block_{block_start}_{block_end}",
            ),
            helper.make_node(
                "Add", [f"{family}_in", "state_increment"], [f"{family}_out"],
                name=f"{family}_transition_block_{block_start}_{block_end}",
            ),
        ])

    nodes.extend([
        helper.make_node("Equal", ["attention_kv_mean", "recurrent_state_mean"], ["attention_equals_recurrent"], name="attention_recurrent_identity"),
        helper.make_node("Equal", ["recurrent_state_mean", "convolution_state_mean"], ["recurrent_equals_convolution"], name="recurrent_convolution_identity"),
        helper.make_node("And", ["attention_equals_recurrent", "recurrent_equals_convolution"], ["local_state_valid_bool"], name="complete_decode_state_identity"),
        helper.make_node("Cast", ["local_state_valid_bool"], ["local_state_valid"], to=TensorProto.FLOAT, name="state_valid_cast"),
        helper.make_node("Gather", ["hidden_before_state", "hidden_token_index"], ["hidden_token"], axis=2, name="take_token"),
        helper.make_node("Gather", ["hidden_before_state", "hidden_valid_index"], ["incoming_valid"], axis=2, name="take_valid"),
        helper.make_node("Gather", ["hidden_before_state", "hidden_rest_indices"], ["hidden_rest"], axis=2, name="take_rest"),
        helper.make_node("Mul", ["incoming_valid", "local_state_valid"], ["combined_valid"], name="propagate_state_validity"),
        helper.make_node("Concat", ["hidden_token", "combined_valid", "hidden_rest"], ["hidden_after_state"], axis=2, name="stateful_block_output"),
    ])

    outputs = []
    if final:
        initializers.extend([
            _initializer("transition_table", _transition_table()),
            _initializer("one_i64", np.asarray(1, dtype=np.int64)),
            _initializer("vocabulary_i64", np.asarray(VOCABULARY_SIZE, dtype=np.int64)),
            _initializer("onehot_depth", np.asarray(VOCABULARY_SIZE, dtype=np.int64)),
            _initializer("onehot_values", np.asarray([0.0, 10.0], dtype=np.float32)),
            _initializer("squeeze_axis", np.asarray([2], dtype=np.int64)),
        ])
        nodes.extend([
            helper.make_node("Squeeze", ["hidden_token", "squeeze_axis"], ["token_float"], name="head_squeeze_token"),
            helper.make_node("Cast", ["token_float"], ["token_ids"], to=TensorProto.INT64, name="head_token_ids"),
            helper.make_node("Gather", ["transition_table", "token_ids"], ["desired_ids"], axis=0, name="head_transition"),
            helper.make_node("Add", ["desired_ids", "one_i64"], ["competitor_unbounded"], name="head_competitor_add"),
            helper.make_node("Mod", ["competitor_unbounded", "vocabulary_i64"], ["competitor_ids"], fmod=0, name="head_competitor_mod"),
            helper.make_node("OneHot", ["desired_ids", "onehot_depth", "onehot_values"], ["desired_logits"], axis=-1, name="head_desired_onehot"),
            helper.make_node("OneHot", ["competitor_ids", "onehot_depth", "onehot_values"], ["competitor_logits"], axis=-1, name="head_competitor_onehot"),
            helper.make_node("Mul", ["desired_logits", "combined_valid"], ["valid_logits"], name="head_valid_logits"),
            helper.make_node("Sub", ["float_one", "combined_valid"], ["invalid_state"], name="head_invalid_state"),
            helper.make_node("Mul", ["competitor_logits", "invalid_state"], ["invalid_logits"], name="head_invalid_logits"),
            helper.make_node("Add", ["valid_logits", "invalid_logits"], ["logits"], name="head_logits"),
        ])
        outputs.append(_value("logits", TensorProto.FLOAT, [1, "sequence", VOCABULARY_SIZE]))
    else:
        nodes.append(helper.make_node("Identity", ["hidden_after_state"], ["hidden_out"], name="publish_hidden"))
        outputs.append(_value("hidden_out", TensorProto.FLOAT, [1, "sequence", HIDDEN_SIZE]))

    for family in ("attention_kv", "recurrent_state", "convolution_state"):
        outputs.append(_value(f"{family}_out", TensorProto.FLOAT, state_shape))

    graph = helper.make_graph(
        nodes,
        f"spec175-{partition}-role-{role_index}",
        inputs,
        outputs,
        initializer=initializers,
    )
    model = helper.make_model(
        graph,
        producer_name="ndnsf-spec175-fixture",
        producer_version="1",
        domain="ndnsf.org/spec175",
        model_version=1,
        opset_imports=[helper.make_operatorsetid("", OPSET)],
    )
    model.ir_version = 9
    metadata = {
        "schemaVersion": SCHEMA,
        "partition": partition,
        "roleIndex": str(role_index),
        "roleCount": str(role_count),
        "blockStart": str(block_start),
        "blockEndExclusive": str(block_end),
        "decodeStateFamilies": "attention-kv,recurrent,convolution",
    }
    for key, value in sorted(metadata.items()):
        item = model.metadata_props.add()
        item.key = key
        item.value = value
    onnx.checker.check_model(model, full_check=True)
    return model


def _sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def build(output: Path) -> None:
    output = output.resolve()
    if output.exists():
        if any(output.iterdir()):
            raise SystemExit(f"OUTPUT_NOT_EMPTY:{output}")
    else:
        output.mkdir(parents=True)

    (output / "tokenizer.json").write_text(
        json.dumps({
            "schemaVersion": "spec175-tiny-tokenizer-v1",
            "eosTokenId": 2,
            "vocabulary": {str(index): f"token-{index}" for index in range(VOCABULARY_SIZE)},
            "promptTokens": {name: value["inputIds"] for name, value in PROMPTS.items()},
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (output / "prompts.json").write_text(
        json.dumps({
            "schemaVersion": "spec175-tiny-prompts-v1",
            "cases": PROMPTS,
            "expectedOutputs": EXPECTED_OUTPUTS,
        }, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    partition_rows = {}
    for partition, ranges in PARTITIONS.items():
        directory = output / partition
        directory.mkdir()
        roles = []
        for role_index, (block_start, block_end) in enumerate(ranges):
            file_name = f"role-{role_index}.onnx"
            path = directory / file_name
            model = _make_stage(
                partition=partition,
                role_index=role_index,
                role_count=len(ranges),
                block_start=block_start,
                block_end=block_end,
            )
            path.write_bytes(model.SerializeToString(deterministic=True))
            roles.append({
                "role": f"/LLM/Pipeline/Stage/{role_index}",
                "blockStart": block_start,
                "blockEndExclusive": block_end,
                "path": f"{partition}/{file_name}",
            })
        partition_rows[partition] = roles

    content = {
        str(path.relative_to(output)): _sha256(path)
        for path in sorted(output.rglob("*"))
        if path.is_file()
    }
    manifest = {
        "schemaVersion": SCHEMA,
        "vocabularySize": VOCABULARY_SIZE,
        "hiddenSize": HIDDEN_SIZE,
        "blockCount": BLOCK_COUNT,
        "eosTokenId": 2,
        "opset": OPSET,
        "stateFamilies": ["attention-kv", "recurrent", "convolution"],
        "partitions": partition_rows,
        "expectedOutputs": EXPECTED_OUTPUTS,
        "content": content,
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    build(Path(args.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
