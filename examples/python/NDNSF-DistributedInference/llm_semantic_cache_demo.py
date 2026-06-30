#!/usr/bin/env python3
"""Minimal provider-local semantic cache demo for NDNSF-DI LLM services.

This demo intentionally keeps embedding and clustering outside the framework.
The application maps prompts to semantic pattern IDs and confidence values, then
uses NDNSF-DI Runtime v1 semantic-cache helpers to decide whether a provider can
return a cached service response.
"""

from __future__ import annotations

import argparse
import csv
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path

from ndnsf_distributed_inference import (
    SemanticCacheDisposition,
    SemanticPatternMeta,
    SemanticServiceCacheEntry,
    SemanticServiceCacheKey,
    SemanticServiceCacheManager,
    semantic_cache_token_saving_ratio,
)


SERVICE = "/LLM/Qwen/Chat"
MODEL_ID = "qwen-small-demo"
TOKENIZER_ID = "qwen-tokenizer-demo"
POLICY_EPOCH = "/Policy/llm-chat/v1"
RESPONSE_SCHEMA = "chat-completion-demo-v1"


DEFAULT_PROMPTS = [
    "Will it rain in Memphis tomorrow?",
    "Give me tomorrow's Memphis weather forecast.",
    "Explain NDNSF semantic cache.",
    "How does NDNSF cache similar LLM answers?",
    "Will it rain in Memphis tomorrow?",
    "Tell me how distributed inference works.",
    "Explain distributed inference with providers.",
    "Give me tomorrow's Memphis weather forecast.",
]


@dataclass(frozen=True)
class DemoRequestResult:
    index: int
    prompt: str
    semantic_pattern_id: str
    confidence: float
    cache_status: str
    latency_ms: float
    prompt_tokens: int
    output_tokens: int
    saved_tokens: int
    response: str


@dataclass(frozen=True)
class DemoSummary:
    count: int
    hits: int
    misses: int
    candidates: int
    hit_ratio: float
    saved_tokens: int
    total_tokens: int
    token_saving_ratio: float
    avg_latency_ms: float
    p50_latency_ms: float
    p95_latency_ms: float


def semantic_pattern_for_prompt(prompt: str) -> tuple[str, float]:
    text = prompt.lower()
    if any(word in text for word in ("weather", "rain", "forecast")):
        return "weather-forecast", 0.94
    if "cache" in text or "semantic" in text:
        return "semantic-cache", 0.93
    if "distributed inference" in text or "provider" in text:
        return "distributed-inference", 0.92
    return "general-chat", 0.80


def estimate_prompt_tokens(prompt: str) -> int:
    return max(1, len([part for part in prompt.replace("?", " ").split() if part]))


def estimated_output_tokens(pattern_id: str) -> int:
    return {
        "weather-forecast": 48,
        "semantic-cache": 96,
        "distributed-inference": 112,
        "general-chat": 64,
    }.get(pattern_id, 64)


def response_for_pattern(pattern_id: str) -> str:
    return {
        "weather-forecast": (
            "A cached weather-style answer can be reused for closely related "
            "forecast questions after policy and confidence checks."
        ),
        "semantic-cache": (
            "A semantic service cache stores final LLM responses for similar "
            "service requests, separate from exact KV or forward-state caches."
        ),
        "distributed-inference": (
            "NDNSF-DI assigns model work to providers and exchanges named "
            "dependency objects while preserving service-level authorization."
        ),
        "general-chat": "A generic cached answer is available for this demo pattern.",
    }.get(pattern_id, "A cached answer is available.")


def build_pattern_meta(prompts: list[str]) -> list[SemanticPatternMeta]:
    grouped: dict[str, dict[str, int]] = {}
    for prompt in prompts:
        pattern_id, _ = semantic_pattern_for_prompt(prompt)
        item = grouped.setdefault(pattern_id, {
            "query_count": 0,
            "prompt_tokens": 0,
            "output_tokens": 0,
        })
        item["query_count"] += 1
        item["prompt_tokens"] += estimate_prompt_tokens(prompt)
        item["output_tokens"] += estimated_output_tokens(pattern_id)
    patterns: list[SemanticPatternMeta] = []
    for pattern_id, item in grouped.items():
        saved = max(0, item["query_count"] - 1) * estimated_output_tokens(pattern_id)
        total = item["prompt_tokens"] + item["output_tokens"]
        patterns.append(SemanticPatternMeta(
            pattern_id=pattern_id,
            conversation_round=1,
            query_count=item["query_count"],
            total_prompt_tokens=item["prompt_tokens"],
            total_output_tokens=item["output_tokens"],
            estimated_saved_tokens=saved,
            proportion_ratio=item["query_count"] / max(1, len(prompts)),
            token_saving_ratio=semantic_cache_token_saving_ratio(
                saved_tokens=saved,
                total_tokens=total,
            ),
        ))
    return patterns


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


class SemanticCacheDemoProvider:
    def __init__(self, *, compute_delay_ms: float, cache_delay_ms: float):
        self.compute_delay_ms = float(compute_delay_ms)
        self.cache_delay_ms = float(cache_delay_ms)
        self.cache = SemanticServiceCacheManager(budget_mb=1.0, min_admission_score=0.0)

    def register_workload_patterns(self, prompts: list[str]) -> None:
        self.cache.register_patterns(build_pattern_meta(prompts))

    def handle_prompt(self, prompt: str, index: int) -> DemoRequestResult:
        pattern_id, confidence = semantic_pattern_for_prompt(prompt)
        key = SemanticServiceCacheKey(
            service_name=SERVICE,
            model_id=MODEL_ID,
            tokenizer_id=TOKENIZER_ID,
            policy_epoch=POLICY_EPOCH,
            semantic_pattern_id=pattern_id,
            response_schema=RESPONSE_SCHEMA,
            app_namespace="llm-semantic-cache-demo",
        )
        output_tokens = estimated_output_tokens(pattern_id)
        prompt_tokens = estimate_prompt_tokens(prompt)
        hint = self.cache.hint_for(key, confidence=confidence)
        start = time.perf_counter()
        entry = self.cache.get(key, confidence=confidence)
        if entry is not None:
            time.sleep(self.cache_delay_ms / 1000.0)
            latency_ms = (time.perf_counter() - start) * 1000.0
            return DemoRequestResult(
                index=index,
                prompt=prompt,
                semantic_pattern_id=pattern_id,
                confidence=confidence,
                cache_status=SemanticCacheDisposition.HIT.value,
                latency_ms=latency_ms,
                prompt_tokens=prompt_tokens,
                output_tokens=output_tokens,
                saved_tokens=output_tokens,
                response=entry.response_payload.decode("utf-8"),
            )
        time.sleep(self.compute_delay_ms / 1000.0)
        response = response_for_pattern(pattern_id)
        payload = response.encode("utf-8")
        cache_entry = self.cache.entry_from_pattern(
            key=key,
            response_payload=payload,
            provider="/provider/semantic-cache-demo",
            confidence_threshold=0.88,
            estimated_prompt_tokens=prompt_tokens,
            estimated_output_tokens=output_tokens,
            byte_count=len(payload),
        )
        self.cache.put(cache_entry)
        latency_ms = (time.perf_counter() - start) * 1000.0
        status = (
            SemanticCacheDisposition.CANDIDATE.value
            if hint.disposition == SemanticCacheDisposition.CANDIDATE else
            SemanticCacheDisposition.MISS.value
        )
        return DemoRequestResult(
            index=index,
            prompt=prompt,
            semantic_pattern_id=pattern_id,
            confidence=confidence,
            cache_status=status,
            latency_ms=latency_ms,
            prompt_tokens=prompt_tokens,
            output_tokens=output_tokens,
            saved_tokens=0,
            response=response,
        )


def summarize_results(results: list[DemoRequestResult]) -> DemoSummary:
    latencies = [item.latency_ms for item in results]
    hits = sum(1 for item in results if item.cache_status == SemanticCacheDisposition.HIT.value)
    misses = sum(1 for item in results if item.cache_status == SemanticCacheDisposition.MISS.value)
    candidates = sum(
        1 for item in results
        if item.cache_status == SemanticCacheDisposition.CANDIDATE.value
    )
    saved_tokens = sum(item.saved_tokens for item in results)
    total_tokens = sum(item.prompt_tokens + item.output_tokens for item in results)
    return DemoSummary(
        count=len(results),
        hits=hits,
        misses=misses,
        candidates=candidates,
        hit_ratio=hits / max(1, len(results)),
        saved_tokens=saved_tokens,
        total_tokens=total_tokens,
        token_saving_ratio=semantic_cache_token_saving_ratio(
            saved_tokens=saved_tokens,
            total_tokens=total_tokens,
        ),
        avg_latency_ms=sum(latencies) / max(1, len(latencies)),
        p50_latency_ms=percentile(latencies, 0.50),
        p95_latency_ms=percentile(latencies, 0.95),
    )


def run_semantic_cache_demo(
    prompts: list[str],
    *,
    compute_delay_ms: float = 20.0,
    cache_delay_ms: float = 1.0,
) -> tuple[list[DemoRequestResult], DemoSummary]:
    provider = SemanticCacheDemoProvider(
        compute_delay_ms=compute_delay_ms,
        cache_delay_ms=cache_delay_ms,
    )
    provider.register_workload_patterns(prompts)
    results = [
        provider.handle_prompt(prompt, index)
        for index, prompt in enumerate(prompts)
    ]
    return results, summarize_results(results)


def write_metrics_csv(path: str | Path, results: list[DemoRequestResult]) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=[
            "index",
            "prompt",
            "semantic_pattern_id",
            "confidence",
            "cache_status",
            "latency_ms",
            "prompt_tokens",
            "output_tokens",
            "saved_tokens",
        ])
        writer.writeheader()
        for item in results:
            row = asdict(item)
            row.pop("response")
            writer.writerow(row)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--prompts",
        default="|".join(DEFAULT_PROMPTS),
        help="Pipe-separated prompt list.",
    )
    parser.add_argument("--compute-delay-ms", type=float, default=20.0)
    parser.add_argument("--cache-delay-ms", type=float, default=1.0)
    parser.add_argument("--metrics-csv", default="")
    parser.add_argument("--summary-json", default="")
    args = parser.parse_args()

    prompts = [item.strip() for item in args.prompts.split("|") if item.strip()]
    results, summary = run_semantic_cache_demo(
        prompts,
        compute_delay_ms=args.compute_delay_ms,
        cache_delay_ms=args.cache_delay_ms,
    )
    if args.metrics_csv:
        write_metrics_csv(args.metrics_csv, results)
    if args.summary_json:
        target = Path(args.summary_json)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(asdict(summary), indent=2) + "\n", encoding="utf-8")
    for item in results:
        print(
            "LLM_SEMANTIC_CACHE_DEMO_RESULT",
            f"index={item.index}",
            f"pattern={item.semantic_pattern_id}",
            f"status={item.cache_status}",
            f"latency_ms={item.latency_ms:.3f}",
            f"saved_tokens={item.saved_tokens}",
            flush=True,
        )
    print(
        "LLM_SEMANTIC_CACHE_DEMO_SUMMARY",
        f"count={summary.count}",
        f"hits={summary.hits}",
        f"misses={summary.misses}",
        f"hit_ratio={summary.hit_ratio:.3f}",
        f"saved_tokens={summary.saved_tokens}",
        f"total_tokens={summary.total_tokens}",
        f"token_saving_ratio={summary.token_saving_ratio:.3f}",
        f"avg_latency_ms={summary.avg_latency_ms:.3f}",
        f"p50_latency_ms={summary.p50_latency_ms:.3f}",
        f"p95_latency_ms={summary.p95_latency_ms:.3f}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
