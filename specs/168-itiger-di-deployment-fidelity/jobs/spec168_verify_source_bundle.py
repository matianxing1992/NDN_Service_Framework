#!/usr/bin/env python3
"""Verify one Spec 168 source bundle's bytes, coverage, and POSIX modes."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import stat


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            value.update(block)
    return value.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--expected-manifest-digest", required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    manifest = root / "source-files.sha256"
    expected = str(args.expected_manifest_digest)
    if expected.startswith("sha256:"):
        expected = expected[7:]
    if sha256(manifest) != expected:
        raise SystemExit("SPEC168_SOURCE_MANIFEST_DIGEST_MISMATCH")
    rows: dict[str, str] = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        digest, relative = line.split("  ", 1)
        path = Path(relative)
        if (path.is_absolute() or ".." in path.parts or relative in rows
                or len(digest) != 64):
            raise SystemExit("SPEC168_SOURCE_MANIFEST_ROW_INVALID")
        rows[relative] = digest
    modes = json.loads((root / "source-modes.json").read_text(encoding="utf-8"))
    if set(modes) != set(rows):
        raise SystemExit("SPEC168_SOURCE_MODE_COVERAGE_MISMATCH")
    actual = {
        str(path.relative_to(root)) for path in root.rglob("*")
        if path.is_file() and path.name != "source-files.sha256"
    }
    if actual != set(rows):
        raise SystemExit("SPEC168_SOURCE_FILE_COVERAGE_MISMATCH")
    for relative, digest in rows.items():
        path = root / relative
        if path.is_symlink() or not path.is_file() or sha256(path) != digest:
            raise SystemExit(f"SPEC168_SOURCE_FILE_INVALID:{relative}")
        observed = stat.S_IMODE(path.stat().st_mode)
        if observed != int(str(modes[relative]), 8):
            raise SystemExit(f"SPEC168_SOURCE_MODE_INVALID:{relative}")
    print(f"SPEC168_SOURCE_BUNDLE_PASS files={len(rows)} digest=sha256:{expected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
