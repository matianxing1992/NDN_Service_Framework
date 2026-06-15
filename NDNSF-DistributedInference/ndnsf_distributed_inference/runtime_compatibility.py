"""Model artifact and runtime backend compatibility helpers.

This module is intentionally planner-neutral.  A model-family-specific planner
may add richer checks later, but deployment tooling should have one place to
reject clearly impossible artifact/runtime pairings.
"""

from __future__ import annotations

from .plan import ModelFamily, ModelFormat, normalize_model_family, normalize_model_format


RUNTIME_COMPATIBILITY: dict[str, dict[str, tuple[str, ...]]] = {
    ModelFamily.GENERIC_ONNX.value: {
        ModelFormat.ONNX.value: ("onnxruntime",),
    },
    ModelFamily.YOLO_ONNX.value: {
        ModelFormat.ONNX.value: ("onnxruntime",),
    },
    ModelFamily.LLM.value: {
        ModelFormat.HF_TRANSFORMERS.value: ("transformers", "vllm"),
        ModelFormat.SAFETENSORS.value: ("transformers", "vllm"),
        ModelFormat.GGUF.value: ("llama.cpp", "ollama"),
        "tensorrt-engine": ("tensorrt-llm",),
        "mlx": ("mlx-lm",),
        ModelFormat.ONNX.value: ("onnxruntime",),
        ModelFormat.CUSTOM.value: ("custom",),
    },
}


def supported_runtime_backends(
    model_family: str | ModelFamily,
    model_format: str | ModelFormat,
) -> tuple[str, ...]:
    family = normalize_model_family(model_family)
    fmt = normalize_model_format(model_format)
    return RUNTIME_COMPATIBILITY.get(family, {}).get(fmt, ())


def default_runtime_backend(
    model_family: str | ModelFamily,
    model_format: str | ModelFormat,
) -> str:
    supported = supported_runtime_backends(model_family, model_format)
    if not supported:
        return ""
    return supported[0]


def validate_runtime_compatibility(
    model_family: str | ModelFamily,
    model_format: str | ModelFormat,
    runtime_backend: str,
    *,
    require_known: bool = False,
) -> str:
    family = normalize_model_family(model_family)
    fmt = normalize_model_format(model_format)
    backend = str(runtime_backend or "").strip()
    supported = supported_runtime_backends(family, fmt)
    if not supported:
        if require_known:
            known = RUNTIME_COMPATIBILITY.get(family, {})
            known_formats = ", ".join(sorted(known)) or "(none)"
            raise ValueError(
                f"unsupported runtime compatibility for model family "
                f"{family!r} and model format {fmt!r}; known formats for "
                f"that family: {known_formats}")
        return backend
    if not backend:
        backend = supported[0]
    if backend not in supported:
        raise ValueError(
            f"runtime backend {backend!r} is incompatible with model family "
            f"{family!r} and model format {fmt!r}; supported backends: "
            f"{', '.join(supported)}")
    return backend
