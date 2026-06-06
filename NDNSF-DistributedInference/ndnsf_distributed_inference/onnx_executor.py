"""Dependency-driven ONNX chunk execution helpers.

The helpers in this module are model-agnostic. They execute one ONNX chunk for
the role assigned by a distributed-inference plan and use role-local dependency
edges to exchange tensor bundles with other providers.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
from io import BytesIO
from pathlib import Path
from threading import Lock
from time import perf_counter
from typing import Mapping, Sequence

import numpy as np
import onnxruntime as ort

from .provider import ProviderRuntimeContext


_SESSION_CACHE_LOCK = Lock()
_SESSION_CACHE: dict[tuple[int, str], ort.InferenceSession] = {}
_DIGEST_CACHE: dict[tuple[str, int, int], str] = {}


@dataclass(frozen=True)
class CachedSession:
    session: ort.InferenceSession
    cache_hit: bool
    session_ms: float


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
                producer_role=producer,
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

    collect_ms = 0.0
    if initial_values is not None:
        values = {
            str(name): np.asarray(value, dtype=np.float32)
            for name, value in initial_values.items()
        }
    else:
        collect_start = perf_counter()
        values = _collect_input_values(
            ctx,
            input_prefetches=input_prefetches,
            ref_timeout_ms=ref_timeout_ms,
            fetch_timeout_ms=fetch_timeout_ms,
        )
        collect_ms = _elapsed_ms(collect_start)

    output_payload, session_info, run_ms = _run_onnx_to_npz(model_path, values)
    output_values = load_npz_payload(output_payload)
    published: list[str] = []
    publish_start = perf_counter()
    for edge in ctx.dependencies.outputs:
        edge_tensors = _available_edge_tensors(output_values, edge.tensors)
        if edge.tensors and not edge_tensors:
            raise KeyError(
                "ONNX output missing dependency tensor(s) for output edge "
                f"{edge.key_scope}: " + ", ".join(edge.tensors)
            )
        edge_publish_start = perf_counter()
        edge_payload = encode_tensor_bundle(
            select_tensor_payload(output_payload, edge_tensors)
        )
        data_name = ctx.planned_large_data_name(edge, ctx.role)
        ctx.ndnsf.publish_large_reference(
            edge.key_scope,
            edge.topic(role_topic_token(ctx.role)),
            edge.topic("ref/" + role_topic_token(ctx.role)),
            edge_payload,
            object_type="application/x-ndnsf-di-tensor-bundle+npz",
            object_id=role_topic_token(ctx.role),
            data_name=data_name,
        )
        edge_publish_ms = _elapsed_ms(edge_publish_start)
        print(
            "NDNSF_DI_DEPENDENCY_OUTPUT_TIMING "
            f"session={_session_token(ctx)} "
            f"role={ctx.role} "
            f"scope={edge.key_scope} "
            f"consumers={','.join(edge.consumers)} "
            f"tensors={','.join(edge_tensors)} "
            f"bytes={len(edge_payload)} "
            f"expected_segments={int(getattr(edge, 'expected_segments', 0) or 0)} "
            f"planned_name={'true' if data_name else 'false'} "
            f"publish_ms={edge_publish_ms:.2f}",
            flush=True,
        )
        published.append(edge.key_scope)
    publish_ms = _elapsed_ms(publish_start)
    print(
        "NDNSF_DI_ONNX_TIMING "
        f"session={_session_token(ctx)} "
        f"role={ctx.role} "
        f"model={Path(model_path).name} "
        f"input_edges={len(ctx.dependencies.inputs)} "
        f"output_edges={len(ctx.dependencies.outputs)} "
        f"collect_ms={collect_ms:.2f} "
        f"session_cache={'hit' if session_info.cache_hit else 'miss'} "
        f"session_ms={session_info.session_ms:.2f} "
        f"run_ms={run_ms:.2f} "
        f"publish_ms={publish_ms:.2f}",
        flush=True,
    )
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
    expected_tensors_by_scope = {
        edge.key_scope: list(edge.tensors)
        for edge in ctx.dependencies.inputs
        if edge.tensors
    }
    for item in prefetches:
        edge = edge_by_scope[item.key_scope]
        wait_start = perf_counter()
        result = ctx.wait_prefetched_input_large_result(
            item.future,
            timeout_ms=fetch_timeout_ms,
        )
        wait_ms = _elapsed_ms(wait_start)
        decode_start = perf_counter()
        if isinstance(result, bytes):
            payload = result
            ref_wait_ms = 0.0
            fetch_ms = 0.0
            prefetch_total_ms = wait_ms
            expected_segments = int(getattr(edge, "expected_segments", 0) or 0)
        else:
            payload = result.payload
            ref_wait_ms = result.ref_wait_ms
            fetch_ms = result.fetch_ms
            prefetch_total_ms = result.total_ms
            expected_segments = result.expected_segments
        tensor_payload = decode_tensor_bundle(payload)
        tensor_values = load_npz_payload(tensor_payload)
        if edge.tensors and not _available_edge_tensors(tensor_values, edge.tensors):
            raise KeyError(
                "activation payload missing every dependency tensor for "
                f"scope={edge.key_scope}: " + ", ".join(edge.tensors)
            )
        values.update(tensor_values)
        decode_ms = _elapsed_ms(decode_start)
        print(
            "NDNSF_DI_DEPENDENCY_INPUT_TIMING "
            f"session={_session_token(ctx)} "
            f"role={ctx.role} "
            f"producer={item.producer} "
            f"scope={item.key_scope} "
            f"bytes={len(payload)} "
            f"future_wait_ms={wait_ms:.2f} "
            f"ref_wait_ms={ref_wait_ms:.2f} "
            f"fetch_ms={fetch_ms:.2f} "
            f"decode_ms={decode_ms:.2f} "
            f"prefetch_total_ms={prefetch_total_ms:.2f} "
            f"expected_segments={expected_segments}",
            flush=True,
        )
    if expected_tensors_by_scope:
        merged_payload = npz_payload(values)
        for scope, tensors in expected_tensors_by_scope.items():
            try:
                verify_tensor_payload(merged_payload, tensors)
            except KeyError as exc:
                raise KeyError(
                    f"merged activation payload missing dependency tensor(s) "
                    f"for scope={scope}: {exc}"
                ) from exc
    return values


def _available_edge_tensors(values: Mapping[str, np.ndarray],
                            tensors: Sequence[str] | None = None) -> list[str]:
    requested = [str(tensor) for tensor in (tensors or ()) if str(tensor)]
    if not requested:
        return []
    available = []
    for tensor in requested:
        try:
            _value_for_input(values, tensor)
        except KeyError:
            continue
        available.append(tensor)
    return available


def _run_onnx_to_npz(model_path: str | Path,
                     values: Mapping[str, np.ndarray]) -> tuple[bytes, CachedSession, float]:
    session_info = _cached_session(model_path)
    session = session_info.session
    feed = {
        input_info.name: _value_for_input(values, input_info.name).astype(np.float32)
        for input_info in session.get_inputs()
    }
    run_start = perf_counter()
    outputs = session.run(None, feed)
    run_ms = _elapsed_ms(run_start)
    return npz_payload({
        output.name: np.asarray(value, dtype=np.float32)
        for output, value in zip(session.get_outputs(), outputs)
    }), session_info, run_ms


def _cached_session(model_path: str | Path) -> CachedSession:
    start = perf_counter()
    path = Path(model_path).resolve()
    stat = path.stat()
    digest = _model_digest(path, stat.st_size, stat.st_mtime_ns)
    key = (int(stat.st_size), digest)
    with _SESSION_CACHE_LOCK:
        session = _SESSION_CACHE.get(key)
        if session is not None:
            return CachedSession(session, True, _elapsed_ms(start))
    session = ort.InferenceSession(
        str(path),
        providers=["CPUExecutionProvider"],
    )
    with _SESSION_CACHE_LOCK:
        cached = _SESSION_CACHE.get(key)
        if cached is None:
            _SESSION_CACHE[key] = session
            return CachedSession(session, False, _elapsed_ms(start))
        return CachedSession(cached, True, _elapsed_ms(start))


def _model_digest(path: Path, size: int, mtime_ns: int) -> str:
    path_key = (str(path), int(size), int(mtime_ns))
    with _SESSION_CACHE_LOCK:
        digest = _DIGEST_CACHE.get(path_key)
        if digest is not None:
            return digest
    sha = hashlib.sha256()
    with path.open("rb") as model_file:
        for chunk in iter(lambda: model_file.read(1024 * 1024), b""):
            sha.update(chunk)
    digest = sha.hexdigest()
    with _SESSION_CACHE_LOCK:
        _DIGEST_CACHE[path_key] = digest
    return digest


def _elapsed_ms(start: float) -> float:
    return (perf_counter() - start) * 1000.0


def _session_token(ctx: ProviderRuntimeContext) -> str:
    return str(getattr(ctx.ndnsf, "session_id", "") or "-").strip("/") or "-"


def _value_for_input(values: Mapping[str, np.ndarray], name: str) -> np.ndarray:
    if name in values:
        return values[name]
    base, dot, suffix = name.rpartition(".")
    if dot and suffix.isdigit() and base in values:
        return values[base]
    raise KeyError(name)
