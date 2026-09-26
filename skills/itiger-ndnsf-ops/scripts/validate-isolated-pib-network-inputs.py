#!/usr/bin/env python3
"""Fail-closed preflight for isolated PIB NDNSF network jobs."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def read(path: Path) -> str:
    if not path.is_file():
        raise ValueError(f"missing input: {path}")
    return path.read_text(encoding="utf-8")


def require(text: str, pattern: str, label: str, failures: list[str]) -> None:
    if not re.search(pattern, text, flags=re.MULTILINE):
        failures.append(label)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--job", type=Path, required=True)
    parser.add_argument("--user-driver", type=Path, required=True)
    parser.add_argument("--service", default="/Inference/NativeTracer")
    parser.add_argument("--user-identity", default="/NDNSF-DI/Tracer/user")
    parser.add_argument("--provider-identity-prefix", default="/NDNSF-DI/Tracer/provider/")
    parser.add_argument("--json", type=Path, metavar="PATH")
    args = parser.parse_args()

    failures: list[str] = []
    policy = read(args.policy)
    job = read(args.job)
    user_driver = read(args.user_driver)

    require(policy, re.escape(args.service), "policy service authorization", failures)
    require(policy, re.escape(args.user_identity), "policy user identity authorization", failures)
    require(policy, re.escape(args.provider_identity_prefix), "policy provider identity authorization", failures)

    isolated = bool(re.search(r"--home\s+['\"]?\$SCRATCH/home-", job))
    if isolated:
        require(job, r"--bootstrap-token-file\s+\S+", "controller bootstrap token file", failures)
        require(job, r"--bootstrap-token(?:=|\s+)\S+", "remote bootstrap token argument", failures)
        require(job, r"NDNSF_CONTROLLER_CERT_FILE\s*=", "controller certificate environment", failures)
        require(job, r"ndnsec\s+cert-dump\s+-i\s+\S+", "controller certificate export", failures)
        require(user_driver, r"--bootstrap-token", "user-driver bootstrap-token option", failures)
        require(user_driver, r"bootstrap_token\s*=", "user-driver bootstrap token binding", failures)

    checks = {
        "policy": str(args.policy),
        "job": str(args.job),
        "user_driver": str(args.user_driver),
        "isolated_pib_detected": isolated,
        "service": args.service,
        "user_identity": args.user_identity,
        "provider_identity_prefix": args.provider_identity_prefix,
        "failures": failures,
    }
    if args.json:
        args.json.write_text(json.dumps(checks, indent=2) + "\n", encoding="utf-8")

    if failures:
        print("ISOLATED_PIB_NETWORK_PREFLIGHT=FAIL")
        for failure in failures:
            print(f"FAIL={failure}")
        return 1
    print("ISOLATED_PIB_NETWORK_PREFLIGHT=PASS")
    print(f"ISOLATED_PIB_DETECTED={str(isolated).lower()}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as exc:
        print(f"ISOLATED_PIB_NETWORK_PREFLIGHT=FAIL\nFAIL={exc}")
        raise SystemExit(1)
