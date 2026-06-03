#!/usr/bin/env python3
"""Provider for the PyTorch-defined fully connected ONNX 2x2 example."""

from __future__ import annotations

from ndnsf_distributed_inference import (
    APPProvider,
    ProviderRuntimeContext,
    execute_onnx_dependency_chunk,
    prefetch_dependency_inputs,
)

from pytorch_2x2_lib import (
    ROLE_S0_0,
    ROLE_S0_1,
    ROLE_S1_0,
    ROLE_S1_1,
    SERVICE,
    decode_input,
    encode_output,
    optional_local_nfd,
    parse_args_with_common,
)


def handle_role(ctx: ProviderRuntimeContext) -> None:
    model_path = ctx.execution.path("model")
    input_prefetches = prefetch_dependency_inputs(ctx)

    if ctx.role == ROLE_S0_0:
        x = decode_input(ctx.request)
        result = execute_onnx_dependency_chunk(
            ctx,
            model_path,
            initial_values={"x": x},
        )
        print(f"PYTORCH_2X2_INTERMEDIATE role={ctx.role} "
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
        print(f"PYTORCH_2X2_INTERMEDIATE role={ctx.role} "
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
        output = result.value("output")
        ctx.ndnsf.publish_final_response(encode_output(0, output))
        print(f"PYTORCH_2X2_FINAL role={ctx.role} output={tuple(output.shape)}", flush=True)
        return

    ctx.ndnsf.fail(f"unsupported role {ctx.role}")

def main() -> int:
    parser = parse_args_with_common("Run PyTorch-defined ONNX 2x2 provider")
    parser.add_argument("--role", choices=[ROLE_S0_0, ROLE_S0_1, ROLE_S1_0, ROLE_S1_1],
                        default="")
    parser.add_argument("--roles", default="all")
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--temp-dir", default="")
    parser.add_argument("--handler-workers", type=int, default=2)
    args = parser.parse_args()
    if args.dry_run:
        print("Run PyTorch-defined ONNX 2x2 provider", args.provider_id, args.role or args.roles)
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
            backends=["onnxruntime"],
            temp_dir=args.temp_dir or None,
            has_model=True,
            can_provision=False,
        )
        provider.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
