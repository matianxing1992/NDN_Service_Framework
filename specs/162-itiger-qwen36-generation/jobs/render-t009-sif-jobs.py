#!/usr/bin/env python3
"""Render candidate-bound T009 SIF jobs from a sealed Docker archive."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = ROOT.parents[2]
MATERIALIZE_TEMPLATE = ROOT / "materialize-qwen36-runtime-t009.sbatch.in"
SMOKE_TEMPLATE = ROOT / "operation-status-sif-smoke-t009.sbatch.in"
HARNESS_SOURCES = {
    "local-docker-operation-status-inner.sh": (
        PROJECT_ROOT
        / "specs/160-itiger-multinode-qwen-collaboration/jobs"
        / "local-docker-operation-status-inner.sh"
    ),
    "local-docker-operation-status-policy.yaml": (
        PROJECT_ROOT
        / "specs/160-itiger-multinode-qwen-collaboration/jobs"
        / "local-docker-operation-status-policy.yaml"
    ),
    "nfd.conf.in": ROOT / "nfd.conf.in",
}

EXPECTED_IMAGE_ID = (
    "sha256:4e2d716343293bd39c645c41dade984e"
    "a2fa15157d3c4680c8180eb15b903612"
)
ARCHIVE_PATH = (
    "/project/tma1/ndnsf-di/images/spec162-4e2d71634329/"
    "candidate-docker-archive.tar.gz"
)
ARCHIVE_SHA256 = (
    "1c0bc7fbe015caeb3bbf2be562883127"
    "c34124b98673dab743ef41835881bff6"
)
ARCHIVE_BYTES = 4_689_331_371
HARNESS_ROOT = (
    "/project/tma1/ndnsf-di/sources/"
    "spec162-t009-runtime-3260f13ca156"
)
ATTEMPT_SEQUENCE = 1
PREDECESSOR_MATERIALIZATION = None

DIGEST_RE = re.compile(r"^sha256:([0-9a-f]{64})$")
TOKEN_RE = re.compile(r"@[A-Z][A-Z0-9_]*@")


class RenderError(ValueError):
    """The supplied source evidence cannot identify the frozen candidate."""


@dataclass(frozen=True)
class CandidateSource:
    image_id: str
    archive_path: str
    archive_sha256: str
    archive_bytes: int
    harness_root: str
    attempt_sequence: int
    predecessor_materialization: str | None = None


DEFAULT_CANDIDATE = CandidateSource(
    image_id=EXPECTED_IMAGE_ID,
    archive_path=ARCHIVE_PATH,
    archive_sha256=ARCHIVE_SHA256,
    archive_bytes=ARCHIVE_BYTES,
    harness_root=HARNESS_ROOT,
    attempt_sequence=ATTEMPT_SEQUENCE,
    predecessor_materialization=PREDECESSOR_MATERIALIZATION,
)


def sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def require_digest(value: str, label: str) -> str:
    match = DIGEST_RE.fullmatch(value)
    if match is None:
        raise RenderError(f"{label}_INVALID")
    return match.group(1)


def expected_archive_manifest(
    candidate: CandidateSource = DEFAULT_CANDIDATE,
) -> dict:
    return {
        "schemaVersion": "spec162-sealed-runtime-source-v1",
        "sourceKind": "sealed-docker-archive",
        "localImageId": candidate.image_id,
        "archive": {
            "path": candidate.archive_path,
            "sha256": "sha256:" + candidate.archive_sha256,
            "bytes": candidate.archive_bytes,
        },
    }


def canonical_manifest_bytes(manifest: dict) -> bytes:
    return json.dumps(
        manifest, sort_keys=True, separators=(",", ":")
    ).encode()


def validate_manifest(
    raw: bytes,
    manifest: dict,
    candidate: CandidateSource = DEFAULT_CANDIDATE,
) -> dict[str, str]:
    expected = expected_archive_manifest(candidate)
    if manifest != expected:
        raise RenderError("ARCHIVE_MANIFEST_IDENTITY_MISMATCH")
    if raw != canonical_manifest_bytes(manifest):
        raise RenderError("ARCHIVE_MANIFEST_NOT_CANONICAL")
    digest = sha256_bytes(raw)
    digest_hex = require_digest(digest, "SOURCE_MANIFEST_DIGEST")
    reference = "sealed-docker-archive@" + digest
    prefix = digest_hex[:12]
    return {
        "SOURCE_REFERENCE": reference,
        "SOURCE_DIGEST": digest,
        "SOURCE_HEX": digest_hex,
        "SOURCE_PREFIX": prefix,
        "IMAGE_ID": candidate.image_id,
        "IMAGE_HEX": candidate.image_id.split(":", 1)[1],
        "ARCHIVE_PATH": candidate.archive_path,
        "ARCHIVE_SHA256": candidate.archive_sha256,
        "ARCHIVE_BYTES": str(candidate.archive_bytes),
        "HARNESS_ROOT": candidate.harness_root,
        "RELEASE_ID": (
            f"spec162-t009-{prefix}-a{candidate.attempt_sequence:03d}"
        ),
        "MATERIALIZE_SUBMISSION_ID": (
            f"spec162-submission-t009-sif-{prefix}-"
            f"{candidate.attempt_sequence:03d}"
        ),
        "MATERIALIZE_RUN_ID": (
            f"spec162-run-t009-sif-{prefix}-"
            f"{candidate.attempt_sequence:03d}"
        ),
        "SMOKE_SUBMISSION_ID": (
            f"spec162-submission-t009-sif-smoke-{prefix}-"
            f"{candidate.attempt_sequence:03d}"
        ),
        "SMOKE_RUN_ID": (
            f"spec162-run-t009-sif-smoke-{prefix}-"
            f"{candidate.attempt_sequence:03d}"
        ),
        "PLAN_ROOT": (
            f"/project/tma1/ndnsf-di/sources/spec162-t009-sif-{prefix}-"
            f"a{candidate.attempt_sequence:03d}"
        ),
    }


def render_template(path: Path, replacements: dict[str, str]) -> str:
    rendered = path.read_text(encoding="utf-8")
    for key, value in replacements.items():
        rendered = rendered.replace(f"@{key}@", value)
    remaining = sorted(set(TOKEN_RE.findall(rendered)))
    if remaining:
        raise RenderError(
            "UNRESOLVED_TEMPLATE_TOKENS:" + ",".join(remaining)
        )
    return rendered


def write_new(path: Path, content: str, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        stream.write(content)
    path.chmod(mode)


def render(
    *,
    output_dir: Path,
    candidate: CandidateSource = DEFAULT_CANDIDATE,
) -> dict:
    if output_dir.exists() and any(output_dir.iterdir()):
        raise RenderError("OUTPUT_DIRECTORY_NOT_EMPTY")
    output_dir.mkdir(parents=True, exist_ok=True)

    require_digest(candidate.image_id, "IMAGE_ID")
    require_digest(
        "sha256:" + candidate.archive_sha256,
        "ARCHIVE_SHA256",
    )
    if candidate.archive_bytes <= 0:
        raise RenderError("ARCHIVE_BYTES_INVALID")
    if candidate.attempt_sequence <= 0:
        raise RenderError("ATTEMPT_SEQUENCE_INVALID")
    manifest = expected_archive_manifest(candidate)
    raw = canonical_manifest_bytes(manifest)
    replacements = validate_manifest(raw, manifest, candidate)
    materialize = render_template(MATERIALIZE_TEMPLATE, replacements)
    smoke = render_template(SMOKE_TEMPLATE, replacements)

    manifest_out = output_dir / "source-manifest.json"
    materialize_out = output_dir / "materialize.sbatch"
    smoke_out = output_dir / "operation-status-sif-smoke.sbatch"
    manifest_out.write_bytes(raw)
    manifest_out.chmod(0o444)
    write_new(materialize_out, materialize, 0o500)
    write_new(smoke_out, smoke, 0o500)
    harness_dir = output_dir / "harness"
    harness_dir.mkdir()
    for name, source in HARNESS_SOURCES.items():
        target = harness_dir / name
        shutil.copyfile(source, target)
        target.chmod(0o500 if name.endswith(".sh") else 0o400)

    release_dir = (
        f"/project/tma1/ndnsf-di/releases/{replacements['RELEASE_ID']}"
    )
    evidence_root = "/project/tma1/ndnsf-di/evidence/spec162"
    plan = {
        "schemaVersion": "spec162-t009-sif-submission-plan-v2",
        "status": "READY_FOR_EXPLICIT_AUTHORIZATION",
        "submitted": False,
        "sourceKind": "sealed-docker-archive",
        "sourceReference": replacements["SOURCE_REFERENCE"],
        "sourceManifestDigest": replacements["SOURCE_DIGEST"],
        "sourceManifestSha256": sha256_file(manifest_out),
        "localImageId": replacements["IMAGE_ID"],
        "archive": candidate.archive_path,
        "archiveSha256": "sha256:" + candidate.archive_sha256,
        "archiveBytes": candidate.archive_bytes,
        "attemptSequence": candidate.attempt_sequence,
        "predecessorMaterialization": (
            candidate.predecessor_materialization
        ),
        "planRoot": replacements["PLAN_ROOT"],
        "harnessRoot": candidate.harness_root,
        "harness": {
            name: {
                "path": f"{replacements['PLAN_ROOT']}/harness/{name}",
                "sha256": sha256_file(harness_dir / name),
            }
            for name in sorted(HARNESS_SOURCES)
        },
        "releaseId": replacements["RELEASE_ID"],
        "releaseDirectory": release_dir,
        "materialization": {
            "submissionId": replacements["MATERIALIZE_SUBMISSION_ID"],
            "runId": replacements["MATERIALIZE_RUN_ID"],
            "script": f"{replacements['PLAN_ROOT']}/materialize.sbatch",
            "scriptSha256": sha256_file(materialize_out),
            "evidenceDirectory": (
                f"{evidence_root}/materialization/"
                f"{replacements['MATERIALIZE_SUBMISSION_ID']}"
            ),
            "exactCommand": (
                "sbatch --parsable "
                f"--comment=spec162:{replacements['MATERIALIZE_SUBMISSION_ID']} "
                f"{replacements['PLAN_ROOT']}/materialize.sbatch"
            ),
        },
        "gpuSmoke": {
            "submissionId": replacements["SMOKE_SUBMISSION_ID"],
            "runId": replacements["SMOKE_RUN_ID"],
            "script": (
                f"{replacements['PLAN_ROOT']}/"
                "operation-status-sif-smoke.sbatch"
            ),
            "scriptSha256": sha256_file(smoke_out),
            "evidenceDirectory": (
                f"{evidence_root}/operation-status-sif-smoke/"
                f"{replacements['SMOKE_SUBMISSION_ID']}"
            ),
            "commandTemplate": (
                "sbatch --parsable "
                f"--comment=spec162:{replacements['SMOKE_SUBMISSION_ID']} "
                "--export=ALL,SPEC162_T009_SIF_SHA256=<64-lowercase-hex> "
                f"{replacements['PLAN_ROOT']}/"
                "operation-status-sif-smoke.sbatch"
            ),
        },
    }
    plan_path = output_dir / "submission-plan.json"
    write_new(
        plan_path,
        json.dumps(plan, indent=2, sort_keys=True) + "\n",
        0o444,
    )
    return plan


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--image-id", default=EXPECTED_IMAGE_ID)
    parser.add_argument("--archive-path", default=ARCHIVE_PATH)
    parser.add_argument("--archive-sha256", default=ARCHIVE_SHA256)
    parser.add_argument("--archive-bytes", type=int, default=ARCHIVE_BYTES)
    parser.add_argument("--harness-root", default=HARNESS_ROOT)
    parser.add_argument(
        "--attempt-sequence", type=int, default=ATTEMPT_SEQUENCE
    )
    parser.add_argument(
        "--predecessor-materialization",
        default=PREDECESSOR_MATERIALIZATION,
    )
    args = parser.parse_args()
    archive_sha256 = args.archive_sha256
    if archive_sha256.startswith("sha256:"):
        archive_sha256 = archive_sha256[len("sha256:") :]
    candidate = CandidateSource(
        image_id=args.image_id,
        archive_path=args.archive_path,
        archive_sha256=archive_sha256,
        archive_bytes=args.archive_bytes,
        harness_root=args.harness_root,
        attempt_sequence=args.attempt_sequence,
        predecessor_materialization=args.predecessor_materialization,
    )
    plan = render(output_dir=args.output_dir, candidate=candidate)
    print(json.dumps(plan, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
