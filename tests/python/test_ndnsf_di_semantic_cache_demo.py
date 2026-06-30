#!/usr/bin/env python3
"""Tests for the minimal NDNSF-DI LLM semantic cache demo."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


DEMO_PATH = (
    Path(__file__).resolve().parents[2] /
    "examples/python/NDNSF-DistributedInference/llm_semantic_cache_demo.py"
)
spec = importlib.util.spec_from_file_location("llm_semantic_cache_demo", DEMO_PATH)
demo = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules["llm_semantic_cache_demo"] = demo
spec.loader.exec_module(demo)


class SemanticCacheDemoTests(unittest.TestCase):
    def test_repeated_and_similar_prompts_hit_provider_local_cache(self) -> None:
        prompts = [
            "Will it rain in Memphis tomorrow?",
            "Give me tomorrow's Memphis weather forecast.",
            "Explain NDNSF semantic cache.",
            "How does NDNSF cache similar LLM answers?",
            "Will it rain in Memphis tomorrow?",
        ]
        results, summary = demo.run_semantic_cache_demo(
            prompts,
            compute_delay_ms=0.0,
            cache_delay_ms=0.0,
        )
        self.assertEqual(summary.count, 5)
        self.assertEqual(summary.hits, 3)
        self.assertEqual(summary.misses, 2)
        self.assertGreater(summary.hit_ratio, 0.5)
        self.assertGreater(summary.saved_tokens, 0)
        self.assertGreater(summary.token_saving_ratio, 0.0)
        self.assertEqual(results[1].cache_status, "hit")
        self.assertEqual(results[3].cache_status, "hit")
        self.assertEqual(results[4].cache_status, "hit")

    def test_demo_writes_csv_without_response_payloads(self) -> None:
        prompts = [
            "Explain distributed inference with providers.",
            "Tell me how distributed inference works.",
        ]
        results, _ = demo.run_semantic_cache_demo(
            prompts,
            compute_delay_ms=0.0,
            cache_delay_ms=0.0,
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "metrics.csv"
            demo.write_metrics_csv(path, results)
            text = path.read_text(encoding="utf-8")
        self.assertIn("semantic_pattern_id", text)
        self.assertIn("cache_status", text)
        self.assertNotIn("response", text.splitlines()[0])


if __name__ == "__main__":
    unittest.main()
