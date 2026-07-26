#!/usr/bin/env python3
"""Create an immutable, content-addressed Spec 112 evidence candidate."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


SCHEMA_VERSION = "spec112-candidate-v1"
NDN_SVS_REVIEW_REF = "refs/heads/review/ndn-svs-convergence"
NDN_SVS_BACKUP_REF = "refs/heads/backup/experimental-before-convergence-20260715"
NDN_SVS_ORIGIN_MASTER_REF = "refs/remotes/origin/master"
DECLARED_MININDN_CELLS = (
    {
        "cellId": "boundary-async-normal",
        "mode": "normal",
        "svsPublish": "async",
        "sizes": "64,4000,5000,6500,8000,16000",
        "faultProfile": "none",
        "timeoutMs": 4000,
    },
    {
        "cellId": "boundary-async-targeted",
        "mode": "targeted",
        "targetedApi": "sync",
        "svsPublish": "async",
        "sizes": "64,4000,5000,6500,8000,16000",
        "faultProfile": "none",
        "timeoutMs": 4000,
    },
    {
        "cellId": "boundary-sync-normal",
        "mode": "normal",
        "svsPublish": "sync",
        "sizes": "64,4000,5000,6500,8000,16000",
        "faultProfile": "none",
        "timeoutMs": 4000,
    },
    {
        "cellId": "boundary-sync-targeted",
        "mode": "targeted",
        "targetedApi": "sync",
        "svsPublish": "sync",
        "sizes": "64,4000,5000,6500,8000,16000",
        "faultProfile": "none",
        "timeoutMs": 4000,
    },
    {
        "cellId": "burst-async-normal",
        "mode": "normal",
        "svsPublish": "async",
        "sizes": "8000x80,64x10,4000x12",
        "faultProfile": "none",
        "timeoutMs": 4000,
    },
    {
        "cellId": "targeted-degraded-timeout",
        "mode": "targeted",
        "targetedApi": "sync",
        "svsPublish": "async",
        "sizes": "64",
        "faultProfile": "degraded-provider-after-targeted-bootstrap",
        "timeoutMs": 1000,
    },
    {
        "cellId": "targeted-degraded-timeout-async",
        "mode": "targeted",
        "targetedApi": "async",
        "svsPublish": "async",
        "sizes": "64",
        "faultProfile": "degraded-provider-after-targeted-bootstrap",
        "timeoutMs": 1000,
    },
    {
        "cellId": "rollback-v2-boundary",
        "mode": "normal",
        "svsPublish": "async",
        "sizes": "64,6500",
        "faultProfile": "none",
        "timeoutMs": 4000,
        "protocolVersion": 2,
    },
)
GENERATED_COMPONENTS = {
    ".codegraph",
    ".git",
    ".planning",
    ".cache",
    ".pytest_cache",
    "__pycache__",
    "build",
    "build-tests",
    "results",
}


class CandidateError(RuntimeError):
    pass


def _run(command: List[str], *, cwd: Optional[Path] = None) -> bytes:
    try:
        return subprocess.run(
            command,
            cwd=str(cwd) if cwd else None,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = ""
        if isinstance(exc, subprocess.CalledProcessError):
            detail = exc.stderr.decode("utf-8", errors="replace").strip()
        raise CandidateError(f"command failed: {' '.join(command)}{': ' + detail if detail else ''}") from exc


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _file_record(path: Path, display_path: str) -> Dict[str, Any]:
    if not path.is_file():
        return {"path": display_path, "state": "missing"}
    stat = path.stat()
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return {
        "path": display_path,
        "state": "present",
        "size": stat.st_size,
        "sha256": digest.hexdigest(),
    }


def _is_generated(relative: Path) -> bool:
    return any(component in GENERATED_COMPONENTS for component in relative.parts)


def _untracked_inputs(repository: Path) -> List[Dict[str, Any]]:
    # Candidate identity must not depend on the invoking account's global Git
    # excludes (the campaign is created as the user and executed through sudo).
    # Repository .gitignore and .git/info/exclude still apply.
    raw = _run([
        "git", "-c", "core.excludesFile=/dev/null",
        "ls-files", "--others", "--exclude-standard", "-z",
    ], cwd=repository)
    records = []
    for encoded in raw.split(b"\0"):
        if not encoded:
            continue
        relative = Path(encoded.decode("utf-8", errors="surrogateescape"))
        if _is_generated(relative):
            continue
        candidate = repository / relative
        if candidate.is_file():
            records.append(_file_record(candidate, relative.as_posix()))
    return sorted(records, key=lambda item: item["path"])


def _repository_record(name: str, path: Path) -> Dict[str, Any]:
    if not (path / ".git").exists():
        raise CandidateError(f"required Git repository is missing: {path}")
    branch = _run(["git", "symbolic-ref", "--quiet", "--short", "HEAD"], cwd=path).decode().strip()
    head = _run(["git", "rev-parse", "HEAD"], cwd=path).decode().strip()
    binary_diff = _run(["git", "diff", "--binary", "HEAD", "--"], cwd=path)
    untracked = _untracked_inputs(path)
    return {
        "name": name,
        "path": str(path.resolve()),
        "branch": branch,
        "head": head,
        "trackedBinaryDiffSha256": _sha256_bytes(binary_diff),
        "trackedBinaryDiffBytes": len(binary_diff),
        "untrackedInputs": untracked,
        "untrackedInputsDigest": _sha256_bytes(
            json.dumps(untracked, sort_keys=True, separators=(",", ":")).encode("utf-8")
        ),
    }


def _git_ref_record(repository: Path, ref: str) -> Dict[str, Any]:
    try:
        oid = _run(["git", "rev-parse", "--verify", ref], cwd=repository).decode().strip()
    except CandidateError:
        return {"ref": ref, "state": "missing"}
    return {"ref": ref, "state": "present", "oid": oid}


def _ndn_svs_topology(repository: Path) -> Dict[str, Any]:
    origin_master = _git_ref_record(repository, NDN_SVS_ORIGIN_MASTER_REF)
    review_head = _git_ref_record(repository, NDN_SVS_REVIEW_REF)
    backup = _git_ref_record(repository, NDN_SVS_BACKUP_REF)
    required = {
        "origin/master": origin_master,
        "validated review head": review_head,
        "permanent backup": backup,
    }
    missing = [label for label, record in required.items() if record["state"] != "present"]
    if missing:
        raise CandidateError("required NDN-SVS topology ref is missing: " + ", ".join(missing))
    try:
        origin_url = _run(["git", "remote", "get-url", "origin"], cwd=repository).decode().strip()
    except CandidateError:
        origin_url = "unavailable"
    return {
        "originUrl": origin_url,
        "originMaster": origin_master,
        "validatedReviewHead": review_head,
        "permanentBackup": backup,
        "creationLabels": {
            "master": _git_ref_record(repository, "refs/heads/master"),
            "Experimental": _git_ref_record(repository, "refs/heads/Experimental"),
        },
        # These are the only labels allowed to move after the six cells pass.
        # The OIDs are derived from stable refs so the validated source identity
        # remains explicit before the final local branch rename.
        "expectedFinal": {
            "masterRef": "refs/heads/master",
            "masterOid": origin_master["oid"],
            "experimentalRef": "refs/heads/Experimental",
            "experimentalOid": review_head["oid"],
            "backupRef": NDN_SVS_BACKUP_REF,
            "backupOid": backup["oid"],
            "remoteMutationAuthorized": False,
        },
    }


def _campaign_inputs(repo_root: Path) -> List[Dict[str, Any]]:
    paths = (
        "Experiments/spec112_candidate_manifest.py",
        "Experiments/spec112_segmented_campaign.py",
        "Experiments/NDNSF_Segmented_Response_Minindn.py",
        "examples/python/segmented_response_provider.py",
        "examples/python/segmented_response_user.py",
        "examples/python/spec112_segmented_common.py",
        "Experiments/Topology/AI_Lab.conf",
    )
    return [_file_record(repo_root / relative, relative) for relative in paths]


def _ndn_svs_installation(repo_root: Path) -> Dict[str, Any]:
    source_root = repo_root.parent / "ndn-svs"
    header_names = ("core.hpp", "svspubsub.hpp", "fetcher.hpp", "store.hpp")
    header_pairs = []
    mismatches = []
    for name in header_names:
        workspace = _file_record(source_root / "ndn-svs" / name, f"ndn-svs/ndn-svs/{name}")
        installed = _file_record(Path("/usr/local/include/ndn-svs") / name,
                                 f"/usr/local/include/ndn-svs/{name}")
        matches = (
            workspace.get("state") == "present"
            and installed.get("state") == "present"
            and workspace.get("sha256") == installed.get("sha256")
        )
        if workspace.get("state") == "present" and not matches:
            mismatches.append(name)
        header_pairs.append({"name": name, "workspace": workspace,
                             "installed": installed, "matches": matches})

    workspace_library = _file_record(source_root / "build/libndn-svs.so",
                                     "ndn-svs/build/libndn-svs.so")
    installed_library = _file_record(Path("/usr/local/lib/libndn-svs.so"),
                                     "/usr/local/lib/libndn-svs.so")
    library_matches = (
        workspace_library.get("state") == "present"
        and installed_library.get("state") == "present"
        and workspace_library.get("sha256") == installed_library.get("sha256")
    )
    if workspace_library.get("state") == "present" and not library_matches:
        mismatches.append("libndn-svs.so")
    if mismatches:
        raise CandidateError(
            "installed NDN-SVS dependency differs from validated workspace: "
            + ", ".join(mismatches)
        )
    return {
        "headerPairs": header_pairs,
        "libraryPair": {
            "workspace": workspace_library,
            "installed": installed_library,
            "matches": library_matches,
        },
        "pkgConfigCflags": _version(["pkg-config", "--cflags", "libndn-svs"]),
        "pkgConfigLibraries": _version(["pkg-config", "--libs", "libndn-svs"]),
        "verified": bool(header_pairs) and all(pair["matches"] for pair in header_pairs)
                    and library_matches,
    }


def _version(command: List[str]) -> str:
    try:
        output = _run(command).decode("utf-8", errors="replace").strip()
    except CandidateError as exc:
        return f"unavailable: {exc}"
    return output


def _binary_records(repo_root: Path) -> List[Dict[str, Any]]:
    parent = repo_root.parent
    paths = [
        (repo_root / "build/unit-tests", "ndn-service-framework/build/unit-tests"),
        (parent / "ndn-svs/build/unit-tests", "ndn-svs/build/unit-tests"),
        (parent / "ndn-svs/build/libndn-svs.so", "ndn-svs/build/libndn-svs.so"),
        (parent / "NAC-ABE/build/tests/unit-tests", "NAC-ABE/build/tests/unit-tests"),
        (parent / "NAC-ABE/build-tests/tests/unit-tests", "NAC-ABE/build-tests/tests/unit-tests"),
        (parent / "NAC-ABE/build/unit-tests", "NAC-ABE/build/unit-tests"),
        (parent / "NAC-ABE/build/libnac-abe.so", "NAC-ABE/build/libnac-abe.so"),
        (Path("/usr/local/lib/libndn-svs.so.0.1.0"), "/usr/local/lib/libndn-svs.so.0.1.0"),
        (Path("/usr/local/lib/libndn-cxx.so.0.9.0"), "/usr/local/lib/libndn-cxx.so.0.9.0"),
        (Path("/usr/local/lib/libnac-abe.so"), "/usr/local/lib/libnac-abe.so"),
        (Path("/usr/local/lib/libopenabe.so"), "/usr/local/lib/libopenabe.so"),
    ]
    python_extensions = sorted(
        (repo_root / "pythonWrapper/ndnsf").glob("_ndnsf*.so")
    )
    if python_extensions:
        paths.extend(
            (path, f"ndn-service-framework/pythonWrapper/ndnsf/{path.name}")
            for path in python_extensions
        )
    else:
        paths.append((
            repo_root / "pythonWrapper/ndnsf/_ndnsf.so",
            "ndn-service-framework/pythonWrapper/ndnsf/_ndnsf.so",
        ))
    return [_file_record(path, display) for path, display in paths]


def _identity(repo_root: Path) -> Dict[str, Any]:
    ndn_svs = repo_root.parent / "ndn-svs"
    repositories = [
        _repository_record("ndn-service-framework", repo_root),
        _repository_record("ndn-svs", ndn_svs),
        _repository_record("NAC-ABE", repo_root.parent / "NAC-ABE"),
    ]
    return {
        "schemaVersion": SCHEMA_VERSION,
        "repositories": repositories,
        "ndnSvsTopology": _ndn_svs_topology(ndn_svs),
        "binaries": _binary_records(repo_root),
        "campaignInputs": _campaign_inputs(repo_root),
        "declaredMiniNdnCells": list(DECLARED_MININDN_CELLS),
        "dependencyInstallation": {"ndnSvs": _ndn_svs_installation(repo_root)},
        "toolchain": {
            "platform": platform.platform(),
            "python": sys.version,
            "cxx": _version(["c++", "--version"]).splitlines()[0],
            "ndnCxx": _version(["pkg-config", "--modversion", "libndn-cxx"]),
            "boost": _version(["dpkg-query", "-W", "-f=${Version}", "libboost-dev"]),
        },
    }


def _canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def create_manifest(result_root: Path, repo_root: Path) -> str:
    repo_root = repo_root.resolve()
    identity = _identity(repo_root)
    identity_sha = _sha256_bytes(_canonical_json(identity))
    candidate_id = f"spec112-{identity_sha[:20]}"
    manifest = {
        "schemaVersion": SCHEMA_VERSION,
        "candidateId": candidate_id,
        "identitySha256": identity_sha,
        "createdAt": datetime.now(timezone.utc).isoformat(),
        "identity": identity,
    }

    result_root.mkdir(parents=True, exist_ok=True)
    candidate_dir = result_root / candidate_id
    manifest_path = candidate_dir / "candidate-manifest.json"
    try:
        candidate_dir.mkdir()
    except FileExistsError as exc:
        if manifest_path.is_file():
            try:
                existing = json.loads(manifest_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                existing = None
            if not isinstance(existing, dict) or existing.get("identitySha256") != identity_sha:
                raise CandidateError(f"candidate identity collision at {candidate_dir}") from exc
        raise CandidateError(f"candidate already exists: {candidate_dir}") from exc

    temporary_path: Optional[Path] = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(prefix=".candidate-manifest-", dir=str(candidate_dir))
        temporary_path = Path(temporary_name)
        with os.fdopen(descriptor, "wb") as destination:
            destination.write(json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=False).encode("utf-8"))
            destination.write(b"\n")
            destination.flush()
            os.fsync(destination.fileno())
        os.replace(str(temporary_path), str(manifest_path))
        temporary_path = None
        directory_fd = os.open(str(candidate_dir), os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except Exception:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        manifest_path.unlink(missing_ok=True)
        try:
            candidate_dir.rmdir()
        except OSError:
            pass
        raise
    return candidate_id


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    create = subparsers.add_parser("create", help="create one immutable candidate manifest")
    create.add_argument("--result-root", type=Path, required=True)
    create.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help=argparse.SUPPRESS,
    )
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.command == "create":
            candidate_id = create_manifest(arguments.result_root, arguments.repo_root)
        else:
            raise CandidateError(f"unsupported command: {arguments.command}")
    except CandidateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(candidate_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
