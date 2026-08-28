#!/usr/bin/env python3
"""Shared fail-closed contract for the frozen Spec 168 formal workload."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def validate_formal_campaign(value: dict[str, Any]) -> dict[str, Any]:
    if value.get("schemaVersion") != "ndnsf-di-qwen-generation-campaign-v1":
        raise ValueError("SPEC168_FORMAL_SCHEMA_REQUIRED")
    if len(value.get("prompts", [])) != 5:
        raise ValueError("SPEC168_FORMAL_FIVE_PROMPTS_REQUIRED")
    repetitions = value.get("repetitions", {})
    if (repetitions.get("warmupPerPrompt") != 1
            or repetitions.get("measuredPerPrompt") != 5
            or repetitions.get("sequential") is not True):
        raise ValueError("SPEC168_FORMAL_REPETITIONS_REQUIRED")
    generation = value.get("generation", {})
    if generation.get("maxNewTokens") != 64:
        raise ValueError("SPEC168_FORMAL_TOKEN_LIMIT_REQUIRED")
    if generation.get("strategy") != "greedy":
        raise ValueError("SPEC168_FORMAL_GREEDY_REQUIRED")
    if "formal" not in str(value.get("campaignId", "")):
        raise ValueError("SPEC168_FORMAL_CAMPAIGN_ID_REQUIRED")
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    try:
        value = json.loads(args.manifest.read_text(encoding="utf-8"))
        if not isinstance(value, dict):
            raise ValueError("SPEC168_FORMAL_OBJECT_REQUIRED")
        validate_formal_campaign(value)
    except Exception as error:  # noqa: BLE001
        print(f"{type(error).__name__}:{error}")
        return 1
    print(
        "SPEC168_FORMAL_CAMPAIGN_PASS "
        f"campaignId={value['campaignId']} prompts=5 requests=30"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
