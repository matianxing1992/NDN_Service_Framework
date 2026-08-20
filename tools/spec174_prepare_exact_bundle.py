#!/usr/bin/env python3
"""Prepare a current, self-contained Spec174 exact-SIF network bundle.

The bundle is generated from the current NativeTracer planner and is sealed
atomically.  It deliberately does not reuse an older Spec170 bundle: the
NDNSF_DATA_V1 expected byte counts must come from the same planner revision as
the candidate SIF.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PLAN_TRACER = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/native_di_tracer/plan_tracer.py"
)
USER_DRIVER = (
    ROOT
    / "examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py"
)
NFD_TEMPLATE = (
    ROOT
    / "packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/nfd.conf.in"
)
WORKLOADS = {
    "spec170-d0-current-sif-workload.sh": (
        ROOT / "tests/fixtures/spec174/spec174-d0-sif-workload.sh"
    ),
    "spec170-d1-current-sif-workload.sh": (
        ROOT / "tests/fixtures/spec174/spec174-d1-sif-workload.sh"
    ),
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def git_revision() -> str:
    return subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()


def prepare(output: Path, *, activation_pad_bytes: int = 0) -> dict[str, object]:
    output = output.expanduser().resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite existing bundle: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent))
    try:
        policy = temporary / "policy-bundle"
        subprocess.run(
            [
                "python3",
                str(PLAN_TRACER),
                "--out",
                str(policy),
                "--activation-pad-bytes",
                str(activation_pad_bytes),
            ],
            cwd=ROOT,
            check=True,
        )
        for source, destination in (
            (NFD_TEMPLATE, temporary / "nfd.conf.in"),
            (USER_DRIVER, temporary / "user_driver.py"),
        ):
            shutil.copy2(source, destination)
        for name, source in WORKLOADS.items():
            shutil.copy2(source, temporary / name)

        files = {}
        for path in sorted(item for item in temporary.rglob("*") if item.is_file()):
            relative = str(path.relative_to(temporary))
            files[relative] = {"bytes": path.stat().st_size, "sha256": sha256(path)}
        plan = json.loads(
            (policy / "native-execution-plan.json").read_text(encoding="utf-8")
        )
        service = next(
            item for item in plan["services"]
            if item["service"] == "/Inference/NativeTracer"
        )
        manifest = {
            "schema": "spec174-exact-sif-bundle-v1",
            "sourceRevision": git_revision(),
            "planner": str(PLAN_TRACER.relative_to(ROOT)),
            "activationPadBytes": activation_pad_bytes,
            "service": service["service"],
            "roles": service.get("roles", []),
            "dependencyBounds": [
                {
                    "scope": item.get("keyScope", ""),
                    "expectedBytes": item.get("expectedBytes", 0),
                    "expectedSegments": item.get("expectedSegments", 0),
                }
                for item in service.get("dependencies", [])
            ],
            "files": files,
        }
        (temporary / "bundle-manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        temporary.rename(output)
        return manifest
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--activation-pad-bytes", type=int, default=0)
    args = parser.parse_args()
    if args.activation_pad_bytes < 0:
        raise SystemExit("--activation-pad-bytes must be non-negative")
    result = prepare(args.output, activation_pad_bytes=args.activation_pad_bytes)
    print(json.dumps({
        "status": "PASS",
        "output": str(Path(args.output).expanduser().resolve()),
        "sourceRevision": result["sourceRevision"],
        "dependencyBounds": result["dependencyBounds"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
