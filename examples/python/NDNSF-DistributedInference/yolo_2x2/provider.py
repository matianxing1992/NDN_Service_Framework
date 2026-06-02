#!/usr/bin/env python3
"""Provider for the real YOLO 2x2 distributed inference example."""

from __future__ import annotations

import subprocess

from ndnsf_distributed_inference import APPProvider, ProviderRuntimeContext

from yolo_2x2_lib import (
    ROLE_S0_0,
    ROLE_S0_1,
    ROLE_S1_0,
    ROLE_S1_1,
    SERVICE,
    decode_binary_payload,
    decode_image,
    decode_image_reference,
    encode_binary_payload,
    encode_yolo_output,
    optional_local_nfd,
    parse_args_with_common,
    run_final_chunk,
    run_intermediate_chunk,
    select_tensor_payload,
    verify_tensor_payload,
    verify_referenced_payload,
)


def _role_index(role: str) -> str:
    return role.strip("/").replace("/", "-")


def _publish_ref(ctx: ProviderRuntimeContext, edge, payload: bytes,
                 suffix: str) -> None:
    name = ctx.ndnsf.publish_large(edge.key_scope, edge.topic(suffix), payload)
    ctx.ndnsf.publish(edge.key_scope, edge.topic("ref/" + suffix), name.encode())


def handle_role(ctx: ProviderRuntimeContext) -> None:
    model_path = ctx.execution.path("model")
    input_prefetch = None
    if ctx.role == ROLE_S0_1:
        input_prefetch = ctx.prefetch_input_large(
            key_scope="stage0-internal",
            topic_suffix="ref/" + _role_index(ROLE_S0_0),
            ref_timeout_ms=10000,
            fetch_timeout_ms=10000,
        )
    elif ctx.role == ROLE_S1_0:
        input_prefetch = ctx.prefetch_input_large(
            key_scope="stage0-to-stage1",
            topic_suffix="ref/" + _role_index(ROLE_S0_1),
            ref_timeout_ms=10000,
            fetch_timeout_ms=10000,
        )
    elif ctx.role == ROLE_S1_1:
        input_prefetch = ctx.prefetch_input_large(
            key_scope="stage1-internal",
            topic_suffix="ref/" + _role_index(ROLE_S1_0),
            ref_timeout_ms=10000,
            fetch_timeout_ms=10000,
        )
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
        activation = run_intermediate_chunk(model_path, images, image_input=True)
        edge = ctx.dependencies.output("stage0-internal")
        payload = encode_binary_payload(
            0,
            select_tensor_payload(activation, edge.tensors),
        )
        _publish_ref(ctx, edge, payload, _role_index(ctx.role))
        print(f"YOLO_2X2_STAGE0_INTERNAL role={ctx.role} "
              f"activation_bytes={len(activation)} tensors={','.join(edge.tensors)}",
              flush=True)
        return

    if ctx.role == ROLE_S0_1:
        input_edge = ctx.dependencies.input("stage0-internal")
        try:
            payload = ctx.wait_prefetched_input_large(input_prefetch, timeout_ms=10000)
        except Exception as exc:
            ctx.ndnsf.fail(f"failed to prefetch stage0 internal activation: {exc}")
            return
        _, activation = decode_binary_payload(payload)
        verify_tensor_payload(activation, input_edge.tensors)
        next_activation = run_intermediate_chunk(model_path, activation)
        output_edge = ctx.dependencies.output("stage0-to-stage1")
        out_payload = encode_binary_payload(
            0,
            select_tensor_payload(next_activation, output_edge.tensors),
        )
        _publish_ref(ctx, output_edge, out_payload, _role_index(ctx.role))
        print(f"YOLO_2X2_STAGE0_BOUNDARY role={ctx.role} "
              f"activation_bytes={len(next_activation)} tensors={','.join(output_edge.tensors)}",
              flush=True)
        return

    if ctx.role == ROLE_S1_0:
        input_edge = ctx.dependencies.input("stage0-to-stage1")
        try:
            payload = ctx.wait_prefetched_input_large(input_prefetch, timeout_ms=10000)
        except Exception as exc:
            ctx.ndnsf.fail(f"failed to prefetch stage0-to-stage1 activation: {exc}")
            return
        _, activation = decode_binary_payload(payload)
        verify_tensor_payload(activation, input_edge.tensors)
        next_activation = run_intermediate_chunk(model_path, activation)
        internal_edge = ctx.dependencies.output("stage1-internal")
        out_payload = encode_binary_payload(
            0,
            select_tensor_payload(next_activation, internal_edge.tensors),
        )
        _publish_ref(ctx, internal_edge, out_payload, _role_index(ctx.role))
        print(f"YOLO_2X2_STAGE1_INTERNAL role={ctx.role} "
              f"activation_bytes={len(next_activation)} tensors={','.join(internal_edge.tensors)}",
              flush=True)
        return

    if ctx.role == ROLE_S1_1:
        internal_edge = ctx.dependencies.input("stage1-internal")
        try:
            peer_payload = ctx.wait_prefetched_input_large(input_prefetch,
                                                           timeout_ms=10000)
        except Exception as exc:
            ctx.ndnsf.fail(f"failed to prefetch stage1 internal activation: {exc}")
            return
        _, activation = decode_binary_payload(peer_payload)
        verify_tensor_payload(activation, internal_edge.tensors)
        output = run_final_chunk(model_path, activation)
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
