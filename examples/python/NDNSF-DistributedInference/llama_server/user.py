#!/usr/bin/env python3
"""NDNSF-DI user for the Qwen GGUF + llama-server example."""

from __future__ import annotations

import argparse
import json

from ndnsf_distributed_inference import APPClient

from llama_server_lib import SERVICE, decode_chat_response, encode_chat_request


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
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    payload = encode_chat_request(
        args.prompt,
        model=args.model,
        system=args.system,
        max_tokens=args.max_tokens,
        temperature=args.temperature,
    )
    if args.dry_run:
        print("LLAMA_SERVER_USER_DRY_RUN", payload.decode("utf-8"))
        return 0

    client = APPClient.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
        group=args.group,
    )
    result = client.distributed_inference(
        SERVICE,
        payload,
        dynamic_provisioning=False,
        ack_timeout_ms=args.ack_timeout_ms,
        timeout_ms=args.timeout_ms,
    )
    response = decode_chat_response(result.payload)
    print("LLAMA_SERVER_USER_RESPONSE", json.dumps(response, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
