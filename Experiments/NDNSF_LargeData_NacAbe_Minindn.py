#!/usr/bin/env python3
"""MiniNDN NAC-ABE large-data positive/negative protocol gate.

This is a correctness gate, not a performance campaign.  It intentionally
uses one controller, one authorized Provider, one unauthorized Provider
identity, and a small deterministic plaintext so that the result is cheap to
reproduce and leaves the existing performance corpus untouched.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from types import SimpleNamespace


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from mininet.log import setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import getPopen  # noqa: E402


APP_CONTROLLER = REPO / "build/examples/App_ServiceController"
APP_PROVIDER = REPO / "build/examples/App_Provider"
APP_USER = REPO / "build/examples/App_User"
TOPOLOGY = REPO / "Experiments/Topology/AI_Lab.conf"
DEFAULT_OUTPUT = REPO / "results/spec170-large-data-minindn-20260819"


class Proc:
    def __init__(self, process, log_file, log_path):
        self.process = process
        self.log_file = log_file
        self.log_path = log_path


def shell_quote(value):
    return perf.shell_quote(value)


def managed(binary, argv):
    return perf.managed_cmd(binary, argv)


def make_args():
    # These are the fields consumed by the shared keychain and environment
    # helpers.  Keep the gate's defaults explicit and non-adaptive.
    return SimpleNamespace(
        controller_node="memphis",
        user_node="memphis",
        provider_nodes="ucla",
        providers=1,
        serve_provider_certs=False,
        debug_ack=False,
        performance_mode=False,
        workload_mode="closed-loop",
        rate_rps=None,
        targeted=False,
        targeted_token_batch_size=None,
        targeted_token_adaptive=False,
        dk_bootstrap_check=False,
        crypto_diagnostics=False,
        timeline_trace=False,
        timeline_trace_sample_rate=100,
        diag_plaintext_ack=False,
        diag_plaintext_response=False,
        svs_piggyback_trace=False,
        svs_parallel_sync_processing=False,
        svs_parallel_workers=4,
        svs_parallel_queue=256,
        svs_sync_publish=False,
        svs_disable_parallel_production=False,
        svs_parallel_production_workers=None,
        svs_disable_parallel_production_signing=False,
        svs_parallel_production_signing=False,
        svs_disable_parallel_production_extra_block=False,
        svs_parallel_production_extra_block=False,
        svs_sync_batching=False,
        svs_sync_batch_ms=0,
        ack_threads=-1,
        extra_identities=["/example/hello/provider/unauthorized"],
    )


def start(node, label, command, output_dir, env, processes):
    log_path = output_dir / f"{label}.log"
    log_file = log_path.open("wb")
    process = getPopen(node, command, envDict=env, shell=True,
                       stdout=log_file, stderr=subprocess.STDOUT)
    processes.append(Proc(process, log_file, log_path))
    return processes[-1]


def stop_one(proc):
    if proc is None:
        return
    if proc.process.poll() is None:
        proc.process.send_signal(signal.SIGINT)
        try:
            proc.process.wait(timeout=4)
        except subprocess.TimeoutExpired:
            proc.process.kill()
            proc.process.wait(timeout=2)
    proc.log_file.flush()
    proc.log_file.close()


def stop_all(processes):
    for proc in reversed(processes):
        stop_one(proc)


def wait_for(path: Path, pattern: str, timeout: float, process=None) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = path.read_text(errors="replace") if path.exists() else ""
        if pattern in text:
            return True
        if process is not None and process.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def wait_for_file(path: Path, timeout: float, process=None) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and path.read_text(errors="replace").strip():
            return True
        if process is not None and process.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def quick_smoke():
    missing = [str(path) for path in (APP_CONTROLLER, APP_PROVIDER, APP_USER, TOPOLOGY)
               if not path.exists()]
    if missing:
        raise RuntimeError("NDNSF_LARGE_DATA_MININDN_QUICK_SMOKE_FAIL "
                           f"missing={missing}")
    print("NDNSF_LARGE_DATA_MININDN_QUICK_SMOKE_OK "
          f"topology={TOPOLOGY} controller=memphis provider=ucla")


def configure_routes(ndn):
    routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
    routing.addOrigin([ndn.net["memphis"]], [
        "/example/hello/controller",
        "/example/hello/user",
        "/example/hello/group",
    ])
    # The default identity is authorized.  The explicitly prepared
    # ``unauthorized`` identity is controller-keyed but has only OTHER policy,
    # so this is a service-authorization negative rather than a missing-key
    # failure.
    routing.addOrigin([ndn.net["ucla"]], [
        "/example/hello/provider",
        "/example/hello/provider/KEY",
        "/example/hello/provider/unauthorized",
        "/example/hello/provider/unauthorized/KEY",
        "/example/hello/group",
    ])
    routing.calculateRoutes()
    for node in ndn.net.hosts:
        Nfdc.setStrategy(node, "/example/hello", Nfdc.STRATEGY_MULTICAST)
        Nfdc.setStrategy(node, "/example/hello/group", Nfdc.STRATEGY_MULTICAST)


def run(output_dir: Path) -> int:
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    # Minindn parses process-global argv in its constructor.  Keep experiment
    # options out of that parser; otherwise --output-dir is reported as an
    # unrelated Minindn CLI error before the topology starts.
    original_argv = list(sys.argv)
    sys.argv = [sys.argv[0]]
    args = make_args()
    plaintext = "ndnsf-large-data-minindn-" + ("0123456789abcdef" * 512)
    name_file = output_dir / "encrypted-data-name.txt"
    # Never accept a name from a previous run.  The publisher writes this
    # file asynchronously after NAC-ABE setup completes.
    name_file.unlink(missing_ok=True)
    processes = []
    ndn = None
    summary = {
        "status": "FAIL",
        "formal_harness": "MiniNDN",
        "topology": str(TOPOLOGY),
        "plaintext_bytes": len(plaintext.encode()),
        "adaptive_admission": False,
        "authorized_identity": "/example/hello/provider",
        "unauthorized_identity": "/example/hello/provider/unauthorized",
    }

    try:
        setLogLevel("warning")
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(TOPOLOGY), workDir=str(output_dir / "minindn"))
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="WARN")
        perf.wait_for_nfd_sockets(ndn, output_dir)
        configure_routes(ndn)
        perf.initialize_example_keychains(ndn, args, output_dir)

        session_base = int(time.time()) + os.getpid()
        env = perf.app_env(output_dir, session_base, args)

        controller = start(
            ndn.net["memphis"], "controller",
            managed(APP_CONTROLLER, [
                "--policy-file", "examples/large-data-validation.policies",
                "--trust-schema", "examples/trust-schema.conf",
            ]), output_dir, env, processes)
        if not wait_for(controller.log_path, "ServiceController started", 10,
                        controller.process):
            raise RuntimeError("controller did not start")

        bootstrap = start(
            ndn.net["ucla"], "provider-bootstrap",
            managed(APP_PROVIDER, []), output_dir, env, processes)
        if not wait_for(bootstrap.log_path, "Registered service handler for /HELLO", 25,
                        bootstrap.process):
            # The exact INFO wording changed across builds; the permission
            # marker is the stronger gate for this experiment.
            if not wait_for(bootstrap.log_path, "Installed provider permission", 10,
                            bootstrap.process):
                raise RuntimeError("authorized bootstrap Provider did not become ready")

        user = start(
            ndn.net["memphis"], "user-publisher",
            managed(APP_USER, [
                "--large-data-publish-test",
                "--large-data-plaintext", plaintext,
                "--large-data-name-file", str(name_file),
            ]), output_dir, env, processes)
        if not wait_for_file(name_file, 35, user.process):
            raise RuntimeError("User did not publish a large-data name")
        encrypted_name = name_file.read_text(errors="replace").strip().splitlines()[0]
        summary["encrypted_data_name"] = encrypted_name

        stop_one(bootstrap)
        processes.remove(bootstrap)

        authorized = start(
            ndn.net["ucla"], "provider-authorized-fetch",
            managed(APP_PROVIDER, [
                "--large-data-fetch-test",
                "--large-data-name", encrypted_name,
                "--expect-large-data-plaintext", plaintext,
            ]), output_dir, env, processes)
        authorized.process.wait(timeout=45)
        authorized_text = authorized.log_path.read_text(errors="replace")
        summary["authorized_returncode"] = authorized.process.returncode
        summary["authorized_success_marker"] = "LARGE_DATA_FETCH_SUCCESS" in authorized_text
        summary["authorized_log"] = str(authorized.log_path)

        unauthorized = start(
            ndn.net["ucla"], "provider-unauthorized-fetch",
            managed(APP_PROVIDER, [
                "--provider-id", "unauthorized",
                "--large-data-fetch-test",
                "--large-data-name", encrypted_name,
                "--expect-large-data-failure",
            ]), output_dir, env, processes)
        unauthorized.process.wait(timeout=45)
        unauthorized_text = unauthorized.log_path.read_text(errors="replace")
        summary["unauthorized_returncode"] = unauthorized.process.returncode
        summary["unauthorized_clean_failure_marker"] = (
            "LARGE_DATA_UNAUTHORIZED_FAILURE_CLEAN" in unauthorized_text)
        summary["unauthorized_log"] = str(unauthorized.log_path)

        user_text = user.log_path.read_text(errors="replace")
        summary["publish_success_marker"] = "LARGE_DATA_PUBLISH_SUCCESS" in user_text
        summary["publish_segment_marker"] = "LARGE_DATA_PUBLISH_SEGMENTS" in user_text
        summary["publish_log"] = str(user.log_path)
        summary["status"] = "PASS" if all((
            summary["publish_success_marker"],
            summary["publish_segment_marker"],
            summary["authorized_returncode"] == 0,
            summary["authorized_success_marker"],
            summary["unauthorized_returncode"] == 0,
            summary["unauthorized_clean_failure_marker"],
        )) else "FAIL"
        return 0 if summary["status"] == "PASS" else 1
    except Exception as exc:
        summary["error"] = f"{type(exc).__name__}: {exc}"
        return 1
    finally:
        summary["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        (output_dir / "summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n")
        stop_all(processes)
        if ndn is not None:
            try:
                ndn.stop()
            finally:
                Minindn.cleanUp()
        sys.argv = original_argv


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick-smoke", action="store_true")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    if args.quick_smoke:
        quick_smoke()
        return 0
    if os.geteuid() != 0:
        raise SystemExit("MiniNDN runner must execute as root")
    return run(args.output_dir)


if __name__ == "__main__":
    raise SystemExit(main())
