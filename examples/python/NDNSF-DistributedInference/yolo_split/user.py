#!/usr/bin/env python3
"""YOLO split-inference user built on NDNSF-DistributedInference."""

from __future__ import annotations

import argparse

from ndnsf_distributed_inference import (
    APPClient,
)

from yolo_split_lib import (
    CONFIG_FILE,
    DEFAULT_INPUT_SIZE,
    ROLE_STAGE0,
    ROLE_STAGE1,
    SERVICE,
    decode_initial_request,
    decode_onnx_output,
    encode_initial_request,
    full_forward,
    full_forward_from_policy_onnx,
    load_yolo_model,
    make_input,
    optional_local_nfd,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="yolo26n.pt")
    parser.add_argument("--input-size", type=int, default=DEFAULT_INPUT_SIZE)
    parser.add_argument("--group", default="",
                        help="Override SVS group from the policy config")
    parser.add_argument("--ack-timeout-ms", type=int, default=500)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--config", default=CONFIG_FILE,
                        help="User-facing distributed inference policy config")
    parser.add_argument("--generated-policy-dir", default="/tmp/ndnsf-di-yolo-policy",
                        help="Directory for generated trust schema and policy files")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--start-local-nfd", action="store_true")
    args = parser.parse_args()

    if args.dry_run:
        print("Run YOLO distributed inference user")
        print("config:", args.config)
        print("service:", SERVICE)
        print(f"roles: {ROLE_STAGE0}, {ROLE_STAGE1}")
        return 0

    with optional_local_nfd(args.start_local_nfd):
        client = APPClient.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
            group=args.group,
            permission_wait_ms=args.permission_wait_ms,
            adaptive_admission=False,
        )
        x = make_input(args.input_size)
        client.register_input_encoder(SERVICE, encode_initial_request)
        request_payload = client.encode_input(SERVICE, x)
        expected_np = full_forward_from_policy_onnx(args.config, decode_initial_request(request_payload))
        if expected_np is None:
            model_name, model = load_yolo_model(args.model)
            expected_np = full_forward(model, decode_initial_request(request_payload)).numpy()
            expected_source = model_name
        else:
            expected_source = "policy-full-onnx"

        result = client.distributed_inference(
            SERVICE,
            request_payload,
            ack_timeout_ms=args.ack_timeout_ms,
            timeout_ms=args.timeout_ms,
        )

        if not result.status:
            print("YOLO_SPLIT_RESULT status=false error=", result.error, sep="")
            return 2

        actual_np = decode_onnx_output(result.payload)
        diff = abs(expected_np - actual_np)
        max_diff = float(diff.max())
        mean_diff = float(diff.mean())
        ok = max_diff < 1e-3
        print(
            "YOLO_SPLIT_RESULT "
            f"status=true backend=onnxruntime expected={expected_source} "
            f"input={args.input_size}x{args.input_size} "
            f"shape={tuple(actual_np.shape)} max_abs_diff={max_diff:.8f} "
            f"mean_abs_diff={mean_diff:.8f} ok={str(ok).lower()}"
        )
        return 0 if ok else 3


if __name__ == "__main__":
    raise SystemExit(main())
