#!/usr/bin/env python3
"""Compare baseline and semantic-cache-aware provider selection.

This campaign keeps the network out of the loop and focuses on the decision
surface exposed through coarse ACK metadata.  It models two llama-server
providers: one cold provider and one warmed provider with resident semantic
responses for the request patterns.
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
from ndnsf_distributed_inference import (
    choose_semantic_cache_provider,
    semantic_cache_ack_fields,
)


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
class SelectionCampaignResult:
    policy: str
    index: int
    prompt: str
    selected_provider: str
    semantic_pattern_id: str
    cache_status: str
    latency_ms: float
    backend_calls: int


@dataclass(frozen=True)
class SelectionCampaignSummary:
    policy: str
    requests: int
    hits: int
    misses: int
    backend_calls: int
    hit_ratio: float
    avg_latency_ms: float
    p50_latency_ms: float
    p95_latency_ms: float


class SimulatedLlamaProvider:
    def __init__(self, name: str, *, backend_delay_ms: float, cache_delay_ms: float):
        self.name = name
        self.backend_delay_ms = float(backend_delay_ms)
        self.cache_delay_ms = float(cache_delay_ms)
        self.cache = provider_mod.LlamaServerSemanticCache(enabled=True, budget_mb=8)
        self.backend_calls = 0

    def warm(self, prompt: str, *, max_tokens: int) -> None:
        payload = encode_chat_request(prompt, max_tokens=max_tokens)
        self.cache.admit(payload, self._response(payload, cached=True))

    def ack_fields(self, payload: bytes) -> dict:
        key, confidence, _, _ = self.cache.key_for_payload(payload)
        hint = self.cache.cache.hint_for(key, confidence=confidence)
        return semantic_cache_ack_fields(hint)

    def handle(self, payload: bytes) -> tuple[str, bytes]:
        cached, _ = self.cache.lookup(payload)
        if cached is not None:
            if self.cache_delay_ms > 0:
                time.sleep(self.cache_delay_ms / 1000.0)
            return "hit", cached
        self.backend_calls += 1
        if self.backend_delay_ms > 0:
            time.sleep(self.backend_delay_ms / 1000.0)
        response = self._response(payload, cached=False)
        self.cache.admit(payload, response)
        return "miss", response

    def _response(self, payload: bytes, *, cached: bool) -> bytes:
        prompt, model, _ = provider_mod._prompt_from_openai_payload(payload)
        pattern_id, _ = provider_mod.semantic_pattern_for_prompt(prompt)
        return json.dumps({
            "provider": self.name,
            "model": model,
            "semantic_pattern_id": pattern_id,
            "cached_seed": cached,
            "content": f"{self.name} response for {pattern_id}",
        }, sort_keys=True).encode("utf-8")


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


def _make_providers(
    prompts: list[str],
    *,
    warm_provider: str,
    backend_delay_ms: float,
    cache_delay_ms: float,
    max_tokens: int,
) -> dict[str, SimulatedLlamaProvider]:
    providers = {
        "provider-a": SimulatedLlamaProvider(
            "provider-a",
            backend_delay_ms=backend_delay_ms,
            cache_delay_ms=cache_delay_ms,
        ),
        "provider-b": SimulatedLlamaProvider(
            "provider-b",
            backend_delay_ms=backend_delay_ms,
            cache_delay_ms=cache_delay_ms,
        ),
    }
    warmed = providers[warm_provider]
    seen_patterns: set[str] = set()
    for prompt in prompts:
        payload = encode_chat_request(prompt, max_tokens=max_tokens)
        key, _, _, _ = warmed.cache.key_for_payload(payload)
        if key.semantic_pattern_id not in seen_patterns:
            warmed.warm(prompt, max_tokens=max_tokens)
            seen_patterns.add(key.semantic_pattern_id)
    return providers


def _select_provider(policy: str, providers: dict[str, SimulatedLlamaProvider],
                     payload: bytes) -> str:
    if policy == "first-provider":
        return sorted(providers)[0]
    if policy == "semantic-cache-aware":
        return choose_semantic_cache_provider({
            name: provider.ack_fields(payload)
            for name, provider in providers.items()
        })
    raise ValueError(f"unknown policy: {policy}")


def run_selection_campaign(
    prompts: list[str],
    *,
    backend_delay_ms: float = 5.0,
    cache_delay_ms: float = 0.25,
    max_tokens: int = 64,
    warm_provider: str = "provider-b",
    policies: tuple[str, ...] = ("first-provider", "semantic-cache-aware"),
) -> tuple[list[SelectionCampaignResult], list[SelectionCampaignSummary]]:
    all_results: list[SelectionCampaignResult] = []
    summaries: list[SelectionCampaignSummary] = []
    for policy in policies:
        providers = _make_providers(
            prompts,
            warm_provider=warm_provider,
            backend_delay_ms=backend_delay_ms,
            cache_delay_ms=cache_delay_ms,
            max_tokens=max_tokens,
        )
        policy_results: list[SelectionCampaignResult] = []
        for index, prompt in enumerate(prompts):
            payload = encode_chat_request(prompt, max_tokens=max_tokens)
            selected = _select_provider(policy, providers, payload)
            provider = providers[selected]
            key, _, _, _ = provider.cache.key_for_payload(payload)
            start = time.perf_counter()
            status, _ = provider.handle(payload)
            latency_ms = (time.perf_counter() - start) * 1000.0
            policy_results.append(SelectionCampaignResult(
                policy=policy,
                index=index,
                prompt=prompt,
                selected_provider=selected,
                semantic_pattern_id=key.semantic_pattern_id,
                cache_status=status,
                latency_ms=latency_ms,
                backend_calls=sum(item.backend_calls for item in providers.values()),
            ))
        all_results.extend(policy_results)
        latencies = [item.latency_ms for item in policy_results]
        hits = sum(1 for item in policy_results if item.cache_status == "hit")
        misses = len(policy_results) - hits
        summaries.append(SelectionCampaignSummary(
            policy=policy,
            requests=len(policy_results),
            hits=hits,
            misses=misses,
            backend_calls=sum(item.backend_calls for item in providers.values()),
            hit_ratio=hits / max(1, len(policy_results)),
            avg_latency_ms=sum(latencies) / max(1, len(latencies)),
            p50_latency_ms=percentile(latencies, 0.50),
            p95_latency_ms=percentile(latencies, 0.95),
        ))
    return all_results, summaries


def write_results_csv(path: str | Path, results: list[SelectionCampaignResult]) -> None:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(asdict(results[0]).keys()))
        writer.writeheader()
        for item in results:
            writer.writerow(asdict(item))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prompts", default="|".join(DEFAULT_PROMPTS))
    parser.add_argument("--backend-delay-ms", type=float, default=5.0)
    parser.add_argument("--cache-delay-ms", type=float, default=0.25)
    parser.add_argument("--max-tokens", type=int, default=64)
    parser.add_argument("--warm-provider", default="provider-b")
    parser.add_argument("--metrics-csv", default="")
    parser.add_argument("--summary-json", default="")
    args = parser.parse_args()

    prompts = [item.strip() for item in args.prompts.split("|") if item.strip()]
    results, summaries = run_selection_campaign(
        prompts,
        backend_delay_ms=args.backend_delay_ms,
        cache_delay_ms=args.cache_delay_ms,
        max_tokens=args.max_tokens,
        warm_provider=args.warm_provider,
    )
    if args.metrics_csv:
        write_results_csv(args.metrics_csv, results)
    if args.summary_json:
        target = Path(args.summary_json)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(
            json.dumps([asdict(item) for item in summaries], indent=2) + "\n",
            encoding="utf-8",
        )
    for item in summaries:
        print(
            "LLAMA_SERVER_SEMANTIC_CACHE_SELECTION_CAMPAIGN_SUMMARY",
            f"policy={item.policy}",
            f"requests={item.requests}",
            f"hits={item.hits}",
            f"misses={item.misses}",
            f"backend_calls={item.backend_calls}",
            f"hit_ratio={item.hit_ratio:.3f}",
            f"avg_latency_ms={item.avg_latency_ms:.3f}",
            f"p50_latency_ms={item.p50_latency_ms:.3f}",
            f"p95_latency_ms={item.p95_latency_ms:.3f}",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
