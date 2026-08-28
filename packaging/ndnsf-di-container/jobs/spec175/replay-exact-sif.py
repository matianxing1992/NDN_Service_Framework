#!/usr/bin/env python3
"""Host-orchestrated Spec175 G4 replay driver.

MiniNDN, Mininet, Open vSwitch, and host routing helpers stay on the host.
This driver never imports them into the SIF and never runs a host NDNSF
application as a substitute. The production runner must consume the
``SPEC175_RUNTIME_SIF``/``SPEC175_APPTAINER`` command-provider contract before
this driver can report a replay result.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[4]
RUNNER = ROOT / "Experiments/NDNSF_DI_StreamedGeneration_Minindn.py"
PRODUCTION_RUNNER = ROOT / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py"
TOPOLOGY = ROOT / "Experiments/Topology/spec175-host-gate.conf"
HOST_PREFLIGHT = ROOT / "packaging/ndnsf-di-container/bin/spec175-host-substrate-preflight"
CASES = tuple(f"M{i:02d}" for i in range(1, 11))
WORKLOAD_SEED = 1750001
REPETITIONS = 3


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return "sha256:" + value.hexdigest()


def without_sha256_prefix(value: str) -> str:
    """Keep the host replay driver compatible with Python 3.8 MiniNDN."""
    return value[7:] if value.startswith("sha256:") else value


def fail(message: str) -> None:
    raise SystemExit("SPEC175_G4_REPLAY_" + message)


def load_host_gate():
    path = ROOT / "packaging/ndnsf-di-container/lib/spec175_host_gate.py"
    spec = importlib.util.spec_from_file_location("spec175_host_gate", path)
    if spec is None or spec.loader is None:
        fail("HOST_GATE_VALIDATOR_UNAVAILABLE")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_manifest(path: Path, sif: Path) -> dict:
    if os.geteuid() != 0:
        fail("REQUIRES_ROOT_FOR_MININDN")
    if not RUNNER.is_file() or not TOPOLOGY.is_file():
        fail("HOST_INPUT_MISSING")
    if not sif.is_file() or sif.stat().st_size == 0:
        fail("SIF_MISSING")
    try:
        result = load_host_gate().validate_host_gate(path, ROOT)
    except Exception as exc:
        fail("HOST_GATE_NOT_PASS:" + str(exc))
    if result.get("total") != 30 or result.get("passed") != 30:
        fail("HOST_GATE_NOT_30_OF_30")
    return result


def inspect_sif(apptainer: str, sif: Path, expected: str) -> dict:
    actual = digest(sif)
    if expected and without_sha256_prefix(actual) != without_sha256_prefix(expected):
        fail("SIF_DIGEST_MISMATCH")
    check = subprocess.run([apptainer, "inspect", "--json", str(sif)],
                           text=True, capture_output=True, check=False)
    if check.returncode != 0:
        fail("SIF_INSPECT_FAILED")
    try:
        labels = json.loads(check.stdout)["data"]["attributes"]["labels"]
    except (KeyError, json.JSONDecodeError) as exc:
        fail("SIF_LABELS_INVALID:" + type(exc).__name__)
    if labels.get("org.ndnsf.di.build-boundary") != "container-runtime-in-sif":
        fail("SIF_BUILD_BOUNDARY_INVALID")
    return {"sha256": actual, "bytes": sif.stat().st_size, "labels": labels}


def verify_source_identity(host: dict, sif_info: dict, source_path: Path) -> None:
    """Verify the candidate SIF's complete seal after host-G3 overlap checks."""
    try:
        source = json.loads(source_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail("SOURCE_SEAL_INVALID:" + type(exc).__name__)
    seal_digest = source.get("sealDigest")
    image_digest = sif_info.get("labels", {}).get("org.ndnsf.di.source-seal")
    if not seal_digest or image_digest != seal_digest:
        fail("SOURCE_SEAL_SIF_MISMATCH")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host-gate-manifest", required=True, type=Path)
    parser.add_argument("--source-seal", required=True, type=Path,
                        help="complete candidate source seal embedded in the SIF")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--sif", type=Path,
                        default=Path(os.environ.get("SPEC175_RUNTIME_SIF", "")))
    parser.add_argument("--apptainer",
                        default=os.environ.get("SPEC175_APPTAINER", ""))
    parser.add_argument("--sif-sha256", default=os.environ.get("SPEC175_SIF_SHA256", ""))
    parser.add_argument("--seed", type=int, default=WORKLOAD_SEED)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if not args.apptainer:
        fail("APPTAINER_REQUIRED")
    if not args.sif:
        fail("SIF_REQUIRED")

    host = load_manifest(args.host_gate_manifest.resolve(), args.sif.resolve())
    sif = inspect_sif(args.apptainer, args.sif.resolve(), args.sif_sha256)
    verify_source_identity(host, sif, args.source_seal.resolve())
    host_preflight = args.output.parent / "host-substrate-preflight.json"
    preflight = subprocess.run([
        str(HOST_PREFLIGHT),
        "--host-gate-manifest", str(args.host_gate_manifest.resolve()),
        "--repository-root", str(ROOT),
        "--topology-file", str(TOPOLOGY),
        "--replay-driver", str(Path(__file__).resolve()),
        "--apptainer", args.apptainer,
        "--expected-apptainer", "1.5.3",
        "--output", str(host_preflight),
    ], text=True, capture_output=True, check=False)
    if preflight.returncode != 0:
        fail("HOST_SUBSTRATE_PREFLIGHT_FAILED")

    source = PRODUCTION_RUNNER.read_text(encoding="utf-8")
    required_runtime_contract = (
        "SPEC175_RUNTIME_SIF", "sif_exec_prefix", "--cleanenv",
        "SPEC175_SIF_HOST_PROCESS_FALLBACK",
    )
    if not PRODUCTION_RUNNER.is_file() or any(
            marker not in source for marker in required_runtime_contract):
        fail("PRODUCTION_RUNNER_HAS_NO_SIF_COMMAND_PROVIDER")

    entries = []
    for repetition in range(1, REPETITIONS + 1):
        for case in CASES:
            run_id = f"{case}-r{repetition}"
            output = args.output.parent / run_id
            command = [
                sys.executable, str(RUNNER),
                "--case", case,
                "--seed", str(args.seed),
                "--output-dir", str(output),
                "--tiny-fixture-root", str(ROOT / "tests/fixtures/spec175/tiny-causal-lm-v1"),
                "--topology-file", str(TOPOLOGY),
                "--runtime-sif", str(args.sif.resolve()),
                "--runtime-apptainer", args.apptainer,
            ]
            entries.append({"case": case, "repetition": repetition,
                            "runId": run_id, "command": command,
                            "runtimeSif": str(args.sif.resolve()),
                            "apptainer": args.apptainer})
            if args.dry_run:
                continue
            env = dict(os.environ)
            env.update({
                "SPEC175_RUN_REAL_MININDN": "1",
                "SPEC175_RUNTIME_SIF": str(args.sif.resolve()),
                "SPEC175_APPTAINER": args.apptainer,
                "SPEC175_SIF_SHA256": sif["sha256"],
            })
            output.mkdir(parents=True, exist_ok=True)
            completed = subprocess.run(command, cwd=ROOT, env=env,
                                       text=True, capture_output=True, check=False)
            (output / "g4.stdout").write_text(completed.stdout, encoding="utf-8")
            (output / "g4.stderr").write_text(completed.stderr, encoding="utf-8")
            result_path = output / "spec175-case-result.json"
            passed = completed.returncode == 0 and result_path.is_file()
            entries[-1].update({"returncode": completed.returncode,
                                "status": "PASS" if passed else "FAIL"})

    passed = sum(entry.get("status") == "PASS" for entry in entries)
    result = {
        "schema": "spec175-g4-host-orchestrated-replay-v1",
        "status": "PASS" if not args.dry_run and len(entries) == 30 and passed == 30 else "FAIL",
        "layer": "host-substrate-plus-exact-sif-runtime",
        "repetitionsPerCase": REPETITIONS,
        "hostGate": host,
        "sourceSeal": {"path": str(args.source_seal.resolve()),
                        "sha256": digest(args.source_seal.resolve())},
        "hostPreflight": str(host_preflight),
        "sif": sif,
        "entries": entries,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(json.dumps({"status": result["status"], "output": str(args.output)}, sort_keys=True))
    return 0 if result["status"] == "PASS" else 4


if __name__ == "__main__":
    raise SystemExit(main())
