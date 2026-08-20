"""Dependency-driven ONNX chunk execution helpers.

The helpers in this module are model-agnostic. They execute one ONNX chunk for
the role assigned by a distributed-inference plan and use role-local dependency
edges to exchange tensor bundles with other providers.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from io import BytesIO
from pathlib import Path
import re
import tempfile
from threading import Lock
from time import perf_counter, time
from typing import Mapping, Sequence

import numpy as np
import onnxruntime as ort

from ...app_sdk.facades import ProviderRuntimeContext


_SESSION_CACHE_LOCK = Lock()
_SESSION_CACHE: dict[tuple[int, str], ort.InferenceSession] = {}
_DIGEST_CACHE: dict[tuple[str, int, int], str] = {}
_SHA256 = re.compile(r"^sha256:[0-9a-f]{64}$")
_ONNX_DTYPE_NAMES = {
    1: "float32", 2: "uint8", 3: "int8", 4: "uint16", 5: "int16",
    6: "int32", 7: "int64", 9: "bool", 10: "float16", 11: "float64",
    12: "uint32", 13: "uint64", 16: "bfloat16",
}


def _require_sha256(value: str, field: str) -> None:
    if not isinstance(value, str) or not _SHA256.fullmatch(value):
        raise ValueError(f"{field} must be a canonical sha256 digest")


@dataclass(frozen=True)
class CertifiedOnnxAssemblyRecipe:
    """Digest-pinned adapter certificate for one Provider-local role slice."""

    model_manifest_digest: str
    artifact_profile_digest: str
    graph_digest: str
    canonical_initializer_digest: str
    adapter_descriptor_digest: str
    assembler_descriptor_digest: str
    backend_abi: str
    role_kind: str
    layer_begin: int
    layer_end: int
    node_indices: tuple[int, ...]
    input_names: tuple[str, ...]
    output_names: tuple[str, ...]
    expected_inputs: tuple[Mapping[str, object], ...]
    expected_outputs: tuple[Mapping[str, object], ...]
    precision: str
    quantization: str = "none"
    layout: str = "native"
    padding: str = "none"
    max_source_bytes: int = 8 * 1024**3
    max_assembled_bytes: int = 8 * 1024**3
    max_nodes: int = 1_000_000
    schema: str = "ndnsf-di-certified-onnx-assembly-v1"

    def __post_init__(self) -> None:
        for field_name in (
            "model_manifest_digest", "artifact_profile_digest", "graph_digest",
            "canonical_initializer_digest",
            "adapter_descriptor_digest", "assembler_descriptor_digest",
        ):
            _require_sha256(getattr(self, field_name), field_name)
        if (self.schema != "ndnsf-di-certified-onnx-assembly-v1"
                or not self.backend_abi or not self.role_kind
                or self.layer_begin < 0 or self.layer_end <= self.layer_begin
                or not self.input_names or not self.output_names
                or len(set(self.input_names)) != len(self.input_names)
                or len(set(self.output_names)) != len(self.output_names)
                or self.max_source_bytes <= 0 or self.max_assembled_bytes <= 0
                or self.max_nodes <= 0):
            raise ValueError("invalid certified ONNX assembly recipe")
        indices = tuple(int(index) for index in self.node_indices)
        if (not indices or any(index < 0 for index in indices)
                or indices != tuple(sorted(set(indices)))):
            raise ValueError("certified ONNX node cover is missing or overlapping")
        object.__setattr__(self, "node_indices", indices)
        object.__setattr__(self, "input_names", tuple(self.input_names))
        object.__setattr__(self, "output_names", tuple(self.output_names))
        for field_name, names in (
            ("expected_inputs", self.input_names),
            ("expected_outputs", self.output_names),
        ):
            contracts = tuple(dict(item) for item in getattr(self, field_name))
            if {str(item.get("name", "")) for item in contracts} != set(names):
                raise ValueError(f"{field_name} does not cover exact ONNX names")
            for item in contracts:
                if not item.get("dtype") or not isinstance(item.get("shape"), (list, tuple)):
                    raise ValueError(f"{field_name} contains an invalid tensor contract")
            object.__setattr__(self, field_name, contracts)

    def to_dict(self) -> dict[str, object]:
        return {
            "schema": self.schema,
            "modelManifestDigest": self.model_manifest_digest,
            "artifactProfileDigest": self.artifact_profile_digest,
            "graphDigest": self.graph_digest,
            "canonicalInitializerDigest": self.canonical_initializer_digest,
            "adapterDescriptorDigest": self.adapter_descriptor_digest,
            "assemblerDescriptorDigest": self.assembler_descriptor_digest,
            "backendAbi": self.backend_abi,
            "roleKind": self.role_kind,
            "layerBegin": self.layer_begin,
            "layerEnd": self.layer_end,
            "nodeIndices": list(self.node_indices),
            "inputNames": list(self.input_names),
            "outputNames": list(self.output_names),
            "expectedInputs": [dict(item) for item in self.expected_inputs],
            "expectedOutputs": [dict(item) for item in self.expected_outputs],
            "precision": self.precision,
            "quantization": self.quantization,
            "layout": self.layout,
            "padding": self.padding,
            "maxSourceBytes": self.max_source_bytes,
            "maxAssembledBytes": self.max_assembled_bytes,
            "maxNodes": self.max_nodes,
        }

    @property
    def digest(self) -> str:
        wire = json.dumps(
            self.to_dict(), sort_keys=True, separators=(",", ":"),
            ensure_ascii=False).encode("utf-8")
        return "sha256:" + hashlib.sha256(wire).hexdigest()

    def validate_role_spec(self, role_spec) -> None:
        if (role_spec.recipe_digest != self.digest
                or role_spec.layer_begin != self.layer_begin
                or role_spec.layer_end != self.layer_end
                or role_spec.role_kind != self.role_kind
                or tuple(getattr(role_spec, "node_indices", ()))
                != self.node_indices):
            raise ValueError("RoleAssemblySpec does not match certified recipe")
        exact_bindings = {
            "model_manifest_digest": self.model_manifest_digest,
            "artifact_profile_digest": self.artifact_profile_digest,
            "graph_digest": self.graph_digest,
            "canonical_initializer_digest": self.canonical_initializer_digest,
            "adapter_descriptor_digest": self.adapter_descriptor_digest,
            "assembler_descriptor_digest": self.assembler_descriptor_digest,
            "backend_abi": self.backend_abi,
            "precision": self.precision,
            "quantization": self.quantization,
            "layout": self.layout,
            "padding": self.padding,
        }
        labels = {
            "model_manifest_digest": "model manifest",
            "artifact_profile_digest": "artifact profile",
            "graph_digest": "graph",
            "canonical_initializer_digest": "canonical initializer",
            "adapter_descriptor_digest": "adapter descriptor",
            "assembler_descriptor_digest": "assembler descriptor",
            "backend_abi": "backend ABI",
            "precision": "precision",
            "quantization": "quantization",
            "layout": "layout",
            "padding": "padding",
        }
        for name, value in exact_bindings.items():
            if getattr(role_spec, name, "") != value:
                raise ValueError(f"RoleAssemblySpec {labels[name]} mismatch")
        def normalize_contracts(values):
            return tuple({
                "name": str(item["name"]),
                "dtype": str(item["dtype"]),
                "shape": tuple(item["shape"]),
            } for item in values)

        if (normalize_contracts(role_spec.expected_inputs)
                != normalize_contracts(self.expected_inputs)
                or normalize_contracts(role_spec.expected_outputs)
                != normalize_contracts(self.expected_outputs)):
            raise ValueError("RoleAssemblySpec I/O contract mismatch")
        envelope = dict(role_spec.resource_envelope)
        if envelope != {
                "maxSourceBytes": self.max_source_bytes,
                "maxAssembledBytes": self.max_assembled_bytes,
                "maxNodes": self.max_nodes,
        }:
            raise ValueError("RoleAssemblySpec resource envelope mismatch")


@dataclass(frozen=True)
class CertifiedOnnxAssembly:
    model_bytes: bytes
    input_names: tuple[str, ...]
    output_names: tuple[str, ...]
    node_count: int
    model_digest: str


def assemble_certified_onnx_model(
    canonical_model: bytes,
    *,
    role_spec,
    recipe: CertifiedOnnxAssemblyRecipe,
) -> CertifiedOnnxAssembly:
    """Extract, check, and load one certified role from canonical ONNX bytes."""
    recipe.validate_role_spec(role_spec)
    source = bytes(canonical_model)
    if not source or len(source) > recipe.max_source_bytes:
        raise ValueError("canonical ONNX source exceeds its resource envelope")
    try:
        import onnx
    except ImportError as exc:  # pragma: no cover - deployment gate covers this
        raise RuntimeError("Provider-local assembly requires onnx") from exc

    with tempfile.TemporaryDirectory(prefix="ndnsf-onnx-assembly-") as directory:
        root = Path(directory)
        source_path = root / "canonical.onnx"
        output_path = root / "assembled.onnx"
        source_path.write_bytes(source)
        try:
            encoded_model = onnx.load_model_from_string(source)
            for initializer in encoded_model.graph.initializer:
                if initializer.data_location == onnx.TensorProto.EXTERNAL:
                    locations = [
                        item.value for item in initializer.external_data
                        if item.key == "location"
                    ]
                    if (len(locations) != 1 or locations[0] != "model.onnx.data"
                            or Path(locations[0]).is_absolute()
                            or ".." in Path(locations[0]).parts):
                        raise ValueError("unsafe ONNX external-data location")
            model = onnx.load(str(source_path), load_external_data=True)
            onnx.checker.check_model(model, full_check=True)
        except ValueError:
            raise
        except Exception as exc:
            raise ValueError("canonical ONNX model validation failed") from exc
        if len(model.graph.node) > recipe.max_nodes:
            raise ValueError("canonical ONNX graph exceeds its node envelope")
        if recipe.layer_end > len(model.graph.node):
            raise ValueError("certified ONNX node cover escapes the graph")
        from .graph import canonical_onnx_identity
        canonical_identity = canonical_onnx_identity(source_path)
        if canonical_identity.graph_digest != recipe.graph_digest:
            raise ValueError("canonical ONNX graph digest mismatch")
        if (canonical_identity.normalized_initializer_content_digest
                != recipe.canonical_initializer_digest):
            raise ValueError("canonical ONNX initializer digest mismatch")
        try:
            onnx.utils.extract_model(
                str(source_path), str(output_path),
                list(recipe.input_names), list(recipe.output_names),
                check_model=True,
            )
            assembled = onnx.load(str(output_path), load_external_data=True)
            onnx.checker.check_model(assembled, full_check=True)
        except Exception as exc:
            raise ValueError("adapter-certified ONNX extraction failed") from exc
        if len(assembled.graph.node) != len(recipe.node_indices):
            raise ValueError("assembled ONNX node cover differs from the certificate")
        original_nodes = [
            model.graph.node[index].SerializeToString(deterministic=True)
            for index in recipe.node_indices
        ]
        assembled_nodes = [
            node.SerializeToString(deterministic=True)
            for node in assembled.graph.node
        ]
        if assembled_nodes != original_nodes:
            raise ValueError("assembled ONNX contains uncertified graph nodes")

        def contract(value_info) -> dict[str, object]:
            tensor = value_info.type.tensor_type
            shape = []
            for dimension in tensor.shape.dim:
                shape.append(
                    int(dimension.dim_value) if dimension.HasField("dim_value")
                    else str(dimension.dim_param))
            return {
                "name": value_info.name,
                "dtype": _ONNX_DTYPE_NAMES.get(
                    int(tensor.elem_type), str(int(tensor.elem_type))),
                "shape": shape,
            }

        actual_inputs = {item.name: contract(item) for item in assembled.graph.input}
        actual_outputs = {item.name: contract(item) for item in assembled.graph.output}
        for expected, actual, field_name in (
            (recipe.expected_inputs, actual_inputs, "input"),
            (recipe.expected_outputs, actual_outputs, "output"),
        ):
            for item in expected:
                observed = actual.get(str(item["name"]))
                if observed is None:
                    raise ValueError(f"assembled ONNX is missing expected {field_name}")
                expected_dtype = _ONNX_DTYPE_NAMES.get(
                    int(item["dtype"]), str(item["dtype"])) \
                    if str(item["dtype"]).isdigit() else str(item["dtype"])
                if (expected_dtype != str(observed["dtype"])
                        or list(item["shape"]) != list(observed["shape"])):
                    raise ValueError(f"assembled ONNX {field_name} dtype/shape mismatch")
        try:
            wire = assembled.SerializeToString(deterministic=True)
        except TypeError:  # pragma: no cover
            wire = assembled.SerializeToString()
        if not wire or len(wire) > recipe.max_assembled_bytes:
            raise ValueError("assembled ONNX exceeds its resource envelope")
        # Loading the exact bytes through the deployment runtime is the final
        # validation boundary; no PyTorch/Transformers path is involved.
        ort.InferenceSession(wire, providers=["CPUExecutionProvider"])
        return CertifiedOnnxAssembly(
            model_bytes=wire,
            input_names=tuple(recipe.input_names),
            output_names=tuple(recipe.output_names),
            node_count=len(assembled.graph.node),
            model_digest="sha256:" + hashlib.sha256(wire).hexdigest(),
        )


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
    buffer = BytesIO()
    np.savez(buffer, payload=np.frombuffer(payload, dtype=np.uint8))
    return buffer.getvalue()


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
        output_ready_epoch_ms = int(time() * 1000)
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
        publish_done_epoch_ms = int(time() * 1000)
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
            f"expected_bytes={int(getattr(edge, 'expected_bytes', 0) or 0)} "
            f"planned_name={'true' if data_name else 'false'} "
            f"data_name={data_name or '-'} "
            f"output_ready_epoch_ms={output_ready_epoch_ms} "
            f"publish_done_epoch_ms={publish_done_epoch_ms} "
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
            expected_bytes = int(getattr(edge, "expected_bytes", 0) or 0)
            used_planned_name = False
        else:
            payload = result.payload
            ref_wait_ms = result.ref_wait_ms
            fetch_ms = result.fetch_ms
            prefetch_total_ms = result.total_ms
            expected_segments = result.expected_segments
            expected_bytes = result.expected_bytes
            used_planned_name = bool(result.used_planned_name)
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
            f"prefetch_overlap_ms={max(0.0, prefetch_total_ms - wait_ms):.2f} "
            f"expected_segments={expected_segments}",
            f"expected_bytes={expected_bytes}",
            f"planned_name={'true' if used_planned_name else 'false'}",
            f"data_name={ctx.planned_large_data_name(edge, item.producer) or '-'}",
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
