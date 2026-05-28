#!/usr/bin/env python3
"""User for the PyTorch eager 2x2 distributed inference example."""

from __future__ import annotations

import tempfile
from pathlib import Path

from ndnsf_distributed_inference import APPClient, InferencePlanBuilder, RuntimeSpec

from pytorch_2x2_lib import (
    SERVICE,
    decode_output,
    encode_input,
    full_forward,
    make_full_model,
    make_input,
    optional_local_nfd,
    parse_args_with_common,
    pytorch_splitter_output,
    split_model,
)


def build_plan(deployment, split) :
    service = pytorch_splitter_output(split).service(SERVICE)
    builder = InferencePlanBuilder.for_service(
        deployment,
        SERVICE,
        runtime=RuntimeSpec(
            name="/Runtime/PyTorch/Eager",
            backend="pytorch-eager",
            entrypoint="python",
        ),
        backend="pytorch-eager",
    )
    for artifact in service.artifacts:
        builder.add_part(
            role=artifact.role,
            artifact_name=artifact.artifact_name,
            model=Path(artifact.path).read_bytes(),
            filename=artifact.resolved_filename(),
            kind=artifact.kind,
            backend=artifact.backend,
            metadata=artifact.metadata,
        )
    return builder.build()


def main() -> int:
    parser = parse_args_with_common("Run PyTorch eager 2x2 user")
    parser.add_argument("--ack-timeout-ms", type=int, default=500)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--async-requests", type=int, default=1)
    args = parser.parse_args()
    if args.dry_run:
        print("Run PyTorch eager 2x2 user")
        print("config:", args.config)
        return 0

    with optional_local_nfd(args.start_local_nfd):
        client = APPClient.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
            group=args.group,
            permission_wait_ms=args.permission_wait_ms,
            adaptive_admission=False,
            async_workers=max(1, args.async_requests),
        )
        with tempfile.TemporaryDirectory(prefix="ndnsf-pytorch-2x2-") as tmp:
            split = split_model(tmp)
            plan = build_plan(client.deployment, split)
            x = make_input()
            expected = full_forward(make_full_model(), x)
            payload = encode_input(x)
            futures = [
                client.infer_async(
                    plan,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                )
                for _ in range(args.async_requests)
            ]
            ok = True
            for index, future in enumerate(futures):
                result = future.result(timeout=args.timeout_ms / 1000 + 10)
                if not result.status:
                    print(f"PYTORCH_2X2_RESULT index={index} status=false error={result.error}")
                    ok = False
                    continue
                _, actual = decode_output(result.payload)
                diff = (actual - expected).abs()
                max_diff = float(diff.max())
                mean_diff = float(diff.mean())
                item_ok = max_diff < 1e-5
                ok = ok and item_ok
                print(
                    "PYTORCH_2X2_RESULT "
                    f"index={index} status=true shape={tuple(actual.shape)} "
                    f"max_abs_diff={max_diff:.8f} mean_abs_diff={mean_diff:.8f} "
                    f"ok={str(item_ok).lower()}"
                )
            client.shutdown()
            return 0 if ok else 3


if __name__ == "__main__":
    raise SystemExit(main())
