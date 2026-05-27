#!/usr/bin/env python3
"""Run one provider role for the intermittent-provider example."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from common import add_process_arguments, optional_local_nfd, print_commands, session_kwargs  # noqa: E402

from ndnsf import NDNSFSession, ProviderConfig  # noqa: E402


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run one intermittent /HELLO provider")
    parser.add_argument("--provider-id", default="A")
    parser.add_argument("--failure-probability", type=float, default=0.2)
    parser.add_argument("--epoch-ms", type=int, default=10000)
    parser.add_argument("--reject-ms", type=int, default=10000)
    parser.add_argument("--processing-delay-ms", type=int, default=5)
    add_process_arguments(parser)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    config = ProviderConfig(
        name=f"intermittent-provider-{args.provider_id}",
        binary="App_IntermittentProvider",
        args=(
            "--provider-id",
            args.provider_id,
            "--failure-probability",
            str(args.failure_probability),
            "--epoch-ms",
            str(args.epoch_ms),
            "--reject-ms",
            str(args.reject_ms),
            "--processing-delay-ms",
            str(args.processing_delay_ms),
        ),
        service="/HELLO",
    )
    session = NDNSFSession(log_dir=Path(args.log_dir) if args.log_dir else None,
                           **session_kwargs(args))
    if args.dry_run:
        print_commands([session.command_for(config.as_application())])
        return 0

    with optional_local_nfd(args.start_local_nfd):
        result = session.run_application(config.as_application())
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="" if result.stderr.endswith("\n") else "\n")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
