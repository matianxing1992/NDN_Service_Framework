#!/usr/bin/env python3
"""Provider for the PyTorch eager 2x2 distributed inference example."""

from __future__ import annotations

from ndnsf_distributed_inference import APPProvider, ProviderRuntimeContext

from pytorch_2x2_lib import (
    ROLE_S0_0,
    ROLE_S0_1,
    ROLE_S1_0,
    ROLE_S1_1,
    SERVICE,
    decode_hidden,
    decode_input,
    decode_output,
    encode_hidden,
    encode_output,
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
    part = load_part(ctx.execution.path("model"))

    if ctx.role in (ROLE_S0_0, ROLE_S0_1):
        x = decode_input(ctx.request)
        hidden = (x @ part["w0"].T + part["b0"]).relu()
        payload = encode_hidden(int(part["hidden_offset"]), hidden)
        edge = ctx.dependencies.output("stage0-to-stage1")
        _publish_ref(ctx, edge, payload, _role_index(ctx.role))
        print(f"PYTORCH_STAGE0 role={ctx.role} hidden={tuple(hidden.shape)}", flush=True)
        return

    if ctx.role in (ROLE_S1_0, ROLE_S1_1):
        input_edge = ctx.dependencies.input("stage0-to-stage1")
        refs = ctx.ndnsf.wait_for(input_edge.key_scope, input_edge.topic("ref"), 2, 10000)
        if len(refs) < 2:
            ctx.ndnsf.fail("timed out waiting for stage0 hidden shards")
            return
        hidden_parts = []
        for ref in refs:
            payload = ctx.ndnsf.fetch_large(ref.payload.decode(), input_edge.key_scope, 10000)
            if payload is None:
                ctx.ndnsf.fail("failed to fetch hidden shard")
                return
            hidden_parts.append(decode_hidden(payload))
        hidden = torch_cat_by_offset(hidden_parts)
        output = hidden @ part["w1"].T + part["b1"]
        out_payload = encode_output(int(part["output_offset"]), output)
        internal_edge = ctx.dependencies.internal_scope("stage1-internal")
        if ctx.role == ROLE_S1_1:
            _publish_ref(ctx, internal_edge, out_payload, _role_index(ctx.role))
            print(f"PYTORCH_STAGE1 role={ctx.role} output={tuple(output.shape)}", flush=True)
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
        final = torch_cat_by_offset([
            decode_output(out_payload),
            decode_output(peer_payload),
        ])
        ctx.ndnsf.publish_final_response(encode_output(0, final))
        print(f"PYTORCH_STAGE1_FINAL output={tuple(final.shape)}", flush=True)
        return

    ctx.ndnsf.fail(f"unsupported role {ctx.role}")


def torch_cat_by_offset(items):
    import torch

    ordered = [value for _, value in sorted(items, key=lambda item: item[0])]
    return torch.cat(ordered, dim=1)


def main() -> int:
    parser = parse_args_with_common("Run PyTorch eager 2x2 provider")
    parser.add_argument("--role", choices=[ROLE_S0_0, ROLE_S0_1, ROLE_S1_0, ROLE_S1_1],
                        default="")
    parser.add_argument("--roles", default="all")
    parser.add_argument("--provider-id", default="")
    parser.add_argument("--temp-dir", default="")
    parser.add_argument("--handler-workers", type=int, default=2)
    args = parser.parse_args()
    if args.dry_run:
        print("Run PyTorch eager 2x2 provider", args.provider_id, args.role or args.roles)
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
            backends=["pytorch-eager"],
            temp_dir=args.temp_dir or None,
            has_model=False,
            can_provision=True,
        )
        provider.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
