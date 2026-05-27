#!/usr/bin/env python3
"""Python-managed intermittent-provider reliability example."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from common import (  # noqa: E402
    add_runtime_arguments,
    optional_local_nfd,
    print_commands,
    session_kwargs,
)

from ndnsf import ControllerConfig, NDNSFSession, ProviderConfig, UserConfig  # noqa: E402
from ndnsf.runtime import ProcessResult, wait_for_startup  # noqa: E402


def provider_ids() -> tuple[str, ...]:
    return ("A", "B", "C")


def planned_commands(session_options: dict | None = None) -> list[list[str]]:
    session = NDNSFSession(**(session_options or {}))
    apps = [ControllerConfig(policy_file="examples/hello.policies").as_application()]
    for provider_id in provider_ids():
        apps.append(ProviderConfig(
            name=f"intermittent-provider-{provider_id}",
            binary="App_IntermittentProvider",
            args=("--provider-id", provider_id, "--failure-probability", "0.2"),
            service="/HELLO",
        ).as_application())
    apps.append(UserConfig(
        name="intermittent-user",
        binary="App_IntermittentUser",
        args=(
            "--rate-rps",
            "30",
            "--duration-ms",
            "60000",
            "--strategy",
            "first-responding",
        ),
        service="/HELLO",
    ).as_application())
    return session.commands_for(apps)


def run_intermittent(
    *,
    log_dir: Path,
    session_options: dict,
    controller_startup_wait_s: float,
    startup_wait_s: float,
    provider_start_gap_s: float,
    configure_local_nfd: bool,
    rate_rps: float,
    duration_ms: int,
    failure_probability: float,
    timeout_ms: int,
    ack_timeout_ms: int,
    strategy: str,
) -> ProcessResult:
    with NDNSFSession(log_dir=log_dir, **session_options) as ndnsf:
        if configure_local_nfd:
            ndnsf.configure_svs_group("/example/hello/group")
        ndnsf.start_controller(ControllerConfig(policy_file="examples/hello.policies"))
        wait_for_startup(controller_startup_wait_s)
        for provider_id in provider_ids():
            ndnsf.start_provider(ProviderConfig(
                name=f"intermittent-provider-{provider_id}",
                binary="App_IntermittentProvider",
                args=(
                    "--provider-id",
                    provider_id,
                    "--failure-probability",
                    str(failure_probability),
                ),
                service="/HELLO",
            ))
            wait_for_startup(provider_start_gap_s)
        wait_for_startup(startup_wait_s)
        return ndnsf.run_user(
            UserConfig(
                name="intermittent-user",
                binary="App_IntermittentUser",
                args=(
                    "--rate-rps",
                    str(rate_rps),
                    "--duration-ms",
                    str(duration_ms),
                    "--ack-timeout-ms",
                    str(ack_timeout_ms),
                    "--timeout-ms",
                    str(timeout_ms),
                    "--strategy",
                    strategy,
                ),
                service="/HELLO",
            ),
            timeout=(duration_ms / 1000.0) + (timeout_ms / 1000.0) + 15.0,
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run or preview the Python-managed intermittent-provider example")
    add_runtime_arguments(parser)
    parser.add_argument("--rate-rps", type=float, default=30.0)
    parser.add_argument("--duration-ms", type=int, default=60000)
    parser.add_argument("--failure-probability", type=float, default=0.2)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--ack-timeout-ms", type=int, default=200)
    parser.add_argument("--strategy", default="first-responding",
                        choices=("first-responding", "random-selection", "all-selected"))
    parser.add_argument("--log-dir", default="results/python_intermittent_reliability")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.dry_run or not args.run:
        print_commands(planned_commands(session_kwargs(args)))
        if not args.run:
            print("\nPass --run to launch the example.")
            return 0

    with optional_local_nfd(args.start_local_nfd):
        result = run_intermittent(
            log_dir=Path(args.log_dir),
            session_options=session_kwargs(args),
            controller_startup_wait_s=args.controller_startup_wait_s,
            startup_wait_s=args.startup_wait_s,
            provider_start_gap_s=args.provider_start_gap_s,
            configure_local_nfd=not args.no_configure_local_nfd,
            rate_rps=args.rate_rps,
            duration_ms=args.duration_ms,
            failure_probability=args.failure_probability,
            timeout_ms=args.timeout_ms,
            ack_timeout_ms=args.ack_timeout_ms,
            strategy=args.strategy,
        )
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="" if result.stderr.endswith("\n") else "\n")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
