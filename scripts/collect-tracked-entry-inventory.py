#!/usr/bin/env python3
"""Inventory every tracked path so entry coverage can be checked rather than asserted.

CD-05 asks for a collector over the **git index** rather than a hand-maintained list: dotfiles, Waf
helpers, `.pc/.pc.in`, `.def/.def.in`, `.cfg/.conf`, `.toml`, lock files, workflows, gitlinks and symlinks
all belong in it, with text and binary assets separated. Model weights and private keys are hashed by
size only, never read.

The output is deterministic: paths sorted, no timestamps, so two checkouts of the same commit compare
equal.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path

# Categories CD-05 names explicitly. A path may belong to several; the first match wins for the
# single-valued `category` field and every match is listed in `categories`.
CATEGORIES = (
    ("workflow", (".github/workflows/",)),
    ("waf-helper", (".waf", "waflib/")),
    ("pkgconfig-template", (".pc.in", ".pc")),
    ("container-definition", (".def.in", ".def")),
    ("config", (".cfg", ".conf", ".toml", ".ini", ".yaml", ".yml", ".json")),
    ("lock", (".lock", "lock.json", "package-lock", "yarn.lock")),
    ("schema", (".xsd", ".schema.json")),
    ("script", (".sh", ".bash", ".py")),
    ("source", (".c", ".cc", ".cpp", ".h", ".hpp")),
    ("document", (".md", ".rst", ".txt")),
)

# Never read these: private material or bulk model/evidence assets.
PRIVATE_SUFFIXES = (".ndnkey", ".pem", ".key", ".p12", ".pfx", ".cert")
MODEL_SUFFIXES = (".onnx", ".onnx_data", ".pt", ".pth", ".gguf", ".safetensors", ".bin")
BULK_SUFFIXES = MODEL_SUFFIXES + (".sqlite", ".db", ".mp4", ".tar", ".gz", ".zip")

TEXT_BYTES = 8192


def tracked_entries(root: Path) -> list[tuple[str, int]]:
    """Return (path, mode) for every tracked path, from the index rather than the working tree."""
    out = subprocess.check_output(["git", "ls-files", "--stage", "-z"], cwd=str(root))
    entries: list[tuple[str, int]] = []
    for record in out.split(b"\0"):
        if not record:
            continue
        meta, _, path = record.partition(b"\t")
        mode = int(meta.split(b" ")[0], 8)
        entries.append((path.decode("utf-8", "surrogateescape"), mode))
    return sorted(entries)


def categorise(path: str) -> list[str]:
    found = []
    for name, needles in CATEGORIES:
        if any(needle in path for needle in needles):
            found.append(name)
    return found or ["other"]


def describe(root: Path, path: str, mode: int) -> dict:
    """Describe one tracked path without reading private or bulk content."""
    absolute = root / path
    suffix = Path(path).suffix.lower()
    lowered = path.lower()
    record = {"path": path, "categories": categorise(path)}

    if mode == 0o120000:
        target = ""
        try:
            target = absolute.resolve().as_posix() if False else str(absolute.parent / os.readlink(absolute))
        except OSError:
            target = ""
        record.update(kind="symlink", target=target,
                      dangling=not absolute.exists(), size=0)
        return record
    if mode == 0o160000:
        record.update(kind="gitlink", size=0)
        return record

    size = absolute.stat().st_size if absolute.exists() else 0
    record.update(kind="file", size=size)
    if any(secret in lowered for secret in PRIVATE_SUFFIXES):
        record.update(content="private-not-read")
        return record
    if suffix in BULK_SUFFIXES:
        record.update(content="bulk-not-read")
        return record

    try:
        with absolute.open("rb") as stream:
            head = stream.read(TEXT_BYTES)
            digest = hashlib.sha256()
            digest.update(head)
            while True:
                chunk = stream.read(1 << 20)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError:
        record.update(content="unreadable")
        return record
    # A NUL byte in the leading window is the cheap, reliable text/binary split; decoding would
    # reject valid UTF-8 continuations that happen to be split at the window boundary.
    record.update(category=record["categories"][0],
                  content="text" if b"\0" not in head else "binary",
                  sha256=digest.hexdigest())
    return record


def _head_commit(root: Path) -> str:
    """Return HEAD, or "unborn" for a repository that has an index but no commit yet."""
    result = subprocess.run(["git", "rev-parse", "--verify", "HEAD"], cwd=str(root),
                            text=True, capture_output=True)
    if result.returncode != 0:
        return "unborn"
    return result.stdout.strip()


def build_inventory(root: Path) -> dict:
    entries = [describe(root, path, mode) for path, mode in tracked_entries(root)]
    counts: dict[str, int] = {}
    for entry in entries:
        for category in entry["categories"]:
            counts[category] = counts.get(category, 0) + 1
    kinds: dict[str, int] = {}
    for entry in entries:
        kinds[entry["kind"]] = kinds.get(entry["kind"], 0) + 1
    return {
        "schema": "tracked-entry-inventory-v1",
        "commit": _head_commit(root),
        "counts": {"paths": len(entries), "byCategory": dict(sorted(counts.items())),
                   "byKind": dict(sorted(kinds.items()))},
        "entries": entries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="checkout to inventory")
    parser.add_argument("--output", help="write JSON here instead of stdout")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    if not (root / ".git").exists():
        parser.error(f"{root} is not a git checkout")
    inventory = build_inventory(root)
    text = json.dumps(inventory, indent=1, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
        print(f"tracked paths: {inventory['counts']['paths']} -> {args.output}")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
