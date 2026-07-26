#!/usr/bin/env python3
"""Consume Provider-validated semantic-name opaque bytes with future prefetch."""

import argparse
import hashlib
import json
import time
from pathlib import Path

from ndnsf import (LiveStreamDescriptor, LiveStreamItemAdmission,
                   ServiceUser)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--descriptor", required=True)
    parser.add_argument("--receipt", required=True)
    parser.add_argument("--count", type=int, default=12)
    parser.add_argument("--minimum-count", type=int)
    parser.add_argument("--minimum-first-cursor", type=int, default=0)
    parser.add_argument("--start", choices=("latest", "beginning"), default="latest")
    parser.add_argument("--fec", action="store_true")
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()

    descriptor_path = Path(args.descriptor)
    deadline = time.monotonic() + args.timeout
    while not descriptor_path.exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    descriptor = LiveStreamDescriptor.from_dict(
        json.loads(descriptor_path.read_text(encoding="utf-8")))
    received = []

    def on_item(item):
        received.append({
            "cursor": item.cursor,
            "name": item.original_name,
            "provider": item.verified_provider,
            "provenance": item.provenance,
            "size": len(item.content),
            "sha256": hashlib.sha256(item.content).hexdigest(),
        })
        return LiveStreamItemAdmission.accept_item()

    user = ServiceUser(
        group="/example/live/group",
        controller="/example/live/controller",
        user="/example/live/user",
        trust_schema="examples/trust-schema.conf",
        serve_certificates=True,
    )
    user.start()
    handle = user.open_live_stream(
        descriptor, start=args.start, aggregate_interest_limit=16,
        enable_fec_recovery=args.fec, interest_lifetime_ms=500,
        on_item=on_item)
    handle.start()
    target = args.minimum_count if args.minimum_count is not None else args.count
    while len(received) < target and time.monotonic() < deadline:
        time.sleep(0.05)
    status = handle.status()
    handle.stop()
    user.stop()
    result = {
        "passed": (len(received) >= target and bool(received) and
                   received[0]["cursor"] >= args.minimum_first_cursor),
        "expected": target,
        "start": args.start,
        "firstCursor": received[0]["cursor"] if received else None,
        "received": received,
        "delivered": int(status.delivered),
        "recovered": int(status.recovered),
        "timeouts": int(status.timeouts),
        "nacks": int(status.nacks),
        "rejected": int(status.rejected),
        "mappingInterests": int(status.mapping_interests),
        "payloadInterests": int(status.payload_interests),
        "state": str(status.state),
        "reason": str(status.reason),
    }
    Path(args.receipt).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n",
                                  encoding="utf-8")
    return 0 if result["passed"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
