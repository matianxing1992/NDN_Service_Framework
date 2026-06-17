#!/usr/bin/env python3
"""NDNSF-DI user for the Qwen GGUF + llama-server example."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import time

from ndnsf_distributed_inference import APPClient

from llama_server_lib import (
    SERVICE,
    decode_chat_response,
    encode_chat_request,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="/tmp/ndnsf-di-llama-server-policy.yaml")
    parser.add_argument("--generated-policy-dir", default="/tmp/ndnsf-di-llama-server-generated")
    parser.add_argument("--group", default="")
    parser.add_argument("--prompt", default="Say hello from NDNSF-DI in one short sentence.")
    parser.add_argument("--system", default="You are a concise assistant.")
    parser.add_argument("--model", default="qwen2.5-0.5b")
    parser.add_argument("--max-tokens", type=int, default=64)
    parser.add_argument("--temperature", type=float, default=0.2)
    parser.add_argument("--ack-timeout-ms", type=int, default=500)
    parser.add_argument("--timeout-ms", type=int, default=60000)
    parser.add_argument("--requests", type=int, default=1,
                        help="Number of measured requests to issue with one APPClient")
    parser.add_argument("--duration-s", type=float, default=0.0,
                        help="Measured duration; overrides --requests when positive")
    parser.add_argument("--interval-ms", type=float, default=0.0,
                        help="Minimum interval between request start times")
    parser.add_argument("--csv", default="",
                        help="Optional CSV path for per-request timing rows")
    parser.add_argument("--quiet-per-request", action="store_true",
                        help="Write per-request timing only to CSV")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.dry_run:
        payload = encode_chat_request(
            args.prompt,
            model=args.model,
            system=args.system,
            max_tokens=args.max_tokens,
            temperature=args.temperature,
        )
        print("LLAMA_SERVER_USER_DRY_RUN", payload.decode("utf-8"))
        return 0

    client = APPClient.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        group=args.group,
    )
    payload = encode_chat_request(
        args.prompt,
        model=args.model,
        system=args.system,
        max_tokens=args.max_tokens,
        temperature=args.temperature,
    )
    rows = []
    csv_file = None
    writer = None
    if args.csv:
        csv_path = Path(args.csv)
        csv_path.parent.mkdir(parents=True, exist_ok=True)
        csv_file = csv_path.open("w", newline="", encoding="utf-8")
        writer = csv.DictWriter(
            csv_file,
            fieldnames=[
                "index", "elapsed_ms", "provider_prompt_ms",
                "provider_predicted_ms", "provider_total_ms",
                "prompt_n", "predicted_n",
            ],
        )
        writer.writeheader()
    deadline = time.perf_counter() + float(args.duration_s) if args.duration_s > 0 else None
    index = 0
    try:
        while True:
            if deadline is not None:
                if time.perf_counter() >= deadline and index > 0:
                    break
            elif index >= max(1, int(args.requests)):
                break
            start = time.perf_counter()
            result = client.distributed_inference(
                SERVICE,
                payload,
                dynamic_provisioning=False,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=args.timeout_ms,
            )
            elapsed_ms = (time.perf_counter() - start) * 1000.0
            response = decode_chat_response(result.payload)
            timings = response.get("timings", {}) if isinstance(response, dict) else {}
            provider_prompt_ms = float(timings.get("prompt_ms") or 0.0)
            provider_predicted_ms = float(timings.get("predicted_ms") or 0.0)
            row = {
                "index": index,
                "elapsed_ms": f"{elapsed_ms:.3f}",
                "provider_prompt_ms": f"{provider_prompt_ms:.3f}",
                "provider_predicted_ms": f"{provider_predicted_ms:.3f}",
                "provider_total_ms": f"{provider_prompt_ms + provider_predicted_ms:.3f}",
                "prompt_n": timings.get("prompt_n", ""),
                "predicted_n": timings.get("predicted_n", ""),
            }
            rows.append(row)
            if writer is not None:
                writer.writerow(row)
                csv_file.flush()
            if not args.quiet_per_request:
                print(
                    "LLAMA_SERVER_USER_TIMING",
                    f"index={index}",
                    f"elapsed_ms={elapsed_ms:.2f}",
                    f"provider_prompt_ms={provider_prompt_ms:.2f}",
                    f"provider_predicted_ms={provider_predicted_ms:.2f}",
                    flush=True,
                )
            if index == 0:
                print("LLAMA_SERVER_USER_RESPONSE", json.dumps(response, sort_keys=True))
            index += 1
            if args.interval_ms > 0:
                elapsed_since_start_ms = (time.perf_counter() - start) * 1000.0
                sleep_ms = float(args.interval_ms) - elapsed_since_start_ms
                if sleep_ms > 0:
                    time.sleep(sleep_ms / 1000.0)
    finally:
        if csv_file is not None:
            csv_file.close()
    if not rows:
        raise RuntimeError("no llama-server requests were measured")
    elapsed_values = [float(row["elapsed_ms"]) for row in rows]
    provider_values = [float(row["provider_total_ms"]) for row in rows]
    print(
        "LLAMA_SERVER_USER_SUMMARY",
        f"count={len(rows)}",
        f"elapsed_avg_ms={sum(elapsed_values) / len(elapsed_values):.2f}",
        f"elapsed_min_ms={min(elapsed_values):.2f}",
        f"elapsed_max_ms={max(elapsed_values):.2f}",
        f"provider_avg_ms={sum(provider_values) / len(provider_values):.2f}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
