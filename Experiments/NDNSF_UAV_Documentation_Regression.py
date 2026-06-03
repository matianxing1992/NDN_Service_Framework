#!/usr/bin/env python3
"""Regression checks for UAV README and release manual status claims.

The goal is not to lint prose.  This script guards the deployment-facing
statements that operators depend on: camera, flight controller, mission, video,
repo/recording, and release usage must stay visible in the English docs, and
the checked-in release manual must not contradict the app README.
"""

from __future__ import annotations

from pathlib import Path


REPO = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    full = REPO / path
    if not full.exists():
        raise AssertionError(f"missing required document: {path}")
    return full.read_text(encoding="utf-8")


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"{label}: missing required text: {needle}")


def reject(text: str, needle: str, label: str) -> None:
    if needle in text:
        raise AssertionError(f"{label}: stale text should be removed: {needle}")


def main() -> int:
    app_readme = read("NDNSF-UAV-APP/README.md")
    release_readme = read("RELEASE/README.md")
    usermanual = read("RELEASE/usermanual.md")

    for needle in [
        "## Current Runtime Status",
        "**Camera.**",
        "**Flight controller.**",
        "**Mission.**",
        "**Video.**",
        "**Repo.**",
        "stream session metadata",
        "encrypted camera recording",
        "Mission images, telemetry logs, detection",
    ]:
        require(app_readme, needle, "NDNSF-UAV-APP/README.md")

    for stale in [
        "fetches frames by name",
        "frame-name/prefetch pipeline",
        "The frame data should be published",
    ]:
        reject(app_readme, stale, "NDNSF-UAV-APP/README.md")

    for needle in [
        "ndnsf-uav-gs",
        "ndnsf-uav-drone",
        "config",
        "NFD",
    ]:
        require(release_readme, needle, "RELEASE/README.md")

    for needle in [
        "Ground Station",
        "Start Video",
        "Stop Video",
        "camera",
        "flight controller",
        "MiniNDN",
    ]:
        require(usermanual, needle, "RELEASE/usermanual.md")

    # The release docs should describe user-facing operation; the app README is
    # allowed to be more technical, but neither should resurrect the old
    # frame-name wording.
    reject(usermanual, "frame-name/prefetch pipeline", "RELEASE/usermanual.md")
    reject(release_readme, "frame-name/prefetch pipeline", "RELEASE/README.md")

    print("NDNSF_UAV_DOCUMENTATION_REGRESSION_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
