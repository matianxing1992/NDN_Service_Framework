#!/usr/bin/env python3
"""Materialize and verify the hash-locked Qwen3.6 Python wheel overlay."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import time
import urllib.error
import urllib.request


SCHEMA = "ndnsf-di-runtime-overlay-lock-v1"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
PYPI_PREFIX = "https://files.pythonhosted.org/packages/"


class WheelhouseError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_lock(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if value.get("schemaVersion") != SCHEMA:
        raise WheelhouseError("QWEN36_OVERLAY_LOCK_SCHEMA_INVALID")
    wheels = value.get("wheelClosure")
    if not isinstance(wheels, dict) or not wheels:
        raise WheelhouseError("QWEN36_WHEEL_CLOSURE_EMPTY")
    filenames: set[str] = set()
    for package, row in sorted(wheels.items()):
        if not isinstance(row, dict):
            raise WheelhouseError(f"QWEN36_WHEEL_ROW_INVALID:{package}")
        filename = str(row.get("filename", ""))
        url = str(row.get("url", ""))
        digest = str(row.get("sha256", ""))
        version = str(row.get("version", ""))
        if not package or not version or not filename.endswith(".whl"):
            raise WheelhouseError(f"QWEN36_WHEEL_IDENTITY_INVALID:{package}")
        if filename in filenames:
            raise WheelhouseError(f"QWEN36_WHEEL_FILENAME_DUPLICATE:{filename}")
        if not url.startswith(PYPI_PREFIX) or not url.endswith(filename):
            raise WheelhouseError(f"QWEN36_WHEEL_URL_INVALID:{package}")
        if SHA256.fullmatch(digest) is None:
            raise WheelhouseError(f"QWEN36_WHEEL_SHA256_INVALID:{package}")
        filenames.add(filename)
    return value


def verify_wheelhouse(lock: dict, output: Path) -> None:
    wheels = lock["wheelClosure"]
    expected = {str(row["filename"]) for row in wheels.values()}
    observed = {path.name for path in output.glob("*.whl")}
    if observed != expected:
        missing = sorted(expected - observed)
        extra = sorted(observed - expected)
        raise WheelhouseError(
            f"QWEN36_WHEELHOUSE_SET_MISMATCH:missing={missing}:extra={extra}"
        )
    for package, row in sorted(wheels.items()):
        path = output / str(row["filename"])
        observed_digest = sha256(path)
        if observed_digest != row["sha256"]:
            raise WheelhouseError(
                f"QWEN36_WHEEL_DIGEST_MISMATCH:{package}:{observed_digest}"
            )


def write_requirements(lock: dict, output: Path) -> Path:
    requirements = output / "requirements.txt"
    lines = [
        f"{package}=={row['version']} --hash=sha256:{row['sha256']}"
        for package, row in sorted(lock["wheelClosure"].items())
    ]
    requirements.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return requirements


def download(url: str, target: Path) -> None:
    partial = target.with_suffix(target.suffix + ".partial")
    for attempt in range(1, 4):
        try:
            request = urllib.request.Request(
                url, headers={"User-Agent": "ndnsf-di-spec162-wheel-sealer/1"})
            with urllib.request.urlopen(request, timeout=120) as source:
                with partial.open("wb") as destination:
                    while True:
                        chunk = source.read(1024 * 1024)
                        if not chunk:
                            break
                        destination.write(chunk)
            os.replace(partial, target)
            return
        except (OSError, urllib.error.URLError):
            partial.unlink(missing_ok=True)
            if attempt == 3:
                raise
            time.sleep(attempt * 2)


def create(lock_path: Path, output: Path) -> dict:
    lock = load_lock(lock_path)
    if output.exists() and any(output.iterdir()):
        raise WheelhouseError("QWEN36_WHEELHOUSE_OUTPUT_NOT_EMPTY")
    output.mkdir(parents=True, exist_ok=True)
    for _package, row in sorted(lock["wheelClosure"].items()):
        download(str(row["url"]), output / str(row["filename"]))
    verify_wheelhouse(lock, output)
    write_requirements(lock, output)
    manifest = {
        "schemaVersion": "ndnsf-di-qwen36-wheelhouse-v1",
        "lockDigest": "sha256:" + sha256(lock_path),
        "wheelCount": len(lock["wheelClosure"]),
        "wheels": {
            package: {
                "filename": row["filename"],
                "sha256": row["sha256"],
            }
            for package, row in sorted(lock["wheelClosure"].items())
        },
    }
    (output / "wheelhouse-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("create", "verify"))
    parser.add_argument("--lock", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    lock_path = Path(args.lock).resolve()
    output = Path(args.output).resolve()
    try:
        if args.action == "create":
            result = create(lock_path, output)
        else:
            lock = load_lock(lock_path)
            verify_wheelhouse(lock, output)
            result = {
                "schemaVersion": "ndnsf-di-qwen36-wheelhouse-v1",
                "lockDigest": "sha256:" + sha256(lock_path),
                "wheelCount": len(lock["wheelClosure"]),
            }
    except (OSError, ValueError, json.JSONDecodeError, WheelhouseError) as error:
        print(str(error), file=os.sys.stderr)
        return 4
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
