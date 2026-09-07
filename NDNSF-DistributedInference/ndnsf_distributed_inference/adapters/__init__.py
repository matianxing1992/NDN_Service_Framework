"""Model-family-neutral adapter contracts and explicit registration."""

# Pure format/oracle submodules must remain usable without the native runtime.
# Public adapter APIs retain their names and load their owners on first access.
_OWNERS = {
    **dict.fromkeys(('AdapterPortDescriptor', 'ApplicationInput', 'InputTransportMode',
        'GraphAdapter', 'InferenceStateClass', 'InferenceStateContract', 'InferenceTaskDescriptor',
        'ModelFamilyAdapter', 'ModelSplitter', 'RunnerAdapter', 'StateAdapter', 'TaskAdapter'), 'base'),
    **dict.fromkeys(('build_llm_text_adapter', 'build_object_detection_adapter',
                    'build_opaque_container_adapter'), 'builtin'),
    **dict.fromkeys(('QWEN36_27B_LAYER_RANGES', 'QWEN36_27B_MODEL', 'QWEN36_27B_REVISION',
        'QWEN36_STAGE_ROLES', 'QwenThreeStageSplitter', 'build_qwen_three_stage_adapter',
        'build_qwen36_27b_three_stage_adapter'), 'qwen'),
    **dict.fromkeys(('build_yolo26n_adapter', 'Yolo26GraphAdapter', 'Yolo26Splitter'), 'yolo'),
}


def __getattr__(name):
    from importlib import import_module
    if name not in _OWNERS:
        raise AttributeError(name)
    value = getattr(import_module('.' + _OWNERS[name], __name__), name)
    globals()[name] = value
    return value


def __dir__():
    return sorted(set(globals()) | set(__all__))


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
