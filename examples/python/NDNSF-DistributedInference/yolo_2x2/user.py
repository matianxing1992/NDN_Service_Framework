#!/usr/bin/env python3
"""User for the YOLO-style 2x2 distributed inference example."""

from __future__ import annotations

from ndnsf_distributed_inference import APPClient

from yolo_2x2_lib import (
    SERVICE,
    build_dynamic_plan,
    build_repo_plan,
    decode_yolo_output,
    encode_image_for_yolo,
    full_forward,
    make_full_model,
    make_input,
    optional_local_nfd,
    parse_args_with_common,
)


def main() -> int:
    parser = parse_args_with_common("Run YOLO 2x2 user")
    parser.add_argument("--ack-timeout-ms", type=int, default=500)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--async-requests", type=int, default=1)
    parser.add_argument("--dynamic-provisioning", action="store_true")
    parser.add_argument("--repo-manifest-file", default="")
    parser.add_argument("--sequential-requests", type=int, default=0)
    args = parser.parse_args()
    if args.dry_run:
        print("Run YOLO 2x2 user")
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
        image = make_input()
        expected = full_forward(make_full_model(), image)
        client.register_input_encoder(SERVICE, encode_image_for_yolo)
        payload = client.encode_input(SERVICE, image)
        if args.repo_manifest_file:
            plan = build_repo_plan(client, args.repo_manifest_file)
        elif args.dynamic_provisioning:
            plan = build_dynamic_plan(client)
        else:
            plan = None
        request_count = args.sequential_requests or args.async_requests
        if plan is not None:
            futures = []
            for _ in range(request_count):
                futures.append(_ImmediateResult(client.infer(
                    plan,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                )))
        else:
            futures = [
                client.infer_service_async(
                    SERVICE,
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
                print(f"YOLO_2X2_RESULT index={index} status=false error={result.error}")
                ok = False
                continue
            _, actual = decode_yolo_output(result.payload)
            diff = abs(actual - expected)
            max_diff = float(diff.max())
            mean_diff = float(diff.mean())
            item_ok = max_diff < 1e-6
            ok = ok and item_ok
            print(
                "YOLO_2X2_RESULT "
                f"index={index} status=true shape={actual.shape} "
                f"max_abs_diff={max_diff:.8f} mean_abs_diff={mean_diff:.8f} "
                f"ok={str(item_ok).lower()}"
            )
        client.shutdown()
        return 0 if ok else 3


class _ImmediateResult:
    def __init__(self, value):
        self._value = value

    def result(self, timeout=None):
        return self._value


if __name__ == "__main__":
    raise SystemExit(main())
