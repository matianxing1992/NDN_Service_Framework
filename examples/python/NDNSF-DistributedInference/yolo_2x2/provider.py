#!/usr/bin/env python3
"""Provider for the real YOLO layout distributed inference example."""

from __future__ import annotations

import subprocess

from ndnsf_distributed_inference import (
    APPProvider,
    ProviderRuntimeContext,
    execute_onnx_dependency_chunk,
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


def handle_role(ctx: ProviderRuntimeContext) -> None:
    model_path = ctx.execution.path("model")
    input_prefetches = prefetch_dependency_inputs(ctx)
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
        service_has_artifacts = bool(provider.deployment.service_policy(service).artifacts)
        dynamic_provisioning = (
            args.dynamic_provisioning or
            (service_has_artifacts and not args.deployed_models)
        )
        provider.serve_service(
            service=service,
            roles=[args.role] if args.role else args.roles,
            handler=handle_role,
            backends=["onnxruntime"],
            temp_dir=args.temp_dir or None,
            has_model=not dynamic_provisioning,
            can_provision=dynamic_provisioning,
            allow_executables=dynamic_provisioning,
        )
        provider.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
