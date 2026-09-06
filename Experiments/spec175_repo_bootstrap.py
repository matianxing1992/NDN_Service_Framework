#!/usr/bin/env python3
"""Publish or fetch the frozen Spec175 tiny-ONNX bundle through DistributedRepo."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import time

from py_repoclient import (
    ArtifactRepositoryApi,
    CollaborationArtifactApiBackend,
    artifact_reference_from_dict,
)
from py_repoclient.orchestration import encode_repo_request
from py_repoclient.service_names import repo_service_for_operation


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
    parser.add_argument("--config", required=True)
    parser.add_argument("--generated-policy-dir", required=True)
    parser.add_argument("--state-root", required=True)
    parser.add_argument("--user", required=True)
    parser.add_argument("--bootstrap-token-file", required=True)
    parser.add_argument("--registration", required=True)
    parser.add_argument("--stage-manifest")
    parser.add_argument("--object-prefix", default="/NDNSF/Spec175/TinyOnnx")
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
              "publishing the first repository request."),
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
    parser.add_argument(
        "--test-only-allow-ephemeral-app-state", action="store_true",
        help=("Allow the named volatile state root only for an explicit "
              "real-MiniNDN test; production use must provide persistent state"),
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


def _response_provider(response) -> str:
    """Extract a provider identity without trusting peer-controlled payload."""
    data_name = str(getattr(response, "data_name", "") or "")
    if "/NDNSF/" in data_name:
        # Response names begin with the provider identity and contain the
        # protocol marker later in the name; rsplit avoids treating a
        # provider whose identity itself contains an NDN namespace as empty.
        prefix = data_name.rsplit("/NDNSF/", 1)[0].rstrip("/")
        if prefix:
            return prefix
    return str(getattr(response, "signer_certificate", "") or "").strip()


def _write_probe_report(path: Path, report: dict) -> None:
    path = path.expanduser().resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(
        f".{path.name}.tmp.{os.getpid()}.{time.time_ns()}")
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _run_live_service_probe_once(backend, args: argparse.Namespace) -> dict:
    """Probe the exact Repo service using the publisher's initialized identity.

    CAPABILITY is read-only.  The probe is deliberately sent through the same
    native ``ServiceUser`` that will publish the stage objects, so a passing
    result proves the route, identity, permission state, and provider response
    before the one-shot publication barrier is released.
    """
    service = repo_service_for_operation("CAPABILITY", "/NDNSF/DistributedRepo")
    probe_id = str(getattr(args, "_probe_id", "") or
                   f"spec175-route-probe-{time.time_ns()}")
    payload = encode_repo_request(
        "CAPABILITY", probeId=probe_id, nonMutating=True,
        publisherIdentity=str(args.user),
    )
    started_ms = time.time_ns() // 1_000_000
    report = {
        "schema": "ndnsf-di-spec175-repo-route-probe-v1",
        "status": "FAIL",
        "probeId": probe_id,
        "publisherIdentity": str(args.user),
        "request": {
            "operation": "CAPABILITY",
            "service": service,
            "nonMutating": True,
            "payloadSha256": "sha256:" + hashlib.sha256(payload).hexdigest(),
            "payloadBytes": len(payload),
        },
        "route": {
            "service": service,
            "face": "native-ServiceUser-transport",
        },
        "timing": {"sendAtMs": started_ms},
    }
    try:
        service_user = getattr(getattr(backend, "control", None),
                               "service_user", None)
        if service_user is None:
            raise RuntimeError("publisher ServiceUser is not initialized")
        timeout_ms = int(
            getattr(args, "probe_timeout_ms", None)
            or max(int(getattr(args, "ack_timeout_ms", 5000)),
                   int(getattr(args, "timeout_ms", 60000))))
        response = service_user.request_service(
            service,
            payload,
            ack_timeout_ms=int(getattr(args, "ack_timeout_ms", 5000)),
            timeout_ms=timeout_ms,
            strategy="first-responding",
            request_id=probe_id,
        )
        finished_ms = time.time_ns() // 1_000_000
        report["timing"]["responseAtMs"] = finished_ms
        # ``ServiceUser.request_service`` deliberately exposes one synchronous
        # terminal result rather than an ACK callback.  Preserve an explicit
        # ACK observation timestamp at that API boundary instead of inventing
        # sub-millisecond ACK timing that the wrapper cannot observe.
        report["timing"]["ackAtMs"] = finished_ms
        report["timing"]["ackTimestampSource"] = (
            "ServiceUser.request_service completion (ACK/Response boundary)")
        report["timing"]["elapsedMs"] = max(0, finished_ms - started_ms)
        report["response"] = {
            "status": bool(getattr(response, "status", False)),
            "error": str(getattr(response, "error", "") or ""),
            "dataName": str(getattr(response, "data_name", "") or ""),
            "signerCertificate": str(
                getattr(response, "signer_certificate", "") or ""),
            "wireDigest": str(getattr(response, "wire_digest", "") or ""),
        }
        provider = _response_provider(response)
        decoded = None
        if bool(getattr(response, "status", False)):
            try:
                decoded = json.loads(bytes(response.payload).decode("utf-8"))
            except (TypeError, ValueError, UnicodeDecodeError):
                decoded = None
        report["response"]["payloadSchema"] = (
            decoded.get("schema") if isinstance(decoded, dict) else "")
        report["route"]["provider"] = provider
        if not bool(getattr(response, "status", False)):
            report["failureReason"] = "REPO_SERVICE_ROUTE_NOT_READY"
        elif not isinstance(decoded, dict) or not provider:
            report["failureReason"] = "REPO_SERVICE_ROUTE_INVALID_RESPONSE"
        else:
            report["status"] = "PASS"
            report["capability"] = {
                "repoNode": str(decoded.get("repoNode", "")),
                "repoMode": str(decoded.get("repoMode", "")),
                "availabilityScore": decoded.get("availabilityScore"),
            }
            if not report["capability"]["repoNode"]:
                report["status"] = "FAIL"
                report["failureReason"] = "REPO_SERVICE_ROUTE_INVALID_RESPONSE"
    except Exception as exc:  # noqa: BLE001 - convert to stable evidence
        report["timing"]["responseAtMs"] = time.time_ns() // 1_000_000
        report["timing"]["ackAtMs"] = None
        report["timing"]["ackTimestampSource"] = "not-observed"
        report["timing"]["elapsedMs"] = max(
            0, report["timing"]["responseAtMs"] - started_ms)
        report["failureReason"] = "REPO_SERVICE_ROUTE_NOT_READY"
        report["error"] = f"{type(exc).__name__}: {exc}"
    output = getattr(args, "probe_output", None)
    if output:
        _write_probe_report(Path(output), report)
    return report


def run_live_service_probe(backend, args: argparse.Namespace) -> dict:
    """Run a bounded sequence of fresh, non-mutating route probes.

    A single missing ACK can occur while a MiniNDN provider is finishing its
    native startup.  Retry with a fresh request ID, while preserving every
    attempt in the final evidence.  This does not turn a marker or a sleep
    into readiness: the publisher is released only after an actual validated
    ACK/Response succeeds.
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
        attempt_args._probe_id = (
            f"spec175-route-probe-{attempt_index + 1}-{time.time_ns()}"
        )
        attempt_args.probe_timeout_ms = min(attempt_timeout_ms, remaining_ms)
        attempt_args.probe_output = None
        attempt = _run_live_service_probe_once(backend, attempt_args)
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
            "schema": "ndnsf-di-spec175-repo-route-probe-v1",
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
        _write_probe_report(Path(output), report)
    return report


def make_backend(args: argparse.Namespace, *, receipts=()):
    token = Path(args.bootstrap_token_file).read_text(encoding="utf-8").strip()
    if not token:
        raise RuntimeError("Spec175 Repo bootstrap token is empty")
    ack_timeout_ms = int(getattr(args, "ack_timeout_ms", 5_000))
    if ack_timeout_ms <= 0:
        raise ValueError("Spec175 Repo ACK timeout must be positive")
    return CollaborationArtifactApiBackend.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        state_root=args.state_root,
        user=args.user,
        bootstrap_token=token,
        committed_receipts=tuple(dict(item) for item in receipts),
        ack_timeout_ms=ack_timeout_ms,
        packet_payload_bytes=7600,
        chunk_bytes=1024 * 1024,
        test_only_allow_ephemeral_state_root=bool(
            getattr(args, "test_only_allow_ephemeral_app_state", False)),
    )


def receipts_for_publish_result(backend, result) -> list[dict]:
    """Return only receipts committed by one publish operation."""
    committed_ids = {
        str(replica.receipt_id)
        for replica in result.replicas
        if str(replica.state) == "COMMITTED" and str(replica.receipt_id)
    }
    return [
        dict(value) for value in backend.last_receipts
        if str(dict(value.get("receipt", {})).get("receiptId", ""))
        in committed_ids
    ]


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
    if len(stages) != 4:
        raise RuntimeError("Spec175 Repo bootstrap requires exactly four stages")
    backend = make_backend(args)
    try:
        # ``make_backend`` creates and joins the publisher ServiceUser.  Its
        # first Request can reach the already-running Repo Provider before the
        # new User has learned enough provider state to receive the ACK.  Keep
        # this cold-start barrier after ServiceUser construction, not before
        # process launch or Provider readiness.
        wait_for_initial_sync(args)
        api = ArtifactRepositoryApi(
            backend, publisher_identity=args.user,
            default_timeout_ms=args.timeout_ms)
        probe_output = getattr(args, "probe_output", None)
        if not probe_output:
            probe_output = str(registration_path.with_name(
                "spec175-repo-route-probe.json"))
        args.probe_output = probe_output
        probe = run_live_service_probe(backend, args)
        if probe.get("status") != "PASS":
            print(
                "REPO_SERVICE_ROUTE_NOT_READY",
                f"probe={probe_output}",
                f"reason={probe.get('failureReason', 'unknown')}",
                flush=True,
            )
            raise RuntimeError(
                "Spec175 Repo service route probe failed; "
                f"evidence={probe_output}")
        print(
            "NDNSF_DI_SPEC175_REPO_ROUTE_PROBE_PASS",
            f"probe={probe_output}",
            f"service={probe['route']['service']}",
            f"provider={probe['route'].get('provider', '')}",
            flush=True,
        )
        # Creating the backend starts the native ServiceUser and completes its
        # permission bootstrap.  Wait only after that initialization so the
        # User and Repo Provider can acquire controller state concurrently,
        # while the first SVS publication is held until the Repo handler is
        # actually ready.
        wait_for_publication_start(args)
        artifacts = []
        for stage in stages:
            path = Path(str(stage["path"]))
            digest = digest_hex(stage["sha256"])
            if not path.is_file() or sha256_file(path) != digest:
                raise RuntimeError(f"Spec175 Repo source digest mismatch: {path}")
            object_name = (
                f"{args.object_prefix.rstrip('/')}/stage-{int(stage['stageIndex'])}-{digest}")
            started = time.perf_counter()
            result = api.publish_file(
                path,
                name=object_name,
                expected_sha256=digest,
                replicas=1,
                policy_epoch=str(manifest["modelDigest"]),
                idempotency_key=(
                    f"spec175:{manifest['modelDigest']}:{stage['stageIndex']}:{digest}"),
                timeout_ms=args.timeout_ms,
            )
            receipts = receipts_for_publish_result(backend, result)
            if result.achieved_replicas != 1 or len(receipts) != 1:
                raise RuntimeError("Spec175 Repo publication lacks one committed receipt")
            artifacts.append({
                "role": str(stage["role"]),
                "stageIndex": int(stage["stageIndex"]),
                "fileSha256": "sha256:" + digest,
                "fileBytes": path.stat().st_size,
                "objectName": str(receipts[0].get("dataName", "")),
                "artifactReference": result.reference.to_dict(),
                "operationId": result.operation_id,
                "receipts": receipts,
                "publishMs": (time.perf_counter() - started) * 1000.0,
            })
            if not artifacts[-1]["objectName"]:
                raise RuntimeError("Spec175 Repo receipt omits committed Data name")
            print(
                "NDNSF_DI_SPEC175_REPO_STAGE_PUBLISHED",
                f"role={stage['role']}", f"sha256={digest}", flush=True)
        record = {
            "schema": "ndnsf-di-spec175-repo-registration-v1",
            "modelDigest": manifest["modelDigest"],
            "revision": manifest["revision"],
            "publisher": args.user,
            "artifactCount": len(artifacts),
            "artifacts": artifacts,
        }
        registration_path.parent.mkdir(parents=True, exist_ok=True)
        registration_path.write_text(
            json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        backend.close()
        print(
            "NDNSF_DI_SPEC175_REPO_PUBLISH_PASS",
            f"registration={registration_path}", flush=True)
        return 0
    finally:
        backend.close()


def fetch(args: argparse.Namespace) -> int:
    if not args.role or not args.destination:
        raise RuntimeError("fetch requires --role and --destination")
    registration = json.loads(
        Path(args.registration).read_text(encoding="utf-8"))
    if registration.get("schema") != "ndnsf-di-spec175-repo-registration-v1":
        raise RuntimeError("unsupported Spec175 Repo registration")
    matches = [
        dict(item) for item in registration.get("artifacts", ())
        if str(item.get("role", "")) == args.role
    ]
    if len(matches) != 1:
        raise RuntimeError(f"Spec175 Repo role registration is not unique: {args.role}")
    item = matches[0]
    receipts = tuple(
        dict(receipt)
        for artifact in registration.get("artifacts", ())
        for receipt in artifact.get("receipts", ())
    )
    backend = make_backend(args, receipts=receipts)
    try:
        api = ArtifactRepositoryApi(
            backend, publisher_identity=args.user,
            default_timeout_ms=args.timeout_ms)
        destination = Path(args.destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        reference = artifact_reference_from_dict(dict(item["artifactReference"]))
        result = api.fetch_file(
            reference, destination, replace=False, timeout_ms=args.timeout_ms)
        expected = digest_hex(item["fileSha256"])
        if (not destination.is_file()
                or destination.stat().st_size != int(item["fileBytes"])
                or sha256_file(destination) != expected):
            raise RuntimeError("Spec175 Repo fetch did not reproduce exact artifact")
        backend.close()
        print(
            "NDNSF_DI_SPEC175_REPO_FETCH_PASS",
            f"role={args.role}", f"destination={destination}",
            f"sha256={expected}", flush=True)
        return 0
    finally:
        backend.close()


def main() -> int:
    args = build_parser().parse_args()
    return publish(args) if args.mode == "publish" else fetch(args)


if __name__ == "__main__":
    raise SystemExit(main())
