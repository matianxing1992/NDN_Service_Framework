"""Qwen model-family adapters."""

from .placement import (
    QWEN36_27B_LAYER_RANGES,
    QWEN36_27B_MODEL,
    QWEN36_27B_REVISION,
    QWEN36_STAGE_ROLES,
    QwenThreeStageSplitter,
    build_qwen_three_stage_adapter,
    build_qwen36_27b_three_stage_adapter,
)
from .parallel import seal_qwen_hybrid_plan
from .tokenizer import StandaloneQwenTokenizer

__all__ = [
    "QWEN36_27B_LAYER_RANGES",
    "QWEN36_27B_MODEL",
    "QWEN36_27B_REVISION",
    "QWEN36_STAGE_ROLES",
    "QwenThreeStageSplitter",
    "build_qwen_three_stage_adapter",
    "build_qwen36_27b_three_stage_adapter",
    "seal_qwen_hybrid_plan",
    "StandaloneQwenTokenizer",
]
