"""Deployment-safe Qwen sampling and incremental text contracts.

This module deliberately depends only on the standard library.  Model logits
come from the ONNX Runtime adapter; tokenization is injected through
``StandaloneQwenTokenizer`` so the deployed package never imports PyTorch or
Transformers.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import math
import json
from typing import Any, Callable, Iterable, Mapping, Sequence
from urllib.parse import quote


class GenerationContractError(ValueError):
    """Raised when a sealed generation contract is malformed."""


@dataclass(frozen=True)
class SamplingConfig:
    mode: str = "Greedy"
    temperature: float = 0.0
    top_k: int = 1
    top_p: float = 1.0
    repetition_penalty: float = 1.0
    seed: int = 1_750_001

    def validate(self, vocabulary_size: int) -> None:
        if self.mode not in {"Greedy", "SeededTopKTopP"}:
            raise GenerationContractError("unsupported sampling mode")
        if vocabulary_size < 1:
            raise GenerationContractError("empty sampling vocabulary")
        if self.mode == "Greedy" and self.temperature != 0.0:
            raise GenerationContractError("Greedy sampling requires temperature=0")
        if self.mode != "Greedy" and not 0.0 < self.temperature <= 5.0:
            raise GenerationContractError("sampling temperature is out of range")
        if not 1 <= self.top_k <= vocabulary_size:
            raise GenerationContractError("top_k is out of range")
        if not 0.0 < self.top_p <= 1.0:
            raise GenerationContractError("top_p is out of range")
        if not 0.1 <= self.repetition_penalty <= 2.0:
            raise GenerationContractError("repetition penalty is out of range")
        if self.seed < 0 or self.seed >= 2**64:
            raise GenerationContractError("sampling seed is out of range")


def _penalized_logits(logits: Sequence[float], generated: Iterable[int],
                      penalty: float) -> list[float]:
    result = [float(value) for value in logits]
    for token in set(int(value) for value in generated):
        if token < 0 or token >= len(result):
            continue
        if result[token] >= 0:
            result[token] /= penalty
        else:
            result[token] *= penalty
    return result


def _splitmix64(value: int) -> int:
    value = (int(value) + 0x9E3779B97F4A7C15) & ((1 << 64) - 1)
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & ((1 << 64) - 1)
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & ((1 << 64) - 1)
    return (value ^ (value >> 31)) & ((1 << 64) - 1)


def _deterministic_unit(seed: int, step: int) -> float:
    return float(_splitmix64(int(seed) + int(step)) >> 11) / float(1 << 53)


def sample_token(logits: Sequence[float], config: SamplingConfig,
                 *, generated: Sequence[int] = (), step: int = 0) -> int:
    """Sample one token with a sealed greedy or seeded Top-K/Top-P policy."""
    if not logits:
        raise GenerationContractError("sampling logits must not be empty")
    config.validate(len(logits))
    adjusted = _penalized_logits(logits, generated, config.repetition_penalty)
    if config.mode == "Greedy":
        return max(range(len(adjusted)), key=lambda index: adjusted[index])

    temperature = config.temperature
    candidates = sorted(range(len(adjusted)),
                        key=lambda index: adjusted[index], reverse=True)
    candidates = candidates[:config.top_k]
    scaled = [adjusted[index] / temperature for index in candidates]
    maximum = max(scaled)
    weights = [math.exp(value - maximum) for value in scaled]
    total = sum(weights)
    if not math.isfinite(total) or total <= 0:
        raise GenerationContractError("non-finite sampling distribution")
    probabilities = [weight / total for weight in weights]
    retained: list[int] = []
    cumulative = 0.0
    for index, probability in zip(candidates, probabilities):
        retained.append(index)
        cumulative += probability
        if cumulative >= config.top_p:
            break
    retained_weights = [weights[candidates.index(index)] for index in retained]
    draw = _deterministic_unit(config.seed, step) * sum(retained_weights)
    cumulative = 0.0
    for offset, (index, weight) in enumerate(zip(retained, retained_weights)):
        cumulative += weight
        if draw < cumulative or offset + 1 == len(retained_weights):
            return index
    raise GenerationContractError("sampler failed to select a token")


@dataclass(frozen=True)
class GenerationTokenEventV1:
    request_id: str
    token_epoch: int
    token_id: int
    text_delta: str
    accepted_prefix_digest: str

    def __post_init__(self) -> None:
        if (not self.request_id or self.token_epoch < 1 or self.token_id < 0
                or not isinstance(self.text_delta, str)
                or not self.accepted_prefix_digest.startswith("sha256:")):
            raise GenerationContractError("invalid GenerationTokenEventV1")
        if len(self.accepted_prefix_digest) != len("sha256:") + 64:
            raise GenerationContractError("invalid token prefix digest")


def accepted_prefix_digest(token_ids: Sequence[int]) -> str:
    wire = ",".join(str(int(token)) for token in token_ids).encode("ascii")
    return "sha256:" + hashlib.sha256(wire).hexdigest()


class IncrementalDetokenizer:
    """Decode committed IDs and emit only a valid UTF-8 suffix per token."""

    def __init__(self, decode: Callable[[Sequence[int]], str], *,
                 stop_strings: Sequence[str] = ()) -> None:
        if not callable(decode):
            raise TypeError("decode callback is required")
        stops = tuple(str(value) for value in stop_strings)
        if len(stops) > 16 or any(not value or len(value.encode("utf-8")) > 256
                                  for value in stops):
            raise GenerationContractError("invalid stop string set")
        self._decode = decode
        self._stop_strings = stops
        self._token_ids: list[int] = []
        self._text = ""
        self._matched_stop = ""

    @property
    def token_ids(self) -> tuple[int, ...]:
        return tuple(self._token_ids)

    @property
    def text(self) -> str:
        return self._text

    @property
    def matched_stop(self) -> str:
        return self._matched_stop

    def preview(self, token_id: int) -> tuple[str, str, str]:
        """Validate a token without mutating committed decoder state."""
        if self._matched_stop:
            raise GenerationContractError("detokenizer is terminal")
        if int(token_id) < 0:
            raise GenerationContractError("token ID must be non-negative")
        candidate_ids = self._token_ids + [int(token_id)]
        decoded = str(self._decode(tuple(candidate_ids)))
        if not decoded.startswith(self._text):
            raise GenerationContractError("tokenizer rewrote committed UTF-8 prefix")
        delta = decoded[len(self._text):]
        # Python strings are Unicode scalar sequences; encode/decode here to
        # make the boundary explicit and reject malformed adapter output.
        delta.encode("utf-8", errors="strict").decode("utf-8", errors="strict")
        matched_stop = ""
        for stop in self._stop_strings:
            if decoded.endswith(stop):
                matched_stop = stop
                break
        return delta, decoded, matched_stop

    def push(self, token_id: int) -> str:
        delta, decoded, matched_stop = self.preview(token_id)
        self._token_ids.append(int(token_id))
        self._text = decoded
        self._matched_stop = matched_stop
        return delta


INTERNAL_ACTIVATION_PREFIX = "/NDNSF/DI/ACTIVATION"
INTERNAL_TOKEN_FEEDBACK_PREFIX = "/NDNSF/DI/TOKEN-FEEDBACK"
EXTERNAL_TOKEN_EVENT_TYPE = "GenerationTokenEventV1"


def _name_component(value: str) -> str:
    encoded = quote(str(value).strip("/"), safe="-._~")
    if not encoded or "/" in encoded:
        raise GenerationContractError("invalid internal name component")
    return encoded


def _role_component(role: str) -> str:
    return _name_component(str(role).rsplit("/", 1)[-1])


def activation_data_name(*, producer: str, requester: str, service: str,
                        request_id: str, attempt_epoch: int,
                        plan_digest: str, generation_id: str,
                        from_role: str, to_role: str,
                        inference_epoch: int) -> str:
    """Build the exact name frozen by generation-state-machine-v1."""
    if int(attempt_epoch) not in (1, 2) or int(inference_epoch) < 0:
        raise GenerationContractError("internal activation epoch is invalid")
    _digest_or_error(plan_digest, "plan")
    return "/".join((
        str(producer).rstrip("/"), INTERNAL_ACTIVATION_PREFIX.lstrip("/"),
        _name_component(requester), *_service_components(service),
        _name_component(request_id), str(int(attempt_epoch)), plan_digest[7:],
        _name_component(generation_id), _role_component(from_role),
        _role_component(to_role), str(int(inference_epoch)),
    ))


def token_feedback_data_name(*, producer: str, requester: str, service: str,
                             request_id: str, attempt_epoch: int,
                             plan_digest: str, generation_id: str,
                             final_role: str, first_role: str,
                             token_epoch: int) -> str:
    if int(attempt_epoch) not in (1, 2) or int(token_epoch) < 1:
        raise GenerationContractError("internal token-feedback epoch is invalid")
    _digest_or_error(plan_digest, "plan")
    return "/".join((
        str(producer).rstrip("/"), INTERNAL_TOKEN_FEEDBACK_PREFIX.lstrip("/"),
        _name_component(requester), *_service_components(service),
        _name_component(request_id), str(int(attempt_epoch)), plan_digest[7:],
        _name_component(generation_id), _role_component(final_role),
        _role_component(first_role), str(int(token_epoch)),
    ))


def _service_components(service: str) -> tuple[str, ...]:
    components = tuple(_name_component(value) for value in str(service).split("/")
                       if value)
    if not components:
        raise GenerationContractError("service name is empty")
    return components


def _digest_or_error(value: str, label: str) -> None:
    if not isinstance(value, str) or not value.startswith("sha256:") \
            or len(value) != len("sha256:") + 64:
        raise GenerationContractError(f"invalid {label} digest")


@dataclass(frozen=True)
class StreamedGenerationResult:
    request_id: str
    token_ids: tuple[int, ...]
    text: str
    finish_reason: str
    events: tuple[GenerationTokenEventV1, ...]


class OnePlanGenerationLoop:
    """Application-owned autoregressive loop over one accepted placement plan.

    ``prefill`` and ``decode`` are provider-role adapters.  The loop owns
    token sampling, incremental text state, the final-role split between
    internal feedback and external events, and the single terminal claim.
    No callback is allowed to create another Request or placement plan.
    """

    def __init__(self, *, request_id: str, requester: str, service: str,
                 attempt_epoch: int, plan_digest: str, generation_id: str,
                 roles: Sequence[str], first_role: str, final_role: str,
                 max_generated_tokens: int, eos_token_ids: Sequence[int] = (),
                 stop_strings: Sequence[str] = (), sampling: SamplingConfig | None = None,
                 decode_text: Callable[[Sequence[int]], str],
                 prefill: Callable[[Any, int], Mapping[str, Any]],
                 decode: Callable[[Any, int, int], Mapping[str, Any]],
                 provider_by_role: Mapping[str, str] | None = None,
                 publish_activation: Callable[[str, str, str, Any], None] | None = None,
                 publish_feedback: Callable[[str, str, Any], None] | None = None,
                 publish_event: Callable[[bytes], Any] | None = None,
                 finish: Callable[[bytes, str], Any] | None = None,
                 cancelled: Callable[[], bool] | None = None,
                 committed_prefix_token_ids: Sequence[int] = ()) -> None:
        if not request_id or not requester or not roles or first_role not in roles \
                or final_role not in roles:
            raise GenerationContractError("one-plan generation identity is incomplete")
        if int(max_generated_tokens) < 1:
            raise GenerationContractError("max_generated_tokens must be positive")
        if int(max_generated_tokens) > 4095:
            raise GenerationContractError("max_generated_tokens exceeds stream bound")
        if int(attempt_epoch) not in (1, 2):
            raise GenerationContractError("attempt_epoch must be 1 or 2")
        _digest_or_error(plan_digest, "plan")
        self.request_id = str(request_id)
        self.requester = str(requester)
        self.service = str(service)
        self.attempt_epoch = int(attempt_epoch)
        self.plan_digest = str(plan_digest)
        self.generation_id = str(generation_id)
        self.roles = tuple(str(role) for role in roles)
        if (len(set(self.roles)) != len(self.roles)
                or any(not role for role in self.roles)):
            raise GenerationContractError("generation roles must be unique")
        self.first_role = str(first_role)
        self.final_role = str(final_role)
        self.max_generated_tokens = int(max_generated_tokens)
        self.eos_token_ids = frozenset(int(value) for value in eos_token_ids)
        self.committed_prefix_token_ids = tuple(
            int(value) for value in committed_prefix_token_ids)
        if (self.attempt_epoch == 1 and self.committed_prefix_token_ids
                or len(self.committed_prefix_token_ids)
                >= self.max_generated_tokens
                or any(value < 0 or value in self.eos_token_ids
                       for value in self.committed_prefix_token_ids)):
            raise GenerationContractError("invalid committed recovery prefix")
        self.sampling = sampling or SamplingConfig()
        self.decode_text = decode_text
        self.prefill = prefill
        self.decode = decode
        self.provider_by_role = {
            str(role): str(provider)
            for role, provider in dict(provider_by_role or {}).items()
        }
        if any(not value for value in self.provider_by_role.values()):
            raise GenerationContractError("activation Provider identity is empty")
        if (publish_activation is not None
                and len(self.roles) > 1
                and set(self.provider_by_role) != set(self.roles)):
            raise GenerationContractError(
                "activation Provider map must cover every generation role")
        self.publish_activation = publish_activation
        self.publish_feedback = publish_feedback
        self.publish_event = publish_event
        self.finish = finish
        self.cancelled = cancelled or (lambda: False)
        self.detokenizer = IncrementalDetokenizer(decode_text,
                                                   stop_strings=stop_strings)

    def run(self, prompt: Any) -> StreamedGenerationResult:
        def require_admitted(result: Any, operation: str) -> None:
            """Reject an explicit Core/transport refusal.

            Test/application callbacks commonly return ``None`` (for example
            ``list.append``), so only an explicit ``False`` or a non-positive
            integer is a refusal.  The native Provider writer returns a
            positive cursor for an admitted event and ``0`` on rejection.
            Keeping this check here prevents token state and feedback from
            advancing after an event was not accepted by Core.
            """
            if result is False:
                raise GenerationContractError(f"{operation} was rejected")
            if isinstance(result, int) and not isinstance(result, bool) and result <= 0:
                raise GenerationContractError(f"{operation} was rejected")

        state = self.prefill(prompt, 0)
        token_ids: list[int] = []
        events: list[GenerationTokenEventV1] = []
        finish_reason = ""
        previous_token = None
        for token_epoch in range(1, self.max_generated_tokens + 1):
            if self.cancelled():
                finish_reason = "cancelled"
                break
            if token_epoch > 1:
                state = self.decode(state, token_epoch - 1, int(previous_token))
            if self.publish_activation is not None:
                activations = (state.get("activations", {})
                               if isinstance(state, Mapping) else {})
                for from_role, to_role in zip(self.roles, self.roles[1:]):
                    payload = activations.get((from_role, to_role))
                    if payload is None:
                        payload = activations.get(to_role)
                    if payload is None:
                        raise GenerationContractError(
                            "missing activation for selected role boundary")
                    activation_name = activation_data_name(
                        producer=self.provider_by_role.get(from_role, from_role),
                        requester=self.requester,
                        service=self.service, request_id=self.request_id,
                        attempt_epoch=self.attempt_epoch,
                        plan_digest=self.plan_digest,
                        generation_id=self.generation_id,
                        from_role=from_role, to_role=to_role,
                        inference_epoch=token_epoch - 1)
                    require_admitted(
                        self.publish_activation(
                            activation_name, from_role, to_role, payload),
                        "activation publication")
            logits = state.get("logits") if isinstance(state, Mapping) else None
            if logits is None:
                raise GenerationContractError("final role did not return logits")
            token = sample_token(logits, self.sampling,
                                 generated=token_ids, step=token_epoch - 1)
            # Core admission is the commit point for a token. A rejected
            # event must not mutate the accepted prefix or tokenizer state.
            delta, _candidate_text, candidate_stop = self.detokenizer.preview(token)
            candidate_token_ids = token_ids + [token]
            event = GenerationTokenEventV1(
                request_id=self.request_id, token_epoch=token_epoch,
                token_id=token, text_delta=delta,
                accepted_prefix_digest=accepted_prefix_digest(candidate_token_ids),
            )
            replaying_prefix = token_epoch <= len(
                self.committed_prefix_token_ids)
            if (replaying_prefix
                    and token != self.committed_prefix_token_ids[token_epoch - 1]):
                raise GenerationContractError(
                    "recomputed committed prefix does not match")
            # Event admission precedes feedback and accepted-prefix mutation.
            if self.publish_event is not None and not replaying_prefix:
                wire = json.dumps({
                    "type": EXTERNAL_TOKEN_EVENT_TYPE,
                    "requestId": event.request_id,
                    "tokenEpoch": event.token_epoch,
                    "tokenId": event.token_id,
                    "textDelta": event.text_delta,
                    "acceptedPrefixDigest": event.accepted_prefix_digest,
                }, sort_keys=True, separators=(",", ":")).encode("utf-8")
                require_admitted(self.publish_event(wire), "event publication")
            self.detokenizer.push(token)
            token_ids.append(token)
            if not replaying_prefix:
                events.append(event)
            if self.publish_feedback is not None:
                feedback_name = token_feedback_data_name(
                    producer=self.final_role, requester=self.requester,
                    service=self.service, request_id=self.request_id,
                    attempt_epoch=self.attempt_epoch, plan_digest=self.plan_digest,
                    generation_id=self.generation_id, final_role=self.final_role,
                    first_role=self.first_role, token_epoch=token_epoch)
                require_admitted(
                    self.publish_feedback(feedback_name, self.first_role, {
                        "tokenId": token, "tokenEpoch": token_epoch,
                        "acceptedPrefixDigest": event.accepted_prefix_digest,
                    }),
                    "token feedback publication")
            if token in self.eos_token_ids:
                finish_reason = "eos"
                break
            if candidate_stop:
                finish_reason = "stop_sequence"
                break
            if token_epoch == self.max_generated_tokens:
                finish_reason = "max_tokens"
                break
            previous_token = token
        if not finish_reason:
            finish_reason = "failed"
        result = StreamedGenerationResult(
            request_id=self.request_id, token_ids=tuple(token_ids),
            text=self.detokenizer.text, finish_reason=finish_reason,
            events=tuple(events))
        if self.finish is not None:
            payload = json.dumps({
                "requestId": result.request_id,
                "tokenIds": list(result.token_ids),
                "text": result.text,
                "finishReason": result.finish_reason,
            }, sort_keys=True, separators=(",", ":")).encode("utf-8")
            require_admitted(
                self.finish(payload, result.finish_reason),
                "stream completion")
        return result


__all__ = [
    "GenerationContractError",
    "SamplingConfig",
    "sample_token",
    "GenerationTokenEventV1",
    "accepted_prefix_digest",
    "IncrementalDetokenizer",
    "INTERNAL_ACTIVATION_PREFIX",
    "INTERNAL_TOKEN_FEEDBACK_PREFIX",
    "EXTERNAL_TOKEN_EVENT_TYPE",
    "activation_data_name",
    "token_feedback_data_name",
    "StreamedGenerationResult",
    "OnePlanGenerationLoop",
]
