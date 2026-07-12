"""Bounded, deterministic contracts for the Spec 105 Qwen MiniNDN pilot."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Sequence


MAX_INPUT_TOKENS = 512
MAX_OUTPUT_TOKENS = 32


class CacheResolution(str, Enum):
    HIT = "HIT"
    FULL_CONTEXT_REBUILD = "FULL_CONTEXT_REBUILD"


class QwenPilotTerminalError(RuntimeError):
    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


@dataclass(frozen=True)
class QwenPilotRequest:
    input_token_ids: tuple[int, ...]
    max_new_tokens: int

    def validate(self) -> None:
        if not self.input_token_ids:
            raise ValueError("Qwen pilot requires at least one input token")
        if len(self.input_token_ids) > MAX_INPUT_TOKENS:
            raise ValueError("Qwen pilot input exceeds 512 tokens")
        if self.max_new_tokens < 1 or self.max_new_tokens > MAX_OUTPUT_TOKENS:
            raise ValueError("Qwen pilot output must be between 1 and 32 tokens")
        if any(not isinstance(token, int) or token < 0 for token in self.input_token_ids):
            raise ValueError("Qwen pilot token IDs must be non-negative integers")


def greedy_decode_fixture(logit_steps: Iterable[Sequence[float]],
                          max_new_tokens: int) -> list[int]:
    """Deterministic argmax oracle used by bounded correctness fixtures."""
    if max_new_tokens < 1 or max_new_tokens > MAX_OUTPUT_TOKENS:
        raise ValueError("max_new_tokens must be between 1 and 32")
    result: list[int] = []
    for logits in logit_steps:
        if len(result) == max_new_tokens:
            break
        if not logits:
            raise ValueError("logit step must not be empty")
        result.append(max(range(len(logits)), key=lambda index: logits[index]))
    if len(result) != max_new_tokens:
        raise ValueError("insufficient logit steps for requested greedy output")
    return result


def resolve_cache_request(*, cache_present: bool, full_context_present: bool,
                          delta_only: bool) -> CacheResolution:
    if cache_present:
        return CacheResolution.HIT
    if full_context_present and not delta_only:
        return CacheResolution.FULL_CONTEXT_REBUILD
    raise QwenPilotTerminalError("CACHE_MISS_FULL_CONTEXT_REQUIRED")
