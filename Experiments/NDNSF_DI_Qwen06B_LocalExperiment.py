#!/usr/bin/env python3
"""Layered local experiment entry point for the native Qwen3-0.6B MiniNDN run.

The command mirrors the TigerCluster boundary without building a SIF:

``check`` -> ``prepare`` -> ``run``

``local`` is a convenience composition of the three steps.  ``check`` and
``prepare`` never start MiniNDN.  ``run`` consumes an immutable preparation
directory and delegates protocol/model behavior to the existing C++-first
MiniNDN runner.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "Experiments") not in sys.path:
    sys.path.insert(0, str(ROOT / "Experiments"))
from native_resource_guard import run_guarded, validate_limits
from native_placement_evidence import (
    collect_native_placement_evidence, native_placement_request_ids,
)

RUNNER = ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"
PROFILE_SCHEMA = "ndnsf-di-qwen06b-local-experiment-v1"
RUN_SCHEMA = "ndnsf-di-qwen06b-local-run-v2"
# The real Qwen3-0.6B canonical external initializer is about 1.5 GiB.  Keep
# a finite admission bound while allowing that artifact through the wrapper.
MODEL_SOURCE_MAX_BYTES = 2 * 1024 * 1024 * 1024
MAX_NEW_TOKENS = 1025
DEFAULT_ARTIFACT_CACHE_ROOT = Path("/var/tmp/ndnsf-di-native-artifacts")
RUN_ID_RE = re.compile(r"^[a-z][a-z0-9-]{1,47}$")
NODE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*$")
PHASES = ("machine", "candidate", "model", "bundle", "minindn", "workload", "cleanup")


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def canonical_digest(value: Any) -> str:
    raw = json.dumps(value, ensure_ascii=False, sort_keys=True,
                     separators=(",", ":")).encode("utf-8")
    return "sha256:" + hashlib.sha256(raw).hexdigest()


def normalized_model_identity(manifest: dict[str, Any]) -> dict[str, str]:
    """Return the model fields that control native compatibility and caching."""
    quantization = str(manifest.get("quantization", "none")).lower()
    return {
        "family": str(manifest.get("modelFamily", "qwen")),
        "name": str(manifest.get("model", "")),
        "dtype": str(manifest.get("dtype", "")),
        "modelFormat": str(manifest.get("modelFormat", "onnx")),
        "quantization": quantization,
        "quantizationSubtype": (
            "weight_only_int8" if quantization == "int8" else quantization),
    }


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2,
                               sort_keys=True) + "\n", encoding="utf-8")


def resolve_path(value: str, field: str) -> Path:
    if not isinstance(value, str) or not value.strip() or "\x00" in value:
        raise ValueError(f"{field} must be a non-empty path")
    path = Path(value).expanduser()
    return path.resolve() if path.is_absolute() else (ROOT / path).resolve()


def resolve_artifact_cache_root(value: Path, run_root: Path) -> Path:
    """Resolve a persistent cache root and keep it outside one run's evidence."""
    cache_root = value.expanduser().resolve()
    resolved_run_root = run_root.expanduser().resolve()
    if cache_root == resolved_run_root or resolved_run_root in cache_root.parents:
        raise ValueError("CACHE_ROOT_MUST_BE_OUTSIDE_RUN_ROOT")
    if cache_root.exists() and not cache_root.is_dir():
        raise ValueError("CACHE_ROOT_NOT_DIRECTORY")
    return cache_root


def load_profile(path: Path) -> tuple[dict[str, Any], str]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"PROFILE_UNREADABLE:{path}") from exc
    if not isinstance(payload, dict) or payload.get("schema") != PROFILE_SCHEMA:
        raise ValueError("PROFILE_SCHEMA_INVALID")
    required = {"schema", "profileId", "topologyFile", "stageNodes",
                "controllerNode", "userNode", "buildDir"}
    missing = sorted(required - set(payload))
    if missing:
        raise ValueError("PROFILE_FIELDS_MISSING:" + ",".join(missing))
    unknown = sorted(set(payload) - {
        "schema", "profileId", "topologyFile", "stageNodes", "controllerNode",
        "userNode", "buildDir", "controllerBinary", "authorityBinary",
        "requesterBinary", "providerBinary", "assemblyWorkerBinary", "oracleBinary", "outputRoot", "resourceLimits",
    })
    if unknown:
        raise ValueError("PROFILE_UNKNOWN_FIELDS:" + ",".join(unknown))
    profile_id = payload.get("profileId")
    if not isinstance(profile_id, str) or not re.fullmatch(r"[a-z][a-z0-9-]{1,47}", profile_id):
        raise ValueError("PROFILE_ID_INVALID")
    stages = payload.get("stageNodes")
    if (not isinstance(stages, list) or len(stages) < 2 or len(stages) > 4 or
            any(not isinstance(node, str) or not NODE_RE.fullmatch(node) for node in stages) or
            len(set(stages)) != len(stages)):
        raise ValueError("PROFILE_STAGE_NODES_INVALID")
    for field in ("controllerNode", "userNode"):
        if not isinstance(payload[field], str) or not NODE_RE.fullmatch(payload[field]):
            raise ValueError(f"PROFILE_{field.upper()}_INVALID")
    profile = dict(payload)
    profile["resourceLimits"] = validate_limits(payload.get("resourceLimits"))
    for field in ("topologyFile", "buildDir", "controllerBinary", "authorityBinary",
                  "requesterBinary", "providerBinary", "assemblyWorkerBinary", "oracleBinary", "outputRoot"):
        if field in profile:
            profile[field] = str(resolve_path(profile[field], field))
    if "controllerBinary" not in profile:
        profile["controllerBinary"] = str(Path(profile["buildDir"]) / "examples/App_ServiceController")
    if "authorityBinary" not in profile:
        profile["authorityBinary"] = str(Path(profile["buildDir"]) / "examples/DI_NativeArtifactAuthority")
    if "requesterBinary" not in profile:
        profile["requesterBinary"] = str(Path(profile["buildDir"]) / "examples/DI_NativeRequester")
    if "providerBinary" not in profile:
        profile["providerBinary"] = str(Path(profile["buildDir"]) / "examples/di-native-provider")
    if "assemblyWorkerBinary" not in profile:
        profile["assemblyWorkerBinary"] = str(Path(profile["buildDir"]) / "DI_NativeOnnxAssemblyWorker")
    if "oracleBinary" not in profile:
        profile["oracleBinary"] = str(Path(profile["buildDir"]) / "examples/spec190-multiturn-oracle")
    if "outputRoot" not in profile:
        profile["outputRoot"] = str(ROOT / "results/spec184-qwen06b-local")
    return profile, sha256_file(path)


def topology_nodes(path: Path) -> set[str]:
    nodes: set[str] = set()
    in_nodes = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line == "[nodes]":
            in_nodes = True
            continue
        if line.startswith("["):
            in_nodes = False
        if in_nodes and line and not line.startswith("#") and line.endswith(":"):
            nodes.add(line[:-1])
    return nodes


def binary_check(path: Path) -> dict[str, Any]:
    result: dict[str, Any] = {"path": str(path), "exists": path.is_file(),
                              "executable": os.access(path, os.X_OK)}
    if not result["exists"] or not result["executable"]:
        result["status"] = "FAIL"
        result["reason"] = "MISSING_OR_NOT_EXECUTABLE"
        return result
    result["sha256"] = sha256_file(path)
    readelf = subprocess.run(["/usr/bin/readelf", "-d", str(path)],
                             text=True, capture_output=True, check=False)
    ldd = subprocess.run(["/usr/bin/ldd", "-r", str(path)],
                          text=True, capture_output=True, check=False)
    dynamic = readelf.stdout + readelf.stderr
    ldd_output = ldd.stdout + ldd.stderr
    unresolved = [line.strip() for line in ldd_output.splitlines()
                  if "not found" in line or "undefined symbol" in line]
    shared_libraries: dict[tuple[str, str], dict[str, str]] = {}
    for line in ldd_output.splitlines():
        dependency = line.strip()
        if "=>" in dependency:
            name, target = (part.strip() for part in dependency.split("=>", 1))
            if not target.startswith("/"):
                continue
            library_path = target.split(" (", 1)[0].strip()
        elif dependency.startswith("/"):
            library_path = dependency.split(" (", 1)[0].strip()
            name = Path(library_path).name
        else:
            continue
        try:
            resolved_path = Path(library_path).resolve(strict=True)
            if not resolved_path.is_file():
                raise OSError("not a regular file")
            record = {"name": name, "path": str(resolved_path),
                      "sha256": sha256_file(resolved_path)}
            shared_libraries[(name, str(resolved_path))] = record
        except (OSError, RuntimeError) as error:
            unresolved.append(f"{name} => {library_path}: {error}")
    runpaths = [line.strip() for line in dynamic.splitlines()
                if "RPATH" in line or "RUNPATH" in line]
    result.update({"readelfExit": readelf.returncode, "lddExit": ldd.returncode,
                   "runpaths": runpaths, "unresolved": unresolved,
                   "sharedLibraries": sorted(shared_libraries.values(),
                                              key=lambda item: (item["name"], item["path"])),
                   "status": "PASS" if readelf.returncode == 0 and
                   ldd.returncode == 0 and not unresolved else "FAIL"})
    if result["status"] == "FAIL":
        result["reason"] = "ABI_OR_LIBRARY_CLOSURE"
    return result


def process_census(run_dir: Path) -> dict[str, Any]:
    """Check for descendants that still carry this run's artifact identity."""
    result: dict[str, Any] = {"runDir": str(run_dir.resolve())}
    try:
        ps = subprocess.run(["/usr/bin/ps", "-eo", "pid=,args="],
                            text=True, capture_output=True, check=False)
    except OSError as exc:
        return {**result, "status": "FAIL", "reason": "PROCESS_CENSUS_FAILED",
                "error": str(exc), "alive": []}
    if ps.returncode != 0:
        return {**result, "status": "FAIL", "reason": "PROCESS_CENSUS_FAILED",
                "returncode": ps.returncode, "alive": []}
    needle = str(run_dir.resolve())
    alive: list[dict[str, Any]] = []
    for line in ps.stdout.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) != 2:
            continue
        try:
            pid = int(fields[0])
        except ValueError:
            continue
        if pid == os.getpid() or needle not in fields[1]:
            continue
        alive.append({"pid": pid, "command": fields[1]})
    return {**result, "status": "PASS" if not alive else "FAIL",
            "reason": None if not alive else "OWNED_PROCESS_ALIVE", "alive": alive}


def _log_texts(log_root: Path) -> list[str]:
    if not log_root.is_dir():
        return []
    texts: list[str] = []
    for path in sorted(log_root.glob("*.log")):
        try:
            texts.append(path.read_text(errors="replace"))
        except OSError:
            continue
    return texts


def startup_markers_observed(log_root: Path) -> bool:
    """Return true only when the native runtime reached all startup roles."""
    texts = _log_texts(log_root)
    if not texts:
        return False
    try:
        controller_text = (log_root / "controller.log").read_text(errors="replace")
    except OSError:
        return False
    if "ServiceController started" not in controller_text:
        return False
    if not any("NATIVE_GRANT_AUTHORITY_READY" in text for text in texts):
        return False
    provider_logs = sorted(log_root.glob("provider-*.log"))
    if not provider_logs:
        return False
    for path in provider_logs:
        try:
            text = path.read_text(errors="replace")
        except OSError:
            return False
        if not any(marker in text for marker in
                   ("NDNSF_DI_NATIVE_PROVIDER_READY",
                    "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY")):
            return False
    return True


def first_failure_marker(log_root: Path) -> str | None:
    markers = (
        "DI_NATIVE_ONNX_PARSE", "DI_NATIVE_ONNX_GRAPH", "DI_NATIVE_ONNX_RECIPE",
        "DI_NATIVE_NO_ADMITTED_PROVIDER", "DI_NATIVE_SELECTION_TIMEOUT",
        "DI_NATIVE_ACK_TIMEOUT", "DI_NATIVE_RESPONSE_TIMEOUT",
        "DI_NATIVE_CONVERSATION_PARENT_MISMATCH", "NATIVE_REQUESTER_FAILED",
    )
    for text in _log_texts(log_root):
        for marker in markers:
            if marker in text:
                return marker
    return None


def validate_placement_strategy_identity(log_root: Path, expected: str,
                                         rounds: int,
                                         expected_provider_offers: int | None = None
                                         ) -> dict[str, Any] | None:
    """Require strategy identity and, for V4, fresh observation evidence."""
    if expected == "fixed":
        return None
    logs = sorted(path for path in log_root.glob("requester-*.log")
                  if path.stem.startswith("requester-") and
                  path.stem[len("requester-"):].isdigit())
    if len(logs) != rounds:
        return {"status": "FAIL", "expected": expected,
                "expectedRounds": rounds, "observedRounds": len(logs),
                "reason": "REQUESTER_ROUND_LOG_COUNT"}
    marker = "NATIVE_REQUEST_PLACEMENT strategy="
    snapshot_marker = "NDNSF_DI_NATIVE_PLACEMENT_OBSERVATION_SNAPSHOT "
    provider_config_path = log_root / "requester/config.json"
    try:
        provider_config = json.loads(provider_config_path.read_text(encoding="utf-8"))
        configured_providers = provider_config["request"]["provider_names"]
    except (OSError, ValueError, TypeError, KeyError):
        configured_providers = None
    if expected == "native-observation-aware-v4":
        if (not isinstance(configured_providers, list) or
                not configured_providers or
                any(not isinstance(provider, str) or not provider
                    for provider in configured_providers) or
                len(set(configured_providers)) != len(configured_providers)):
            return {"status": "FAIL", "expected": expected,
                    "config": str(provider_config_path),
                    "reason": "CONFIGURED_PROVIDER_ROSTER_INVALID"}
        configured_roster = sorted(configured_providers)
        if (expected_provider_offers is not None and
                len(configured_roster) != expected_provider_offers):
            return {"status": "FAIL", "expected": expected,
                    "expectedProviderOffers": expected_provider_offers,
                    "configuredProviderCount": len(configured_roster),
                    "reason": "CONFIGURED_PROVIDER_ROSTER_COUNT"}
    else:
        configured_roster = []
    expected_probe_pairs: list[list[str]] | None = None
    if expected == "native-observation-aware-v4":
        plan_paths = sorted(log_root.glob("native-*-execution-plan.json"))
        if len(plan_paths) != 1:
            return {"status": "FAIL", "expected": expected,
                    "executionPlans": [str(path) for path in plan_paths],
                    "reason": "EXECUTION_PLAN_COUNT"}
        try:
            plan = json.loads(plan_paths[0].read_text(encoding="utf-8"))
            services = plan["services"]
            if not isinstance(services, list) or len(services) != 1:
                raise ValueError("expected one service")
            service = services[0]
            roles = service["roles"]
            dependencies = service["dependencies"]
            if (not isinstance(roles, list) or not roles or
                    any(not isinstance(role, str) or not role for role in roles) or
                    len(set(roles)) != len(roles) or
                    not isinstance(dependencies, list)):
                raise ValueError("invalid role graph")
            role_set = set(roles)
            has_inter_role_dependency = False
            for dependency in dependencies:
                if not isinstance(dependency, dict):
                    raise ValueError("invalid dependency")
                producers = dependency.get("producers")
                consumers = dependency.get("consumers")
                if (not isinstance(producers, list) or not producers or
                        not isinstance(consumers, list) or not consumers or
                        any(role not in role_set for role in producers + consumers)):
                    raise ValueError("dependency role is outside the plan")
                has_inter_role_dependency = has_inter_role_dependency or any(
                    producer != consumer
                    for producer in producers for consumer in consumers)
        except (OSError, ValueError, TypeError, KeyError):
            return {"status": "FAIL", "expected": expected,
                    "executionPlan": str(plan_paths[0]),
                    "reason": "EXECUTION_PLAN_INVALID"}
        expected_probe_pairs = []
        if has_inter_role_dependency and len(configured_roster) >= 2:
            for source in configured_roster:
                for destination in configured_roster:
                    if source != destination:
                        expected_probe_pairs.append([source, destination])
                        if len(expected_probe_pairs) == 16:
                            break
                if len(expected_probe_pairs) == 16:
                    break
    observations = []
    for path in logs:
        try:
            text = path.read_text(errors="replace")
        except OSError:
            return {"status": "FAIL", "expected": expected,
                    "log": str(path), "reason": "REQUESTER_LOG_UNREADABLE"}
        identities = [line.split(marker, 1)[1].split()[0]
                      for line in text.splitlines() if marker in line]
        if not identities or any(identity != expected for identity in identities):
            return {"status": "FAIL", "expected": expected,
                    "observed": identities, "log": str(path),
                    "reason": "STRATEGY_IDENTITY_MISMATCH"}
        observation = {"log": str(path), "identities": identities}
        if expected == "native-observation-aware-v4":
            snapshot_records = []
            for line in text.splitlines():
                if snapshot_marker not in line:
                    continue
                fields = {}
                for token in line.split(snapshot_marker, 1)[1].split():
                    if "=" in token:
                        key, value = token.split("=", 1)
                        fields[key] = value
                snapshot_records.append(fields)
            if len(snapshot_records) != 1:
                return {"status": "FAIL", "expected": expected,
                        "observedSnapshotEvents": len(snapshot_records),
                        "log": str(path), "reason": "SNAPSHOT_EVENT_COUNT"}
            snapshot = snapshot_records[0]
            if snapshot.get("status") != "accepted":
                return {"status": "FAIL", "expected": expected,
                        "snapshot": snapshot, "log": str(path),
                        "reason": "SNAPSHOT_NOT_ACCEPTED"}
            if not re.fullmatch(r"sha256:[0-9a-f]{64}",
                                snapshot.get("snapshotDigest", "")):
                return {"status": "FAIL", "expected": expected,
                        "snapshot": snapshot, "log": str(path),
                        "reason": "SNAPSHOT_DIGEST_INVALID"}
            integer_fields = ("capturedAtMs", "ageMs", "expiresAtMs", "expiresInMs",
                              "providerOffers", "activeProbeRtt", "activeProbeGoodput")
            try:
                numbers = {field: int(snapshot[field]) for field in integer_fields}
            except (KeyError, ValueError):
                return {"status": "FAIL", "expected": expected,
                        "snapshot": snapshot, "log": str(path),
                        "reason": "SNAPSHOT_FIELDS_INVALID"}
            if (numbers["ageMs"] < 0 or numbers["expiresInMs"] <= 0 or
                    numbers["expiresAtMs"] <= numbers["capturedAtMs"] or
                    numbers["ageMs"] + numbers["expiresInMs"] !=
                    numbers["expiresAtMs"] - numbers["capturedAtMs"]):
                return {"status": "FAIL", "expected": expected,
                        "snapshot": snapshot, "log": str(path),
                        "reason": "SNAPSHOT_FRESHNESS_INVALID"}
            if (expected_provider_offers is not None and
                    numbers["providerOffers"] != expected_provider_offers):
                return {"status": "FAIL", "expected": expected,
                        "expectedProviderOffers": expected_provider_offers,
                        "snapshot": snapshot, "log": str(path),
                        "reason": "SNAPSHOT_PROVIDER_ROSTER_MISMATCH"}
            try:
                roster = json.loads(snapshot["providerRoster"])
                offer_providers = json.loads(snapshot["providerOfferProviders"])
                rtt_pairs = json.loads(snapshot["rttPairs"])
                goodput_edges = json.loads(snapshot["goodputEdges"])
                rtt_rows = json.loads(snapshot["activeProbeRttRows"])
                goodput_rows = json.loads(snapshot["activeProbeGoodputRows"])
            except (KeyError, ValueError, TypeError):
                return {"status": "FAIL", "expected": expected,
                        "snapshot": snapshot, "log": str(path),
                        "reason": "SNAPSHOT_SUMMARY_INVALID"}
            if (not isinstance(roster, list) or
                    any(not isinstance(provider, str) for provider in roster) or
                    sorted(roster) != configured_roster or
                    len(set(roster)) != len(roster)):
                return {"status": "FAIL", "expected": expected,
                        "configuredProviderRoster": configured_roster,
                        "snapshotProviderRoster": roster, "log": str(path),
                        "reason": "SNAPSHOT_PROVIDER_ROSTER_MISMATCH"}
            if (not isinstance(offer_providers, list) or
                    any(not isinstance(provider, str) for provider in offer_providers) or
                    sorted(offer_providers) != configured_roster or
                    len(set(offer_providers)) != len(offer_providers)):
                return {"status": "FAIL", "expected": expected,
                        "configuredProviderRoster": configured_roster,
                        "snapshotOfferProviders": offer_providers, "log": str(path),
                        "reason": "SNAPSHOT_OFFER_PROVIDER_MISMATCH"}

            def parse_pairs(value: Any) -> list[tuple[str, str]] | None:
                if not isinstance(value, list):
                    return None
                pairs = []
                for pair in value:
                    if (not isinstance(pair, dict) or
                            set(pair) != {"src_provider", "dst_provider"}):
                        return None
                    source = pair["src_provider"]
                    destination = pair["dst_provider"]
                    if (not isinstance(source, str) or
                            not isinstance(destination, str) or
                            source not in configured_roster or
                            destination not in configured_roster or
                            source == destination):
                        return None
                    pairs.append((source, destination))
                if len(set(pairs)) != len(pairs):
                    return None
                return pairs

            allowed_rtt_pairs = parse_pairs(rtt_pairs)
            allowed_goodput_edges = parse_pairs(goodput_edges)
            expected_pairs = [tuple(pair) for pair in (expected_probe_pairs or [])]
            if (allowed_rtt_pairs is None or allowed_goodput_edges is None or
                    allowed_rtt_pairs != expected_pairs or
                    allowed_goodput_edges != expected_pairs or
                    not allowed_rtt_pairs):
                return {"status": "FAIL", "expected": expected,
                        "snapshot": snapshot, "log": str(path),
                        "reason": "SNAPSHOT_PROBE_ALLOWLIST_INVALID"}

            def validate_rows(value: Any, allowed: list[tuple[str, str]],
                              expected_reporter: str) -> set[tuple[str, str]] | None:
                if not isinstance(value, list):
                    return None
                observed = set()
                for row in value:
                    if not isinstance(row, dict):
                        return None
                    reporter = row.get("reported_by")
                    source = row.get("src_provider")
                    destination = row.get("dst_provider")
                    pair = (source, destination)
                    if (not isinstance(reporter, str) or
                            reporter not in configured_roster or
                            pair not in allowed or reporter != row.get(expected_reporter)):
                        return None
                    observed.add(pair)
                if len(observed) != len(value):
                    return None
                return observed

            observed_rtt_pairs = validate_rows(rtt_rows, allowed_rtt_pairs,
                                               "src_provider")
            observed_goodput_edges = validate_rows(goodput_rows,
                                                   allowed_goodput_edges,
                                                   "dst_provider")
            if (observed_rtt_pairs is None or
                    observed_goodput_edges is None or
                    (expected_pairs and not observed_goodput_edges) or
                    numbers["activeProbeRtt"] != len(rtt_rows) or
                    numbers["activeProbeGoodput"] != len(goodput_rows)):
                return {"status": "FAIL", "expected": expected,
                        "snapshot": snapshot, "log": str(path),
                        "reason": "SNAPSHOT_ACTIVE_PROBE_ROWS_INCOMPLETE"}
            observation["placementObservationSnapshot"] = snapshot
        observations.append(observation)
    return {"status": "PASS", "expected": expected,
            "expectedRounds": rounds, "observedRounds": len(logs),
            "observations": observations}


def collect_qwen_boundaries(log_root: Path, rounds: int,
                            child_record: dict[str, Any]) -> dict[str, Any]:
    """Keep network qualification independent of the existing C++ model oracle."""
    oracle = child_record.get("cppOracle", {})
    model_status = oracle.get("status", "NOT_EVALUATED") if isinstance(oracle, dict) else "NOT_EVALUATED"
    if model_status not in {"PASS", "FAIL"}:
        model_status = "NOT_EVALUATED"
    result: dict[str, Any] = {
        "model": {"status": model_status, "oracle": oracle},
    }
    try:
        config = json.loads((log_root / "requester/config.json").read_text())
        plans = sorted(log_root.glob("native-*-execution-plan.json"))
        if len(plans) != 1:
            raise ValueError("EXECUTION_PLAN_COUNT")
        plan = json.loads(plans[0].read_text())
        roles = plan["services"][0]["roles"]
        manifest = config["catalog"]["source"]["model_manifest_digest"]
        score_mode = config["request"].get("placement_score_mode", "preparation-and-network")
        provider_texts = []
        for path in sorted(log_root.glob("provider-*.log")):
            try:
                provider_texts.append(path.read_text(errors="replace"))
            except OSError:
                # Missing provider evidence prevents placement PASS, but does
                # not discard the requester-side authenticated probe evidence.
                pass
        provider_text = "\n".join(provider_texts)
        # The model runner splits requester-N.log only after model success.
        # Recover network evidence from its raw log when that success gate fails.
        raw_path = log_root / "requester.log"
        raw_text = raw_path.read_text(errors="replace") if raw_path.is_file() else ""
        raw_requests = native_placement_request_ids(raw_text)
        reports = []
        for index in range(rounds):
            try:
                round_path = log_root / f"requester-{index}.log"
                request_id = None
                if round_path.is_file():
                    text = round_path.read_text(errors="replace")
                elif index < len(raw_requests):
                    text = raw_text
                    request_id = raw_requests[index]
                else:
                    raise OSError(f"REQUESTER_ROUND_EVIDENCE_MISSING:{index}")
                reports.append(collect_native_placement_evidence(
                    text, provider_text, roles, expected_manifest_digest=manifest,
                    expected_score_mode=score_mode, request_id=request_id))
            except OSError as error:
                reports.append({"boundaries": {
                    boundary: {"status": "FAIL", "reason": str(error)}
                    for boundary in ("probe", "placement")}})
        for boundary in ("probe", "placement"):
            result[boundary] = {
                "status": "PASS" if reports and all(
                    report["boundaries"][boundary]["status"] == "PASS"
                    for report in reports) else "FAIL",
                "rounds": [{"requestId": report.get("requestId"),
                            **report["boundaries"][boundary]} for report in reports],
            }
        result["placement"]["decisions"] = [
            report.get("placementDecision") for report in reports]
    except (OSError, ValueError, TypeError, KeyError, IndexError) as error:
        for boundary in ("probe", "placement"):
            result[boundary] = {"status": "FAIL", "reason": str(error)}
    return result


def canonical_source_info(source_path: Path | None,
                          initializer_path: Path | None,
                          cache_root: Path | None = None) -> dict[str, Any]:
    """Validate the canonical model objects before any MiniNDN process starts.

    The staged ONNX files are role artifacts.  They are not the canonical source
    object consumed by the native post-ACK assembler, so the source is an
    explicit input to the local experiment rather than synthesized from stage
    metadata.
    """
    if source_path is None:
        return {"status": "WAITING_EXTERNAL_INPUT",
                "reason": "MODEL_CANONICAL_SOURCE_REQUIRED"}
    source_path = source_path.expanduser().resolve()
    if not source_path.is_file():
        return {"status": "FAIL", "reason": "MODEL_CANONICAL_SOURCE_MISSING",
                "path": str(source_path)}
    source_bytes = source_path.stat().st_size
    result: dict[str, Any] = {"path": str(source_path), "bytes": source_bytes,
                              "sha256": sha256_file(source_path)}
    if source_bytes == 0 or source_bytes > MODEL_SOURCE_MAX_BYTES:
        result.update(status="FAIL", reason="MODEL_CANONICAL_SOURCE_SIZE_INVALID",
                      maxBytes=MODEL_SOURCE_MAX_BYTES)
        return result
    summary_path = None
    if cache_root is not None:
        cache_root = cache_root.expanduser().resolve()
        summary_path = (cache_root / "compatibility" / "canonical-source" /
                        result["sha256"][7:] / "summary.json")
        try:
            cached = json.loads(summary_path.read_text(encoding="utf-8"))
            if (isinstance(cached, dict) and
                    cached.get("schema") == "ndnsf-di-canonical-source-summary-v1" and
                    cached.get("sourceSha256") == result["sha256"] and
                    cached.get("status") == "PASS"):
                result.update({key: cached[key] for key in
                               ("status", "graphName", "nodeCount", "inputCount",
                                "outputCount")})
        except (OSError, UnicodeError, json.JSONDecodeError, KeyError, TypeError):
            pass
    if result.get("status") != "PASS":
        try:
            import onnx
            model = onnx.load(str(source_path), load_external_data=False)
        except ImportError:
            result.update(status="FAIL", reason="MODEL_ONNX_VALIDATOR_UNAVAILABLE")
            return result
        except Exception as exc:
            result.update(status="FAIL", reason="MODEL_CANONICAL_SOURCE_NOT_ONNX",
                          error=type(exc).__name__)
            return result
        result.update(status="PASS", graphName=str(model.graph.name),
                      nodeCount=len(model.graph.node), inputCount=len(model.graph.input),
                      outputCount=len(model.graph.output))
        if summary_path is not None:
            try:
                write_json(summary_path, {
                    "schema": "ndnsf-di-canonical-source-summary-v1",
                    "sourceSha256": result["sha256"],
                    "status": result["status"],
                    "graphName": result["graphName"],
                    "nodeCount": result["nodeCount"],
                    "inputCount": result["inputCount"],
                    "outputCount": result["outputCount"],
                })
            except OSError:
                # The compatibility cache is an optimization.  A cache write
                # failure must not turn a valid candidate into a false reject.
                pass
    if initializer_path is not None:
        initializer_path = initializer_path.expanduser().resolve()
        if not initializer_path.is_file():
            return {**result, "status": "FAIL",
                    "reason": "MODEL_CANONICAL_INITIALIZER_MISSING",
                    "initializerPath": str(initializer_path)}
        initializer_bytes = initializer_path.stat().st_size
        initializer = {"path": str(initializer_path), "bytes": initializer_bytes,
                       "sha256": sha256_file(initializer_path)}
        if initializer_bytes == 0 or initializer_bytes > MODEL_SOURCE_MAX_BYTES:
            return {**result, "status": "FAIL",
                    "reason": "MODEL_CANONICAL_INITIALIZER_SIZE_INVALID",
                    "initializer": initializer, "maxBytes": MODEL_SOURCE_MAX_BYTES}
        result["initializer"] = initializer
    return result


def load_model_inputs(stage_manifest: Path, stage_root: Path | None,
                      canonical_source: Path | None = None,
                      canonical_initializer: Path | None = None,
                      cache_root: Path | None = None) -> dict[str, Any]:
    spec = importlib.util.spec_from_file_location("qwen_runner", RUNNER)
    if spec is None or spec.loader is None:
        raise ValueError("RUNNER_IMPORT_FAILED")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    manifest, stages = module.load_stage_manifest(stage_manifest, stage_root)
    module.qwen_state_successor_pairs(stages)
    tokenizer_raw = Path(str(manifest.get("tokenizer", ""))).expanduser()
    candidates = [tokenizer_raw]
    if not tokenizer_raw.is_absolute():
        candidates.extend([stage_manifest.parent / tokenizer_raw,
                           stage_root / tokenizer_raw if stage_root else stage_manifest.parent / tokenizer_raw])
    tokenizer = None
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved.is_dir():
            resolved /= "tokenizer.json"
        if resolved.is_file():
            tokenizer = resolved
            break
    if tokenizer is None:
        raise ValueError("MODEL_TOKENIZER_MISSING")
    return {"manifest": manifest, "stages": stages, "tokenizer": tokenizer,
            "manifestSha256": sha256_file(stage_manifest),
            "tokenizerSha256": sha256_file(tokenizer),
            "canonicalSource": canonical_source_info(canonical_source,
                                                       canonical_initializer,
                                                       cache_root)}


def node_mapping_info(path: Path | None,
                      stages: list[dict[str, Any]],
                      source_node_count: int | None = None) -> dict[str, Any]:
    """Validate the semantic-to-source mapping required by native assembly."""
    if path is None:
        return {"status": "WAITING_EXTERNAL_INPUT",
                "reason": "MODEL_NODE_MAPPING_REQUIRED"}
    path = path.expanduser().resolve()
    if not path.is_file():
        return {"status": "FAIL", "reason": "MODEL_NODE_MAPPING_MISSING",
                "path": str(path)}
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return {"status": "FAIL", "reason": "MODEL_NODE_MAPPING_INVALID",
                "path": str(path), "error": type(exc).__name__}
    mapping = payload.get("mapping", payload) if isinstance(payload, dict) else None
    if not isinstance(mapping, dict) or not stages:
        return {"status": "FAIL", "reason": "MODEL_NODE_MAPPING_COVER_INVALID",
                "path": str(path)}
    layer_range = stages[-1].get("layerRange")
    if isinstance(layer_range, dict):
        layer_end = int(layer_range.get("endExclusive", -1))
    elif isinstance(layer_range, list) and len(layer_range) == 2:
        layer_end = int(layer_range[1])
    else:
        layer_end = -1
    expected = {"embedding", "final-norm-head"} | {
        f"layer-{index:02d}" for index in range(max(0, layer_end))}
    if set(mapping) != expected:
        return {"status": "FAIL", "reason": "MODEL_NODE_MAPPING_COVER_INVALID",
                "path": str(path), "expectedKeys": len(expected),
                "actualKeys": len(mapping)}
    if any(not isinstance(indices, list) or not indices or
           any(not isinstance(index, int) or index < 0 for index in indices)
           for indices in mapping.values()):
        return {"status": "FAIL", "reason": "MODEL_NODE_MAPPING_INDEX_INVALID",
                "path": str(path)}
    declared_nodes = payload.get("source_nodes") if isinstance(payload, dict) else None
    if (not isinstance(declared_nodes, int) or declared_nodes <= 0 or
            (source_node_count is not None and declared_nodes != source_node_count)):
        return {"status": "FAIL", "reason": "MODEL_NODE_MAPPING_SOURCE_COUNT_INVALID",
                "path": str(path), "declaredSourceNodes": declared_nodes,
                "sourceNodeCount": source_node_count}
    mapped_indices = [index for indices in mapping.values() for index in indices]
    if (len(mapped_indices) != declared_nodes or
            len(set(mapped_indices)) != declared_nodes or
            set(mapped_indices) != set(range(declared_nodes))):
        return {"status": "FAIL", "reason": "MODEL_NODE_MAPPING_INDEX_COVER_INVALID",
                "path": str(path), "declaredSourceNodes": declared_nodes}
    return {"status": "PASS", "path": str(path),
            "sha256": sha256_file(path), "keys": len(mapping),
            "mappedSourceNodes": len(mapped_indices),
            "sourceNodes": declared_nodes}


def preflight(args: argparse.Namespace, profile: dict[str, Any], profile_sha: str) -> dict[str, Any]:
    topology = Path(profile["topologyFile"])
    if not topology.is_file():
        raise ValueError("TOPOLOGY_MISSING:" + str(topology))
    nodes = topology_nodes(topology)
    requested_nodes = set(profile["stageNodes"]) | {profile["controllerNode"], profile["userNode"]}
    missing_nodes = sorted(requested_nodes - nodes)
    machine = {"status": "PASS", "topology": str(topology),
               "topologySha256": sha256_file(topology), "nodes": sorted(nodes),
               "requestedNodes": sorted(requested_nodes), "missingNodes": missing_nodes,
               "binaries": {}}
    if missing_nodes:
        machine.update(status="FAIL", reason="TOPOLOGY_NODE_MISSING")
    binary_specs = (("controller", profile["controllerBinary"]),
                    ("authority", profile["authorityBinary"]),
                    ("requester", profile["requesterBinary"]),
                    ("provider", profile["providerBinary"]),
                    ("assemblyWorker", profile["assemblyWorkerBinary"]),
                    ("oracle", profile["oracleBinary"]))
    for name, raw in binary_specs:
        machine["binaries"][name] = binary_check(Path(raw))
    if any(item["status"] != "PASS" for item in machine["binaries"].values()):
        machine.update(status="FAIL", reason="NATIVE_BINARY_PREFLIGHT")
    build_receipt_path = Path(profile["buildDir"]) / "spec180-native-build.json"
    if build_receipt_path.is_file():
        machine["buildReceipt"] = {"path": str(build_receipt_path.resolve()),
                                    "sha256": sha256_file(build_receipt_path)}
    else:
        machine["buildReceipt"] = {"path": str(build_receipt_path.resolve()),
                                    "status": "FAIL", "reason": "MISSING"}
        machine.update(status="FAIL", reason="NATIVE_BUILD_RECEIPT_PREFLIGHT")
    canonical_source_arg = getattr(args, "canonical_source", None)
    canonical_initializer_arg = getattr(args, "canonical_initializer", None)
    node_mapping_arg = getattr(args, "node_mapping", None)
    canonical_source = (canonical_source_arg.resolve()
                        if canonical_source_arg else None)
    canonical_initializer = (canonical_initializer_arg.resolve()
                             if canonical_initializer_arg else None)
    model = load_model_inputs(args.stage_manifest.resolve(),
                              args.stage_root.resolve() if args.stage_root else None,
                              canonical_source, canonical_initializer,
                              args.cache_dir)
    node_mapping = node_mapping_info(
        node_mapping_arg.resolve() if node_mapping_arg else None,
        model["stages"],
        model["canonicalSource"].get("nodeCount"))
    candidate_payload = {
        "schema": "ndnsf-di-qwen06b-app-manifest-v1",
        "application": "Qwen3-0.6B-native-MiniNDN",
        "modelIdentity": normalized_model_identity(model["manifest"]),
        "runner": {"path": str(RUNNER), "sha256": sha256_file(RUNNER)},
        "probeValidationLinks": {
            "path": str(ROOT / "Experiments/native_probe_validation_links.py"),
            "sha256": sha256_file(ROOT / "Experiments/native_probe_validation_links.py")},
        "qualificationEvidence": {
            "path": str(ROOT / "Experiments/native_placement_evidence.py"),
            "sha256": sha256_file(ROOT / "Experiments/native_placement_evidence.py")},
        "transferProfileAnalyser": {
            "path": str(ROOT / "Experiments/native_qwen_transfer_profile.py"),
            "sha256": sha256_file(ROOT / "Experiments/native_qwen_transfer_profile.py")},
        "resourceGuard": {"sha256": sha256_file(ROOT / "Experiments/native_resource_guard.py"),
                          "limits": validate_limits(profile.get("resourceLimits"))},
        "profile": {"path": str(args.profile.resolve()), "sha256": profile_sha,
                    "profileId": profile["profileId"]},
        "topology": {"path": str(topology), "sha256": machine["topologySha256"]},
        "buildDir": profile["buildDir"],
        "buildReceipt": machine["buildReceipt"],
        "binaries": {name: {"path": item["path"], "sha256": item.get("sha256"),
                            "sharedLibraries": item.get("sharedLibraries", [])}
                     for name, item in machine["binaries"].items()},
        "modelManifest": {"path": str(args.stage_manifest.resolve()),
                           "sha256": model["manifestSha256"]},
        "tokenizer": {"path": str(model["tokenizer"]), "sha256": model["tokenizerSha256"]},
        "stages": [{"role": stage["role"], "path": stage["path"],
                    "sha256": stage["sha256"], "bytes": stage["bytes"]}
                   for stage in model["stages"]],
        "canonicalSource": model["canonicalSource"],
        "nodeMapping": node_mapping,
    }
    candidate_digest = canonical_digest(candidate_payload)
    candidate = {**candidate_payload, "candidateId": "qwen06b-" + candidate_digest[7:19],
                 "candidateDigest": candidate_digest}
    model_status = model["canonicalSource"].get("status")
    status = ("PASS" if machine["status"] == "PASS" and
              model_status == "PASS" and node_mapping["status"] == "PASS"
              else "FAIL")
    return {"schema": RUN_SCHEMA, "profileId": profile["profileId"],
            "profileSha256": profile_sha, "machine": machine,
            "model": {"manifestSha256": model["manifestSha256"],
                      "tokenizerSha256": model["tokenizerSha256"],
                      "canonicalSource": model["canonicalSource"],
                      "stages": [{"role": s["role"], "sha256": s["sha256"], "bytes": s["bytes"]}
                                 for s in model["stages"]]},
            "candidate": candidate, "status": status}


def command_for(args: argparse.Namespace, profile: dict[str, Any], run_dir: Path,
                candidate: dict[str, Any] | None = None) -> list[str]:
    model_identity = (candidate or {}).get("modelIdentity", {})
    model_family = str(model_identity.get("family", "qwen"))
    model_name = str(model_identity.get("name", ""))
    command = [sys.executable, str(RUNNER),
               "--stage-manifest", str(args.stage_manifest.resolve()),
               "--stage-root", str(args.stage_root.resolve()),
               "--model-family", model_family,
               "--model-name", model_name,
               "--topology", profile["topologyFile"], "--stage-nodes", ",".join(profile["stageNodes"]),
               "--controller-node", profile["controllerNode"], "--user-node", profile["userNode"],
                "--build", profile["buildDir"], "--controller-binary", profile["controllerBinary"],
                "--placement-strategy", getattr(
                    args, "placement_strategy", "native-observation-aware-v4"),
                "--provider-role-mode", getattr(args, "provider_role_mode", "stage"),
                "--rounds", str(args.rounds), "--max-new-tokens", str(args.max_new_tokens),
               "--run-root", str(run_dir / "workload"), "--nlsr-wait-s", str(args.nlsr_wait_s),
               "--startup-timeout-s", str(args.startup_timeout_s),
               "--resource-limits-json", json.dumps(profile["resourceLimits"], sort_keys=True),
               "--direct-start"]
    ack_timeout_ms = getattr(args, "ack_timeout_ms", None)
    probe_bandwidth = getattr(args, "probe_validation_bandwidth_mbps", None)
    if probe_bandwidth is not None:
        command.extend(["--probe-validation-bandwidth-mbps", str(probe_bandwidth)])
    encrypted_repository_path = getattr(args, "encrypted_repository_path", None)
    if encrypted_repository_path is not None:
        command.extend(["--encrypted-repository-path",
                        str(encrypted_repository_path.expanduser().resolve())])
    if ack_timeout_ms is not None:
        command.extend(["--ack-timeout-ms", str(ack_timeout_ms)])
    if getattr(args, "require_multi_token", False):
        command.append("--require-multi-token")
    cache_dir = getattr(args, "cache_dir", None)
    if cache_dir is not None:
        command.extend(["--artifact-cache-root", str(
            resolve_artifact_cache_root(cache_dir, run_dir / "workload"))])
    for field, option in (
        ("authorityBinary", "--authority-binary"),
        ("requesterBinary", "--requester-binary"),
        ("providerBinary", "--provider-binary"),
        ("assemblyWorkerBinary", "--assembly-worker-binary"),
        ("oracleBinary", "--oracle-binary"),
    ):
        command.extend([option, profile[field]])
    if candidate:
        command.extend(["--stage-manifest-sha256", candidate["modelManifest"]["sha256"],
                        "--tokenizer-sha256", candidate["tokenizer"]["sha256"],
                        "--topology-sha256", candidate["topology"]["sha256"]])
        build_receipt = candidate.get("buildReceipt")
        if build_receipt and build_receipt.get("sha256"):
            command.extend(["--build-receipt-sha256", build_receipt["sha256"]])
        for name, option in (("controller", "--controller-binary-sha256"),
                             ("authority", "--authority-binary-sha256"),
                             ("requester", "--requester-binary-sha256"),
                             ("provider", "--provider-binary-sha256"),
                             ("assemblyWorker", "--assembly-worker-binary-sha256"),
                             ("oracle", "--oracle-binary-sha256")):
            digest = candidate.get("binaries", {}).get(name, {}).get("sha256")
            if digest:
                command.extend([option, digest])
    if getattr(args, "canonical_source", None):
        command.extend(["--canonical-source", str(args.canonical_source.resolve())])
        if candidate and candidate.get("canonicalSource", {}).get("sha256"):
            command.extend(["--canonical-source-sha256",
                            candidate["canonicalSource"]["sha256"]])
    if getattr(args, "canonical_initializer", None):
        command.extend(["--canonical-initializer", str(args.canonical_initializer.resolve())])
        initializer = (candidate or {}).get("canonicalSource", {}).get("initializer", {})
        if initializer.get("sha256"):
            command.extend(["--canonical-initializer-sha256", initializer["sha256"]])
    if getattr(args, "node_mapping", None):
        command.extend(["--node-mapping", str(args.node_mapping.resolve())])
        mapping = (candidate or {}).get("nodeMapping", {}).get("sha256")
        if mapping:
            command.extend(["--node-mapping-sha256", mapping])
    if args.input_token_ids:
        command.extend(["--input-token-ids", args.input_token_ids])
    if args.delta_token_ids:
        command.extend(["--delta-token-ids", args.delta_token_ids])
    if args.negative_parent:
        command.append("--negative-parent")
    return command


def validate_prepared_bundle(run_dir: Path, launch: dict[str, Any],
                             command: list[str]) -> dict[str, Any]:
    """Revalidate every immutable input recorded by ``prepare`` before launch."""
    bundle_path = run_dir / "bundle-manifest.json"
    if not bundle_path.is_file():
        raise SystemExit("BUNDLE_REQUIRED")
    try:
        bundle = json.loads(bundle_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        raise SystemExit("BUNDLE_INVALID")
    if (not isinstance(bundle, dict) or
            bundle.get("candidateDigest") != launch.get("candidateDigest") or
            bundle.get("commandDigest") != canonical_digest(command) or
            launch.get("bundleDigest") != canonical_digest(bundle) or
            bundle.get("status") != "PASS"):
        raise SystemExit("BUNDLE_INPUTS_CHANGED")
    app_manifest = run_dir / "app-manifest.json"
    app_manifest_ref = bundle.get("appManifest")
    try:
        app_manifest_digest = sha256_file(app_manifest)
    except OSError:
        app_manifest_digest = None
    if (not isinstance(app_manifest_ref, dict) or
            app_manifest_ref.get("path") != str(app_manifest) or
            app_manifest_digest is None or
            app_manifest_ref.get("sha256") != app_manifest_digest):
        raise SystemExit("BUNDLE_APP_MANIFEST_CHANGED")
    return bundle


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "prepare", "run", "local"))
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--stage-manifest", type=Path, required=True)
    parser.add_argument("--stage-root", type=Path, required=True)
    parser.add_argument("--canonical-source", type=Path, default=None,
                        help="canonical ONNX graph object used by native post-ACK assembly")
    parser.add_argument("--canonical-initializer", type=Path, default=None,
                        help="optional external canonical ONNX initializer object")
    parser.add_argument("--node-mapping", type=Path, required=True,
                        help="authenticated semantic-to-canonical ONNX node mapping JSON")
    parser.add_argument("--output-root", type=Path, default=None)
    parser.add_argument("--cache-dir", "--artifact-cache-root", dest="cache_dir",
                        type=Path, default=DEFAULT_ARTIFACT_CACHE_ROOT,
                        help="persistent cross-run model/provider cache root; outside each run")
    parser.add_argument("--run-id", default=None)
    parser.add_argument("--rounds", type=int, default=None,
                        help="round count to freeze during prepare; run defaults to the frozen command")
    parser.add_argument("--max-new-tokens", type=int, default=1)
    parser.add_argument("--placement-strategy",
                        choices=("fixed", "native-pre-split-first",
                                 "native-observation-aware-v4"),
                        default=None,
                        help="strategy to freeze during prepare; run defaults to the frozen command")
    parser.add_argument("--provider-role-mode", choices=("stage", "all"), default="stage",
                        help="advertise each Provider's stage role or every role in the manifest")
    parser.add_argument("--ack-timeout-ms", type=int, default=None,
                        help="override the native runner ACK collection window")
    parser.add_argument("--probe-validation-bandwidth-mbps", type=int, choices=(1, 4, 10),
                        help="Freeze controlled links with MTU HTB burst during prepare")
    parser.add_argument("--encrypted-repository-path", type=Path, default=None,
                        help="Freeze the native runner's external ciphertext Repo path during prepare")
    parser.add_argument("--require-multi-token", action="store_true",
                        help="require each native round to produce multiple tokens or EOS")
    parser.add_argument("--input-token-ids", default="")
    parser.add_argument("--delta-token-ids", default="")
    parser.add_argument("--negative-parent", action="store_true")
    parser.add_argument("--nlsr-wait-s", type=float, default=8.0)
    parser.add_argument("--startup-timeout-s", type=float, default=60.0)
    args = parser.parse_args()
    if args.ack_timeout_ms is not None and args.ack_timeout_ms <= 0:
        parser.error("--ack-timeout-ms must be positive")
    return args


def main() -> int:
    args = parse_args()
    if args.action != "run":
        if args.rounds is None:
            args.rounds = 2
        if args.placement_strategy is None:
            args.placement_strategy = "native-observation-aware-v4"
    profile_path = args.profile.expanduser().resolve()
    profile, profile_sha = load_profile(profile_path)
    if args.output_root is None:
        args.output_root = Path(profile["outputRoot"])
    args.output_root = args.output_root.expanduser().resolve()
    if args.run_id is None:
        args.run_id = datetime.now(timezone.utc).strftime("run-%Y%m%d-%H%M%S")
    if not RUN_ID_RE.fullmatch(args.run_id):
        raise SystemExit("RUN_ID_INVALID")
    if ((args.rounds is not None and (args.rounds < 1 or args.rounds > 8)) or args.max_new_tokens < 1
            or args.max_new_tokens > MAX_NEW_TOKENS):
        raise SystemExit("WORKLOAD_LIMIT_INVALID")
    try:
        check = preflight(args, profile, profile_sha)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        check = {"schema": RUN_SCHEMA, "profileId": profile["profileId"],
                 "profileSha256": profile_sha, "status": "FAIL", "reason": str(exc)}
    if args.action == "check":
        print(json.dumps(check, ensure_ascii=False, indent=2, sort_keys=True))
        return 0 if check["status"] == "PASS" else 1
    if check["status"] != "PASS":
        print(json.dumps(check, ensure_ascii=False, indent=2, sort_keys=True), file=sys.stderr)
        return 1
    run_dir = args.output_root / args.run_id
    if args.action in ("prepare", "local"):
        args.cache_dir = resolve_artifact_cache_root(args.cache_dir, run_dir / "workload")
        run_dir.mkdir(parents=True, exist_ok=False)
        run_dir.chmod(0o700)
        write_json(run_dir / "preflight.json", check)
        write_json(run_dir / "app-manifest.json", check["candidate"])
        launch = {"schema": "ndnsf-di-qwen06b-launch-v1", "runId": args.run_id,
                  "candidateId": check["candidate"]["candidateId"],
                  "candidateDigest": check["candidate"]["candidateDigest"],
                  "profileId": profile["profileId"], "profileSha256": profile_sha,
                  "cacheRoot": str(args.cache_dir),
                  "command": command_for(args, profile, run_dir, check["candidate"]),
                  "status": "NOT_EVALUATED"}
        bundle = {"schema": "ndnsf-di-qwen06b-bundle-v1", "runId": args.run_id,
                  "candidateDigest": launch["candidateDigest"],
                  "profileSha256": profile_sha, "command": launch["command"],
                  "commandDigest": canonical_digest(launch["command"]),
                  "appManifest": {"path": str(run_dir / "app-manifest.json"),
                                  "sha256": sha256_file(run_dir / "app-manifest.json")},
                  "status": "PASS"}
        write_json(run_dir / "bundle-manifest.json", bundle)
        launch["bundleDigest"] = canonical_digest(bundle)
        write_json(run_dir / "launch.json", launch)
        phases = {phase: {"status": "NOT_EVALUATED"} for phase in PHASES}
        phases["machine"] = {"status": check["machine"]["status"],
                              "topology": check["machine"].get("topology")}
        phases["candidate"] = {"status": "PASS",
                                "manifest": str(run_dir / "app-manifest.json")}
        phases["model"] = {"status": "PASS",
                            "canonicalSource": check["model"].get("canonicalSource")}
        phases["bundle"] = {"status": "PASS", "manifest": str(run_dir / "bundle-manifest.json"),
                             "commandDigest": bundle["commandDigest"]}
        write_json(run_dir / "run-record.json", {"schema": RUN_SCHEMA, "runId": args.run_id,
                  "candidateId": launch["candidateId"], "candidateDigest": launch["candidateDigest"],
                  "profileId": profile["profileId"], "profileSha256": profile_sha,
                  "phases": phases,
                  "status": "NOT_EVALUATED", "preparedAt": now()})
        print(json.dumps({"run": str(run_dir), "candidateId": launch["candidateId"],
                          "candidateDigest": launch["candidateDigest"], "status": "NOT_EVALUATED"},
                         ensure_ascii=False, sort_keys=True))
        if args.action == "prepare":
            return 0
    else:
        run_dir = (args.output_root / args.run_id).resolve()
        launch_path = run_dir / "launch.json"
        if not launch_path.is_file():
            raise SystemExit("PREPARE_REQUIRED")
    if os.geteuid() != 0:
        raise SystemExit("MININDN_REQUIRES_ROOT: run local/run with sudo -E")
    launch = json.loads((run_dir / "launch.json").read_text(encoding="utf-8"))
    if launch.get("candidateDigest") != check["candidate"]["candidateDigest"]:
        raise SystemExit("PREPARED_INPUTS_CHANGED")
    command = launch.get("command")
    if not isinstance(command, list) or not all(isinstance(item, str) for item in command):
        raise SystemExit("LAUNCH_RECORD_INVALID")
    record_path = run_dir / "run-record.json"
    bundle = validate_prepared_bundle(run_dir, launch, command)
    strategy_flag_count = command.count("--placement-strategy")
    if strategy_flag_count > 1:
        raise SystemExit("LAUNCH_PLACEMENT_STRATEGY_AMBIGUOUS")
    frozen_strategy = (command[command.index("--placement-strategy") + 1]
                       if strategy_flag_count == 1 else "fixed")
    if (args.placement_strategy is not None and
            args.placement_strategy != frozen_strategy):
        raise SystemExit("PREPARED_PLACEMENT_STRATEGY_MISMATCH")
    args.placement_strategy = frozen_strategy
    probe_flag = "--probe-validation-bandwidth-mbps"
    if command.count(probe_flag) > 1:
        raise SystemExit("LAUNCH_PROBE_VALIDATION_BANDWIDTH_AMBIGUOUS")
    frozen_probe_bandwidth = (int(command[command.index(probe_flag) + 1])
                              if probe_flag in command else None)
    if (args.probe_validation_bandwidth_mbps is not None and
            args.probe_validation_bandwidth_mbps != frozen_probe_bandwidth):
        raise SystemExit("PREPARED_PROBE_VALIDATION_BANDWIDTH_MISMATCH")
    args.probe_validation_bandwidth_mbps = frozen_probe_bandwidth
    rounds_flag_count = command.count("--rounds")
    if rounds_flag_count > 1:
        raise SystemExit("LAUNCH_ROUNDS_AMBIGUOUS")
    try:
        frozen_rounds = (int(command[command.index("--rounds") + 1])
                         if rounds_flag_count == 1 else 2)
    except (ValueError, IndexError) as exc:
        raise SystemExit("LAUNCH_ROUNDS_INVALID") from exc
    if args.rounds is not None and args.rounds != frozen_rounds:
        raise SystemExit("PREPARED_ROUNDS_MISMATCH")
    if not 1 <= frozen_rounds <= 8:
        raise SystemExit("LAUNCH_ROUNDS_INVALID")
    args.rounds = frozen_rounds
    bundle_path = run_dir / "bundle-manifest.json"
    record = json.loads(record_path.read_text(encoding="utf-8"))
    record["phases"]["machine"] = {"status": "PASS"}
    record["phases"]["candidate"] = {"status": "PASS", "manifest": str(run_dir / "app-manifest.json")}
    record["phases"]["model"] = {"status": "PASS"}
    record["phases"]["bundle"] = {"status": "PASS", "manifest": str(bundle_path),
                                     "commandDigest": bundle["commandDigest"]}
    record["phases"]["minindn"] = {"status": "RUNNING", "startedAt": now()}
    record["phases"]["workload"] = {"status": "RUNNING"}
    record["status"] = "RUNNING"
    write_json(record_path, record)
    log = run_dir / "logs/runner.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    try:
        with log.open("x", encoding="utf-8") as stream:
            supervision = run_guarded(
                command, cwd=ROOT, stdout=stream,
                sample_path=run_dir / "resource-samples.jsonl",
                limits=profile["resourceLimits"])
    except (OSError, ValueError) as exc:
        supervision = {"returncode": None,
                       "boundary": ("EVIDENCE_CONFLICT" if isinstance(exc, FileExistsError)
                                    else "RESOURCE_SUPERVISOR_ERROR:" + type(exc).__name__),
                       "cleanup": "NOT_STARTED", "remainingProcesses": []}
    record["resourceGuard"] = supervision
    child_record = run_dir / "workload/run-record.json"
    child_status = None
    child_evidence: dict[str, Any] = {}
    if child_record.is_file():
        try:
            child_evidence = json.loads(child_record.read_text(encoding="utf-8"))
            if not isinstance(child_evidence, dict):
                child_evidence = {}
                raise ValueError("CHILD_RECORD_NOT_OBJECT")
            child_status = child_evidence.get("status")
        except (OSError, ValueError, TypeError):
            child_status = None
    failure_marker = first_failure_marker(run_dir / "workload")
    startup_ready = startup_markers_observed(run_dir / "workload")
    placement_identity = validate_placement_strategy_identity(
        run_dir / "workload", args.placement_strategy, args.rounds,
        expected_provider_offers=len(profile["stageNodes"]))
    if placement_identity is not None:
        record["placementStrategyIdentity"] = placement_identity
    boundaries_ok = True
    if args.placement_strategy == "native-observation-aware-v4":
        record["boundaries"] = collect_qwen_boundaries(
            run_dir / "workload", args.rounds, child_evidence)
        boundaries_ok = all(boundary["status"] == "PASS"
                            for boundary in record["boundaries"].values())
        print("SPEC196_NATIVE_BOUNDARIES " + json.dumps(record["boundaries"], sort_keys=True))
    if supervision["boundary"]:
        boundary = supervision["boundary"]
        record["phases"]["minindn"] = {
            "status": "PASS" if startup_ready else "NOT_EVALUATED",
            "finishedAt": now()}
        record["phases"]["workload"] = {"status": "NOT_EVALUATED", "reason": boundary}
        record["status"] = ("RESOURCE_BOUNDARY" if boundary.startswith("RESOURCE_BOUNDARY:")
                            else "FAIL")
    elif (supervision["returncode"] == 0 and child_status == "PASS" and
          (placement_identity is None or placement_identity["status"] == "PASS") and
          boundaries_ok):
        record["phases"]["minindn"] = {"status": "PASS", "finishedAt": now()}
        record["phases"]["workload"] = {"status": "PASS", "childRecord": str(child_record),
                                          "elapsedSeconds": round(time.monotonic() - started, 3)}
        record["status"] = "PASS"
    else:
        record["phases"]["minindn"] = {"status": "PASS" if startup_ready else "FAIL", "finishedAt": now(),
                                        "returncode": supervision["returncode"], "log": str(log)}
        evidence_mismatch = (supervision["returncode"] == 0 and child_status == "PASS" and
                             ((placement_identity is not None and
                               placement_identity["status"] != "PASS") or not boundaries_ok))
        placement_reason = ((placement_identity or {}).get("reason", "EVIDENCE_GATE_FAILED")
                            if boundaries_ok else "SEPARATE_BOUNDARY_FAILED")
        record["phases"]["workload"] = {"status": "FAIL" if startup_ready else "NOT_EVALUATED",
                                          "reason": ("PLACEMENT_QUALIFICATION_EVIDENCE:" +
                                                     placement_reason
                                                     if evidence_mismatch else
                                                     "NATIVE_REQUEST_FAILED:" + failure_marker
                                                     if startup_ready and failure_marker else
                                                     "MININDN_START_OR_REQUEST_FAILED")}
        record["status"] = "FAIL"
    cleanup = process_census(run_dir)
    if supervision["cleanup"] != "PASS":
        cleanup = {"status": supervision["cleanup"],
                   "reason": ("RESOURCE_GUARD_CHILD_RESIDUE" if supervision["remainingProcesses"]
                              else supervision["boundary"] or "RESOURCE_GUARD_CLEANUP_UNOBSERVED"),
                   "remainingProcesses": supervision["remainingProcesses"],
                   "census": cleanup}
    record["phases"]["cleanup"] = cleanup
    if cleanup["status"] != "PASS":
        record["status"] = "FAIL"
    launch["status"] = record["status"]
    launch["finishedAt"] = now()
    write_json(run_dir / "launch.json", launch)
    record["finishedAt"] = now()
    record["command"] = command
    record["log"] = str(log)
    write_json(record_path, record)
    print(json.dumps({"run": str(record_path), "runId": args.run_id,
                      "candidateDigest": record["candidateDigest"], "status": record["status"]},
                     ensure_ascii=False, sort_keys=True))
    return 0 if record["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
