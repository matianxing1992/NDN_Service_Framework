from __future__ import annotations

import json
import importlib.util
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[3]
LIB = REPO / "packaging" / "ndnsf-di-container" / "lib"
if str(LIB) not in sys.path:
    sys.path.insert(0, str(LIB))

FIXTURES = REPO / "tests" / "container" / "fixtures"
SCHEMAS = REPO / "packaging" / "ndnsf-di-container" / "schemas"


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def load_impl(name: str):
    module_name = "ndnsf_container_" + name.replace("/", "_")
    existing = sys.modules.get(module_name)
    if existing is not None:
        return existing
    spec = importlib.util.spec_from_file_location(module_name, LIB / f"{name}.py")
    if spec is None or spec.loader is None:
        raise ImportError(name)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop(module_name, None)
        raise
    return module


def valid_slurm_evidence() -> dict:
    sha = "a" * 64
    return {
        "schemaVersion": "1.0",
        "runId": "itiger-rtx5000-001",
        "candidate": {"candidateId": "spec107-c1", "sourceRevision": "0123456"},
        "release": {
            "releaseId": "spec108-r1",
            "manifestDigest": sha,
            "ociReference": f"registry.example/ndnsf-di@sha256:{sha}",
            "ociDigest": f"sha256:{sha}",
        },
        "profileDigest": sha,
        "materialization": {
            "adapter": "slurm-apptainer", "type": "sif", "id": f"sha256:{sha}",
            "path": "/project/tma1/ndnsf-di/sif/r1.sif", "runtimeVersion": "1.3.3", "verified": True,
        },
        "adapterEvidence": {
            "kind": "slurm-apptainer", "jobId": "145855", "partition": "bigTiger",
            "nodeList": ["itiger07"], "requestedTres": {}, "allocatedTres": {},
            "state": "COMPLETED", "exitCode": "0:0", "apptainerVersionCompute": "1.3.3",
            "evidenceCopy": "PASS",
        },
        "storage": {
            "projectRoot": "/project/tma1/ndnsf-di", "scratchRoot": "/tmp/ndnsf-di-145855-run",
            "evidenceRoot": "/project/tma1/ndnsf-di/evidence/run", "capacityChecks": [{}],
            "scratchWriteFsync": "PASS", "promotionManifestDigest": sha,
        },
        "network": {"topology": "single-node", "nfdMode": "job-scoped", "status": "PASS"},
        "backend": {
            "requested": "onnxruntime-cuda", "observed": "cuda", "allowCpuFallback": False,
            "fallbackOccurred": False, "status": "PASS", "physicalGpus": [],
        },
        "tests": [],
        "authority": {
            "substrate": "PASS", "candidate": "PASS", "physicalProduction": "DEFERRED",
            "physicalProductionOwner": "Spec 106",
        },
        "redaction": {"status": "PASS", "scanner": "unit", "findings": 0},
        "outcome": "PASS", "startedAt": "2026-07-12T12:00:00Z", "finishedAt": "2026-07-12T12:01:00Z",
    }
