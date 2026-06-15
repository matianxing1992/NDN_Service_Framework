#!/usr/bin/env python3
"""Provider for the real YOLO layout distributed inference example."""

from __future__ import annotations

import subprocess

from ndnsf_distributed_inference import (
    APPProvider,
    ArtifactProvisioningState,
    ProviderRuntimeContext,
    execute_onnx_dependency_chunk,
    materialize_role_artifacts,
    materialized_path,
    prefetch_dependency_inputs,
)

from yolo_2x2_lib import (
    decode_image,
    decode_image_reference,
    encode_yolo_output,
    optional_local_nfd,
    parse_args_with_common,
    verify_referenced_payload,
    yolo_inference_service,
)


ACTIVE_SERVICE = ""


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

    is_first_chunk = not ctx.dependencies.inputs
    is_final_chunk = not ctx.dependencies.outputs

    if is_first_chunk:
        try:
            image_ref = decode_image_reference(ctx.request)
            image_payload = ctx.ndnsf.fetch_encrypted_large_data(
                str(image_ref["data_name"]),
                ACTIVE_SERVICE,
            )
            if image_payload is None:
                ctx.ndnsf.fail("failed to fetch input image reference")
                return
            verify_referenced_payload(image_ref, image_payload)
        except Exception as exc:
            ctx.ndnsf.fail(f"failed to load input image reference: {exc}")
            return
        images = decode_image(image_payload)
        result = execute_onnx_dependency_chunk(
            ctx,
            model_path,
            initial_values={"images": images},
        )
        if is_final_chunk:
            output = result.value("predictions")
            ctx.ndnsf.publish_final_response(encode_yolo_output(0, output))
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
    ctx.ndnsf.publish_final_response(encode_yolo_output(0, output))
    print(f"YOLO_LAYOUT_FINAL role={ctx.role} output={output.shape}", flush=True)


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
    args = parser.parse_args()
    if args.dry_run:
        print("Run YOLO 2x2 provider", args.provider_id, args.role or args.roles)
        return 0
    with optional_local_nfd(args.start_local_nfd):
        provider = APPProvider.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
            provider_id=args.provider_id,
            group=args.group,
            handler_workers=args.handler_workers,
        )
        service = yolo_inference_service(provider.deployment)
        global ACTIVE_SERVICE
        ACTIVE_SERVICE = service
        selected_roles = _roles_from_args(provider, service, args.role, args.roles)
        local_artifacts: dict[str, dict] = {}
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
        provider.serve_service(
            service=service,
            roles=selected_roles,
            handler=handle_role,
            backends=["onnxruntime"],
            temp_dir=args.temp_dir or None,
            has_model=(not dynamic_provisioning) or bool(args.artifact_references),
            can_provision=dynamic_provisioning,
            allow_executables=dynamic_provisioning,
            readiness_probe=readiness,
            local_artifacts=local_artifacts,
        )
        provider.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
