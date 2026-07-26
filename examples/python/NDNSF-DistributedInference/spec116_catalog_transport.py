#!/usr/bin/env python3
"""Publish or fetch one Spec 116 exact-name signed APP record."""

from __future__ import annotations

import argparse
import hashlib
import time

from ndnsf import ServiceUser


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("mode", choices=("publish", "fetch"))
    value.add_argument("--group", default="/example/hello/group")
    value.add_argument("--controller", default="/example/hello/controller")
    value.add_argument("--identity", default="/example/hello/user")
    value.add_argument("--trust-schema", default="examples/trust-schema.conf")
    value.add_argument(
        "--record-name",
        default=("/example/hello/user/NDNSF/DI/DEFINITION/"
                 "spec116/sha256:catalog-smoke"))
    value.add_argument("--payload", default="spec116-signed-catalog-record")
    value.add_argument("--hold-seconds", type=float, default=20.0)
    value.add_argument("--timeout-ms", type=int, default=5000)
    return value


def main() -> int:
    args = parser().parse_args()
    user = ServiceUser(
        group=args.group,
        controller=args.controller,
        user=args.identity,
        trust_schema=args.trust_schema,
        permission_wait_ms=1500)
    user.start()
    try:
        if args.mode == "publish":
            payload = args.payload.encode("utf-8")
            result = user.publish_signed_app_data(
                args.record_name, payload,
                freshness_ms=max(1000, int(args.hold_seconds * 2000)))
            if not result.success or result.data_name != args.record_name:
                print(f"SPEC116_APP_DATA_FAILED error={result.error}", flush=True)
                return 2
            print(
                "SPEC116_APP_DATA_PUBLISHED "
                f"name={result.data_name} sha256={hashlib.sha256(payload).hexdigest()}",
                flush=True)
            time.sleep(max(0.0, args.hold_seconds))
            return 0

        result = user.fetch_signed_app_data(
            args.record_name, args.identity, timeout_ms=args.timeout_ms)
        if not result.success:
            print(f"SPEC116_APP_DATA_FAILED error={result.error}", flush=True)
            return 3
        expected = args.payload.encode("utf-8")
        if result.payload != expected or result.data_name != args.record_name:
            print("SPEC116_APP_DATA_FAILED error=content-binding-mismatch", flush=True)
            return 4
        print(
            "SPEC116_APP_DATA_FETCHED "
            f"name={result.data_name} signer={result.signer_certificate} "
            f"sha256={hashlib.sha256(result.payload).hexdigest()}",
            flush=True)
        return 0
    finally:
        user.stop()


if __name__ == "__main__":
    raise SystemExit(main())
