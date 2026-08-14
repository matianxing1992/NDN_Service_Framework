#!/usr/bin/env python3
"""Fail-fast semantic preflight for Spec170 Tiger network jobs.

This checker is intentionally source/manifest based.  It prevents a real
allocation when a DATA_DRIVEN/V3 plan is paired with the legacy preplanned
driver, or when a requested V3 CPU diagnostic does not expose the V3 lifecycle.
It does not claim that a network run succeeded.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def _service_plans(plan: dict) -> list[dict]:
    services = plan.get("services")
    if not isinstance(services, list) or not services:
        raise ValueError("plan has no services")
    return [item for item in services if isinstance(item, dict)]


def validate(
    *, plan_path: Path | None, user_driver: Path, provider_driver: Path | None,
    require_v3: bool, cpu_mode: bool,
) -> list[str]:
    errors: list[str] = []
    user_text = user_driver.read_text(encoding="utf-8")
    provider_text = (
        provider_driver.read_text(encoding="utf-8")
        if provider_driver is not None else ""
    )
    plan_services: list[dict] = []
    if plan_path is not None:
        try:
            plan = json.loads(plan_path.read_text(encoding="utf-8"))
            plan_services = _service_plans(plan)
        except (OSError, json.JSONDecodeError, ValueError) as exc:
            errors.append(f"invalid execution plan: {exc}")

    policies = {
        str(item.get("executionPolicy", ""))
        for item in plan_services
        if item.get("executionPolicy")
    }
    if "DATA_DRIVEN_V2" in policies and "LEGACY_READY_SET_V1" in user_text:
        errors.append(
            "DATA_DRIVEN_V2 plan is paired with a driver that emits "
            "LEGACY_READY_SET_V1"
        )
    if require_v3:
        required_user_markers = (
            "begin_collaboration", "acks_closed", "commit_plan",
        )
        missing = [marker for marker in required_user_markers
                   if marker not in user_text]
        if missing:
            errors.append(
                "V3 user driver is missing lifecycle markers: "
                + ", ".join(missing)
            )
        if "request_collaboration(" in user_text:
            errors.append(
                "V3 user driver still calls compatibility request_collaboration"
            )
        if provider_driver is None:
            errors.append("V3 preflight requires a Provider driver")
        else:
            provider_markers = ("DIProviderOfferIssuerV3", "DI_PLACEMENT_V3")
            missing_provider = [marker for marker in provider_markers
                                if marker not in provider_text]
            if missing_provider:
                errors.append(
                    "V3 Provider driver is missing markers: "
                    + ", ".join(missing_provider)
                )
    if cpu_mode:
        combined = user_text + "\n" + provider_text
        if "cpu" not in combined.lower():
            errors.append("CPU mode requested but no CPU execution marker is present")
        if "require_cuda" in combined and "--require-cuda" in combined:
            errors.append(
                "CPU mode driver contains an explicit CUDA-required path; "
                "split GPU and CPU jobs"
            )
    if errors:
        return errors
    return [
        "planPolicy=" + (",".join(sorted(policies)) or "none"),
        "userLifecycle=V3" if require_v3 else "userLifecycle=unspecified",
        "resourceMode=CPU" if cpu_mode else "resourceMode=unspecified",
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--user-driver", type=Path, required=True)
    parser.add_argument("--provider-driver", type=Path)
    parser.add_argument("--require-v3", action="store_true")
    parser.add_argument("--cpu", action="store_true")
    args = parser.parse_args(argv)
    try:
        details = validate(
            plan_path=args.plan,
            user_driver=args.user_driver,
            provider_driver=args.provider_driver,
            require_v3=args.require_v3,
            cpu_mode=args.cpu,
        )
    except OSError as exc:
        print(f"SPEC170_TIGER_CONTRACT_PREFLIGHT_FAIL error={exc}", file=sys.stderr)
        return 2
    if details and details[0].startswith("planPolicy="):
        print("SPEC170_TIGER_CONTRACT_PREFLIGHT_PASS " + " ".join(details))
        return 0
    print("SPEC170_TIGER_CONTRACT_PREFLIGHT_FAIL", file=sys.stderr)
    for item in details:
        print("  " + item, file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
