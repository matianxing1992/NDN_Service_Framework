"""Build the run-pinned native requester assets for the Spec196 YOLO path."""

from __future__ import annotations

import dataclasses
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import sys
from typing import Any

from native_placement_evidence import (
    _evidence_fields,
    collect_native_placement_evidence,
    validate_native_probe_evidence,
    validate_native_placement_evidence,
)


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")


def _digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _digest_value(value: Any) -> str:
    return _digest_bytes(_canonical_bytes(value))


def _write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2,
                               sort_keys=True) + "\n", encoding="utf-8")


def _write_ed25519_keypair(directory: Path, stem: str) -> tuple[Path, Path]:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    directory.mkdir(parents=True, exist_ok=True)
    private_path = directory / f"{stem}-private.pem"
    public_path = directory / f"{stem}-public.pem"
    private_key = Ed25519PrivateKey.generate()
    private_path.write_bytes(private_key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ))
    public_path.write_bytes(private_key.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    ))
    private_path.chmod(0o600)
    public_path.chmod(0o644)
    return private_path, public_path


def _public_key_id(public_path: Path) -> str:
    from cryptography.hazmat.backends import default_backend
    from cryptography.hazmat.primitives import serialization

    key = serialization.load_pem_public_key(
        public_path.read_bytes(), backend=default_backend())
    raw = key.public_bytes(serialization.Encoding.Raw,
                           serialization.PublicFormat.Raw)
    return _digest_bytes(raw)


def remap_yolo_semantic_partition(
        partition: dict[str, Any], role_map: dict[str, str]) -> dict[str, Any]:
    """Translate the signed semantic role names into runner service roles."""
    result = json.loads(json.dumps(partition))
    result["roleNodeSets"] = {
        role_map[role]: nodes for role, nodes in result["roleNodeSets"].items()
    }
    result["branchOwnership"] = {
        branch: role_map.get(role, role)
        for branch, role in result.get("branchOwnership", {}).items()
    }
    for item in result["tensorInterfaces"]:
        item["producerRole"] = role_map[item["producerRole"]]
        item["consumerRoles"] = sorted(role_map[role]
                                        for role in item["consumerRoles"])
    result["roleInterfaces"] = {
        role_map[role]: value
        for role, value in result["roleInterfaces"].items()
    }
    for item in result["dependencyEdges"]:
        item["fromRole"] = role_map[item["fromRole"]]
        item["toRole"] = role_map[item["toRole"]]
    for item in result["safeCuts"]:
        item["fromRole"] = role_map[item["fromRole"]]
        item["toRole"] = role_map[item["toRole"]]
        item["cutId"] = f"{item['fromRole']}-to-{item['toRole']}"
    return result


def native_yolo_splitter(manifest: dict[str, Any],
                         role_map: dict[str, str]) -> dict[str, Any]:
    catalogue = manifest.get("catalogue")
    candidates = catalogue.get("candidates") if isinstance(catalogue, dict) else None
    if not isinstance(candidates, list):
        raise RuntimeError("SPEC196_YOLO_SIGNED_CATALOGUE_MISSING")
    shared = next((item for item in candidates
                   if item.get("candidateId") == "shared-backbone-two-shard-v1"), None)
    atomic = next((item for item in candidates
                   if item.get("candidateId") == "atomic-v1"), None)
    if shared is None or atomic is None:
        raise RuntimeError("SPEC196_YOLO_REQUIRED_CANDIDATE_MISSING")
    components = [{
        "candidate_id": str(atomic["candidateId"]),
        "priority": int(atomic["selectionPriority"]),
        "roles": [str(item["role"]) for item in atomic["roles"]],
        "node_names_by_role": {},
        "input_ingress_role": str(atomic["inputIngressRole"]),
        "result_egress_role": str(atomic["resultEgressRole"]),
        "merge_kind": str(atomic["mergeKind"]),
        "candidate_digest": str(atomic["candidateDigest"]),
        "semantic_partition": {},
    }]
    components[0]["input_ingress_role"] = "FullModel"
    components[0]["result_egress_role"] = "FullModel"
    mapped_roles = [role_map[str(item["role"])] for item in shared["roles"]]
    partition = remap_yolo_semantic_partition(
        shared["semanticPartition"], role_map)
    if set(partition["roleNodeSets"]) != set(mapped_roles):
        raise RuntimeError("SPEC196_YOLO_ROLE_REMAP_INCOMPLETE")
    components.append({
        "candidate_id": str(shared["candidateId"]),
        "priority": int(shared["selectionPriority"]),
        "roles": mapped_roles,
        "node_names_by_role": {},
        "input_ingress_role": role_map[str(shared["inputIngressRole"])],
        "result_egress_role": role_map[str(shared["resultEgressRole"])],
        "merge_kind": str(shared["mergeKind"]),
        "candidate_digest": str(shared["candidateDigest"]),
        "semantic_partition": partition,
    })
    return {"kind": "YOLO", "components": components,
            "postprocessing": manifest.get("postprocessing", {})}


def add_native_grant_service(policy_path: Path, *, requester: str,
                             authority: str) -> None:
    """Grant the run-scoped native artifact authority its protected service."""
    import yaml

    policy = yaml.safe_load(policy_path.read_text(encoding="utf-8")) or {}
    services = [item for item in policy.get("services", [])
                if isinstance(item, dict) and item.get("name") != "/HELLO"]
    services.append({
        "name": "/HELLO",
        "model": "/HELLO",
        "users": [requester],
        "providers": [{"identity": authority, "roles": ["/HELLO"]}],
        "roles": ["/HELLO"],
        "dependencies": [],
    })
    policy["services"] = services
    users: dict[str, set[str]] = {}
    providers: dict[str, list[dict[str, Any]]] = {}
    for service in services:
        name = str(service.get("name", ""))
        for identity in service.get("users", []):
            users.setdefault(str(identity), set()).add(name)
        for provider in service.get("providers", []):
            if isinstance(provider, dict) and provider.get("identity"):
                providers.setdefault(str(provider["identity"]), []).append({
                    "service": name, "roles": provider.get("roles", [])})
    policy["authorization_summary"] = {
        "users": [{"identity": identity, "services": sorted(names)}
                  for identity, names in sorted(users.items())],
        "providers": [{"identity": identity, "services": values}
                      for identity, values in sorted(providers.items())],
    }
    policy_path.write_text(yaml.safe_dump(policy, sort_keys=False),
                           encoding="utf-8")


def prepare_native_yolo_assets(
        *, bundle_root: Path, checkpoint: Path, input_size: int,
        run_id: str, service_name: str, model_uri: str, group: str,
        requester: str, controller: str, authority: str,
        providers: list[str], service_roles: list[str],
        timeout_ms: int, ack_timeout_ms: int) -> dict[str, Any]:
    """Export and bind all run-specific catalog, grant, and offer inputs."""
    if len(providers) < 2 or len(providers) > 16 or len(set(providers)) != len(providers):
        raise RuntimeError("SPEC196_YOLO_PROVIDER_ROSTER_INVALID")
    if any(not name.startswith("/") for name in providers):
        raise RuntimeError("SPEC196_YOLO_PROVIDER_NAME_INVALID")
    role_map = {
        "BackboneNeck": "/Backbone",
        "DetectShard0": "/Head/Shard/0",
        "DetectShard1": "/Head/Shard/1",
        "Merge": "/Merge",
    }
    if set(role_map.values()) != set(service_roles):
        raise RuntimeError("SPEC196_YOLO_SERVICE_ROLE_SET_MISMATCH")
    checkpoint = checkpoint.expanduser().resolve(strict=True)
    if bundle_root.exists():
        raise RuntimeError(f"SPEC196_YOLO_BUNDLE_ALREADY_EXISTS:{bundle_root}")
    bundle_root.mkdir(parents=True, mode=0o700)
    package_dir = bundle_root / "canonical-package"
    catalogue_private, catalogue_public = _write_ed25519_keypair(
        bundle_root, "catalogue")
    catalogue_registry = bundle_root / "catalogue-registry.json"
    _write_json(catalogue_registry, {
        "status": "CONFIGURED",
        "catalogue": {
            "authorityId": controller + "/catalogue",
            "keyId": "spec196-yolo-catalogue-ed25519-" + run_id,
            "publicKeyAlgorithm": "ed25519",
            "signatureAlgorithm": "ed25519",
            "publicKeyPath": catalogue_public.name,
            "publicKeySha256": _digest_bytes(catalogue_public.read_bytes()),
        },
    })

    repo = Path(__file__).resolve().parents[1]
    exporter_path = repo / "tools/ndnsf-di/export_spec180_yolo26_onnx.py"
    module_spec = importlib.util.spec_from_file_location(
        "spec196_yolo_exporter", exporter_path)
    if module_spec is None or module_spec.loader is None:
        raise RuntimeError("SPEC196_YOLO_EXPORTER_UNAVAILABLE")
    exporter = importlib.util.module_from_spec(module_spec)
    sys.modules[module_spec.name] = exporter
    module_spec.loader.exec_module(exporter)
    try:
        manifest = exporter.build_package(
            checkpoint, package_dir, input_size=input_size,
            signing_key=catalogue_private, registry_path=catalogue_registry)
    finally:
        catalogue_private.unlink(missing_ok=True)

    ndnsf_root = str(repo / "NDNSF-DistributedInference")
    if ndnsf_root not in sys.path:
        sys.path.insert(0, ndnsf_root)
    try:
        from ndnsf_distributed_inference.adapters.onnx.graph import canonical_onnx_identity
        from ndnsf_distributed_inference.adapters.yolo.adapter import build_yolo26n_adapter
    except ImportError as error:
        raise RuntimeError("SPEC196_YOLO_ADAPTER_IMPORT_FAILED") from error

    adapter = build_yolo26n_adapter(package_dir, registry_path=catalogue_registry)
    source_info = manifest["source"]
    content_digest = "sha256:" + str(source_info["checkpointSha256"])
    semantics_digest = _digest_value({
        "preprocessing": manifest.get("preprocessing", {}),
        "postprocessing": manifest.get("postprocessing", {}),
    })
    model = adapter.describe_model(
        "YOLO26n", content_digest, semantics_digest,
        source_revision=str(manifest.get("graphRevision", "")))
    graph = adapter.graph.inspect(model)
    model_json = dataclasses.asdict(model)
    graph_path = package_dir / "canonical/yolo26n.onnx"
    weights_path = package_dir / str(manifest["weights"]["path"])
    canonical_source_digest = _digest_bytes(graph_path.read_bytes())
    manifest_digest = _digest_bytes((package_dir / "manifest.json").read_bytes())
    canonical_graph_digest = canonical_onnx_identity(graph_path).graph_digest
    artifact_profile_digest = _digest_value({
        "graph": str(manifest["graph"]["graphDigest"]),
        "weights": str(manifest["weights"]["digest"]),
        "graphRevision": str(manifest["graphRevision"]),
    })
    graph_bytes = graph_path.stat().st_size
    initializer_bytes = weights_path.stat().st_size
    max_source_bytes = max(graph_bytes, initializer_bytes) + 64 * 1024 * 1024
    max_assembled_bytes = graph_bytes + initializer_bytes + 64 * 1024 * 1024
    max_publication_bytes = max_source_bytes + max_assembled_bytes + 16 * 1024 * 1024
    prepared_bytes = max_source_bytes + max_assembled_bytes + 4 * 1024 * 1024
    ims_limit = min(1_000_000, max(8192,
        math.ceil(max_publication_bytes / 7000) + 8192))
    max_nodes = max(1, len(manifest["graph"].get("nodes", [])))

    authority_private, authority_public = _write_ed25519_keypair(
        bundle_root, "authority")
    requester_private, requester_public = _write_ed25519_keypair(
        bundle_root, "requester")
    content_key = bundle_root / "content.key"
    content_key.write_bytes(os.urandom(32))
    content_key.chmod(0o600)
    authority_public_shared = bundle_root / "authority-public.pem"
    authority_public_shared.write_bytes(authority_public.read_bytes())
    authority_public_shared.chmod(0o644)
    protection_epoch = "epoch-1"
    content_key_id = "spec196-yolo-content-" + run_id

    provider_material: dict[str, dict[str, str]] = {}
    offer_public_key_files: dict[str, str] = {}
    offer_entries: list[dict[str, str]] = []
    recipient_public_files: dict[str, str] = {}
    for index, provider in enumerate(providers):
        # NativeProtectedGrantCredentials resolves the registry's public key
        # from the run root. Keep provider directories directly below it.
        directory = bundle_root / f"provider-{index}"
        recipient_private, recipient_public = _write_ed25519_keypair(
            directory, "recipient")
        offer_private, offer_public = _write_ed25519_keypair(directory, "offer")
        provider_authority_public = directory / "authority-public.pem"
        provider_authority_public.write_bytes(authority_public.read_bytes())
        provider_authority_public.chmod(0o644)
        registry = {
            "schemaVersion": 1,
            "status": "CONFIGURED",
            "artifactPolicyAuthority": {
                "acceptedModelFamilies": [model_uri],
                "authorityId": authority,
                "grantSchema": "ndnsf-di-key-grant-v1",
                "keyId": "spec196-yolo-artifact-policy-ed25519-" + run_id,
                "protectionEpochs": [protection_epoch],
                "publicKeyAlgorithm": "ed25519",
                "publicKeyPath": provider_authority_public.name,
                "publicKeySha256": _digest_bytes(authority_public.read_bytes()),
                "signatureAlgorithm": "ed25519",
            },
        }
        _write_json(directory / "trust-root-registry-v1.json", registry)
        recipient_map = directory / "recipient-key-map.json"
        _write_json(recipient_map, {provider: str(recipient_private.resolve())})
        recipient_public_files[provider] = str(recipient_public.resolve())
        key_id = _public_key_id(offer_public)
        offer_public_key_files[key_id] = str(offer_public.resolve())
        key_prefix = provider + "/KEY"
        offer_entries.append({
            "provider": provider,
            "service": service_name,
            "keyLocatorPrefix": key_prefix,
            "signerKeyId": key_id,
            "certificateName": key_prefix + "/ID-CERT",
        })
        provider_material[provider] = {
            "offer_private_key": str(offer_private.resolve()),
            "authority_public_key": str(provider_authority_public.resolve()),
            "recipient_key_map": str(recipient_map.resolve()),
        }

    offer_candidate_id = "spec196-yolo-native"
    offer_candidate_digest = _digest_value({
        "candidate_id": offer_candidate_id,
        "run_id": run_id,
        "manifest_digest": manifest_digest,
    })
    offer_policy = {
        "schema": "spec180-provider-offer-trust-v1",
        "candidateId": offer_candidate_id,
        "candidateDigest": offer_candidate_digest,
        "trustSchema": group.rstrip("/") + "/trust",
        "entries": offer_entries,
    }

    authority_dir = bundle_root / "authority"
    authority_dir.mkdir(mode=0o700)
    (authority_dir / "authority-private.pem").write_bytes(authority_private.read_bytes())
    (authority_dir / "authority-private.pem").chmod(0o600)
    (authority_dir / "requester-public.pem").write_bytes(requester_public.read_bytes())
    (authority_dir / "requester-public.pem").chmod(0o644)
    (authority_dir / "authority-public.pem").write_bytes(authority_public.read_bytes())
    (authority_dir / "authority-public.pem").chmod(0o644)
    (authority_dir / "content.key").write_bytes(content_key.read_bytes())
    (authority_dir / "content.key").chmod(0o600)
    (authority_dir / "trust-schema.conf").write_text(
        (repo / "examples/trust-schema.conf").read_text(encoding="utf-8"),
        encoding="utf-8")
    authority_private.unlink(missing_ok=True)
    content_key.unlink(missing_ok=True)
    authority_config = {
        "schema": "ndnsf-di-native-authority-v1",
        "run_for_ms": 600_000,
        "permission_bootstrap_ms": 60_000,
        "max_grant_ttl_ms": 600_000,
        "authority": {
            "identity": authority,
            "service": "/HELLO",
            "group": group,
            "controller_identity": controller,
            "requester_identity": requester,
            "protection_epoch": protection_epoch,
            "content_key_id": content_key_id,
            "trust_schema_file": "trust-schema.conf",
            "authority_private_key_file": "authority-private.pem",
            "requester_public_key_file": "requester-public.pem",
            "content_key_file": "content.key",
            "allowed_model_manifests": [manifest_digest],
            "recipient_public_key_files": recipient_public_files,
            "publication_sources": {
                manifest_digest: {
                    "model_name": model_json["model_name"],
                    "model_content_digest": model_json["content_digest"],
                    "canonical_source_digest": canonical_source_digest,
                    "initializer_object_digest": str(manifest["weights"]["digest"]),
                    "artifact_profile_digest": artifact_profile_digest,
                },
            },
        },
    }
    authority_config_path = authority_dir / "authority.json"
    _write_json(authority_config_path, authority_config)

    trust_schema = bundle_root / "trust-schema.conf"
    trust_schema.write_text((repo / "examples/trust-schema.conf").read_text(
        encoding="utf-8"), encoding="utf-8")
    splitter = native_yolo_splitter(manifest, role_map)
    requester_config = {
        "schema": "ndnsf-di-native-requester-v1",
        "catalog": {
            "schema": "ndnsf-di-native-request-catalog-v1",
            "model": model_json,
            "source": {
                "data_name": f"{model_uri}/source/{content_digest[7:]}",
                "digest": canonical_source_digest,
                "model_manifest_digest": manifest_digest,
                "canonical_graph_digest": canonical_graph_digest,
                "file": "canonical-package/canonical/yolo26n.onnx",
                "initializer_file": "canonical-package/" + str(manifest["weights"]["path"]),
                "initializer_digest": str(manifest["weights"]["digest"]),
            },
            "recipe": {
                "artifact_profile_digest": artifact_profile_digest,
                "assembler_descriptor_digest": _digest_bytes(
                    b"ndnsf-di-certified-onnx-assembler-v1"),
                "backend_abi": "onnxruntime-cpu-v1",
                "precision": "float32",
                "quantization": "none",
                "layout": "NCHW",
                "padding": "none",
                "protection_epoch": protection_epoch,
                "max_source_bytes": max_source_bytes,
                "max_assembled_bytes": max_assembled_bytes,
                "max_nodes": max_nodes,
            },
            "publication": {
                "artifact_root": model_uri.rstrip("/") + "/artifacts",
                "package_manifest_digest": manifest_digest,
                "max_publication_bytes": max_publication_bytes,
            },
            "input_format": "OPAQUE",
            "max_payload_bytes": 4 << 20,
            "splitter": splitter,
            "node_mapping": {},
            "state_inputs": {},
            "state_outputs": {},
        },
        "core": {
            "requester_identity": requester,
            "authority_identity": controller,
            "group": group,
            "trust_schema_file": "trust-schema.conf",
        },
        "grant": {
            "authority_identity": authority,
            "authority_service": "/HELLO",
            "authority_public_key_file": "authority-public.pem",
            "requester_private_key_file": "requester-private.pem",
            "protection_epoch": protection_epoch,
        },
        "offer_admission": {
            "policy": offer_policy,
            "public_key_files": offer_public_key_files,
            "candidate_digest": offer_candidate_digest,
        },
        "limits": {
            "bootstrap_ms": 60_000,
            "large_data_ims_limit": ims_limit,
            "max_source_bytes": max_source_bytes,
            "max_assembled_bytes": max_assembled_bytes,
            "max_prepared_bytes": prepared_bytes,
            "max_prepared_entries": 8,
        },
        "request": {
            "service": service_name,
            "task": str(adapter.task.descriptor.task_name),
            "adapter_composition_digest": adapter.composition_digest,
            "task_descriptor_digest": _digest_value({
                "name": adapter.task.descriptor.task_name,
                "input": adapter.task.descriptor.input_schema_digest,
                "options": adapter.task.descriptor.options_schema_digest,
                "result": adapter.task.descriptor.result_schema_digest,
            }),
            "generation_mode": "TOKEN_DIAGNOSTIC",
            "input_layout_digest": _digest_value({
                "tensor": "images", "dtype": "float32",
                "shape": [1, 3, int(input_size), int(input_size)],
                "layout": "NCHW",
            }),
            "security_policy_digest": _digest_value({
                "model_manifest_digest": manifest_digest,
                "protection_epoch": protection_epoch,
                "policy": "spec196-yolo-protected-placement-v1",
            }),
            "placement_strategy": "native-observation-aware-v4",
            "placement_score_mode": "network-only",
            "max_candidates": len(splitter["components"]),
            "max_policy_ms": min(60_000, max(5000, timeout_ms)),
            "provider_names": providers,
            "max_reentries": 1,
            "timeout_ms": max(1, int(timeout_ms)),
            "ack_timeout_ms": max(1, int(ack_timeout_ms)),
        },
    }
    (bundle_root / "requester-private.pem").write_bytes(requester_private.read_bytes())
    (bundle_root / "requester-private.pem").chmod(0o600)
    (bundle_root / "requester-public.pem").write_bytes(requester_public.read_bytes())
    (bundle_root / "requester-public.pem").chmod(0o644)
    (bundle_root / "authority-public.pem").write_bytes(authority_public.read_bytes())
    (bundle_root / "authority-public.pem").chmod(0o644)
    requester_config_path = bundle_root / "requester.json"
    _write_json(requester_config_path, requester_config)
    return {
        "bundle_root": bundle_root,
        "package_dir": package_dir,
        "catalogue_registry": catalogue_registry,
        "requester_config": requester_config_path,
        "authority_binary_config": authority_config_path,
        "authority_dir": authority_dir,
        "provider_material": provider_material,
        "runtime_inputs": sorted(path for path in bundle_root.rglob("*")
                                 if path.is_file()),
        "roles": list(splitter["components"][1]["roles"]),
        "model": model_json,
        "manifest_digest": manifest_digest,
        "canonical_graph_digest": canonical_graph_digest,
        "artifact_profile_digest": artifact_profile_digest,
        "offer_candidate_digest": offer_candidate_digest,
    }


def validate_native_yolo_model_evidence(user_text: str) -> dict[str, Any]:
    """Validate the model terminal result independently of probing and scoring."""
    result_lines = [line for line in user_text.splitlines()
                    if line.startswith("YOLO_NATIVE_REQUEST_RESULT ")]
    if len(result_lines) != 1:
        raise RuntimeError("SPEC196_NATIVE_YOLO_RESULT_MARKER_COUNT")
    result = _evidence_fields(result_lines[0])
    try:
        payload_bytes = int(result.get("payload_bytes", "0"))
    except ValueError:
        payload_bytes = 0
    if result.get("status") != "true" or payload_bytes <= 0:
        raise RuntimeError("SPEC196_NATIVE_YOLO_INFERENCE_FAILED")
    request_id = result.get("request", "")
    if not request_id:
        raise RuntimeError("SPEC196_NATIVE_YOLO_REQUEST_ID_MISSING")

    return result


def collect_native_yolo_evidence(user_text: str, provider_text: str,
                                 expected_roles: list[str], *,
                                 expected_manifest_digest: str,
                                 expected_score_mode: str | None = None) -> dict[str, Any]:
    """Keep probe, placement and model outcomes separate; require all for PASS."""
    report = collect_native_placement_evidence(
        user_text, provider_text, expected_roles,
        expected_manifest_digest=expected_manifest_digest,
        expected_score_mode=expected_score_mode)
    try:
        model = validate_native_yolo_model_evidence(user_text)
        report["modelResult"] = model
        report["boundaries"]["model"] = {"status": "PASS"}
        if report.get("requestId") and model["request"] != report["requestId"]:
            for boundary in ("probe", "placement"):
                report["boundaries"][boundary] = {
                    "status": "FAIL", "reason": "SPEC196_NATIVE_YOLO_MODEL_REQUEST_MISMATCH"}
    except (RuntimeError, TypeError, ValueError) as error:
        report["boundaries"]["model"] = {"status": "FAIL", "reason": str(error)}
    report["status"] = ("PASS" if all(row["status"] == "PASS"
                                    for row in report["boundaries"].values()) else "FAIL")
    return report


def validate_native_yolo_evidence(user_text: str, provider_text: str,
                                  expected_roles: list[str], *,
                                  expected_manifest_digest: str) -> dict[str, Any]:
    """Compatibility entry: retain strict failure behavior and complete evidence."""
    model = validate_native_yolo_model_evidence(user_text)
    try:
        probes = validate_native_probe_evidence(user_text, model["request"])
        placement = validate_native_placement_evidence(
            user_text, provider_text, expected_roles,
            expected_manifest_digest=expected_manifest_digest, probe_evidence=probes)
    except RuntimeError as error:
        raise RuntimeError(str(error).replace(
            "SPEC196_NATIVE_", "SPEC196_NATIVE_YOLO_", 1)) from error
    return {**probes, **placement, "modelResult": model}
