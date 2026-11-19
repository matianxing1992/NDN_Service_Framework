"""Qwen exports load on selection; offline codecs do not open runtime bindings."""

from importlib import import_module

_EXPORTS = {
    'QWEN36_27B_LAYER_RANGES': '.placement',
    'QWEN36_27B_DECODE_MODE': '.placement',
    'QWEN36_27B_MODEL': '.placement',
    'QWEN36_27B_MODALITY': '.placement',
    'QWEN36_27B_MTP_ENABLED': '.placement',
    'QWEN36_27B_PRECISION': '.placement',
    'QWEN36_27B_REVISION': '.placement',
    'QWEN36_27B_THINKING_MODE': '.placement',
    'QWEN36_STAGE_ROLES': '.placement',
    'QwenThreeStageSplitter': '.placement',
    'build_qwen_three_stage_adapter': '.placement',
    'build_qwen36_27b_three_stage_adapter': '.placement',
    'seal_qwen_hybrid_plan': '.parallel',
    'StandaloneQwenTokenizer': '.tokenizer',
    'GenerationContractError': '.generation',
    'GenerationTokenEventV1': '.generation',
    'OnePlanGenerationLoop': '.generation',
    'StreamedGenerationResult': '.generation',
    'activation_data_name': '.generation',
    'token_feedback_data_name': '.generation',
    'IncrementalDetokenizer': '.generation',
    'SamplingConfig': '.generation',
    'accepted_prefix_digest': '.generation',
    'sample_token': '.generation',
    'DecodeStateBundleV1': '.stateful_onnx',
    'DecodeStateContractError': '.stateful_onnx',
    'DecodeStateIdentityV1': '.stateful_onnx',
    'DecodeStateTransaction': '.stateful_onnx',
    'PersistentStatefulOnnxSession': '.stateful_onnx',
    'StatefulOnnxIOContractV1': '.stateful_onnx',
    'StateComponent': '.stateful_onnx',
}


def __getattr__(name):
    module = _EXPORTS.get(name)
    if module is None:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    value = getattr(import_module(module, __name__), name)
    globals()[name] = value
    return value


__all__ = list(_EXPORTS)
