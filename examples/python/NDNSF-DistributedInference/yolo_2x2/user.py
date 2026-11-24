#!/usr/bin/env python3
"""Thin public Runtime host for native YOLO inference and numerical comparison."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import sys
from threading import Event
import time

from ndnsf_distributed_inference.adapters.yolo import build_yolo26n_adapter
from ndnsf_distributed_inference.adapters.yolo.reference import compare_reference, load_reference
from yolo_2x2_lib import (
    DEFAULT_INPUT_SIZE,
    decode_yolo_output,
    encode_native_tensor_bundle,
    optional_local_nfd,
)

_REPO_ROOT = Path(__file__).resolve().parents[4]


def _prepare_yolo_input(args, package):
    reference = load_reference(package, _REPO_ROOT, args.input_size)
    payload = encode_native_tensor_bundle({"images": reference.input_tensor})
    if args.input_payload_file:
        if Path(args.input_payload_file).expanduser().read_bytes() != payload:
            raise ValueError("REGISTERED_FIXTURE_PAYLOAD_MISMATCH")
    return reference, payload


def _emit_native_request_result(record: str) -> None:
    # C++ loggers share this destination and may leave an unfinished line.
    # Frame the result in one write so the collector sees a standalone row.
    encoded = ("\n" + record + "\n").encode("utf-8")
    if os.write(sys.stdout.fileno(), encoded) != len(encoded):
        raise OSError("incomplete native YOLO result record write")


def _load_yolo_native_payload(runtime, args) -> int:
    """Submit the maintained YOLO payload through the native requester.

    The native configuration is the only composition authority in this branch:
    catalog, grant/admission, preparation and split/placement owners are
    constructed by the public C++ Runtime. The tensor bundle is published as an
    encrypted repository object and the bound reference is submitted through
    the native requester; the selected Provider owns fetch/decrypt.
    """
    if not args.native_requester_config:
        raise RuntimeError("native YOLO route requires --native-requester-config")
    if not args.canonical_package or not args.catalogue_registry:
        raise RuntimeError(
            "native YOLO route requires --canonical-package and --catalogue-registry")
    duration = float(args.sequential_duration_s)
    if (args.async_requests < 1 or args.sequential_requests < 0 or
            args.preflight_requests < 0 or args.sequential_interval_ms < 0 or
            not math.isfinite(duration) or duration < 0):
        raise ValueError("native YOLO request counts, duration and interval are invalid")

    package = Path(args.canonical_package).expanduser().resolve()
    registry = Path(args.catalogue_registry).expanduser().resolve()
    adapter = build_yolo26n_adapter(package, registry_path=registry)
    numerical_reference, payload = _prepare_yolo_input(args, package)

    from ndnsf_distributed_inference import api

    user = runtime.user()
    model = user.prepare()
    manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    source = manifest.get("source")
    expected_digest = (
        "sha256:" + str(source.get("checkpointSha256", ""))
        if isinstance(source, dict) else "")
    input_schema = json.loads(model.capabilities.input_schema_json)
    if (model.manifest.model_name != "YOLO26n" or
            model.manifest.model_digest != expected_digest or
            input_schema.get("digest") != adapter.descriptor.input_schema_digest):
        raise RuntimeError("native YOLO requester model identity does not match package")

    options = api.RequestOptions()
    options.timeout_s = args.timeout_ms / 1000
    options.ack_timeout_s = args.ack_timeout_ms / 1000
    options.output_mode = "FULL"
    operator_request = json.loads(
        Path(args.native_requester_config).read_text(encoding="utf-8"))["request"]
    if operator_request.get("placement_strategy"):
        options.placement = runtime.placement_strategy(operator_request["placement_strategy"])
    options.provider_names = operator_request.get("provider_names", [])
    gate_dir = Path(args.native_restart_gate) if args.native_restart_gate else None
    if gate_dir is not None and (
            not args.native_restart_ticket or args.native_restart_after < 2 or
            args.sequential_requests < args.native_restart_after + 2 or duration > 0):
        raise RuntimeError("native restart gate requires a ticket and two requests on each side")
    if not model.manifest.task_name:
        raise RuntimeError("native YOLO runtime has no task identity")

    pending = []

    def submit(index, prefix):
        started = time.monotonic()
        print(f"{prefix}_REQUEST_START index={index}", flush=True)
        reference = user.publish_input(model, list(payload), timeout_s=options.timeout_s)
        handle = user.request(model, api.Input.repository(reference, list(b"{}")), options=options)
        completed = Event()
        completed_at = [None]

        def observe_completion(_error, _result):
            if completed_at[0] is None:
                completed_at[0] = time.monotonic()
            completed.set()

        try:
            completion = handle.on_completion(observe_completion)
        except BaseException:
            handle.cancel()
            raise
        item = (handle, completed, completion, started, index, prefix, completed_at)
        pending.append(item)
        return item

    def finish(item):
        handle, completed, completion, started, index, prefix, completed_at = item
        finished_at = None
        try:
            try:
                ready = completed.wait(options.timeout_s + 1)
                finished_at = completed_at[0] if ready else time.monotonic()
                if not ready:
                    raise TimeoutError("native request did not report completion")
                result = handle.result(timeout_s=0)
            except KeyboardInterrupt:
                handle.cancel()
                _emit_native_request_result(
                    f"{prefix}_REQUEST_INTERRUPTED request={handle.id} "
                    f"status={handle.status_name}")
                raise
            except Exception as exc:
                handle.cancel()
                if finished_at is None:
                    finished_at = time.monotonic()
                _emit_native_request_result(
                    f"{prefix}_REQUEST_RESULT status=false request={handle.id} "
                    f"index={index} elapsed_ms={(finished_at - started) * 1000:.3f} "
                    f"error={exc}")
                return 3
            response_payload = bytes(result.payload)
            try:
                _, actual = decode_yolo_output(response_payload, native_predictions_only=True)
                oracle = compare_reference(numerical_reference, actual)
                matched = bool(oracle["matched"])
                max_diff = oracle["maxAbsError"]
                mean_diff = None
            except (ValueError, KeyError, UnicodeError, OverflowError):
                matched = False
                max_diff = mean_diff = float("inf")
            succeeded = bool(response_payload) and matched
            _emit_native_request_result(
                f"{prefix}_REQUEST_RESULT status={str(succeeded).lower()} "
                f"request={handle.id} payload_bytes={len(response_payload)} "
                f"max_diff={max_diff} mean_diff={mean_diff} index={index} "
                f"elapsed_ms={(finished_at - started) * 1000:.3f}")
            return 0 if succeeded else 4
        finally:
            try:
                completion.cancel()
            finally:
                pending.remove(item)

    def restart_boundary(request_index):
        if gate_dir is not None and request_index + 1 == args.native_restart_after:
            # No request is active here; the harness may restart Providers while
            # this process retains its native runtime and published fixed source.
            marker = {"ticket": args.native_restart_ticket, "pid": os.getpid(),
                      "completed": request_index + 1}
            ready = gate_dir / "ready.json"
            temporary = gate_dir / "ready.tmp"
            temporary.write_text(json.dumps(marker), encoding="utf-8")
            temporary.replace(ready)
            print("YOLO_NATIVE_RESTART_GATE_READY " + json.dumps(marker), flush=True)
            deadline = time.monotonic() + 600
            release = gate_dir / "release.json"
            while not release.is_file():
                if time.monotonic() >= deadline:
                    raise RuntimeError("native Provider restart gate timed out")
                time.sleep(0.05)
            if json.loads(release.read_text(encoding="utf-8")) != marker:
                raise RuntimeError("native Provider restart gate release does not match")
            print("YOLO_NATIVE_RESTART_GATE_RELEASED " + json.dumps(marker), flush=True)

    try:
        for index in range(args.preflight_requests):
            code = finish(submit(index, "YOLO_NATIVE_PREFLIGHT"))
            if code:
                return code
        if args.sequential_requests or duration > 0:
            deadline = time.monotonic() + duration if duration > 0 else None
            index = 0
            while ((deadline is not None and time.monotonic() < deadline) or
                   (deadline is None and index < args.sequential_requests)):
                item = submit(index, "YOLO_NATIVE")
                code = finish(item)
                if code:
                    return code
                restart_boundary(index)
                index += 1
                delay = item[3] + args.sequential_interval_ms / 1000 - time.monotonic()
                if deadline is not None:
                    delay = min(delay, max(0, deadline - time.monotonic()))
                if delay > 0:
                    time.sleep(delay)
        else:
            batch = [submit(index, "YOLO_NATIVE") for index in range(args.async_requests)]
            for item in batch:
                code = finish(item)
                if code:
                    return code
        return 0
    finally:
        active_failure = sys.exc_info()[0] is not None
        cleanup_errors = []
        for handle, _event, subscription, _started, _index, _prefix, _finished in pending:
            for release in (handle.cancel, subscription.cancel):
                try:
                    release()
                except Exception as exc:
                    cleanup_errors.append(str(exc))
        if cleanup_errors:
            _emit_native_request_result(
                "YOLO_NATIVE_CLEANUP_FAILED errors=" + repr(cleanup_errors))
            if not active_failure:
                raise RuntimeError("native YOLO request cleanup failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-requester-config", required=True,
                        help="operator-pinned native requester configuration")
    parser.add_argument("--canonical-package", required=True,
                        help="signed YOLO canonical package directory")
    parser.add_argument("--catalogue-registry", required=True,
                        help="catalogue trust-root registry JSON")
    parser.add_argument("--input-size", type=int, default=DEFAULT_INPUT_SIZE)
    parser.add_argument("--input-payload-file", default="",
                        help="optional exact registered TensorBundle bytes")
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--async-requests", type=int, default=1)
    parser.add_argument("--sequential-requests", type=int, default=0)
    parser.add_argument("--sequential-duration-s", type=float, default=0.0)
    parser.add_argument("--sequential-interval-ms", type=int, default=0)
    parser.add_argument("--preflight-requests", type=int, default=0)
    parser.add_argument("--native-restart-gate", default="")
    parser.add_argument("--native-restart-after", type=int, default=0)
    parser.add_argument("--native-restart-ticket", default="")
    parser.add_argument("--start-local-nfd", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.dry_run:
        print("Native YOLO config:", args.native_requester_config)
        return 0

    from ndnsf_distributed_inference import api

    with optional_local_nfd(args.start_local_nfd):
        config = api.RuntimeConfig()
        config.native_config_path = str(Path(args.native_requester_config).resolve())
        runtime = api.Runtime.open(config)
        try:
            if os.environ.get("NDNSF_DI_INIT_TRACE") == "1":
                print("NDNSF_DI_INIT_TRACE stage=user_after_runtime", flush=True)
            return _load_yolo_native_payload(runtime, args)
        finally:
            active_failure = sys.exc_info()[0] is not None
            errors = []
            try:
                runtime.close()
            except Exception as error:
                errors.append("close: " + str(error))
            try:
                if not runtime.drain(timeout_s=6):
                    errors.append("native YOLO requester did not drain")
            except Exception as error:
                errors.append("drain: " + str(error))
            if errors:
                _emit_native_request_result("YOLO_NATIVE_CLEANUP_FAILED errors=" + repr(errors))
                if not active_failure:
                    raise RuntimeError("native YOLO Runtime cleanup failed")
            else:
                _emit_native_request_result("YOLO_NATIVE_REQUESTER_DRAINED")


if __name__ == "__main__":
    raise SystemExit(main())
