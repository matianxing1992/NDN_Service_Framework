#!/usr/bin/env python3
"""Spec183 operator entrypoint.

The five public actions are deliberately fail-closed.  A structurally valid
profile is not a release: ``prepare`` freezes content-verified inputs only,
``local`` needs a prepared immutable run, and ``submit`` needs the corresponding
allocation gate. Freezing alone returns 78/NOT_EVALUATED; execution without
qualified evidence remains disabled. The private ``run`` action is used only by
the checked-in Slurm wrapper after an allocation has been granted.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
BUNDLE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(BUNDLE))
from runtime.yolo_profile import (ClosureError, HASH, _file_identity, _operator_path,
                                  _read_plane, check_operator_profile, resolve_run_plan)


INCOMPLETE = 78

CASE_GATE = {
    "single-node-gpu": "localSif",
    "two-node-gpu": "singleNodeGpu",
    "negative-dependency": "twoNodeGpu",
}


def _json_digest(value):
    return "sha256:" + hashlib.sha256(json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def _safe_output(path: Path) -> Path:
    """Resolve an operator output without following a pre-existing symlink."""
    path = Path(path)
    if any(part.is_symlink() for part in (path, *path.parents)):
        raise ClosureError("RUN_OUTPUT_SYMLINK")
    path = Path(os.path.abspath(str(path)))
    if path == Path(path.anchor):
        raise ClosureError("RUN_OUTPUT_ROOT")
    return path


def _write_readonly(path: Path, value: dict) -> None:
    """Write a small receipt atomically; never overwrite a run artifact."""
    path = Path(path)
    if path.exists() or path.is_symlink():
        raise ClosureError("RUN_ARTIFACT_EXISTS")
    if path.parent.exists():
        if path.parent.is_symlink() or not path.parent.is_dir():
            raise ClosureError("RUN_ARTIFACT_PARENT")
    else:
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=False)
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    temporary = path.with_name(path.name + ".tmp")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    fd = os.open(str(temporary), flags, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(str(temporary), str(path))
        path.chmod(0o444)
    finally:
        if temporary.exists():
            temporary.unlink()


def _file_ref(profile_path: Path, name: str, row: dict) -> Path:
    if not isinstance(row, dict) or set(row) != {"path", "bytes", "sha256"}:
        raise ClosureError("GATE_REFERENCE")
    target = Path(_operator_path(row["path"], profile_path.parent, local=True))
    _file_identity(target.parent, name, dict(row, path=target.name))
    return target


def _gate_receipt(profile_path: Path, profile: dict, gate: str) -> dict:
    """Read a signed/owned prerequisite receipt without treating its hash as PASS."""
    gates = profile.get("release", {}).get("gates", {})
    row = gates.get(gate)
    if row is None:
        raise ClosureError("GATE_MISSING:" + gate)
    path = _file_ref(profile_path, gate, row)
    value = _read_plane(path)
    if (not isinstance(value, dict)
            or value.get("status") not in ("PASS", "READY")
            or value.get("qualification") not in ("PASS", "READY", "QUALIFIED")):
        raise ClosureError("GATE_NOT_QUALIFIED:" + gate)
    return {"name": gate, "path": str(path), "sha256": row["sha256"], "receipt": value}


def _dispatch_report(profile: Path) -> tuple[dict, dict]:
    report = check_operator_profile(profile, stage="dispatch")
    loaded = report.get("profile")
    # check_operator_profile intentionally does not return the mutable profile;
    # load it again only after the content check, anchored to the profile path.
    from runtime.yolo_profile import load_operator_profile
    loaded = load_operator_profile(profile, stage="dispatch")["profile"]
    return report, loaded


def _prepared_path(output: Path, run_id: str) -> Path:
    if not isinstance(run_id, str) or not re.fullmatch(r"[a-z][a-z0-9-]{1,47}", run_id):
        raise ClosureError("RUN_ID")
    return _safe_output(output) / run_id / "prepare.json"


def _load_prepared(output: Path, run_id: str) -> dict:
    path = _prepared_path(output, run_id)
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ClosureError("RUN_PREPARATION_SYMLINK")
    if not path.is_file():
        raise ClosureError("RUN_NOT_PREPARED")
    value = _read_plane(path)
    fields = {"schema", "status", "qualification", "runId", "case", "candidateDigest",
              "profileDigest", "plan", "bundle", "harnessManifestSha256"}
    if (set(value) != fields or value["schema"] != "tiger-yolo-prepared-run-v1"
            or value["status"] != "PREPARED" or value["qualification"] != "NOT_EVALUATED"
            or value["runId"] != run_id or not HASH.fullmatch(value["candidateDigest"])
            or not HASH.fullmatch(value["profileDigest"])
            or not isinstance(value["plan"], dict)
            or value["plan"].get("runId") != run_id
            or not isinstance(value["bundle"], str)
            or not HASH.fullmatch(value["harnessManifestSha256"])):
        raise ClosureError("RUN_PREPARATION_RECEIPT")
    bundle = Path(value["bundle"])
    if any(p.is_symlink() for p in (bundle, *bundle.parents)):
        raise ClosureError("RUN_BUNDLE_SYMLINK")
    return value


def _collection_file(root: Path) -> Path:
    """Return the one worker-owned collection input, without following links."""
    path = root / "collection-input.json"
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ClosureError("COLLECTION_INPUT_SYMLINK")
    if not path.is_file():
        raise ClosureError("COLLECTION_INPUT_MISSING")
    return path


def _collection_path(root: Path, value, *, label: str, directory: bool = False) -> Path:
    """Resolve a retained evidence path and reject symlink/path ambiguity."""
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ClosureError("COLLECTION_PATH:" + label)
    path = Path(value)
    if not path.is_absolute():
        path = root / path
    path = Path(os.path.abspath(str(path)))
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ClosureError("COLLECTION_PATH_SYMLINK:" + label)
    if (directory and not path.is_dir()) or (not directory and not path.is_file()):
        raise ClosureError("COLLECTION_PATH_MISSING:" + label)
    return path


def _digest_field(value, label: str) -> str:
    if not isinstance(value, str) or not HASH.fullmatch(value):
        raise ClosureError("COLLECTION_DIGEST:" + label)
    return value


def _load_collection_input(path: Path, *, root: Path, prepared: dict) -> tuple[dict, str]:
    """Validate the worker-to-collector handoff before importing the oracle."""
    payload = path.read_bytes()
    digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    value = _read_plane(path)
    common = {"schema", "status", "runId", "candidateDigest", "case", "kind"}
    if (not isinstance(value, dict) or not common.issubset(value)
            or value["schema"] != "tiger-yolo-collection-input-v1"
            or value["status"] != "READY"
            or value["runId"] != prepared["runId"]
            or value["candidateDigest"] != prepared["candidateDigest"]
            or value["case"] != prepared["case"]
            or value["kind"] not in ("normal", "expected-rejection")):
        raise ClosureError("COLLECTION_INPUT_BINDING")
    if value["kind"] == "normal":
        required = common | {"runtimeCandidateDigest", "placementCandidateId",
                             "placementCandidateDigest", "graphDigest", "catalogueDigest",
                             "providersByRole", "nodes", "references", "certifiedGraph"}
        if set(value) not in (required, required | {"allocationExpected"}):
            raise ClosureError("COLLECTION_INPUT_SCHEMA")
        for key in ("runtimeCandidateDigest", "placementCandidateDigest", "graphDigest",
                    "catalogueDigest"):
            _digest_field(value[key], key)
        if (not isinstance(value["placementCandidateId"], str)
                or not re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", value["placementCandidateId"])):
            raise ClosureError("COLLECTION_PLACEMENT_CANDIDATE")
        roles = {"BackboneNeck", "DetectShard0", "DetectShard1", "Merge"}
        if (not isinstance(value["providersByRole"], dict)
                or set(value["providersByRole"]) != roles
                or any(not isinstance(v, str) or not v for v in value["providersByRole"].values())):
            raise ClosureError("COLLECTION_PROVIDER_ROLES")
        expected_nodes = 2 if value["case"] == "two-node-gpu" else 1
        if (not isinstance(value["nodes"], dict)
                or set(value["nodes"]) != {str(i) for i in range(expected_nodes)}):
            raise ClosureError("COLLECTION_NODE_COVERAGE")
        nodes = {}
        node_fields = {"root", "receiptDigest", "preparationDigest"}
        if value["case"] != "local-cpu":
            node_fields |= {"allocationDigest", "gpuProbeDigest"}
        for key, row in value["nodes"].items():
            if not isinstance(row, dict) or set(row) != node_fields:
                raise ClosureError("COLLECTION_NODE_SCHEMA")
            nodes[int(key)] = {
                field: (_collection_path(root, row[field], label=f"node-{key}-{field}",
                                         directory=(field == "root"))
                        if field == "root" else _digest_field(row[field], f"node-{key}-{field}"))
                for field in row
            }
        expected_requests = 4 if value["case"] == "two-node-gpu" else 2
        if (not isinstance(value["references"], list)
                or len(value["references"]) != expected_requests):
            raise ClosureError("COLLECTION_REFERENCE_COVERAGE")
        references = []
        for index, row in enumerate(value["references"]):
            if (not isinstance(row, dict) or set(row) != {"package", "repository", "inputSize"}
                    or type(row["inputSize"]) is not int or row["inputSize"] <= 0):
                raise ClosureError("COLLECTION_REFERENCE_SCHEMA")
            references.append({"package": _collection_path(root, row["package"], label=f"reference-{index}-package", directory=True),
                               "repository": _collection_path(root, row["repository"], label=f"reference-{index}-repository", directory=True),
                               "inputSize": row["inputSize"]})
        if not isinstance(value["certifiedGraph"], dict):
            raise ClosureError("COLLECTION_CERTIFIED_GRAPH")
        value = dict(value, nodes=nodes, references=references)
        return value, digest
    required = common | {"requestId", "attempt", "candidateDigestForRequest",
                         "requestDeadlineMs", "rejection"}
    if set(value) != required:
        raise ClosureError("COLLECTION_REJECTION_SCHEMA")
    if (not isinstance(value["requestId"], str) or not value["requestId"].startswith("/")
            or type(value["attempt"]) is not int or not 0 < value["attempt"] < 2**64
            or not HASH.fullmatch(value["candidateDigestForRequest"])
            or type(value["requestDeadlineMs"]) is not int
            or not 1501 <= value["requestDeadlineMs"] <= 60000
            or not isinstance(value["rejection"], dict)):
        raise ClosureError("COLLECTION_REJECTION_BINDING")
    return value, digest


def _not_ready(action: str, reason: str, report: dict | None = None) -> int:
    value = {"status": "INCOMPLETE", "action": action,
             "qualification": "NOT_EVALUATED", "reason": reason}
    if report is not None:
        value["check"] = report
    print(json.dumps(value, sort_keys=True))
    return INCOMPLETE


def _submission_command(profile_path: Path, profile: dict, args, prepared: dict,
                        submission_key: str) -> list[str]:
    """Render allocation argv for review; this does not authorize dispatch."""
    cluster = profile["cluster"]
    output = _safe_output(args.output)
    bundle = Path(prepared["bundle"])
    if (not bundle.is_dir() or bundle.is_symlink()
            or any(parent.is_symlink() for parent in bundle.parents)):
        raise ClosureError("RUN_BUNDLE_SYMLINK")
    if args.case not in CASE_GATE or prepared["case"] != args.case:
        raise ClosureError("SUBMIT_CASE")
    nodes = 1 if args.case == "single-node-gpu" else 2
    seconds = cluster["wallTimeSeconds"]
    walltime = f"{seconds // 3600:02d}:{seconds % 3600 // 60:02d}:{seconds % 60:02d}"
    wrapper = bundle / "jobs/yolo/run.sbatch"
    if not wrapper.is_file() or wrapper.is_symlink() or not os.access(wrapper, os.X_OK):
        raise ClosureError("RUN_WRAPPER")
    command = [
        "sbatch", "--parsable", "--export=NONE",
        "--partition=" + cluster["partition"],
        "--account=" + cluster["account"],
        "--nodes=" + str(nodes), "--ntasks=" + str(nodes), "--ntasks-per-node=1",
        "--gres=gpu:" + cluster["gpuClass"] + ":1",
        "--cpus-per-task=" + str(cluster["cpusPerNode"]),
        "--mem=" + str(cluster["memoryGiB"]) + "G",
        "--time=" + walltime,
        "--job-name=tiger-yolo-" + args.case,
        "--comment=" + submission_key,
        "--output=" + str(output / (args.run_id + "-slurm-%j.log")),
        str(wrapper), str(bundle), str(profile_path.resolve()), str(output),
        args.run_id, args.case,
    ]
    constraint = cluster.get("constraint", "")
    if constraint:
        command.insert(5, "--constraint=" + constraint)
    return command


def _prepare(args) -> int:
    profile = Path(args.profile)
    report, value = _dispatch_report(profile)
    # Freezing checked bytes is not executing them. Runtime qualification is
    # consumed by local/submit, not produced by this offline preparation step.
    if report.get("integrity") != "VERIFIED":
        return _not_ready("prepare", "DISPATCH_INTEGRITY", report)
    plan = resolve_run_plan(profile, stage="dispatch", case=args.case,
                            run_id=args.run_id, output=args.output)
    if plan["documentDigest"] != report["documentDigest"]:
        raise ClosureError("PROFILE_CHANGED_DURING_PREPARE")
    from runtime.yolo_bundle import freeze_harness
    manifest_ref = value["evidence"]["harnessManifest"]
    manifest = _file_ref(profile, "harnessManifest", manifest_ref)
    run_root = _safe_output(args.output) / args.run_id
    if run_root.exists() or run_root.is_symlink():
        raise ClosureError("RUN_ARTIFACT_EXISTS")
    run_root.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        run_root.mkdir(mode=0o700, exist_ok=False)
    except FileExistsError as exc:
        raise ClosureError("RUN_ARTIFACT_EXISTS") from exc
    bundle = run_root / "bundle"
    frozen = freeze_harness(manifest, bundle,
                            expected_manifest_sha256=manifest_ref["sha256"],
                            source_root=manifest.parent)
    # Preserve the source; the frozen copy is bound independently so subsequent
    # source edits cannot silently change this run's harness.
    candidate = _json_digest({"profile": report["documentDigest"], "plan": plan,
                              "harness": frozen["manifestSha256"]})
    receipt = {"schema": "tiger-yolo-prepared-run-v1", "status": "PREPARED",
               "qualification": "NOT_EVALUATED", "runId": args.run_id,
               "case": args.case, "candidateDigest": candidate,
               "profileDigest": report["documentDigest"], "plan": plan,
               "bundle": str(bundle), "harnessManifestSha256": frozen["manifestSha256"]}
    _write_readonly(run_root / "prepare.json", receipt)
    print(json.dumps(receipt, sort_keys=True))
    return INCOMPLETE


def _local(args) -> int:
    report, value = _dispatch_report(Path(args.profile))
    if report.get("qualification") != "READY":
        return _not_ready("local", "DISPATCH_GATE", report)
    prepared = _load_prepared(args.output, args.run_id)
    if args.case != "local-cpu" or prepared["case"] != args.case:
        raise ClosureError("LOCAL_CASE")
    _gate_receipt(Path(args.profile), value, "hostMinindn")
    # The real SIF worker is intentionally enabled only after T008/T009/T010
    # produce the local gate receipt.  This branch prevents a structural profile
    # from silently becoming a fake local qualification.
    return _not_ready("local", "LOCAL_WORKER_NOT_WIRED", {"prepared": prepared["candidateDigest"]})


def _submit(args) -> int:
    if args.case not in CASE_GATE:
        raise ClosureError("SUBMIT_CASE")
    profile_path = Path(args.profile)
    report, value = _dispatch_report(profile_path)
    if report.get("qualification") != "READY":
        return _not_ready("submit", "DISPATCH_GATE", report)
    prepared = _load_prepared(args.output, args.run_id)
    if prepared["case"] != args.case:
        raise ClosureError("SUBMIT_CASE")
    if report["documentDigest"] != prepared["profileDigest"]:
        raise ClosureError("PROFILE_CHANGED_AFTER_PREPARE")
    gate_name = CASE_GATE[args.case]
    _gate_receipt(profile_path, value, gate_name)
    # A generic PASS marker cannot establish remote staging or allocation
    # readiness. T012 must validate the promoted bundle and wire the runner
    # before any journal mutation or sbatch call is enabled.
    return _not_ready("submit", "REMOTE_STAGING_NOT_WIRED",
                      {"prepared": prepared["candidateDigest"], "case": args.case})


def _collect(args) -> int:
    profile_path = Path(args.profile)
    report, _ = _dispatch_report(profile_path)
    if report.get("qualification") != "READY":
        return _not_ready("collect", "DISPATCH_GATE", report)
    prepared = _load_prepared(args.output, args.run_id)
    if report["documentDigest"] != prepared["profileDigest"]:
        raise ClosureError("PROFILE_CHANGED_AFTER_PREPARE")
    root = _safe_output(args.output) / args.run_id
    verdict = root / "verdict.json"
    if any(p.is_symlink() for p in (verdict, *verdict.parents)):
        raise ClosureError("VERDICT_SYMLINK")
    previous = None
    if verdict.is_file():
        value = _read_plane(verdict)
        if (not isinstance(value, dict) or value.get("runId") != args.run_id
                or value.get("candidateDigest") != prepared["candidateDigest"]
                or value.get("collectorSchema") != "tiger-yolo-collector-v1"
                or value.get("status") != "PASS"):
            raise ClosureError("VERDICT_BINDING")
        previous = value
    try:
        collection_path = _collection_file(root)
    except ClosureError as exc:
        return _not_ready("collect", str(exc), {"prepared": prepared["candidateDigest"]})
    try:
        collection, collection_digest = _load_collection_input(
            collection_path, root=root, prepared=prepared)
        from runtime import yolo_result
        if collection["kind"] == "normal":
            from ndnsf_distributed_inference.adapters.yolo.reference import load_reference
            references = [load_reference(row["package"], row["repository"], row["inputSize"])
                          for row in collection["references"]]
            final = yolo_result.collect_normal_verdict(
                collection["nodes"], references, plan=prepared["plan"],
                runtime_candidate_digest=collection["runtimeCandidateDigest"],
                placement_candidate_id=collection["placementCandidateId"],
                placement_candidate_digest=collection["placementCandidateDigest"],
                graph_digest=collection["graphDigest"],
                catalogue_digest=collection["catalogueDigest"],
                providers_by_role=collection["providersByRole"],
                allocation_expected=collection.get("allocationExpected"),
                certified_graph=collection["certifiedGraph"])
        else:
            request = prepared["plan"].get("requests", [{}])[0]
            if request.get("requestId") != collection["requestId"]:
                raise ClosureError("COLLECTION_REJECTION_REQUEST")
            final = yolo_result.finalize_expected_rejection(
                collection["rejection"], plan=prepared["plan"],
                request_id=collection["requestId"], attempt=collection["attempt"],
                candidate_digest=collection["candidateDigestForRequest"],
                request_deadline_ms=collection["requestDeadlineMs"])
        final = dict(final, runId=args.run_id,
                     candidateDigest=prepared["candidateDigest"],
                     collectorSchema="tiger-yolo-collector-v1",
                     collectionInputDigest=collection_digest)
        # Retained PASS is a historical result, not authority to skip its
        # evidence. Re-run the same collector even for an unchanged handoff;
        # nested node logs, native outputs and references may have changed.
        if previous is not None:
            if previous != final:
                raise ClosureError("VERDICT_REANALYSIS_MISMATCH")
        else:
            _write_readonly(verdict, final)
        print(json.dumps(final, sort_keys=True))
        return 0
    except (ClosureError, ValueError, OSError, ImportError) as exc:
        reason = str(exc) if isinstance(exc, ClosureError) else "COLLECTION_REJECTED"
        failure = {"schema": "tiger-yolo-collection-failure-v1", "status": "FAIL",
                   "runId": args.run_id, "candidateDigest": prepared["candidateDigest"],
                   "collectionInputDigest": "sha256:" + hashlib.sha256(
                       collection_path.read_bytes()).hexdigest(), "reason": reason}
        failure_path = root / "collection-failure.json"
        if not failure_path.exists() and not failure_path.is_symlink():
            _write_readonly(failure_path, failure)
        return _not_ready("collect", "COLLECTION_REJECTED", {"reason": reason})


def _run(args) -> int:
    if not os.environ.get("SLURM_JOB_ID"):
        raise ClosureError("ALLOCATION_REQUIRED")
    # The Slurm wrapper reaches this private action only after submit has a
    # qualified staged bundle.  Until T012 wires the real worker, fail closed;
    # never turn a job allocation into an unvalidated inference claim.
    raise ClosureError("RUNNER_NOT_WIRED")


def _common(parser, *, case=False):
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--output", type=Path, required=True)
    if case:
        parser.add_argument("--case", choices=("local-cpu", "single-node-gpu",
                                                 "two-node-gpu", "negative-dependency"),
                            required=True)


def _error(exc):
    print(json.dumps({"status": "REJECTED", "qualification": "NOT_EVALUATED",
                      "reason": str(exc)}, sort_keys=True))
    return 2


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    check = commands.add_parser("check", help="read-only content check; not runtime qualification")
    check.add_argument("--profile", type=Path, required=True)
    check.add_argument("--stage", choices=("inputs", "runtime", "dispatch"), default="dispatch")
    check.add_argument("--run-id", help="optional run preview; requires --output and --case")
    check.add_argument("--output", type=Path)
    check.add_argument("--case", choices=("local-cpu", "single-node-gpu", "two-node-gpu", "negative-dependency"))
    prepare = commands.add_parser("prepare", help="freeze content-verified inputs; not runtime qualification")
    _common(prepare, case=True)
    local = commands.add_parser("local", help="run the qualified local-cpu gate")
    _common(local, case=True)
    submit = commands.add_parser("submit", help="submit one qualified Slurm case")
    _common(submit, case=True)
    collect = commands.add_parser("collect", help="recompute a retained verdict")
    _common(collect)
    runner = commands.add_parser("run", help=argparse.SUPPRESS)
    _common(runner, case=True)
    args = parser.parse_args(argv)
    try:
        if args.action == "check":
            preview = (args.run_id is not None, args.output is not None, args.case is not None)
            if any(preview) and not all(preview):
                raise ClosureError("RUN_PREVIEW_OPTIONS")
            plan = (resolve_run_plan(args.profile, stage=args.stage, case=args.case,
                                     run_id=args.run_id, output=args.output) if all(preview) else None)
            report = check_operator_profile(args.profile, stage=args.stage)
            if plan is not None:
                if plan["documentDigest"] != report["documentDigest"]:
                    raise ClosureError("PROFILE_CHANGED_DURING_CHECK")
                report["runPlan"] = plan
            print(json.dumps(report, sort_keys=True))
            return INCOMPLETE
        if args.action == "prepare":
            return _prepare(args)
        if args.action == "local":
            return _local(args)
        if args.action == "submit":
            return _submit(args)
        if args.action == "collect":
            return _collect(args)
        return _run(args)
    except ClosureError as exc:
        return _error(exc)


if __name__ == "__main__":
    sys.exit(main())
