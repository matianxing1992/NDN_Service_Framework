#!/usr/bin/env python3
"""Run one real NDNSF DistributedRepo node for the YOLO 2x2 example."""

from __future__ import annotations

import argparse
import signal
import threading
import time

from ndnsf_distributed_inference.app_sdk import APPDeployment
from py_repoclient.orchestration import RepoNodeApp


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

    # spec181 T005 repair: the repo serves through the native provider run
    # loop (GIL released).  CPython delivers SIGINT handlers only at
    # bytecode boundaries on the main thread, which the C++ run loop never
    # reaches — so a main-thread run() would ignore the supervised cleanup
    # SIGINT until it times out and falls back to SIGKILL (-9), failing the
    # terminal-cleanup gate.  Run the native loop on a worker thread and
    # keep the main thread in a Python wait loop: the signal handler only
    # sets an event (fast, no blocking work inside the signal context), and
    # the main thread then stops the native provider and exits 130 well
    # within the supervised 3 s window.
    stop_requested = threading.Event()

    def _on_sigint(signum, frame):
        del signum, frame
        stop_requested.set()

    signal.signal(signal.SIGINT, _on_sigint)
    runner_thread = threading.Thread(target=app.run, daemon=True)
    runner_thread.start()
    while not stop_requested.wait(0.5):
        if not runner_thread.is_alive():
            return 0
    try:
        app.provider.stop()
    except Exception:
        pass
    runner_thread.join(timeout=3)
    return 130


if __name__ == "__main__":
    raise SystemExit(main())
