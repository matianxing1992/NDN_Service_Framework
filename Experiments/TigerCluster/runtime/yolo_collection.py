"""Authoritative worker-to-collector handoff for Spec183 YOLO runs.

The Slurm rank processes own node receipts; this module is the small outer
coordinator seam that joins them after every rank has returned.  It only
publishes references to already-retained evidence.  It never creates ACKs,
assignments, model outputs, GPU observations, or a verdict.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re

from .yolo_profile import HASH, _read_plane


class CollectionHandoffError(ValueError):
    """The worker-to-collector evidence handoff is incomplete or unbound."""


_ROLES = frozenset(("BackboneNeck", "DetectShard0", "DetectShard1", "Merge"))
_CASES = frozenset(("local-cpu", "single-node-gpu", "two-node-gpu", "negative-dependency"))


def _digest(payload: bytes) -> str:
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def _json_digest(value: dict) -> str:
    return _digest(json.dumps(value, sort_keys=True, separators=(",", ":"),
                    allow_nan=False).encode())


def _owned_path(value, *, label: str, directory: bool, root: Path | None = None) -> Path:
    if not isinstance(value, (str, Path)) or "\x00" in str(value):
        raise CollectionHandoffError("HANDOFF_PATH:" + label)
    path = Path(value)
    if not path.is_absolute():
        if root is None:
            raise CollectionHandoffError("HANDOFF_PATH:" + label)
        path = root / path
    path = Path(os.path.abspath(str(path)))
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise CollectionHandoffError("HANDOFF_PATH_SYMLINK:" + label)
    if directory:
        if not path.is_dir():
            raise CollectionHandoffError("HANDOFF_DIRECTORY:" + label)
    elif not path.is_file():
        raise CollectionHandoffError("HANDOFF_FILE:" + label)
    return path


def _write_once(path: Path, value: dict) -> str:
    """Write one immutable handoff without replacing an existing record."""
    path = Path(path)
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise CollectionHandoffError("HANDOFF_OUTPUT_SYMLINK")
    if path.exists() or path.is_symlink():
        raise CollectionHandoffError("HANDOFF_OUTPUT_EXISTS")
    if not path.parent.is_dir():
        raise CollectionHandoffError("HANDOFF_OUTPUT_PARENT")
    payload = (json.dumps(value, sort_keys=True, separators=(",", ":"),
                          allow_nan=False) + "\n").encode()
    temporary = path.with_name(path.name + ".tmp")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    fd = os.open(str(temporary), flags, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(str(temporary), str(path))
        path.chmod(0o444)
    finally:
        if temporary.exists():
            temporary.unlink()
    return _digest(payload)


def _node_receipt(root: Path, *, plan: dict, rank: int,
                  candidate_digest: str) -> tuple[str, str]:
    receipt_path = _owned_path(root / "node-receipt.json", label=f"node-{rank}-receipt",
                               directory=False)
    payload = receipt_path.read_bytes()
    receipt = _read_plane(receipt_path)
    required = {"schema", "runId", "case", "rank", "planDigest",
                "preparationDigest", "candidateDigest", "launches", "cleanup",
                "cleanupSummary", "qualification"}
    if (not isinstance(receipt, dict) or set(receipt) != required
            or receipt["schema"] != "tiger-yolo-node-receipt-v3"
            or receipt["runId"] != plan["runId"] or receipt["case"] != plan["case"]
            or receipt["rank"] != rank
            or receipt["candidateDigest"] != candidate_digest
            or not HASH.fullmatch(receipt["preparationDigest"])
            or receipt["qualification"] != "NODE_CLEANUP_COMPONENT_ONLY"):
        raise CollectionHandoffError("HANDOFF_NODE_RECEIPT_BINDING")
    return _digest(payload), receipt["preparationDigest"]


def publish_normal_handoff(path: Path, *, plan: dict, node_roots: dict,
                           references: list[dict], runtime_candidate_digest: str,
                           placement_candidate_id: str,
                           placement_candidate_digest: str, graph_digest: str,
                           catalogue_digest: str, providers_by_role: dict,
                           certified_graph: dict,
                           allocation_expected: dict | None = None) -> dict:
    """Publish a normal or registered-negative handoff after all ranks return.

    ``node_roots`` and ``references`` are supplied by the real worker/staging
    owner.  The function requires all expected rank receipts and refuses to
    create a partial handoff; a caller must invoke it only after both rank
    processes have returned and their exit statuses were checked.
    """
    if (not isinstance(plan, dict) or plan.get("case") not in _CASES
            or not isinstance(plan.get("runId"), str)
            or not HASH.fullmatch(runtime_candidate_digest)
            or not HASH.fullmatch(placement_candidate_digest)
            or not HASH.fullmatch(graph_digest)
            or not HASH.fullmatch(catalogue_digest)
            or not isinstance(placement_candidate_id, str)
            or re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", placement_candidate_id) is None
            or not isinstance(providers_by_role, dict)
            or set(providers_by_role) != _ROLES
            or any(not isinstance(v, str) or not v for v in providers_by_role.values())
            or not isinstance(certified_graph, dict)
            or certified_graph.get("graphDigest") != graph_digest):
        raise CollectionHandoffError("HANDOFF_DESCRIPTOR")
    negative = plan['case'] == 'negative-dependency'
    expected_ranks = {0, 1} if plan["case"] in ('two-node-gpu', 'negative-dependency') else {0}
    if (not isinstance(node_roots, dict) or set(node_roots) != expected_ranks
            or any(type(rank) is not int for rank in node_roots)):
        raise CollectionHandoffError("HANDOFF_NODE_COVERAGE")
    nodes = {}
    for rank in sorted(expected_ranks):
        root = _owned_path(node_roots[rank], label=f"node-{rank}-root", directory=True)
        expected_root = Path(plan["output"]) / ("node" + str(rank))
        if root != expected_root:
            raise CollectionHandoffError("HANDOFF_NODE_ROOT_BINDING")
        receipt_digest, preparation_digest = _node_receipt(
            root, plan=plan, rank=rank, candidate_digest=runtime_candidate_digest)
        nodes[str(rank)] = {"root": str(root), "receiptDigest": receipt_digest,
                            "preparationDigest": preparation_digest}
        if plan["case"] != "local-cpu":
            if allocation_expected is None:
                raise CollectionHandoffError("HANDOFF_ALLOCATION_REQUIRED")
            allocation = root / "slurm-allocation.json"
            probe = root / "gpu-probe.json"
            nodes[str(rank)].update({
                "allocationDigest": _digest(_owned_path(allocation,
                    label=f"node-{rank}-allocation", directory=False).read_bytes()),
                "gpuProbeDigest": _digest(_owned_path(probe,
                    label=f"node-{rank}-gpu-probe", directory=False).read_bytes()),
            })
    expected_references = 0 if negative else 4 if plan["case"] == "two-node-gpu" else 2
    if not isinstance(references, list) or len(references) != expected_references:
        raise CollectionHandoffError("HANDOFF_REFERENCE_COVERAGE")
    retained = []
    for index, reference in enumerate(references):
        if (not isinstance(reference, dict) or set(reference) != {"package", "repository", "inputSize"}
                or type(reference["inputSize"]) is not int or reference["inputSize"] <= 0):
            raise CollectionHandoffError("HANDOFF_REFERENCE_SCHEMA")
        package = _owned_path(reference["package"], label=f"reference-{index}-package",
                              directory=True)
        repository = _owned_path(reference["repository"], label=f"reference-{index}-repository",
                                  directory=True)
        retained.append({"package": str(package), "repository": str(repository),
                         "inputSize": reference["inputSize"]})
    if plan["case"] != "local-cpu":
        if (not isinstance(allocation_expected, dict)
                or set(allocation_expected) != {"job_id", "submission_key", "partition", "gpu_type"}):
            raise CollectionHandoffError("HANDOFF_ALLOCATION_SCHEMA")
    value = {"schema": "tiger-yolo-collection-input-v1", "status": "READY",
             "runId": plan["runId"], "candidateDigest": runtime_candidate_digest,
             "case": plan["case"], "kind": "expected-rejection" if negative else "normal",
             "runtimeCandidateDigest": runtime_candidate_digest,
             "placementCandidateId": placement_candidate_id,
             "placementCandidateDigest": placement_candidate_digest,
             "graphDigest": graph_digest, "catalogueDigest": catalogue_digest,
             "providersByRole": dict(providers_by_role), "nodes": nodes,
             "references": retained, "certifiedGraph": certified_graph}
    if allocation_expected is not None:
        value["allocationExpected"] = dict(allocation_expected)
    output = Path(path)
    digest = _write_once(output, value)
    return dict(value, path=str(output), collectionInputDigest=digest)
