#!/usr/bin/env python3
"""Export static Wan ONNX source for the native block partition catalog."""

import argparse
from collections import Counter
from contextlib import ExitStack
import hashlib
import json
import math
from pathlib import Path
import resource
import struct
import time

import numpy as np
import onnx
from onnx import TensorProto, helper
import torch
from diffusers.models.transformers.transformer_wan import (
    WanRotaryPosEmbed,
    WanTransformer3DModel,
    WanTransformerBlock,
)
from safetensors import safe_open


RECIPE = json.loads(Path(__file__).with_name("recipes").joinpath(
    "spec263-wan-source.json").read_text())


def fit_int4_scales(packed, data, scales, block):
    """Reduce blockwise weight error without using the qualification input."""
    source = np.ascontiguousarray(data.T).reshape(-1, block)
    result = packed.reshape(-1, block // 2)
    factors = scales.reshape(-1)
    for start in range(0, len(source), 8192):
        stop = start + 8192
        values = source[start:stop]
        current = result[start:stop]
        best_scale = factors[start:stop].copy()
        old = np.empty(values.shape, np.float32)
        old[:, 0::2] = current & 15
        old[:, 1::2] = current >> 4
        old -= 8
        residual = values - old * best_scale[:, None]
        best_error = np.einsum("ij,ij->i", residual, residual)
        best_q = old.astype(np.int8)
        initial = np.abs(best_scale)
        for factor in (1, 0.9, 1.1, 0.8, 1.2, 0.7, 1.3):
            for sign in (1, -1):
                scale = sign * factor * initial
                for _ in range(2):
                    safe_scale = np.where(scale != 0, scale, 1)
                    q = np.clip(np.rint(values / safe_scale[:, None]), -8, 7)
                    denominator = np.einsum("ij,ij->i", q, q)
                    scale = np.einsum("ij,ij->i", q, values) / np.maximum(denominator, 1)
                    residual = values - q * scale[:, None]
                    error = np.einsum("ij,ij->i", residual, residual)
                    better = error < best_error
                    best_error[better] = error[better]
                    best_scale[better] = scale[better]
                    best_q[better] = q[better]
        nibble = (best_q + 8).astype(np.uint8)
        current[:] = nibble[:, 0::2] | (nibble[:, 1::2] << 4)
        factors[start:stop] = best_scale


class SourceGraph:
    def __init__(self, sidecar, packed_range=None):
        self.sidecar = sidecar
        self.packed_range = packed_range
        self.nodes = []
        self.initializers = []
        self.inputs = {}
        self.shapes = {}

    def input(self, name, shape):
        self.inputs[name] = tuple(shape)
        self.shapes[name] = tuple(shape)
        return name

    def operator(self, name, op, inputs, shape, domain="", **attributes):
        self.shapes[name] = tuple(shape)
        self.nodes.append(helper.make_node(op, inputs, [name], name=name, domain=domain,
                                           **attributes))
        return name

    def constant(self, name, value, dtype=np.float32):
        array = np.asarray(value, dtype=np.dtype(dtype).newbyteorder("<"), order="C")
        self.shapes[name] = array.shape
        if array.nbytes >= 1 << 20:
            proto = TensorProto(name=name, data_type=helper.np_dtype_to_tensor_dtype(array.dtype),
                                dims=array.shape, data_location=TensorProto.EXTERNAL)
            for key, val in (("location", "model.onnx.data"), ("offset", self.sidecar.tell()),
                             ("length", array.nbytes)):
                proto.external_data.add(key=key, value=str(val))
            self.sidecar.write(memoryview(array).cast("B"))
        else:
            proto = onnx.numpy_helper.from_array(array, name)
        self.initializers.append(proto)
        return name

    def linear(self, name, data, weight, bias, columns=None, quantize=True):
        # ONNX MatMul uses [K,N], while the checkpoint stores [N,K].
        if columns is not None:
            weight, bias = weight[columns], bias[columns]
        shape = self.shapes[data][:-1] + (weight.shape[0],)
        bits = None
        if self.packed_range and quantize:
            layer = int(name.partition("/")[0].removeprefix("block-"))
            first, end = self.packed_range
            if not first <= layer < end:
                bits = 8
            elif not name.endswith(("/q", "/k")):
                bits = 4
        if bits:
            from onnxruntime.capi._pybind_state import quantize_matmul_4bits, quantize_matmul_8bits

            n, k = weight.shape
            block = 32
            if k % block:
                raise ValueError(f"{name} input is not aligned to block-32 INT{bits}")
            packed = np.zeros((n, k // block, block * bits // 8), np.uint8)
            zero = np.zeros((n, (k // block * bits + 7) // 8), np.uint8)
            scales = np.zeros((n, k // block), np.float32)
            matrix = np.ascontiguousarray(weight.T, dtype=np.float32)
            pack = quantize_matmul_4bits if bits == 4 else quantize_matmul_8bits
            pack(packed, matrix, scales, zero, block, n, k, True)
            if bits == 4:
                fit_int4_scales(packed, matrix, scales, block)
            rhs = self.constant(name + "/weight", packed, np.uint8)
            scale = self.constant(name + "/weight-scale", scales.reshape(-1))
            projection = self.operator(name + "/projection", "MatMulNBits",
                                       [data, rhs, scale], shape, domain="com.microsoft",
                                       K=k, N=n, bits=bits, block_size=block, accuracy_level=1)
        else:
            rhs = self.constant(name + "/weight", weight.T)
            projection = self.operator(name + "/projection", "MatMul", [data, rhs], shape)
        offset = self.constant(name + "/bias", bias)
        return projection, self.operator(name + "/biased", "Add", [projection, offset], shape)

    def rms(self, name, data, gamma, epsilon, axes, columns):
        if columns is not None:
            gamma = gamma[columns]
        shape = self.shapes[data]
        scale = self.constant(name + "/gamma", gamma)
        square = self.operator(name + "/square", "Mul", [data, data], shape)
        stats = shape[:-1] + (1,)
        mean = self.operator(name + "/mean", "ReduceMean", [square, axes], stats, keepdims=1)
        shifted = self.operator(name + "/epsilon", "Add", [mean, epsilon], stats)
        root = self.operator(name + "/root", "Sqrt", [shifted], stats)
        unit = self.operator(name + "/unit", "Div", [data, root], shape)
        return mean, self.operator(name + "/output", "Mul", [unit, scale], shape)

    def reshape(self, name, data, shape):
        target = self.constant(name + "/shape", shape, np.int64)
        return self.operator(name, "Reshape", [data, target], shape)

    def rotate(self, name, data, cos, sin, head_width):
        shape = self.shapes[data]
        half = head_width // 2
        sizes = self.constant(name + "/sizes", [half, half], np.int64)
        real, imag = name + "/real", name + "/imag"
        self.nodes.append(helper.make_node("Split", [data, sizes], [real, imag],
                                           name=name + "/split", axis=-1))
        self.shapes[real] = self.shapes[imag] = shape[:-1] + (half,)
        neg = self.operator(name + "/negative", "Neg", [imag], self.shapes[imag])
        conjugate = self.operator(name + "/shuffle", "Concat", [neg, real], shape, axis=-1)
        direct = self.operator(name + "/cos", "Mul", [data, cos], shape)
        shift = self.operator(name + "/sin", "Mul", [conjugate, sin], shape)
        return self.operator(name + "/rope", "Add", [direct, shift], shape)

    def attention(self, name, hidden, condition, position, mask, weight, heads, head_width,
                  epsilon, axes):
        self_attention = condition is None
        memory = hidden if self_attention else condition
        n = self.shapes[hidden][1]
        m = self.shapes[memory][1]
        batch, _, width = self.shapes[hidden]
        if self_attention:
            # P maps [r0,i0,r1,i1,...] to [r0,r1,...,i0,i1,...] inside each head.
            # Apply P to Q and K only. QK^T is invariant because both use P.
            order = rotary_coordinates(head_width)
            columns = (np.arange(heads)[:, None] * head_width + order).reshape(-1)
        else:
            columns = None
        q, query = self.linear(name + "/q", hidden, weight("to_q.weight"),
                               weight("to_q.bias"), columns)
        k, key = self.linear(name + "/k", memory, weight("to_k.weight"),
                             weight("to_k.bias"), columns)
        v, value = self.linear(name + "/v", memory, weight("to_v.weight"), weight("to_v.bias"))
        q_stat, query = self.rms(name + "/norm-q", query, weight("norm_q.weight"),
                                 epsilon, axes, columns)
        k_stat, key = self.rms(name + "/norm-k", key, weight("norm_k.weight"),
                               epsilon, axes, columns)

        query = self.reshape(name + "/q-heads", query, (batch, n, heads, head_width))
        key = self.reshape(name + "/k-heads", key, (batch, m, heads, head_width))
        value = self.reshape(name + "/v-heads", value, (batch, m, heads, head_width))
        if self_attention:
            cos, sin = position
            query = self.rotate(name + "/q-pos", query, cos, sin, head_width)
            key = self.rotate(name + "/k-pos", key, cos, sin, head_width)
        query = self.operator(name + "/q-transpose", "Transpose", [query],
                              (batch, heads, n, head_width), perm=[0, 2, 1, 3])
        key = self.operator(name + "/k-transpose", "Transpose", [key],
                            (batch, heads, head_width, m), perm=[0, 2, 3, 1])
        value = self.operator(name + "/v-transpose", "Transpose", [value],
                              (batch, heads, m, head_width), perm=[0, 2, 1, 3])
        scores_shape = (batch, heads, n, m)
        scores = self.operator(name + "/qk", "MatMul", [query, key], scores_shape)
        factor = self.constant(name + "/scale", 1.0 / math.sqrt(head_width))
        scores = self.operator(name + "/scaled", "Mul", [scores, factor], scores_shape)
        scores = self.operator(name + "/masked", "Add", [scores, mask], scores_shape)
        prob = self.operator(name + "/softmax", "Softmax", [scores], scores_shape, axis=-1)
        weighted = self.operator(name + "/values", "MatMul", [prob, value],
                                 (batch, heads, n, head_width))
        ordered = self.operator(name + "/ordered", "Transpose", [weighted],
                                (batch, n, heads, head_width), perm=[0, 2, 1, 3])
        merged = self.reshape(name + "/merge", ordered, (batch, n, width))
        out, biased = self.linear(name + "/o", merged, weight("to_out.0.weight"),
                                  weight("to_out.0.bias"))
        anchors = {"kind": "self" if self_attention else "cross", "q": q, "k": k, "v": v,
                   "q_stat": q_stat, "k_stat": k_stat, "out": out}
        return biased, anchors

    def wan_block(self, name, hidden, condition, modulation, position, masks, weight, config):
        shape = self.shapes[hidden]
        width = shape[-1]
        heads = config["num_attention_heads"]
        head_width = config["attention_head_dim"]
        epsilon = config["eps"]
        ff = config["ffn_dim"]
        initial_node = len(self.nodes)

        table = self.constant(name + "/scale-shift", weight("scale_shift_table"))
        mod = self.operator(name + "/modulation", "Add", [modulation, table], (1, 6, width))
        sizes = self.constant(name + "/mod-sizes", np.ones(6, dtype=np.int64), np.int64)
        parts = [name + "/mod-" + str(i) for i in range(6)]
        self.nodes.append(helper.make_node("Split", [mod, sizes], parts,
                                           name=name + "/mod-split", axis=1))
        for part in parts:
            self.shapes[part] = (1, 1, width)
        one = self.constant(name + "/one", 1)
        affine = self.constant(name + "/unit-gamma", np.ones(width))
        rms_axes = self.constant(name + "/rms-axes", [-1], np.int64)
        rms_eps = self.constant(name + "/rms-epsilon", epsilon)

        def modulated(stage, data, shift, scale):
            norm = self.operator(stage + "/norm", "LayerNormalization", [data, affine],
                                 shape, axis=-1, epsilon=epsilon)
            gain = self.operator(stage + "/gain", "Add", [scale, one], (1, 1, width))
            scaled = self.operator(stage + "/scaled", "Mul", [norm, gain], shape)
            return self.operator(stage + "/shifted", "Add", [scaled, shift], shape)

        def residual(stage, previous, value, gate=None):
            if gate is not None:
                value = self.operator(stage + "/gated", "Mul", [value, gate], shape)
            return self.operator(stage + "/residual", "Add", [previous, value], shape)

        self_input = modulated(name + "/self-input", hidden, parts[0], parts[1])
        output, self_anchors = self.attention(name + "/self", self_input, None, position,
            masks[0], lambda key: weight("attn1." + key), heads, head_width, rms_eps, rms_axes)
        hidden = residual(name + "/self", hidden, output, parts[2])

        gamma = self.constant(name + "/cross-gamma", weight("norm2.weight"))
        beta = self.constant(name + "/cross-beta", weight("norm2.bias"))
        cross_input = self.operator(name + "/cross-input", "LayerNormalization",
                                    [hidden, gamma, beta], shape, axis=-1, epsilon=epsilon)
        output, cross_anchors = self.attention(name + "/cross", cross_input, condition,
            position, masks[1], lambda key: weight("attn2." + key),
            heads, head_width, rms_eps, rms_axes)
        hidden = residual(name + "/cross", hidden, output)

        ffn_input = modulated(name + "/ffn-input", hidden, parts[3], parts[4])
        up, lifted = self.linear(name + "/up", ffn_input, weight("ffn.net.0.proj.weight"),
                                 weight("ffn.net.0.proj.bias"))
        activated = self.operator(name + "/gelu", "Gelu", [lifted], shape[:-1] + (ff,),
                                  approximate="tanh")
        down, biased = self.linear(name + "/down", activated, weight("ffn.net.2.weight"),
                                   weight("ffn.net.2.bias"))
        output = residual(name + "/ffn", hidden, biased, parts[5])
        return output, {"begin": initial_node, "end": len(self.nodes),
                        "attention": [self_anchors, cross_anchors], "ffn": [up, down]}

    def save(self, path, output):
        def float_port(name, shape):
            return helper.make_tensor_value_info(name, TensorProto.FLOAT, shape)

        initialized = {init.name for init in self.initializers}
        graph = helper.make_graph(self.nodes, "spec263-wan-source",
                                  [float_port(*item) for item in self.inputs.items()],
                                  [float_port(output, self.shapes[output])], self.initializers)
        for name, shape in self.shapes.items():
            if name not in self.inputs and name not in initialized and name != output:
                graph.value_info.append(float_port(name, shape))
        opsets = [helper.make_opsetid("", 20)]
        if self.packed_range:
            opsets.append(helper.make_opsetid("com.microsoft", 1))
        model = helper.make_model(graph, ir_version=9, opset_imports=opsets)
        path.write_bytes(model.SerializeToString(deterministic=True))


def rotary_coordinates(head_width):
    if head_width % 2:
        raise ValueError("Wan rotary coordinates require an even head width")
    return np.r_[np.arange(0, head_width, 2), np.arange(1, head_width, 2)]


def checkpoint(root, stack):
    manifest = root / "diffusion_pytorch_model.safetensors.index.json"
    mapping = json.loads(manifest.read_text())["weight_map"]
    shards = {}

    def weight(name):
        filename = mapping[name]
        if filename not in shards:
            shards[filename] = stack.enter_context(safe_open(root / filename, framework="np"))
        return shards[filename].get_tensor(name)

    return weight, mapping


def check_config(config):
    expected = {"num_layers": 30, "num_attention_heads": 12, "attention_head_dim": 128,
                "text_dim": 4096, "ffn_dim": 8960, "cross_attn_norm": True,
                "qk_norm": "rms_norm_across_heads", "patch_size": [1, 2, 2],
                "added_kv_proj_dim": None, "image_dim": None}
    if any(config.get(key) != value for key, value in expected.items()):
        raise ValueError("checkpoint configuration differs from the Spec 263 Wan profile")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_files(root, expected, choices=None):
    names = sorted(choices if choices is not None else expected)
    observed = {name: digest(root / name) for name in names}
    if any(observed[name] != expected[name] for name in names):
        raise ValueError("checkpoint bytes differ from the frozen Spec 263 source")
    return observed


def probe_block(root, output, index):
    import onnxruntime as ort

    started = time.monotonic()
    config = json.loads((root / "config.json").read_text())
    check_config(config)
    if index not in range(config["num_layers"]):
        raise ValueError("block index is outside the confirmed checkpoint")
    torch.set_num_threads(4)
    torch.set_num_interop_threads(1)
    heads, head_width = config["num_attention_heads"], config["attention_head_dim"]
    width = heads * head_width
    seed = RECIPE["input"]["latent_seed"]
    generator = torch.Generator().manual_seed(seed)

    def values(*shape):
        return 0.125 * torch.randn(*shape, generator=generator)

    hidden, context, mod = values(1, 64, width), values(1, 12, width), values(1, 6, width)
    rope = WanRotaryPosEmbed(head_width, tuple(config["patch_size"]), config["rope_max_seq_len"])
    cosine, sine = rope(torch.zeros(1, config["in_channels"], 1, 16, 16))
    prefix = f"blocks.{index}."
    with ExitStack() as stack:
        weight, mapping = checkpoint(root, stack)
        shards = {file for key, file in mapping.items() if key.startswith(prefix)}
        observed = verify_files(root, RECIPE["transformer"]["files"], shards)
        reference = WanTransformerBlock(width, config["ffn_dim"], heads, config["qk_norm"],
                                        config["cross_attn_norm"], config["eps"]).eval()
        reference.load_state_dict({key[len(prefix):]: torch.from_numpy(weight(key).copy())
                                   for key in mapping if key.startswith(prefix)}, strict=True)
        with torch.inference_mode():
            expected = reference(hidden, context, mod, (cosine, sine)).numpy()
        del reference
        order = rotary_coordinates(head_width)
        feeds = {"hidden": hidden.numpy(), "context": context.numpy(), "modulation": mod.numpy(),
                 "cos": cosine.numpy()[..., order].astype(np.float32),
                 "sin": sine.numpy()[..., order].astype(np.float32),
                 "self-mask": np.zeros((1, 1, 1, 64), np.float32),
                 "cross-mask": np.zeros((1, 1, 1, 12), np.float32)}
        model = output / "model.onnx"
        sidecar = output / "model.onnx.data"
        with sidecar.open("wb") as stream:
            graph = SourceGraph(stream)
            for name, data in feeds.items():
                graph.input(name, data.shape)
            terminal, anchors = graph.wan_block(
                f"block-{index}", "hidden", "context", "modulation", ("cos", "sin"),
                ("self-mask", "cross-mask"), lambda key: weight(prefix + key), config)
            graph.save(model, terminal)

    onnx.checker.check_model(str(model))
    options = ort.SessionOptions()
    options.intra_op_num_threads = 4
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(str(model), options, providers=["CPUExecutionProvider"])
    actual, = session.run([terminal], feeds)
    atol, rtol = 5e-5, 5e-5
    assert actual.shape == expected.shape and np.isfinite(actual).all()
    np.testing.assert_allclose(actual, expected, atol=atol, rtol=rtol)
    np.savez(output / "reference.npz", **feeds, expected=expected)
    result = {"block": index, "seed": seed, "shape": list(expected.shape), "atol": atol, "rtol": rtol,
              "maxAbs": float(np.max(np.abs(actual - expected))),
              "nodes": len(graph.nodes), "operators": dict(Counter(node.op_type for node in graph.nodes)),
              "graphSha256": digest(model), "sidecarSha256": digest(sidecar),
              "sourceShards": observed,
              "anchors": anchors, "elapsedSeconds": time.monotonic() - started,
              "peakRssKiB": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)


def full_graph(graph, weight, config, tokens, first, end):
    width = config["num_attention_heads"] * config["attention_head_dim"]
    channels = config["in_channels"]
    dim = config["freq_dim"]
    qpos = (1, 64, 1, config["attention_head_dim"])
    latent = graph.input("latent", (1, channels, 1, 16, 16))
    time_input = graph.input("timestep", (1,))
    text = graph.input("text-conditioning", (1, tokens, config["text_dim"]))
    position = (graph.input("cos", qpos), graph.input("sin", qpos))
    masks = (graph.input("self-mask", (1, 1, 1, 64)),
             graph.input("cross-mask", (1, 1, 1, tokens)))

    patch = config["patch_size"]
    kernel = graph.constant("patch/weight", weight("patch_embedding.weight"))
    bias = graph.constant("patch/bias", weight("patch_embedding.bias"))
    projected = graph.operator("patch/conv", "Conv", [latent, kernel, bias], (1, width, 1, 8, 8),
                               kernel_shape=patch, strides=patch)
    packed = graph.reshape("patch/flatten", projected, (1, width, 64))
    hidden = graph.operator("patch/tokens", "Transpose", [packed], (1, 64, width),
                            perm=[0, 2, 1])

    def linear(path, data):
        source = "condition_embedder." + path
        _, value = graph.linear(path.replace(".", "/"), data, weight(source + ".weight"),
                                weight(source + ".bias"), quantize=False)
        return value

    def silu(name, data):
        shape = graph.shapes[data]
        gate = graph.operator(name + "/sigmoid", "Sigmoid", [data], shape)
        return graph.operator(name, "Mul", [data, gate], shape)

    half = dim // 2
    exponent = -math.log(10000.0) * torch.arange(half, dtype=torch.float32) / half
    freq = graph.constant("time/frequency", torch.exp(exponent).numpy())
    phase = graph.operator("time/phase", "Mul", [time_input, freq], (half,))
    phase = graph.reshape("time/row", phase, (1, half))
    cos = graph.operator("time/cos", "Cos", [phase], (1, half))
    sin = graph.operator("time/sin", "Sin", [phase], (1, half))
    features = graph.operator("time/features", "Concat", [cos, sin], (1, dim), axis=1)
    value = linear("time_embedder.linear_1", features)
    value = silu("time/intermediate", value)
    timestep = linear("time_embedder.linear_2", value)
    value = silu("time/modulated", timestep)
    modulation = graph.reshape("time/modulation", linear("time_proj", value), (1, 6, width))

    value = linear("text_embedder.linear_1", text)
    value = graph.operator("text/activation", "Gelu", [value], graph.shapes[value],
                           approximate="tanh")
    condition = linear("text_embedder.linear_2", value)

    windows = []
    for index in range(config["num_layers"]):
        prefix = f"blocks.{index}."
        hidden, anchors = graph.wan_block(f"block-{index}", hidden, condition, modulation,
                                          position, masks, lambda key: weight(prefix + key),
                                          config)
        windows.append(anchors)

    # The final head has its own two-row modulation and restores the latent grid.
    time_row = graph.reshape("output/time-row", timestep, (1, 1, width))
    table = graph.constant("output/scale-shift", weight("scale_shift_table"))
    params = graph.operator("output/modulation", "Add", [time_row, table], (1, 2, width))
    sizes = graph.constant("output/sizes", [1, 1], np.int64)
    shift, scale = "output/shift", "output/scale"
    graph.nodes.append(helper.make_node("Split", [params, sizes], [shift, scale],
                                        name="output/mod-split", axis=1))
    graph.shapes[shift] = graph.shapes[scale] = (1, 1, width)
    gamma = graph.constant("output/unit-gamma", np.ones(width))
    one = graph.constant("output/one", 1.0)
    shape = graph.shapes[hidden]
    value = graph.operator("output/norm", "LayerNormalization", [hidden, gamma], shape,
                           axis=-1, epsilon=config["eps"])
    gain = graph.operator("output/gain", "Add", [scale, one], (1, 1, width))
    value = graph.operator("output/scaled", "Mul", [value, gain], shape)
    value = graph.operator("output/shifted", "Add", [value, shift], shape)
    _, value = graph.linear("output/project", value, weight("proj_out.weight"),
                            weight("proj_out.bias"), quantize=False)
    pt, ph, pw = patch
    out = config["out_channels"]
    features = graph.reshape("output/patch-grid", value, (1, 1, 8, 8, pt, ph, pw, out))
    unpatch = graph.operator("output/unpatch", "Transpose", [features],
                             (1, out, 1, pt, 8, ph, 8, pw), perm=[0, 7, 1, 4, 2, 5, 3, 6])
    output = graph.reshape("flow", unpatch, (1, out, 1, 16, 16))

    ids = {node.name: f"onnx-node-{i}" for i, node in enumerate(graph.nodes)}
    selected = windows[first:end]
    anchors = {
        "boundary_node": f"onnx-node-{selected[0]['begin']}",
        "suffix_node": f"onnx-node-{selected[-1]['end']}",
        "attention_windows": [
            {key: value if key == "kind" else ids[value] for key, value in attention.items()}
            for window in selected for attention in window["attention"]
        ],
        "ffn_windows": [{"up_node": ids[window["ffn"][0]], "down_node": ids[window["ffn"][1]]}
                        for window in selected],
    }
    return output, anchors


def export_full(root, output, config, tokens, first, end, packed):
    started = time.monotonic()
    model, sidecar = output / "model.onnx", output / "model.onnx.data"
    with ExitStack() as stack:
        weights, _ = checkpoint(root, stack)
        with sidecar.open("wb") as stream:
            graph = SourceGraph(stream, packed_range=(first, end) if packed else None)
            terminal, anchors = full_graph(graph, weights, config, tokens, first, end)
            graph.save(model, terminal)
    onnx.checker.check_model(str(model))
    (output / "source-anchors.json").write_text(json.dumps(anchors, indent=2) + "\n")
    result = {"nodes": len(graph.nodes), "operators": dict(Counter(node.op_type for node in graph.nodes)),
              "blockRange": [first, end], "contextTokens": tokens,
              "graphSha256": digest(model), "sidecarSha256": digest(sidecar),
              "sidecarBytes": sidecar.stat().st_size, "elapsedSeconds": time.monotonic() - started,
              "peakRssKiB": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}
    (output / "export.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)


def tensor_bundle(feeds):
    result = bytearray(b"NDITB001" + struct.pack("<I", len(feeds)))
    for name, value in feeds.items():
        key = name.encode("utf-8")
        data = np.asarray(value, "<f4", order="C").tobytes()
        result.extend(struct.pack("<I", len(key)) + key)
        result.extend(struct.pack("<II", 1, value.ndim))
        result.extend(struct.pack("<" + "q" * value.ndim, *value.shape))
        result.extend(struct.pack("<Q", len(data)) + data)
    return result


def reference_full(root, output, config, conditioning, timestep):
    started = time.monotonic()
    tokens = np.load(conditioning, allow_pickle=False)
    if tokens.ndim != 3 or tokens.shape[0] != 1 or tokens.shape[2] != config["text_dim"] or \
            not 1 <= tokens.shape[1] <= 512 or not np.isfinite(tokens).all():
        raise ValueError("expected finite offline Wan text embeddings with shape [1,T,4096]")
    tokens = np.ascontiguousarray(tokens, dtype=np.float32)
    width = config["attention_head_dim"]
    torch.set_num_threads(4)
    torch.set_num_interop_threads(1)
    generator = torch.Generator().manual_seed(RECIPE["input"]["latent_seed"])
    latent = torch.randn((1, config["in_channels"], 1, 16, 16), generator=generator)
    times = torch.tensor([timestep], dtype=torch.float32)
    position = WanRotaryPosEmbed(width, tuple(config["patch_size"]), config["rope_max_seq_len"])
    cos, sin = position(latent)
    order = rotary_coordinates(width)
    feeds = {"latent": latent.numpy(), "text-conditioning": tokens,
             "timestep": times.numpy(),
             "cos": cos.numpy()[..., order].astype(np.float32),
             "sin": sin.numpy()[..., order].astype(np.float32),
             "self-mask": np.zeros((1, 1, 1, 64), np.float32),
             "cross-mask": np.zeros((1, 1, 1, tokens.shape[1]), np.float32)}
    model = WanTransformer3DModel.from_pretrained(
        str(root), local_files_only=True, torch_dtype=torch.float32,
        low_cpu_mem_usage=True, use_safetensors=True).eval()
    with torch.inference_mode():
        expected = model(latent, times, torch.from_numpy(tokens)).sample.numpy()
    if expected.shape != (1, config["out_channels"], 1, 16, 16) or not np.isfinite(expected).all():
        raise ValueError("invalid shape or non-finite reference output")
    (output / "input.bundle").write_bytes(tensor_bundle(feeds))
    np.savez(output / "reference.npz", **feeds, expected=expected)
    oracle = {"tensor": "flow", "shape": list(expected.shape),
              "float32": expected.reshape(-1).tolist(), "atol": 1e-3, "rtol": 1e-3}
    (output / "oracle.json").write_text(json.dumps(oracle) + "\n")
    evidence = {"timestep": timestep, "contextTokens": tokens.shape[1],
                "conditioningSha256": digest(conditioning),
                "inputBundleSha256": digest(output / "input.bundle"),
                "referenceSha256": digest(output / "reference.npz"),
                "elapsedSeconds": time.monotonic() - started,
                "peakRssKiB": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}
    (output / "reference.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(evidence), flush=True)


def check_projection_inventory(nodes, middle, packed):
    expected = {}
    first, end = middle
    suffixes = [f"{attention}/{projection}" for attention in ("self", "cross")
                for projection in ("q", "k", "v", "o")] + ["up", "down"]
    for layer in range(30):
        for suffix in suffixes:
            bits = 32
            if packed:
                if not first <= layer < end:
                    bits = 8
                elif not suffix.endswith(("/q", "/k")):
                    bits = 4
            expected[f"block-{layer}/{suffix}/projection"] = bits

    observed = {}
    for node in nodes:
        if node.name not in expected:
            if node.op_type == "MatMulNBits":
                raise ValueError(f"unexpected packed projection {node.name}")
            continue
        if node.name in observed:
            raise ValueError(f"duplicate projection {node.name}")
        if node.op_type == "MatMul" and not node.domain and len(node.input) == 2:
            bits = 32
        else:
            attrs = {item.name: item.i for item in node.attribute}
            bits = attrs.get("bits")
            if (node.op_type != "MatMulNBits" or node.domain != "com.microsoft" or
                    len(node.input) != 3 or bits not in (4, 8) or
                    attrs.get("block_size") != 32 or attrs.get("accuracy_level") != 1):
                raise ValueError(f"invalid packed contract for {node.name}")
        if bits != expected[node.name]:
            raise ValueError(f"unexpected precision for {node.name}")
        observed[node.name] = bits
    if observed != expected:
        raise ValueError(f"missing projections: {sorted(expected.keys() - observed.keys())}")
    return dict(sorted(Counter(observed.values()).items()))


def audit_exports(fp32, mixed, reference, output):
    import shutil

    import onnxruntime as ort

    source = json.loads((reference / "reference.json").read_text())
    for name, recorded in (("input.bundle", "inputBundleSha256"), ("reference.npz", "referenceSha256")):
        if digest(reference / name) != source[recorded]:
            raise ValueError("independent reference identity differs")
    graph_identities = {}
    projection_inventory = {}
    for artifact, packed_count in ((fp32, 0), (mixed, 292)):
        export = json.loads((artifact / "export.json").read_text())
        if (export["contextTokens"] != source["contextTokens"] or
                export["operators"].get("MatMulNBits", 0) != packed_count or
                export["blockRange"] != RECIPE["input"]["middle_blocks"]):
            raise ValueError("exported context, operator or block inventory differs")
        graph_identities[str(artifact)] = verify_files(
            artifact, {"model.onnx": export["graphSha256"],
                       "model.onnx.data": export["sidecarSha256"]})
        graph = onnx.load(artifact / "model.onnx", load_external_data=False).graph
        projection_inventory[str(artifact)] = check_projection_inventory(
            graph.node, export["blockRange"], packed_count != 0)
    if digest(fp32 / "source-anchors.json") != digest(mixed / "source-anchors.json"):
        raise ValueError("the uncut exports have different source anchors")
    bounds = RECIPE["numerical_bounds"]

    def run(artifact, feeds):
        options = ort.SessionOptions()
        options.enable_cpu_mem_arena = False
        options.intra_op_num_threads = 4
        options.inter_op_num_threads = 1
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
        options.add_session_config_entry("session.disable_prepacking", "1")
        started = time.monotonic()
        session = ort.InferenceSession(str(artifact / "model.onnx"), options,
                                       providers=["CPUExecutionProvider"])
        ports = {port.name for port in session.get_inputs()}
        if ports != set(feeds):
            raise ValueError("reference named inputs differ from exported graph")
        actual, = session.run(["flow"], feeds)
        return actual, time.monotonic() - started

    def compare(actual, expected, criterion, elapsed):
        if actual.shape != expected.shape or not np.isfinite(actual).all() or not np.isfinite(expected).all():
            raise ValueError("uncut reference shape or finite-value check failed")
        absolute = np.abs(actual.astype(np.float64) - expected)
        allowed = criterion["atol"] + criterion["rtol"] * np.abs(expected)
        return {**criterion, "shape": list(expected.shape), "elements": expected.size,
                "violations": int(np.count_nonzero(absolute > allowed)),
                "maxAbs": float(np.max(absolute)), "rmse": float(np.sqrt(np.mean(absolute**2))),
                "elapsedSeconds": elapsed}

    with np.load(reference / "reference.npz", allow_pickle=False) as values:
        feeds = {key: values[key] for key in values.files if key != "expected"}
        raw, elapsed = run(fp32, feeds)
        fp32_check = compare(raw, values["expected"], bounds["fp32_export"], elapsed)
        quantized, elapsed = run(mixed, feeds)
        drift_check = compare(quantized, raw, bounds["quantization_drift"], elapsed)
    result = {"referenceSha256": source["referenceSha256"],
              "inputBundleSha256": source["inputBundleSha256"], "fp32Export": fp32_check,
              "quantizationDrift": drift_check, "graphs": graph_identities,
              "projectionInventory": projection_inventory,
              "peakRssKiB": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}
    (output / "uncut-audit.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    if fp32_check["violations"] or drift_check["violations"]:
        raise ValueError("uncut Wan numerical gate failed")
    shutil.copyfile(reference / "input.bundle", output / "input.bundle")
    oracle = {"tensor": "flow", "shape": list(quantized.shape),
              "float32": quantized.reshape(-1).tolist(), **bounds["same_mixed_source"]}
    (output / "oracle.json").write_text(json.dumps(oracle) + "\n")


PROMPT = RECIPE["text_encoder"]["prompt"]


def audit_slices(source, run_root, output):
    """Match the installed cold artifacts to the original packed projections."""
    log = (run_root / "requester-hybrid-cold.log").read_text()
    selections = [line for line in log.splitlines() if "NDNSF_DI_NATIVE_SELECTION_COMMITTED " in line]
    if len(selections) != 1:
        raise ValueError("expected one cold hybrid Selection")
    selection = selections[0]
    manifest_digest = selection.split(" manifestDigest=", 1)[1].split()[0]
    selected = json.loads(selection.split(" manifestJson=", 1)[1])
    source_graph = source / "model.onnx"
    metadata = selected["metadata"]
    if (f"sha256:{digest(source_graph)}" != metadata["canonicalSourceDigest"] or
            f"sha256:{digest(source / 'model.onnx.data')}" !=
            metadata["canonicalInitializerObjectDigest"]):
        raise ValueError("source files differ from the cold Selection")
    graph = onnx.load(source_graph, load_external_data=False).graph
    anchors = json.loads((source / "source-anchors.json").read_text())
    first = int(anchors["boundary_node"].removeprefix("onnx-node-"))
    end = int(anchors["suffix_node"].removeprefix("onnx-node-"))
    middle = {node.name: node for node in graph.node[first:end]
              if node.name.endswith("/projection") and node.op_type in ("MatMul", "MatMulNBits")}
    ordinal_names = {f"onnx-node-{index}": graph.node[index].name for index in range(first, end)}
    if len(middle) != 10 * (RECIPE["input"]["middle_blocks"][1] - RECIPE["input"]["middle_blocks"][0]):
        raise ValueError("source middle projection inventory differs")
    initializers = {tensor.name: tensor for tensor in graph.initializer}

    def array(tensors, name, path):
        return onnx.numpy_helper.to_array(tensors[name], base_dir=str(path.parent))

    def attributes(node):
        return {attribute.name: attribute.i for attribute in node.attribute}

    def manifest_for(role, provider):
        root = run_root / provider / "cache" / "assembled" / role
        matches = []
        for path in root.glob("*/manifest.json"):
            manifest = json.loads(path.read_text())
            if manifest["modelManifestDigest"] == manifest_digest:
                matches.append((path.parent, manifest))
        if len(matches) != 1:
            raise ValueError(f"expected one cold artifact for {role}")
        return matches[0]

    outer_nodes = {}
    for role, provider, nodes in (
        ("Prefix", "provider", graph.node[:first]),
        ("Suffix", "provider2", graph.node[end:]),
    ):
        path, _ = manifest_for(role, provider)
        assembled = onnx.load(path / "model.onnx", load_external_data=False).graph
        if list(assembled.node) != list(nodes):
            raise ValueError(f"{role} source operators, attributes or order differ")
        outer_nodes[role] = len(nodes)

    packed_projections = {}
    fp32_projections = {}
    for rank, provider in enumerate(("provider", "provider2")):
        role = f"Rank{rank}"
        root, manifest = manifest_for(role, provider)
        checked = set()
        for index, _ in enumerate(json.loads(manifest["sourceEntryFiles"])):
            path = (root / f"entry-{index}" if index else root) / "model.onnx"
            entry = onnx.load(path, load_external_data=False).graph
            tensors = {tensor.name: tensor for tensor in entry.initializer}
            for node in entry.node:
                name = ordinal_names.get(node.name, node.name)
                if name not in middle:
                    if node.op_type == "MatMulNBits":
                        raise ValueError(f"{role} unexpected packed projection {name}")
                    continue
                original = middle[name]
                if name in checked or node.op_type != original.op_type:
                    raise ValueError(f"{role} duplicate or replaced projection {name}")
                if node.op_type == "MatMul":
                    full = array(initializers, original.input[1], source_graph)
                    width = full.shape[1] // 2
                    expected = full[:, rank * width:(rank + 1) * width]
                    actual = array(tensors, node.input[1], path)
                    if (not name.endswith(("/q/projection", "/k/projection")) or
                            actual.shape != expected.shape or actual.dtype != np.float32 or
                            expected.tobytes() != actual.tobytes()):
                        raise ValueError(f"{role} FP32 Q/K slice differs for {name}")
                    checked.add(name)
                    continue
                geometry = attributes(original)
                n, k, block = geometry["N"], geometry["K"], geometry["block_size"]
                split_output = not name.endswith(("/o/projection", "/down/projection"))
                axis = "N" if split_output else "K"
                shard = geometry[axis] // 2
                if attributes(node) != dict(geometry, **{axis: shard}) or len(node.input) != 3:
                    raise ValueError(f"{role} packed projection geometry differs for {name}")
                for port in (1, 2):
                    full = array(initializers, original.input[port], source_graph)
                    full = full.reshape(n, k // block, -1)
                    actual = array(tensors, node.input[port], path)
                    if split_output:
                        expected = full[rank * shard:(rank + 1) * shard]
                    else:
                        width = shard // block
                        expected = full[:, rank * width:(rank + 1) * width]
                    if expected.dtype != actual.dtype or expected.tobytes() != actual.tobytes():
                        raise ValueError(f"{role} packed data differs for {name} input {port}")
                checked.add(name)
        if checked != middle.keys():
            raise ValueError(f"{role} missing packed projections: {middle.keys() - checked}")
        packed_projections[role] = sorted(name for name in checked if middle[name].op_type == "MatMulNBits")
        fp32_projections[role] = sorted(checked - set(packed_projections[role]))
    result = {"selectionManifest": manifest_digest, "outerNodes": outer_nodes,
              "middleSourceOrdinalRange": [first, end], "exactPackedProjections": packed_projections,
              "exactFp32Projections": fp32_projections}
    (output / "slice-audit.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)


def text_token_inputs(root):
    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(str(root / "tokenizer"), local_files_only=True)
    return tokenizer(PROMPT, return_tensors="pt", add_special_tokens=True)


def prepare_conditioning(root, output):
    from transformers import UMT5Config, UMT5EncoderModel

    started = time.monotonic()
    source = root / "models_t5_umt5-xxl-enc-bf16.pth"
    expected = RECIPE["text_encoder"]
    observed = verify_files(root, expected["files"])
    if digest(root / "config.json") != expected["config_sha256"]:
        raise ValueError("Wan UMT5 configuration digest differs")
    config = UMT5Config.from_pretrained(str(root), local_files_only=True)
    inputs = text_token_inputs(root)
    if inputs.input_ids.tolist() != [expected["token_ids"]]:
        raise ValueError("tokenization differs from the frozen Wan prompt")
    torch.set_num_threads(4)
    torch.set_num_interop_threads(1)
    source_weights = torch.load(source, map_location="cpu", mmap=True, weights_only=True)
    embedding = source_weights["token_embedding.weight"]
    converted = {"shared.weight": embedding, "encoder.embed_tokens.weight": embedding,
                 "encoder.final_layer_norm.weight": source_weights["norm.weight"]}
    names = {
        "norm1.weight": "0.layer_norm.weight",
        "norm2.weight": "1.layer_norm.weight",
        "pos_embedding.embedding.weight": "0.SelfAttention.relative_attention_bias.weight",
        "ffn.gate.0.weight": "1.DenseReluDense.wi_0.weight",
        "ffn.fc1.weight": "1.DenseReluDense.wi_1.weight",
        "ffn.fc2.weight": "1.DenseReluDense.wo.weight",
    }
    names.update({f"attn.{name}.weight": f"0.SelfAttention.{name}.weight" for name in "qkvo"})
    checked = {"token_embedding.weight", "norm.weight"}
    for index in range(config.num_layers):
        for old, new in names.items():
            key = f"blocks.{index}.{old}"
            converted[f"encoder.block.{index}.layer.{new}"] = source_weights[key]
            checked.add(key)
    if checked != set(source_weights):
        raise ValueError("unexpected original Wan UMT5 checkpoint entries")
    with torch.device("meta"):
        encoder = UMT5EncoderModel(config).eval()
    encoder.load_state_dict(converted, strict=True, assign=True)
    # Assign directly from the read-only memory map. This avoids a resident
    # second model or converting the encoder to FP32 on a constrained host.
    with torch.inference_mode():
        encoded = encoder(**inputs).last_hidden_state.float().numpy()
    if encoded.shape != (1, inputs.input_ids.shape[1], 4096) or not np.isfinite(encoded).all():
        raise ValueError("invalid Wan UMT5 output")
    target = output / "conditioning.npy"
    np.save(target, encoded, allow_pickle=False)
    result = {"prompt": PROMPT, "tokenIds": inputs.input_ids.tolist(),
              "conditionSha256": digest(target), "sourceSha256": observed,
              "shape": list(encoded.shape), "dtype": "BF16 weights, FP32 output",
              "elapsedSeconds": time.monotonic() - started,
              "peakRssKiB": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}
    (output / "conditioning.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transformer", type=Path,
                        help="verified Diffusers transformer checkpoint directory")
    parser.add_argument("--output", type=Path, required=True)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--block", type=int,
                        help="export one block and compare to an independent FP32 forward")
    action.add_argument("--context-tokens", type=int, help="export the full static graph")
    action.add_argument("--conditioning", type=Path,
                        help="run the independent reference using a frozen .npy embedding")
    action.add_argument("--audit-exports", type=Path, nargs=2, metavar=("FP32", "MIXED"),
                        help="verify the uncut exports and freeze the mixed output oracle")
    action.add_argument("--audit-slices", type=Path, nargs=2, metavar=("MIXED", "DRIVER"),
                        help="verify the retained cold Provider artifacts against the source")
    parser.add_argument("--reference-dir", type=Path,
                        help="independent reference for --audit-exports")
    action.add_argument("--text-encoder", type=Path, help="prepare frozen text conditioning")
    parser.add_argument("--packed", action="store_true",
                        help="use the frozen INT8/INT4/FP32 Wan projection profile")
    parser.add_argument("--middle", nargs=2, type=int, default=RECIPE["input"]["middle_blocks"],
                        metavar=("FIRST", "END"), help="zero-based half-open block range")
    parser.add_argument("--timestep", type=int, default=RECIPE["input"]["timestep"])
    args = parser.parse_args()
    if (args.audit_exports is None) != (args.reference_dir is None):
        parser.error("--audit-exports and --reference-dir must be supplied together")
    if args.audit_exports is not None:
        args.output.mkdir(parents=True)
        audit_exports(*args.audit_exports, args.reference_dir, args.output)
        return
    if args.audit_slices is not None:
        args.output.mkdir(parents=True)
        audit_slices(*args.audit_slices, args.output)
        return
    if args.text_encoder is not None:
        args.output.mkdir(parents=True)
        prepare_conditioning(args.text_encoder, args.output)
        return
    if args.transformer is None:
        parser.error("--transformer is required unless preparing text conditioning")
    config = json.loads((args.transformer / "config.json").read_text())
    check_config(config)
    small_files = {"config.json", "diffusion_pytorch_model.safetensors.index.json"}
    verify_files(args.transformer, RECIPE["transformer"]["files"],
                 small_files if args.block is not None else None)
    first, end = args.middle
    if not 0 < first < end < config["num_layers"] or end - first < 2:
        parser.error("--middle requires two consecutive blocks and nonempty outer stages")
    if args.context_tokens is not None and not 1 <= args.context_tokens <= 512:
        parser.error("--context-tokens must be in [1,512]")
    if args.packed and args.context_tokens is None:
        parser.error("--packed requires --context-tokens")
    args.output.mkdir(parents=True)
    if args.block is not None:
        probe_block(args.transformer, args.output, args.block)
    elif args.conditioning is not None:
        reference_full(args.transformer, args.output, config, args.conditioning, args.timestep)
    else:
        export_full(args.transformer, args.output, config, args.context_tokens,
                    first, end, args.packed)


if __name__ == "__main__":
    main()
