#!/usr/bin/env python3
"""Generate an uncut, two-block attention/FFN source and its FP64 oracle."""

import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np
from onnx import TensorProto, checker, helper, numpy_helper, shape_inference
from onnx.reference import ReferenceEvaluator


WIDTH = 64
HEADS = 2
HEAD_WIDTH = WIDTH // HEADS
VIDEO_TOKENS = 2
TEXT_TOKENS = 3
EPSILON = 1e-4
GELU_FACTOR = math.sqrt(2 / math.pi)


class Source:
    def __init__(self, packed):
        self.packed = packed
        self.nodes = []
        self.initializers = []
        self.attention = []
        self.ffn = []

    def constant(self, name, value):
        self.initializers.append(numpy_helper.from_array(np.asarray(value), name))
        return name

    def op(self, name, operator, *inputs, **attrs):
        self.nodes.append(helper.make_node(operator, inputs, [name], name=name, **attrs))
        return name

    def linear(self, name, x, weight, seed):
        channels = np.arange(weight.size).reshape(weight.shape)
        n = weight.shape[1]
        # Every weight is exactly representable by the symmetric block-32 layout.
        quant = ((channels.T * 13 + channels.T // 7 + seed) % 15 - 7).astype(np.int8)
        scales = ((np.arange(n * 2).reshape(n, 2) + seed) % 5 + 1).astype(np.float32) / 128
        weight[:] = (quant * np.repeat(scales, 32, axis=1)).T
        if self.packed:
            u = (quant + 8).astype(np.uint8)
            stored = (u[:, ::2] | (u[:, 1::2] << 4)).reshape(n, 2, 16)
            return self.op(name, "MatMulNBits", x,
                           self.constant(name + "/weight", stored),
                           self.constant(name + "/scale", scales.ravel()),
                           domain="com.microsoft", K=WIDTH, N=n, bits=4,
                           block_size=32, accuracy_level=1)
        return self.op(name, "MatMul", x, self.constant(name + "/weight", weight))

    def biased(self, name, x, seed):
        weight = np.empty((WIDTH, WIDTH), dtype=np.float32)
        projection = self.linear(name + "/projection", x, weight, seed)
        bias = ((np.arange(WIDTH) + 3 * seed) % 13 - 6).astype(np.float32) / 128
        graph_output = self.op(name + "/bias-add", "Add", projection,
                               self.constant(name + "/bias", bias))
        return graph_output, weight.astype(np.float64), bias.astype(np.float64)

    def rms(self, name, value, seed):
        square = self.op(name + "/square", "Mul", value, value)
        mean = self.op(name + "/mean", "ReduceMean", square, "axes")
        shifted = self.op(name + "/shift", "Add", mean, "epsilon")
        root = self.op(name + "/root", "Sqrt", shifted)
        divided = self.op(name + "/normalized", "Div", value, root)
        gamma = (1 + ((np.arange(WIDTH) + seed) % 7 - 3) / 16).astype(np.float32)
        return self.op(name + "/scale", "Mul", divided,
                       self.constant(name + "/gamma", gamma)), gamma.astype(np.float64)

    def rotate(self, name, value):
        parts = [name + "/first", name + "/second"]
        self.nodes.append(helper.make_node("Split", [value, "half-head"], parts,
                                           name=name + "/split", axis=-1))
        negative = self.op(name + "/neg", "Neg", parts[1])
        rotated = self.op(name + "/rotated", "Concat", negative, parts[0], axis=-1)
        normal = self.op(name + "/cos", "Mul", value, "cos")
        shift = self.op(name + "/sin", "Mul", rotated, "sin")
        return self.op(name + "/rope", "Add", normal, shift)

    def attention_region(self, name, hidden, context, seed, feeds, reference):
        cross = context is not None
        key_input, key_ref = ("context", context) if cross else (hidden, reference)
        q, wq, bq = self.biased(name + "/q", hidden, seed)
        k, wk, bk = self.biased(name + "/k", key_input, seed + 1)
        v, wv, bv = self.biased(name + "/v", key_input, seed + 2)
        q, q_gamma = self.rms(name + "/norm-q", q, seed)
        k, k_gamma = self.rms(name + "/norm-k", k, seed + 1)
        q = self.op(name + "/q-heads", "Reshape", q, "video-heads")
        key_shape = "text-heads" if cross else "video-heads"
        k = self.op(name + "/k-heads", "Reshape", k, key_shape)
        v = self.op(name + "/v-heads", "Reshape", v, key_shape)
        if not cross:
            q = self.rotate(name + "/position-q", q)
            k = self.rotate(name + "/position-k", k)
        q = self.op(name + "/q-transpose", "Transpose", q, perm=[0, 2, 1, 3])
        k = self.op(name + "/k-transpose", "Transpose", k, perm=[0, 2, 3, 1])
        v = self.op(name + "/v-transpose", "Transpose", v, perm=[0, 2, 1, 3])
        logits = self.op(name + "/logits", "MatMul", q, k)
        logits = self.op(name + "/scaled", "Mul", logits, "score-scale")
        mask = "text-mask" if cross else "video-mask"
        logits = self.op(name + "/masked", "Add", logits, mask)
        probability = self.op(name + "/probability", "Softmax", logits, axis=-1)
        mixed = self.op(name + "/weighted", "MatMul", probability, v)
        mixed = self.op(name + "/restore-order", "Transpose", mixed, perm=[0, 2, 1, 3])
        mixed = self.op(name + "/merge-heads", "Reshape", mixed, "video-hidden")
        out, wo, bo = self.biased(name + "/o", mixed, seed + 3)
        self.attention.append({
            "kind": "cross" if cross else "self", "q": name + "/q/projection",
            "k": name + "/k/projection", "v": name + "/v/projection",
            "q_stat": name + "/norm-q/mean", "k_stat": name + "/norm-k/mean",
            "out": name + "/o/projection",
        })

        def rms(xs, coefficient):
            return xs / np.sqrt(np.mean(xs ** 2, axis=-1, keepdims=True) + EPSILON) * coefficient

        q_ref = rms(reference @ wq + bq, q_gamma).reshape(1, VIDEO_TOKENS, HEADS, HEAD_WIDTH)
        tokens = TEXT_TOKENS if cross else VIDEO_TOKENS
        k_ref = rms(key_ref @ wk + bk, k_gamma).reshape(1, tokens, HEADS, HEAD_WIDTH)
        v_ref = (key_ref @ wv + bv).reshape(1, tokens, HEADS, HEAD_WIDTH).transpose(0, 2, 1, 3)
        if not cross:
            def rotate(ref):
                first, second = np.split(ref, 2, axis=-1)
                return ref * feeds["cos"] + np.concatenate([-second, first], axis=-1) * feeds["sin"]

            q_ref, k_ref = rotate(q_ref), rotate(k_ref)
        scores = (q_ref.transpose(0, 2, 1, 3) @ k_ref.transpose(0, 2, 3, 1))
        scores = scores / math.sqrt(HEAD_WIDTH) + feeds[mask]
        probability = np.exp(scores - np.max(scores, axis=-1, keepdims=True))
        probability /= probability.sum(axis=-1, keepdims=True)
        heads = (probability @ v_ref).transpose(0, 2, 1, 3).reshape(1, VIDEO_TOKENS, WIDTH)
        return out, heads @ wo + bo

    def model(self, feeds):
        for name, value in {
            "axes": np.array([-1], dtype=np.int64),
            "epsilon": np.float32(EPSILON),
            "score-scale": np.float32(1 / math.sqrt(HEAD_WIDTH)),
            "video-heads": np.array([1, VIDEO_TOKENS, HEADS, HEAD_WIDTH], dtype=np.int64),
            "text-heads": np.array([1, TEXT_TOKENS, HEADS, HEAD_WIDTH], dtype=np.int64),
            "video-hidden": np.array([1, VIDEO_TOKENS, WIDTH], dtype=np.int64),
            "half-head": np.array([HEAD_WIDTH // 2, HEAD_WIDTH // 2], dtype=np.int64),
            "prefix-scale": np.float32(0.875),
            "suffix-scale": np.float32(1.125),
            "ffn-gate": np.float32(0.75),
            "self-gate": np.float32(0.625),
            "norm-gamma": np.ones(WIDTH, dtype=np.float32),
            "modulation-scale": np.float32(1.0625),
            "modulation-shift": np.float32(-0.03125),
        }.items():
            self.constant(name, value)

        def normalize(name, hidden, reference, modulate):
            value = self.op(name + "/layer-norm", "LayerNormalization", hidden, "norm-gamma",
                            axis=-1, epsilon=EPSILON)
            centered = reference - reference.mean(axis=-1, keepdims=True)
            expected = centered / np.sqrt((centered ** 2).mean(axis=-1, keepdims=True) + EPSILON)
            if modulate:
                value = self.op(name + "/modulated", "Mul", value, "modulation-scale")
                value = self.op(name + "/shifted", "Add", value, "modulation-shift")
                expected = expected * 1.0625 - 0.03125
            return value, expected

        hidden = self.op("prefix", "Mul", "x", "prefix-scale")
        ref = feeds["x"].astype(np.float64) * 0.875
        for block in range(2):
            prefix = "block-" + str(block)
            for cross in (False, True):
                name = prefix + ("/cross" if cross else "/self")
                inlet, expected = normalize(name, hidden, ref, not cross)
                out, value = self.attention_region(name, inlet, feeds["context"] if cross else None,
                                                   9 * block + 5 * cross, feeds, expected)
                if not cross:
                    out = self.op(name + "/gated", "Mul", out, "self-gate")
                    value *= 0.625
                hidden = self.op(name + "/residual", "Add", hidden, out)
                ref = ref + value
            inlet, expected = normalize(prefix + "/ffn", hidden, ref, True)
            up, wu, bu = self.biased(prefix + "/up", inlet, 17 + 3 * block)
            activated = self.op(prefix + "/gelu", "Gelu", up, approximate="tanh")
            down, wd, bd = self.biased(prefix + "/down", activated, 19 + 3 * block)
            gated = self.op(prefix + "/ffn-gate", "Mul", down, "ffn-gate")
            hidden = self.op(prefix + "/residual", "Add", hidden, gated)
            self.ffn.append({"up": prefix + "/up/projection", "down": prefix + "/down/projection"})
            u = expected @ wu + bu
            ref += 0.75 * ((0.5 * u * (1 + np.tanh(GELU_FACTOR * (u + 0.044715 * u ** 3))))
                           @ wd + bd)
        self.op("y", "Mul", hidden, "suffix-scale")
        info = [helper.make_tensor_value_info(n, TensorProto.FLOAT, x.shape)
                for n, x in feeds.items()]
        output = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, VIDEO_TOKENS, WIDTH])
        graph = helper.make_graph(self.nodes, "two-attention-ffn-blocks", info, [output],
                                  self.initializers)
        imports = [helper.make_opsetid("", 20)]
        if self.packed:
            imports.append(helper.make_opsetid("com.microsoft", 1))
        return helper.make_model(graph, opset_imports=imports, ir_version=9), ref * 1.125


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    target = args.output
    target.mkdir(parents=True, exist_ok=True)
    position = (np.arange(VIDEO_TOKENS).reshape(1, -1, 1, 1) * 0.3
                + np.arange(HEAD_WIDTH // 2).reshape(1, 1, 1, -1) / 31)
    position = np.concatenate([position, position], axis=-1)
    feeds = {
        "x": (((np.arange(VIDEO_TOKENS * WIDTH) * 17) % 29 - 14) / 32)
             .reshape(1, VIDEO_TOKENS, WIDTH).astype(np.float32),
        "context": (((np.arange(TEXT_TOKENS * WIDTH) * 7) % 31 - 15) / 32)
                   .reshape(1, TEXT_TOKENS, WIDTH).astype(np.float32),
        "cos": np.cos(position).astype(np.float32),
        "sin": np.sin(position).astype(np.float32),
        "video-mask": np.array([[[[0, -8], [0, 0]]]], dtype=np.float32),
        "text-mask": np.array([[[[0, -8, 0], [0, 0, -7]]]], dtype=np.float32),
    }
    dense, oracle = Source(False).model(feeds)
    source = Source(True)
    packed, matched = source.model(feeds)
    np.testing.assert_array_equal(matched, oracle)
    checker.check_model(dense, full_check=True)
    # The packed form preserves the dense operator interfaces. Record them so
    # ONNX shape inference does not have to know the ORT contrib schema.
    packed.graph.value_info.extend(shape_inference.infer_shapes(dense).graph.value_info)
    checker.check_model(packed)
    reference = ReferenceEvaluator(dense).run(None, feeds)
    np.testing.assert_allclose(reference[0], oracle, rtol=1e-5, atol=1e-6)
    (target / "dense.onnx").write_bytes(dense.SerializeToString(deterministic=True))
    (target / "model.onnx").write_bytes(packed.SerializeToString(deterministic=True))
    np.savez(target / "inputs.npz", **feeds)
    (target / "oracle.json").write_text(
        json.dumps({"tensor": "y", "shape": list(oracle.shape),
                    "float32": oracle.ravel().tolist(), "atol": 1e-6, "rtol": 1e-5}) + "\n",
        encoding="utf-8")
    # The harness supplies structural anchors and input bytes. The native catalog
    # resolves the bounds, head geometry, graph frontiers and dependencies.
    ids = {node.name: f"onnx-node-{index}" for index, node in enumerate(packed.graph.node)}
    splitter = {
        "boundary_node": "onnx-node-1",
        "suffix_node": ids["y"],
        "attention_windows": [
            {key: value if key == "kind" else ids[value] for key, value in attention.items()}
            for attention in source.attention
        ],
        "ffn_windows": [{"up_node": ids[item["up"]], "down_node": ids[item["down"]]}
                        for item in source.ffn],
    }
    (target / "source-anchors.json").write_text(json.dumps(splitter, indent=2) + "\n",
                                                encoding="utf-8")
    bundle = bytearray(b"NDITB001" + struct.pack("<I", len(feeds)))
    for name, array in feeds.items():
        key = name.encode("utf-8")
        data = array.astype("<f4").tobytes()
        bundle.extend(struct.pack("<I", len(key)) + key)
        bundle.extend(struct.pack("<II", 1, array.ndim))
        bundle.extend(struct.pack("<" + "q" * array.ndim, *array.shape))
        bundle.extend(struct.pack("<Q", len(data)) + data)
    (target / "input.bundle").write_bytes(bundle)


if __name__ == "__main__":
    main()
