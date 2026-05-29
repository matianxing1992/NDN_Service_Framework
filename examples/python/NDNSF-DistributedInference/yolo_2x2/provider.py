#!/usr/bin/env python3
"""Provider for the YOLO-style 2x2 distributed inference example."""

from __future__ import annotations

import subprocess

from ndnsf_distributed_inference import APPProvider, ProviderRuntimeContext

from yolo_2x2_lib import (
    ROLE_S0_0,
    ROLE_S0_1,
    ROLE_S1_0,
    ROLE_S1_1,
    SERVICE,
    concat_by_offset,
    decode_hidden,
    decode_image,
    decode_yolo_output,
    encode_hidden,
    encode_yolo_output,
    load_part,
    optional_local_nfd,
    parse_args_with_common,
)


def _role_index(role: str) -> str:
    return role.strip("/").replace("/", "-")


def _publish_ref(ctx: ProviderRuntimeContext, edge, payload: bytes,
                 suffix: str) -> None:
    name = ctx.ndnsf.publish_large(edge.key_scope, edge.topic(suffix), payload)
    ctx.ndnsf.publish(edge.key_scope, edge.topic("ref/" + suffix), name.encode())


def handle_role(ctx: ProviderRuntimeContext) -> None:
    model_path = ctx.execution.path("model")
    _probe_downloaded_runner(ctx, model_path)
    part = load_part(model_path)

    if ctx.role in (ROLE_S0_0, ROLE_S0_1):
        image = decode_image(ctx.request)
        x = image.reshape(1, -1)
        hidden = x @ part["w0"].T + part["b0"]
        hidden = (hidden * (hidden > 0)).astype("float32")
        payload = encode_hidden(int(part["hidden_offset"]), hidden)
        edge = ctx.dependencies.output("stage0-to-stage1")
        _publish_ref(ctx, edge, payload, _role_index(ctx.role))
        print(f"YOLO_2X2_STAGE0 role={ctx.role} hidden={hidden.shape}", flush=True)
        return

    if ctx.role in (ROLE_S1_0, ROLE_S1_1):
        input_edge = ctx.dependencies.input("stage0-to-stage1")
        refs = ctx.ndnsf.wait_for(input_edge.key_scope, input_edge.topic("ref"), 2, 10000)
        if len(refs) < 2:
            ctx.ndnsf.fail("timed out waiting for stage0 activation shards")
            return
        hidden_parts = []
        for ref in refs:
            payload = ctx.ndnsf.fetch_large(ref.payload.decode(), input_edge.key_scope, 10000)
            if payload is None:
                ctx.ndnsf.fail("failed to fetch stage0 activation shard")
                return
            hidden_parts.append(decode_hidden(payload))
        hidden = concat_by_offset(hidden_parts)
        output = hidden @ part["w1"].T + part["b1"]
        out_payload = encode_yolo_output(int(part["output_offset"]), output)
        internal_edge = ctx.dependencies.internal_scope("stage1-internal")

        if ctx.role == ROLE_S1_1:
            _publish_ref(ctx, internal_edge, out_payload, _role_index(ctx.role))
            print(f"YOLO_2X2_STAGE1 role={ctx.role} output={output.shape}", flush=True)
            return

        ref = ctx.ndnsf.wait_one(internal_edge.key_scope,
                                 internal_edge.topic("ref/Stage-1-Shard-1"),
                                 10000)
        if ref is None:
            ctx.ndnsf.fail("timed out waiting for stage1 shard1 output")
            return
        peer_payload = ctx.ndnsf.fetch_large(ref.payload.decode(),
                                             internal_edge.key_scope,
                                             10000)
        if peer_payload is None:
            ctx.ndnsf.fail("failed to fetch stage1 shard1 output")
            return
        final = concat_by_offset([
            decode_yolo_output(out_payload),
            decode_yolo_output(peer_payload),
        ])
        ctx.ndnsf.publish_final_response(encode_yolo_output(0, final))
        print(f"YOLO_2X2_STAGE1_FINAL output={final.shape}", flush=True)
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
