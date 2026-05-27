#!/usr/bin/env python3
"""Python-managed HELLO service example using the reusable NDNSF wrapper API."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

from common import (  # noqa: E402
    add_runtime_arguments,
    optional_local_nfd,
    print_commands,
    session_kwargs,
)

from ndnsf import ControllerConfig, NDNSFSession  # noqa: E402
from ndnsf.runtime import ProcessResult, wait_for_startup  # noqa: E402


def planned_commands(session_options: dict | None = None) -> list[list[str]]:
    session = NDNSFSession(**(session_options or {}))
    apps = [
        ControllerConfig(policy_file="examples/hello.policies").as_application(),
    ]
    commands = session.commands_for(apps)
    commands.append([sys.executable, "examples/python/hello_provider.py"])
    commands.append([sys.executable, "examples/python/hello_user.py"])
    return commands


def run_hello(
    *,
    log_dir: Path,
    session_options: dict,
    controller_startup_wait_s: float,
    startup_wait_s: float,
    configure_local_nfd: bool,
    timeout_ms: int,
    ack_timeout_ms: int,
) -> ProcessResult:
    log_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    repo = Path(__file__).resolve().parents[2]
    python_path = str(repo / "pythonWrapper")
    env["PYTHONPATH"] = (
        python_path if not env.get("PYTHONPATH")
        else python_path + os.pathsep + env["PYTHONPATH"]
    )
    if session_options.get("library_dirs"):
        lib_path = os.pathsep.join(str(path) for path in session_options["library_dirs"])
        env["LD_LIBRARY_PATH"] = (
            lib_path if not env.get("LD_LIBRARY_PATH")
            else lib_path + os.pathsep + env["LD_LIBRARY_PATH"]
        )

    common_args = []
    if session_options.get("binary_dir"):
        common_args.extend(["--binary-dir", str(session_options["binary_dir"])])
    for library_dir in session_options.get("library_dirs", []):
        common_args.extend(["--library-dir", str(library_dir)])
    if session_options.get("cwd"):
        common_args.extend(["--working-dir", str(session_options["cwd"])])

    with NDNSFSession(log_dir=log_dir, **session_options) as ndnsf:
        if configure_local_nfd:
            ndnsf.configure_svs_group("/example/hello/group")
        ndnsf.start_controller(
            ControllerConfig(policy_file="examples/hello.policies"))
        wait_for_startup(controller_startup_wait_s)

        provider_log = (log_dir / "python-provider.out").open("w", encoding="utf-8")
        provider = subprocess.Popen(
            [sys.executable, "examples/python/hello_provider.py", *common_args],
            cwd=str(Path(__file__).resolve().parents[2]),
            env=env,
            stdout=provider_log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_for_startup(startup_wait_s)
            result = subprocess.run(
                [
                    sys.executable,
                    "examples/python/hello_user.py",
                    "--ack-timeout-ms", str(ack_timeout_ms),
                    "--timeout-ms", str(timeout_ms),
                    *common_args,
                ],
                cwd=str(Path(__file__).resolve().parents[2]),
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=(timeout_ms / 1000.0) + 10.0,
                check=False,
            )
            return ProcessResult("hello-user", result.returncode, result.stdout, result.stderr)
        finally:
            if provider.poll() is None:
                provider.terminate()
                try:
                    provider.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    provider.kill()
                    provider.wait(timeout=3)
            provider_log.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run or preview the Python-managed NDNSF HELLO example")
    add_runtime_arguments(parser)
    parser.add_argument("--timeout-ms", type=int, default=5000)
    parser.add_argument("--ack-timeout-ms", type=int, default=300)
    parser.add_argument("--log-dir", default="results/python_hello_service")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.dry_run or not args.run:
        print_commands(planned_commands(session_kwargs(args)))
        if not args.run:
            print("\nPass --run to launch the example.")
            return 0

    with optional_local_nfd(args.start_local_nfd):
        result = run_hello(
            log_dir=Path(args.log_dir),
            session_options=session_kwargs(args),
            controller_startup_wait_s=args.controller_startup_wait_s,
            startup_wait_s=args.startup_wait_s,
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
