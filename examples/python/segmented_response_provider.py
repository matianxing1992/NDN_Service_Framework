#!/usr/bin/env python3
"""Provider used by the MiniNDN segmented-response diagnostic campaign."""

from __future__ import annotations

import argparse
import json
import sys
from typing import List, Optional

from common import add_process_arguments, optional_local_nfd, session_kwargs
from ndnsf import ServiceProvider
from spec112_segmented_common import (
    decode_request,
    ndn_name,
    optional_ndn_name,
    positive_int,
    response_payload,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Serve deterministic NDNSF responses with requester-selected sizes")
    parser.add_argument("--provider-id", type=optional_ndn_name, default="")
    parser.add_argument("--service", type=ndn_name, default="/HELLO")
    parser.add_argument("--max-response-bytes", type=positive_int, default=1 << 20)
    add_process_arguments(parser)
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)

    exit_code = 0
    handled = 0
    rejected = 0
    try:
        provider = ServiceProvider(provider_id=args.provider_id, **session_kwargs(args))

        @provider.handler(args.service)
        def respond(request: bytes) -> bytes:
            nonlocal handled, rejected
            try:
                run_id, index, size = decode_request(request, args.max_response_bytes)
            except ValueError as exc:
                rejected += 1
                print(
                    "SEGMENTED_PROVIDER_REJECT "
                    + json.dumps({"error": str(exc), "requestHex": request.hex()}, sort_keys=True),
                    flush=True,
                )
                return b"SPEC112_INVALID_DIAGNOSTIC_REQUEST"

            handled += 1
            print(
                "SEGMENTED_PROVIDER_HANDLER "
                + json.dumps(
                    {
                        "index": index,
                        "requestIdentity": f"{run_id}:{index}",
                        "responseBytes": size,
                        "runId": run_id,
                    },
                    sort_keys=True,
                ),
                flush=True,
            )
            return response_payload(size, run_id, index)

        with optional_local_nfd(args.start_local_nfd):
            result = provider.run(args.service)
            exit_code = int(result) if result is not None else 0
    except Exception as exc:
        exit_code = 2
        print(f"SEGMENTED_PROVIDER_ERROR {type(exc).__name__}: {exc}", file=sys.stderr, flush=True)
    finally:
        print(
            "SEGMENTED_PROVIDER_EXIT "
            + json.dumps(
                {
                    "exitCode": exit_code,
                    "handled": handled,
                    "rejected": rejected,
                    "schemaVersion": "spec112-segmented-provider-v1",
                },
                sort_keys=True,
            ),
            flush=True,
        )
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
