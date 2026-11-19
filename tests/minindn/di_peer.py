#!/usr/bin/env python3
"""Small real DI peers for the Spec192 native Repo MiniNDN qualification."""

from __future__ import annotations

import argparse
import hashlib
import json
import signal
import threading
from pathlib import Path

from ndnsf import ServiceUser
from ndnsf_distributed_inference.app_sdk.contracts import (
    ArtifactReference as DIArtifactReference,
)
from ndnsf_distributed_inference.app_sdk.facades import APPClient, APPProvider
from ndnsf_distributed_inference.repo_artifact_resolver import (
    DIRepoArtifactResolver,
    binding_from_native_handle,
)
from py_repoclient import ArtifactHandle, RepoClient


SERVICE = "/Spec192/DI/Infer"
ROLE = "Inspect"
SLOTS = ("model", "input", "output")


class _RepoFetchProbe:
    """Log the native Repo boundary failure without changing its behavior."""

    def __init__(self, repo_client: RepoClient, identity: str):
        self._repo_client = repo_client
        self._identity = identity

    def fetch_file(self, handle, destination, *, timeout_ms: int = 30000):
        print("SPEC193_DI_REPO_FETCH_BEGIN " + json.dumps({
            "identity": self._identity,
            "digest": handle.reference.content_digest,
            "size": int(handle.reference.size_bytes),
        }, sort_keys=True), flush=True)
        try:
            result = self._repo_client.fetch_file(
                handle, destination, timeout_ms=timeout_ms)
            payload = Path(destination).read_bytes()
            print("SPEC193_DI_REPO_FETCH_OK " + json.dumps({
                "identity": self._identity,
                "digest": hashlib.sha256(payload).hexdigest(), "size": len(payload),
            }, sort_keys=True), flush=True)
            return result
        except Exception as exc:
            print("SPEC192_DI_REPO_FETCH_REJECTED " + json.dumps({
                "identity": self._identity,
                "error_type": type(exc).__name__,
                "error": str(exc),
                "destination_exists": Path(destination).exists(),
            }, sort_keys=True), flush=True)
            raise


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _make_bindings(args: argparse.Namespace) -> int:
    role_entries = {}
    for slot in SLOTS:
        handle_path = args.handle_dir / f"{slot}-handle.json"
        handle = ArtifactHandle.from_json(handle_path.read_text(encoding="utf-8"))
        native_reference = handle.reference
        projection = DIArtifactReference(
            uri=f"urn:spec192:{slot}:{native_reference.content_digest}",
            digest=f"{native_reference.digest_algorithm}:{native_reference.content_digest}",
            size_bytes=int(native_reference.size_bytes),
            mount_path=f"{slot}.bin",
        )
        binding = binding_from_native_handle(
            handle, projection, role=ROLE, use=slot)
        role_entries[slot] = {
            "repoBinding": binding.to_dict(),
            "filename": f"{slot}.bin",
            "kind": "onnx-model" if slot == "model" else "application-data",
        }
        if args.cache_artifacts:
            role_entries[slot]["cache_name"] = "spec193-" + slot
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(
        {"roles": {ROLE: role_entries}}, sort_keys=True, indent=2) + "\n",
        encoding="utf-8")
    print(json.dumps({
        "mode": "bindings",
        "slots": list(SLOTS),
        "digests": {
            slot: role_entries[slot]["repoBinding"]["artifactReference"]["digest"]
            for slot in SLOTS
        },
        "output_sha256": _sha256(args.output),
    }, sort_keys=True))
    return 0


def _serve_provider(args: argparse.Namespace) -> int:
    app = APPProvider.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        provider_id=args.provider_id,
        group=args.group,
        handler_threads=2,
        ack_threads=1,
        handler_workers=1,
    )
    # The production RepoClient uses its own Face here, separate from the
    # APPProvider Face. Its caller identity is the independently authorized
    # Repo reader, not the service Provider identity.
    repo_user = ServiceUser(
        group=args.group,
        controller=args.controller,
        user=args.repo_identity,
        trust_schema=str(args.trust_schema),
        permission_wait_ms=20000,
        adaptive_admission=False,
    )
    repo_user.start()
    repo_client = RepoClient(
        user=repo_user,
        repo_service_name=args.repo_service,
        ack_timeout_ms=1500,
        timeout_ms=30000,
    )
    # Preserve the resolver's exact native-client type check. The test probe
    # wraps only the backing fetch call after construction and rethrows errors.
    resolver = DIRepoArtifactResolver(repo_client)
    resolver._client = _RepoFetchProbe(repo_client, args.repo_identity)

    def handler(ctx) -> None:
        expected = json.loads(bytes(ctx.request).decode("utf-8"))
        actual = {}
        for slot in SLOTS:
            artifact_path = ctx.execution.path(slot)
            payload = artifact_path.read_bytes()
            actual[slot] = {
                "sha256": hashlib.sha256(payload).hexdigest(),
                "size": len(payload),
            }
            if actual[slot] != expected[slot]:
                ctx.ndnsf.fail(f"Spec192 {slot} artifact digest/size mismatch")
                return
        observed = {
            "provider": str(app.deployment.provider_prefix).rstrip("/")
                        + "/" + args.provider_id,
            "role": ROLE,
            "artifacts": actual,
        }
        print("SPEC192_DI_PROVIDER_ARTIFACTS " + json.dumps(observed, sort_keys=True),
              flush=True)
        ctx.ndnsf.publish_final_response(
            json.dumps(observed, sort_keys=True, separators=(",", ":")).encode("utf-8"))

    app.serve_service(
        service=SERVICE,
        roles=[ROLE],
        handler=handler,
        backends=["onnxruntime"],
        temp_dir=str(args.temp_dir),
        has_model=True,
        can_provision=True,
        local_artifacts={ROLE: {}},
        repo_resolver=resolver,
    )

    stopped = threading.Event()
    stop_requested = threading.Event()

    def request_stop(_signum, _frame):
        stop_requested.set()
        stopped.set()

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    app_started = False
    try:
        # start() returns only after the native Provider has installed the
        # handlers, completed its permission fetch, and entered its Face event
        # loop. Do not publish the harness readiness marker before that gate.
        app.start()
        app_started = True
        print("SPEC192_DI_PROVIDER_SERVING " + json.dumps({
            "provider": str(app.deployment.provider_prefix).rstrip("/")
                        + "/" + args.provider_id,
            "boot_epoch": app.provider_boot_epoch,
            "repo_identity": args.repo_identity,
        }, sort_keys=True), flush=True)
        while not stopped.wait(0.25):
            pass
        if not stop_requested.is_set():
            raise RuntimeError("DI Provider stopped without an external stop request")
        return 0
    finally:
        if app_started:
            app.stop()
        repo_user.stop()


def _request(args: argparse.Namespace) -> int:
    client = APPClient.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        group=args.group,
        permission_wait_ms=20000,
        async_workers=1,
    )
    try:
        references = json.loads(args.artifact_references.read_text(encoding="utf-8"))
        plan = client.service_plan(SERVICE, artifact_references=references)
        expected = {}
        for slot in SLOTS:
            binding = references["roles"][ROLE][slot]["repoBinding"]
            projected = binding["artifactReference"]
            expected[slot] = {
                "sha256": projected["digest"].split(":", 1)[1],
                "size": projected["size_bytes"],
            }
        result = client.infer(
            plan,
            json.dumps(expected, sort_keys=True, separators=(",", ":")).encode("utf-8"),
            ack_timeout_ms=1500,
            timeout_ms=60000,
            freshness_ms=60000,
        )
        if not result.status:
            message = str(result.error or "").strip()
            if args.expect_denied:
                print("SPEC192_DI_REQUEST_FAILED " + json.dumps({
                    "status": False,
                    "error": message,
                    "request_id": result.request_id,
                }, sort_keys=True), flush=True)
                return 0
            raise RuntimeError("DI request failed: " + (message or "no error detail"))
        if args.expect_denied:
            raise RuntimeError("unauthorized Provider unexpectedly completed the DI request")
        response = json.loads(result.payload.decode("utf-8"))
        for slot in SLOTS:
            if response.get("artifacts", {}).get(slot) != expected[slot]:
                raise RuntimeError(f"DI response has the wrong {slot} artifact evidence")
        print("SPEC192_DI_REQUEST_PASS " + json.dumps({
            "request_id": result.request_id,
            "provider": response.get("provider"),
            "artifacts": response["artifacts"],
        }, sort_keys=True), flush=True)
        return 0
    finally:
        client.shutdown()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("bindings", "provider", "request"), required=True)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--generated-policy-dir", type=Path)
    parser.add_argument("--group", default="/example/repo/group")
    parser.add_argument("--controller", default="/example/repo/controller")
    parser.add_argument("--trust-schema", type=Path)
    parser.add_argument("--repo-service", default="/NDNSF/DistributedRepo/Object/v1")
    parser.add_argument("--provider-id", default="di")
    parser.add_argument("--repo-identity", default="/example/repo/user")
    parser.add_argument("--temp-dir", type=Path, default=Path("/tmp/spec192-di-provider"))
    parser.add_argument("--artifact-references", type=Path)
    parser.add_argument("--handle-dir", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--expect-denied", action="store_true")
    parser.add_argument("--cache-artifacts", action="store_true")
    args = parser.parse_args()

    if args.mode == "bindings":
        if args.handle_dir is None or args.output is None:
            parser.error("bindings mode requires --handle-dir and --output")
        return _make_bindings(args)
    if args.config is None or args.generated_policy_dir is None:
        parser.error("provider and request modes require --config and --generated-policy-dir")
    if args.mode == "provider":
        if args.trust_schema is None:
            parser.error("provider mode requires --trust-schema")
        return _serve_provider(args)
    if args.artifact_references is None:
        parser.error("request mode requires --artifact-references")
    return _request(args)


if __name__ == "__main__":
    raise SystemExit(main())
