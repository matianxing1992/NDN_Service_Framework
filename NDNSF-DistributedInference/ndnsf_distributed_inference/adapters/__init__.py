"""Model-family-neutral adapter contracts and explicit registration."""

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
from .qwen import (
    QWEN36_27B_LAYER_RANGES,
    QWEN36_27B_MODEL,
    QWEN36_27B_REVISION,
    QWEN36_STAGE_ROLES,
    QwenThreeStageSplitter,
    build_qwen_three_stage_adapter,
    build_qwen36_27b_three_stage_adapter,
)
from .yolo import build_yolo26n_adapter, Yolo26GraphAdapter, Yolo26Splitter


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
    "QWEN36_27B_LAYER_RANGES",
    "QWEN36_27B_MODEL",
    "QWEN36_27B_REVISION",
    "QWEN36_STAGE_ROLES",
    "QwenThreeStageSplitter",
    "build_qwen_three_stage_adapter",
    "build_qwen36_27b_three_stage_adapter",
    "build_yolo26n_adapter",
    "Yolo26GraphAdapter",
    "Yolo26Splitter",
    "register",
]
