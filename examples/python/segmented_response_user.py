#!/usr/bin/env python3
"""Issue a deterministic response-size sequence through one NDNSF user."""

from __future__ import annotations

import argparse
import json
import threading
import time
from pathlib import Path
from typing import List, Optional

from common import add_process_arguments, optional_local_nfd, session_kwargs
from ndnsf import ServiceUser
from spec112_segmented_common import (
    MAX_SEQUENCE_REQUESTS,
    decode_request,
    encode_request,
    ndn_name,
    positive_int,
    response_payload,
    run_identity,
)


expected_payload = response_payload


def nonnegative_int(value: str) -> int:
    try:
        number = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc
    if number < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return number


def positive_float(value: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if number <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def parse_sizes(value: str) -> list[int]:
    sizes: list[int] = []
    for raw_item in value.split(","):
        item = raw_item.strip()
        if not item:
            continue
        if "x" in item.lower():
            size_text, count_text = item.lower().split("x", 1)
            try:
                size = int(size_text)
                count = int(count_text)
            except ValueError as exc:
                raise argparse.ArgumentTypeError("sizes and repetition counts must be integers") from exc
        else:
            try:
                size = int(item)
            except ValueError as exc:
                raise argparse.ArgumentTypeError("sizes must be integers") from exc
            count = 1
        if size < 1 or count < 1:
            raise argparse.ArgumentTypeError("sizes and repetition counts must be positive")
        if len(sizes) + count > MAX_SEQUENCE_REQUESTS:
            raise argparse.ArgumentTypeError(
                f"expanded sequence exceeds {MAX_SEQUENCE_REQUESTS} requests"
            )
        sizes.extend([size] * count)
    if not sizes:
        raise argparse.ArgumentTypeError("at least one response size is required")
    return sizes


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run an NDNSF segmented-response diagnostic sequence")
    parser.add_argument("--run-id", type=run_identity, required=True)
    parser.add_argument("--service", type=ndn_name, default="/HELLO")
    parser.add_argument("--provider", type=ndn_name, default="/example/hello/provider")
    parser.add_argument("--mode", choices=("normal", "targeted"), default="normal")
    parser.add_argument("--targeted-api", choices=("sync", "async"), default="sync")
    parser.add_argument("--sizes", type=parse_sizes, default=parse_sizes("64"))
    parser.add_argument("--ack-timeout-ms", type=positive_int, default=1000)
    parser.add_argument("--timeout-ms", type=positive_int, default=4000)
    parser.add_argument("--max-response-bytes", type=positive_int, default=1 << 20)
    parser.add_argument("--pause-after-index", type=nonnegative_int)
    parser.add_argument("--resume-file", type=Path)
    parser.add_argument("--pause-timeout-s", type=positive_float, default=30.0)
    add_process_arguments(parser)
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if any(size > args.max_response_bytes for size in args.sizes):
        parser.error("a requested size exceeds --max-response-bytes")
    if (args.pause_after_index is None) != (args.resume_file is None):
        parser.error("--pause-after-index and --resume-file must be provided together")
    if args.mode != "targeted" and args.targeted_api != "sync":
        parser.error("--targeted-api async requires --mode targeted")
    if args.pause_after_index is not None and args.pause_after_index >= len(args.sizes) - 1:
        parser.error("--pause-after-index must leave at least one subsequent request")
    results: list[dict[str, object]] = []
    pause_error = ""

    with optional_local_nfd(args.start_local_nfd):
        user = ServiceUser(**session_kwargs(args))
        for index, size in enumerate(args.sizes):
            request = encode_request(args.run_id, index, size)
            started = time.monotonic()
            timeout_terminal_count = 0
            response_terminal_count = 0
            try:
                if args.mode == "targeted" and args.targeted_api == "async":
                    terminal = threading.Event()
                    terminal_lock = threading.Lock()
                    terminal_state: dict[str, object] = {}

                    def on_response(value) -> None:
                        nonlocal response_terminal_count
                        with terminal_lock:
                            response_terminal_count += 1
                            terminal_state.setdefault("response", value)
                        terminal.set()

                    def on_timeout(request_id: str) -> None:
                        nonlocal timeout_terminal_count
                        with terminal_lock:
                            timeout_terminal_count += 1
                            terminal_state.setdefault("timeoutRequestId", request_id)
                        terminal.set()

                    user.request_service_targeted_async(
                        args.provider,
                        args.service,
                        request,
                        on_response=on_response,
                        on_timeout=on_timeout,
                        timeout_ms=args.timeout_ms,
                    )
                    terminal.wait((args.timeout_ms + 500) / 1000.0)
                    time.sleep(0.05)
                    with terminal_lock:
                        response = terminal_state.get("response")
                        timeout_request_id = str(terminal_state.get("timeoutRequestId", ""))
                    if response is None:
                        payload = b""
                        status = False
                        error = (
                            f"timeout: {timeout_request_id}"
                            if timeout_request_id
                            else "local deadline"
                        )
                        wire_request_id = timeout_request_id
                    else:
                        payload = bytes(response.payload)
                        status = bool(response.status)
                        error = str(response.error)
                        wire_request_id = str(getattr(response, "request_id", ""))
                elif args.mode == "targeted":
                    response = user.request_service_targeted(
                        args.provider,
                        args.service,
                        request,
                        timeout_ms=args.timeout_ms,
                    )
                    payload = bytes(response.payload)
                    status = bool(response.status)
                    error = str(response.error)
                    wire_request_id = str(getattr(response, "request_id", ""))
                    timeout_terminal_count = 1 if error.startswith("timeout:") else 0
                    response_terminal_count = 1 if status else 0
                else:
                    response = user.request_service(
                        args.service,
                        request,
                        ack_timeout_ms=args.ack_timeout_ms,
                        timeout_ms=args.timeout_ms,
                    )
                    payload = bytes(response.payload)
                    status = bool(response.status)
                    error = str(response.error)
                    wire_request_id = str(getattr(response, "request_id", ""))
            except Exception as exc:
                payload = b""
                status = False
                error = f"{type(exc).__name__}: {exc}"
                wire_request_id = ""
            elapsed_ms = (time.monotonic() - started) * 1000.0
            ok = status and payload == expected_payload(size, args.run_id, index)
            result = {
                "index": index,
                "mode": args.mode,
                "requestIdentity": f"{args.run_id}:{index}",
                "requestedBytes": size,
                "receivedBytes": len(payload),
                "status": status,
                "ok": ok,
                "error": error,
                "requestId": wire_request_id,
                "elapsedMs": round(elapsed_ms, 3),
                "targetedApi": args.targeted_api if args.mode == "targeted" else "not-applicable",
                "timeoutTerminalCount": timeout_terminal_count,
                "responseTerminalCount": response_terminal_count,
                "terminalCount": timeout_terminal_count + response_terminal_count,
                "deadlineLimitMs": args.timeout_ms + 500,
                "deadlineWithinLimit": elapsed_ms <= args.timeout_ms + 500,
            }
            results.append(result)
            print("SEGMENTED_RESPONSE_RESULT " + json.dumps(result, sort_keys=True), flush=True)
            if args.pause_after_index == index:
                print(
                    "SEGMENTED_RESPONSE_PAUSED "
                    + json.dumps(
                        {"index": index, "requestIdentity": f"{args.run_id}:{index}"},
                        sort_keys=True,
                    ),
                    flush=True,
                )
                deadline = time.monotonic() + args.pause_timeout_s
                while not args.resume_file.exists() and time.monotonic() < deadline:
                    time.sleep(0.05)
                if not args.resume_file.exists():
                    pause_error = f"resume file did not appear within {args.pause_timeout_s} seconds"
                    print(f"SEGMENTED_RESPONSE_PAUSE_ERROR {pause_error}", flush=True)
                    break
                print(
                    "SEGMENTED_RESPONSE_RESUMED "
                    + json.dumps({"index": index, "resumeFile": str(args.resume_file)}, sort_keys=True),
                    flush=True,
                )

    passed = sum(1 for result in results if result["ok"])
    exit_code = 0 if passed == len(args.sizes) and not pause_error else 1
    summary = {
        "schemaVersion": "spec112-segmented-user-v1",
        "runId": args.run_id,
        "mode": args.mode,
        "total": len(args.sizes),
        "resultCount": len(results),
        "passed": passed,
        "failed": len(args.sizes) - passed,
        "exitCode": exit_code,
        "pauseError": pause_error,
    }
    print("SEGMENTED_RESPONSE_SUMMARY " + json.dumps(summary, sort_keys=True), flush=True)
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
