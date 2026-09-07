"""Production-shaped rank operator for the Spec183 YOLO application.

This module is the narrow seam between an immutable prepared run and the
maintained application coordinator.  It owns no placement decisions and does
not manufacture ACKs, selections, model outputs, or verdicts.  The caller must
provide the signed preparation receipt, allocation evidence (for GPU cases),
and the independent request collector callback.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import re
import secrets

from .yolo_worker import NodeRuntime, StartupBarrier, assigned_roles
from .yolo_profile import application_sync_prefix


class OperatorError(ValueError):
    pass


_DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")
_MODES = {"local-cpu", "single-node-gpu", "two-node-gpu"}


def _directory(value, code: str) -> Path:
    if not isinstance(value, (str, Path)):
        raise OperatorError(code)
    path = Path(value)
    if (not path.is_absolute() or ".." in path.parts
            or any(p.is_symlink() for p in (path, *path.parents))
            or not path.is_dir()):
        raise OperatorError(code)
    return path


def _digest(value, code: str) -> str:
    if not isinstance(value, str) or _DIGEST.fullmatch(value) is None:
        raise OperatorError(code)
    return value


def _finite(value, code: str, *, minimum: float = 0.0, maximum: float = 3600.0) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise OperatorError(code)
    if not minimum < value <= maximum:
        raise OperatorError(code)
    return float(value)


def _validate_plan(plan: dict, *, mode: str, rank: int) -> tuple[tuple[str, ...], tuple[int, ...]]:
    if not isinstance(plan, dict) or plan.get("schema") != "tiger-yolo-run-plan-v1":
        raise OperatorError("OPERATOR_PLAN")
    if mode not in _MODES or plan.get("case") != mode:
        raise OperatorError("OPERATOR_MODE")
    if type(rank) is not int or rank < 0:
        raise OperatorError("OPERATOR_RANK")
    nodes = plan.get("nodes")
    if not isinstance(nodes, list) or len(nodes) != (1 if mode != "two-node-gpu" else 2):
        raise OperatorError("OPERATOR_NODES")
    matching = [row for row in nodes if isinstance(row, dict) and row.get("rank") == rank]
    if len(matching) != 1 or set(matching[0].get("roles", ())) != set(assigned_roles(mode, rank)):
        raise OperatorError("OPERATOR_ROLE_LAYOUT")
    ranks = tuple(sorted(row.get("rank") for row in nodes))
    if ranks not in ((0,), (0, 1)):
        raise OperatorError("OPERATOR_RANK_LAYOUT")
    if not isinstance(plan.get("runId"), str) or re.fullmatch(r"[a-z][a-z0-9-]{1,47}", plan["runId"]) is None:
        raise OperatorError("OPERATOR_RUN_ID")
    try:
        application_sync_prefix(plan.get("applicationName"))
    except (TypeError, ValueError):
        raise OperatorError("OPERATOR_APPLICATION_NAME")
    return tuple(matching[0]["roles"]), ranks


def _validate_endpoints(endpoints: list[dict], ranks: tuple[int, ...]) -> None:
    if not isinstance(endpoints, list) or len(endpoints) != len(ranks):
        raise OperatorError("OPERATOR_ENDPOINTS")
    seen = set()
    for row in endpoints:
        if (not isinstance(row, dict) or set(row) != {"rank", "address", "port"}
                or type(row["rank"]) is not int or row["rank"] not in ranks
                or row["rank"] in seen or not isinstance(row["address"], str)
                or not row["address"] or any(c.isspace() for c in row["address"])
                or type(row["port"]) is not int or not 1024 <= row["port"] <= 65535):
            raise OperatorError("OPERATOR_ENDPOINTS")
        seen.add(row["rank"])
    if seen != set(ranks):
        raise OperatorError("OPERATOR_ENDPOINTS")


def _validate_options(startup_options: dict, request_options: dict) -> None:
    if not isinstance(startup_options, dict) or set(startup_options) != {
            "repo_free_bytes", "permission_wait_ms", "network_probe_seconds"}:
        raise OperatorError("OPERATOR_STARTUP_OPTIONS")
    if type(startup_options["repo_free_bytes"]) is not int or not 0 < startup_options["repo_free_bytes"] <= 2**63 - 1:
        raise OperatorError("OPERATOR_REPO_CAPACITY")
    if type(startup_options["permission_wait_ms"]) is not int or not 1 <= startup_options["permission_wait_ms"] <= 120000:
        raise OperatorError("OPERATOR_PERMISSION_BUDGET")
    _finite(startup_options["network_probe_seconds"], "OPERATOR_NETWORK_BUDGET", maximum=120)
    expected = {"package", "catalog_data_name", "catalog_signer", "permission_wait_ms",
                "request_deadline_ms", "process_timeout_seconds", "protection_epoch"}
    if not isinstance(request_options, dict) or set(request_options) != expected:
        raise OperatorError("OPERATOR_REQUEST_OPTIONS")
    package = _directory(request_options["package"], "OPERATOR_PACKAGE")
    if package.is_symlink():
        raise OperatorError("OPERATOR_PACKAGE")
    for key in ("catalog_data_name", "catalog_signer"):
        value = request_options[key]
        if (not isinstance(value, str) or not value.startswith("/") or value == "/"
                or any(ord(c) < 33 or ord(c) == 127 for c in value)):
            raise OperatorError("OPERATOR_CATALOG")
    if type(request_options["permission_wait_ms"]) is not int or not 1 <= request_options["permission_wait_ms"] <= 120000:
        raise OperatorError("OPERATOR_PERMISSION_BUDGET")
    if type(request_options["request_deadline_ms"]) is not int or not 1501 <= request_options["request_deadline_ms"] <= 60000:
        raise OperatorError("OPERATOR_REQUEST_DEADLINE")
    _finite(request_options["process_timeout_seconds"], "OPERATOR_PROCESS_BUDGET", maximum=3600)
    epoch = request_options["protection_epoch"]
    if (not isinstance(epoch, str) or epoch == "plaintext-v1"
            or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", epoch) is None):
        raise OperatorError("OPERATOR_PROTECTED_EPOCH")


def run_rank(*, plan: dict, profile: dict, mode: str, rank: int, bundle: Path,
             public: Path, homes: dict[str, Path], output: Path, node: Path,
             startup_directory: Path, completion_directory: Path,
             preparation_digest: str, candidate_digest: str, endpoints: list[dict],
             startup_seconds: float, completion_seconds: float,
             startup_options: dict, request_options: dict, accept_request,
             allocation_expected: dict | None = None, probe_id: str | None = None,
             gpu_device: str | None = None) -> dict:
    """Run one rank through the maintained application lifecycle.

    The function is deliberately explicit about every boundary input.  It is
    suitable for a local CPU rank and for one `srun` task; the caller decides
    how two ranks are launched and how the final collector joins their receipts.
    """
    roles, ranks = _validate_plan(plan, mode=mode, rank=rank)
    _digest(preparation_digest, "OPERATOR_PREPARATION_DIGEST")
    _digest(candidate_digest, "OPERATOR_CANDIDATE_DIGEST")
    bundle = _directory(bundle, "OPERATOR_BUNDLE")
    public = _directory(public, "OPERATOR_PUBLIC")
    output = _directory(output, "OPERATOR_OUTPUT")
    node = _directory(node, "OPERATOR_NODE")
    startup_directory = _directory(startup_directory, "OPERATOR_STARTUP_DIRECTORY")
    completion_directory = _directory(completion_directory, "OPERATOR_COMPLETION_DIRECTORY")
    if startup_directory == completion_directory:
        raise OperatorError("OPERATOR_BARRIER_ALIAS")
    if not isinstance(profile, dict):
        raise OperatorError("OPERATOR_PROFILE")
    if not callable(accept_request):
        raise OperatorError("OPERATOR_COLLECTOR")
    _validate_endpoints(endpoints, ranks)
    _validate_options(startup_options, request_options)
    if mode == "local-cpu":
        if gpu_device is not None or allocation_expected is not None:
            raise OperatorError("OPERATOR_CPU_ALLOCATION")
    else:
        if not isinstance(allocation_expected, dict):
            raise OperatorError("OPERATOR_GPU_ALLOCATION")
        if gpu_device is None or not isinstance(gpu_device, str) or not gpu_device:
            raise OperatorError("OPERATOR_GPU_SELECTOR")
    if probe_id is None:
        probe_id = secrets.token_hex(16)
    if not isinstance(probe_id, str) or re.fullmatch(r"[a-f0-9]{32}", probe_id) is None:
        raise OperatorError("OPERATOR_PROBE_ID")
    try:
        worker = NodeRuntime.from_preparation(
            plan, expected_receipt_digest=preparation_digest,
            candidate_digest=candidate_digest, profile=profile, mode=mode, rank=rank,
            bundle=bundle, homes=homes, public=public, output=output, node=node,
            gpu_device=gpu_device, cleanup_seconds=float(profile["timing"]["cleanupSeconds"]))
    except (KeyError, TypeError, ValueError) as exc:
        raise OperatorError("OPERATOR_PREPARATION") from exc
    startup = StartupBarrier(startup_directory, run_id=plan["runId"], probe_id=probe_id,
        candidate_digest=candidate_digest, ranks=ranks, rank=rank,
        seconds=_finite(startup_seconds, "OPERATOR_STARTUP_BUDGET"), check=worker.check)

    def completion_factory():
        return StartupBarrier(completion_directory, run_id=plan["runId"], probe_id=probe_id,
            candidate_digest=candidate_digest, ranks=ranks, rank=rank,
            seconds=_finite(completion_seconds, "OPERATOR_COMPLETION_BUDGET"), check=worker.check)

    from apps.yolo import run_normal_node
    return run_normal_node(worker, startup, completion_factory=completion_factory,
        endpoints=endpoints, startup_options=startup_options,
        request_options=request_options, accept_request=accept_request,
        allocation_expected=allocation_expected)


def finalize_normal_collection(*, plan: dict, rank_results: dict, node_roots: dict,
                               collection_path: Path, references: list[dict],
                               runtime_candidate_digest: str,
                               placement_candidate_id: str,
                               placement_candidate_digest: str, graph_digest: str,
                               catalogue_digest: str, providers_by_role: dict,
                               certified_graph: dict,
                               allocation_expected: dict | None = None) -> dict:
    """Join completed rank returns and publish the collector handoff.

    This is the outer coordinator boundary for a future ``srun`` owner.  It
    runs only after every expected rank has returned a clean worker receipt;
    the handoff writer then re-reads each retained ``node-receipt.json`` and
    binds its bytes and preparation digest.  No result or verdict is inferred
    from a rank exit code or from an incomplete rank map.
    """
    if not isinstance(plan, dict) or plan.get("case") not in {
            "local-cpu", "single-node-gpu", "two-node-gpu"}:
        raise OperatorError("OPERATOR_COLLECTION_CASE")
    expected = {0, 1} if plan["case"] == "two-node-gpu" else {0}
    if (not isinstance(rank_results, dict) or set(rank_results) != expected
            or any(type(rank) is not int for rank in rank_results)):
        raise OperatorError("OPERATOR_COLLECTION_RANKS")
    for rank, result in rank_results.items():
        if (not isinstance(result, dict) or result.get("rank") != rank
                or result.get("runId") != plan.get("runId")
                or result.get("case") != plan.get("case")
                or result.get("qualification") != "NODE_CLEANUP_COMPONENT_ONLY"):
            raise OperatorError("OPERATOR_COLLECTION_RECEIPT")
    from .yolo_collection import publish_normal_handoff
    return publish_normal_handoff(
        collection_path, plan=plan, node_roots=node_roots, references=references,
        runtime_candidate_digest=runtime_candidate_digest,
        placement_candidate_id=placement_candidate_id,
        placement_candidate_digest=placement_candidate_digest,
        graph_digest=graph_digest, catalogue_digest=catalogue_digest,
        providers_by_role=providers_by_role, certified_graph=certified_graph,
        allocation_expected=allocation_expected)


def descriptor_digest(descriptor: dict) -> str:
    """Return a stable digest for an operator descriptor without reading paths."""
    if not isinstance(descriptor, dict):
        raise OperatorError("OPERATOR_DESCRIPTOR")
    return "sha256:" + __import__("hashlib").sha256(json.dumps(
        descriptor, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()
