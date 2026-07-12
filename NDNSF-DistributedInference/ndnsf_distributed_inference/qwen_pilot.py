"""Bounded, deterministic contracts for the Spec 105 Qwen MiniNDN pilot."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Callable, Iterable, Sequence


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


def compare_token_sequences(expected: Sequence[int], actual: Sequence[int]) -> None:
    if len(expected) != len(actual):
        raise QwenPilotTerminalError(
            f"TOKEN_COUNT_MISMATCH expected={len(expected)} actual={len(actual)}")
    for index, (expected_token, actual_token) in enumerate(zip(expected, actual)):
        if expected_token != actual_token:
            raise QwenPilotTerminalError(
                f"TOKEN_MISMATCH index={index} expected={expected_token} actual={actual_token}")


class QwenPilotOrchestrator:
    """Bounded greedy loop around an injected tokenizer and staged-logit call."""

    def __init__(self,
                 tokenizer: Callable[[str], Sequence[int]],
                 staged_logits: Callable[[tuple[int, ...]], Sequence[float]]) -> None:
        self._tokenizer = tokenizer
        self._staged_logits = staged_logits

    def request(self, prompt: str, max_new_tokens: int) -> QwenPilotRequest:
        if not isinstance(prompt, str) or not prompt:
            raise ValueError("Qwen pilot prompt must not be empty")
        request = QwenPilotRequest(
            tuple(int(token) for token in self._tokenizer(prompt)),
            int(max_new_tokens),
        )
        request.validate()
        return request

    def generate(self, request: QwenPilotRequest) -> list[int]:
        request.validate()
        context = list(request.input_token_ids)
        generated: list[int] = []
        for _ in range(request.max_new_tokens):
            logits = self._staged_logits(tuple(context))
            if not logits:
                raise QwenPilotTerminalError("EMPTY_LOGITS")
            token = max(range(len(logits)), key=lambda index: logits[index])
            generated.append(token)
            context.append(token)
        return generated
