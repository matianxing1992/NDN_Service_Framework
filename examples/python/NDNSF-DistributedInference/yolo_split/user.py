#!/usr/bin/env python3
"""YOLO split-inference user built on NDNSF-DistributedInference."""

from __future__ import annotations

import argparse
from pathlib import Path
import tempfile

from ndnsf_distributed_inference import (
    DistributedInferencePlan,
    APPClient,
    InferencePlanBuilder,
)
from ndnsf_distributed_inference.backends.onnxruntime import runtime_spec

from yolo_split_lib import (
    CONFIG_FILE,
    DEFAULT_INPUT_SIZE,
    ROLE_STAGE0,
    ROLE_STAGE1,
    SERVICE,
    decode_initial_request,
    decode_onnx_output,
    encode_initial_request,
    export_yolo_split_onnx,
    full_forward,
    load_yolo_model,
    make_input,
    optional_local_nfd,
    yolo_splitter_output,
)


def build_plan(deployment, model_name: str, exported: dict,
               input_size: int) -> DistributedInferencePlan:
    runtime = runtime_spec()
    split = int(exported["split"])
    saved_indices = [int(value) for value in exported["saved_indices"]]
    common_metadata = {
        "model": model_name,
        "split": split,
        "saved_indices": saved_indices,
        "input_size": input_size,
    }
    split_service = yolo_splitter_output(exported).service(SERVICE)
    builder = InferencePlanBuilder.for_service(
        deployment,
        SERVICE,
        runtime=runtime,
    ).metadata(**common_metadata)
    stage0_artifact = split_service.artifact_for_role(ROLE_STAGE0)
    stage1_artifact = split_service.artifact_for_role(ROLE_STAGE1)
    builder.add_part(
        role=ROLE_STAGE0,
        artifact_name=stage0_artifact.artifact_name,
        model=Path(stage0_artifact.path).read_bytes(),
        filename=stage0_artifact.resolved_filename(),
        kind=stage0_artifact.kind,
        metadata=stage0_artifact.metadata,
    ).add_part(
        role=ROLE_STAGE1,
        artifact_name=stage1_artifact.artifact_name,
        model=Path(stage1_artifact.path).read_bytes(),
        filename=stage1_artifact.resolved_filename(),
        kind=stage1_artifact.kind,
        metadata=stage1_artifact.metadata,
    )
    return builder.build()


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
        model_name, model = load_yolo_model(args.model)
        x = make_input(args.input_size)
        request_payload = encode_initial_request(x)
        expected = full_forward(model, decode_initial_request(request_payload))

        with tempfile.TemporaryDirectory(prefix="ndnsf-yolo-onnx-export-") as export_dir:
            exported = export_yolo_split_onnx(args.model, export_dir, args.input_size)
            plan = build_plan(client.deployment, model_name, exported, args.input_size)
            result = client.infer(
                plan,
                request_payload,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=args.timeout_ms,
            )

        if not result.status:
            print("YOLO_SPLIT_RESULT status=false error=", result.error, sep="")
            return 2

        actual_np = decode_onnx_output(result.payload)
        expected_np = expected.numpy()
        diff = abs(expected_np - actual_np)
        max_diff = float(diff.max())
        mean_diff = float(diff.mean())
        ok = max_diff < 1e-3
        print(
            "YOLO_SPLIT_RESULT "
            f"status=true backend=onnxruntime model={model_name} "
            f"input={args.input_size}x{args.input_size} "
            f"shape={tuple(actual_np.shape)} max_abs_diff={max_diff:.8f} "
            f"mean_abs_diff={mean_diff:.8f} ok={str(ok).lower()}"
        )
        return 0 if ok else 3


if __name__ == "__main__":
    raise SystemExit(main())
