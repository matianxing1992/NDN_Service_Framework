#!/usr/bin/env python3
"""Runtime v1 cache-pressure smoke for long-context planning."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from ndnsf_distributed_inference.runtime_v1 import (
    ContextObjectKind,
    LongContextManager,
    prefix_state,
    write_json,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    manager = LongContextManager(budget_mb=0.001)
    manager.put(prefix_state(
        object_id="prefix-old",
        prefix_id="prefix-old",
        model_id="qwen",
        tokenizer_id="qwen-tokenizer",
        provider="llm-4gb",
        token_count=128,
        byte_count=2048,
    ))
    manager.put(prefix_state(
        object_id="prefix-new",
        prefix_id="prefix-new",
        model_id="qwen",
        tokenizer_id="qwen-tokenizer",
        provider="llm-8gb",
        token_count=128,
        byte_count=2048,
    ))
    manager.get(ContextObjectKind.PREFIX_STATE, prefix_id="prefix-new")
    manager.get(ContextObjectKind.PREFIX_STATE, prefix_id="prefix-missing")

    payload = {
        "status": "ok",
        "telemetry": manager.telemetry(),
        "events": manager.events(),
        "interpretation": (
            "Cache pressure can evict prefix state; a missing prefix should "
            "force the planner/runtime to use prefill or replan instead of "
            "assuming a provider-local KV hit."
        ),
    }
    write_json(args.out_dir / "cache-pressure-smoke.json", payload)
    print(json.dumps({
        "status": "ok",
        "outDir": str(args.out_dir),
        "events": len(manager.events()),
        "evictions": manager.telemetry().evictions,
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
