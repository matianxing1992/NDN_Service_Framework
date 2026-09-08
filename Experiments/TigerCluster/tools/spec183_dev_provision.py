"""Spec183 local development: real containerized execution on this host.

Two subcommands drive the maintained host chain against the base SIF:

``provision`` runs the containerized offline issuer
(``resolve_provision_inputs`` -> ``stage_provision_inputs`` ->
``provision_run``) and pins a real ``tiger-yolo-preparation-v1`` receipt under
the prepared run.  This is the first genuinely executing Spec183 step: the
exact SIF, the real signed candidate package, the fixed experiment key set,
and the installed DI owners, with no fabricated inputs.

``run-local`` then drives one complete ``local-cpu`` rank through
``run_rank`` (NFD forwarder, network setup, Controller/Repo/Providers, the
warmup + measured requests against the real ONNX model on CPU), accepting
each request into the collector directory.  Verdicts against the oracle
belong to the T005/T006 owners; this tool records the execution, not a PASS.

Development only: the profile's cluster apptainer locator
(``/usr/bin/apptainer`` on Tiger) is overridden with this host's apptainer
binary for every invocation; the profile itself is never modified.  A run's
issuer-inputs/public/private/prepare-output/node/startup/completion
directories are exclusive and a retry after failure requires removing them
first (fail-closed, like ``provision_run`` itself).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Iterable

_TOOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_TOOL_DIR.parent))  # TigerCluster: runtime.*, tools.*
_REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_APPTAINER = "/opt/apptainer/1.5.3/bin/apptainer"
DEFAULT_PROFILE = str(_REPO_ROOT / "Experiments/TigerCluster/profiles/yolo-two-node.json")


def _sha256_file(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def _prepared(output: Path, run_id: str) -> dict:
    receipt = Path(output) / run_id / "prepare.json"
    if not receipt.is_file() or receipt.is_symlink():
        raise SystemExit(f"no prepared run at {receipt}; run submit.py prepare first")
    value = json.loads(receipt.read_text())
    if (not isinstance(value, dict) or value.get("schema") != "tiger-yolo-prepared-run-v2"
            or value.get("status") != "PREPARED"):
        raise SystemExit(f"prepared run receipt invalid at {receipt}")
    return value


def _runtime_profile(profile: Path, prepared: dict, apptainer: str) -> dict:
    """Operator profile with absolute paths plus this host's apptainer/sif."""
    from runtime.yolo_profile import load_operator_profile, resolve_provision_inputs
    loaded = load_operator_profile(profile, stage="dispatch")["profile"]
    resolved = resolve_provision_inputs(profile, plan=prepared["plan"],
                                        runtime_candidate_digest=prepared["candidateDigest"])
    runtime = dict(resolved["runtimeProfile"])
    runtime["apptainer"] = apptainer  # host override, never the profile
    merged = dict(loaded)
    merged.update(runtime)
    return merged


def _mkdir700(path: Path) -> None:
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise SystemExit(f"path lies below a symlink: {path}")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)


def provision(args) -> int:
    from runtime import yolo_operator as operator
    from runtime.yolo_operator import OperatorError
    from runtime.yolo_profile import resolve_provision_inputs

    profile = Path(args.profile).resolve()
    prepared = _prepared(Path(args.output).resolve(), args.run_id)
    bundle = Path(prepared["bundle"])
    plan = prepared["plan"]
    candidate = prepared["candidateDigest"]
    harness_digest = prepared["harnessManifestSha256"]

    resolved = resolve_provision_inputs(profile, plan=plan,
                                        runtime_candidate_digest=candidate)
    runtime_profile = dict(resolved["runtimeProfile"])
    runtime_profile["apptainer"] = args.apptainer  # host override

    run_root = Path(args.output).resolve() / args.run_id
    inputs = run_root / "issuer-inputs"
    public = run_root / "public"
    private = run_root / "private"
    prepare_output = run_root / "prepare-output"
    for directory in (public, private, prepare_output):
        if any(p.is_symlink() for p in (directory, *directory.parents)):
            raise SystemExit(f"path lies below a symlink: {directory}")
        if not directory.is_dir():
            directory.mkdir(mode=0o700, parents=True)
    (private / "root").mkdir(mode=0o700, exist_ok=True)  # --home target
    try:
        descriptor_digest = operator.stage_provision_inputs(resolved, inputs)
    except OperatorError as exc:
        print(json.dumps({"status": "REJECTED", "reason": str(exc)}, sort_keys=True))
        return 2
    print(json.dumps({"status": "STAGED", "descriptorDigest": descriptor_digest,
                      "inputs": str(inputs)}, sort_keys=True))
    try:
        receipt = operator.provision_run(
            runtime_profile=runtime_profile, bundle=bundle, harness_digest=harness_digest,
            inputs=inputs, descriptor_digest=descriptor_digest,
            package=Path(resolved["package"]), public=public, private=private,
            output=prepare_output, seconds=args.seconds,
            cleanup_seconds=args.cleanup_seconds)
    except OperatorError as exc:
        print(json.dumps({"status": "REJECTED", "reason": str(exc)}, sort_keys=True))
        return 2
    print(json.dumps(receipt, sort_keys=True))
    return 0


def run_local(args) -> int:
    from runtime import yolo_operator as operator
    from runtime.yolo_operator import OperatorError, run_rank

    profile = Path(args.profile).resolve()
    output = Path(args.output).resolve()
    prepared = _prepared(output, args.run_id)
    plan = prepared["plan"]
    run_root = output / args.run_id
    public = run_root / "public"
    private = run_root / "private"
    preparation_digest = _sha256_file(public / "preparation.json")
    merged = _runtime_profile(profile, prepared, args.apptainer)

    homes = {role: private / role for role in plan["identities"]}
    # from_preparation requires the worker output to be plan.output/node<rank>;
    # the node directory (NFD socket) is a separate path.
    node = run_root / "node"
    startup_dir = run_root / "startup"
    completion_dir = run_root / "completion"
    worker_output = run_root / "node0"
    for directory in (node, startup_dir, completion_dir, worker_output):
        _mkdir700(directory)
    startup_options = {"repo_free_bytes": 1 << 30,
                       "permission_wait_ms": args.permission_wait_ms,
                       "network_probe_seconds": args.network_probe_seconds}
    request_options = {
        "package": _package_path(run_root),
        **_request_identifiers(public),
        "permission_wait_ms": args.permission_wait_ms,
        "request_deadline_ms": args.request_deadline_ms,
        "process_timeout_seconds": args.process_timeout_seconds,
        "protection_epoch": merged["security"]["protectionEpoch"]}

    accepted = []

    def accept(request, request_output):
        accepted.append(request.get("requestId"))
        destination = Path(request_output)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(request, sort_keys=True))
        print(json.dumps({"status": "REQUEST_ACCEPTED",
                          "requestId": request.get("requestId")}, sort_keys=True))

    print(json.dumps({"status": "RUN_LOCAL_START", "runId": args.run_id,
                      "preparationDigest": preparation_digest,
                      "candidateDigest": prepared["candidateDigest"]}, sort_keys=True))
    try:
        result = run_rank(
            plan=plan, profile=merged, mode="local-cpu", rank=0,
            bundle=Path(prepared["bundle"]), public=public, homes=homes,
            output=worker_output, node=node,
            startup_directory=startup_dir, completion_directory=completion_dir,
            preparation_digest=preparation_digest,
            candidate_digest=prepared["candidateDigest"],
            endpoints=[{"rank": 0, "address": "127.0.0.1",
                        "port": merged["cluster"]["tcpPort"]}],
            startup_seconds=args.startup_seconds,
            completion_seconds=args.completion_seconds,
            startup_options=startup_options, request_options=request_options,
            accept_request=accept)
    except (OperatorError, ValueError, RuntimeError, TimeoutError) as exc:
        print(json.dumps({"status": "REJECTED", "reason": str(exc),
                          "accepted": accepted}, sort_keys=True))
        return 2
    print(json.dumps({"status": "RUN_LOCAL_DONE", "result": result,
                      "accepted": accepted}, sort_keys=True))
    return 0


def _package_path(run_root: Path) -> str:
    """The spec183-signed canonical package root (manifest.json parent)."""
    candidates = [
        _REPO_ROOT / "Experiments/TigerCluster/.cache/model/spec183-signed/canonical-package",
    ]
    for candidate in candidates:
        if (candidate / "manifest.json").is_file():
            return str(candidate)
    raise SystemExit("spec183-signed canonical package not found")


def _request_identifiers(public: Path) -> dict:
    """Catalogue names from the real preparation receipt in /config."""
    from runtime.yolo_profile import _read_plane
    receipt = _read_plane(public / "preparation.json")
    if (not isinstance(receipt, dict) or not isinstance(receipt.get("catalogueDataName"), str)
            or not isinstance(receipt.get("catalogueSigner"), str)):
        raise SystemExit(f"preparation receipt missing catalogue names: {public}")
    return {"catalog_data_name": receipt["catalogueDataName"],
            "catalog_signer": receipt["catalogueSigner"]}


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    provision_p = sub.add_parser("provision", help="run the containerized offline issuer")
    provision_p.add_argument("--profile", default=DEFAULT_PROFILE)
    provision_p.add_argument("--run-id", required=True)
    provision_p.add_argument("--output", type=Path, required=True)
    provision_p.add_argument("--apptainer", default=DEFAULT_APPTAINER)
    provision_p.add_argument("--seconds", type=float, default=900)
    provision_p.add_argument("--cleanup-seconds", type=float, default=60)

    local_p = sub.add_parser("run-local", help="drive one local-cpu rank end to end")
    local_p.add_argument("--profile", default=DEFAULT_PROFILE)
    local_p.add_argument("--run-id", required=True)
    local_p.add_argument("--output", type=Path, required=True)
    local_p.add_argument("--apptainer", default=DEFAULT_APPTAINER)
    local_p.add_argument("--startup-seconds", type=float, default=120)
    local_p.add_argument("--completion-seconds", type=float, default=300)
    local_p.add_argument("--permission-wait-ms", type=int, default=30000)
    local_p.add_argument("--network-probe-seconds", type=float, default=20)
    local_p.add_argument("--request-deadline-ms", type=int, default=60000)
    local_p.add_argument("--process-timeout-seconds", type=float, default=300)

    args = parser.parse_args(argv)
    if args.command == "provision":
        return provision(args)
    return run_local(args)


if __name__ == "__main__":
    sys.exit(main())
