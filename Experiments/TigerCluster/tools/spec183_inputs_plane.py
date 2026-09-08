"""Spec183 inputs content plane: render/check from the real received inputs.

The inputs stage requires exactly four real files (yolo_profile.REQUIRED_FILES):
``sourceLock``, ``sourceSeal``, ``buildDefinition`` and ``baseSif``.  Their real
counterparts live in the ignored CAS under ``Experiments/TigerCluster/.cache``
and in the committed handoff lock:

    sourceLock        development-handoff.lock.json (committed, TigerCluster/)
    sourceSeal        .cache/handoff/development-20260906/source/source-seal.json
    buildDefinition   .cache/handoff/development-20260906/runtime.def.in
    baseSif           .cache/base-sif/spec180-runtime.sif

``render`` assembles one plane root (default
``Experiments/TigerCluster/.cache/planes/inputs``) that contains the four real
files and writes ``plane.json`` with exact path/bytes/sha256 rows.  Files are
hard-linked (never symlinked; check_plane rejects symlinks and the CAS is an
ignored read-only receipt area), so no large copy is needed.  An existing row
with identical bytes is kept; a changed file is reported and requires the old
row to be removed first (fail-closed, no silent overwrite of a receipt).
``check`` runs the production validator ``yolo_profile.check_plane`` and prints
its integrity report; ``VERIFIED`` here is content integrity only and never a
runtime qualification.

Rendering is deterministic: repeated renders on unchanged inputs produce the
same ``plane.json`` bytes, so the plane identity is reproducible.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from typing import Iterable

_REPO_ROOT = Path(__file__).resolve().parents[3]
# (plane row name, real file path relative to the repo root)
SOURCES = {
    "sourceLock": Path("Experiments/TigerCluster/development-handoff.lock.json"),
    "sourceSeal": Path("Experiments/TigerCluster/.cache/handoff/development-20260906/source/source-seal.json"),
    "buildDefinition": Path("Experiments/TigerCluster/.cache/handoff/development-20260906/runtime.def.in"),
    "baseSif": Path("Experiments/TigerCluster/.cache/base-sif/spec180-runtime.sif"),
}
DEFAULT_PLANE_REL = Path("Experiments/TigerCluster/.cache/planes/inputs")
PLANE_SCHEMA = "tiger-yolo-plane-v1"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        while True:
            chunk = stream.read(4 * 1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def source_files(path: Path, expected: set[str]) -> dict:
    """Read explicit artifact paths relative to their descriptor, not cwd."""
    value = json.loads(path.read_text())
    if (not isinstance(value, dict) or set(value) != expected
            or any(not isinstance(item, str) or not item for item in value.values())):
        raise ValueError('PLANE_SOURCE_FILES')
    return {name: path.absolute().parent/Path(item) for name, item in value.items()}


def render(root: Path, *, verify: bool = False, sources=None, layout=None) -> dict:
    """Assemble the four real files under ``root`` and write plane.json."""
    root = Path(root).resolve()
    if any(part.is_symlink() for part in (root, *root.parents)):
        raise RuntimeError("plane root lies below a symlink")
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    files: dict[str, dict] = {}
    selected = SOURCES if sources is None else sources
    if set(selected) != set(SOURCES) or layout not in (None, 'layered-v1'):
        raise ValueError('PLANE_SOURCE_FILES')
    for name, relative in selected.items():
        source = _REPO_ROOT / relative
        if not source.is_file() or source.is_symlink():
            raise RuntimeError(f"real input missing for {name}: {source}")
        target = root / relative.name
        if target.is_symlink():
            raise RuntimeError(f"plane file {name} is a symlink: {target}")
        try:
            os.link(source, target)
        except FileExistsError:
            pass  # already linked from a previous render
        except OSError as exc:
            raise RuntimeError(f"cannot link {name} into the plane root") from exc
        if not target.is_file():
            raise RuntimeError(f"plane file {name} was not linked: {target}")
        observed = _sha256(target)
        if observed != _sha256(source):
            raise RuntimeError(f"plane file {name} does not match its source "
                               f"({observed})")
        if not (target.stat().st_nlink > 1 or target == source):
            raise RuntimeError(f"plane file {name} is not the CAS file")
        files[name] = {"path": target.name, "bytes": target.stat().st_size,
                       "sha256": observed}
    document = {"schema": PLANE_SCHEMA, "stage": "inputs", "parentId": None,
                "files": files, "parameters": {"maxBuildJobs": 2}}
    if layout is not None:
        document['parameters']['layout'] = layout
    payload = json.dumps(document, sort_keys=True, separators=(",", ":")).encode()
    plane = root / "plane.json"
    if plane.exists() and plane.read_bytes() != payload:
        raise RuntimeError(f"{plane} already records different identities; "
                           "remove the stale plane root before re-rendering")
    plane.write_bytes(payload)
    return document


def check(root: Path) -> dict:
    """Run the production validator; integrity only, never qualification."""
    from runtime.yolo_profile import check_plane, ClosureError
    path = root / "plane.json"
    try:
        return check_plane(path, expected_stage="inputs")
    except ClosureError as exc:
        return {"status": "INVALID", "integrity": "REJECTED", "reason": str(exc)}


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("render", "check"):
        child = sub.add_parser(name, help="assemble / validate the inputs plane")
        child.add_argument("--root", default=None,
                           help="plane root, absolute or repo-root-relative")
        if name == 'render':
            child.add_argument('--source-files', type=Path,
                               help='JSON mapping of the four input names to explicit paths')
            child.add_argument('--layout', choices=('layered-v1',))
    args = parser.parse_args(argv)
    root = Path(args.root or str(_REPO_ROOT / DEFAULT_PLANE_REL))
    if not root.is_absolute():
        root = _REPO_ROOT / root
    if args.command == "render":
        selected = source_files(args.source_files, set(SOURCES)) if args.source_files else None
        document = render(root, sources=selected, layout=args.layout)
        print(json.dumps({"status": "RENDERED",
                          "id": _sha256(root / "plane.json"),
                          "files": {n: row["sha256"] for n, row in
                                    document["files"].items()}}, sort_keys=True))
        return 0
    report = check(root)
    print(json.dumps(report, sort_keys=True))
    return 0 if report.get("integrity") == "VERIFIED" else 2


if __name__ == "__main__":
    sys.exit(main())
