#!/usr/bin/env python3
"""Provider for the real YOLO 2x2 distributed inference example."""

from __future__ import annotations

import subprocess

from ndnsf_distributed_inference import (
    APPProvider,
    ProviderRuntimeContext,
    execute_onnx_dependency_chunk,
    prefetch_dependency_inputs,
)

from yolo_2x2_lib import (
    ROLE_S0_0,
    ROLE_S0_1,
    ROLE_S1_0,
    ROLE_S1_1,
    SERVICE,
    decode_image,
    decode_image_reference,
    encode_yolo_output,
    optional_local_nfd,
    parse_args_with_common,
    verify_referenced_payload,
)


def handle_role(ctx: ProviderRuntimeContext) -> None:
    model_path = ctx.execution.path("model")
    input_prefetches = prefetch_dependency_inputs(ctx)
    _probe_downloaded_runner(ctx, model_path)

    if ctx.role == ROLE_S0_0:
        try:
            image_ref = decode_image_reference(ctx.request)
            image_payload = ctx.ndnsf.fetch_encrypted_large_data(
                str(image_ref["data_name"]),
                SERVICE,
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
        print(f"YOLO_2X2_STAGE0_INTERNAL role={ctx.role} "
              f"outputs={','.join(result.published_edges)}",
              flush=True)
        return

    if ctx.role in (ROLE_S0_1, ROLE_S1_0):
        try:
            result = execute_onnx_dependency_chunk(
                ctx,
                model_path,
                input_prefetches=input_prefetches,
            )
        except Exception as exc:
            ctx.ndnsf.fail(f"failed to execute dependency-driven ONNX chunk: {exc}")
            return
        print(f"YOLO_2X2_INTERMEDIATE role={ctx.role} "
              f"outputs={','.join(result.published_edges)}",
              flush=True)
        return

    if ctx.role == ROLE_S1_1:
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
        print(f"YOLO_2X2_STAGE1_FINAL role={ctx.role} output={output.shape}", flush=True)
        return

    ctx.ndnsf.fail(f"unsupported role {ctx.role}")


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
    parser = parse_args_with_common("Run YOLO 2x2 provider")
    parser.add_argument("--role", choices=[ROLE_S0_0, ROLE_S0_1, ROLE_S1_0, ROLE_S1_1],
                        default="")
    parser.add_argument("--roles", default="all")
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--temp-dir", default="")
    parser.add_argument("--handler-workers", type=int, default=2)
    parser.add_argument("--dynamic-provisioning", action="store_true")
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
        provider.serve(
            service=SERVICE,
            roles=[args.role] if args.role else args.roles,
            handler=handle_role,
            backends=["numpy"],
            temp_dir=args.temp_dir or None,
            has_model=not args.dynamic_provisioning,
            can_provision=args.dynamic_provisioning,
            allow_executables=args.dynamic_provisioning,
        )
        provider.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
