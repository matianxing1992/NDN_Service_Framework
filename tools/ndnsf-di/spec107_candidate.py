#!/usr/bin/env python3
"""Command-line surface for Spec 107 lineage, artifacts, and identities."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Any, Mapping

from spec107_artifacts import ArtifactError, materialize_artifact_set
from spec107_identity import (
    IdentityError,
    build_candidate_identity,
    build_campaign_identity,
    committed_source_digest,
    validate_candidate_identity,
)
from spec107_lineage import (
    LineageError,
    assert_mutation_allowed,
    default_repo_root,
    verify_lineage_lock,
)


class CliError(ValueError):
    pass


CANDIDATE_INPUT_SCHEMA = "ndnsf-di-spec107-candidate-inputs-v1"
CANDIDATE_INPUT_FILES = (
    ("profile", "profile"),
    ("model", "model"),
    ("plan", "plan"),
    ("artifact", "artifact"),
    ("lineage", "lineage"),
    ("workload", "workload"),
    ("tokenizer", "tokenizer"),
    ("trust_policy", "trustPolicy"),
    ("command", "command"),
)


def _load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CliError(f"JSON_INPUT_INVALID:{path}:{exc}") from exc
    if not isinstance(value, dict):
        raise CliError(f"JSON_INPUT_INVALID:{path}:root-not-object")
    return value


def _write_exclusive(path: Path, value: Mapping[str, object], *, repo_root: Path) -> Path:
    resolved = assert_mutation_allowed(path, repo_root=repo_root)
    resolved.parent.mkdir(parents=True, exist_ok=True)
    try:
        with resolved.open("x", encoding="utf-8") as stream:
            json.dump(dict(value), stream, indent=2, sort_keys=True)
            stream.write("\n")
    except FileExistsError as exc:
        raise CliError(f"OUTPUT_EXISTS:{resolved}") from exc
    return resolved


def _model_revision(path: Path) -> str:
    payload = _load_object(path)
    model = payload.get("model")
    if not isinstance(model, dict):
        raise CliError("MODEL_MANIFEST_INVALID:model")
    identifier = model.get("id")
    revision = model.get("revision")
    if not isinstance(identifier, str) or not isinstance(revision, str):
        raise CliError("MODEL_MANIFEST_INVALID:id-or-revision")
    return f"{identifier}@{revision}"


def _artifact_rows(source: Path) -> list[dict[str, str]]:
    files = sorted(source.glob("*.onnx"))
    if len(files) != 3:
        raise CliError(f"ARTIFACT_SOURCE_COUNT_INVALID:{len(files)}")
    return [
        {"role": f"/LLM/Pipeline/Stage/{index}", "source": path.name}
        for index, path in enumerate(files)
    ]


def _require_clean_tracked_source(repo: Path) -> None:
    try:
        probe = subprocess.run(
            ["git", "status", "--porcelain=v1", "--untracked-files=no"],
            cwd=repo, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False)
    except OSError as exc:
        raise CliError(f"CANDIDATE_SOURCE_GIT_UNAVAILABLE:{exc}") from exc
    if probe.returncode != 0:
        raise CliError(
            "CANDIDATE_SOURCE_GIT_INVALID:" + probe.stderr.strip())
    if probe.stdout.strip():
        raise CliError("CANDIDATE_SOURCE_TREE_DIRTY")


def _candidate_input_path(
    value: Path, *, key: str, repo: Path,
) -> tuple[Path, str, str]:
    resolved = value if value.is_absolute() else repo / value
    resolved = resolved.resolve()
    if "spec105" in resolved.as_posix().lower():
        raise CliError(f"SPEC105_IDENTITY_REJECTED:{key}:{resolved}")
    try:
        data = resolved.read_bytes()
    except OSError as exc:
        raise CliError(f"CANDIDATE_INPUT_INVALID:{key}:{resolved}:{exc}") from exc
    if not resolved.is_file():
        raise CliError(f"CANDIDATE_INPUT_INVALID:{key}:{resolved}:not-file")
    try:
        retained_path = resolved.relative_to(repo).as_posix()
    except ValueError:
        retained_path = resolved.as_posix()
    return resolved, "sha256:" + hashlib.sha256(data).hexdigest(), retained_path


def _candidate_inputs(args: argparse.Namespace, repo: Path) -> dict[str, Any]:
    _require_clean_tracked_source(repo)
    digests = {"source": committed_source_digest(repo)}
    retained: dict[str, str] = {}
    for argument, digest_key in CANDIDATE_INPUT_FILES:
        _, digest, retained_path = _candidate_input_path(
            getattr(args, argument), key=digest_key, repo=repo)
        digests[digest_key] = digest
        retained[digest_key] = retained_path
    payload = {
        "schema": CANDIDATE_INPUT_SCHEMA,
        "digests": digests,
        "inputs": retained,
    }
    _write_exclusive(args.output, payload, repo_root=repo)
    return payload


def _lineage_verify(args: argparse.Namespace, repo: Path) -> dict[str, Any]:
    return verify_lineage_lock(args.lock, repo_root=repo)


def _artifact_prepare(args: argparse.Namespace, repo: Path) -> dict[str, Any]:
    manifest_path = args.model_manifest
    if not manifest_path.is_absolute():
        manifest_path = repo / manifest_path
    return materialize_artifact_set(
        source_root=args.source,
        output_root=args.output_root,
        artifacts=_artifact_rows(args.source),
        candidate_id=args.candidate_id,
        model_revision=_model_revision(manifest_path),
        repo_root=repo,
        reserve_bytes=args.reserve_bytes,
    )


def _candidate_create(args: argparse.Namespace, repo: Path) -> dict[str, Any]:
    _require_clean_tracked_source(repo)
    payload = _load_object(args.digests)
    digests = payload.get("digests", payload)
    if not isinstance(digests, dict):
        raise CliError("CANDIDATE_DIGESTS_INVALID")
    if digests.get("source") != committed_source_digest(repo):
        raise CliError("CANDIDATE_SOURCE_DIGEST_MISMATCH")
    candidate = build_candidate_identity(
        digests,
        created_at=args.created_at,
        generator_version=args.generator_version,
    )
    _write_exclusive(args.output, candidate, repo_root=repo)
    return candidate


def _campaign_preregister(args: argparse.Namespace, repo: Path) -> dict[str, Any]:
    candidate = validate_candidate_identity(_load_object(args.candidate))
    campaign = build_campaign_identity(
        candidate,
        kind=args.kind,
        ordinal=args.ordinal,
        command_digest=args.command_digest,
        output_root=args.campaign_output_root,
    )
    _write_exclusive(args.output, campaign, repo_root=repo)
    return campaign


def _gate_generate(args: argparse.Namespace, repo: Path) -> dict[str, Any]:
    try:
        from build_spec107_release_bundle import (
            ReleaseBundleError,
            build_release_bundle,
        )
    except (ImportError, AttributeError) as exc:
        raise CliError("SPEC107_GATE_NOT_IMPLEMENTED_UNTIL_T067") from exc
    try:
        return build_release_bundle(
            feature=args.feature,
            output=args.output,
            repo_root=repo,
        )
    except ReleaseBundleError as exc:
        raise CliError(str(exc)) from exc


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root", type=Path, default=default_repo_root(),
        help="repository root (defaults to this checkout)")
    commands = parser.add_subparsers(dest="command", required=True)

    lineage = commands.add_parser("lineage")
    lineage_commands = lineage.add_subparsers(dest="lineage_command", required=True)
    verify = lineage_commands.add_parser("verify")
    verify.add_argument("--lock", type=Path, required=True)
    verify.set_defaults(handler=_lineage_verify)

    artifact = commands.add_parser("artifact")
    artifact_commands = artifact.add_subparsers(dest="artifact_command", required=True)
    prepare = artifact_commands.add_parser("prepare")
    prepare.add_argument("--source", type=Path, required=True)
    prepare.add_argument("--output-root", type=Path, required=True)
    prepare.add_argument("--candidate-id")
    prepare.add_argument(
        "--model-manifest", type=Path,
        default=Path("examples/ndnsf-di-qwen-pilot.model.json"))
    prepare.add_argument("--reserve-bytes", type=int, default=1024 * 1024 * 1024)
    prepare.set_defaults(handler=_artifact_prepare)

    candidate = commands.add_parser("candidate")
    candidate_commands = candidate.add_subparsers(dest="candidate_command", required=True)
    inputs = candidate_commands.add_parser("inputs")
    for argument, _ in CANDIDATE_INPUT_FILES:
        inputs.add_argument(
            "--" + argument.replace("_", "-"),
            dest=argument, type=Path, required=True)
    inputs.add_argument("--output", type=Path, required=True)
    inputs.set_defaults(handler=_candidate_inputs)
    create = candidate_commands.add_parser("create")
    create.add_argument("--digests", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--created-at")
    create.add_argument("--generator-version", default="spec107-tools-v1")
    create.set_defaults(handler=_candidate_create)

    campaign = commands.add_parser("campaign")
    campaign_commands = campaign.add_subparsers(dest="campaign_command", required=True)
    preregister = campaign_commands.add_parser("preregister")
    preregister.add_argument("--kind", required=True)
    preregister.add_argument("--ordinal", type=int, default=1)
    preregister.add_argument("--candidate", type=Path, required=True)
    preregister.add_argument("--command-digest", required=True)
    preregister.add_argument("--campaign-output-root", required=True)
    preregister.add_argument("--output", type=Path, required=True)
    preregister.set_defaults(handler=_campaign_preregister)

    gate = commands.add_parser("gate")
    gate_commands = gate.add_subparsers(dest="gate_command", required=True)
    generate = gate_commands.add_parser("generate")
    generate.add_argument("--feature", type=Path, required=True)
    generate.add_argument("--output", type=Path, required=True)
    generate.set_defaults(handler=_gate_generate)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    repo = args.repo_root.resolve()
    try:
        result = args.handler(args, repo)
    except (ArtifactError, CliError, IdentityError, LineageError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
