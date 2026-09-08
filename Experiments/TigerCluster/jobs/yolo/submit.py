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
import subprocess
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


def _prepared_candidate(value):
    return _json_digest({'profile': value['profileDigest'], 'plan': value['plan'],
        'harness': value['harnessManifestSha256'], 'content': value['contentIdentities']})


def _content_identities(value):
    if (not isinstance(value, dict) or set(value) != {'inputs', 'runtime', 'dispatch'}
            or any(not isinstance(item, str) or not HASH.fullmatch(item) for item in value.values())):
        raise ClosureError('RUN_CONTENT_IDENTITIES')
    return value


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


def _gate_receipt(profile_path: Path, profile: dict, gate: str, *, prepared=None) -> dict:
    """Read a signed/owned prerequisite receipt without treating its hash as PASS."""
    gates = profile.get("release", {}).get("gates", {})
    row = gates.get(gate)
    if row is None:
        raise ClosureError("GATE_MISSING:" + gate)
    path = _file_ref(profile_path, gate, row)
    value = _read_plane(path)
    if gate == 'hostMinindn':
        # Host qualification predates this SIF, but must name the same sealed
        # source used by its native build. Consume the existing receipt owner.
        from lib.spec183_yolo_host_gate import validate_yolo_host_gate
        try:
            source_path = Path(value['sourceSeal']['path'])
            if (not source_path.is_absolute()
                    or any(p.is_symlink() for p in (source_path, *source_path.parents))):
                raise ValueError('HOST_SOURCE_PATH')
            validated = validate_yolo_host_gate(path, source_seal_path=source_path)
            runtime_path = _file_ref(profile_path, 'runtime', profile['release']['runtime'])
            native_ref = _read_plane(runtime_path)['files']['nativeManifest']
            native_path = _file_ref(runtime_path, 'nativeManifest', native_ref)
            native = _read_plane(native_path)
            artifacts = native.get('artifacts')
            names = {'provider', 'faultProvider', 'controller', 'framework',
                     'ndn-svs', 'nac-abe', 'ndn-sd', 'extension', 'repoExtension'}
            if (native.get('schemaVersion') != 'spec170-container-native-build-v1'
                    or native.get('buildBoundary') != 'container-runtime-in-sif'
                    or native.get('status') != 'PASS'
                    or native.get('sourceSealSha256') != validated['sourceSeal']['sha256']):
                raise ValueError('HOST_SIF_SOURCE_MISMATCH')
            if (not isinstance(artifacts, list) or len(artifacts) != len(names)
                    or {item.get('name') for item in artifacts if isinstance(item, dict)} != names
                    or any(not isinstance(item.get('sha256'), str)
                           or not HASH.fullmatch(item['sha256'])
                           or item.get('finalSha256') != item['sha256']
                           for item in artifacts)):
                raise ValueError('HOST_SIF_NATIVE_CLOSURE')
        except (KeyError, TypeError, ValueError, OSError) as exc:
            raise ClosureError('GATE_HOST_SOURCE_BINDING') from exc
        return {'name': gate, 'path': str(path), 'sha256': row['sha256'], 'receipt': validated}
    cases = {'localSif': 'local-cpu', 'singleNodeGpu': 'single-node-gpu', 'twoNodeGpu': 'two-node-gpu'}
    if gate not in cases or not isinstance(prepared, dict):
        raise ClosureError('GATE_CURRENT_PREPARATION_REQUIRED')
    if (path.name != 'verdict.json' or not isinstance(value, dict)
            or value.get('status') != 'PASS'
            or value.get('qualification') != 'NORMAL_EXPERIMENT_PASS'
            or value.get('case') != cases[gate]
            or value.get('collectorSchema') != 'tiger-yolo-collector-v1'):
        raise ClosureError('GATE_NOT_QUALIFIED:' + gate)
    previous = _load_prepared(path.parent.parent, path.parent.name)
    try:
        if (previous['case'] != cases[gate]
                or previous['runId'] == prepared['runId']
                or previous['candidateDigest'] == prepared['candidateDigest']
                or previous['contentIdentities'] != prepared['contentIdentities']
                or previous['harnessManifestSha256'] != prepared['harnessManifestSha256']
                or previous['plan']['effectiveBehavior']['profile'] != prepared['plan']['effectiveBehavior']['profile']):
            raise ValueError('GATE_REUSE_IDENTITY')
        from runtime.yolo_bundle import verify_harness
        verify_harness(Path(prepared['bundle']),
                       expected_manifest_sha256=prepared['harnessManifestSha256'])
        verified = _reanalyze_retained(path.parent, previous)
        if verified != value:
            raise ValueError('GATE_VERDICT_REANALYSIS_MISMATCH')
        if previous['case'] != 'local-cpu':
            _verify_terminal_record(path.parent, previous, require_pass=True)
    except (KeyError, TypeError, ValueError, OSError, ImportError) as exc:
        raise ClosureError('GATE_RETAINED_EVIDENCE:' + gate) from exc
    return {"name": gate, "path": str(path), "sha256": row["sha256"], "receipt": value}


def _dispatch_report(profile: Path) -> tuple[dict, dict]:
    report = check_operator_profile(profile, stage="dispatch")
    # check_operator_profile intentionally does not return the mutable profile;
    # load it again only after the content check, anchored to the profile path.
    from runtime.yolo_profile import load_operator_profile
    loaded = load_operator_profile(profile, stage="dispatch")
    if loaded['documentDigest'] != report['documentDigest']:
        raise ClosureError('PROFILE_CHANGED_DURING_DISPATCH')
    return report, loaded['profile']


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
              "profileDigest", "plan", "bundle", "harnessManifestSha256", "contentIdentities"}
    if (not isinstance(value, dict) or set(value) != fields
            or value["schema"] != "tiger-yolo-prepared-run-v2"
            or value["status"] != "PREPARED" or value["qualification"] != "NOT_EVALUATED"
            or value["runId"] != run_id
            or any(not isinstance(value[key], str) or not HASH.fullmatch(value[key])
                   for key in ('candidateDigest', 'profileDigest', 'harnessManifestSha256'))
            or not isinstance(value["plan"], dict)
            or value["plan"].get("runId") != run_id
            or not isinstance(value["bundle"], str)):
        raise ClosureError("RUN_PREPARATION_RECEIPT")
    _content_identities(value['contentIdentities'])
    if (value['plan'].get('case') != value['case']
            or value['plan'].get('documentDigest') != value['profileDigest']
            or value['plan'].get('output') != str(path.parent)
            or _prepared_candidate(value) != value['candidateDigest']):
        raise ClosureError('RUN_PREPARATION_CONTENT_BINDING')
    bundle = Path(value["bundle"])
    if not bundle.is_absolute() or any(p.is_symlink() for p in (bundle, *bundle.parents)):
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
        if value['runtimeCandidateDigest'] != prepared['candidateDigest']:
            raise ClosureError('COLLECTION_RUNTIME_BINDING')
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
        "/usr/bin/sbatch", "--parsable", "--export=NONE", "--no-requeue",
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
    receipt = {"schema": "tiger-yolo-prepared-run-v2", "status": "PREPARED",
               "qualification": "NOT_EVALUATED", "runId": args.run_id,
               "case": args.case, "contentIdentities": _content_identities(report['identities']),
               "profileDigest": report["documentDigest"], "plan": plan,
               "bundle": str(bundle), "harnessManifestSha256": frozen["manifestSha256"]}
    receipt['candidateDigest'] = _prepared_candidate(receipt)
    _write_readonly(run_root / "prepare.json", receipt)
    print(json.dumps(receipt, sort_keys=True))
    return INCOMPLETE


def _local(args) -> int:
    report, value = _dispatch_report(Path(args.profile))
    if report.get("integrity") != "VERIFIED":
        return _not_ready("local", "DISPATCH_GATE", report)
    prepared = _load_prepared(args.output, args.run_id)
    if args.case != "local-cpu" or prepared["case"] != args.case:
        raise ClosureError("LOCAL_CASE")
    if report["documentDigest"] != prepared["profileDigest"]:
        raise ClosureError("PROFILE_CHANGED_AFTER_PREPARE")
    _gate_receipt(Path(args.profile), value, "hostMinindn")
    return _execute_local(args, value, prepared)


def _enter_frozen(args, prepared, action):
    """Use the same verified harness for execution and offline reanalysis."""
    from runtime.yolo_bundle import verify_harness
    bundle = Path(prepared['bundle'])
    verify_harness(bundle, expected_manifest_sha256=prepared['harnessManifestSha256'])
    if BUNDLE == bundle:
        return None
    import subprocess
    command = [sys.executable, '-B', str(bundle / 'jobs/yolo/submit.py'), action,
        '--profile', str(Path(args.profile).absolute()), '--run-id', args.run_id,
        '--output', str(_safe_output(args.output))]
    if action in ('local', 'submit', 'run', 'rank'):
        command += ['--case', args.case]
    if action == 'collect' and getattr(args, 'reconcile', False):
        command += ['--reconcile']
    return subprocess.run(command, cwd=bundle, check=False).returncode


def _execute_local(args, profile, prepared):
    """Enter the frozen runner only after source-bound host qualification."""
    plan = resolve_run_plan(Path(args.profile), stage='dispatch', case=args.case,
                            run_id=args.run_id, output=args.output)
    if plan != prepared['plan'] or prepared['candidateDigest'] != _prepared_candidate(prepared):
        raise ClosureError('LOCAL_PREPARED_PLAN_BINDING')
    result = _enter_frozen(args, prepared, 'local')
    if result is not None:
        return result
    try:
        from runtime.yolo_profile import resolve_provision_inputs
        from runtime.yolo_operator import execute_local_run
        resolved = resolve_provision_inputs(Path(args.profile), plan=plan,
            runtime_candidate_digest=prepared['candidateDigest'])
        execute_local_run(prepared=prepared, profile=profile, resolved=resolved)
        return _collect(args)
    except (ValueError, OSError, ImportError, RuntimeError, TimeoutError) as exc:
        raise ClosureError('LOCAL_EXECUTION_FAILED:' + type(exc).__name__) from exc


def _submit(args) -> int:
    if args.case not in CASE_GATE:
        raise ClosureError("SUBMIT_CASE")
    profile_path = Path(args.profile)
    report, value = _dispatch_report(profile_path)
    if report.get("integrity") != "VERIFIED":
        return _not_ready("submit", "DISPATCH_GATE", report)
    prepared = _load_prepared(args.output, args.run_id)
    if prepared["case"] != args.case:
        raise ClosureError("SUBMIT_CASE")
    if report["documentDigest"] != prepared["profileDigest"]:
        raise ClosureError("PROFILE_CHANGED_AFTER_PREPARE")
    if report['identities'] != prepared['contentIdentities']:
        raise ClosureError('CONTENT_CHANGED_AFTER_PREPARE')
    result = _enter_frozen(args, prepared, 'submit')
    if result is not None:
        return result
    gate_name = CASE_GATE[args.case]
    _gate_receipt(profile_path, value, gate_name, prepared=prepared)
    if args.case == 'negative-dependency':
        return _not_ready('submit','NEGATIVE_RUNNER_NOT_WIRED')
    if not _shared_submission_paths(args,value,prepared):
        return _not_ready('submit','SHARED_STAGING_REQUIRED')
    return _submit_shared(args,value,prepared)


def _shared_submission_paths(args,profile,prepared):
    """Receiver-side closure only; this does not upload or relocate a run."""
    storage=profile.get('storage',{})
    if not {'remoteArtifactRoot','sharedRunRoot','sharedLockRoot'} <= set(storage):
        return False
    roots=[_safe_output(storage[key]) for key in ('remoteArtifactRoot','sharedRunRoot','sharedLockRoot')]
    artifact,runs,locks=roots
    root=_safe_output(args.output)/args.run_id
    if (any(not p.is_dir() for p in roots) or root.parent!=runs
            or prepared['plan']['output']!=str(root) or Path(prepared['bundle'])!=root/'bundle'):
        return False
    def within(path, allowed):
        path=_safe_output(path)
        for parent in allowed:
            try: path.relative_to(parent); return True
            except ValueError: pass
        return False
    if not within(Path(args.profile).absolute(),(artifact,)):
        return False
    def references(item,gate=False):
        if isinstance(item,dict):
            if set(item)=={'path','bytes','sha256'}:
                return within(item['path'],(runs,) if gate else (artifact,))
            return all(references(v,gate or k=='gates') for k,v in item.items())
        if isinstance(item,list): return all(references(v,gate) for v in item)
        return True
    if not references(profile): return False
    key=Path(profile['security']['authorityPrivateKey'])
    if (not within(key,(artifact,)) or not key.is_file() or key.stat().st_mode & 0o077):
        raise ClosureError('SUBMIT_PRIVATE_KEY_LOCATION_OR_MODE')
    return True


def _submit_shared(args,profile,prepared):
    """Single receiver-side sbatch owner, with durable uncertainty and recovery."""
    import datetime
    import time
    from runtime.yolo_submission import SubmissionJournal, JournalError, observe_submission, verify_operator_python
    from runtime.yolo_storage import measured_capacity
    if os.environ.get('SLURM_JOB_ID'):
        raise ClosureError('SUBMIT_INSIDE_ALLOCATION')
    seconds=profile['timing']['progressTimeoutSeconds']
    # A shared-looking pathname on a workstation is not a cluster preflight.
    try:
        journal=SubmissionJournal(Path(profile['storage']['sharedLockRoot']),
            candidate_id=prepared['contentIdentities']['dispatch'],gate=args.case)
        command=_submission_command(Path(args.profile),profile,args,prepared,journal._submission_key(args.run_id))
        site=subprocess.run(['/usr/bin/scontrol','show','config'],check=True,
            capture_output=True,timeout=seconds,env={'PATH':'/usr/bin:/bin','LC_ALL':'C'})
        if (len(site.stdout)>4*1024*1024 or site.stderr
                or re.findall(rb'^ClusterName\s*=\s*(\S+)\s*$',site.stdout,re.M)!=[b'itiger']):
            raise ClosureError('SUBMIT_CLUSTER_BINDING')
        verify_operator_python(Path(prepared['bundle']),seconds=seconds)
        measured_capacity(Path(args.output),profile['storage']['peakBytes']+profile['storage']['marginBytes'])
        try:
            row=journal.get(args.run_id)
        except JournalError as exc:
            if str(exc)!='RUN_NOT_REGISTERED': raise
            try: row=journal.reserve(args.run_id)
            except JournalError as race:
                if str(race)!='RUN_ALREADY_REGISTERED': raise
                row=journal.get(args.run_id)
        root=Path(prepared['plan']['output'])
        intent_path=root/'submission-intent.json'
        if row['state'] in ('PASS','FAIL','INCOMPLETE','CANCELLED_BEFORE_SUBMIT'):
            raise ClosureError('SUBMIT_RUN_ALREADY_CLOSED')
        if intent_path.exists():
            intent=_read_plane(intent_path)
            if (not isinstance(intent,dict) or set(intent)!= {'schema','runId','candidateDigest','argv','querySince'}
                    or intent['schema']!='tiger-yolo-submission-intent-v1'
                    or intent['runId']!=args.run_id or intent['candidateDigest']!=prepared['candidateDigest']
                    or intent['argv']!=command or not isinstance(intent['querySince'],str)
                    or re.fullmatch(r'\d{4}-\d{2}-\d{2}',intent['querySince']) is None):
                raise ClosureError('SUBMISSION_INTENT_BINDING')
        elif row['state']=='PREPARED':
            since=(datetime.datetime.now(datetime.timezone.utc)-datetime.timedelta(days=1)).date().isoformat()
            intent=dict(schema='tiger-yolo-submission-intent-v1',runId=args.run_id,
                candidateDigest=prepared['candidateDigest'],argv=command,querySince=since)
            _write_readonly(intent_path,intent)
        else:
            raise ClosureError('SUBMISSION_INTENT_MISSING')
        if row['state'] in ('SUBMITTING','SUBMISSION_UNKNOWN'):
            observed=observe_submission(submission_key=row['submissionKey'],partition=profile['cluster']['partition'],
                                        since=intent['querySince'],seconds=seconds)
            _write_readonly(root/('submission-query-'+str(time.time_ns())+'.json'),observed)
            try: row=journal.reconcile(args.run_id,observed['jobs'])
            except JournalError as race:
                current=journal.get(args.run_id)
                if str(race)!='INVALID_TRANSITION' or current['state'] not in ('SUBMITTED','RUNNING'): raise
                row=current
        elif row['state']=='PREPARED':
            journal.mark_submitting(args.run_id)
            try:
                response=subprocess.run(command,check=False,capture_output=True,timeout=seconds,
                    cwd=Path(prepared['bundle']),env={'PATH':'/usr/bin:/bin','LC_ALL':'C'})
                job=response.stdout.decode('ascii').strip()
                if response.returncode!=0 or response.stderr or re.fullmatch(r'[1-9][0-9]{0,9}(?:;itiger)?',job) is None:
                    raise ValueError('SUBMISSION_RESPONSE_UNCERTAIN')
                job=job.split(';')[0]
                _write_readonly(root/'submission-response.json',dict(jobId=job,exitCode=response.returncode,
                    stdout=response.stdout.decode('ascii'),submissionKey=row['submissionKey']))
                current=journal.get(args.run_id)
                if current['state'] in ('SUBMITTING','SUBMISSION_UNKNOWN'):
                    row=journal.record_submission(args.run_id,job)
                elif current['state'] in ('SUBMITTED','RUNNING') and current['jobId']==job:
                    row=current
                else: raise JournalError('SUBMISSION_ACK_CONFLICT')
            except (ValueError,OSError,subprocess.SubprocessError) as exc:
                current=journal.get(args.run_id)
                if current['state'] in ('SUBMITTING','SUBMISSION_UNKNOWN'):
                    journal.mark_unknown(args.run_id)
                return _not_ready('submit','SUBMISSION_UNCERTAIN',{'errorType':type(exc).__name__})
        if row['state'] not in ('SUBMITTED','RUNNING'):
            return _not_ready('submit','SUBMISSION_UNKNOWN',{'runId':args.run_id})
        print(json.dumps(dict(status=row['state'],jobId=row['jobId'],runId=args.run_id,
            candidateDigest=prepared['candidateDigest'],qualification='NOT_EVALUATED'),sort_keys=True))
        return 0
    except (ValueError,OSError,subprocess.SubprocessError) as exc:
        raise ClosureError('SHARED_SUBMISSION:'+str(exc)) from exc


def _reanalyze_retained(root: Path, prepared: dict) -> dict:
    """Recompute retained evidence using saved bindings, not a mutable profile.

    The enclosing CLI/gate verifies content identity and the frozen harness.
    This is read-only: no receipt mutation, native process, or scheduler call.
    """
    collection_path = _collection_file(root)
    collection, collection_digest = _load_collection_input(
        collection_path, root=root, prepared=prepared)
    from runtime import yolo_result
    if collection['kind'] == 'normal':
        if prepared['case'] != 'local-cpu':
            cleanup = _verify_srun_cleanup(root, prepared)
            if cleanup['jobId'] != collection.get('allocationExpected', {}).get('job_id'):
                raise ClosureError('SRUN_COLLECTION_JOB_BINDING')
            from runtime.yolo_storage import verify_storage_cleanup
            verify_storage_cleanup(root, prepared, cleanup['jobId'])
        from runtime.yolo_bundle import reference_owner, verify_harness
        verify_harness(Path(prepared['bundle']),
                       expected_manifest_sha256=prepared['harnessManifestSha256'])
        owner = reference_owner(Path(prepared['bundle']))
        references = [owner.load_reference(row['package'], row['repository'], row['inputSize'])
                      for row in collection['references']]
        expected = prepared['plan']['effectiveBehavior']['profile']
        if any(ref.manifest_digest != expected['workload']['packageManifest']['sha256']
               or ref.oracle_digest != expected['oracle']['reference']['sha256']
               or ref.fixture_digest != expected['oracle']['input']['sha256'] for ref in references):
            raise ClosureError('COLLECTION_REFERENCE_PROFILE_BINDING')
        final = yolo_result.collect_normal_verdict(
            collection['nodes'], references, plan=prepared['plan'],
            runtime_candidate_digest=collection['runtimeCandidateDigest'],
            placement_candidate_id=collection['placementCandidateId'],
            placement_candidate_digest=collection['placementCandidateDigest'],
            graph_digest=collection['graphDigest'], catalogue_digest=collection['catalogueDigest'],
            providers_by_role=collection['providersByRole'],
            allocation_expected=collection.get('allocationExpected'),
            certified_graph=collection['certifiedGraph'])
    else:
        request = prepared['plan'].get('requests', [{}])[0]
        if request.get('requestId') != collection['requestId']:
            raise ClosureError('COLLECTION_REJECTION_REQUEST')
        final = yolo_result.finalize_expected_rejection(
            collection['rejection'], plan=prepared['plan'], request_id=collection['requestId'],
            attempt=collection['attempt'], candidate_digest=collection['candidateDigestForRequest'],
            request_deadline_ms=collection['requestDeadlineMs'])
    return dict(final, runId=prepared['runId'], candidateDigest=prepared['candidateDigest'],
        collectorSchema='tiger-yolo-collector-v1', collectionInputDigest=collection_digest)


def _collect(args) -> int:
    profile_path = Path(args.profile)
    report, profile = _dispatch_report(profile_path)
    if report.get("integrity") != "VERIFIED":
        return _not_ready("collect", "DISPATCH_GATE", report)
    prepared = _load_prepared(args.output, args.run_id)
    if report["documentDigest"] != prepared["profileDigest"]:
        raise ClosureError("PROFILE_CHANGED_AFTER_PREPARE")
    result = _enter_frozen(args, prepared, 'collect')
    if result is not None:
        return result
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
        _maybe_reconcile(args, profile, prepared, 'INCOMPLETE')
        return _not_ready("collect", str(exc), {"prepared": prepared["candidateDigest"]})
    try:
        final = _reanalyze_retained(root, prepared)
        # Retained PASS is a historical result, not authority to skip its
        # evidence. Re-run the same collector even for an unchanged handoff;
        # nested node logs, native outputs and references may have changed.
        if previous is not None:
            if previous != final:
                raise ClosureError("VERDICT_REANALYSIS_MISMATCH")
        else:
            _write_readonly(verdict, final)
    except (ClosureError, ValueError, OSError, ImportError) as exc:
        reason = str(exc) if isinstance(exc, ClosureError) else "COLLECTION_REJECTED"
        failure = {"schema": "tiger-yolo-collection-failure-v1", "status": "FAIL",
                   "runId": args.run_id, "candidateDigest": prepared["candidateDigest"],
                   "collectionInputDigest": "sha256:" + hashlib.sha256(
                       collection_path.read_bytes()).hexdigest(), "reason": reason}
        failure_path = root / "collection-failure.json"
        if not failure_path.exists() and not failure_path.is_symlink():
            _write_readonly(failure_path, failure)
        _maybe_reconcile(args, profile, prepared, 'FAIL')
        return _not_ready("collect", "COLLECTION_REJECTED", {"reason": reason})
    # Scheduler observation failure is not numerical collection failure, and
    # must not write collection-failure.json or release a still-live job.
    terminal = _maybe_reconcile(args, profile, prepared, 'PASS')
    if terminal is not None and terminal != 'PASS':
        return _not_ready('collect', 'ALLOCATION_FAILED', {'terminalStatus':terminal})
    print(json.dumps(final, sort_keys=True))
    return 0


def _verify_terminal_record(root, prepared, *, require_pass=False):
    from runtime.yolo_allocation import validate_terminal_allocation
    path = Path(root)/'allocation-terminal.json'
    if any(p.is_symlink() for p in (path,*path.parents)):
        raise ClosureError('TERMINAL_RECORD_SYMLINK')
    value = _read_plane(path)
    if (not isinstance(value, dict) or set(value) != {
            'runId','candidateDigest','status','observation'}
            or value['runId'] != prepared['runId']
            or value['candidateDigest'] != prepared['candidateDigest']
            or value['status'] not in ('PASS','FAIL','INCOMPLETE')):
        raise ClosureError('TERMINAL_RECORD_BINDING')
    observed = value['observation']
    if not isinstance(observed, dict) or set(observed) != {'receipt','accounting','queue'}:
        raise ClosureError('TERMINAL_OBSERVATION')
    receipt = observed['receipt']
    try:
        validated = validate_terminal_allocation(observed['accounting'].encode('ascii'),
            observed['queue'].encode('ascii'), job_id=receipt['jobId'],
            submission_key=receipt['submissionKey'], partition=receipt['partition'], uid=receipt['uid'])
    except (KeyError, AttributeError, TypeError, ValueError) as exc:
        raise ClosureError('TERMINAL_OBSERVATION') from exc
    if receipt != validated:
        raise ClosureError('TERMINAL_RECEIPT_CHANGED')
    key = hashlib.sha256((prepared['contentIdentities']['dispatch']+':'+prepared['case']).encode()).hexdigest()
    expected_comment = 'spec183-'+hashlib.sha256((key+':'+prepared['runId']).encode()).hexdigest()
    if receipt['submissionKey'] != expected_comment:
        raise ClosureError('TERMINAL_SUBMISSION_BINDING')
    if require_pass:
        cleanup = _verify_srun_cleanup(Path(root), prepared)
        if (value['status'] != 'PASS' or receipt['state'] != 'COMPLETED'
                or receipt['exitCode'] != '0:0' or receipt['jobId'] != cleanup['jobId']):
            raise ClosureError('TERMINAL_NOT_PASS')
    return value


def _maybe_reconcile(args, profile, prepared, outcome):
    if not getattr(args, 'reconcile', False):
        return None
    if prepared['case'] == 'local-cpu' or os.environ.get('SLURM_JOB_ID'):
        raise ClosureError('TERMINAL_OBSERVER_CONTEXT')
    root = _safe_output(args.output)/args.run_id
    if root.parent != Path(profile['storage']['sharedRunRoot']):
        raise ClosureError('TERMINAL_SHARED_ROOT')
    from runtime.yolo_submission import SubmissionJournal
    from runtime.yolo_allocation import capture_terminal_allocation
    journal = SubmissionJournal(Path(profile['storage']['sharedLockRoot']),
        candidate_id=prepared['contentIdentities']['dispatch'], gate=prepared['case'])
    row = journal.get(args.run_id)
    if row['state'] not in ('SUBMITTED','RUNNING','PASS','FAIL','INCOMPLETE'):
        raise ClosureError('TERMINAL_JOB_NOT_ACKNOWLEDGED')
    path = root/'allocation-terminal.json'
    try:
        if path.exists():
            record = _verify_terminal_record(root, prepared)
            observation = record['observation']
        else:
            observation = capture_terminal_allocation(job_id=row['jobId'],
                submission_key=row['submissionKey'], partition=profile['cluster']['partition'],
                seconds=profile['timing']['progressTimeoutSeconds'])
            record = None
        receipt = observation['receipt']
        if (receipt['jobId'] != row['jobId'] or receipt['submissionKey'] != row['submissionKey']
                or receipt['partition'] != profile['cluster']['partition'] or receipt['uid'] != os.getuid()):
            raise ClosureError('TERMINAL_JOURNAL_BINDING')
        status = outcome if receipt['state']=='COMPLETED' and receipt['exitCode']=='0:0' else 'FAIL'
        if record is None:
            record = dict(runId=args.run_id,candidateDigest=prepared['candidateDigest'],
                          status=status,observation=observation)
            _write_readonly(path,record)
            _verify_terminal_record(root,prepared,require_pass=status=='PASS')
        elif record['status'] != status:
            raise ClosureError('TERMINAL_OUTCOME_CHANGED')
        if row['state'] in ('SUBMITTED','RUNNING'):
            journal.finish(args.run_id,row['jobId'],status)
        elif row['state'] != status:
            raise ClosureError('TERMINAL_JOURNAL_CHANGED')
        return status
    except (ValueError,OSError,TimeoutError,subprocess.SubprocessError) as exc:
        raise ClosureError('TERMINAL_RECONCILIATION:'+str(exc)) from exc


def _verify_srun_cleanup(root, prepared):
    value = _read_plane(root / 'srun-cleanup.json')
    if (not isinstance(value, dict) or set(value) != {'runId', 'jobId', 'candidateDigest', 'cleanup'}
            or value['runId'] != prepared['runId']
            or value['candidateDigest'] != prepared['candidateDigest']
            or not isinstance(value['jobId'], str)
            or re.fullmatch(r'[1-9][0-9]{0,9}', value['jobId']) is None
            or not isinstance(value['cleanup'], list) or len(value['cleanup']) != 1):
        raise ClosureError('SRUN_CLEANUP_BINDING')
    row = value['cleanup'][0]
    if (not isinstance(row, dict) or row.get('name') != 'yolo-srun'
            or row.get('kind') != 'finite' or row.get('reaped') is not True
            or row.get('forced') is not False or type(row.get('exitCode')) is not int
            or row['exitCode'] != 0 or 'cleanupError' in row or row.get('cleanupTimedOut')):
        raise ClosureError('SRUN_CLEANUP_FAILED')
    return value


def _allocated_context(args, *, task=False):
    if not os.environ.get("SLURM_JOB_ID"):
        raise ClosureError("ALLOCATION_REQUIRED")
    if args.case not in ('single-node-gpu', 'two-node-gpu'):
        raise ClosureError('NEGATIVE_RUNNER_NOT_WIRED')
    report, profile = _dispatch_report(Path(args.profile))
    if report.get('integrity') != 'VERIFIED':
        raise ClosureError('RUN_CONTENT_NOT_VERIFIED')
    prepared = _load_prepared(args.output, args.run_id)
    if (prepared['case'] != args.case or prepared['profileDigest'] != report['documentDigest']
            or prepared['contentIdentities'] != report['identities']):
        raise ClosureError('RUN_PREPARED_BINDING')
    if _safe_output(args.output) != Path(profile['storage']['sharedRunRoot']):
        raise ClosureError('RUN_SHARED_OUTPUT_BINDING')
    if Path(prepared['bundle']) != _safe_output(args.output) / args.run_id / 'bundle':
        raise ClosureError('RUN_SHARED_BUNDLE_BINDING')
    from runtime.yolo_submission import SubmissionJournal
    journal = SubmissionJournal(Path(profile['storage']['sharedLockRoot']),
        candidate_id=prepared['contentIdentities']['dispatch'], gate=args.case)
    row = journal.get(args.run_id)
    expected_state = 'RUNNING' if task else 'SUBMITTED'
    if not task and row['state'] in ('SUBMITTING','SUBMISSION_UNKNOWN'):
        import time
        deadline=time.monotonic()+profile['timing']['progressTimeoutSeconds']
        while row['state'] in ('SUBMITTING','SUBMISSION_UNKNOWN'):
            if time.monotonic()>=deadline:
                raise ClosureError('RUN_SUBMISSION_ACK_TIMEOUT')
            time.sleep(min(0.05,max(0,deadline-time.monotonic())))
            row=journal.get(args.run_id)
    if row['state'] != expected_state or row['jobId'] != os.environ['SLURM_JOB_ID']:
        raise ClosureError('RUN_JOURNAL_JOB_BINDING')
    expected = dict(job_id=row['jobId'], submission_key=row['submissionKey'],
                    partition=profile['cluster']['partition'], gpu_type=profile['cluster']['gpuClass'])
    return profile, prepared, journal, expected


def _rank(args) -> int:
    """Internal srun entry; no qualification is inferred from its zero exit."""
    profile, prepared, _, expected = _allocated_context(args, task=True)
    result = _enter_frozen(args, prepared, 'rank')
    if result is not None:
        return result
    from runtime.yolo_profile import resolve_provision_inputs
    from runtime.yolo_operator import execute_single_gpu_run, execute_distributed_rank
    resolved = resolve_provision_inputs(Path(args.profile), plan=prepared['plan'],
                                        runtime_candidate_digest=prepared['candidateDigest'])
    if args.case == 'single-node-gpu':
        execute_single_gpu_run(prepared=prepared, profile=profile, resolved=resolved,
                               allocation_expected=expected)
    else:
        rank = os.environ.get('SLURM_PROCID')
        if rank not in ('0', '1'):
            raise ClosureError('DISTRIBUTED_TASK_RANK')
        execute_distributed_rank(prepared=prepared, profile=profile, resolved=resolved,
                                 allocation_expected=expected, rank=int(rank))
    return 0


def _run(args) -> int:
    """Own one finite srun step and collect its actual retained outputs."""
    profile, prepared, journal, expected = _allocated_context(args)
    result = _enter_frozen(args, prepared, 'run')
    if result is not None:
        return result
    _gate_receipt(Path(args.profile), profile, CASE_GATE[args.case], prepared=prepared)
    from runtime.worker import run_finite_application
    root = Path(prepared['plan']['output'])
    nodes = 2 if args.case == 'two-node-gpu' else 1
    if nodes == 2:
        import secrets
        _write_readonly(root/'distributed-control.json', dict(runId=args.run_id,
            candidateDigest=prepared['candidateDigest'], jobId=expected['job_id'],
            probeId=secrets.token_hex(16)))
    command = ['/usr/bin/srun', '--exact', '--nodes='+str(nodes), '--ntasks='+str(nodes),
        '--ntasks-per-node=1', '--kill-on-bad-exit=1', '--mpi=none',
        '--cpus-per-task=' + str(profile['cluster']['cpusPerNode']), '--gpus-per-task=1',
        '/usr/bin/python3', '-B', str(Path(prepared['bundle']) / 'jobs/yolo/submit.py'),
        'rank', '--profile', str(Path(args.profile).absolute()), '--run-id', args.run_id,
        '--output', str(_safe_output(args.output)), '--case', args.case]
    journal.mark_running(args.run_id, expected['job_id'])
    cleanup = []
    try:
        run_finite_application('yolo-srun', command, root / 'srun.log', cleanup,
            seconds=profile['cluster']['wallTimeSeconds'] - profile['timing']['cleanupSeconds'],
            cleanup_seconds=profile['timing']['cleanupSeconds'], cwd=Path(prepared['bundle']))
    finally:
        _write_readonly(root / 'srun-cleanup.json', dict(runId=args.run_id,
            jobId=expected['job_id'], candidateDigest=prepared['candidateDigest'], cleanup=cleanup))
    _verify_srun_cleanup(root, prepared)
    if nodes == 2:
        from runtime.yolo_profile import resolve_provision_inputs
        from runtime.yolo_operator import finalize_distributed_run
        resolved = resolve_provision_inputs(Path(args.profile), plan=prepared['plan'],
                                            runtime_candidate_digest=prepared['candidateDigest'])
        finalize_distributed_run(prepared=prepared, profile=profile, resolved=resolved,
                                 allocation_expected=expected)
    # External collect/reconciliation must observe Slurm termination before
    # releasing the shared journal. A batch still executing cannot close it.
    return _collect(args)


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
    collect.add_argument('--reconcile', action='store_true',
        help='outside a job, verify Slurm termination and close the shared submission journal')
    runner = commands.add_parser("run", help=argparse.SUPPRESS)
    _common(runner, case=True)
    rank = commands.add_parser('rank', help=argparse.SUPPRESS)
    _common(rank, case=True)
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
        if args.action == 'rank':
            return _rank(args)
        return _run(args)
    except ClosureError as exc:
        return _error(exc)


if __name__ == "__main__":
    sys.exit(main())
