#!/usr/bin/env python3
"""Publish or fetch the frozen Spec175 tiny-ONNX bundle through DistributedRepo."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import time

from ndnsf import ServiceUser
from py_repoclient import (
    ArtifactCapabilityRequirements,
    ArtifactHandle,
    ArtifactReference,
    ArtifactRepositoryApi,
    PersistentRepoClientConfig,
    RepoWriteConsistency,
    SelectedRepoSet,
    SelectedRepoTarget,
)

REPO_POLICY_EPOCH = "repo-policy-v1"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def digest_hex(value: object) -> str:
    text = str(value)
    return text[7:] if text.startswith("sha256:") else text


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("publish", "fetch"))
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--repo-provider", required=True)
    parser.add_argument("--repo-node", required=True)
    parser.add_argument("--repo-service-prefix", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--registration", required=True)
    parser.add_argument("--stage-manifest")
    parser.add_argument("--role")
    parser.add_argument("--destination")
    parser.add_argument("--timeout-ms", type=int, default=60_000)
    parser.add_argument(
        "--ack-timeout-ms", type=int, default=5_000,
        help=("ACK collection deadline for the repository control-plane "
              "collaboration; distinct from the streamed invocation deadline"),
    )
    parser.add_argument(
        "--initial-sync-settle-s", type=float, default=0.0,
        help=("Bounded cold-start interval after the publisher ServiceUser "
              "joins the SVS group and before its first route probe"),
    )
    parser.add_argument(
        "--publication-start-barrier-file",
        help=("Optional one-shot file barrier. The publisher initializes its "
              "NDNSF user first, then waits for the exact token before "
              "publishing the first artifact."),
    )
    parser.add_argument(
        "--publication-start-barrier-token",
        help="Exact token required in --publication-start-barrier-file",
    )
    parser.add_argument(
        "--publication-start-timeout-s", type=float, default=60.0,
        help="Bounded wait for the optional publication start barrier",
    )
    parser.add_argument(
        "--probe-output",
        help=("Evidence path for the same-identity, non-mutating repository "
              "route probe run before publication is released"),
    )
    parser.add_argument(
        "--probe-timeout-ms", type=int,
        help="Optional global timeout for the pre-publication route probe",
    )
    parser.add_argument(
        "--probe-attempt-timeout-ms", type=int,
        help="Per-attempt timeout for the bounded route-probe retry loop",
    )
    parser.add_argument(
        "--probe-retries", type=int, default=2,
        help="Number of fresh route-probe retries after the first attempt",
    )
    parser.add_argument(
        "--probe-retry-backoff-ms", type=int, default=250,
        help="Bounded delay between fresh route-probe attempts",
    )
    return parser


def wait_for_initial_sync(args: argparse.Namespace) -> None:
    """Hold the first publication until the new ServiceUser can learn peers."""
    settle_s = float(getattr(args, "initial_sync_settle_s", 0.0))
    if settle_s < 0.0 or settle_s > 60.0:
        raise ValueError("initial_sync_settle_s must be between 0 and 60")
    if settle_s == 0.0:
        return
    time.sleep(settle_s)
    print(
        "NDNSF_DI_REPO_USER_SVS_SETTLED",
        f"seconds={settle_s}",
        flush=True,
    )


def _write_json(path: Path, report: dict) -> None:
    path = path.expanduser().resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(
        f".{path.name}.tmp.{os.getpid()}.{time.time_ns()}")
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _run_live_service_probe_once(api, requirements, args: argparse.Namespace) -> dict:
    started_ms = time.time_ns() // 1_000_000
    report = {
        "schema": "ndnsf-di-spec175-repo-route-probe-v2",
        "status": "FAIL",
        "publisherIdentity": args.user,
        "route": {"service": args.repo_service_prefix.rstrip("/") + "/CAPABILITY"},
        "timing": {"sendAtMs": started_ms},
    }
    try:
        candidates = api.discover_repo_candidates(requirements, args.probe_timeout_ms)
        report["probeId"] = candidates.request_id
        report["observedCandidates"] = [
            {"repoNode": candidate.repo_node, "provider": candidate.provider_identity,
             "servicePrefix": candidate.service_prefix}
            for candidate in candidates.candidates
        ]
        matches = [candidate for candidate in candidates.candidates
                   if candidate.provider_identity == args.repo_provider
                   and candidate.repo_node == args.repo_node
                   and candidate.service_prefix == args.repo_service_prefix]
        if len(matches) != 1:
            raise RuntimeError("expected authenticated Repo capability was not discovered")
        candidate = matches[0]
        report["status"] = "PASS"
        report["route"].update(provider=candidate.provider_identity,
                               servicePrefix=candidate.service_prefix)
        report["capability"] = {"repoNode": candidate.repo_node}
        report["ack"] = {"wireDigest": candidate.ack_wire_digest,
                         "signerKeyLocator": candidate.signer_key_locator}
        report["timing"]["ackClosureAtMs"] = candidate.observed_at_ms
    except Exception as error:  # Convert bounded native failures to the probe record.
        report["failureReason"] = "REPO_SERVICE_ROUTE_NOT_READY"
        report["error"] = f"{type(error).__name__}: {error}"
    report["timing"]["elapsedMs"] = max(0, time.time_ns() // 1_000_000 - started_ms)
    return report


def run_live_service_probe(api, requirements, args: argparse.Namespace) -> dict:
    """Run a bounded sequence of fresh, non-mutating route probes.

    A single missing ACK can occur while a MiniNDN provider is finishing its
    native startup.  Retry with a fresh request ID, while preserving every
    attempt in the final evidence.  This does not turn a marker or a sleep
    into readiness: the publisher is released only after an actual native
    authenticated ACK closure succeeds.
    """
    retries = int(getattr(args, "probe_retries", 2))
    if retries < 0 or retries > 5:
        raise ValueError("probe_retries must be between 0 and 5")
    backoff_ms = int(getattr(args, "probe_retry_backoff_ms", 250))
    if backoff_ms < 0 or backoff_ms > 10_000:
        raise ValueError("probe_retry_backoff_ms must be between 0 and 10000")
    total_timeout_ms = int(
        getattr(args, "probe_timeout_ms", None)
        or max(int(getattr(args, "ack_timeout_ms", 5000)),
               int(getattr(args, "timeout_ms", 60000))))
    if total_timeout_ms <= 0:
        raise ValueError("probe timeout must be positive")
    attempt_timeout_ms = int(
        getattr(args, "probe_attempt_timeout_ms", None)
        or max(int(getattr(args, "ack_timeout_ms", 5000)), 10_000))
    if attempt_timeout_ms <= 0:
        raise ValueError("probe attempt timeout must be positive")

    deadline = time.monotonic() + total_timeout_ms / 1000.0
    attempts: list[dict] = []
    for attempt_index in range(retries + 1):
        remaining_ms = int(max(0.0, (deadline - time.monotonic()) * 1000.0))
        if remaining_ms <= 0:
            break
        attempt_args = argparse.Namespace(**vars(args))
        attempt_args.probe_timeout_ms = min(attempt_timeout_ms, remaining_ms)
        attempt = _run_live_service_probe_once(api, requirements, attempt_args)
        attempts.append(attempt)
        if attempt.get("status") == "PASS":
            break
        if attempt_index < retries:
            remaining_ms = int(max(0.0, (deadline - time.monotonic()) * 1000.0))
            if remaining_ms <= 0:
                break
            time.sleep(min(backoff_ms, remaining_ms) / 1000.0)

    if attempts:
        report = dict(attempts[-1])
    else:
        report = {
            "schema": "ndnsf-di-spec175-repo-route-probe-v2",
            "status": "FAIL",
            "failureReason": "REPO_SERVICE_ROUTE_NOT_READY",
            "error": "probe deadline expired before an attempt",
        }
    report["attemptCount"] = len(attempts)
    report["maxRetries"] = retries
    report["retryBackoffMs"] = backoff_ms
    report["attempts"] = attempts
    output = getattr(args, "probe_output", None)
    if output:
        _write_json(Path(output), report)
    return report


@contextmanager
def open_repo_api(args: argparse.Namespace):
    # The MiniNDN launcher installs the exact signed identity certificate
    # and its owned key into each node's private PIB/TPM before startup.
    user = ServiceUser(
        group=args.group, controller=args.controller, user=args.user,
        trust_schema=args.trust_schema, permission_wait_ms=20000,
    )
    config = PersistentRepoClientConfig()
    config.provider_scope = [args.repo_provider]
    config.service_prefix = args.repo_service_prefix
    config.ack_collection_time_ms = args.ack_timeout_ms
    config.request_timeout_ms = args.timeout_ms
    config.operation_wait_timeout_ms = args.timeout_ms
    try:
        user.start()
        yield ArtifactRepositoryApi.make_persistent(user, config)
    finally:
        user.stop()


def stage_reference(args, manifest, stage, path, *, format_version="artifact-manifest-v2"):
    digest = digest_hex(stage["sha256"])
    model_digest = digest_hex(manifest["modelDigest"])
    name = (f"{args.user.rstrip('/')}/NDNSF/Spec175/TinyOnnx/"
            f"{model_digest}/stage-{int(stage['stageIndex'])}/{digest}")
    root_manifest_name = "" if format_version == "raw-file-v1" else name + "/root"
    reference = ArtifactReference(
        name, digest, path.stat().st_size, root_manifest_name,
        args.user, REPO_POLICY_EPOCH, format_version=format_version,
    )
    reference.validate_producer_ownership()
    return reference


def wait_for_publication_start(args: argparse.Namespace) -> None:
    """Wait until the already-initialized publisher may issue its first request."""
    barrier_value = getattr(args, "publication_start_barrier_file", None)
    expected = str(
        getattr(args, "publication_start_barrier_token", None) or "")
    if not barrier_value and not expected:
        return
    if not barrier_value or not expected:
        raise ValueError(
            "publication start barrier file and token must be provided together")
    timeout_s = float(getattr(args, "publication_start_timeout_s", 60.0))
    if timeout_s <= 0:
        raise ValueError("publication start timeout must be positive")

    barrier = Path(str(barrier_value))
    print(
        "NDNSF_DI_SPEC175_REPO_PUBLISHER_WAITING",
        f"barrier={barrier}", flush=True)
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if barrier.is_file():
            actual = barrier.read_text(encoding="utf-8").strip()
            if actual != expected:
                raise RuntimeError(
                    "Spec175 Repo publication start barrier token mismatch")
            print(
                "NDNSF_DI_SPEC175_REPO_PUBLISHER_RELEASED",
                f"barrier={barrier}", flush=True)
            return
        time.sleep(0.05)
    raise TimeoutError(
        f"Spec175 Repo publication start barrier timed out: {barrier}")


def publish(args: argparse.Namespace) -> int:
    if not args.stage_manifest:
        raise RuntimeError("publish requires --stage-manifest")
    registration_path = Path(args.registration)
    if registration_path.exists():
        raise FileExistsError(registration_path)
    manifest_path = Path(args.stage_manifest)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    stages = list(manifest.get("stages", ()))
    if [(int(stage["stageIndex"]), stage["role"]) for stage in stages] != [
        (index, f"/LLM/Pipeline/Stage/{index}") for index in range(4)
    ]:
        raise RuntimeError("Spec175 Repo bootstrap requires the four ordered stage roles")
    for stage in stages:
        path = Path(stage["path"])
        if not path.is_file() or sha256_file(path) != digest_hex(stage["sha256"]):
            raise RuntimeError(f"Spec175 Repo source digest mismatch: {path}")

    with open_repo_api(args) as api:
        wait_for_initial_sync(args)
        requirements = ArtifactCapabilityRequirements()
        requirements.artifact = stage_reference(
            args, manifest, stages[0], Path(stages[0]["path"]), format_version="raw-file-v1")
        if not args.probe_output:
            args.probe_output = str(registration_path.with_name("spec175-repo-route-probe.json"))

        # The launcher waits for the publisher to join before opening its
        # publication barrier. Complete authenticated discovery first.
        probe = run_live_service_probe(api, requirements, args)
        if probe["status"] != "PASS":
            raise RuntimeError(f"Spec175 Repo service route probe failed: {args.probe_output}")
        print("NDNSF_DI_SPEC175_REPO_ROUTE_PROBE_PASS",
              f"probe={args.probe_output}", f"service={probe['route']['service']}",
              f"provider={probe['route']['provider']}", flush=True)

        wait_for_publication_start(args)
        target = SelectedRepoTarget()
        target.repo_node = probe["capability"]["repoNode"]
        target.provider_identity = probe["route"]["provider"]
        target.service_prefix = probe["route"]["servicePrefix"]
        selected = SelectedRepoSet()
        selected.targets = [target]
        selected.seed_repo_id = target.repo_node
        selected.consistency = RepoWriteConsistency.ALL

        artifacts = []
        for stage in stages:
            path = Path(stage["path"])
            reference = stage_reference(args, manifest, stage, path)
            started = time.perf_counter()
            result = api.publish_file(path, reference, selected, args.timeout_ms)
            if (result.achieved_replicas != 1 or result.handle is None or
                    result.receipt is None):
                state = result.operation_status.state if result.operation_status else "unknown"
                raise RuntimeError(
                    f"Spec175 Repo publication for {stage['role']} did not commit: "
                    f"operation={result.operation_id} state={state} "
                    f"replicas={result.achieved_replicas}/{result.requested_replicas}")
            committed = [item.receipt_id for item in result.receipt.replica_commits
                         if item.state == "COMMITTED"]
            if len(committed) != 1 or list(result.handle.commit_receipt_ids) != [
                result.receipt.receipt_id, *committed
            ]:
                raise RuntimeError("Spec175 Repo handle and receipt do not match")
            receipt = api.get_receipt(result.receipt.receipt_id, result.handle, args.timeout_ms)
            if receipt.to_json() != result.receipt.to_json():
                raise RuntimeError("Spec175 Repo commit receipt changed")
            artifacts.append({
                "role": str(stage["role"]),
                "stageIndex": int(stage["stageIndex"]),
                "fileSha256": "sha256:" + reference.content_digest,
                "fileBytes": reference.size_bytes,
                "objectName": result.handle.reference.root_manifest_name,
                "artifactHandle": json.loads(result.handle.to_json()),
                "receipt": json.loads(receipt.to_json()),
                "operationId": result.operation_id,
                "publishMs": (time.perf_counter() - started) * 1000.0,
            })
            print("NDNSF_DI_SPEC175_REPO_STAGE_PUBLISHED",
                  f"role={stage['role']}", f"sha256={reference.content_digest}", flush=True)
        _write_json(registration_path, {
            "schema": "ndnsf-di-spec175-repo-registration-v2",
            "modelDigest": manifest["modelDigest"],
            "revision": manifest["revision"],
            "publisher": reference.canonicalized().publisher_identity,
            "artifactCount": len(artifacts),
            "artifacts": artifacts,
        })

    print("NDNSF_DI_SPEC175_REPO_PUBLISH_PASS",
          f"registration={registration_path}", flush=True)
    return 0


def fetch(args: argparse.Namespace) -> int:
    if not args.role or not args.destination:
        raise RuntimeError("fetch requires --role and --destination")
    registration = json.loads(
        Path(args.registration).read_text(encoding="utf-8"))
    if registration.get("schema") != "ndnsf-di-spec175-repo-registration-v2":
        raise RuntimeError("unsupported Spec175 Repo registration")
    matches = [
        dict(item) for item in registration.get("artifacts", ())
        if str(item.get("role", "")) == args.role
    ]
    if len(matches) != 1:
        raise RuntimeError(f"Spec175 Repo role registration is not unique: {args.role}")
    item = matches[0]
    handle = ArtifactHandle.from_json(json.dumps(item["artifactHandle"]))
    reference = handle.reference
    expected = digest_hex(item["fileSha256"])
    if (reference.publisher_identity != registration["publisher"] or
            reference.content_digest != expected or reference.size_bytes != int(item["fileBytes"])):
        raise RuntimeError("Spec175 Repo artifact handle does not match the registered role")
    with open_repo_api(args) as api:
        destination = Path(args.destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        # The publisher owns the aggregate operation receipt. Readers use
        # the replica bindings and the native authenticated fetch path.
        fetched = api.fetch_file(handle, destination, args.timeout_ms)
        if (fetched.repo_node != args.repo_node or
                fetched.provider_identity != args.repo_provider or
                fetched.bytes_verified != reference.size_bytes):
            raise RuntimeError("Spec175 Repo fetch does not bind the expected replica and size")
        if (not destination.is_file() or destination.stat().st_size != reference.size_bytes
                or sha256_file(destination) != expected):
            raise RuntimeError("Spec175 Repo fetch did not reproduce exact artifact")
    print("NDNSF_DI_SPEC175_REPO_FETCH_PASS",
          f"role={args.role}", f"destination={destination}", f"sha256={expected}", flush=True)
    return 0


def main() -> int:
    args = build_parser().parse_args()
    return publish(args) if args.mode == "publish" else fetch(args)


if __name__ == "__main__":
    raise SystemExit(main())
