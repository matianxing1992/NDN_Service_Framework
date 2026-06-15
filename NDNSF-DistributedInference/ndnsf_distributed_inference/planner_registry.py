"""Planner backend registry for NDNSF-DistributedInference.

The registry is deliberately small: it records which planner kind owns a
model-family-specific planning backend, while concrete model packages keep
their own splitter implementation details.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable

from .plan import (
    ModelFamily,
    ModelFormat,
    PlannerDescriptor,
    PlannerKind,
    normalize_model_format,
    normalize_model_family,
    normalize_planner_kind,
)
from .runtime_compatibility import validate_runtime_compatibility


@dataclass(frozen=True)
class PlannerRequest:
    """Standard planner input contract.

    The core fields are intentionally model-family-neutral.  Backends may read
    typed values from ``options`` while still returning a common
    ``PlannerResult`` for deployment tooling.
    """

    planner_kind: str | PlannerKind
    model_family: str | ModelFamily
    model_path: str
    output_dir: str
    model_format: str | ModelFormat = ModelFormat.UNKNOWN
    runtime_backend: str = ""
    layout: str = ""
    input_size: int = 0
    provider_profiles: list[Any] = field(default_factory=list)
    options: dict[str, Any] = field(default_factory=dict)
    metadata: dict[str, Any] = field(default_factory=dict)

    def normalized_planner_kind(self) -> str:
        return normalize_planner_kind(self.planner_kind)

    def normalized_model_family(self) -> str:
        return normalize_model_family(self.model_family)

    def normalized_model_format(self) -> str:
        return normalize_model_format(self.model_format)

    def normalized_runtime_backend(self) -> str:
        return str(self.runtime_backend or self.option("runtime_backend", "")).strip()

    def validated_runtime_backend(self, *, require_known: bool = False) -> str:
        return validate_runtime_compatibility(
            self.normalized_model_family(),
            self.normalized_model_format(),
            self.normalized_runtime_backend(),
            require_known=require_known,
        )

    def descriptor(self) -> PlannerDescriptor:
        runtime_backend = self.validated_runtime_backend()
        return PlannerDescriptor(
            model_family=self.normalized_model_family(),
            model_format=self.normalized_model_format(),
            planner_kind=self.normalized_planner_kind(),
            metadata={
                **dict(self.metadata or {}),
                **({"runtimeBackend": runtime_backend} if runtime_backend else {}),
            },
        )

    def option(self, name: str, default: Any = None) -> Any:
        return (self.options or {}).get(name, default)


@dataclass(frozen=True)
class PlannerResult:
    """Standard planner output contract.

    ``split_plan`` is the model-specific low-level result.  ``score_summary``
    and ``selected_candidate`` are normalized so tooling can inspect planner
    quality without knowing YOLO or future LLM internals.
    """

    request: PlannerRequest
    split_plan: dict[str, Any]
    score_summary: dict[str, Any] = field(default_factory=dict)
    selected_candidate: dict[str, Any] = field(default_factory=dict)
    metadata: dict[str, Any] = field(default_factory=dict)

    def normalized_planner_kind(self) -> str:
        return self.request.normalized_planner_kind()

    def normalized_model_family(self) -> str:
        return self.request.normalized_model_family()


@dataclass(frozen=True)
class PlannerBackend:
    """One registered planner backend.

    ``metadata`` is backend-owned.  A model-specific package may put CLI option
    mappings or scoring defaults there without changing the framework-level
    registry shape.
    """

    planner_kind: str | PlannerKind
    model_family: str | ModelFamily = ModelFamily.GENERIC_ONNX
    model_format: str | ModelFormat = ModelFormat.UNKNOWN
    name: str = ""
    description: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)
    handler: Callable[[PlannerRequest], PlannerResult] | None = None

    def normalized_planner_kind(self) -> str:
        return normalize_planner_kind(self.planner_kind)

    def normalized_model_family(self) -> str:
        return normalize_model_family(self.model_family)

    def normalized_model_format(self) -> str:
        return normalize_model_format(self.model_format)

    def descriptor(self) -> PlannerDescriptor:
        return PlannerDescriptor(
            model_family=self.normalized_model_family(),
            model_format=self.normalized_model_format(),
            planner_kind=self.normalized_planner_kind(),
            metadata=dict(self.metadata or {}),
        )

    def plan(self, request: PlannerRequest) -> PlannerResult:
        if request.normalized_planner_kind() != self.normalized_planner_kind():
            raise ValueError(
                "planner request kind does not match backend: "
                f"{request.normalized_planner_kind()} != "
                f"{self.normalized_planner_kind()}")
        if request.normalized_model_family() != self.normalized_model_family():
            raise ValueError(
                "planner request model family does not match backend: "
                f"{request.normalized_model_family()} != "
                f"{self.normalized_model_family()}")
        backend_format = self.normalized_model_format()
        request_format = request.normalized_model_format()
        if backend_format != ModelFormat.UNKNOWN.value and request_format != backend_format:
            raise ValueError(
                "planner request model format does not match backend: "
                f"{request_format} != {backend_format}")
        request.validated_runtime_backend()
        if self.handler is None:
            raise NotImplementedError(
                f"planner backend has no handler: {self.normalized_planner_kind()}")
        return self.handler(request)


class PlannerBackendRegistry:
    """Registry from planner kind to backend metadata."""

    def __init__(self) -> None:
        self._backends: dict[str, PlannerBackend] = {}

    def register(self, backend: PlannerBackend) -> PlannerBackend:
        planner_kind = backend.normalized_planner_kind()
        if planner_kind in self._backends:
            raise ValueError(f"planner backend already registered: {planner_kind}")
        self._backends[planner_kind] = backend
        return backend

    def get(self, planner_kind: str | PlannerKind) -> PlannerBackend:
        key = normalize_planner_kind(planner_kind)
        try:
            return self._backends[key]
        except KeyError as exc:
            available = ", ".join(sorted(self._backends)) or "(none)"
            raise KeyError(
                f"unknown planner kind {key!r}; available: {available}"
            ) from exc

    def has(self, planner_kind: str | PlannerKind) -> bool:
        return normalize_planner_kind(planner_kind) in self._backends

    def backends(self) -> list[PlannerBackend]:
        return [
            self._backends[key]
            for key in sorted(self._backends)
        ]

    def plan(self, request: PlannerRequest) -> PlannerResult:
        return self.get(request.planner_kind).plan(request)


def default_planner_registry() -> PlannerBackendRegistry:
    """Return framework-level planner placeholders.

    Model packages can extend or replace this registry with executable
    backends.  These defaults keep the public planner vocabulary centralized.
    """

    registry = PlannerBackendRegistry()
    registry.register(PlannerBackend(
        planner_kind=PlannerKind.ONNX_DAG,
        model_family=ModelFamily.GENERIC_ONNX,
        model_format=ModelFormat.ONNX,
        name="Generic ONNX DAG planner",
        description="Generic ONNX graph dependency planner.",
    ))
    registry.register(PlannerBackend(
        planner_kind=PlannerKind.LLM_PIPELINE,
        model_family=ModelFamily.LLM,
        model_format=ModelFormat.UNKNOWN,
        name="LLM pipeline planner placeholder",
        description="Reserved planner kind for future LLM pipeline execution.",
    ))
    registry.register(PlannerBackend(
        planner_kind=PlannerKind.LLM_PREFILL_DECODE,
        model_family=ModelFamily.LLM,
        model_format=ModelFormat.UNKNOWN,
        name="LLM prefill/decode planner placeholder",
        description="Reserved planner kind for future LLM prefill/decode split.",
    ))
    registry.register(PlannerBackend(
        planner_kind=PlannerKind.LLM_TENSOR_PARALLEL,
        model_family=ModelFamily.LLM,
        model_format=ModelFormat.UNKNOWN,
        name="LLM tensor-parallel planner placeholder",
        description="Reserved planner kind for future LLM tensor parallelism.",
    ))
    return registry
