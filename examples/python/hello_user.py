#!/usr/bin/env python3
"""Python business-logic user for the HELLO example."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from common import add_process_arguments, optional_local_nfd, print_commands, session_kwargs  # noqa: E402

from ndnsf import ServiceUser  # noqa: E402


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the HELLO user")
    parser.add_argument("--ack-timeout-ms", type=int, default=300)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument(
        "--list-services",
        action="store_true",
        help="fetch permissions and print services this user is allowed to call",
    )
    add_process_arguments(parser)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.dry_run:
        print_commands([[
            "python",
            "-c",
            "ServiceUser().request_service('/HELLO', b'HELLO', "
            f"ack_timeout_ms={args.ack_timeout_ms}, timeout_ms={args.timeout_ms})",
        ]])
        return 0

    with optional_local_nfd(args.start_local_nfd):
        user = ServiceUser(**session_kwargs(args))
        if args.list_services:
            services = user.get_allowed_services()
            if not services:
                print("No allowed services found")
                return 1
            for entry in services:
                provider = entry.provider_service or "-"
                token = entry.token or "-"
                print(f"{entry.service}\tprovider={provider}\ttoken={token}")
            return 0

        response = user.request_service(
            "/HELLO",
            b"HELLO",
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=args.timeout_ms,
        )
    if response.status:
        print(response.payload.decode(errors="replace"))
        return 0
    print(response.error, file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
