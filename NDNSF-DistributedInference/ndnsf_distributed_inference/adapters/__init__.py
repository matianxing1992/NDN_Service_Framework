"""Model-family-neutral contracts; model exports load only when selected."""

from importlib import import_module

from .base import (
    AdapterPortDescriptor,
    ApplicationInput,
    InputTransportMode,
    GraphAdapter,
    InferenceStateClass,
    InferenceStateContract,
    InferenceTaskDescriptor,
    ModelFamilyAdapter,
    ModelSplitter,
    RunnerAdapter,
    StateAdapter,
    TaskAdapter,
)
from .builtin import (
    build_llm_text_adapter,
    build_object_detection_adapter,
    build_opaque_container_adapter,
)
_MODEL_EXPORTS = {
    "QWEN36_27B_LAYER_RANGES": ".qwen",
    "QWEN36_27B_MODEL": ".qwen",
    "QWEN36_27B_REVISION": ".qwen",
    "QWEN36_STAGE_ROLES": ".qwen",
    "QwenThreeStageSplitter": ".qwen",
    "build_qwen_three_stage_adapter": ".qwen",
    "build_qwen36_27b_three_stage_adapter": ".qwen",
    "build_yolo26n_adapter": ".yolo",
    "Yolo26GraphAdapter": ".yolo",
    "Yolo26Splitter": ".yolo",
}


def __getattr__(name):
    module = _MODEL_EXPORTS.get(name)
    if module is None:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    value = getattr(import_module(module, __name__), name)
    globals()[name] = value
    return value


def register(registry, adapter):
    registry.register(adapter)
    return adapter


__all__ = [
    "AdapterPortDescriptor",
    "ApplicationInput",
    "InputTransportMode",
    "GraphAdapter",
    "InferenceStateClass",
    "InferenceStateContract",
    "InferenceTaskDescriptor",
    "ModelFamilyAdapter",
    "ModelSplitter",
    "RunnerAdapter",
    "StateAdapter",
    "TaskAdapter",
    "build_llm_text_adapter",
    "build_object_detection_adapter",
    "build_opaque_container_adapter",
    *_MODEL_EXPORTS,
    "register",
]
