#!/usr/bin/env python3
"""Python entry point for the NDNSF distributed AI collaboration demo.

This example intentionally uses the Python wrapper as an orchestration layer
around the existing C++ NDNSF runtime. It does not reimplement NDNSF protocol
logic in Python.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
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


@dataclass(frozen=True)
class AIProviderSpec:
    """Role-to-provider mapping for this 3-stage x 3-shard FNN demo."""

    role: str
    provider_id: str = ""


def ai_provider_specs() -> list[AIProviderSpec]:
    return [
        AIProviderSpec("p00", ""),
        AIProviderSpec("p01", "A"),
        AIProviderSpec("p02", "B"),
        AIProviderSpec("p10", "C"),
        AIProviderSpec("p11", "D"),
        AIProviderSpec("p12", "E"),
        AIProviderSpec("p20", "F"),
        AIProviderSpec("p21", "G"),
        AIProviderSpec("p22", "H"),
    ]


def planned_commands(session_options: dict | None = None) -> list[list[str]]:
    session = NDNSFSession(**(session_options or {}))
    apps = [
        ControllerConfig(
            policy_file="examples/ai_collaboration.policies").as_application()
    ]
    for spec in ai_provider_specs():
        args = [
            "--role",
            spec.role,
        ]
        if spec.provider_id:
            args.extend(["--provider-id", spec.provider_id])
        apps.append(ProviderConfig(
            name=f"ai-provider-{spec.role}",
            binary="AI_DistributedCollaborationProvider",
            args=tuple(args),
            service="/AI/FNN/Inference",
            role=spec.role,
        ).as_application())
    apps.append(UserConfig(
        name="ai-user",
        binary="AI_User",
        args=("--timeout-ms", "12000", "--ack-timeout-ms", "1000"),
        service="/AI/FNN/Inference",
    ).as_application())
    return session.commands_for(apps)


def run_local_distributed_ai(
    *,
    log_dir: Path,
    session_options: dict,
    controller_startup_wait_s: float,
    startup_wait_s: float,
    provider_start_gap_s: float,
    configure_local_nfd: bool,
    timeout_ms: int,
    ack_timeout_ms: int,
) -> ProcessResult:
    with NDNSFSession(log_dir=log_dir, **session_options) as ndnsf:
        if configure_local_nfd:
            ndnsf.configure_svs_group("/example/hello/group")

        ndnsf.start_controller(
            ControllerConfig(policy_file="examples/ai_collaboration.policies"))
        wait_for_startup(controller_startup_wait_s)

        for spec in ai_provider_specs():
            provider_args = ["--role", spec.role]
            if spec.provider_id:
                provider_args.extend(["--provider-id", spec.provider_id])
            ndnsf.start_provider(ProviderConfig(
                name=f"ai-provider-{spec.role}",
                binary="AI_DistributedCollaborationProvider",
                args=tuple(provider_args),
                service="/AI/FNN/Inference",
                role=spec.role,
            ))
            wait_for_startup(provider_start_gap_s)

        wait_for_startup(startup_wait_s)
        return ndnsf.run_user(
            UserConfig(
                name="ai-user",
                binary="AI_User",
                args=(
                    "--timeout-ms",
                    str(timeout_ms),
                    "--ack-timeout-ms",
                    str(ack_timeout_ms),
                ),
                service="/AI/FNN/Inference",
            ),
            timeout=(timeout_ms / 1000.0) + 10.0,
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run or preview the Python-managed NDNSF distributed AI demo")
    add_runtime_arguments(parser)
    parser.add_argument("--timeout-ms", type=int, default=12000,
                        help="AI_User collaboration timeout")
    parser.add_argument("--ack-timeout-ms", type=int, default=1000,
                        help="AI_User ACK collection timeout")
    parser.add_argument("--log-dir", default="results/python_ai_distributed_collaboration",
                        help="Directory for wrapper-managed stdout/stderr logs")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.dry_run or not args.run:
        print_commands(planned_commands(session_kwargs(args)))
        if not args.run:
            print("\nPass --run to launch the demo.")
            return 0

    with optional_local_nfd(args.start_local_nfd):
        result = run_local_distributed_ai(
            log_dir=Path(args.log_dir),
            session_options=session_kwargs(args),
            controller_startup_wait_s=args.controller_startup_wait_s,
            startup_wait_s=args.startup_wait_s,
            provider_start_gap_s=args.provider_start_gap_s,
            configure_local_nfd=not args.no_configure_local_nfd,
            timeout_ms=args.timeout_ms,
            ack_timeout_ms=args.ack_timeout_ms,
        )
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="" if result.stderr.endswith("\n") else "\n")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
