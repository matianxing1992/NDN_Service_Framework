#!/usr/bin/env python3
"""Run a reproducible llama-server provider semantic-cache smoke experiment.

The script exercises the real ``make_llama_server_handler`` path with a fake
llama-server backend.  It proves that similar OpenAI chat requests can hit the
provider-local semantic cache and avoid a second backend call.
"""

from __future__ import annotations

import argparse
import csv
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import provider as provider_mod
from llama_server_lib import encode_chat_request


DEFAULT_PROMPTS = [
    "Will it rain in Memphis tomorrow?",
    "Give me tomorrow's Memphis weather forecast.",
    "Explain NDNSF semantic cache.",
    "How does NDNSF cache similar LLM answers?",
    "Tell me how distributed inference works.",
    "Explain distributed inference with providers.",
    "Will it rain in Memphis tomorrow?",
    "How does NDNSF cache similar LLM answers?",
]


@dataclass(frozen=True)
class SmokeRequestResult:
    index: int
    prompt: str
    semantic_pattern_id: str
    cache_status: str
    provider_backend_calls: int
    latency_ms: float
    response_bytes: int


@dataclass(frozen=True)
class SmokeSummary:
    count: int
    hits: int
    misses: int
    backend_calls: int
    hit_ratio: float
    token_saving_ratio: float
    avg_latency_ms: float
    p50_latency_ms: float
    p95_latency_ms: float


class _FakeState:
    def require_ready(self) -> None:
        return None


class _FakeNdnsf:
    def __init__(self) -> None:
        self.responses: list[bytes] = []
        self.failures: list[str] = []

    def publish_final_response(self, payload: bytes) -> None:
        self.responses.append(payload)

    def fail(self, reason: str) -> None:
        self.failures.append(reason)


class _FakeContext:
    def __init__(self, payload: bytes) -> None:
        self.request = payload
        self.role = provider_mod.ROLE
        self.ndnsf = _FakeNdnsf()


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    ordered = sorted(values)
    index = (len(ordered) - 1) * pct
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    if lower == upper:
        return ordered[lower]
    weight = index - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _fake_llama_response(payload: bytes, call_index: int) -> bytes:
    prompt, model, _ = provider_mod._prompt_from_openai_payload(payload)
    pattern_id, _ = provider_mod.semantic_pattern_for_prompt(prompt)
    response = {
        "id": f"fake-llama-server-{call_index}",
        "object": "chat.completion",
        "model": model,
        "choices": [{
            "index": 0,
            "message": {
                "role": "assistant",
                "content": f"fresh backend answer for {pattern_id}",
            },
            "finish_reason": "stop",
        }],
    }
    return json.dumps(response, sort_keys=True).encode("utf-8")


def run_provider_semantic_cache_smoke(
    prompts: list[str],
    *,
    backend_delay_ms: float = 5.0,
    max_tokens: int = 64,
) -> tuple[list[SmokeRequestResult], SmokeSummary]:
    calls: list[bytes] = []

    def fake_call(payload: bytes, *, base_url: str):
        calls.append(payload)
        time.sleep(max(0.0, backend_delay_ms) / 1000.0)
        return _fake_llama_response(payload, len(calls))

    original_call = provider_mod.call_llama_server_chat
    provider_mod.call_llama_server_chat = fake_call
    cache = provider_mod.LlamaServerSemanticCache(enabled=True)
    handler = provider_mod.make_llama_server_handler(
        _FakeState(),
        "http://fake-llama-server.local:8080",
        cache,
    )
    results: list[SmokeRequestResult] = []
    try:
        for index, prompt in enumerate(prompts):
            payload = encode_chat_request(prompt, max_tokens=max_tokens)
            key, _, _, _ = cache.key_for_payload(payload)
            calls_before = len(calls)
            ctx = _FakeContext(payload)
            start = time.perf_counter()
            handler(ctx)
            latency_ms = (time.perf_counter() - start) * 1000.0
            if ctx.ndnsf.failures:
                raise RuntimeError("; ".join(ctx.ndnsf.failures))
            response_bytes = len(ctx.ndnsf.responses[-1]) if ctx.ndnsf.responses else 0
            status = "hit" if len(calls) == calls_before else "miss"
            results.append(SmokeRequestResult(
                index=index,
                prompt=prompt,
                semantic_pattern_id=key.semantic_pattern_id,
                cache_status=status,
                provider_backend_calls=len(calls),
                latency_ms=latency_ms,
                response_bytes=response_bytes,
            ))
    finally:
        provider_mod.call_llama_server_chat = original_call
    latencies = [item.latency_ms for item in results]
    hits = sum(1 for item in results if item.cache_status == "hit")
    misses = sum(1 for item in results if item.cache_status == "miss")
    summary = SmokeSummary(
        count=len(results),
        hits=hits,
        misses=misses,
        backend_calls=len(calls),
        hit_ratio=hits / max(1, len(results)),
        token_saving_ratio=cache.token_saving_ratio(),
        avg_latency_ms=sum(latencies) / max(1, len(latencies)),
        p50_latency_ms=percentile(latencies, 0.50),
        p95_latency_ms=percentile(latencies, 0.95),
    )
    return results, summary


def write_metrics_csv(path: str | Path, results: list[SmokeRequestResult]) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(asdict(results[0]).keys()))
        writer.writeheader()
        for item in results:
            writer.writerow(asdict(item))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--prompts",
        default="|".join(DEFAULT_PROMPTS),
        help="Pipe-separated prompt list.",
    )
    parser.add_argument("--backend-delay-ms", type=float, default=5.0)
    parser.add_argument("--max-tokens", type=int, default=64)
    parser.add_argument("--metrics-csv", default="")
    parser.add_argument("--summary-json", default="")
    args = parser.parse_args()

    prompts = [item.strip() for item in args.prompts.split("|") if item.strip()]
    results, summary = run_provider_semantic_cache_smoke(
        prompts,
        backend_delay_ms=args.backend_delay_ms,
        max_tokens=args.max_tokens,
    )
    if args.metrics_csv:
        write_metrics_csv(args.metrics_csv, results)
    if args.summary_json:
        target = Path(args.summary_json)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(asdict(summary), indent=2) + "\n", encoding="utf-8")
    for item in results:
        print(
            "LLAMA_SERVER_SEMANTIC_CACHE_PROVIDER_SMOKE_RESULT",
            f"index={item.index}",
            f"pattern={item.semantic_pattern_id}",
            f"status={item.cache_status}",
            f"backend_calls={item.provider_backend_calls}",
            f"latency_ms={item.latency_ms:.3f}",
            flush=True,
        )
    print(
        "LLAMA_SERVER_SEMANTIC_CACHE_PROVIDER_SMOKE_SUMMARY",
        f"count={summary.count}",
        f"hits={summary.hits}",
        f"misses={summary.misses}",
        f"backend_calls={summary.backend_calls}",
        f"hit_ratio={summary.hit_ratio:.3f}",
        f"token_saving_ratio={summary.token_saving_ratio:.3f}",
        f"avg_latency_ms={summary.avg_latency_ms:.3f}",
        f"p50_latency_ms={summary.p50_latency_ms:.3f}",
        f"p95_latency_ms={summary.p95_latency_ms:.3f}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
