#!/usr/bin/env python3
"""Local N/N+1 activation and rollback drill for Spec 107."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
from typing import Any


CANDIDATE_RE = re.compile(r"^spec107-c1(?:-[0-9a-f]{12}){6}$")
DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")


class OperationsError(RuntimeError):
    pass


def _fail(code: str, detail: str = "") -> None:
    raise OperationsError(code + (f":{detail}" if detail else ""))


def _tree_digest(root: Path) -> str:
    digest = hashlib.sha256()
    if not root.exists():
        return "sha256:" + digest.hexdigest()
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix().encode()
        digest.update(len(relative).to_bytes(8, "big"))
        digest.update(relative)
        if path.is_file():
            data = path.read_bytes()
            digest.update(len(data).to_bytes(8, "big"))
            digest.update(data)
    return "sha256:" + digest.hexdigest()


class LocalReleaseOperations:
    def __init__(self, *, root: Path | str) -> None:
        self.root = Path(root).resolve()
        self.activation = self.root / "opt/ndnsf-di"
        self.repo = self.root / "var/lib/ndnsf-repo"
        self.cache = self.root / "var/cache/ndnsf-di"
        self.activation.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def _manifest(release: Path) -> dict[str, Any]:
        try:
            value = json.loads((release / "release.json").read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            _fail("OPERATIONS_RELEASE_INVALID", str(exc))
        if (not isinstance(value, dict) or
                not isinstance(value.get("releaseId"), str) or
                CANDIDATE_RE.fullmatch(str(value.get("candidateId"))) is None or
                DIGEST_RE.fullmatch(str(value.get("planDigest"))) is None):
            _fail("OPERATIONS_RELEASE_INVALID")
        return value

    def _replace_link(self, name: str, target: Path) -> None:
        link = self.activation / name
        temporary = self.activation / f".{name}.new"
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        temporary.symlink_to(target)
        os.replace(temporary, link)

    def activate(self, release_root: Path | str) -> dict[str, object]:
        release = Path(release_root).resolve()
        manifest = self._manifest(release)
        repo_before = _tree_digest(self.repo)
        current = self.activation / "current"
        previous_target = current.resolve() if current.is_symlink() else None
        if previous_target is not None and previous_target != release:
            self._replace_link("previous", previous_target)

        cache_decision = "REUSED_COMPATIBLE"
        binding_path = self.cache / "binding.json"
        if binding_path.is_file():
            try:
                binding = json.loads(binding_path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                binding = {}
            if binding.get("planDigest") != manifest["planDigest"]:
                shutil.rmtree(self.cache)
                self.cache.mkdir(parents=True)
                cache_decision = "DISCARDED_INCOMPATIBLE"
        else:
            self.cache.mkdir(parents=True, exist_ok=True)
            cache_decision = "EMPTY"
        binding_path.write_text(json.dumps({
            "candidateId": manifest["candidateId"],
            "planDigest": manifest["planDigest"],
            "releaseId": manifest["releaseId"],
        }, sort_keys=True) + "\n", encoding="utf-8")
        self._replace_link("current", release)
        repo_after = _tree_digest(self.repo)
        if repo_before != repo_after:
            _fail("OPERATIONS_REPO_MUTATED")
        return {
            "activeReleaseId": manifest["releaseId"],
            "candidateId": manifest["candidateId"],
            "planDigest": manifest["planDigest"],
            "cacheDecision": cache_decision,
            "repoDigest": repo_after,
            "repoPreserved": True,
            "physicalProductionDeferred": True,
        }

    def rollback(self) -> dict[str, object]:
        current = self.activation / "current"
        previous = self.activation / "previous"
        if not current.is_symlink() or not previous.is_symlink():
            _fail("OPERATIONS_ROLLBACK_UNAVAILABLE")
        repo_before = _tree_digest(self.repo)
        current_target = current.resolve()
        previous_target = previous.resolve()
        self._replace_link("current", previous_target)
        self._replace_link("previous", current_target)
        manifest = self._manifest(previous_target)
        repo_after = _tree_digest(self.repo)
        if repo_before != repo_after:
            _fail("OPERATIONS_REPO_MUTATED")
        return {
            "activeReleaseId": manifest["releaseId"],
            "candidateId": manifest["candidateId"],
            "planDigest": manifest["planDigest"],
            "repoDigest": repo_after,
            "repoPreserved": True,
            "physicalProductionDeferred": True,
        }


__all__ = ["LocalReleaseOperations", "OperationsError"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True)
    parser.add_argument("--release-n", required=True)
    parser.add_argument("--release-n1", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    output = Path(args.output)
    if output.exists():
        print(f"OPERATIONS_OUTPUT_EXISTS:{output}", file=sys.stderr)
        return 2
    drill = LocalReleaseOperations(root=args.root)
    try:
        activated_n = drill.activate(args.release_n)
        upgraded = drill.activate(args.release_n1)
        rolled_back = drill.rollback()
        verdict = "PASS"
        error = None
    except Exception as exc:
        activated_n = upgraded = rolled_back = None
        verdict = "BLOCK"
        error = str(exc)
    record = {
        "schema": "ndnsf-di-spec107-local-operations-v1",
        "activationN": activated_n, "upgradeN1": upgraded,
        "rollbackN": rolled_back, "verdict": verdict,
        "error": error, "physicalProductionDeferred": True,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(record, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(json.dumps(record, sort_keys=True))
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    import sys
    raise SystemExit(main())
