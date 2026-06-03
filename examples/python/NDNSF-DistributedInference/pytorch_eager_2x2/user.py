#!/usr/bin/env python3
"""User for the PyTorch-defined fully connected ONNX 2x2 example."""

from __future__ import annotations

import numpy as np

from ndnsf_distributed_inference import APPClient

from pytorch_2x2_lib import (
    SERVICE,
    decode_output,
    encode_input,
    full_forward,
    make_full_model,
    make_input,
    optional_local_nfd,
    parse_args_with_common,
)


def main() -> int:
    parser = parse_args_with_common("Run PyTorch-defined ONNX 2x2 user")
    parser.add_argument("--ack-timeout-ms", type=int, default=500)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--async-requests", type=int, default=1)
    args = parser.parse_args()
    if args.dry_run:
        print("Run PyTorch-defined ONNX 2x2 user")
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
        x = make_input()
        expected = full_forward(make_full_model(), x)
        client.register_input_encoder(SERVICE, encode_input)
        payload = client.encode_input(SERVICE, x)
        ok = True
        if args.async_requests <= 1:
            results = [client.distributed_inference(
                SERVICE,
                payload,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=args.timeout_ms,
                dynamic_provisioning=False,
            )]
        else:
            futures = [
                client.async_distributed_inference(
                    SERVICE,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                    dynamic_provisioning=False,
                )
                for _ in range(args.async_requests)
            ]
            results = [
                future.result(timeout=args.timeout_ms / 1000 + 10)
                for future in futures
            ]
        for index, result in enumerate(results):
            if not result.status:
                print(f"PYTORCH_2X2_RESULT index={index} status=false error={result.error}")
                ok = False
                continue
            _, actual = decode_output(result.payload)
            diff = np.abs(actual - expected)
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
