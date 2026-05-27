"""Shared helpers for Python-managed NDNSF examples.

This module is intentionally kept under examples/.  The reusable framework API
lives in the installable ndnsf Python package; these helpers only reduce
duplication among the checked-in example applications.
"""

from __future__ import annotations

from contextlib import contextmanager
import shutil
import subprocess
from typing import Iterator, Sequence
from pathlib import Path


def print_commands(commands: Sequence[Sequence[str]]) -> None:
    print("NDNSF Python example dry run. Commands:")
    for command in commands:
        print("  " + " ".join(str(part) for part in command))


def add_runtime_arguments(parser) -> None:
    parser.add_argument("--run", action="store_true",
                        help="Launch the C++ NDNSF applications")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the C++ commands without launching them")
    parser.add_argument("--start-local-nfd", action="store_true",
                        help="Start nfd if needed; stop only the instance started here")
    parser.add_argument("--no-configure-local-nfd", action="store_true",
                        help="Do not set the local SVS group multicast strategy")
    parser.add_argument("--controller-startup-wait-s", type=float, default=1.0,
                        help="Seconds to wait after starting ServiceController")
    parser.add_argument("--startup-wait-s", type=float, default=3.0,
                        help="Seconds to wait after starting providers")
    parser.add_argument("--provider-start-gap-s", type=float, default=0.3,
                        help="Seconds to wait between provider starts")
    parser.add_argument("--binary-dir", default="",
                        help="Directory containing installed NDNSF C++ binaries")
    parser.add_argument("--library-dir", action="append", default=[],
                        help="Directory to prepend to LD_LIBRARY_PATH")
    parser.add_argument("--working-dir", default="",
                        help="Working directory for launched C++ applications")


def add_process_arguments(parser) -> None:
    parser.add_argument("--log-dir", default="")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--start-local-nfd", action="store_true",
                        help="Start nfd if needed; stop only the instance started here")
    parser.add_argument("--binary-dir", default="",
                        help="Directory containing installed NDNSF C++ binaries")
    parser.add_argument("--library-dir", action="append", default=[],
                        help="Directory to prepend to LD_LIBRARY_PATH")
    parser.add_argument("--working-dir", default="",
                        help="Working directory for launched C++ applications")


def session_kwargs(args) -> dict:
    kwargs = {}
    if getattr(args, "binary_dir", ""):
        kwargs["binary_dir"] = Path(args.binary_dir)
    if getattr(args, "library_dir", None):
        kwargs["library_dirs"] = [Path(value) for value in args.library_dir]
    if getattr(args, "working_dir", ""):
        kwargs["cwd"] = Path(args.working_dir)
    return kwargs



@contextmanager
def optional_local_nfd(enabled: bool) -> Iterator[None]:
    """Optionally start a local NFD for real-machine single-host examples."""

    started_here = False
    if enabled:
        if shutil.which("nfd-start") is None or shutil.which("nfd-stop") is None:
            raise RuntimeError("nfd-start/nfd-stop are required for --start-local-nfd")
        running = subprocess.run(["pgrep", "-x", "nfd"],
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL,
                                 check=False).returncode == 0
        if not running:
            subprocess.run(["nfd-start"], check=True)
            started_here = True
    try:
        yield
    finally:
        if started_here:
            subprocess.run(["nfd-stop"], check=False,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
