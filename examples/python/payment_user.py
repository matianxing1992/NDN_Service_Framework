#!/usr/bin/env python3
"""Run only the user role for the payment collaboration example."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from common import add_process_arguments, optional_local_nfd, print_commands, session_kwargs  # noqa: E402

from ndnsf import NDNSFSession, UserConfig  # noqa: E402


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the payment collaboration user")
    parser.add_argument("--timeout-ms", type=int, default=12000)
    parser.add_argument("--ack-timeout-ms", type=int, default=1000)
    add_process_arguments(parser)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    config = UserConfig(
        name="payment-user",
        binary="Payment_User",
        args=(
            "--timeout-ms",
            str(args.timeout_ms),
            "--ack-timeout-ms",
            str(args.ack_timeout_ms),
        ),
        service="/Payment/Checkout",
    )
    session = NDNSFSession(log_dir=Path(args.log_dir) if args.log_dir else None,
                           **session_kwargs(args))
    if args.dry_run:
        print_commands([session.command_for(config.as_application())])
        return 0

    with optional_local_nfd(args.start_local_nfd):
        result = session.run_user(config, timeout=(args.timeout_ms / 1000.0) + 10.0)
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="" if result.stderr.endswith("\n") else "\n")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
