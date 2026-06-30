#!/usr/bin/env python3
"""Tests for llama-server provider semantic-cache integration."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_DIR = ROOT / "examples/python/NDNSF-DistributedInference/llama_server"
sys.path.insert(0, str(ROOT / "pythonWrapper"))
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))
sys.path.insert(0, str(EXAMPLE_DIR))

spec = importlib.util.spec_from_file_location(
    "llama_server_provider",
    EXAMPLE_DIR / "provider.py",
)
provider_mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules["llama_server_provider"] = provider_mod
spec.loader.exec_module(provider_mod)

from llama_server_lib import encode_chat_request  # noqa: E402

smoke_spec = importlib.util.spec_from_file_location(
    "llama_server_semantic_cache_smoke",
    EXAMPLE_DIR / "run_semantic_cache_provider_smoke.py",
)
smoke_mod = importlib.util.module_from_spec(smoke_spec)
assert smoke_spec.loader is not None
sys.modules["llama_server_semantic_cache_smoke"] = smoke_mod
smoke_spec.loader.exec_module(smoke_mod)

network_smoke_spec = importlib.util.spec_from_file_location(
    "llama_server_semantic_cache_network_smoke",
    EXAMPLE_DIR / "run_semantic_cache_network_smoke.py",
)
network_smoke_mod = importlib.util.module_from_spec(network_smoke_spec)
assert network_smoke_spec.loader is not None
sys.modules["llama_server_semantic_cache_network_smoke"] = network_smoke_mod
network_smoke_spec.loader.exec_module(network_smoke_mod)

selection_campaign_spec = importlib.util.spec_from_file_location(
    "llama_server_semantic_cache_selection_campaign",
    EXAMPLE_DIR / "run_semantic_cache_selection_campaign.py",
)
selection_campaign_mod = importlib.util.module_from_spec(selection_campaign_spec)
assert selection_campaign_spec.loader is not None
sys.modules["llama_server_semantic_cache_selection_campaign"] = selection_campaign_mod
selection_campaign_spec.loader.exec_module(selection_campaign_mod)


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
        self.role = "/LLM/LlamaServer"
        self.ndnsf = _FakeNdnsf()


class LlamaSemanticCacheProviderTests(unittest.TestCase):
    def test_cache_wrapper_hits_similar_openai_chat_payloads(self) -> None:
        cache = provider_mod.LlamaServerSemanticCache(enabled=True)
        first = encode_chat_request("Will it rain in Memphis tomorrow?", max_tokens=32)
        second = encode_chat_request("Give me tomorrow's Memphis weather forecast.", max_tokens=32)
        self.assertIsNone(cache.lookup(first)[0])
        self.assertTrue(cache.admit(first, b'{"cached": true}'))
        payload, meta = cache.lookup(second)
        self.assertEqual(payload, b'{"cached": true}')
        self.assertEqual(meta["status"], "hit")
        self.assertGreater(cache.token_saving_ratio(), 0.0)

    def test_handler_uses_cache_after_first_llama_server_response(self) -> None:
        calls: list[bytes] = []

        def fake_call(payload: bytes, *, base_url: str):
            calls.append(payload)
            return b'{"choices": [{"message": {"content": "cached answer"}}]}'

        original_call = provider_mod.call_llama_server_chat
        provider_mod.call_llama_server_chat = fake_call
        try:
            cache = provider_mod.LlamaServerSemanticCache(enabled=True)
            handler = provider_mod.make_llama_server_handler(
                _FakeState(),
                "http://127.0.0.1:8080",
                cache,
            )
            first = _FakeContext(encode_chat_request(
                "Explain NDNSF semantic cache.",
                max_tokens=64,
            ))
            second = _FakeContext(encode_chat_request(
                "How does NDNSF cache similar LLM answers?",
                max_tokens=64,
            ))
            handler(first)
            handler(second)
        finally:
            provider_mod.call_llama_server_chat = original_call

        self.assertEqual(len(calls), 1)
        self.assertEqual(len(first.ndnsf.responses), 1)
        self.assertEqual(len(second.ndnsf.responses), 1)
        self.assertFalse(first.ndnsf.failures)
        self.assertFalse(second.ndnsf.failures)
        self.assertEqual(cache.hits, 1)
        self.assertEqual(cache.misses, 1)

    def test_smoke_runner_reports_hits_and_backend_call_savings(self) -> None:
        prompts = [
            "Will it rain in Memphis tomorrow?",
            "Give me tomorrow's Memphis weather forecast.",
            "Explain NDNSF semantic cache.",
            "How does NDNSF cache similar LLM answers?",
            "Will it rain in Memphis tomorrow?",
        ]
        results, summary = smoke_mod.run_provider_semantic_cache_smoke(
            prompts,
            backend_delay_ms=0.0,
            max_tokens=32,
        )
        self.assertEqual(summary.count, 5)
        self.assertEqual(summary.backend_calls, 2)
        self.assertEqual(summary.hits, 3)
        self.assertEqual(summary.misses, 2)
        self.assertGreater(summary.hit_ratio, 0.5)
        self.assertGreater(summary.token_saving_ratio, 0.0)
        self.assertEqual(results[1].cache_status, "hit")
        self.assertEqual(results[3].cache_status, "hit")
        self.assertEqual(results[4].cache_status, "hit")

    def test_network_smoke_fake_openai_backend_records_calls(self) -> None:
        with network_smoke_mod.fake_openai_server(0.0) as (base_url, state):
            payload = encode_chat_request("Explain NDNSF semantic cache.", max_tokens=8)
            response = provider_mod.call_llama_server_chat(payload, base_url=base_url)
        decoded = response.decode("utf-8")
        self.assertIn("fake-openai-1", decoded)
        self.assertEqual(len(state.calls), 1)

    def test_selection_campaign_prefers_warmed_semantic_cache_provider(self) -> None:
        prompts = [
            "Will it rain in Memphis tomorrow?",
            "Give me tomorrow's Memphis weather forecast.",
            "Explain NDNSF semantic cache.",
            "How does NDNSF cache similar LLM answers?",
        ]
        results, summaries = selection_campaign_mod.run_selection_campaign(
            prompts,
            backend_delay_ms=0.0,
            cache_delay_ms=0.0,
            max_tokens=16,
        )
        by_policy = {item.policy: item for item in summaries}
        self.assertEqual(by_policy["first-provider"].backend_calls, 2)
        self.assertEqual(by_policy["semantic-cache-aware"].backend_calls, 0)
        self.assertEqual(by_policy["semantic-cache-aware"].hits, 4)
        selected = [
            item.selected_provider for item in results
            if item.policy == "semantic-cache-aware"
        ]
        self.assertEqual(selected, ["provider-b"] * 4)


if __name__ == "__main__":
    unittest.main()
