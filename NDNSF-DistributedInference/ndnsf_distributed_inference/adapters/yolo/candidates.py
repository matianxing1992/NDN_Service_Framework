"""Registered provider-independent YOLO26n candidate catalogue."""

from __future__ import annotations

import hashlib
import json
import base64
from dataclasses import dataclass, field
import math
from pathlib import Path
from typing import Any, Mapping


CATALOGUE_SCHEMA = "spec180-yolo-catalogue-v1"
EXPECTED_IDS = ("atomic-v1", "shared-backbone-two-shard-v1")


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8")


def _digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(_canonical(value)).hexdigest()


@dataclass(frozen=True)
class RegisteredYoloCandidate:
    candidate_id: str
    priority: int
    roles: tuple[Mapping[str, str], ...]
    input_ingress_role: str
    result_egress_role: str
    merge_kind: str
    safe_cuts: tuple[Mapping[str, Any], ...]
    candidate_digest: str
    semantic_partition: Mapping[str, Any] = field(default_factory=dict)


def _verify_semantic_partition(value: Any, role_names: tuple[str, ...]) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        raise ValueError("YOLO semantic partition is missing")
    if not value and role_names == ("FullModel",):
        return {}
    if value.get("schema") != "spec180-yolo-semantic-partition-v1":
        raise ValueError("YOLO semantic partition schema is unsupported")
    role_sets = value.get("roleNodeSets")
    if not isinstance(role_sets, Mapping) or set(role_sets) != set(role_names):
        raise ValueError("YOLO semantic partition role cover is incomplete")
    all_nodes: list[str] = []
    for role in role_names:
        nodes = role_sets.get(role)
        if (not isinstance(nodes, list) or not nodes
                or any(not isinstance(node, str) or not node for node in nodes)
                or len(set(nodes)) != len(nodes)):
            raise ValueError("YOLO semantic partition node set is invalid")
        all_nodes.extend(nodes)
    if len(set(all_nodes)) != len(all_nodes):
        raise ValueError("YOLO semantic partition assigns a node twice")
    node_branch = value.get("nodeBranch")
    ownership = value.get("branchOwnership")
    if (not isinstance(node_branch, Mapping) or set(node_branch) != set(all_nodes)
            or not isinstance(ownership, Mapping) or not ownership):
        raise ValueError("YOLO semantic partition branch ownership is incomplete")
    if any(not isinstance(branch, str) or not branch
           or not isinstance(role, str) or role not in role_names
           for branch, role in ownership.items()):
        raise ValueError("YOLO semantic partition branch ownership is invalid")
    if any(not isinstance(node, str) or not isinstance(branch, str)
           or not branch or branch not in ownership
           for node, branch in node_branch.items()):
        raise ValueError("YOLO semantic partition node branches are invalid")
    owner_by_node = {
        node: role for role, nodes in role_sets.items() for node in nodes
    }
    if any(ownership[node_branch[node]] != owner_by_node[node]
           for node in all_nodes):
        raise ValueError("YOLO semantic partition branch role disagrees with node set")
    role_interfaces = value.get("roleInterfaces")
    if (not isinstance(role_interfaces, Mapping)
            or set(role_interfaces) != set(role_names)):
        raise ValueError("YOLO semantic partition role interfaces are incomplete")
    for role in role_names:
        descriptor = role_interfaces.get(role)
        if (not isinstance(descriptor, Mapping)
                or not isinstance(descriptor.get("inputs"), list)
                or not descriptor["inputs"]
                or not isinstance(descriptor.get("outputs"), list)
                or not descriptor["outputs"]):
            raise ValueError("YOLO semantic partition role interface is invalid")
        for endpoint in list(descriptor["inputs"]) + list(descriptor["outputs"]):
            if (not isinstance(endpoint, Mapping)
                    or not isinstance(endpoint.get("name"), str)
                    or not endpoint["name"]
                    or not isinstance(endpoint.get("dtype"), str)
                    or not isinstance(endpoint.get("shape"), list)):
                raise ValueError("YOLO semantic role endpoint is invalid")
    interfaces = value.get("tensorInterfaces")
    if not isinstance(interfaces, list) or not interfaces:
        raise ValueError("YOLO semantic partition tensor interfaces are missing")
    interface_ids: list[str] = []
    for item in interfaces:
        if not isinstance(item, Mapping):
            raise ValueError("YOLO semantic partition interface is invalid")
        required = ("edgeId", "producerNode", "producerRole", "consumerNodes",
                    "consumerRoles", "dtype", "shape")
        if any(key not in item for key in required):
            raise ValueError("YOLO semantic partition interface is incomplete")
        if (not isinstance(item["edgeId"], str) or not item["edgeId"]
                or not isinstance(item["producerNode"], str)
                or item["producerNode"] not in set(all_nodes)
                or item["producerRole"] not in role_names
                or not isinstance(item["consumerNodes"], list)
                or not item["consumerNodes"]
                or not isinstance(item["consumerRoles"], list)
                or not item["consumerRoles"]
                or any(role not in role_names for role in item["consumerRoles"])
                or not isinstance(item["dtype"], str)
                or not isinstance(item["shape"], list)):
            raise ValueError("YOLO semantic partition interface is invalid")
        interface_ids.append(item["edgeId"])
    if len(set(interface_ids)) != len(interface_ids):
        raise ValueError("YOLO semantic partition duplicates a tensor interface")
    dependencies = value.get("dependencyEdges")
    cuts = value.get("safeCuts")
    if not isinstance(dependencies, list) or not dependencies:
        raise ValueError("YOLO semantic partition dependencies are missing")
    if not isinstance(cuts, list) or not cuts:
        raise ValueError("YOLO semantic partition safe cuts are missing")
    dependency_pairs: list[tuple[str, str]] = []
    for item in dependencies:
        if (not isinstance(item, Mapping) or item.get("fromRole") not in role_names
                or item.get("toRole") not in role_names
                or item.get("fromRole") == item.get("toRole")
                or not isinstance(item.get("tensorEdges"), list)
                or not item["tensorEdges"]):
            raise ValueError("YOLO semantic partition dependency is invalid")
        dependency_pairs.append((str(item["fromRole"]), str(item["toRole"])))
    if len(set(dependency_pairs)) != len(dependency_pairs):
        raise ValueError("YOLO semantic partition duplicates a dependency")
    cut_pairs: list[tuple[str, str]] = []
    for item in cuts:
        if (not isinstance(item, Mapping) or not item.get("cutId")
                or item.get("fromRole") not in role_names
                or item.get("toRole") not in role_names
                or item.get("fromRole") == item.get("toRole")
                or not isinstance(item.get("boundaryTensors"), list)
                or not item["boundaryTensors"]):
            raise ValueError("YOLO semantic partition safe cut is invalid")
        cut_pairs.append((str(item["fromRole"]), str(item["toRole"])))
    if len(set(cut_pairs)) != len(cut_pairs):
        raise ValueError("YOLO semantic partition duplicates a safe cut")
    equivalence = value.get("equivalence")
    try:
        atol = float(equivalence.get("atol", 0)) if isinstance(equivalence, Mapping) else 0.0
        rtol = float(equivalence.get("rtol", 0)) if isinstance(equivalence, Mapping) else 0.0
    except (TypeError, ValueError):
        atol = rtol = 0.0
    if (not isinstance(equivalence, Mapping)
            or not isinstance(equivalence.get("oraclePath"), str)
            or not equivalence["oraclePath"]
            or not isinstance(equivalence.get("method"), str)
            or not equivalence["method"]
            or not math.isfinite(atol) or atol <= 0
            or not math.isfinite(rtol) or rtol <= 0):
        raise ValueError("YOLO semantic partition equivalence oracle is invalid")
    return dict(value)


def _verify_candidate(row: Mapping[str, Any]) -> RegisteredYoloCandidate:
    required = ("candidateId", "selectionPriority", "roles",
                "inputIngressRole", "resultEgressRole", "mergeKind",
                "safeCuts", "candidateDigest", "semanticPartition")
    if any(key not in row for key in required):
        raise ValueError("YOLO catalogue candidate is incomplete")
    unsigned = dict(row)
    expected = str(unsigned.pop("candidateDigest"))
    if _digest(unsigned) != expected:
        raise ValueError("YOLO catalogue candidate digest is not registered")
    roles = tuple(dict(item) for item in row["roles"])
    role_names = tuple(str(item.get("role", "")) for item in roles)
    if not role_names or any(not name or not item.get("kind") for name, item in zip(role_names, roles)):
        raise ValueError("YOLO catalogue role identity is incomplete")
    ingress = str(row["inputIngressRole"])
    egress = str(row["resultEgressRole"])
    if ingress not in role_names or egress not in role_names:
        raise ValueError("YOLO catalogue ingress/egress role is not declared")
    if row["candidateId"] == "atomic-v1" and role_names != ("FullModel",):
        raise ValueError("atomic YOLO candidate must contain FullModel only")
    if row["candidateId"] == "shared-backbone-two-shard-v1" and role_names != (
            "BackboneNeck", "DetectShard0", "DetectShard1", "Merge"):
        raise ValueError("shared YOLO candidate has an unexpected role set")
    if row["candidateId"] == "shared-backbone-two-shard-v1" and not row["safeCuts"]:
        raise ValueError("shared YOLO candidate has no safe cuts")
    semantic_partition = _verify_semantic_partition(row["semanticPartition"], role_names)
    if row["candidateId"] == "shared-backbone-two-shard-v1":
        declared_cuts = semantic_partition.get("safeCuts", ())
        if (not isinstance(row["safeCuts"], list)
                or any(not isinstance(item, Mapping)
                       or "afterNode" in item for item in row["safeCuts"])
                or row["safeCuts"] != list(declared_cuts)):
            raise ValueError("YOLO candidate safe cuts are not semantic and graph-bound")
    priority = int(row["selectionPriority"])
    if priority < 0:
        raise ValueError("YOLO catalogue selection priority must be non-negative")
    return RegisteredYoloCandidate(
        candidate_id=str(row["candidateId"]),
        priority=priority,
        roles=roles,
        input_ingress_role=ingress,
        result_egress_role=egress,
        merge_kind=str(row["mergeKind"]),
        safe_cuts=tuple(dict(item) for item in row["safeCuts"]),
        candidate_digest=expected,
        semantic_partition=semantic_partition,
    )


def verify_catalogue(catalogue: Mapping[str, Any]) -> tuple[RegisteredYoloCandidate, ...]:
    """Verify the registered candidate set without making list order authority."""

    if catalogue.get("schema") != CATALOGUE_SCHEMA:
        raise ValueError("YOLO catalogue schema is unsupported")
    rows = catalogue.get("candidates")
    if not isinstance(rows, list):
        raise ValueError("YOLO catalogue candidates are not a list")
    ids = tuple(str(item.get("candidateId", "")) for item in rows)
    if len(ids) != len(set(ids)) or set(ids) != set(EXPECTED_IDS):
        raise ValueError("YOLO catalogue candidate set is incomplete or duplicated")
    candidates = tuple(_verify_candidate(row) for row in rows)
    if set(item.candidate_id for item in candidates) != set(EXPECTED_IDS):
        raise ValueError("YOLO catalogue candidates are incomplete")
    return candidates


def verify_catalogue_signature(catalogue: Mapping[str, Any], registry_path: str | Path) -> None:
    """Verify the signed catalogue against the checked-in trust root."""

    from cryptography.exceptions import InvalidSignature

    try:
        registry = json.loads(Path(registry_path).read_text(encoding="utf-8"))
        entry = registry["catalogue"]
        signature = catalogue["signature"]
        registry_file = Path(registry_path)
        # The checked-in registry stores paths relative to the feature
        # directory (the registry itself lives in ``contracts/``); isolated
        # tests may place the registry and key beside each other.
        feature_dir = (registry_file.parent
                       if (registry_file.parent / "spec.md").exists()
                       else registry_file.parent.parent
                       if registry_file.parent.name == "contracts"
                       else registry_file.parent)
        public_path = feature_dir / str(entry["publicKeyPath"])
        public_bytes = public_path.read_bytes()
        if hashlib.sha256(public_bytes).hexdigest() != str(entry["publicKeySha256"])[7:]:
            raise ValueError("YOLO catalogue trust-root public key digest mismatch")
        if (registry.get("status") != "CONFIGURED"
                or signature.get("authorityId") != entry.get("authorityId")
                or signature.get("keyId") != entry.get("keyId")
                or signature.get("algorithm") != "ed25519"):
            raise ValueError("YOLO catalogue signer is not registered")
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
        try:
            from cryptography.hazmat.backends import default_backend
            key = serialization.load_pem_public_key(public_bytes, backend=default_backend())
        except (TypeError, ValueError):
            key = Ed25519PublicKey.from_public_bytes(public_bytes)
        if not isinstance(key, Ed25519PublicKey):
            raise ValueError("YOLO catalogue trust root is not Ed25519")
        body = dict(catalogue)
        body.pop("signature", None)
        key.verify(base64.b64decode(str(signature["valueB64"])), _canonical(body))
    except (OSError, KeyError, ValueError, TypeError, InvalidSignature, json.JSONDecodeError) as exc:
        if isinstance(exc, ValueError) and str(exc).startswith("YOLO catalogue"):
            raise
        raise ValueError("YOLO catalogue signature verification failed") from exc
