"""Qwen-owned bounded deterministic MiniNDN pilot contracts."""

from __future__ import annotations

from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from enum import Enum
import hashlib
import json
from threading import BoundedSemaphore, Lock
from time import time
from typing import Callable, Iterable, Mapping, Sequence, TypeVar

from ...artifact_deployment import (
    ExecutionContext,
    RuntimePreparationEvidence,
)


MAX_INPUT_TOKENS = 512
MAX_OUTPUT_TOKENS = 64
GenerationResult = TypeVar("GenerationResult")


class GenerationQueueFull(RuntimeError):
    pass


@dataclass(frozen=True)
class GenerationSchedulerSnapshot:
    active: int
    queued: int
    completed: int
    failed: int
    unfinished: int
    max_active_observed: int
    max_queued_observed: int
    token_progress: dict[str, int]


class BoundedGenerationScheduler:
    """Run each bounded generation as one worker-owned application job."""

    def __init__(self, *, max_workers: int, max_queued: int) -> None:
        if max_workers < 1:
            raise ValueError("max_workers must be positive")
        if max_queued < 0:
            raise ValueError("max_queued must be non-negative")
        self._capacity = BoundedSemaphore(max_workers + max_queued)
        self._lock = Lock()
        self._active = 0
        self._queued = 0
        self._completed = 0
        self._failed = 0
        self._max_active_observed = 0
        self._max_queued_observed = 0
        self._token_progress: dict[str, int] = {}
        self._executor = ThreadPoolExecutor(
            max_workers=max_workers,
            thread_name_prefix="ndnsf-di-generation",
        )

    def submit(
        self,
        session_id: str,
        generation: Callable[[Callable[[int], None]], GenerationResult],
    ) -> Future:
        if not session_id:
            raise ValueError("session_id must not be empty")
        if not self._capacity.acquire(blocking=False):
            raise GenerationQueueFull("generation scheduler queue is full")
        with self._lock:
            if session_id in self._token_progress:
                self._capacity.release()
                raise ValueError(f"duplicate generation session: {session_id}")
            self._token_progress[session_id] = 0
            self._queued += 1
            self._max_queued_observed = max(
                self._max_queued_observed, self._queued)

        def run_generation() -> GenerationResult:
            with self._lock:
                self._queued -= 1
                self._active += 1
                self._max_active_observed = max(
                    self._max_active_observed, self._active)

            def report_progress(token_count: int) -> None:
                with self._lock:
                    next_count = int(token_count)
                    current_count = self._token_progress[session_id]
                    if next_count <= current_count:
                        raise ValueError(
                            "generation token progress must increase monotonically")
                    self._token_progress[session_id] = next_count

            try:
                result = generation(report_progress)
            except BaseException:
                with self._lock:
                    self._failed += 1
                raise
            else:
                with self._lock:
                    self._completed += 1
                return result
            finally:
                with self._lock:
                    self._active -= 1
                self._capacity.release()

        try:
            return self._executor.submit(run_generation)
        except BaseException:
            with self._lock:
                self._queued -= 1
                self._token_progress.pop(session_id, None)
            self._capacity.release()
            raise

    def snapshot(self) -> GenerationSchedulerSnapshot:
        with self._lock:
            return GenerationSchedulerSnapshot(
                active=self._active,
                queued=self._queued,
                completed=self._completed,
                failed=self._failed,
                unfinished=self._active + self._queued,
                max_active_observed=self._max_active_observed,
                max_queued_observed=self._max_queued_observed,
                token_progress=dict(self._token_progress),
            )

    def shutdown(self, wait: bool = True) -> None:
        self._executor.shutdown(wait=wait)


class CacheResolution(str, Enum):
    HIT = "HIT"
    FULL_CONTEXT_REBUILD = "FULL_CONTEXT_REBUILD"


class QwenPilotTerminalError(RuntimeError):
    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


class QwenCudaRuntimePreparer:
    """Adapter boundary that proves a verified Qwen shard is CUDA-ready."""

    def __init__(
        self, *, adapter_id: str, adapter_version: str,
        backend: str, device: str,
        load_shard: Callable[[ExecutionContext, str], object],
        warmup: Callable[[object], bool],
        runtime_probe: Callable[[object], Mapping[str, object]],
        clock_ms: Callable[[], int] | None = None,
    ) -> None:
        if (not adapter_id or not adapter_version or not backend or not device
                or not callable(load_shard) or not callable(warmup)
                or not callable(runtime_probe)):
            raise ValueError("invalid Qwen CUDA runtime preparer")
        self.adapter_id = adapter_id
        self.adapter_version = adapter_version
        self.backend = backend
        self.device = device
        self._load_shard = load_shard
        self._warmup = warmup
        self._runtime_probe = runtime_probe
        self._clock_ms = clock_ms or (lambda: int(time() * 1000))

    def __call__(
        self, execution: ExecutionContext,
        progress: Callable[[str, float], None],
    ) -> RuntimePreparationEvidence:
        artifact_digests = []
        for artifact in execution.spec.artifacts or []:
            path = execution.path(artifact.name)
            if not path.is_file():
                raise RuntimeError(
                    f"Qwen runtime artifact is unavailable: {artifact.name}")
            digest = hashlib.sha256()
            with path.open("rb") as artifact_stream:
                while True:
                    chunk = artifact_stream.read(1024 * 1024)
                    if not chunk:
                        break
                    digest.update(chunk)
            observed = digest.hexdigest()
            expected = str(artifact.sha256)
            if expected.startswith("sha256:"):
                expected = expected[7:]
            if observed != expected:
                raise RuntimeError(
                    f"Qwen runtime artifact hash mismatch: {artifact.name}")
            artifact_digests.append("sha256:" + observed)
        if not artifact_digests:
            raise RuntimeError("Qwen runtime has no verified model artifact")

        progress("LOADING", 0.75)
        handle = self._load_shard(execution, self.device)
        if handle is None:
            raise RuntimeError("Qwen adapter returned no loaded runtime")
        progress("WARMING", 0.90)
        if self._warmup(handle) is not True:
            raise RuntimeError("Qwen adapter warmup did not complete")
        probe = dict(self._runtime_probe(handle) or {})
        return RuntimePreparationEvidence(
            adapter_id=self.adapter_id,
            adapter_version=self.adapter_version,
            backend=str(probe.get("backend", "")),
            device=str(probe.get("device", "")),
            artifact_digests=tuple(artifact_digests),
            load_completed=True,
            warmup_completed=True,
            cpu_fallback_count=int(probe.get("cpuFallbackCount", -1)),
            prepared_at_ms=int(self._clock_ms()),
        )


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
            raise ValueError("Qwen pilot output must be between 1 and 64 tokens")
        if any(not isinstance(token, int) or token < 0 for token in self.input_token_ids):
            raise ValueError("Qwen pilot token IDs must be non-negative integers")


@dataclass(frozen=True)
class QwenTokenEvidence:
    request_id: str
    token_index: int
    context_length: int
    token_id: int

    def __post_init__(self) -> None:
        if (not self.request_id or self.token_index < 0
                or self.context_length <= 0 or self.token_id < 0):
            raise ValueError("invalid Qwen token evidence")


@dataclass(frozen=True)
class QwenGenerationResponse:
    request_id: str
    input_token_ids: tuple[int, ...]
    generated_token_ids: tuple[int, ...]
    decoded_text: str
    stop_reason: str
    token_evidence: tuple[QwenTokenEvidence, ...]

    SCHEMA = "ndnsf-di-qwen-generation-response-v1"

    def __post_init__(self) -> None:
        object.__setattr__(self, "input_token_ids", tuple(self.input_token_ids))
        object.__setattr__(
            self, "generated_token_ids", tuple(self.generated_token_ids))
        object.__setattr__(self, "token_evidence", tuple(self.token_evidence))
        if (not self.request_id or not self.input_token_ids
                or not self.generated_token_ids or not self.decoded_text
                or self.stop_reason not in {"EOS", "MAX_NEW_TOKENS"}
                or len(self.token_evidence) != len(self.generated_token_ids)):
            raise ValueError("invalid complete Qwen generation Response")
        for index, evidence in enumerate(self.token_evidence):
            if (evidence.request_id != self.request_id
                    or evidence.token_index != index
                    or evidence.context_length
                    != len(self.input_token_ids) + index
                    or evidence.token_id != self.generated_token_ids[index]):
                raise ValueError("Qwen token evidence is not ordered and bound")

    def to_bytes(self) -> bytes:
        return json.dumps({
            "schema": self.SCHEMA,
            "requestId": self.request_id,
            "inputTokenIds": list(self.input_token_ids),
            "generatedTokenIds": list(self.generated_token_ids),
            "decodedText": self.decoded_text,
            "stopReason": self.stop_reason,
            "tokenEvidence": [{
                "requestId": item.request_id,
                "tokenIndex": item.token_index,
                "contextLength": item.context_length,
                "tokenId": item.token_id,
            } for item in self.token_evidence],
        }, sort_keys=True, separators=(",", ":")).encode()

    @classmethod
    def from_bytes(cls, payload: bytes) -> "QwenGenerationResponse":
        try:
            value = json.loads(bytes(payload).decode())
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("malformed Qwen generation Response") from exc
        expected = {
            "schema", "requestId", "inputTokenIds", "generatedTokenIds",
            "decodedText", "stopReason", "tokenEvidence",
        }
        if (not isinstance(value, dict) or set(value) != expected
                or value.get("schema") != cls.SCHEMA):
            raise ValueError("unsupported Qwen generation Response")
        return cls(
            request_id=str(value["requestId"]),
            input_token_ids=tuple(int(item) for item in value["inputTokenIds"]),
            generated_token_ids=tuple(
                int(item) for item in value["generatedTokenIds"]),
            decoded_text=str(value["decodedText"]),
            stop_reason=str(value["stopReason"]),
            token_evidence=tuple(QwenTokenEvidence(
                request_id=str(item["requestId"]),
                token_index=int(item["tokenIndex"]),
                context_length=int(item["contextLength"]),
                token_id=int(item["tokenId"]),
            ) for item in value["tokenEvidence"]),
        )


def greedy_decode_fixture(logit_steps: Iterable[Sequence[float]],
                          max_new_tokens: int) -> list[int]:
    """Deterministic argmax oracle used by bounded correctness fixtures."""
    if max_new_tokens < 1 or max_new_tokens > MAX_OUTPUT_TOKENS:
        raise ValueError("max_new_tokens must be between 1 and 64")
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

    def generate_complete(
        self, request: QwenPilotRequest, *, request_id: str,
        eos_token_ids: set[int] | frozenset[int],
        decode: Callable[[Sequence[int]], str],
    ) -> QwenGenerationResponse:
        """Generate a complete answer inside one durable wire invocation."""
        request.validate()
        if not request_id or not callable(decode):
            raise ValueError("Qwen generation identity/decoder is missing")
        eos = {int(token) for token in eos_token_ids}
        context = list(request.input_token_ids)
        generated = []
        evidence = []
        stop_reason = "MAX_NEW_TOKENS"
        for index in range(request.max_new_tokens):
            logits = self._staged_logits(tuple(context))
            if not logits:
                raise QwenPilotTerminalError("EMPTY_LOGITS")
            token = max(range(len(logits)), key=lambda item: logits[item])
            evidence.append(QwenTokenEvidence(
                request_id=request_id,
                token_index=index,
                context_length=len(context),
                token_id=token,
            ))
            generated.append(token)
            context.append(token)
            if token in eos:
                stop_reason = "EOS"
                break
        decoded = str(decode(tuple(generated)))
        if not decoded:
            raise QwenPilotTerminalError("EMPTY_OUTPUT")
        return QwenGenerationResponse(
            request_id=request_id,
            input_token_ids=request.input_token_ids,
            generated_token_ids=tuple(generated),
            decoded_text=decoded,
            stop_reason=stop_reason,
            token_evidence=tuple(evidence),
        )


def publish_complete_generation(
    context: object, response: QwenGenerationResponse,
) -> None:
    """Publish the one complete Response through authenticated NDNSF Core."""
    if not isinstance(response, QwenGenerationResponse):
        raise TypeError("complete Qwen generation Response is required")
    context.publish_final_response(response.to_bytes())
