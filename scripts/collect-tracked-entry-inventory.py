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
import re
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


def tracked_entries(root: Path) -> list[tuple[str, int, str]]:
    """Return (path, mode, blob oid) for every tracked path, from the index.

    The index, not the working tree, decides both membership and content: a tracked regular file that the
    working tree replaced with a symlink must not be read through that link, and a deleted path is still an
    index member.
    """
    out = subprocess.check_output(["git", "ls-files", "--stage", "-z"], cwd=str(root))
    entries: list[tuple[str, int, str]] = []
    for record in out.split(b"\0"):
        if not record:
            continue
        meta, _, path = record.partition(b"\t")
        parts = meta.split(b" ")
        mode = int(parts[0], 8)
        oid = parts[1].decode("ascii") if len(parts) > 1 else ""
        entries.append((path.decode("utf-8", "surrogateescape"), mode, oid))
    return sorted(entries)


def index_blobs(root: Path, oids: list[str]) -> dict[str, bytes]:
    """Read blob contents from the object database in one batch, never through the working tree."""
    wanted = sorted({oid for oid in oids if oid and set(oid) != {"0"}})
    if not wanted:
        return {}
    stream = subprocess.run(["git", "cat-file", "--batch"], cwd=str(root),
                            input="\n".join(wanted).encode() + b"\n",
                            stdout=subprocess.PIPE, check=True).stdout
    blobs: dict[str, bytes] = {}
    position = 0
    while position < len(stream):
        newline = stream.index(b"\n", position)
        header = stream[position:newline].split()
        position = newline + 1
        if len(header) < 3 or header[1] == b"missing":
            continue
        size = int(header[2])
        blobs[header[0].decode("ascii")] = stream[position:position + size]
        position += size + 1
    return blobs


def categorise(path: str) -> list[str]:
    found = []
    for name, needles in CATEGORIES:
        if any(needle in path for needle in needles):
            found.append(name)
    return found or ["other"]


WORKTREE_TYPE = {0o100644: "file", 0o100755: "file", 0o120000: "symlink", 0o160000: "gitlink"}


def describe(root: Path, path: str, mode: int, blob: bytes | None) -> dict:
    """Describe one tracked path from index metadata and index content.

    Content comes from the object database, so a tracked regular file that the working tree replaced with a
    symlink cannot leak an external file. The working tree is consulted only to record drift.
    """
    absolute = root / path
    suffix = Path(path).suffix.lower()
    lowered = path.lower()
    record = {"path": path, "categories": categorise(path)}
    expected = WORKTREE_TYPE.get(mode, "other")

    # A dangling symlink is present as a path even though Path.exists() follows the link and reports False.
    present = absolute.is_symlink() or absolute.exists()
    actual = None
    if present:
        try:
            actual = "symlink" if absolute.is_symlink() else (
                "file" if absolute.is_file() else "dir")
        except OSError:
            actual = None
    if not present:
        record["worktree"] = "missing"
    elif expected == "file" and actual == "file":
        record["worktree"] = "file"
    elif expected == "symlink" and actual == "symlink":
        record["worktree"] = "symlink"
    elif expected == "gitlink":
        record["worktree"] = actual or "absent"
    else:
        record["worktree"] = f"type-changed:{actual or 'unknown'}"

    if mode == 0o120000:
        target = blob.decode("utf-8", "surrogateescape") if blob is not None else ""
        record.update(kind="symlink", target=target, absolute_target=target.startswith("/"), size=0)
        return record
    if mode == 0o160000:
        record.update(kind="gitlink", size=0)
        return record

    record.update(kind="file", size=len(blob) if blob is not None else 0)
    if any(secret in lowered for secret in PRIVATE_SUFFIXES):
        record.update(content="private-not-read")
        return record
    if suffix in BULK_SUFFIXES:
        record.update(content="bulk-not-read")
        return record
    if blob is None:
        record.update(content="blob-unavailable")
        return record
    # A NUL byte in the leading window is the cheap, reliable text/binary split; decoding would
    # reject valid UTF-8 continuations that happen to be split at the window boundary.
    record.update(category=record["categories"][0],
                  content="text" if b"\0" not in blob[:TEXT_BYTES] else "binary",
                  sha256=hashlib.sha256(blob).hexdigest())
    return record


def _head_commit(root: Path) -> str:
    """Return HEAD, or "unborn" for a repository that has an index but no commit yet."""
    result = subprocess.run(["git", "rev-parse", "--verify", "HEAD"], cwd=str(root),
                            text=True, capture_output=True)
    if result.returncode != 0:
        return "unborn"
    return result.stdout.strip()


def build_inventory(root: Path) -> dict:
    tracked = tracked_entries(root)
    blobs = index_blobs(root, [oid for _, _, oid in tracked])
    entries = [describe(root, path, mode, blobs.get(oid)) for path, mode, oid in tracked]
    counts: dict[str, int] = {}
    for entry in entries:
        for category in entry["categories"]:
            counts[category] = counts.get(category, 0) + 1
    drift: dict[str, int] = {}
    for entry in entries:
        state = entry.get("worktree", "")
        if state and state != entry.get("kind"):
            drift[state] = drift.get(state, 0) + 1
    kinds: dict[str, int] = {}
    for entry in entries:
        kinds[entry["kind"]] = kinds.get(entry["kind"], 0) + 1
    return {
        "schema": "tracked-entry-inventory-v1",
        "commit": _head_commit(root),
        "counts": {"paths": len(entries), "byCategory": dict(sorted(counts.items())),
                   "byKind": dict(sorted(kinds.items())),
                   "worktreeDrift": dict(sorted(drift.items()))},
        "entries": entries,
    }


# Files whose content declares what the project actually runs. A path named in one of these is treated as
# a live entry; a tracked path named nowhere is a candidate for migration or retirement under CD-05/T009.
CONSUMER_FILES = (
    "install_ndnsf_stack.sh",
    "Experiments/profiles/index.json",
    "tests/check-profiles.json",
    "tests/run-checks.py",
)
CONSUMER_GLOBS = (".github/workflows/*.yml", ".github/workflows/*.yaml")

PATH_TOKEN = re.compile(
    r"(?<![\w/-])((?:[\w.@+-]+/)*[\w.@+-]+\.(?:py|sh|cpp|hpp|cc|h|json|conf|md|toml|yml|yaml|txt|def|in|xml|xsd))"
)


def consumer_declarations(root: Path) -> dict[str, set[str]]:
    """Map each consumer file to the repository-relative paths it names."""
    declared: dict[str, set[str]] = {}
    files = [root / name for name in CONSUMER_FILES]
    for pattern in CONSUMER_GLOBS:
        files.extend(sorted(root.glob(pattern)))
    for path in files:
        if not path.is_file():
            continue
        text = path.read_text(errors="replace")
        found = set()
        for token in PATH_TOKEN.findall(text):
            cleaned = token.lstrip("./").replace("\\", "/")
            found.add(cleaned)
        declared[path.relative_to(root).as_posix()] = found
    return declared


def cross_check(root: Path, inventory: dict) -> dict:
    """Report registrations that point at nothing, and tracked paths nothing declares."""
    tracked = {entry["path"] for entry in inventory["entries"]}
    top_level = {path.split("/", 1)[0] for path in tracked if "/" in path}
    top_level |= {path for path in tracked if "/" not in path}
    basenames: dict[str, set[str]] = {}
    for path in tracked:
        basenames.setdefault(path.rsplit("/", 1)[-1], set()).add(path)

    declared = consumer_declarations(root)
    dangling: list[dict] = []
    ambiguous: list[dict] = []
    referenced: set[str] = set()
    for consumer, names in sorted(declared.items()):
        for name in sorted(names):
            matches = {name} & tracked
            if not matches and name in basenames:
                candidates = basenames[name]
                # A bare basename that maps to several tracked paths identifies none of them; recording all
                # of them as referenced would overstate coverage.
                matches = candidates if len(candidates) == 1 else set()
                if len(candidates) > 1:
                    ambiguous.append({"consumer": consumer, "reference": name,
                                      "candidates": sorted(candidates)})
            if matches:
                referenced |= matches
            elif "/" in name and name.split("/", 1)[0] in top_level:
                # A registration must start in a directory this repository actually tracks. Tokens such as
                # RESULT_ROOT/build-context.txt are run outputs or shell variables, not registrations.
                dangling.append({"consumer": consumer, "reference": name})

    unclaimed_spec = sorted(
        path for path in tracked
        if re.search(r"spec[-_]?\d{3}", path, re.IGNORECASE) and path not in referenced
    )
    return {
        "consumers": {name: len(names) for name, names in sorted(declared.items())},
        "dangling": sorted(dangling, key=lambda item: (item["consumer"], item["reference"])),
        "ambiguous": sorted(ambiguous, key=lambda item: (item["consumer"], item["reference"])),
        "trackedReferenced": len(referenced & tracked),
        "notNamedByConsumers": unclaimed_spec,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="checkout to inventory")
    parser.add_argument("--output", help="write JSON here instead of stdout")
    parser.add_argument("--cross-check", action="store_true",
                        help="also report dangling declarations and unclaimed tracked paths")
    args = parser.parse_args()
    root = Path(args.root).resolve()
    if not (root / ".git").exists():
        parser.error(f"{root} is not a git checkout")
    inventory = build_inventory(root)
    if args.cross_check:
        inventory["crossCheck"] = cross_check(root, inventory)
    text = json.dumps(inventory, indent=1, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
        print(f"tracked paths: {inventory['counts']['paths']} -> {args.output}")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
