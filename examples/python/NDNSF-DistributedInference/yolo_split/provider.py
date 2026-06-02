#!/usr/bin/env python3
"""YOLO split-inference provider built on NDNSF-DistributedInference."""

from __future__ import annotations

import argparse

from ndnsf_distributed_inference import (
    APPProvider,
    ProviderRuntimeContext,
)

from yolo_split_lib import (
    CONFIG_FILE,
    ROLE_STAGE0,
    ROLE_STAGE1,
    SERVICE,
    optional_local_nfd,
    run_onnx_stage0,
    run_onnx_stage1,
)


def handle_role(ctx: ProviderRuntimeContext) -> None:
    ndnsf = ctx.ndnsf

    if ctx.role == ROLE_STAGE0:
        activation = run_onnx_stage0(ctx.execution.path("model"), ctx.request)
        large_name = ctx.publish_output_large(
            activation,
            max_segment_size=7000,
            freshness_ms=60000,
        )
        edge = ctx.dependencies.output()
        ndnsf.publish(edge.key_scope, edge.topic("ref"), large_name.encode())
        print(f"YOLO_STAGE0 model={ctx.execution.path('model')} "
              f"work_dir={ctx.execution.work_dir} activation_bytes={len(activation)}",
              flush=True)
        return

    if ctx.role == ROLE_STAGE1:
        edge = ctx.dependencies.input()
        ref = ndnsf.wait_one(edge.key_scope, edge.topic("ref"), 10000)
        if ref is None:
            ndnsf.fail("timed out waiting for stage0 activation reference")
            return
        activation = ndnsf.fetch_large(ref.payload.decode(), edge.key_scope, 10000)
        if activation is None:
            ndnsf.fail("failed to fetch segmented stage0 activation")
            return
        payload = run_onnx_stage1(ctx.execution.path("model"), activation)
        ndnsf.publish_final_response(payload)
        print(f"YOLO_STAGE1 model={ctx.execution.path('model')} "
              f"work_dir={ctx.execution.work_dir} activation_bytes={len(activation)} "
              f"response_bytes={len(payload)}", flush=True)
        return

    ndnsf.fail(f"unsupported role {ctx.role}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=[ROLE_STAGE0, ROLE_STAGE1], default="",
                        help="Restrict this provider to one role")
    parser.add_argument("--roles", default="all",
                        help="Comma-separated role capabilities, or 'all'")
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--group", default="",
                        help="Override SVS group from the policy config")
    parser.add_argument("--temp-dir", default="")
    parser.add_argument("--config", default=CONFIG_FILE,
                        help="User-facing distributed inference policy config")
    parser.add_argument("--generated-policy-dir", default="/tmp/ndnsf-di-yolo-policy",
                        help="Directory for generated trust schema and policy files")
    parser.add_argument("--allow-executables", action="store_true",
                        help="Allow executable artifacts only if policy security gates pass")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--start-local-nfd", action="store_true")
    args = parser.parse_args()

    provider_id = args.provider_id
    if args.dry_run:
        roles_text = args.role or args.roles
        print(f"Run YOLO distributed inference provider roles={roles_text} "
              f"provider_id={provider_id!r}")
        print("config:", args.config)
        return 0

    with optional_local_nfd(args.start_local_nfd):
        app_provider = APPProvider.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
            provider_id=provider_id,
            group=args.group,
            handler_threads=4,
            ack_threads=2,
        )
        roles = [args.role] if args.role else args.roles
        app_provider.serve(
            service=SERVICE,
            roles=roles,
            handler=handle_role,
            backends=["onnxruntime"],
            temp_dir=args.temp_dir or None,
            has_model=True,
            can_provision=False,
            allow_executables=args.allow_executables,
        )
        app_provider.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
