#!/usr/bin/env python3
"""User for the real YOLO layout distributed inference example."""

from __future__ import annotations

from ndnsf_distributed_inference import APPClient
from pathlib import Path
import os
import time

from yolo_2x2_lib import (
    DEFAULT_MODEL,
    DEFAULT_INPUT_SIZE,
    YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
    YOLO_PARALLEL_OUTPUT_SEMANTICS,
    compare_yolo_outputs,
    decode_yolo_output,
    decode_image,
    encode_native_tensor_bundle,
    full_forward,
    make_input,
    optional_local_nfd,
    parse_args_with_common,
    run_local_onnx_pipeline,
    run_local_parallel_detect_scale_pipeline,
    run_local_parallel_output_pipeline,
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
    parser.add_argument("--sequential-duration-s", type=float, default=0.0,
                        help="Run sequential requests for this many seconds; 0 disables duration mode")
    parser.add_argument("--sequential-interval-ms", type=int, default=0,
                        help="Minimum interval between sequential request starts")
    parser.add_argument("--preflight-requests", type=int, default=0,
                        help="Warm the deployed plan/session before measured requests")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--input-size", type=int, default=DEFAULT_INPUT_SIZE)
    parser.add_argument("--native-tensor-input", action="store_true",
                        help="publish request input as an NDNSF-DI native tensor bundle")
    args = parser.parse_args()
    if args.dry_run:
        print("Run YOLO 2x2 user")
        print("config:", args.config)
        return 0

    with optional_local_nfd(args.start_local_nfd):
        trace_init = os.environ.get("NDNSF_DI_INIT_TRACE") == "1"
        client = APPClient.from_config(
            args.config,
            generated_policy_dir=args.generated_policy_dir,
            group=args.group,
            permission_wait_ms=args.permission_wait_ms,
            adaptive_admission=False,
            async_workers=max(1, args.async_requests),
        )
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_client", flush=True)
        service = yolo_inference_service(client.deployment)
        service_policy = client.deployment.service_policy(service)
        metadata = service_policy.metadata or {}
        layout = str(metadata.get("layout", "2x2"))
        layout_semantics = str(metadata.get("layout_semantics", ""))
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_before_make_input", flush=True)
        image = make_input(args.input_size)
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_make_input", flush=True)
        reference_image_payload = client.encode_input(service, image)
        if trace_init:
            print(
                "NDNSF_DI_INIT_TRACE "
                f"stage=user_after_encode_input bytes={len(reference_image_payload)}",
                flush=True,
            )
        image_payload = (
            encode_native_tensor_bundle({"images": image})
            if args.native_tensor_input else
            reference_image_payload
        )
        if trace_init:
            print(
                "NDNSF_DI_INIT_TRACE "
                f"stage=user_before_publish_input bytes={len(image_payload)}",
                flush=True,
            )
        payload = client.publish_large_payload_reference(
            service,
            image_payload,
            object_label="inference-input-image",
            object_type="application/x-ndnsf-di-input+npz",
            freshness_ms=120000,
        )
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_publish_input", flush=True)
        inference_image = decode_image(reference_image_payload)
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_decode_input", flush=True)
        artifact_paths = {
            artifact.role: artifact.path
            for artifact in service_policy.artifacts
            if getattr(artifact, "path", "")
        }
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_before_expected", flush=True)
        if artifact_paths and all(Path(path).exists() for path in artifact_paths.values()):
            if layout_semantics == YOLO_PARALLEL_DETECT_SCALE_SEMANTICS:
                expected = run_local_parallel_detect_scale_pipeline(
                    artifact_paths,
                    inference_image,
                    layout,
                )
            elif layout_semantics == YOLO_PARALLEL_OUTPUT_SEMANTICS:
                expected = run_local_parallel_output_pipeline(
                    artifact_paths,
                    inference_image,
                    layout,
                )
            else:
                expected = run_local_onnx_pipeline(
                    artifact_paths,
                    inference_image,
                    service_policy.roles,
                )
        else:
            expected = full_forward(args.model, inference_image)
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_expected", flush=True)
        duration_s = max(0.0, float(args.sequential_duration_s or 0.0))
        interval_s = max(0.0, float(args.sequential_interval_ms or 0) / 1000.0)
        request_count = args.sequential_requests or args.async_requests
        if duration_s > 0:
            request_count = max(1, int(duration_s / max(interval_s, 0.001)))
        dynamic_provisioning = None
        if args.deployed_models:
            dynamic_provisioning = False
        elif args.dynamic_provisioning or args.repo_manifest_file:
            dynamic_provisioning = True
        plan_session = None
        if dynamic_provisioning:
            plan = client.service_plan(
                service,
                runtime=runtime_spec(),
                artifact_references=args.repo_manifest_file or None,
            )
            plan_session = client.deploy_plan(plan, freshness_ms=120000)

        def invoke_once():
            if plan_session is not None:
                return client.invoke_plan(
                    plan_session,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                )
            return client.distributed_inference(
                service,
                payload,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=args.timeout_ms,
                dynamic_provisioning=False,
                runtime=runtime_spec(),
                artifact_references=args.repo_manifest_file or None,
            )

        preflight_requests = max(0, int(args.preflight_requests or 0))
        for index in range(preflight_requests):
            started = time.perf_counter()
            epoch_started = time.time()
            if plan_session is not None:
                preflight = client.preflight_plan(
                    plan_session,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                )
            else:
                preflight = client.distributed_inference(
                    service,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                    dynamic_provisioning=False,
                    runtime=runtime_spec(),
                    artifact_references=args.repo_manifest_file or None,
                )
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            epoch_finished = time.time()
            print(
                "YOLO_LAYOUT_PREFLIGHT "
                f"layout={layout} index={index} "
                f"epoch_start_s={epoch_started:.6f} "
                f"epoch_end_s={epoch_finished:.6f} "
                f"status={str(preflight.status).lower()} "
                f"elapsed_ms={elapsed_ms:.2f} "
                f"error={preflight.error}"
            )
            if not preflight.status:
                client.shutdown()
                return 4

        if args.sequential_requests or duration_s > 0:
            futures = []
            run_deadline = time.perf_counter() + duration_s if duration_s > 0 else None
            index = 0
            while True:
                if run_deadline is not None and time.perf_counter() >= run_deadline:
                    break
                if run_deadline is None and index >= request_count:
                    break
                started = time.perf_counter()
                epoch_started = time.time()
                result = invoke_once()
                futures.append(_TimedFuture(_ImmediateResult(result), started, time.perf_counter(),
                                            epoch_started, time.time()))
                index += 1
                if interval_s > 0:
                    next_start = started + interval_s
                    delay = next_start - time.perf_counter()
                    if delay > 0:
                        if run_deadline is not None:
                            delay = min(delay, max(0.0, run_deadline - time.perf_counter()))
                        time.sleep(delay)
        else:
            futures = []
            for _ in range(request_count):
                started = time.perf_counter()
                epoch_started = time.time()
                if plan_session is not None:
                    future = client.invoke_plan_async(
                        plan_session,
                        payload,
                        ack_timeout_ms=args.ack_timeout_ms,
                        timeout_ms=args.timeout_ms,
                    )
                else:
                    future = client.async_distributed_inference(
                        service,
                        payload,
                        ack_timeout_ms=args.ack_timeout_ms,
                        timeout_ms=args.timeout_ms,
                        dynamic_provisioning=False,
                        runtime=runtime_spec(),
                        artifact_references=args.repo_manifest_file or None,
                    )
                futures.append(_TimedFuture(future, started, None, epoch_started, None))
        ok = True
        for index, timed in enumerate(futures):
            result = timed.future.result(timeout=args.timeout_ms / 1000 + 10)
            finished = timed.finished if timed.finished is not None else time.perf_counter()
            epoch_finished = timed.epoch_finished if timed.epoch_finished is not None else time.time()
            elapsed_ms = (finished - timed.started) * 1000.0
            if not result.status:
                print(
                    f"YOLO_LAYOUT_RESULT layout={layout} index={index} "
                    f"epoch_start_s={timed.epoch_started:.6f} "
                    f"epoch_end_s={epoch_finished:.6f} "
                    f"status=false inference_elapsed_ms={elapsed_ms:.2f} error={result.error}"
                )
                if layout == "2x2":
                    print(
                        f"YOLO_2X2_RESULT index={index} status=false "
                        f"epoch_start_s={timed.epoch_started:.6f} "
                        f"epoch_end_s={epoch_finished:.6f} "
                        f"inference_elapsed_ms={elapsed_ms:.2f} error={result.error}"
                    )
                ok = False
                continue
            _, actual = decode_yolo_output(result.payload)
            atol = 1e-3
            rtol = 1e-4
            item_ok, max_diff, mean_diff = compare_yolo_outputs(
                actual,
                expected,
                atol=atol,
                rtol=rtol,
            )
            ok = ok and item_ok
            print(
                "YOLO_LAYOUT_RESULT "
                f"layout={layout} "
                f"index={index} "
                f"epoch_start_s={timed.epoch_started:.6f} "
                f"epoch_end_s={epoch_finished:.6f} "
                f"status=true shape={actual.shape} "
                f"max_abs_diff={max_diff:.8f} mean_abs_diff={mean_diff:.8f} "
                f"atol={atol:.1e} rtol={rtol:.1e} "
                f"inference_elapsed_ms={elapsed_ms:.2f} "
                f"ok={str(item_ok).lower()}"
            )
            if layout == "2x2":
                print(
                    "YOLO_2X2_RESULT "
                    f"index={index} "
                    f"epoch_start_s={timed.epoch_started:.6f} "
                    f"epoch_end_s={epoch_finished:.6f} "
                    f"status=true shape={actual.shape} "
                    f"max_abs_diff={max_diff:.8f} mean_abs_diff={mean_diff:.8f} "
                    f"atol={atol:.1e} rtol={rtol:.1e} "
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
    def __init__(self, future, started: float, finished: float | None,
                 epoch_started: float, epoch_finished: float | None):
        self.future = future
        self.started = started
        self.finished = finished
        self.epoch_started = epoch_started
        self.epoch_finished = epoch_finished


if __name__ == "__main__":
    raise SystemExit(main())
