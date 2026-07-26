#!/usr/bin/env python3
"""Python business-logic provider for the HELLO example."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from common import add_process_arguments, optional_local_nfd, print_commands, session_kwargs  # noqa: E402

from ndnsf import ServiceProvider  # noqa: E402


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run a HELLO provider")
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--service", default="/HELLO")
    parser.add_argument("--selection-gated", action="store_true")
    parser.add_argument("--prepare-failure", action="store_true")
    add_process_arguments(parser)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.dry_run:
        print_commands([[
            "python",
            "-c",
            f"ServiceProvider(provider_id={args.provider_id!r}).handler({args.service!r}); provider.run()",
        ]])
        return 0

    provider = ServiceProvider(provider_id=args.provider_id, **session_kwargs(args))

    if args.selection_gated:
        def prepare(context):
            if args.prepare_failure:
                raise RuntimeError("injected generic preparation failure")
            print("SPEC129_PREPARE READY " + context["provider_identity"], flush=True)
            return {
                "artifactDigest": "sha256:spec129-generic-artifact",
                "deploymentInstanceId": "instance-" + args.provider_id.lower(),
                "operationId": "prepare-" + args.provider_id.lower(),
            }
        provider.set_deployment_prepare_handler(prepare)

    @provider.handler(args.service)
    def hello(request: bytes) -> bytes:
        print("SPEC129_EXECUTE provider=" + (args.provider_id or "default"), flush=True)
        return b"HELLO" if request == b"HELLO" else b"unexpected payload"

    with optional_local_nfd(args.start_local_nfd):
        return provider.run(args.service)


if __name__ == "__main__":
    raise SystemExit(main())
