"""Render bounded Spec 110 rootless OCI build jobs without submitting them."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shlex
from typing import Any


class RootlessBuildError(ValueError):
    """Unsafe or incomplete rootless build definition."""


SAFE = re.compile(r"^[A-Za-z0-9._+-]+$")
DIGEST_REF = re.compile(r"^[^\s]+@sha256:[a-f0-9]{64}$")
WALL_TIME = re.compile(r"^[0-9]{2}:[0-9]{2}:[0-9]{2}$")
SAFE_PATH = re.compile(r"^/[A-Za-z0-9._/+:-]+$")

DEFAULT_PROBE_BASE = (
    "docker.io/library/alpine@sha256:"
    "c64c687cbea9300178b30c95835354e34c4e4febc4badfe27102879de0483b5e"
)
DEFAULT_GPU_BUILD_BASE = (
    "nvidia/cuda@sha256:"
    "0a1cb6e7bd047a1067efe14efdf0276352d5ca643dfd77963dab1a4f05a003a4"
)
DEFAULT_GPU_RUNTIME_BASE = (
    "nvidia/cuda@sha256:"
    "0bb88834d973ca1b450fcc2a05333c6fe45510bee289912a5391274c351c4a4d"
)


def _digest(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _safe_token(value: str, label: str) -> str:
    if SAFE.fullmatch(value) is None:
        raise RootlessBuildError(f"ROOTLESS_BUILD_{label}_INVALID")
    return value


def _absolute(path: Path | str, label: str) -> Path:
    value = Path(path)
    if not value.is_absolute() or SAFE_PATH.fullmatch(str(value)) is None:
        raise RootlessBuildError(f"ROOTLESS_BUILD_{label}_INVALID")
    return value


def _under(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def render_rootless_build_job(
    *,
    source_root: Path | str,
    project_root: Path | str,
    output_path: Path | str,
    release_id: str,
    mode: str = "diagnostic",
    partition: str = "bigTiger",
    account: str = "",
    qos: str = "normal",
    wall_time: str = "00:05:00",
    cpus: int = 4,
    memory: str = "8G",
    probe_base: str = DEFAULT_PROBE_BASE,
    gpu_build_base: str = DEFAULT_GPU_BUILD_BASE,
    gpu_runtime_base: str = DEFAULT_GPU_RUNTIME_BASE,
    allow_test_root: bool = False,
) -> dict[str, Any]:
    """Render one immutable CPU build job and a checksum-bound review record."""
    source = _absolute(source_root, "SOURCE_ROOT")
    project = _absolute(project_root, "PROJECT_ROOT")
    output = _absolute(output_path, "OUTPUT")
    if mode not in {"diagnostic", "full"}:
        raise RootlessBuildError("ROOTLESS_BUILD_MODE_INVALID")
    _safe_token(release_id, "RELEASE_ID")
    for label, value in (("PARTITION", partition), ("QOS", qos), ("MEMORY", memory)):
        _safe_token(value, label)
    if account:
        _safe_token(account, "ACCOUNT")
    if WALL_TIME.fullmatch(wall_time) is None or cpus < 1 or cpus > 64:
        raise RootlessBuildError("ROOTLESS_BUILD_RESOURCES_INVALID")
    for label, value in (
        ("PROBE_BASE", probe_base),
        ("GPU_BUILD_BASE", gpu_build_base),
        ("GPU_RUNTIME_BASE", gpu_runtime_base),
    ):
        if DIGEST_REF.fullmatch(value) is None:
            raise RootlessBuildError(f"ROOTLESS_BUILD_{label}_NOT_PINNED")
    expected_project = Path("/project") / os.environ.get("USER", "") / "ndnsf-di"
    test_allowed = allow_test_root or os.environ.get("NDNSF_SPEC110_ALLOW_TEST_ROOT") == "1"
    if not test_allowed and project != expected_project:
        raise RootlessBuildError("ROOTLESS_BUILD_PROJECT_ROOT_INVALID")
    if not test_allowed and not _under(source, project / "source"):
        raise RootlessBuildError("ROOTLESS_BUILD_SOURCE_OUTSIDE_PROJECT")
    if not test_allowed and not _under(output, project / "campaigns" / "spec110"):
        raise RootlessBuildError("ROOTLESS_BUILD_OUTPUT_OUTSIDE_CAMPAIGN")
    template = (
        Path(__file__).resolve().parents[2]
        / "packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/rootless-build.sbatch.in"
    )
    builder = (
        source / "workspace/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/rootless-build.sh"
        if mode == "full"
        else source / "packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/rootless-build.sh"
    )
    evidence = project / "campaigns/spec110/rootless-build" / release_id
    values = {
        "JOB_NAME": f"s110-build-{release_id}"[:64],
        "PARTITION": partition,
        "ACCOUNT_DIRECTIVE": f"#SBATCH --account={account}" if account else "",
        "QOS": qos,
        "WALL_TIME": wall_time,
        "CPUS": str(cpus),
        "MEMORY": memory,
        "OUTPUT_LOG": str(evidence / "slurm-%j.out"),
        "BUILDER": shlex.quote(str(builder)),
        "MODE": mode,
        "SOURCE_ROOT": shlex.quote(str(source)),
        "PROJECT_ROOT": shlex.quote(str(project)),
        "RELEASE_ID": release_id,
        "EVIDENCE_DIR": shlex.quote(str(evidence)),
        "PROBE_BASE": shlex.quote(probe_base),
        "GPU_BUILD_BASE": shlex.quote(gpu_build_base),
        "GPU_RUNTIME_BASE": shlex.quote(gpu_runtime_base),
    }
    text = template.read_text(encoding="utf-8")
    for key, value in values.items():
        text = text.replace(f"@@{key}@@", value)
    if "@@" in text:
        raise RootlessBuildError("ROOTLESS_BUILD_TEMPLATE_UNRESOLVED")
    output.parent.mkdir(parents=True, exist_ok=True)
    evidence.mkdir(parents=True, exist_ok=True)
    try:
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o750)
    except FileExistsError as exc:
        raise RootlessBuildError("ROOTLESS_BUILD_RENDER_EXISTS") from exc
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write(text)
        stream.flush()
        os.fsync(stream.fileno())
    record = {
        "schemaVersion": "spec110-rootless-build-render-v1",
        "status": "RENDERED_NOT_SUBMITTED",
        "diagnosticOnly": mode == "diagnostic",
        "releaseId": release_id,
        "mode": mode,
        "sourceRoot": str(source),
        "projectRoot": str(project),
        "scriptPath": str(output),
        "scriptSha256": _digest(output),
        "resources": {
            "partition": partition,
            "account": account or None,
            "qos": qos,
            "wallTime": wall_time,
            "cpus": cpus,
            "memory": memory,
            "gres": None,
        },
    }
    record_path = output.with_suffix(output.suffix + ".render.json")
    with record_path.open("x", encoding="utf-8") as stream:
        json.dump(record, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    return record


__all__ = [
    "DEFAULT_GPU_BUILD_BASE",
    "DEFAULT_GPU_RUNTIME_BASE",
    "DEFAULT_PROBE_BASE",
    "RootlessBuildError",
    "render_rootless_build_job",
]
