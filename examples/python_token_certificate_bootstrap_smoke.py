#!/usr/bin/env python3
"""Run one role for the direct Python API token-certificate bootstrap smoke."""

from __future__ import annotations

import argparse
import sys

from ndnsf import ServiceController, ServiceProvider, ServiceUser


def _run_user(label: str) -> int:
    user = ServiceUser(
        bootstrap_token="user-token-045",
        permission_wait_ms=2500,
    )
    response = user.request_service(
        "/HELLO",
        b"HELLO",
        ack_timeout_ms=600,
        timeout_ms=9000,
    )
    if not response.status:
        raise RuntimeError(f"{label} request failed: {response.error}")
    if response.payload != b"HELLO":
        raise RuntimeError(f"{label} response payload mismatch: {response.payload!r}")
    print(f"{label}=OK", flush=True)
    return 0


def _run_controller() -> int:
    ServiceController(
        policy_file="examples/hello.policies",
        bootstrap_token_file="examples/hello.bootstrap-tokens",
    ).run()
    return 0


def _run_provider() -> int:
    provider = ServiceProvider(
        bootstrap_token="provider-token-045",
    )
    provider.add_handler("/HELLO", lambda payload: b"HELLO")
    provider.run("/HELLO")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("controller", "provider", "user"), required=True)
    parser.add_argument("--label", default="PYTHON_TOKEN_BOOTSTRAP_REQUEST")
    args = parser.parse_args()

    if args.role == "controller":
        return _run_controller()
    if args.role == "provider":
        return _run_provider()
    return _run_user(args.label)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"PYTHON_TOKEN_CERTIFICATE_BOOTSTRAP_SMOKE=FAIL: {error}", file=sys.stderr)
        raise
