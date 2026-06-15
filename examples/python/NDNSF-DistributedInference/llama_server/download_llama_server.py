#!/usr/bin/env python3
"""Download a prebuilt llama-server executable for the local platform.

This helper intentionally keeps runtime deployment separate from NDNSF service
invocation.  A provider can run it once during node setup, then serve the LLM
through NDNSF without shipping the executable on every inference request.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import stat
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path
from urllib import request


GITHUB_LATEST_RELEASE = "https://api.github.com/repos/ggml-org/llama.cpp/releases/latest"


def platform_asset_keywords(system: str | None = None,
                            machine: str | None = None) -> tuple[tuple[str, ...], str]:
    system = (system or platform.system()).lower()
    machine = (machine or platform.machine()).lower()
    if system == "windows":
        arch = "x64" if machine in ("amd64", "x86_64") else machine
        return (("win",), arch)
    if system == "linux":
        if machine in ("x86_64", "amd64"):
            return (("ubuntu", "linux"), "x64")
        if machine in ("aarch64", "arm64"):
            return (("ubuntu", "linux"), "arm64")
        return (("ubuntu", "linux"), machine)
    if system == "darwin":
        if machine in ("arm64", "aarch64"):
            return (("macos",), "arm64")
        return (("macos",), "x64")
    return ((system,), machine)


def _asset_score(name: str, os_aliases: tuple[str, ...], arch: str) -> int:
    lowered = name.lower()
    if not lowered.endswith((".zip", ".tar.gz", ".tgz")):
        return -1
    if "server" not in lowered and "bin" not in lowered:
        return -1
    if not any(alias in lowered for alias in os_aliases):
        return -1
    if arch and arch not in lowered:
        return -1
    score = 0
    score += 20
    score += 10
    if "cuda" in lowered:
        score -= 3
    if "rocm" in lowered or "openvino" in lowered or "sycl" in lowered:
        score -= 2
    if "vulkan" in lowered:
        score -= 1
    if "cpu" in lowered:
        score += 1
    return score


def select_release_asset(release: dict, *,
                         system: str | None = None,
                         machine: str | None = None) -> dict:
    os_aliases, arch = platform_asset_keywords(system, machine)
    assets = release.get("assets", [])
    ranked = sorted(
        (
            (_asset_score(str(asset.get("name", "")), os_aliases, arch), asset)
            for asset in assets
        ),
        key=lambda item: item[0],
        reverse=True,
    )
    if not ranked or ranked[0][0] < 1:
        names = ", ".join(str(asset.get("name", "")) for asset in assets) or "(none)"
        raise RuntimeError(
            f"could not find llama.cpp release asset for platform os={os_aliases} "
            f"arch={arch}; available assets: {names}")
    return ranked[0][1]


def _download_json(url: str) -> dict:
    with request.urlopen(url, timeout=30) as response:
        return json.loads(response.read().decode("utf-8"))


def _download_file(url: str, destination: Path) -> None:
    with request.urlopen(url, timeout=120) as response:
        destination.write_bytes(response.read())


def _find_llama_server(root: Path) -> Path:
    candidates = [
        path for path in root.rglob("*")
        if path.is_file() and path.name in ("llama-server", "llama-server.exe")
    ]
    if not candidates:
        raise RuntimeError(f"archive did not contain llama-server under {root}")
    return candidates[0]


def _ensure_safe_archive_target(root: Path, member_name: str) -> None:
    target = (root / member_name).resolve()
    try:
        target.relative_to(root.resolve())
    except ValueError as exc:
        raise RuntimeError(f"unsafe archive path: {member_name}") from exc


def _extract_archive(archive: Path, destination: Path) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="llama-server-extract-") as tmp:
        tmpdir = Path(tmp)
        if archive.name.endswith(".zip"):
            with zipfile.ZipFile(archive) as zf:
                for member in zf.namelist():
                    _ensure_safe_archive_target(tmpdir, member)
                zf.extractall(tmpdir)
        elif archive.name.endswith((".tar.gz", ".tgz")):
            with tarfile.open(archive) as tf:
                for member in tf.getmembers():
                    _ensure_safe_archive_target(tmpdir, member.name)
                tf.extractall(tmpdir)
        else:
            raise RuntimeError(f"unsupported archive type: {archive}")
        executable = _find_llama_server(tmpdir)
        target = destination / executable.name
        target.write_bytes(executable.read_bytes())
        target.chmod(target.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        return target


def download_llama_server(destination: Path, *,
                          release_url: str = GITHUB_LATEST_RELEASE,
                          dry_run: bool = False) -> Path:
    release = _download_json(release_url)
    asset = select_release_asset(release)
    asset_url = str(asset.get("browser_download_url", ""))
    if not asset_url:
        raise RuntimeError(f"release asset has no browser_download_url: {asset}")
    destination.mkdir(parents=True, exist_ok=True)
    if dry_run:
        print(
            "LLAMA_SERVER_DOWNLOAD_PLAN",
            f"asset={asset.get('name')}",
            f"url={asset_url}",
            f"destination={destination}",
        )
        return destination / ("llama-server.exe" if platform.system().lower() == "windows" else "llama-server")
    with tempfile.TemporaryDirectory(prefix="llama-server-download-") as tmp:
        archive = Path(tmp) / str(asset.get("name", "llama.cpp-release.zip"))
        _download_file(asset_url, archive)
        return _extract_archive(archive, destination)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dest", default="third_party/llama.cpp/bin")
    parser.add_argument("--release-url", default=GITHUB_LATEST_RELEASE)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    executable = download_llama_server(
        Path(args.dest),
        release_url=args.release_url,
        dry_run=args.dry_run,
    )
    print(f"LLAMA_SERVER_DOWNLOAD_OK executable={executable}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
