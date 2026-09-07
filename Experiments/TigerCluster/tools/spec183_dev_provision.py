"""Spec183 local development: run the containerized offline issuer for real.

Drives the existing host chain ``resolve_provision_inputs`` ->
``stage_provision_inputs`` -> ``provision_run`` against the base SIF on this
development host, producing a real ``tiger-yolo-preparation-v1`` receipt under
the already-prepared run.  This is the first genuinely executing step of the
Spec183 pipeline: the exact SIF, the real signed candidate package, the fixed
experiment key set, and the installed DI owners, with no fabricated inputs.

Development only: the profile's cluster apptainer locator
(``/usr/bin/apptainer`` on Tiger) is overridden with this host's apptainer
binary for the invocation; the profile itself is never modified.  A run's
issuer-inputs/public/private/prepare-output directories are exclusive and a
retry after failure requires removing them first (fail-closed, like
``provision_run`` itself).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Iterable

_TOOL_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_TOOL_DIR.parent))  # TigerCluster: runtime.*, tools.*
_REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_APPTAINER = "/opt/apptainer/1.5.3/bin/apptainer"
DEFAULT_PROFILE = str(_REPO_ROOT / "Experiments/TigerCluster/profiles/yolo-two-node.json")


def _prepared(output: Path, run_id: str) -> dict:
    receipt = Path(output) / run_id / "prepare.json"
    if not receipt.is_file() or receipt.is_symlink():
        raise SystemExit(f"no prepared run at {receipt}; run submit.py prepare first")
    value = json.loads(receipt.read_text())
    if (not isinstance(value, dict) or value.get("schema") != "tiger-yolo-prepared-run-v1"
            or value.get("status") != "PREPARED"):
        raise SystemExit(f"prepared run receipt invalid at {receipt}")
    return value


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--profile", default=DEFAULT_PROFILE)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--apptainer", default=DEFAULT_APPTAINER,
                        help="this host's apptainer (development override)")
    parser.add_argument("--seconds", type=float, default=900,
                        help="preparation budget for the container command")
    parser.add_argument("--cleanup-seconds", type=float, default=60)
    args = parser.parse_args(argv)

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
    runtime_profile["apptainer"] = args.apptainer  # host override, never the profile

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


if __name__ == "__main__":
    sys.exit(main())
