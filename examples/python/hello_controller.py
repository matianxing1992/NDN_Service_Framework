#!/usr/bin/env python3
"""Run only the controller role for the HELLO example."""

from __future__ import annotations

import argparse

from common import add_native_actor_arguments, optional_local_nfd, print_commands  # noqa: E402

from ndnsf import ServiceController  # noqa: E402


HELLO_BOOTSTRAP_IDENTITIES = [
    "/example/hello/provider",
    "/example/hello/provider/A",
    "/example/hello/provider/B",
    "/example/hello/provider/C",
    "/example/hello/user",
]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run the HELLO ServiceController")
    parser.add_argument("--policy-file", default="examples/hello.policies")
    add_native_actor_arguments(parser)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.dry_run:
        print_commands([[
            "python",
            "-c",
            f"ServiceController(policy_file={args.policy_file!r}).run()",
        ]])
        return 0

    with optional_local_nfd(args.start_local_nfd):
        controller = ServiceController(
            controller_prefix="/example/hello/controller",
            policy_file=args.policy_file,
            bootstrap_identities=HELLO_BOOTSTRAP_IDENTITIES,
        )
        return controller.run()


if __name__ == "__main__":
    raise SystemExit(main())
