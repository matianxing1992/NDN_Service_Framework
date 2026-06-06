"""Dependency-driven ONNX chunk execution helpers.

The helpers in this module are model-agnostic. They execute one ONNX chunk for
the role assigned by a distributed-inference plan and use role-local dependency
edges to exchange tensor bundles with other providers.
"""

from __future__ import annotations

from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Mapping, Sequence

import numpy as np
import onnxruntime as ort

from .provider import ProviderRuntimeContext


@dataclass(frozen=True)
class PrefetchedDependency:
    key_scope: str
    producer: str
    future: object


@dataclass(frozen=True)
class OnnxExecutionResult:
    values: dict[str, np.ndarray]
    published_edges: tuple[str, ...] = ()

    def first_value(self) -> np.ndarray:
        return next(iter(self.values.values()))

    def value(self, name: str, default_first: bool = True) -> np.ndarray:
        try:
            return _value_for_input(self.values, name)
        except KeyError:
            if default_first:
                return self.first_value()
            raise


def role_topic_token(role: str) -> str:
    return str(role).strip("/").replace("/", "-") or "role"


def npz_payload(values: Mapping[str, np.ndarray]) -> bytes:
    buffer = BytesIO()
    np.savez(buffer, **{
        str(name): np.asarray(value, dtype=np.float32)
        for name, value in values.items()
    })
    return buffer.getvalue()


def load_npz_payload(payload: bytes) -> dict[str, np.ndarray]:
    with np.load(BytesIO(payload), allow_pickle=False) as data:
        return {name: data[name] for name in data.files}


def encode_tensor_bundle(payload: bytes) -> bytes:
    return npz_payload({
        "payload": np.frombuffer(payload, dtype=np.uint8),
    })


def decode_tensor_bundle(payload: bytes) -> bytes:
    values = load_npz_payload(payload)
    if "payload" not in values:
        raise KeyError("tensor bundle missing payload")
    return values["payload"].astype(np.uint8).tobytes()


def select_tensor_payload(payload: bytes,
                          tensors: Sequence[str] | None = None) -> bytes:
    requested = [str(tensor) for tensor in (tensors or ()) if str(tensor)]
    if not requested:
        return payload
    values = load_npz_payload(payload)
    selected: dict[str, np.ndarray] = {}
    missing: list[str] = []
    for tensor in requested:
        try:
            selected[tensor] = _value_for_input(values, tensor)
        except KeyError:
            missing.append(tensor)
    if missing:
        raise KeyError(
            "activation payload missing dependency tensor(s): " +
            ", ".join(missing))
    return npz_payload(selected)


def verify_tensor_payload(payload: bytes,
                          tensors: Sequence[str] | None = None) -> None:
    if tensors:
        select_tensor_payload(payload, tensors)


def prefetch_dependency_inputs(
    ctx: ProviderRuntimeContext,
    *,
    ref_timeout_ms: int = 60000,
    fetch_timeout_ms: int = 60000,
) -> list[PrefetchedDependency]:
    """Prefetch all planned large-object inputs for the current role."""

    prefetches: list[PrefetchedDependency] = []
    for edge in ctx.dependencies.inputs:
        for producer in edge.producers:
            future = ctx.prefetch_input_large(
                key_scope=edge.key_scope,
                topic_suffix="ref/" + role_topic_token(producer),
                ref_timeout_ms=ref_timeout_ms,
                fetch_timeout_ms=fetch_timeout_ms,
            )
            prefetches.append(PrefetchedDependency(
                key_scope=edge.key_scope,
                producer=producer,
                future=future,
            ))
    return prefetches


def execute_onnx_dependency_chunk(
    ctx: ProviderRuntimeContext,
    model_path: str | Path,
    *,
    initial_values: Mapping[str, np.ndarray] | None = None,
    input_prefetches: Sequence[PrefetchedDependency] | None = None,
    ref_timeout_ms: int = 60000,
    fetch_timeout_ms: int = 60000,
) -> OnnxExecutionResult:
    """Run one ONNX chunk and publish declared output-edge tensor bundles."""

    if initial_values is not None:
        values = {
            str(name): np.asarray(value, dtype=np.float32)
            for name, value in initial_values.items()
        }
    else:
        values = _collect_input_values(
            ctx,
            input_prefetches=input_prefetches,
            ref_timeout_ms=ref_timeout_ms,
            fetch_timeout_ms=fetch_timeout_ms,
        )

    output_payload = _run_onnx_to_npz(model_path, values)
    output_values = load_npz_payload(output_payload)
    published: list[str] = []
    for edge in ctx.dependencies.outputs:
        edge_payload = encode_tensor_bundle(
            select_tensor_payload(output_payload, edge.tensors)
        )
        ctx.ndnsf.publish_large_reference(
            edge.key_scope,
            edge.topic(role_topic_token(ctx.role)),
            edge.topic("ref/" + role_topic_token(ctx.role)),
            edge_payload,
            object_type="application/x-ndnsf-di-tensor-bundle+npz",
            object_id=role_topic_token(ctx.role),
        )
        published.append(edge.key_scope)
    return OnnxExecutionResult(
        values=output_values,
        published_edges=tuple(published),
    )


def _collect_input_values(
    ctx: ProviderRuntimeContext,
    *,
    input_prefetches: Sequence[PrefetchedDependency] | None = None,
    ref_timeout_ms: int = 60000,
    fetch_timeout_ms: int = 60000,
) -> dict[str, np.ndarray]:
    prefetches = list(input_prefetches or prefetch_dependency_inputs(
        ctx,
        ref_timeout_ms=ref_timeout_ms,
        fetch_timeout_ms=fetch_timeout_ms,
    ))
    values: dict[str, np.ndarray] = {}
    edge_by_scope = {edge.key_scope: edge for edge in ctx.dependencies.inputs}
    for item in prefetches:
        edge = edge_by_scope[item.key_scope]
        payload = ctx.wait_prefetched_input_large(
            item.future,
            timeout_ms=fetch_timeout_ms,
        )
        tensor_payload = decode_tensor_bundle(payload)
        verify_tensor_payload(tensor_payload, edge.tensors)
        values.update(load_npz_payload(tensor_payload))
    return values


def _run_onnx_to_npz(model_path: str | Path,
                     values: Mapping[str, np.ndarray]) -> bytes:
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    feed = {
        input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
        for input_info in session.get_inputs()
    }
    outputs = session.run(None, feed)
    return npz_payload({
        output.name: np.asarray(value, dtype=np.float32)
        for output, value in zip(session.get_outputs(), outputs)
    })


def _value_for_input(values: Mapping[str, np.ndarray], name: str) -> np.ndarray:
    if name in values:
        return values[name]
    base, dot, suffix = name.rpartition(".")
    if dot and suffix.isdigit() and base in values:
        return values[base]
    raise KeyError(name)
