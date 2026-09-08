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
import subprocess
import time

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


def stage_provision_inputs(resolved: dict, destination: Path) -> str:
    """Stage only small issuer inputs, including a private 0600 key copy.

    This is not the public byte-freezing prepare command. Call only at the
    qualified local/allocation preparation boundary; never publish this private
    input directory as part of the public harness or a result bundle.
    """
    import hashlib
    from .identities import _read_credential, _create_credential, _credential_document
    destination = Path(destination)
    if (not destination.is_absolute() or ".." in destination.parts
            or any(p.is_symlink() for p in (destination, *destination.parents))
            or destination.exists() or not destination.parent.is_dir()):
        raise OperatorError("OPERATOR_INPUT_DESTINATION")
    rows = resolved["publicInputs"]
    required = {"template.json", "trust/contracts/trust-root-registry-v1.json"}
    if not isinstance(rows, dict) or not required.issubset(rows):
        raise OperatorError("OPERATOR_INPUT_INVENTORY")
    payloads = {}
    for name, row in rows.items():
        if name not in required and re.fullmatch(r"trust/contracts/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.pub", name) is None:
            raise OperatorError("OPERATOR_INPUT_PATH")
        payload = _read_credential(Path(row["path"]))
        if len(payload) != row["bytes"] or "sha256:" + hashlib.sha256(payload).hexdigest() != row["sha256"]:
            raise OperatorError("OPERATOR_INPUT_CHANGED")
        payloads[name] = payload
    secret = _read_credential(Path(resolved["authorityPrivateKey"]), private=True)
    destination.mkdir(mode=0o700, exist_ok=False)
    for name, payload in payloads.items():
        target = destination / name
        target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        _create_credential(target, payload)
    (destination / "private").mkdir(mode=0o700)
    _create_credential(destination / "private/artifact-policy-authority.key", secret)
    descriptor = destination / "prepare.json"
    _credential_document(descriptor, resolved["descriptor"])
    descriptor_digest = "sha256:" + hashlib.sha256(_read_credential(descriptor)).hexdigest()
    from apps.yolo import preparation_arguments
    preparation_arguments(descriptor, descriptor_digest)
    return descriptor_digest


def provision_run(*, runtime_profile: dict, bundle: Path, harness_digest: str,
                  inputs: Path, descriptor_digest: str, package: Path,
                  public: Path, private: Path, output: Path,
                  seconds: float, cleanup_seconds: float) -> dict:
    """Invoke the installed offline issuer and pin its actual public receipt.

    The caller must qualify the runtime before calling this internal boundary.
    Hash checks here bind execution bytes, not GPU/model qualification. Inputs
    contain prepare.json, template.json, trust/contracts and private/; only this
    offline container receives the complete issuer directory. Partial output
    and logs are retained, so retry requires a new preparation directory.
    """
    from apps.yolo import preparation_arguments, validate_preparation_roots
    from .baseline import PYTHON, Processes, container_command, container_env, digest, write_json
    from .yolo_bundle import _bytes, verify_harness, verify_preparation

    seconds = _finite(seconds, "OPERATOR_PREPARATION_BUDGET")
    deadline = time.monotonic() + seconds
    cleanup_seconds = _finite(cleanup_seconds, "OPERATOR_CLEANUP_BUDGET")
    bundle = _directory(bundle, "OPERATOR_BUNDLE")
    inputs = _directory(inputs, "OPERATOR_PREPARATION_INPUTS")
    package = _directory(package, "OPERATOR_PACKAGE")
    public = _directory(public, "OPERATOR_PUBLIC")
    private = _directory(private, "OPERATOR_PRIVATE")
    output = _directory(output, "OPERATOR_OUTPUT")
    roots = (bundle, inputs, package, public, private, output)
    if any(a == b or a in b.parents or b in a.parents
           for index, a in enumerate(roots) for b in roots[index + 1:]):
        raise OperatorError("OPERATOR_PREPARATION_OVERLAP")
    validate_preparation_roots(public, private)
    if any(output.iterdir()):
        raise OperatorError("OPERATOR_PREPARATION_OUTPUT_NOT_EMPTY")
    verify_harness(bundle, expected_manifest_sha256=harness_digest)
    options = preparation_arguments(inputs / "prepare.json", descriptor_digest)
    from .identities import _read_credential
    _read_credential(inputs / "private/artifact-policy-authority.key", private=True)
    # Validate the public inputs before creating any child. The issuer remains
    # responsible for the private/public key match and signed catalogue checks.
    for path, expected in ((inputs / "template.json", options["template_digest"]),
                           (inputs / "trust/contracts/trust-root-registry-v1.json", options["registry_digest"]),
                           (package / "manifest.json", options["manifest_digest"])):
        if any(p.is_symlink() for p in (path, *path.parents)) or not path.is_file():
            raise OperatorError("OPERATOR_PREPARATION_INPUT_FILE")
        if "sha256:" + digest(path) != expected:
            raise OperatorError("OPERATOR_PREPARATION_INPUT_DIGEST")
    sif = Path(runtime_profile["sif"])
    if (not sif.is_absolute() or not sif.is_file()
            or any(p.is_symlink() for p in (sif, *sif.parents))
            or digest(sif) != runtime_profile["sifSha256"]):
        raise OperatorError("OPERATOR_PREPARATION_SIF_DIGEST")
    command = container_command(runtime_profile, bundle, private / "root", public,
        output, [PYTHON, "-m", "apps.yolo", "prepare", "--descriptor", "/inputs/prepare.json",
                 "--descriptor-sha256", descriptor_digest],
        prepare=private, artifacts=package, preparation_inputs=inputs)
    children = Processes(output / "logs")
    if time.monotonic() >= deadline:
        raise OperatorError('OPERATOR_PREPARATION_TIMEOUT')
    try:
        child = children.start("prepare", command, env=container_env(), cwd=bundle)
        try:
            code = child.wait(timeout=max(0.001, deadline-time.monotonic()))
        except subprocess.TimeoutExpired as exc:
            raise OperatorError("OPERATOR_PREPARATION_TIMEOUT") from exc
        if code != 0:
            raise OperatorError("OPERATOR_PREPARATION_EXIT:" + str(code))
    finally:
        cleanup = children.close(seconds=cleanup_seconds)
        write_json(output / "cleanup.json", {"records": cleanup})
    if any(not row.get("reaped") or row.get("forced") or row.get("cleanupError") for row in cleanup):
        raise OperatorError("OPERATOR_PREPARATION_CLEANUP")
    if time.monotonic() >= deadline:
        raise OperatorError('OPERATOR_PREPARATION_TIMEOUT')
    import hashlib
    receipt_digest = "sha256:" + hashlib.sha256(_bytes(public / "preparation.json")).hexdigest()
    receipt = verify_preparation(public, options["plan"], expected_receipt_digest=receipt_digest,
                                 candidate_digest=options["runtime_candidate_digest"])
    expected = {"templateDigest": options["template_digest"],
                "packageManifestDigest": options["manifest_digest"],
                "registryDigest": options["registry_digest"],
                "protectionEpoch": options["protection_epoch"],
                "placementCandidateId": options["placement_candidate_id"],
                "placementCandidateDigest": options["placement_candidate_digest"]}
    if any(receipt.get(key) != value for key, value in expected.items()):
        raise OperatorError("OPERATOR_PREPARATION_RECEIPT_INPUTS")
    return {"status": "PREPARED", "qualification": "NOT_EVALUATED",
            "receiptDigest": receipt_digest, "preparation": receipt}


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
    Multi-rank callers must generate one probe_id per invocation and pass it
    unchanged to every rank; independently generated IDs cannot share barriers.
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
    startup_seconds = _finite(startup_seconds, "OPERATOR_STARTUP_BUDGET")
    completion_seconds = _finite(completion_seconds, "OPERATOR_COMPLETION_BUDGET")
    timing = profile.get("timing")
    if not isinstance(timing, dict):
        raise OperatorError("OPERATOR_CLEANUP_BUDGET")
    cleanup_seconds = _finite(timing.get("cleanupSeconds"), "OPERATOR_CLEANUP_BUDGET")
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
        if len(ranks) > 1:
            raise OperatorError("OPERATOR_SHARED_PROBE_REQUIRED")
        probe_id = secrets.token_hex(16)
    if not isinstance(probe_id, str) or re.fullmatch(r"[a-f0-9]{32}", probe_id) is None:
        raise OperatorError("OPERATOR_PROBE_ID")
    try:
        worker = NodeRuntime.from_preparation(
            plan, expected_receipt_digest=preparation_digest,
            candidate_digest=candidate_digest, profile=profile, mode=mode, rank=rank,
            bundle=bundle, homes=homes, public=public, output=output, node=node,
            gpu_device=gpu_device, cleanup_seconds=cleanup_seconds)
    except (KeyError, TypeError, ValueError) as exc:
        raise OperatorError("OPERATOR_PREPARATION") from exc
    startup = StartupBarrier(startup_directory, run_id=plan["runId"], probe_id=probe_id,
        candidate_digest=candidate_digest, ranks=ranks, rank=rank,
        seconds=startup_seconds, check=worker.check)

    def completion_factory():
        return StartupBarrier(completion_directory, run_id=plan["runId"], probe_id=probe_id,
            candidate_digest=candidate_digest, ranks=ranks, rank=rank,
            seconds=completion_seconds, check=worker.check)

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


def execute_local_run(*, prepared: dict, profile: dict, resolved: dict) -> dict:
    return _execute_single_node(prepared=prepared, profile=profile, resolved=resolved,
                                mode='local-cpu')


def _normal_reference(prepared, profile, resolved):
    from .yolo_bundle import reference_owner
    from .yolo_profile import _file_identity
    package = _directory(resolved['package'], 'LOCAL_RUN_PACKAGE')
    owner = reference_owner(Path(prepared['bundle']))
    fixture = Path(profile['oracle']['input']['path'])
    repository = fixture
    for _ in Path(owner.FIXTURE_PATH).parts:
        repository = repository.parent
    if repository / owner.FIXTURE_PATH != fixture:
        raise OperatorError('LOCAL_RUN_FIXTURE_LAYOUT')
    for name in ('input', 'reference'):
        record = profile['oracle'][name]
        path = Path(record['path'])
        _file_identity(path.parent, name, dict(record, path=path.name))
    reference = owner.load_reference(package, repository, 640)
    if (reference.manifest_digest != resolved['descriptor']['manifestDigest']
            or reference.fixture_digest != profile['oracle']['input']['sha256']
            or reference.oracle_digest != profile['oracle']['reference']['sha256']):
        raise OperatorError('LOCAL_RUN_ORACLE_BINDING')
    return reference, package, repository


def _distributed_control(prepared, expected):
    from .yolo_profile import _read_plane
    root = Path(prepared['plan']['output'])
    value = _read_plane(root / 'distributed-control.json')
    if (not isinstance(value, dict) or set(value) != {'runId', 'candidateDigest', 'jobId', 'probeId'}
            or value['runId'] != prepared['runId'] or value['candidateDigest'] != prepared['candidateDigest']
            or value['jobId'] != expected['job_id'] or not isinstance(value['probeId'], str)
            or re.fullmatch(r'[a-f0-9]{32}', value['probeId']) is None):
        raise OperatorError('DISTRIBUTED_CONTROL_BINDING')
    return value


def _allocation_endpoints(hosts, port, seconds):
    """Resolve only scheduler-attested hostnames within one bounded query budget."""
    import ipaddress
    import time
    deadline = time.monotonic() + seconds
    result = []
    for rank, host in enumerate(hosts):
        if not isinstance(host, str) or re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.-]{0,252}', host) is None:
            raise OperatorError('DISTRIBUTED_HOSTNAME')
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise OperatorError('DISTRIBUTED_DNS_TIMEOUT')
        query = subprocess.run(['/usr/bin/getent', 'ahostsv4', host], check=True,
            capture_output=True, timeout=remaining, env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
        if len(query.stdout) > 16384:
            raise OperatorError('DISTRIBUTED_DNS_SIZE')
        addresses = {str(ipaddress.IPv4Address(line.split()[0]))
                     for line in query.stdout.decode('ascii').splitlines() if line.strip()}
        if len(addresses) != 1:
            raise OperatorError('DISTRIBUTED_DNS_AMBIGUOUS')
        address = addresses.pop()
        ip = ipaddress.IPv4Address(address)
        if ip.is_loopback or ip.is_unspecified or ip.is_multicast:
            raise OperatorError('DISTRIBUTED_DNS_ADDRESS')
        result.append(dict(rank=rank, address=address, port=port))
    if len(result) != 2 or result[0]['address'] == result[1]['address']:
        raise OperatorError('DISTRIBUTED_DISTINCT_NODES')
    return result


def execute_distributed_rank(*, prepared, profile, resolved, allocation_expected, rank):
    """One actual srun rank; rank zero signs once and both own their node lifecycle."""
    import time
    from .yolo_allocation import capture_task_allocation
    from .yolo_bundle import verify_harness, verify_preparation
    from .yolo_profile import _read_plane
    from .identities import _credential_document
    from .yolo_graph_reference import read_request_reference
    from .yolo_result import collect_request_result
    plan = prepared['plan']
    _validate_plan(plan, mode='two-node-gpu', rank=rank)
    root = _directory(plan['output'], 'DISTRIBUTED_ROOT')
    bundle = _directory(prepared['bundle'], 'DISTRIBUTED_BUNDLE')
    control = _distributed_control(prepared, allocation_expected)
    verify_harness(bundle, expected_manifest_sha256=prepared['harnessManifestSha256'])
    if (resolved['descriptor']['plan'] != plan or prepared['case'] != 'two-node-gpu'
            or resolved['descriptor']['runtimeCandidateDigest'] != prepared['candidateDigest']):
        raise OperatorError('DISTRIBUTED_PREPARED_BINDING')
    timing = profile['timing']
    permission_ms = min(120000, timing['progressTimeoutSeconds'] * 1000)
    process_seconds = (permission_ms + timing['requestDeadlineMs']) / 1000
    completion_seconds = len(plan['requests']) * process_seconds
    if timing['stagingSeconds'] + timing['startupSeconds'] + completion_seconds + timing['cleanupSeconds'] > profile['cluster']['wallTimeSeconds']:
        raise OperatorError('DISTRIBUTED_WALLTIME_BUDGET')
    observed = capture_task_allocation(**allocation_expected, rank=rank, node_count=2,
                                      seconds=timing['progressTimeoutSeconds'])
    endpoints = _allocation_endpoints(observed['receipt']['hosts'], profile['cluster']['tcpPort'],
                                     timing['progressTimeoutSeconds'])
    from .yolo_storage import NodeScratch
    with NodeScratch(prepared=prepared, profile=profile, runtime_profile=resolved['runtimeProfile'],
                     allocation=observed, rank=rank) as storage:
        return _execute_distributed_workload(prepared=prepared, profile=profile,
            resolved=dict(resolved, runtimeProfile=storage.runtime_profile), allocation_expected=allocation_expected,
            rank=rank, observed=observed, endpoints=endpoints, control=control, node=storage.node,
            staging_deadline=storage.deadline)


def _execute_distributed_workload(*, prepared, profile, resolved, allocation_expected, rank,
                                  observed, endpoints, control, node, staging_deadline):
    from .yolo_bundle import verify_preparation
    from .yolo_profile import _read_plane
    from .identities import _credential_document
    from .yolo_graph_reference import read_request_reference
    from .yolo_result import collect_request_result
    from .yolo_storage import measured_capacity
    plan, root, bundle = prepared['plan'], Path(prepared['plan']['output']), Path(prepared['bundle'])
    timing = profile['timing']
    permission_ms = min(120000, timing['progressTimeoutSeconds']*1000)
    process_seconds = (permission_ms+timing['requestDeadlineMs'])/1000
    completion_seconds = len(plan['requests'])*process_seconds
    provision_path = root / 'distributed-preparation.json'
    try:
        if rank == 0:
            _credential_document(root / 'distributed-started.json', control)
            descriptor = stage_provision_inputs(resolved, root / 'issuer-inputs')
            for name in ('public', 'private', 'prepare-output', 'startup', 'completion'):
                (root / name).mkdir(mode=0o700)
            (root / 'private/root').mkdir(mode=0o700)
            provision = provision_run(runtime_profile=resolved['runtimeProfile'], bundle=bundle,
                harness_digest=prepared['harnessManifestSha256'], inputs=root/'issuer-inputs',
                descriptor_digest=descriptor, package=Path(resolved['package']), public=root/'public',
                private=root/'private', output=root/'prepare-output', seconds=staging_deadline-time.monotonic(),
                cleanup_seconds=timing['cleanupSeconds'])
            _credential_document(provision_path, dict(control=control, provision=provision))
        else:
            deadline = staging_deadline + timing['cleanupSeconds']
            while True:
                if (root/'failed-0.json').exists():
                    raise OperatorError('DISTRIBUTED_PREPARATION_PEER_FAILED')
                if provision_path.exists():
                    break
                if time.monotonic() >= deadline:
                    raise OperatorError('DISTRIBUTED_PREPARATION_TIMEOUT')
                time.sleep(0.05)
        stored = _read_plane(provision_path)
        if not isinstance(stored, dict) or set(stored) != {'control', 'provision'} or stored['control'] != control:
            raise OperatorError('DISTRIBUTED_PREPARATION_BINDING')
        provision = stored['provision']
        receipt = verify_preparation(root/'public', plan, expected_receipt_digest=provision['receiptDigest'],
                                     candidate_digest=prepared['candidateDigest'])
        if receipt != provision['preparation']:
            raise OperatorError('DISTRIBUTED_PREPARATION_CHANGED')
        reference = _normal_reference(prepared, profile, resolved)[0] if rank == 0 else None
        output = root / ('node'+str(rank))
        output.mkdir(mode=0o700)
        accepted = []
        def accept(request, request_output):
            if rank != 0:
                raise OperatorError('DISTRIBUTED_USER_RANK')
            read_request_reference(request_output/'graph-reference.json', run_id=plan['runId'],
                request_id=request['requestId'], runtime_candidate_digest=prepared['candidateDigest'],
                placement_candidate_digest=receipt['placementCandidateDigest'], graph_digest=receipt['graphDigest'])
            collect_request_result(request_output, reference, case='two-node-gpu', request_id=request['requestId'],
                attempt_id='attempt-1', candidate_id=receipt['placementCandidateId'],
                candidate_digest=receipt['placementCandidateDigest'], graph_digest=receipt['graphDigest'],
                catalogue_digest=receipt['catalogueDigest'])
            accepted.append(request['index'])
        result = run_rank(plan=plan, profile=dict(profile, **resolved['runtimeProfile']), mode='two-node-gpu',
            rank=rank, bundle=bundle, public=root/'public',
            homes={role: root/'private'/role for role in assigned_roles('two-node-gpu', rank)},
            output=output, node=node, startup_directory=root/'startup', completion_directory=root/'completion',
            preparation_digest=provision['receiptDigest'], candidate_digest=prepared['candidateDigest'],
            endpoints=endpoints, startup_seconds=timing['startupSeconds'], completion_seconds=completion_seconds,
            startup_options=dict(repo_free_bytes=measured_capacity(root,
                profile['storage']['peakBytes']+profile['storage']['marginBytes'])['freeBytes'],
                permission_wait_ms=permission_ms, network_probe_seconds=timing['progressTimeoutSeconds']),
            request_options=dict(package=Path(resolved['package']), catalog_data_name=receipt['catalogueDataName'],
                catalog_signer=receipt['catalogueSigner'], permission_wait_ms=permission_ms,
                request_deadline_ms=timing['requestDeadlineMs'], process_timeout_seconds=process_seconds,
                protection_epoch=profile['security']['protectionEpoch']), accept_request=accept,
            allocation_expected=allocation_expected, probe_id=control['probeId'],
            gpu_device=observed['receipt']['visible'])
        if accepted != (list(range(len(plan['requests']))) if rank == 0 else []):
            raise OperatorError('DISTRIBUTED_REQUEST_COVERAGE')
        return result
    except BaseException as exc:
        _credential_document(root/('failed-'+str(rank)+'.json'), dict(control, errorType=type(exc).__name__))
        raise


def finalize_distributed_run(*, prepared, profile, resolved, allocation_expected):
    """After srun exits, join both retained worker receipts and actual preparation."""
    from .yolo_profile import _read_plane
    from .yolo_bundle import verify_preparation
    root, plan = Path(prepared['plan']['output']), prepared['plan']
    control = _distributed_control(prepared, allocation_expected)
    value = _read_plane(root/'distributed-preparation.json')
    if value.get('control') != control:
        raise OperatorError('DISTRIBUTED_PREPARATION_BINDING')
    receipt = verify_preparation(root/'public', plan, expected_receipt_digest=value['provision']['receiptDigest'],
                                 candidate_digest=prepared['candidateDigest'])
    _, package, repository = _normal_reference(prepared, profile, resolved)
    return finalize_normal_collection(plan=plan,
        rank_results={rank: _read_plane(root/('node'+str(rank))/'node-receipt.json') for rank in (0, 1)},
        node_roots={rank: root/('node'+str(rank)) for rank in (0, 1)}, collection_path=root/'collection-input.json',
        references=[dict(package=package, repository=repository, inputSize=640) for _ in plan['requests']],
        runtime_candidate_digest=prepared['candidateDigest'],
        providers_by_role={role: plan['identities'][role] for role in ('BackboneNeck','DetectShard0','DetectShard1','Merge')},
        placement_candidate_id=receipt['placementCandidateId'], placement_candidate_digest=receipt['placementCandidateDigest'],
        graph_digest=receipt['graphDigest'], catalogue_digest=receipt['catalogueDigest'],
        certified_graph={'graphDigest': receipt['graphDigest']}, allocation_expected=allocation_expected)


def execute_single_gpu_run(*, prepared: dict, profile: dict, resolved: dict,
                           allocation_expected: dict) -> dict:
    """Run a normal GPU case inside the journal-bound single srun task."""
    from .yolo_allocation import capture_task_allocation
    observed = capture_task_allocation(**allocation_expected, rank=0, node_count=1,
        seconds=profile['timing']['progressTimeoutSeconds'])
    # Query before any issuer/native process, then let NodeRuntime retain and
    # revalidate the allocation at the provider launch boundary as usual.
    from .yolo_storage import NodeScratch
    with NodeScratch(prepared=prepared, profile=profile, runtime_profile=resolved['runtimeProfile'],
                     allocation=observed, rank=0) as storage:
        return _execute_single_node(prepared=prepared, profile=profile,
            resolved=dict(resolved, runtimeProfile=storage.runtime_profile), mode='single-node-gpu',
            allocation_expected=allocation_expected, gpu_device=observed['receipt']['visible'],
            node_override=storage.node, staging_deadline=storage.deadline)


def _execute_single_node(*, prepared: dict, profile: dict, resolved: dict, mode: str,
                         allocation_expected=None, gpu_device=None, node_override=None,
                         staging_deadline=None) -> dict:
    """Execute one complete normal single-node case through existing owners.

    The public CLI must validate the relevant prior gate before entering
    this internal boundary. This function produces retained collection input,
    never a PASS from process return codes. Partial output is not reusable.
    """
    from .yolo_bundle import reference_owner, verify_harness
    from .yolo_profile import _file_identity
    from .yolo_result import collect_request_result
    from .yolo_graph_reference import read_request_reference
    from .identities import _credential_document
    from .yolo_storage import measured_capacity
    plan = prepared['plan']
    _validate_plan(plan, mode=mode, rank=0)
    root = _directory(plan['output'], 'LOCAL_RUN_ROOT')
    bundle = _directory(prepared['bundle'], 'LOCAL_RUN_BUNDLE')
    verify_harness(bundle, expected_manifest_sha256=prepared['harnessManifestSha256'])
    if (mode not in ('local-cpu', 'single-node-gpu') or prepared['case'] != mode
            or prepared['runId'] != plan['runId']
            or resolved['descriptor']['plan'] != plan
            or resolved['descriptor']['runtimeCandidateDigest'] != prepared['candidateDigest']):
        raise OperatorError('LOCAL_RUN_BINDING')
    timing = profile['timing']
    permission_ms = min(120000, timing['progressTimeoutSeconds'] * 1000)
    process_seconds = (permission_ms + timing['requestDeadlineMs']) / 1000
    completion_seconds = len(plan['requests']) * process_seconds
    if (timing['stagingSeconds'] + timing['startupSeconds'] + completion_seconds
            + timing['cleanupSeconds'] > profile['cluster']['wallTimeSeconds']):
        raise OperatorError('LOCAL_RUN_WALLTIME_BUDGET')
    reference, package, repository = _normal_reference(prepared, profile, resolved)
    names = ('issuer-inputs', 'public', 'private', 'prepare-output', 'node',
             'startup', 'completion', 'node0')
    paths = {name: root / name for name in names}
    if node_override is not None:
        names = tuple(name for name in names if name != 'node')
        paths.pop('node')
    if any(path.exists() or path.is_symlink() for path in paths.values()):
        raise OperatorError('LOCAL_RUN_ALREADY_STARTED')
    if node_override is not None:
        paths['node'] = _directory(node_override, 'LOCAL_NODE_SCRATCH')
    started = dict(schema='tiger-yolo-local-execution-v1', status='STARTED', case=mode,
        runId=plan['runId'], candidateDigest=prepared['candidateDigest'])
    _credential_document(root / 'local-execution.json', started)
    try:
        descriptor = stage_provision_inputs(resolved, paths['issuer-inputs'])
        for name in names[1:]:
            paths[name].mkdir(mode=0o700)
        (paths['private'] / 'root').mkdir(mode=0o700)
        provision = provision_run(runtime_profile=resolved['runtimeProfile'], bundle=bundle,
            harness_digest=prepared['harnessManifestSha256'], inputs=paths['issuer-inputs'],
            descriptor_digest=descriptor, package=package, public=paths['public'],
            private=paths['private'], output=paths['prepare-output'],
            seconds=(timing['stagingSeconds'] if staging_deadline is None else staging_deadline-time.monotonic()),
            cleanup_seconds=timing['cleanupSeconds'])
        receipt = provision['preparation']
        graph_digest = _digest(receipt.get('graphDigest'), 'LOCAL_RUN_GRAPH')
        catalogue_digest = _digest(receipt.get('catalogueDigest'), 'LOCAL_RUN_CATALOGUE')
        providers = {role: plan['identities'][role] for role in
                     ('BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge')}
        accepted = []

        def accept(request, output):
            read_request_reference(output / 'graph-reference.json',
                run_id=plan['runId'], request_id=request['requestId'],
                runtime_candidate_digest=prepared['candidateDigest'],
                placement_candidate_digest=receipt['placementCandidateDigest'],
                graph_digest=graph_digest)
            collect_request_result(output, reference, case=mode,
                request_id=request['requestId'], attempt_id='attempt-1',
                candidate_id=receipt['placementCandidateId'],
                candidate_digest=receipt['placementCandidateDigest'],
                graph_digest=graph_digest, catalogue_digest=catalogue_digest)
            accepted.append(request['index'])

        merged = dict(profile, **resolved['runtimeProfile'])
        rank_result = run_rank(plan=plan, profile=merged, mode=mode, rank=0,
            bundle=bundle, public=paths['public'],
            homes={role: paths['private'] / role for role in plan['identities']},
            output=paths['node0'], node=paths['node'], startup_directory=paths['startup'],
            completion_directory=paths['completion'],
            preparation_digest=provision['receiptDigest'], candidate_digest=prepared['candidateDigest'],
            endpoints=[dict(rank=0, address='127.0.0.1', port=profile['cluster']['tcpPort'])],
            startup_seconds=timing['startupSeconds'], completion_seconds=completion_seconds,
            startup_options=dict(repo_free_bytes=measured_capacity(root,
                profile['storage']['peakBytes']+profile['storage']['marginBytes'])['freeBytes'],
                permission_wait_ms=permission_ms, network_probe_seconds=timing['progressTimeoutSeconds']),
            request_options=dict(package=package, catalog_data_name=receipt['catalogueDataName'],
                catalog_signer=receipt['catalogueSigner'], permission_wait_ms=permission_ms,
                request_deadline_ms=timing['requestDeadlineMs'], process_timeout_seconds=process_seconds,
                protection_epoch=profile['security']['protectionEpoch']), accept_request=accept,
            allocation_expected=allocation_expected, gpu_device=gpu_device)
        if accepted != list(range(len(plan['requests']))):
            raise OperatorError('LOCAL_RUN_REQUEST_COVERAGE')
        return finalize_normal_collection(plan=plan, rank_results={0: rank_result},
            node_roots={0: paths['node0']}, collection_path=root / 'collection-input.json',
            references=[dict(package=package, repository=repository, inputSize=640)
                        for _ in plan['requests']],
            runtime_candidate_digest=prepared['candidateDigest'], providers_by_role=providers,
            placement_candidate_id=receipt['placementCandidateId'],
            placement_candidate_digest=receipt['placementCandidateDigest'], graph_digest=graph_digest,
            catalogue_digest=catalogue_digest, certified_graph={'graphDigest': graph_digest},
            allocation_expected=allocation_expected)
    except BaseException as exc:
        _credential_document(root / 'local-execution-failure.json', dict(started,
            status='FAIL', errorType=type(exc).__name__))
        raise


def descriptor_digest(descriptor: dict) -> str:
    """Return a stable digest for an operator descriptor without reading paths."""
    if not isinstance(descriptor, dict):
        raise OperatorError("OPERATOR_DESCRIPTOR")
    return "sha256:" + __import__("hashlib").sha256(json.dumps(
        descriptor, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()
