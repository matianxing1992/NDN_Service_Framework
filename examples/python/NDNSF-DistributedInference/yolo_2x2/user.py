#!/usr/bin/env python3
"""User for the real YOLO layout distributed inference example."""

from __future__ import annotations

from ndnsf_distributed_inference import APPClient
from pathlib import Path
import time

from yolo_2x2_lib import (
    DEFAULT_MODEL,
    DEFAULT_INPUT_SIZE,
    decode_yolo_output,
    decode_image,
    full_forward,
    make_input,
    optional_local_nfd,
    parse_args_with_common,
    run_local_onnx_pipeline,
    runtime_spec,
    yolo_inference_service,
)


def main() -> int:
    parser = parse_args_with_common("Run YOLO 2x2 user")
    parser.add_argument("--ack-timeout-ms", type=int, default=500)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--async-requests", type=int, default=1)
    parser.add_argument("--dynamic-provisioning", action="store_true",
                        help="kept for older commands; service invocation now provisions dynamically by default")
    parser.add_argument("--deployed-models", action="store_true",
                        help="use providers that already have local model shards")
    parser.add_argument(
        "--repo-manifest-file",
        default="",
        help="artifact reference manifest produced by the repo-backed deployer",
    )
    parser.add_argument("--sequential-requests", type=int, default=0)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--input-size", type=int, default=DEFAULT_INPUT_SIZE)
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
        service = yolo_inference_service(client.deployment)
        service_policy = client.deployment.service_policy(service)
        layout = str((service_policy.metadata or {}).get("layout", "2x2"))
        image = make_input(args.input_size)
        image_payload = client.encode_input(service, image)
        payload = client.publish_large_payload_reference(
            service,
            image_payload,
            object_label="inference-input-image",
            object_type="application/x-ndnsf-di-input+npz",
            freshness_ms=120000,
        )
        inference_image = decode_image(image_payload)
        artifact_paths = {
            artifact.role: artifact.path
            for artifact in service_policy.artifacts
            if getattr(artifact, "path", "")
        }
        if artifact_paths and all(Path(path).exists() for path in artifact_paths.values()):
            expected = run_local_onnx_pipeline(
                artifact_paths,
                inference_image,
                service_policy.roles,
            )
        else:
            expected = full_forward(args.model, inference_image)
        request_count = args.sequential_requests or args.async_requests
        dynamic_provisioning = None
        if args.deployed_models:
            dynamic_provisioning = False
        elif args.dynamic_provisioning or args.repo_manifest_file:
            dynamic_provisioning = True
        if args.sequential_requests:
            futures = []
            for _ in range(request_count):
                started = time.perf_counter()
                result = client.distributed_inference(
                    service,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                    dynamic_provisioning=dynamic_provisioning,
                    runtime=runtime_spec(),
                    artifact_references=args.repo_manifest_file or None,
                )
                futures.append(_TimedFuture(_ImmediateResult(result), started, time.perf_counter()))
        else:
            futures = []
            for _ in range(request_count):
                started = time.perf_counter()
                future = client.async_distributed_inference(
                    service,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                    dynamic_provisioning=dynamic_provisioning,
                    runtime=runtime_spec(),
                    artifact_references=args.repo_manifest_file or None,
                )
                futures.append(_TimedFuture(future, started, None))
        ok = True
        for index, timed in enumerate(futures):
            result = timed.future.result(timeout=args.timeout_ms / 1000 + 10)
            finished = timed.finished if timed.finished is not None else time.perf_counter()
            elapsed_ms = (finished - timed.started) * 1000.0
            if not result.status:
                print(
                    f"YOLO_LAYOUT_RESULT layout={layout} index={index} "
                    f"status=false inference_elapsed_ms={elapsed_ms:.2f} error={result.error}"
                )
                if layout == "2x2":
                    print(
                        f"YOLO_2X2_RESULT index={index} status=false "
                        f"inference_elapsed_ms={elapsed_ms:.2f} error={result.error}"
                    )
                ok = False
                continue
            _, actual = decode_yolo_output(result.payload)
            diff = abs(actual - expected)
            max_diff = float(diff.max())
            mean_diff = float(diff.mean())
            item_ok = max_diff < 1e-6
            ok = ok and item_ok
            print(
                "YOLO_LAYOUT_RESULT "
                f"layout={layout} "
                f"index={index} status=true shape={actual.shape} "
                f"max_abs_diff={max_diff:.8f} mean_abs_diff={mean_diff:.8f} "
                f"inference_elapsed_ms={elapsed_ms:.2f} "
                f"ok={str(item_ok).lower()}"
            )
            if layout == "2x2":
                print(
                    "YOLO_2X2_RESULT "
                    f"index={index} status=true shape={actual.shape} "
                    f"max_abs_diff={max_diff:.8f} mean_abs_diff={mean_diff:.8f} "
                    f"inference_elapsed_ms={elapsed_ms:.2f} "
                    f"ok={str(item_ok).lower()}"
                )
        client.shutdown()
        return 0 if ok else 3


class _ImmediateResult:
    def __init__(self, value):
        self._value = value

    def result(self, timeout=None):
        return self._value


class _TimedFuture:
    def __init__(self, future, started: float, finished: float | None):
        self.future = future
        self.started = started
        self.finished = finished


if __name__ == "__main__":
    raise SystemExit(main())
