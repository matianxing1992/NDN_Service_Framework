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
RUNNER = ROOT / "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"
PROFILE_SCHEMA = "ndnsf-di-qwen06b-local-experiment-v1"
RUN_SCHEMA = "ndnsf-di-qwen06b-local-run-v2"
MODEL_SOURCE_MAX_BYTES = 1 << 20
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


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2,
                               sort_keys=True) + "\n", encoding="utf-8")


def resolve_path(value: str, field: str) -> Path:
    if not isinstance(value, str) or not value.strip() or "\x00" in value:
        raise ValueError(f"{field} must be a non-empty path")
    path = Path(value).expanduser()
    return path.resolve() if path.is_absolute() else (ROOT / path).resolve()


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
        "requesterBinary", "providerBinary", "assemblyWorkerBinary", "outputRoot",
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
    for field in ("topologyFile", "buildDir", "controllerBinary", "authorityBinary",
                  "requesterBinary", "providerBinary", "assemblyWorkerBinary", "outputRoot"):
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
    unresolved = [line.strip() for line in (ldd.stdout + ldd.stderr).splitlines()
                  if "not found" in line or "undefined symbol" in line]
    runpaths = [line.strip() for line in dynamic.splitlines()
                if "RPATH" in line or "RUNPATH" in line]
    result.update({"readelfExit": readelf.returncode, "lddExit": ldd.returncode,
                   "runpaths": runpaths, "unresolved": unresolved,
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


def canonical_source_info(source_path: Path | None,
                          initializer_path: Path | None) -> dict[str, Any]:
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
                      canonical_initializer: Path | None = None) -> dict[str, Any]:
    spec = importlib.util.spec_from_file_location("qwen_runner", RUNNER)
    if spec is None or spec.loader is None:
        raise ValueError("RUNNER_IMPORT_FAILED")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    manifest, stages = module.load_stage_manifest(stage_manifest, stage_root)
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
                                                       canonical_initializer)}


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
    for name, raw in (("controller", profile["controllerBinary"]),
                      ("authority", profile["authorityBinary"]),
                      ("requester", profile["requesterBinary"]),
                      ("provider", profile["providerBinary"]),
                      ("assemblyWorker", profile["assemblyWorkerBinary"])):
        machine["binaries"][name] = binary_check(Path(raw))
    if any(item["status"] != "PASS" for item in machine["binaries"].values()):
        machine.update(status="FAIL", reason="NATIVE_BINARY_PREFLIGHT")
    canonical_source_arg = getattr(args, "canonical_source", None)
    canonical_initializer_arg = getattr(args, "canonical_initializer", None)
    canonical_source = (canonical_source_arg.resolve()
                        if canonical_source_arg else None)
    canonical_initializer = (canonical_initializer_arg.resolve()
                             if canonical_initializer_arg else None)
    model = load_model_inputs(args.stage_manifest.resolve(),
                              args.stage_root.resolve() if args.stage_root else None,
                              canonical_source, canonical_initializer)
    candidate_payload = {
        "schema": "ndnsf-di-qwen06b-app-manifest-v1",
        "application": "Qwen3-0.6B-native-MiniNDN",
        "runner": {"path": str(RUNNER), "sha256": sha256_file(RUNNER)},
        "profile": {"path": str(args.profile.resolve()), "sha256": profile_sha,
                    "profileId": profile["profileId"]},
        "topology": {"path": str(topology), "sha256": machine["topologySha256"]},
        "buildDir": profile["buildDir"],
        "buildReceipt": ({"path": str(Path(profile["buildDir"]) / "spec180-native-build.json"),
                          "sha256": sha256_file(Path(profile["buildDir"]) / "spec180-native-build.json")}
                         if (Path(profile["buildDir"]) / "spec180-native-build.json").is_file() else None),
        "binaries": {name: {"path": item["path"], "sha256": item.get("sha256")}
                     for name, item in machine["binaries"].items()},
        "modelManifest": {"path": str(args.stage_manifest.resolve()),
                           "sha256": model["manifestSha256"]},
        "tokenizer": {"path": str(model["tokenizer"]), "sha256": model["tokenizerSha256"]},
        "stages": [{"role": stage["role"], "path": stage["path"],
                    "sha256": stage["sha256"], "bytes": stage["bytes"]}
                   for stage in model["stages"]],
        "canonicalSource": model["canonicalSource"],
    }
    candidate_digest = canonical_digest(candidate_payload)
    candidate = {**candidate_payload, "candidateId": "qwen06b-" + candidate_digest[7:19],
                 "candidateDigest": candidate_digest}
    model_status = model["canonicalSource"].get("status")
    status = "PASS" if machine["status"] == "PASS" and model_status == "PASS" else "FAIL"
    return {"schema": RUN_SCHEMA, "profileId": profile["profileId"],
            "profileSha256": profile_sha, "machine": machine,
            "model": {"manifestSha256": model["manifestSha256"],
                      "tokenizerSha256": model["tokenizerSha256"],
                      "canonicalSource": model["canonicalSource"],
                      "stages": [{"role": s["role"], "sha256": s["sha256"], "bytes": s["bytes"]}
                                 for s in model["stages"]]},
            "candidate": candidate, "status": status}


def command_for(args: argparse.Namespace, profile: dict[str, Any], run_dir: Path) -> list[str]:
    command = [sys.executable, str(RUNNER),
               "--stage-manifest", str(args.stage_manifest.resolve()),
               "--stage-root", str(args.stage_root.resolve()),
               "--topology", profile["topologyFile"], "--stage-nodes", ",".join(profile["stageNodes"]),
               "--controller-node", profile["controllerNode"], "--user-node", profile["userNode"],
               "--build", profile["buildDir"], "--controller-binary", profile["controllerBinary"],
               "--rounds", str(args.rounds), "--max-new-tokens", str(args.max_new_tokens),
               "--run-root", str(run_dir / "workload"), "--nlsr-wait-s", str(args.nlsr_wait_s),
               "--startup-timeout-s", str(args.startup_timeout_s)]
    if getattr(args, "canonical_source", None):
        command.extend(["--canonical-source", str(args.canonical_source.resolve())])
    if getattr(args, "canonical_initializer", None):
        command.extend(["--canonical-initializer", str(args.canonical_initializer.resolve())])
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
    parser.add_argument("--output-root", type=Path, default=None)
    parser.add_argument("--run-id", default=None)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--max-new-tokens", type=int, default=1)
    parser.add_argument("--input-token-ids", default="")
    parser.add_argument("--delta-token-ids", default="0")
    parser.add_argument("--negative-parent", action="store_true")
    parser.add_argument("--nlsr-wait-s", type=float, default=8.0)
    parser.add_argument("--startup-timeout-s", type=float, default=60.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    profile_path = args.profile.expanduser().resolve()
    profile, profile_sha = load_profile(profile_path)
    if args.output_root is None:
        args.output_root = Path(profile["outputRoot"])
    args.output_root = args.output_root.expanduser().resolve()
    if args.run_id is None:
        args.run_id = datetime.now(timezone.utc).strftime("run-%Y%m%d-%H%M%S")
    if not RUN_ID_RE.fullmatch(args.run_id):
        raise SystemExit("RUN_ID_INVALID")
    if args.rounds < 1 or args.rounds > 8 or args.max_new_tokens < 1 or args.max_new_tokens > 64:
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
        run_dir.mkdir(parents=True, exist_ok=False)
        run_dir.chmod(0o700)
        write_json(run_dir / "preflight.json", check)
        write_json(run_dir / "app-manifest.json", check["candidate"])
        launch = {"schema": "ndnsf-di-qwen06b-launch-v1", "runId": args.run_id,
                  "candidateId": check["candidate"]["candidateId"],
                  "candidateDigest": check["candidate"]["candidateDigest"],
                  "profileId": profile["profileId"], "profileSha256": profile_sha,
                  "command": command_for(args, profile, run_dir), "status": "NOT_EVALUATED"}
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
    with log.open("w", encoding="utf-8") as stream:
        completed = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT,
                                   check=False)
    child_record = run_dir / "workload/run-record.json"
    child_status = None
    if child_record.is_file():
        try:
            child_status = json.loads(child_record.read_text(encoding="utf-8")).get("status")
        except (OSError, ValueError, TypeError):
            child_status = None
    failure_marker = first_failure_marker(run_dir / "workload")
    startup_ready = startup_markers_observed(run_dir / "workload")
    if completed.returncode == 0 and child_status == "PASS":
        record["phases"]["minindn"] = {"status": "PASS", "finishedAt": now()}
        record["phases"]["workload"] = {"status": "PASS", "childRecord": str(child_record),
                                          "elapsedSeconds": round(time.monotonic() - started, 3)}
        record["status"] = "PASS"
    else:
        record["phases"]["minindn"] = {"status": "PASS" if startup_ready else "FAIL", "finishedAt": now(),
                                        "returncode": completed.returncode, "log": str(log)}
        record["phases"]["workload"] = {"status": "FAIL" if startup_ready else "NOT_EVALUATED",
                                          "reason": ("NATIVE_REQUEST_FAILED:" + failure_marker
                                                     if startup_ready and failure_marker
                                                     else "MININDN_START_OR_REQUEST_FAILED")}
        record["status"] = "FAIL"
    cleanup = process_census(run_dir)
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
