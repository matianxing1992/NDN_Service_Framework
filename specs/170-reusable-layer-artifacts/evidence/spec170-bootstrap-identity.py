#!/usr/bin/env python3
"""Bootstrap one controller-signed identity in its isolated PIB.

This is a bounded workload helper: the native provider executable historically
did not expose its certificate-bootstrap option, so D0 uses this helper to
prepare the exact PIB before launching the provider.  The source CLI receives
the same option in the follow-up source fix.
"""

from __future__ import annotations

import argparse
import json

from ndnsf import ServiceProvider, ServiceUser


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("provider", "user"), required=True)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--group", required=True)
    parser.add_argument("--controller", required=True)
    parser.add_argument("--trust-schema", required=True)
    parser.add_argument("--token", required=True)
    args = parser.parse_args()

    if args.role == "provider":
        runtime = ServiceProvider(
            group=args.group,
            controller=args.controller,
            provider_prefix=args.identity,
            trust_schema=args.trust_schema,
            serve_certificates=True,
            bootstrap_token=args.token,
        )
    else:
        runtime = ServiceUser(
            group=args.group,
            controller=args.controller,
            user=args.identity,
            trust_schema=args.trust_schema,
            permission_wait_ms=5000,
            serve_certificates=True,
            bootstrap_token=args.token,
        )

    allowed = []
    if args.role == "user":
        allowed = [entry.service for entry in runtime.get_allowed_services()]
    print(json.dumps({
        "identity": args.identity,
        "role": args.role,
        "allowed": allowed,
        "status": "bootstrapped",
    }, sort_keys=True), flush=True)
    runtime.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
