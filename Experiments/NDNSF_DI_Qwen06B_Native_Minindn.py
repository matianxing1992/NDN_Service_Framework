#!/usr/bin/env python3
"""Run a real Qwen3-0.6B CPU native request through a MiniNDN topology.

The Python process owns only topology, identities, files, and child-process
lifecycle.  Request planning, grants, Provider admission, ONNX execution,
streaming, and conversation checkpoints stay in the C++ executables.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import signal
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from cryptography.hazmat.primitives.serialization import (
    Encoding,
    PublicFormat,
    load_pem_public_key,
)
from cryptography.hazmat.backends import default_backend

ROOT = Path(__file__).resolve().parents[1]
ROLE_PREFIX = "/LLM/Pipeline/Stage/"
SERVICE = "/AI/LLM/Pipeline/QwenNative"
GROUP = "/example/ndnsf-qwen06b/group"
APP_ROOT = "/example/ndnsf-qwen06b"
CONTROLLER = APP_ROOT + "/controller"
AUTHORITY = APP_ROOT + "/authority"
USER = APP_ROOT + "/user"


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def digest_text(value: str) -> str:
    return digest_bytes(value.encode("utf-8"))


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def qwen_graph_digest(model_name: str, revision: str,
                      precision: str, ranges: list[list[int]]) -> str:
    layers = int(ranges[-1][1])
    nodes = ["embedding"] + [
        f"layer-{index:02d}" for index in range(layers)
    ] + ["final-norm-head"]
    edges = []
    for index in range(1, len(nodes)):
        if index == 1:
            edge = "hidden-embedding-to-layer-00"
        elif index == len(nodes) - 1:
            edge = f"hidden-layer-{layers - 1}-to-final"
        else:
            edge = f"hidden-layer-{index - 2}-to-{index - 1}"
        edges.append(edge)
    return digest_bytes(canonical_bytes({
        "model": model_name,
        "revision": revision,
        "precision": precision,
        "decode_mode": "single-token-autoregressive",
        "modality": "text-only",
        "mtp_enabled": False,
        "thinking_mode": "disabled",
        "layer_ranges": ranges,
        "nodes": nodes,
        "edges": edges,
        "legal_cuts": edges,
    }))


def run_host(command: list[str], *, check: bool = True) -> None:
    subprocess.run(command, cwd=ROOT, check=check,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def make_external_key(directory: Path, stem: str) -> tuple[Path, Path]:
    directory.mkdir(parents=True, exist_ok=True)
    private = directory / f"{stem}-private.pem"
    public = directory / f"{stem}-public.pem"
    run_host(["/usr/bin/openssl", "genpkey", "-algorithm", "ED25519",
              "-out", str(private)])
    run_host(["/usr/bin/openssl", "pkey", "-in", str(private), "-pubout",
              "-out", str(public)])
    private.chmod(0o600)
    public.chmod(0o644)
    return private, public


def public_key_digest(path: Path) -> str:
    key = load_pem_public_key(path.read_bytes(), backend=default_backend())
    raw = key.public_bytes(Encoding.Raw, PublicFormat.Raw)
    return digest_bytes(raw)


def parse_csv_ints(value: str) -> list[int]:
    try:
        result = [int(item.strip()) for item in value.split(",") if item.strip()]
    except ValueError as exc:
        raise ValueError(f"invalid integer list: {value}") from exc
    if not result:
        raise ValueError("integer list must not be empty")
    if any(item < 0 for item in result):
        raise ValueError("token IDs must be non-negative")
    return result


def tensor_bundle(token_ids: list[int]) -> bytes:
    payload = b"".join(struct.pack("<q", value) for value in token_ids)
    return (b"NDITB001" + struct.pack("<I", 1) + struct.pack("<I", 9)
            + b"input_ids" + struct.pack("<I", 3) + struct.pack("<I", 2)
            + struct.pack("<q", 1) + struct.pack("<q", 1)
            + struct.pack("<q", len(token_ids)) + struct.pack("<Q", len(payload))
            + payload)


def load_stage_manifest(path: Path, stage_root: Path | None):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    stages = manifest.get("stages")
    if not isinstance(stages, list) or len(stages) < 2:
        raise ValueError("Qwen stage manifest requires at least two stages")
    resolved = []
    for stage in stages:
        role = str(stage.get("role", ""))
        expected = f"{ROLE_PREFIX}{len(resolved)}"
        if role != expected:
            raise ValueError(f"stage role must be {expected}, got {role}")
        raw = Path(str(stage.get("path", ""))).expanduser()
        candidates = [raw]
        if not raw.is_absolute():
            candidates.insert(0, path.parent / raw)
            if stage_root:
                candidates.insert(0, stage_root / raw.name)
        artifact = next((item.resolve() for item in candidates if item.is_file()), None)
        if artifact is None:
            raise ValueError(f"stage artifact is missing: {raw}")
        actual = digest_bytes(artifact.read_bytes())
        expected_digest = str(stage.get("sha256", ""))
        if expected_digest.startswith("sha256:"):
            expected_digest = expected_digest[7:]
        if actual[7:] != expected_digest:
            raise ValueError(f"stage digest mismatch: {artifact}")
        resolved.append({**stage, "path": str(artifact), "sha256": actual,
                         "bytes": artifact.stat().st_size})
    return manifest, resolved


def stage_plan_and_manifest(output: Path, model_manifest: dict, stages: list[dict],
                            max_tokens: int, tokenizer_digest: str):
    roles = [str(stage["role"]) for stage in stages]
    dependencies = []
    for index in range(len(roles) - 1):
        dependencies.append({
            "producers": [roles[index]], "consumers": [roles[index + 1]],
            "keyScope": f"pipeline-stage-{index}-to-{index + 1}",
            "topicPrefix": "/NDNSF/DI/QWEN",
            "objectNameTemplate": (
                "{producerProvider}/NDNSF/DI/QWEN/{sessionId}/"
                "{producerRole}/{consumerRole}/{sequence}"),
            "expectedSegments": 0, "expectedBytes": 0, "required": True,
            "segmentNaming": {"mode": "ndn-segment-component",
                               "staticSegmentCount": 0, "dynamicFallback": True},
            "tensors": ["hidden_states", "attention_mask", "position_ids"],
        })
    artifacts = []
    for index, stage in enumerate(stages):
        cache_inputs = list(stage.get("cacheInputs", []))
        cache_outputs = list(stage.get("cacheOutputs", []))
        metadata = {
            "inputNames": ",".join(stage["inputNames"]),
            "outputNames": ",".join(stage["outputNames"]),
            "forceOutputBundle": "true",
            "executionProvider": "cpu",
            "allowCpuFallback": "false",
            "deviceId": "cpu0",
            "dtype": str(model_manifest.get("dtype", "float32")),
            "passthroughTensors": "attention_mask,position_ids",
            "kvTensorMap": ",".join(f"{a}={b}" for a, b in zip(cache_inputs, cache_outputs)),
            "kvOutputTensors": ",".join(cache_outputs),
            "kvOutputScope": "kv-state",
            "outputBundleScope": "final-response" if index == len(stages) - 1 else f"pipeline-stage-{index}-to-{index + 1}",
            "streamingGeneration": "true",
            "statefulModel": "false",
            "maxGeneratedTokens": str(max_tokens),
            "eosTokenIds": "151645",
            "tokenizerDigest": tokenizer_digest,
        }
        if index < len(stages) - 1:
            metadata["outputAlias.hidden_states_out"] = "hidden_states"
        if index > 0:
            for name in ("hidden_states", "attention_mask", "position_ids"):
                metadata[f"inputScope.{name}"] = f"pipeline-stage-{index - 1}-to-{index}"
        artifacts.append({
            "role": stage["role"], "path": stage["path"],
            "artifact": f"/Artifact/Qwen3-0.6B/Stage/{index}",
            "filename": Path(stage["path"]).name, "kind": "model",
            "backend": "onnxruntime", "metadata": metadata,
        })
    model_name = str(model_manifest["model"])
    revision = str(model_manifest.get("modelRevision", ""))
    model_uri = "/Model/Qwen3/0.6B"
    plan = {"version": 2, "services": [{
        "schemaVersion": 2, "service": SERVICE, "model": model_uri,
        "modelRepository": model_name, "modelRevision": revision,
        "dtype": model_manifest.get("dtype", "float32"), "modelFamily": "qwen",
        "modelFormat": "onnx", "plannerKind": "native-qwen-layer",
        "runtimeBackend": "onnxruntime", "executionPolicy": "DATA_DRIVEN_V2",
        "roles": roles, "dependencies": dependencies,
    }]}
    service_manifest = {"services": [{
        "name": SERVICE, "model": model_uri, "modelRepository": model_name,
        "modelRevision": revision, "dtype": model_manifest.get("dtype", "float32"),
        "roles": roles, "dependencies": dependencies, "artifacts": artifacts,
        "modelFamily": "qwen", "modelFormat": "onnx",
        "plannerKind": "native-qwen-layer", "runtimeBackend": "onnxruntime",
    }]}
    plan_path = output / "native-qwen-execution-plan.json"
    manifest_path = output / "native-qwen-service-manifest.json"
    plan_path.write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n")
    manifest_path.write_text(json.dumps(service_manifest, indent=2, sort_keys=True) + "\n")
    return plan_path, manifest_path


def policy_text(provider_names: list[str]) -> str:
    entries = "".join(
        " provider-policy\n {\n  for %s\n  allow { %s\n  %s/ROLE/LLM/Pipeline/Stage/0 }\n }\n"
        % (name, SERVICE, APP_ROOT) for name in provider_names)
    return (f"name {CONTROLLER}/NDNSF/ControllerPolicy/v1\n\n"
            "provider-policies\n{\n provider-policy\n {\n  for " + AUTHORITY
            + "\n  allow { /HELLO }\n }\n" + entries + "}\n\n"
            "user-policies\n{\n user-policy\n {\n  for " + USER
            + "\n  allow { " + SERVICE + "\n  /HELLO }\n }\n}\n")


def env_for(home: Path, node: str) -> dict[str, str]:
    return {"HOME": str(home), "NDN_CLIENT_CONF": str(home / ".ndn/client.conf"),
            "NDN_CLIENT_PIB": "pib-sqlite3:" + str(home / "pib"),
            "NDN_CLIENT_TPM": "tpm-file:" + str(home / "tpm"),
            "NDN_CLIENT_TRANSPORT": f"unix:///run/nfd/{node}.sock",
            "PATH": "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"}


def env_command(env: dict[str, str], command: str) -> str:
    return "env " + " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items()) + " " + command


def wait_for_marker(process, log: Path, markers: tuple[str, ...], timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log.read_text(errors="replace") if log.exists() else ""
        for marker in markers:
            if marker in text:
                return marker
        if process.poll() is not None:
            raise RuntimeError(f"process exited {process.returncode}: {log}")
        time.sleep(0.2)
    raise RuntimeError(f"timeout waiting for {markers}: {log}")


def validate_native_output(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"native output is not JSON: {path}") from exc
    if value.get("schema") != "NDNSF-DI-FINAL-V1":
        raise RuntimeError(f"native output schema is unexpected: {path}")
    token_ids = value.get("tokenIds")
    if not isinstance(token_ids, list) or not token_ids:
        raise RuntimeError(f"native output has no generated token IDs: {path}")
    if any(not isinstance(item, int) for item in token_ids):
        raise RuntimeError(f"native output token IDs are not integers: {path}")
    return {"tokenCount": len(token_ids), "tokenIds": token_ids}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage-manifest", type=Path, required=True)
    parser.add_argument("--stage-root", type=Path, default=None)
    parser.add_argument("--topology", type=Path, default=ROOT / "Experiments/Topology/AI_Lab.conf")
    parser.add_argument("--stage-nodes", default="ucla,arizona,wustl")
    parser.add_argument("--controller-node", default="memphis")
    parser.add_argument("--user-node", default="neu")
    parser.add_argument("--build", type=Path, default=ROOT / "build-spec184-b5-candidate-r4")
    parser.add_argument("--controller-binary", type=Path, default=ROOT / "build-spec184-b5-candidate/examples/App_ServiceController")
    parser.add_argument("--run-root", type=Path, default=None)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--max-new-tokens", type=int, default=2)
    parser.add_argument("--input-token-ids", default="")
    parser.add_argument("--delta-token-ids", default="0")
    parser.add_argument("--negative-parent", action="store_true")
    parser.add_argument("--nlsr-wait-s", type=float, default=8.0)
    parser.add_argument("--startup-timeout-s", type=float, default=60.0)
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise SystemExit("MININDN_REQUIRES_ROOT: run this script with sudo -E")
    if args.rounds < 1 or args.rounds > 8:
        raise SystemExit("--rounds must be between 1 and 8")
    if args.negative_parent and args.rounds < 2:
        raise SystemExit("--negative-parent requires --rounds >= 2")
    if args.max_new_tokens < 1 or args.max_new_tokens > 64:
        raise SystemExit("--max-new-tokens must be between 1 and 64")
    stage_manifest_path = args.stage_manifest.expanduser().resolve()
    model_manifest, stages = load_stage_manifest(stage_manifest_path, args.stage_root)
    stage_nodes = [item.strip() for item in args.stage_nodes.split(",") if item.strip()]
    if len(stage_nodes) != len(stages) or len(set(stage_nodes)) != len(stage_nodes):
        raise SystemExit("--stage-nodes count must match stage manifest and contain no duplicates")
    if not args.topology.expanduser().is_file():
        raise SystemExit(f"topology is missing: {args.topology}")
    tokenizer_raw = Path(str(model_manifest.get("tokenizer", ""))).expanduser()
    tokenizer_candidates = [tokenizer_raw]
    if not tokenizer_raw.is_absolute():
        tokenizer_candidates.extend([
            stage_manifest_path.parent / tokenizer_raw,
            (args.stage_root.expanduser() / tokenizer_raw)
            if args.stage_root else stage_manifest_path.parent / tokenizer_raw,
        ])
    tokenizer = None
    for candidate in tokenizer_candidates:
        resolved = candidate.resolve()
        if resolved.is_dir():
            resolved = resolved / "tokenizer.json"
        if resolved.is_file():
            tokenizer = resolved
            break
    if tokenizer is None:
        raise SystemExit(f"tokenizer.json is missing: {tokenizer_raw}")
    tokenizer_digest = digest_bytes(tokenizer.read_bytes())
    revision = str(model_manifest.get("modelRevision", ""))
    model_name = "Qwen3-0.6B"
    ranges = []
    for stage in stages:
        layer_range = stage.get("layerRange")
        if isinstance(layer_range, dict):
            ranges.append([int(layer_range["start"]), int(layer_range["endExclusive"])])
        elif isinstance(layer_range, list) and len(layer_range) == 2:
            ranges.append([int(layer_range[0]), int(layer_range[1])])
        else:
            raise ValueError(f"stage layerRange is invalid: {stage.get('role')}")
    graph_digest = qwen_graph_digest(model_name, revision, "float32", ranges)
    adapter = {
        "name": "qwen", "version": "1", "state_digest": digest_text("qwen-native-state"),
        "abi": "qwen-native-onnxruntime-cpu-v1", "model_formats": ["onnx"],
        "tasks": ["text-generation"], "backends": ["onnxruntime"], "precisions": ["float32"],
        "input_schema_digest": digest_text("qwen-input-schema"),
        "options_schema_digest": digest_text("qwen-options-schema"),
        "result_schema_digest": digest_text("qwen-result-schema"),
        "graph_schema_digest": digest_text("qwen-graph-schema"),
        "split_schema_digest": digest_text("qwen-split-schema"),
        "state_schema_digest": digest_text("qwen-state-schema"),
        "graph_inspectable": True, "splittable": True, "deterministic_analysis": True,
    }
    model_descriptor = {
        "model_name": model_name, "content_digest": digest_text(f"{model_name}:{revision}:weights"),
        "semantics_digest": digest_text(f"{model_name}:{revision}:semantics"),
        "graph_digest": graph_digest, "model_format": "onnx", "precision": "float32",
        "adapter": adapter, "source_revision": revision,
    }
    adapter_digest = digest_bytes(canonical_bytes(adapter))
    manifest_digest = digest_bytes(stage_manifest_path.read_bytes())
    source_payload = {"model": model_name, "revision": revision, "layerRanges": ranges,
                      "stageDigests": [stage["sha256"] for stage in stages],
                      "tokenizerDigest": tokenizer_digest}
    if args.run_root is None:
        run_root = Path(tempfile.mkdtemp(prefix="ndnsf-qwen06b-minindn-", dir="/tmp"))
    else:
        run_root = args.run_root.expanduser().resolve()
        run_root.mkdir(parents=True, exist_ok=True)
    run_root.chmod(0o700)
    (run_root / "requester").mkdir()
    source_path = run_root / "requester/model-source.json"
    source_path.write_bytes(canonical_bytes(source_payload))
    source_digest = digest_bytes(source_path.read_bytes())
    stage_plan, service_manifest = stage_plan_and_manifest(
        run_root, model_manifest, stages, args.max_new_tokens, tokenizer_digest)
    provider_names = [f"{APP_ROOT}/provider-{index}" for index in range(len(stages))]
    role_map_digest = digest_bytes(canonical_bytes(list(zip(
        [stage["role"] for stage in stages], provider_names))))
    catalog = {
        "schema": "ndnsf-di-native-request-catalog-v1", "model": model_descriptor,
        "source": {"data_name": "/catalog/qwen06b/source", "digest": source_digest,
                    "model_manifest_digest": manifest_digest,
                    "canonical_graph_digest": graph_digest},
        "recipe": {"artifact_profile_digest": digest_text("qwen06b-profile"),
                   "assembler_descriptor_digest": digest_text("qwen06b-assembler"),
                   "backend_abi": "onnxruntime-cpu-v1", "precision": "float32",
                   "quantization": "none", "layout": "native", "padding": "none",
                   "protection_epoch": "epoch-1", "max_source_bytes": 1 << 20,
                   "max_assembled_bytes": 8 << 30, "max_nodes": 64},
        "publication": {"artifact_root": "/Model/Qwen3-0.6B/artifacts",
                         "package_manifest_digest": manifest_digest},
        "input_format": "OPAQUE", "max_payload_bytes": 4 << 20,
        "splitter": {"kind": "QWEN", "layer_ranges": ranges,
                     "artifact_digests_by_role": {stage["role"]: stage["sha256"] for stage in stages},
                     "weight_bytes_by_role": {stage["role"]: stage["bytes"] for stage in stages},
                     "roles": [stage["role"] for stage in stages],
                     "tensor_degrees": [1] * len(stages),
                     "input_ingress_role": stages[0]["role"],
                     "result_egress_role": stages[-1]["role"]},
        "node_mapping": {"embedding": [0],
                         **{f"layer-{index:02d}": [index + 1] for index in range(ranges[-1][1])},
                         "final-norm-head": [ranges[-1][1] + 1]},
        "state_inputs": {stage["role"]: {"attention_kv_in": ["attention_kv_in"],
                                          "recurrent_state_in": ["recurrent_state_in"],
                                          "convolution_state_in": ["convolution_state_in"]} for stage in stages},
        "state_outputs": {stage["role"]: {"attention_kv_out": ["attention_kv_out"],
                                           "recurrent_state_out": ["recurrent_state_out"],
                                           "convolution_state_out": ["convolution_state_out"]} for stage in stages},
    }
    requester_dir = run_root / "requester"
    catalog_path = requester_dir / "catalog.json"
    catalog_path.write_text(json.dumps(catalog, indent=2, sort_keys=True) + "\n")
    input_ids = parse_csv_ints(args.input_token_ids) if args.input_token_ids else [1]
    (requester_dir / "input.bin").write_bytes(tensor_bundle(input_ids))
    delta_ids = parse_csv_ints(args.delta_token_ids)
    (requester_dir / "delta.bin").write_bytes(tensor_bundle(delta_ids))
    policy = run_root / "policy.conf"
    policy.write_text(policy_text(provider_names))
    (run_root / "trust-schema.conf").write_text((ROOT / "examples/trust-schema.conf").read_text())
    authority_dir = run_root / "authority"; authority_dir.mkdir()
    provider_dirs = []
    for index in range(len(stages)):
        path = run_root / f"provider-{index}"; path.mkdir(); provider_dirs.append(path)
    req_private, req_public = make_external_key(requester_dir, "requester")
    auth_private, auth_public = make_external_key(authority_dir, "authority")
    (authority_dir / "content-key.bin").write_bytes(os.urandom(32))
    (authority_dir / "content-key.bin").chmod(0o600)
    provider_keys = {}
    for index, directory in enumerate(provider_dirs):
        recipient_private, recipient_public = make_external_key(directory, "recipient")
        offer_private, offer_public = make_external_key(directory, "offer")
        provider_keys[index] = (recipient_private, recipient_public, offer_private, offer_public)
    # Build one public certificate set, copy it to each node, then remove
    # private key files for identities that node does not own.  This mirrors
    # a real deployment: validators can resolve peer certificates without
    # turning every MiniNDN host into a shared private-key store.
    nodes = [args.controller_node, args.user_node, *stage_nodes]
    homes = {node: run_root / "homes" / node for node in nodes}
    for node, home in homes.items():
        (home / ".ndn").mkdir(parents=True, exist_ok=True)
        (home / "pib").mkdir(parents=True, exist_ok=True)
        (home / "tpm").mkdir(parents=True, exist_ok=True)
        (home / ".ndn/client.conf").write_text(f"transport=unix:///run/nfd/{node}.sock\n")
    identities = [(args.controller_node, CONTROLLER), (args.controller_node, AUTHORITY),
                  (args.user_node, USER), *zip(stage_nodes, provider_names)]
    bootstrap_home = run_root / "homes/bootstrap"
    (bootstrap_home / ".ndn").mkdir(parents=True, exist_ok=True)
    (bootstrap_home / "pib").mkdir(parents=True, exist_ok=True)
    (bootstrap_home / "tpm").mkdir(parents=True, exist_ok=True)
    (bootstrap_home / ".ndn/client.conf").write_text("transport=unix:///run/nfd/bootstrap.sock\n")
    certs = {}
    for _, identity in identities:
        cert = run_root / "certs" / (identity.strip("/").replace("/", "_") + ".cert")
        cert.parent.mkdir(parents=True, exist_ok=True)
        prefix = env_command(env_for(bootstrap_home, "bootstrap"), "ndnsec")
        subprocess.run(prefix + f" key-gen -t r {shlex.quote(identity)}", shell=True, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        subprocess.run(prefix + f" cert-dump -i {shlex.quote(identity)} > {shlex.quote(str(cert))}",
                       shell=True, check=True)
        certs[identity] = cert
    for node, home in homes.items():
        shutil.copytree(bootstrap_home / "pib", home / "pib", dirs_exist_ok=True)
        shutil.copytree(bootstrap_home / "tpm", home / "tpm", dirs_exist_ok=True)
        with sqlite3.connect(home / "pib/pib.db") as conn:
            conn.execute("update tpmInfo set tpm_locator=?", ("tpm-file:" + str(home / "tpm"),))
            conn.commit()
        keep = {identity for owner, identity in identities if owner == node}
        with sqlite3.connect(home / "pib/pib.db") as conn:
            rows = conn.execute("select identities.identity, keys.key_name from identities join keys on keys.identity_id=identities.id").fetchall()
        for identity_blob, key_name in rows:
            try:
                from ndn.encoding import Component, Name
                parts, _ = Name.decode(bytes(identity_blob))
                identity = "/" + "/".join(Component.to_str(item.tobytes()) for item in parts)
            except Exception:
                continue
            if identity in keep:
                continue
            (home / "tpm/ndnsec-key-file" / (hashlib.sha256(bytes(key_name)).hexdigest() + ".privkey")).unlink(missing_ok=True)
    for node, home in homes.items():
        default_identity = next(identity for owner, identity in identities if owner == node)
        prefix = env_command(env_for(home, node), "ndnsec")
        subprocess.run(prefix + f" set-default -n {shlex.quote(default_identity)}",
                       shell=True, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Find each provider's actual signing key prefix for NativeOfferAdmission.
    def key_prefix(index: int) -> str:
        db = homes[stage_nodes[index]] / "pib/pib.db"
        with sqlite3.connect(db) as conn:
            for (value,) in conn.execute("select key_name from keys"):
                raw = bytes(value)
                try:
                    from ndn.encoding import Component, Name
                    parts, _ = Name.decode(raw)
                    uri = "/" + "/".join(Component.to_str(item.tobytes()) for item in parts)
                except Exception:
                    continue
                if uri.startswith(provider_names[index] + "/KEY/"):
                    return uri
        raise RuntimeError(f"provider key prefix unavailable: {provider_names[index]}")
    offer_entries = []; public_key_files = {}
    for index, provider in enumerate(provider_names):
        _, _, _, offer_public = provider_keys[index]
        offer_id = public_key_digest(offer_public)
        public_key_files[offer_id] = str(offer_public)
        prefix = key_prefix(index)
        offer_entries.append({"provider": provider, "service": SERVICE,
                              "keyLocatorPrefix": prefix,
                              "signerKeyId": offer_id,
                              "certificateName": prefix + "/ID-CERT"})
    candidate_digest = digest_text("qwen06b-offer-policy")
    offer_policy = {"schema": "spec180-provider-offer-trust-v1", "candidateId": "qwen06b-local",
                    "candidateDigest": candidate_digest, "trustSchema": APP_ROOT + "/trust",
                    "entries": offer_entries}
    authority_cfg = {
        "schema": "ndnsf-di-native-authority-v1", "run_for_ms": 300000,
        "permission_bootstrap_ms": 30000, "max_grant_ttl_ms": 120000,
        "authority": {"identity": AUTHORITY, "service": "/HELLO", "group": GROUP,
                       "controller_identity": CONTROLLER, "requester_identity": USER,
                       "protection_epoch": "epoch-1", "content_key_id": "model-key",
                       "trust_schema_file": "../trust-schema.conf",
                       "authority_private_key_file": "authority-private.pem",
                       "requester_public_key_file": "../requester/requester-public.pem",
                       "content_key_file": "content-key.bin",
                       "allowed_model_manifests": [manifest_digest],
                       "recipient_public_key_files": {provider: str(provider_dirs[index] / "recipient-public.pem") for index, provider in enumerate(provider_names)},
                       "publication_sources": {manifest_digest: {
                           "model_name": model_descriptor["model_name"],
                           "model_content_digest": model_descriptor["content_digest"],
                           "canonical_source_digest": source_digest,
                           "artifact_profile_digest": catalog["recipe"]["artifact_profile_digest"]}}}}
    (authority_dir / "authority-private.pem").write_bytes(auth_private.read_bytes())
    (authority_dir / "requester-public.pem").write_bytes(req_public.read_bytes())
    for index, provider in enumerate(provider_names):
        provider_dirs[index].joinpath("recipient-public.pem").write_bytes(provider_keys[index][1].read_bytes())
    (authority_dir / "authority.json").write_text(json.dumps(authority_cfg, indent=2, sort_keys=True) + "\n")
    # Provider maps and requester config are written after key paths are stable.
    for index, provider in enumerate(provider_names):
        directory = provider_dirs[index]
        (directory / "plan.json").write_text(stage_plan.read_text())
        # Serving providers advertise role metadata and wait for the
        # authenticated Selection projection to bind a concrete artifact.
        # Keeping exporter-local `path` fields here would make the native
        # provider reject the manifest as a preassembled startup artifact.
        provider_manifest = json.loads(service_manifest.read_text())
        for service in provider_manifest.get("services", []):
            for artifact in service.get("artifacts", []):
                artifact.pop("path", None)
        (directory / "manifest.json").write_text(
            json.dumps(provider_manifest, indent=2, sort_keys=True) + "\n")
        (directory / "trust-schema.conf").write_text((ROOT / "examples/trust-schema.conf").read_text())
        (directory / "authority-public.pem").write_bytes(auth_public.read_bytes())
        (directory / "offer-private.pem").write_bytes(provider_keys[index][2].read_bytes())
        (directory / "recipient-private.pem").write_bytes(provider_keys[index][0].read_bytes())
        (directory / "recipient-key-map.json").write_text(json.dumps({provider: str(directory / "recipient-private.pem")}))
    requester_cfg = {
        "schema": "ndnsf-di-native-requester-v1",
        "catalog": {**catalog, "source": {**catalog["source"], "file": "model-source.json"}},
        "core": {"requester_identity": USER, "authority_identity": CONTROLLER,
                 "group": GROUP, "trust_schema_file": "../trust-schema.conf"},
        "grant": {"authority_identity": AUTHORITY, "authority_service": "/HELLO",
                   "authority_public_key_file": "authority-public.pem",
                   "requester_private_key_file": "requester-private.pem",
                   "protection_epoch": "epoch-1"},
        "offer_admission": {"policy": offer_policy, "public_key_files": public_key_files,
                            "candidate_digest": candidate_digest},
        "limits": {"bootstrap_ms": 30000, "max_source_bytes": 1 << 20,
                    "max_assembled_bytes": 8 << 30},
        "request": {"service": SERVICE, "task": "text-generation",
                     "adapter_composition_digest": adapter_digest,
                     "task_descriptor_digest": digest_text("qwen06b-task"),
                     "generation_mode": "TOKEN_STREAMING", "tokenizer_digest": tokenizer_digest,
                     "input_layout_digest": digest_text("qwen06b-input-layout"),
                     "security_policy_digest": digest_text("qwen06b-security"),
                     "max_candidates": 1, "max_policy_ms": 5000, "provider_names": provider_names,
                     "max_reentries": 1, "no_progress_ms": 30000, "timeout_ms": 180000,
                     "ack_timeout_ms": 30000, "options_file": "options.json"},
        "conversation": {"schema": "ndnsf-di-native-conversation-v1",
                          "journal": {"state_root": "conversation-state", "identity": "qwen06b-user",
                                      "keys": [{"id": "active", "file": "conversation.key"}],
                                      "quota_bytes": 134217728},
                          "owner": {"requester_identity": USER, "service_name": SERVICE,
                                    "security_domain_digest": digest_text("qwen06b-security")},
                          "turn": {"conversation_id": "qwen06b-minindn-conversation",
                                   "parent_context_epoch": 0, "service_name": SERVICE,
                                   "plan_role_map_digest": role_map_digest,
                                   "retention_deadline_ms": int(time.time() * 1000) + 240000,
                                   "mode": "FULL_CONTEXT", "generation_id": "0123456789abcdef0123456789abcdef",
                                   "canonical_token_ids": input_ids,
                                   "expected_roles": [stage["role"] for stage in stages]},
                          "checkpoint_output_file": "conversation-state.json"},
    }
    (requester_dir / "requester-private.pem").write_bytes(req_private.read_bytes())
    (requester_dir / "authority-public.pem").write_bytes(auth_public.read_bytes())
    (requester_dir / "conversation.key").write_bytes(os.urandom(32))
    (requester_dir / "conversation.key").chmod(0o600)
    (requester_dir / "trust-schema.conf").write_text((ROOT / "examples/trust-schema.conf").read_text())
    (requester_dir / "options.json").write_text(json.dumps({
        "useCache": True, "outputMode": "TOKEN_STREAMING",
        "generationId": "0123456789abcdef0123456789abcdef",
        "maxNewTokens": args.max_new_tokens, "tokenizerDigest": tokenizer_digest,
        "eosTokenIds": [151645], "sampling": {"mode": "Greedy", "temperature": 0.0,
        "topK": 1, "topP": 1.0, "repetitionPenalty": 1.0, "seed": 18406},
        "stopStrings": [], "tokenInputName": "input_ids",
    }, indent=2, sort_keys=True) + "\n")
    (requester_dir / "config.json").write_text(json.dumps(requester_cfg, indent=2, sort_keys=True) + "\n")
    # MiniNDN and process orchestration starts here.
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.apps.nlsr import Nlsr
    from minindn.helpers.nfdc import Nfdc
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.minindn import Minindn
    from minindn.util import getPopen
    # MiniNDN's constructor parses the process argv itself.  Remove runner
    # options after our parser has consumed them so its CLI never sees them.
    sys.argv = [sys.argv[0]]
    Minindn.cleanUp()
    ndn = Minindn(topoFile=str(args.topology.expanduser().resolve()))
    processes = []
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        AppManager(ndn, ndn.net.hosts, Nlsr, sync="psync", security=False,
                   faceType="udp", nFaces=3, routingType="link-state", logLevel="INFO")
        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin([ndn.net[args.controller_node]], [CONTROLLER, CONTROLLER + "/KEY", AUTHORITY, AUTHORITY + "/KEY"])
        rh.addOrigin([ndn.net[args.user_node]], [USER, USER + "/KEY"])
        for node, provider in zip(stage_nodes, provider_names):
            rh.addOrigin([ndn.net[node]], [provider, provider + "/KEY"])
        rh.addOrigin([ndn.net[args.user_node], *[ndn.net[node] for node in stage_nodes]], [GROUP])
        rh.calculateRoutes()
        time.sleep(max(0.0, args.nlsr_wait_s))
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, APP_ROOT, Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, GROUP, Nfdc.STRATEGY_MULTICAST)
        def start(node_name: str, label: str, command: str, env: dict[str, str]):
            log = run_root / f"{label}.log"
            handle = log.open("wb")
            proc = getPopen(ndn.net[node_name], command, envDict=env, shell=True,
                            stdout=handle, stderr=subprocess.STDOUT)
            processes.append((proc, handle, log))
            return proc, log
        build = args.build.expanduser().resolve()
        authority_env = env_for(homes[args.controller_node], args.controller_node)
        # App_ServiceController keeps the maintained trust-schema default as a
        # repository-relative path.  MiniNDN launches commands from a host
        # namespace working directory, so make that dependency explicit while
        # keeping all generated run artifacts absolute and portable.
        controller_cmd = (f"cd {shlex.quote(str(ROOT))} && exec "
                          f"{shlex.quote(str(args.controller_binary.expanduser().resolve()))} "
                          f"--controller-prefix {shlex.quote(CONTROLLER)} --policy-file {shlex.quote(str(policy))} "
                          f"--ensure-identities {shlex.quote(AUTHORITY + ',' + ','.join(provider_names) + ',' + USER)} "
                          "--no-serve-certificates --run-for-ms 300000")
        controller_proc, controller_log = start(args.controller_node, "controller", controller_cmd, authority_env)
        wait_for_marker(controller_proc, controller_log, ("ServiceController started",), args.startup_timeout_s)
        authority_cmd = (f"{shlex.quote(str(build / 'examples/DI_NativeArtifactAuthority'))} "
                         f"--config {shlex.quote(str(authority_dir / 'authority.json'))}")
        authority_proc, authority_log = start(args.controller_node, "authority", authority_cmd, authority_env)
        wait_for_marker(authority_proc, authority_log, ("NATIVE_GRANT_AUTHORITY_READY",), args.startup_timeout_s)
        for index, node in enumerate(stage_nodes):
            provider = provider_names[index]; directory = provider_dirs[index]
            env = env_for(homes[node], node)
            env.update({"SPEC181_GRANT_AUTHORITY_PUBLIC_KEY": str(directory / "authority-public.pem"),
                        "SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(directory / "recipient-key-map.json"),
                        "NDNSF_DI_WORKER_BINARY": str(build / "DI_NativeOnnxAssemblyWorker")})
            command = (f"{shlex.quote(str(build / 'examples/di-native-provider'))} --plan {shlex.quote(str(directory / 'plan.json'))} "
                       f"--manifest {shlex.quote(str(directory / 'manifest.json'))} --service {shlex.quote(SERVICE)} "
                       f"--provider {shlex.quote(provider)} --group {shlex.quote(GROUP)} --controller {shlex.quote(CONTROLLER)} "
                       f"--trust-schema {shlex.quote(str(directory / 'trust-schema.conf'))} --roles {shlex.quote(stages[index]['role'])} "
                       f"--serve --run-for-ms 240000 --artifact-cache-dir {shlex.quote(str(directory / 'cache'))} "
                       f"--tokenizer-json {shlex.quote(str(tokenizer))} --selection-offer-key-file {shlex.quote(str(directory / 'offer-private.pem'))} "
                       "--offer-backend onnxruntime-cpu --offer-can-provision --offer-has-model")
            proc, log = start(node, f"provider-{index}", command, env)
            wait_for_marker(proc, log, ("NDNSF_DI_NATIVE_PROVIDER_READY", "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY"), args.startup_timeout_s)
        requester_env = env_for(homes[args.user_node], args.user_node)
        requester_cmd = (f"{shlex.quote(str(build / 'examples/DI_NativeRequester'))} --config {shlex.quote(str(requester_dir / 'config.json'))} "
                         f"--input {shlex.quote(str(requester_dir / 'input.bin'))} --output {shlex.quote(str(requester_dir / 'output-0.bin'))}")
        first_proc, first_log = start(args.user_node, "requester-0", requester_cmd, requester_env)
        first_proc.wait(timeout=240)
        first_text = first_log.read_text(errors="replace")
        if first_proc.returncode != 0 or "NATIVE_REQUEST_SUCCEEDED" not in first_text or "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN" not in first_text:
            raise RuntimeError(f"first native Qwen round failed: {first_log}")
        first_output = validate_native_output(requester_dir / "output-0.bin")
        round_records = [{"round": 0, "returncode": first_proc.returncode,
                          "log": str(first_log), "output": str(requester_dir / "output-0.bin"),
                          **first_output}]
        for round_index in range(1, args.rounds):
            cfg = json.loads((requester_dir / "config.json").read_text())
            generation_id = f"{round_index:032x}"
            cfg["request"]["options_file"] = f"options-{round_index}.json"
            cfg["conversation"]["turn"] = {"mode": "APPEND_DELTA", "generation_id": generation_id,
                "parent_state_file": "conversation-state.json", "delta_token_ids": delta_ids}
            cfg["conversation"].pop("checkpoint_output_file", None)
            (requester_dir / f"options-{round_index}.json").write_text(json.dumps({
                "useCache": True, "outputMode": "TOKEN_STREAMING", "generationId": generation_id,
                "maxNewTokens": args.max_new_tokens, "tokenizerDigest": tokenizer_digest,
                "eosTokenIds": [151645], "sampling": {"mode": "Greedy", "temperature": 0.0,
                "topK": 1, "topP": 1.0, "repetitionPenalty": 1.0, "seed": 18406 + round_index},
                "tokenInputName": "input_ids"}, indent=2, sort_keys=True))
            cfg_path = requester_dir / f"config-{round_index}.json"; cfg_path.write_text(json.dumps(cfg, indent=2, sort_keys=True) + "\n")
            proc, log = start(args.user_node, f"requester-{round_index}",
                              requester_cmd.replace("config.json", cfg_path.name).replace("input.bin", "delta.bin").replace("output-0.bin", f"output-{round_index}.bin"), requester_env)
            proc.wait(timeout=240)
            text = log.read_text(errors="replace")
            if proc.returncode != 0 or "NATIVE_REQUEST_SUCCEEDED" not in text:
                raise RuntimeError(f"native Qwen round {round_index} failed: {log}")
            output_summary = validate_native_output(requester_dir / f"output-{round_index}.bin")
            round_records.append({"round": round_index, "returncode": proc.returncode,
                                  "log": str(log), "output": str(requester_dir / f"output-{round_index}.bin"),
                                  **output_summary})
        if args.negative_parent:
            cfg = json.loads((requester_dir / "config-1.json").read_text() if args.rounds > 1 else (requester_dir / "config.json").read_text())
            cfg["conversation"]["turn"]["parent_checkpoint_digest"] = "sha256:" + "0" * 64
            bad = requester_dir / "config-negative.json"; bad.write_text(json.dumps(cfg, indent=2, sort_keys=True) + "\n")
            proc, log = start(args.user_node, "requester-negative",
                              requester_cmd.replace("config.json", bad.name).replace("input.bin", "delta.bin").replace("output-0.bin", "output-negative.bin"), requester_env)
            proc.wait(timeout=120); text = log.read_text(errors="replace")
            if proc.returncode == 0 or "DI_NATIVE_CONVERSATION_PARENT_MISMATCH" not in text:
                raise RuntimeError(f"negative parent case did not fail closed: {log}")
            round_records.append({"round": "negative-parent", "returncode": proc.returncode, "log": str(log)})
        record = {"schema": "ndnsf-di-qwen06b-native-minindn-run-v1", "model": model_name,
                  "revision": revision, "stageManifest": str(stage_manifest_path),
                  "stageManifestDigest": manifest_digest, "tokenizerDigest": tokenizer_digest,
                  "topology": str(args.topology.expanduser().resolve()), "stageNodes": stage_nodes,
                  "rounds": round_records, "status": "PASS"}
        (run_root / "run-record.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        print("NDNSF_DI_QWEN06B_NATIVE_MININDN_PASS " + json.dumps(record, sort_keys=True))
        return 0
    finally:
        for proc, handle, _ in reversed(processes):
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
        for proc, handle, _ in reversed(processes):
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(timeout=2)
            handle.close()
        ndn.stop()


if __name__ == "__main__":
    raise SystemExit(main())
