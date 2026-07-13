from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys


REPO = Path(__file__).resolve().parents[4]
TOOLS = REPO / "tools" / "ndnsf-di"
FIXTURES = REPO / "tests" / "container" / "itiger-qwen" / "fixtures"
CONTRACTS = REPO / "specs" / "109-ndnsf-di-itiger-qwen-scaling" / "contracts"
D = "sha256:" + "a" * 64


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def load_tool(name: str):
    key = "spec109_test_" + name
    if key in sys.modules:
        return sys.modules[key]
    spec = importlib.util.spec_from_file_location(key, TOOLS / f"{name}.py")
    if spec is None or spec.loader is None:
        raise ImportError(name)
    module = importlib.util.module_from_spec(spec)
    sys.modules[key] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop(key, None)
        raise
    return module


def source_snapshot():
    return {
        "headCommit": "b" * 40, "treeDigest": D, "capturedAt": "2026-07-13T00:00:00Z",
        "worktreeState": "CLEAN", "binaryDiffDigest": None,
        "untrackedManifestDigest": None, "untrackedArchiveDigest": None,
        "includedPaths": ["tracked-files@HEAD"], "excludedPaths": ["results/"],
        "snapshotDigest": D,
    }


def predecessor_gate():
    ids = [f"107:T{i:03d}" for i in range(27, 39)] + [f"108:T{i:03d}" for i in range(91, 103)]
    return {
        "schemaVersion": "1.0", "requiredTaskIds": ids,
        "entries": {task: {
            "requiredStatus": "PASS", "observedStatus": "PASS", "schemaVersion": "v1",
            "artifactPath": f"evidence/{task.replace(':', '-')}.json",
            "artifactDigest": D, "identityDigest": D,
        } for task in ids},
        "gateDigest": D,
    }


def profile(size: str = "0.5B"):
    token = size.lower().replace("b", "B")
    return {
        "schemaVersion": "2.0", "candidateId": "spec109-candidate-a",
        "sourceSnapshot": source_snapshot(), "predecessorGate": predecessor_gate(),
        "deploymentBinding": {"spec108ProfileDigest": D, "releaseDigest": D,
                              "resolvedResourceFingerprint": D},
        "model": {"repository": f"Qwen/Qwen2.5-{token}-Instruct", "revision": "c" * 40,
                  "sizeClass": size, "tokenizerDigest": D, "licenseDigest": D,
                  "dtype": "float16", "quantization": "none", "artifactSetDigest": D},
        "workload": {"promptSetDigest": D, "promptCount": 2, "contextTokens": 32,
                     "maxNewTokens": 2, "decode": "greedy", "arrivalMode": "open-loop",
                     "targetRps": 1.0, "maxInFlight": 2, "requestTimeoutMs": 30000,
                     "cacheState": "controlled-reset", "warmupRequests": 2,
                     "measurementSeconds": 60, "repetitions": 3,
                     "loggingProfile": "warn-sampled", "runOrderSeed": 109},
        "stages": [{"role": "stage0", "artifactDigest": D, "logicalGpu": 0,
                    "sessionOptionsDigest": D}],
        "fallbackPolicy": "disabled",
    }


def matrix():
    return {
        "schemaVersion": "2.0", "campaignId": "spec109-campaign-a",
        "campaignGateDigest": D, "locked": True, "finalized": False,
        "models": ["0.5B"],
        "cells": {"c1": {"candidateId": "candidate-a", "modelSize": "0.5B",
                           "mode": "ndnsf-di-correctness", "repetition": 0,
                           "state": "NOT_STARTED", "reasonCode": "", "runId": None,
                           "evidenceDigest": None, "gateScope": "none", "gateId": None,
                           "gateDigest": None}},
        "runs": {}, "physicalProduction": "DEFERRED",
    }


def unavailable_distribution(count: int = 0):
    p = {"status": "UNAVAILABLE_INSUFFICIENT_N", "value": None}
    return {"count": count, "mean": None, "p50": dict(p), "p95": dict(p), "p99": dict(p)}


def evidence():
    dist = unavailable_distribution()
    return {
        "schemaVersion": "2.0", "runId": "r1", "cellId": "c1",
        "sourceSnapshotDigest": D, "predecessorGateDigest": D,
        "deploymentProfileDigest": D, "candidateId": "candidate-a",
        "model": {"repository": "Qwen/Qwen2.5-0.5B-Instruct", "revision": "c" * 40,
                  "tokenizerDigest": D, "licenseDigest": D, "dtype": "float16",
                  "artifactSetDigest": D},
        "container": {"ociDigest": D, "sifSha256": D, "apptainerVersion": "1.3.3"},
        "slurm": {"jobId": "145855", "account": "devs", "qos": "normal",
                  "partition": "bigTiger", "walltimeSeconds": 300, "cpus": 2,
                  "memoryBytes": 8000000000, "requestedTres": "gres/gpu=1",
                  "allocatedTres": "gres/gpu=1", "nodeList": ["itiger07"],
                  "state": "COMPLETED", "exitCode": 0},
        "workload": {"profileDigest": D, "arrivalMode": "open-loop", "targetRps": 1.0,
                     "maxInFlight": 2, "requestTimeoutMs": 30000, "promptCount": 2,
                     "cacheState": "controlled-reset", "warmupRequests": 2,
                     "measurementSeconds": 0},
        "backend": {"requested": "cuda", "fallbackAllowed": False, "fallbackUsed": False,
                    "fullCuda": True, "profileDigest": D,
                    "nodeAssignments": [{"role": "stage0", "nodeName": "MatMul_0",
                                         "provider": "CUDAExecutionProvider", "modelNode": True}],
                    "gpuMappings": [{"role": "stage0", "uuid": "GPU-test", "model": "RTX 5000",
                                     "containerDevice": 0}]},
        "correctness": {"promptSetDigest": D, "inputTokenIds": [1], "outputTokenIds": [2],
                        "referenceOutputTokenIds": [2], "exactMatch": True,
                        "checkpoints": [{"name": "logits", "kind": "logits", "rtol": 0.01,
                                         "atol": 0.001, "maxAbsError": 0.0,
                                         "maxRelError": 0.0, "pass": True}]},
        "metrics": {"completed": 1, "failed": 0, "ttftMs": dict(dist),
                    "interTokenMs": dict(dist), "tokensPerSecond": dict(dist),
                    "requestThroughput": dict(dist), "confidenceIntervals": {},
                    "resources": {}, "stages": [], "dependencies": []},
        "comparison": {"role": "correctness-oracle", "comparisonFingerprint": D,
                       "matchedBaselineCellId": None, "matched": False},
        "terminal": {"status": "PASS", "reasonCode": "", "originalExitCode": 0},
        "promotion": {"durablePath": "/project/tma1/ndnsf-di/evidence/r1",
                      "complete": True, "manifestDigest": D},
        "authority": {"substrate": "PASS", "oracle": "PASS", "artifact": "PASS",
                      "stagedBaseline": "DEFERRED", "candidateCorrectness": "DEFERRED",
                      "candidatePerformance": "DEFERRED", "physicalProduction": "DEFERRED",
                      "physicalProductionOwner": "Spec 106"},
        "checksums": {"stdout": D},
    }
