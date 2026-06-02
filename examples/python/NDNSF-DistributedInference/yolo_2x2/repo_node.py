#!/usr/bin/env python3
"""Run one real NDNSF DistributedRepo node for the YOLO 2x2 example."""

from __future__ import annotations

import argparse

from ndnsf_distributed_inference import APPDeployment, RepoNodeApp


CONFIG_FILE = "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"
REPO_SERVICE = "/NDNSF/DistributedRepo"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default=CONFIG_FILE)
    parser.add_argument("--generated-policy-dir",
                        default="/tmp/ndnsf-di-yolo-2x2-policy")
    parser.add_argument("--provider-id", required=True,
                        help="Provider id suffix, such as repoA")
    parser.add_argument("--repo-node", required=True,
                        help="Stable repo node name advertised in ACK metadata")
    parser.add_argument("--free-bytes", type=int, default=4_000_000_000)
    parser.add_argument("--memory-cache-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--preallocate-bytes", type=int, default=0)
    parser.add_argument("--failure-domain", default="")
    parser.add_argument("--storage-dir", default="",
                        help="Provider-local repo storage directory")
    parser.add_argument("--advertise-stored-prefixes", action="store_true",
                        help="Advertise stored Data prefixes through NLSR")
    parser.add_argument("--handler-threads", type=int, default=4)
    parser.add_argument("--ack-threads", type=int, default=2)
    parser.add_argument("--no-serve-certificates", action="store_true")
    args = parser.parse_args()

    deployment = APPDeployment.from_config(
        args.config,
        generated_policy_dir=args.generated_policy_dir,
    ).deployment
    app = RepoNodeApp(
        repo_node=args.repo_node,
        service_name=REPO_SERVICE,
        provider_id=args.provider_id,
        group=deployment.group,
        controller=deployment.controller,
        provider_prefix=deployment.provider_prefix,
        trust_schema=deployment.trust_schema,
        free_bytes=args.free_bytes,
        failure_domain=args.failure_domain,
        storage_dir=args.storage_dir or None,
        memory_cache_bytes=args.memory_cache_bytes,
        preallocate_bytes=args.preallocate_bytes,
        advertise_stored_prefixes=args.advertise_stored_prefixes,
        handler_threads=args.handler_threads,
        ack_threads=args.ack_threads,
        serve_certificates=not args.no_serve_certificates,
    )
    return app.run()


if __name__ == "__main__":
    raise SystemExit(main())
