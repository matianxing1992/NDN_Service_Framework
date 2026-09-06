#!/usr/bin/env python3
"""Provider for the real YOLO layout distributed inference example."""

from __future__ import annotations

import signal
import threading
import base64
import hashlib
import json
from pathlib import Path
import os
import subprocess

from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from ndnsf_distributed_inference.app_sdk import APPProvider, ProviderRuntimeContext
from ndnsf_distributed_inference.provider import DIProviderOfferIssuerV3
from ndnsf_distributed_inference.policy import load_or_generate_deployment
from ndnsf_distributed_inference.artifact_deployment import (
    ArtifactProvisioningState,
    materialize_role_artifacts,
    materialized_path,
)
from ndnsf_distributed_inference.adapters.onnx.executor import (
    execute_onnx_dependency_chunk,
    prefetch_dependency_inputs,
)

from yolo_2x2_lib import (
    decode_image,
    decode_image_reference,
    _decode_native_tensor_bundle,
    _yolo_merge_postprocess,
    encode_native_tensor_bundle,
    encode_yolo_output,
    optional_local_nfd,
    parse_args_with_common,
    verify_referenced_payload,
    yolo_inference_service,
)


ACTIVE_SERVICE = ""


def _load_v3_offer_signer(key_file: str):
    """Load the candidate-bound Ed25519 key used for V3 ACK offers.

    Provider-offer signatures are an execution input, not a caller-supplied
    HMAC fixture.  The private key is read only in the Provider process; only
    the derived public-key ID is exposed to the offer object.
    """
    path = Path(key_file).expanduser().resolve()
    if not path.is_file() or not path.stat().st_mode & 0o400:
        raise RuntimeError("selection offer signing key is not a readable file")
    try:
        key = serialization.load_pem_private_key(
            path.read_bytes(), password=None, backend=default_backend())
    except (OSError, ValueError, TypeError) as exc:
        raise RuntimeError("invalid selection offer signing key") from exc
    if not isinstance(key, Ed25519PrivateKey):
        raise RuntimeError("selection offer signing key must be Ed25519")
    public = key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    key_id = "sha256:" + hashlib.sha256(public).hexdigest()

    def sign_offer_digest(digest: str) -> str:
        return base64.b64encode(key.sign(str(digest).encode("utf-8"))).decode("ascii")

    return key_id, sign_offer_digest


def _local_model_artifacts(model_path: str, roles: list[str]) -> dict[str, dict]:
    """Bind one canonical ONNX file to explicitly selected local roles."""
    if not model_path:
        return {}
    path = Path(model_path).expanduser().resolve()
    if not path.is_file() or not path.stat().st_mode & 0o400:
        raise RuntimeError("local model path is not a readable file")
    digest = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
    return {
        role: {
            "path": str(path),
            "artifact": "spec180-canonical/" + path.name,
            "filename": path.name,
            "kind": "onnx-model",
            "backend": "onnxruntime-cpu",
            "metadata": {"source": "spec180-canonical-package",
                         "contentDigest": digest},
        }
        for role in roles
    }


def _roles_from_args(provider: APPProvider, service: str, role: str, roles: str):
    if role:
        return [role]
    if isinstance(roles, str) and roles.lower() == "all":
        return provider.roles_for_service(service)
    return [part.strip() for part in roles.split(",") if part.strip()]


def _install_yolo_artifacts(
    *,
    artifact_references: str,
    artifact_cache_dir: str,
    roles: list[str],
    local_artifacts: dict[str, dict],
) -> None:
    for role in roles:
        artifacts = materialize_role_artifacts(
            artifact_references,
            role,
            artifact_cache_dir,
        )
        model = materialized_path(artifacts, "model")
        artifact = artifacts["model"]
        local_artifacts[role] = {
            "path": str(model),
            "artifact": artifact.manifest.object_name,
            "filename": model.name,
            "kind": artifact.manifest.object_type or "onnx-model",
            "backend": "onnxruntime",
            "metadata": dict(artifact.metadata or {}),
        }
    print(
        "YOLO_ARTIFACTS_MATERIALIZED",
        f"roles={','.join(roles)}",
        f"cache={artifact_cache_dir}",
        flush=True,
    )


def handle_role(ctx: ProviderRuntimeContext) -> None:
    input_prefetches = prefetch_dependency_inputs(ctx)
    model_path = ctx.execution.path("model")
    _probe_downloaded_runner(ctx, model_path)

    if (os.environ.get("SPEC180_YN_MUTATION", "") == "Y-N-I"
            and str(ctx.role) == "DetectShard0"):
        # DetectShard0 is a non-ingress role in the fixed shared candidate.
        # Calling the maintained ownership API here must be rejected before
        # the role can fetch or execute any model input.
        try:
            ctx.fetch_application_input()
        except Exception:
            ctx.ndnsf.fail("DI_INPUT_FETCH_ROLE_MISMATCH")
            print(
                "SPEC180_YN_NEGATIVE_RESULT status=PASS subcase=Y-N-I "
                "boundary=PROVIDER_EXECUTION_STARTED "
                "reason=NON_INGRESS_INPUT_REJECTED",
                flush=True,
            )
            return
        raise RuntimeError("DI_INPUT_FETCH_ROLE_MISMATCH_ACCEPTED")

    is_first_chunk = not ctx.dependencies.inputs
    is_final_chunk = not ctx.dependencies.outputs

    if is_first_chunk:
        try:
            image_payload = _fetch_application_input(ctx)
        except Exception as exc:
            ctx.ndnsf.fail(f"failed to load input image reference: {exc}")
            return
        # The ACK-driven runner binds the native tensor bundle format
        # (NDITB001); decode that, falling back to the legacy npz image
        # encoding for non-ACK-driven callers.
        if image_payload.startswith(b"NDITB001"):
            images = _decode_native_tensor_bundle(image_payload)["images"]
        else:
            images = decode_image(image_payload)
        result = execute_onnx_dependency_chunk(
            ctx,
            model_path,
            initial_values={"images": images},
        )
        if is_final_chunk:
            output = result.value("predictions")
            ctx.publish_terminal_result(encode_yolo_output(0, output))
            print(f"YOLO_LAYOUT_FINAL role={ctx.role} output={output.shape}", flush=True)
            return
        print(f"YOLO_LAYOUT_FIRST role={ctx.role} "
              f"outputs={','.join(result.published_edges)}",
              flush=True)
        return

    if not is_final_chunk:
        try:
            result = execute_onnx_dependency_chunk(
                ctx,
                model_path,
                input_prefetches=input_prefetches,
            )
        except Exception as exc:
            ctx.ndnsf.fail(f"failed to execute dependency-driven ONNX chunk: {exc}")
            return
        print(f"YOLO_LAYOUT_INTERMEDIATE role={ctx.role} "
              f"outputs={','.join(result.published_edges)}",
              flush=True)
        return

    if str(ctx.role).rstrip("/") in ("Merge", "/Merge") or \
            str(ctx.role).rstrip("/").endswith("/Merge"):
        # The native Merge owns deterministic postprocessing only (no ONNX
        # layer); run the Python equivalent (spec181 T008) and publish the
        # native predictions bundle the ACK-driven collector expects.
        try:
            predictions = _yolo_merge_postprocess(ctx, input_prefetches)
        except Exception as exc:
            ctx.ndnsf.fail(f"failed to execute YOLO Merge postprocess: {exc}")
            return
        ctx.publish_terminal_result(
            encode_native_tensor_bundle({"predictions": predictions}))
        print(f"YOLO_LAYOUT_FINAL role={ctx.role} output={predictions.shape}",
              flush=True)
        return
    try:
        result = execute_onnx_dependency_chunk(
            ctx,
            model_path,
            input_prefetches=input_prefetches,
        )
    except Exception as exc:
        ctx.ndnsf.fail(f"failed to execute final ONNX chunk: {exc}")
        return
    output = result.value("predictions")
    ctx.publish_terminal_result(encode_yolo_output(0, output))
    print(f"YOLO_LAYOUT_FINAL role={ctx.role} output={output.shape}", flush=True)


def _fetch_application_input(ctx: ProviderRuntimeContext) -> bytes:
    """Fetch the request input through the V3 ownership boundary.

    The maintained ACK-driven path always uses ``fetch_application_input``.
    The explicit offline-oracle path may still receive the historical request
    envelope, so it retains a narrow compatibility fallback to the old
    reference helper when dataflow ownership is not enforced.
    """

    try:
        return bytes(ctx.fetch_application_input())
    except Exception:
        if bool(getattr(ctx, "enforce_dataflow_ownership", False)):
            raise
        image_ref = decode_image_reference(ctx.request)
        image_payload = ctx.ndnsf.fetch_encrypted_large_data(
            str(image_ref["data_name"]), ACTIVE_SERVICE)
        if image_payload is None:
            raise RuntimeError("failed to fetch input image reference")
        verify_referenced_payload(image_ref, image_payload)
        return bytes(image_payload)


def _probe_downloaded_runner(ctx: ProviderRuntimeContext, model_path) -> None:
    try:
        runner = ctx.execution.executable("runner")
    except KeyError:
        return
    completed = subprocess.run(
        [str(runner), "--probe", ctx.role, str(model_path)],
        check=True,
        cwd=str(ctx.execution.work_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    print(completed.stdout.strip(), flush=True)


def _load_grant_keys(provider_id: str = "", *, provider_prefix="/example/provider"):
    """Load protected-epoch grant key material from the runner environment.

    Returns (authority_public_key, recipient_private_key, authority_identity)
    under a protected epoch; (None, None, "") for plaintext-v1, where the grant path is never
    entered (spec181 T001 wiring).  The recipient key is this Provider's
    own Ed25519 or EC P-256 key, looked up by Provider identity in the
    runner-supplied recipient map independently of offer signing.
    """
    epoch = os.environ.get("SPEC181_PROTECTION_EPOCH", "").strip()
    if not epoch or epoch == "plaintext-v1":
        return None, None, ""
    from ndnsf_distributed_inference.security.registry_keys import (
        load_artifact_policy_authority_registry, load_grant_recipient_private_key)
    authority_pub_path = Path(os.environ["SPEC181_GRANT_AUTHORITY_PUBLIC_KEY"])
    policy = load_artifact_policy_authority_registry(
        authority_pub_path.with_name("trust-root-registry-v1.json"),
        model_family="YOLO26n", protection_epoch=epoch)
    recipient_map_path = Path(os.environ["SPEC181_PROVIDER_RECIPIENT_KEY_MAP"])
    recipient_entries = json.loads(recipient_map_path.read_text(
        encoding="utf-8"))
    identity = str(provider_prefix).rstrip("/")
    if provider_id:
        identity += "/" + str(provider_id).strip("/")
    recipient_priv_path = recipient_entries.get(identity)
    if not recipient_priv_path:
        raise ValueError(
            f"no grant recipient key for Provider identity {identity}")
    recipient_key = load_grant_recipient_private_key(recipient_priv_path)
    return policy.public_key, recipient_key, policy.authority_id


def main() -> int:
    parser = parse_args_with_common("Run YOLO layout provider")
    parser.add_argument("--role", default="")
    parser.add_argument("--roles", default="all")
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--temp-dir", default="")
    parser.add_argument("--handler-workers", type=int, default=2)
    parser.add_argument("--dynamic-provisioning", action="store_true",
                        help="kept for older commands; providers can provision dynamically by default")
    parser.add_argument("--deployed-models", action="store_true",
                        help="load role artifacts from local paths in the service policy")
    parser.add_argument("--artifact-references", default="",
                        help="Repo-backed artifact reference file to install before serving ONNX roles")
    parser.add_argument("--artifact-cache-dir", default="/tmp/ndnsf-di-yolo-artifacts",
                        help="Provider-local cache for materialized ONNX/runtime artifacts")
    parser.add_argument("--sync-materialize-before-serve", action="store_true",
                        help="Wait for artifact installation before registering service capability")
    parser.add_argument("--install-timeout-s", type=float, default=300.0)
    parser.add_argument(
        "--selection-offer-key-file",
        default="",
        help="Ed25519 private key for candidate-bound ProviderOfferV3 ACKs",
    )
    parser.add_argument(
        "--local-model-path", default="",
        help="absolute canonical ONNX model path for local role execution",
    )
    parser.add_argument(
        "--backend", default="onnxruntime-cpu",
        choices=("onnxruntime-cpu", "onnxruntime-cuda"),
        help=("ORT execution backend; the Tiger functional gate maps the "
              "three model roles to CUDA device 0"),
    )
    args = parser.parse_args()
    if args.dry_run:
        print("Run YOLO 2x2 provider", args.provider_id, args.role or args.roles)
        return 0
    with optional_local_nfd(args.start_local_nfd):
        grant_deployment = load_or_generate_deployment(
            args.config, args.generated_policy_dir)
        grant_authority_public_key, grant_recipient_private_key, grant_authority_identity = (
            _load_grant_keys(args.provider_id, provider_prefix=grant_deployment.provider_prefix))
        provider = APPProvider.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
            provider_id=args.provider_id,
            group=args.group,
            handler_workers=args.handler_workers,
            grant_authority_public_key=grant_authority_public_key,
            grant_authority_identity=grant_authority_identity,
            grant_recipient_private_key=grant_recipient_private_key,
        )
        service = yolo_inference_service(provider.deployment)
        global ACTIVE_SERVICE
        ACTIVE_SERVICE = service
        selected_roles = _roles_from_args(provider, service, args.role, args.roles)
        if os.environ.get("SPEC180_YN_MUTATION", "") == "Y-N-C":
            # Exercise the real capability publication path with an incomplete
            # closed ACK snapshot.  Keep a Provider process alive even when a
            # fixture advertises only the removed role; the replacement is an
            # already allowlisted role and cannot restore FullModel/Merge
            # coverage.
            selected_roles = [
                role for role in selected_roles
                if role not in {"FullModel", "Merge"}
            ]
            if not selected_roles:
                selected_roles = ["BackboneNeck"]
        local_artifacts: dict[str, dict] = {}
        if args.local_model_path:
            local_artifacts.update(
                _local_model_artifacts(args.local_model_path, selected_roles))
        readiness = None
        if args.artifact_references:
            provisioning = ArtifactProvisioningState(
                component="yolo onnx artifacts",
                initial_status="installing",
                initial_message="materializing ONNX role artifacts",
            )
            provisioning.start_install(
                lambda: _install_yolo_artifacts(
                    artifact_references=args.artifact_references,
                    artifact_cache_dir=args.artifact_cache_dir,
                    roles=selected_roles,
                    local_artifacts=local_artifacts,
                ),
                installing_message="materializing ONNX role artifacts",
                ready_message="ONNX role artifacts ready",
                thread_name="ndnsf-di-yolo-artifact-install",
                start_marker="YOLO_ARTIFACT_INSTALL_STARTED",
                fail_marker="YOLO_ARTIFACT_INSTALL_FAILED",
            )
            readiness = provisioning.ack
            if args.sync_materialize_before_serve:
                if not provisioning.wait_ready(args.install_timeout_s):
                    raise RuntimeError("YOLO artifacts did not become ready")
        service_has_artifacts = bool(provider.deployment.service_policy(service).artifacts)
        dynamic_provisioning = (
            args.dynamic_provisioning or
            (service_has_artifacts and not args.deployed_models and not args.artifact_references)
        )
        # A candidate-bound local artifact is authoritative for this process;
        # do not advertise dynamic provisioning for the same role set.
        if local_artifacts:
            dynamic_provisioning = False
        offer_issuer_v3 = None
        if args.selection_offer_key_file:
            signer_key_id, sign_offer_digest = _load_v3_offer_signer(
                args.selection_offer_key_file)
            provider_identity = (
                str(provider.deployment.provider_prefix).rstrip("/")
                + "/" + str(args.provider_id).strip("/"))
            offer_issuer_v3 = DIProviderOfferIssuerV3(
                provider=provider_identity,
                service=service,
                boot_epoch=provider.provider_boot_epoch,
                # DeviceTopologyProfile lists accelerator identities only;
                # an ONNX Runtime CPU Provider therefore has no device entry.
                devices=(),
                signer_key_id=signer_key_id,
                sign_offer_digest=sign_offer_digest,
            )
        provider.serve_service(
            service=service,
            roles=selected_roles,
            handler=handle_role,
            backends=[args.backend],
            temp_dir=args.temp_dir or None,
            has_model=(not dynamic_provisioning) or bool(args.artifact_references),
            can_provision=dynamic_provisioning,
            # The YOLO artifacts are ONNX models executed in-process by
            # ONNX Runtime, never downloaded executables.  Executable
            # artifact authorization stays disabled for this application.
            allow_executables=False,
            readiness_probe=readiness,
            local_artifacts=local_artifacts,
            selection_offer_issuer_v3=offer_issuer_v3,
        )
        # spec181 T005: the Python provider serves through the native run
        # loop (GIL released); SIGINT would never be delivered at bytecode
        # boundaries.  Run the native loop on a worker thread and keep the
        # main thread in a wait loop whose signal handler only sets an event,
        # then stop() and exit 130 inside the supervised 3 s window.
        stop_requested = threading.Event()

        def _on_sigint(signum, frame):
            del signum, frame
            stop_requested.set()

        signal.signal(signal.SIGINT, _on_sigint)
        runner_thread = threading.Thread(target=provider.run, daemon=True)
        runner_thread.start()
        while not stop_requested.wait(0.5):
            if not runner_thread.is_alive():
                return 0
        try:
            provider.stop()
        except Exception:
            pass
        runner_thread.join(timeout=3)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
