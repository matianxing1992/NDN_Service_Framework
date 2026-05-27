#!/usr/bin/env python3
"""Run one provider role for the distributed AI example."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from common import add_process_arguments, optional_local_nfd, print_commands, session_kwargs  # noqa: E402

from ndnsf import NDNSFSession, ProviderConfig  # noqa: E402


def provider_config(role: str, provider_id: str) -> ProviderConfig:
    args = ["--role", role]
    if provider_id:
        args.extend(["--provider-id", provider_id])
    return ProviderConfig(
        name=f"ai-provider-{role}",
        binary="AI_DistributedCollaborationProvider",
        args=tuple(args),
        service="/AI/FNN/Inference",
        role=role,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run one AI collaboration provider")
    parser.add_argument("--role", required=True,
                        help="AI collaboration role, for example p00, p01, ..., p22")
    parser.add_argument("--provider-id", default="",
                        help="Optional provider identity suffix, for example A")
    add_process_arguments(parser)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    config = provider_config(args.role, args.provider_id)
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
