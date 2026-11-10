"""Inspect local inference artifacts; native C++ owns computation identity and admission."""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import importlib.metadata
import io
import json
import os
from pathlib import Path
import re
import stat
import sys


MAX_LOCK_BYTES = 256 * 1024
MAX_DISTRIBUTIONS = 128
MAX_RECORDS = 65536
MAX_METADATA_BYTES = 256 * 1024
MAX_RECORD_METADATA_BYTES = 8 * 1024 * 1024
MAX_FILE_BYTES = 1024 * 1024 * 1024
MAX_TOTAL_BYTES = 4 * MAX_FILE_BYTES
CHUNK_BYTES = 65536


class _BoundedDistribution(importlib.metadata.PathDistribution):
    """Admit bytes and record counts before stdlib metadata materializes entries."""

    def __init__(self, path: Path, record_budget: list[int]):
        super().__init__(path)
        self.info_path = path
        self.record_budget = record_budget

    def read_text(self, filename):
        if filename not in ("METADATA", "RECORD"):
            return None
        path = self.info_path / filename
        if not path.resolve(strict=True).is_relative_to(self.info_path):
            raise ValueError("runtime metadata escapes distribution directory")
        if not path.is_file():
            raise ValueError("missing or nonregular runtime metadata")
        limit = MAX_RECORD_METADATA_BYTES if filename == "RECORD" else MAX_METADATA_BYTES
        with path.open("rb") as stream:
            raw = stream.read(limit + 1)
        if len(raw) > limit:
            raise ValueError("runtime metadata exceeds byte limit")
        text = raw.decode("utf-8")
        if filename == "RECORD":
            for row in csv.reader(io.StringIO(text)):
                self.record_budget[0] += 1
                if self.record_budget[0] > MAX_RECORDS or len(row) != 3:
                    raise ValueError("runtime file records exceed limit or are malformed")
        return text


def _discover_distributions(prefix: Path):
    record_budget = [0]
    scanned = 0
    if len(sys.path) > MAX_DISTRIBUTIONS:
        raise ValueError("excessive runtime import roots")
    roots = sorted({Path(p).resolve() for p in sys.path if p and Path(p).is_dir()})
    if len(roots) > MAX_DISTRIBUTIONS:
        raise ValueError("excessive runtime import roots")
    for root in roots:
        with os.scandir(root) as entries:
            for entry in entries:
                scanned += 1
                if scanned > MAX_RECORDS:
                    raise ValueError("runtime discovery exceeds entry limit")
                if entry.name.endswith(".egg-info"):
                    raise ValueError("runtime requires wheel distribution metadata")
                if not entry.name.endswith(".dist-info"):
                    continue
                path = Path(entry.path).resolve(strict=True)
                if not path.is_relative_to(prefix) or not path.is_dir():
                    raise ValueError("runtime distribution escapes isolated environment")
                yield _BoundedDistribution(path, record_budget)


def _name(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", value):
        raise ValueError("invalid runtime distribution name")
    return re.sub(r"[-_.]+", "-", value.lower())


def _load_lock(path: Path) -> tuple[dict[str, str], str]:
    if not path.is_file():
        raise ValueError("runtime lock is not a regular file")
    with path.open("rb") as stream:
        raw = stream.read(MAX_LOCK_BYTES + 1)
    if len(raw) > MAX_LOCK_BYTES:
        raise ValueError("runtime lock exceeds byte limit")
    versions: dict[str, str] = {}
    current = None
    hashes = 0
    continued = False
    for line in raw.decode("ascii").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if line in ("--index-url https://pypi.org/simple",
                    "--extra-index-url https://download.pytorch.org/whl/cpu"):
            if continued:
                raise ValueError("index inside runtime requirement")
            continue
        if line.startswith("--hash="):
            if not continued or not re.fullmatch(r"--hash=sha256:[0-9a-f]{64}(?: \\)?", line):
                raise ValueError("invalid runtime artifact hash")
            hashes += 1
            continued = line.endswith("\\")
            continue
        if continued or (current is not None and hashes == 0):
            raise ValueError("runtime requirement has no complete artifact hashes")
        match = re.fullmatch(r"([A-Za-z0-9._-]+)==([A-Za-z0-9+._-]+) \\", line)
        if match is None:
            raise ValueError("runtime lock requires exact hashed versions")
        current = _name(match[1])
        if current in versions or len(versions) >= MAX_DISTRIBUTIONS:
            raise ValueError("duplicate or excessive runtime distributions")
        versions[current] = match[2]
        hashes = 0
        continued = True
    if not versions or continued or hashes == 0:
        raise ValueError("incomplete runtime lock")
    return versions, "sha256:" + hashlib.sha256(raw).hexdigest()


def _hash_file(path: Path, budget: list[int], *, prefix: Path | None = None) -> tuple[str, int]:
    # Linux resolves links while opening; validate and hash that same owned descriptor.
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise ValueError("runtime artifact is not a regular file")
        if prefix is not None:
            resolved = Path(os.readlink("/proc/self/fd/" + str(descriptor)))
            if str(resolved).endswith(" (deleted)"):
                raise ValueError("runtime artifact was removed during inspection")
            if not resolved.is_absolute() or not resolved.is_relative_to(prefix):
                raise ValueError("runtime file escapes isolated environment")
        digest = hashlib.sha256()
        size = 0
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            while True:
                block = stream.read(CHUNK_BYTES)
                if not block:
                    break
                size += len(block)
                budget[0] += len(block)
                if size > MAX_FILE_BYTES or budget[0] > MAX_TOTAL_BYTES:
                    raise ValueError("runtime artifacts exceed byte limit")
                digest.update(block)
        return digest.hexdigest(), size
    finally:
        os.close(descriptor)


def _inspect_packages(versions: dict[str, str], distributions, prefix: Path) -> dict:
    installed = {}
    for distribution in distributions:
        if len(installed) >= MAX_DISTRIBUTIONS:
            raise ValueError("excessive installed distributions")
        name = _name(distribution.metadata["Name"])
        if name in installed:
            raise ValueError("duplicate or excessive installed distributions")
        installed[name] = distribution
    if set(installed) != set(versions):
        raise ValueError("installed distribution set differs from runtime lock")
    result = {}
    budget = [0]
    records = 0
    for name in sorted(versions):
        distribution = installed[name]
        if distribution.version != versions[name]:
            raise ValueError("installed distribution version differs from runtime lock: " + name)
        files = distribution.files
        if not files:
            raise ValueError("installed distribution has no file records: " + name)
        inventory = {}
        byte_count = 0
        for entry in files:
            records += 1
            if records > MAX_RECORDS:
                raise ValueError("runtime file records exceed limit")
            relative = str(entry)
            if relative.endswith(".pyc"):
                continue  # Generated bytecode is not a wheel artifact.
            if relative in inventory:
                raise ValueError("duplicate runtime file record")
            path = Path(distribution.locate_file(entry))
            digest, size = _hash_file(path, budget, prefix=prefix)
            expected = entry.hash
            if expected is None:
                if not relative.endswith(".dist-info/RECORD"):
                    raise ValueError("unhashed runtime file record")
            elif (expected.mode != "sha256" or
                  base64.urlsafe_b64encode(bytes.fromhex(digest)).rstrip(b"=").decode() !=
                  expected.value):
                raise ValueError("runtime file differs from installed RECORD: " + relative)
            inventory[relative] = "sha256:" + digest
            byte_count += size
        canonical = json.dumps(inventory, sort_keys=True, separators=(",", ":")).encode()
        result[name] = {"version": versions[name], "fileCount": len(inventory),
                        "bytes": byte_count,
                        "filesDigest": "sha256:" + hashlib.sha256(canonical).hexdigest()}
    return result


def _loaded_python_library(maps_path: Path) -> Path | None:
    # Distribution build variables can name a development-only symlink even
    # when the running interpreter embeds Python and loads no libpython.
    with maps_path.open() as stream:
        text = stream.read(MAX_METADATA_BYTES + 1)
    if len(text) > MAX_METADATA_BYTES:
        raise ValueError("runtime mappings exceed byte limit")
    name = re.compile(r"libpython" + re.escape(
        f"{sys.version_info.major}.{sys.version_info.minor}") + r"\.so(?:\.[0-9]+)*$")
    libraries = set()
    for line in text.splitlines():
        fields = line.split(None, 5)
        if len(fields) == 6 and name.fullmatch(Path(fields[5]).name):
            libraries.add(Path(fields[5]).resolve(strict=True))
        elif len(fields) == 6 and fields[5].endswith(" (deleted)"):
            if name.fullmatch(Path(fields[5][:-10]).name):
                raise ValueError("loaded Python library was deleted")
    if len(libraries) > 1:
        raise ValueError("multiple loaded Python libraries")
    return next(iter(libraries), None)


def inspect_person_runtime(lock_path: Path) -> dict:
    """Return bounded current artifact fingerprints, without application identity fields."""
    if sys.prefix == sys.base_prefix:
        raise ValueError("person runtime requires an isolated Python environment")
    prefix = Path(sys.prefix).resolve(strict=True)
    versions, lock_digest = _load_lock(lock_path)
    distributions = []
    for distribution in _discover_distributions(prefix):
        if len(distributions) >= MAX_DISTRIBUTIONS:
            raise ValueError("excessive installed distributions")
        distributions.append(distribution)
    packages = _inspect_packages(versions, distributions, prefix)
    budget = [sum(package["bytes"] for package in packages.values())]
    artifacts = {"pythonExecutable": Path(sys.executable).resolve(strict=True),
                 "venvConfig": prefix / "pyvenv.cfg"}
    library = _loaded_python_library(Path("/proc/self/maps"))
    if library is not None:
        artifacts["pythonLibrary"] = library
    ultralytics = next(d for d in distributions if _name(d.metadata["Name"]) == "ultralytics")
    artifacts["byteTrackConfig"] = Path(ultralytics.locate_file(
        "ultralytics/cfg/trackers/bytetrack.yaml")).resolve(strict=True)
    fingerprints = {}
    for name, path in sorted(artifacts.items()):
        digest, size = _hash_file(path, budget)
        fingerprints[name] = {"digest": "sha256:" + digest, "bytes": size}
    return {"schema": "uav-person-runtime-v1", "pythonVersion": sys.version.split()[0],
            "lockDigest": lock_digest, "distributions": packages, "artifacts": fingerprints}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", required=True, type=Path)
    arguments = parser.parse_args(argv)
    try:
        result = inspect_person_runtime(arguments.lock)
    except Exception as error:
        print("person-runtime-invalid: " + str(error), file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True, separators=(",", ":"), allow_nan=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
