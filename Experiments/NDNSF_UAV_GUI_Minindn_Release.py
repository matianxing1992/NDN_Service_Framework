#!/usr/bin/env python3
"""Run the NDNSF UAV MiniNDN demo with a packaged release directory.

This launcher is intentionally separate from NDNSF_UAV_GUI_Minindn.py so it is
obvious that the APP binaries come from RELEASE/<release>/bin instead
of build/examples. It still reuses the normal MiniNDN topology and arguments.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import runpy
import sys


REPO = Path(__file__).resolve().parents[1]
DEFAULT_RELEASE_ROOT = REPO / "RELEASE"
DEFAULT_RELEASE_PREFIX = "NDNSF-UAV-"


def newest_release_dir(root: Path) -> Path | None:
    candidates = [
        path for path in root.iterdir()
        if path.is_dir() and path.name.startswith(DEFAULT_RELEASE_PREFIX) and
        (path / "bin" / "App_ServiceController").exists() and
        (path / "bin" / "UavDroneApp").exists() and
        (path / "bin" / "UavGroundStationApp").exists()
    ] if root.exists() else []
    if not candidates:
      return None
    return max(candidates, key=lambda path: path.stat().st_mtime)


def parse_release_args(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(
        description="Run NDNSF UAV MiniNDN with binaries from a RELEASE directory",
        add_help=True)
    parser.add_argument("--release-dir", default="",
                        help="Path to an unpacked NDNSF-UAV release directory. "
                             "Default: newest RELEASE/NDNSF-UAV-* directory.")
    parser.add_argument("--print-release-command", action="store_true",
                        help="Print the underlying command shape and exit.")
    return parser.parse_known_args(argv)


def main(argv: list[str] | None = None) -> int:
    args, passthrough = parse_release_args(list(argv if argv is not None else sys.argv[1:]))
    release_dir = Path(args.release_dir).expanduser().resolve() if args.release_dir else newest_release_dir(DEFAULT_RELEASE_ROOT)
    if release_dir is None:
        raise SystemExit("No release directory found. Build one with "
                         "packaging/uav-release/create-portable-release.sh")

    bin_dir = release_dir / "bin"
    required = ["App_ServiceController", "UavDroneApp", "UavGroundStationApp"]
    missing = [name for name in required if not (bin_dir / name).is_file()]
    if missing:
        raise SystemExit(f"Release directory is missing APP binaries: {', '.join(missing)}")

    os.environ["NDNSF_UAV_APP_BUILD_DIR"] = str(bin_dir)
    print(f"NDNSF_UAV_RELEASE_MININDN using release: {release_dir}")
    print(f"NDNSF_UAV_RELEASE_MININDN app bin dir: {bin_dir}")

    if args.print_release_command:
        print("NDNSF_UAV_APP_BUILD_DIR=" + str(bin_dir) +
              " python3 Experiments/NDNSF_UAV_GUI_Minindn.py " +
              " ".join(passthrough))
        return 0

    script = REPO / "Experiments" / "NDNSF_UAV_GUI_Minindn.py"
    sys.argv = [str(script), *passthrough]
    runpy.run_path(str(script), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
