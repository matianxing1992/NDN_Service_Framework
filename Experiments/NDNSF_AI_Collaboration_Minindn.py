#!/usr/bin/env python3
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf
from mininet.log import info, setLogLevel
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
from minindn.helpers.nfdc import Nfdc
from minindn.minindn import Minindn
from minindn.util import getPopen

APP_CONTROLLER = REPO / "build/examples/App_ServiceController"
APP_PROVIDER = REPO / "build/examples/AI_DistributedCollaborationProvider"
APP_USER = REPO / "build/examples/AI_User"
TOPO = REPO / "Experiments/Topology/AI_testbed.conf"
OUT = REPO / "results/ai_collaboration_minindn_quick"


class Args(SimpleNamespace):
    pass


def log(message):
    info(message + "\n")


def start(node, name, cmd, env, procs):
    path = OUT / f"{name}.log"
    f = path.open("wb")
    log(f"start {name} on {node.name}: {cmd}")
    p = getPopen(node, cmd, envDict=env, shell=True, stdout=f, stderr=subprocess.STDOUT)
    procs.append((p, f, path))
    return p, path


def wait_log(path, needle, timeout=30, proc=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            return False
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        time.sleep(0.2)
    return False


def stop(procs):
    for p, f, _ in reversed(procs):
        if p.poll() is None:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(timeout=3)
            except Exception:
                p.kill()
        f.close()


def main():
    setLogLevel("info")
    OUT.mkdir(parents=True, exist_ok=True)
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPO))
    procs = []
    args = Args(
        controller_node="memphis",
        user_node="memphis",
        providers=9,
        provider_nodes="ucla,wustl,uiuc,csu,arizona,caida,neu,umich,pku",
        serve_provider_certs=False,
        debug_ack=False,
        timeline_trace=False,
        dk_bootstrap_check=False,
        crypto_diagnostics=False,
        diag_plaintext_ack=False,
        diag_plaintext_response=False,
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
    )

    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, OUT)

        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin(
            [ndn.net["memphis"]],
            ["/example/hello/controller", "/example/hello/user", "/example/hello/group"],
        )
        origins = [
            ("ucla", "/example/hello/provider"),
            ("wustl", "/example/hello/provider/A"),
            ("uiuc", "/example/hello/provider/B"),
            ("csu", "/example/hello/provider/C"),
            ("arizona", "/example/hello/provider/D"),
            ("caida", "/example/hello/provider/E"),
            ("neu", "/example/hello/provider/F"),
            ("umich", "/example/hello/provider/G"),
            ("pku", "/example/hello/provider/H"),
        ]
        for node_name, prefix in origins:
            rh.addOrigin([ndn.net[node_name]], [prefix, prefix + "/KEY", "/example/hello/group"])
        rh.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/example/hello", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/example/hello/group", Nfdc.STRATEGY_MULTICAST)

        perf.initialize_example_keychains(ndn, args, OUT)
        session = int(time.time()) + os.getpid()
        env = perf.app_env(OUT, session, args)

        ctrl_cmd = perf.managed_cmd(
            APP_CONTROLLER, ["--policy-file", "examples/ai_collaboration.policies"]
        )
        start(ndn.net["memphis"], "controller", ctrl_cmd, env, procs)
        time.sleep(2)

        providers = [
            ("ucla", "provider-p00", ["--role", "p00"]),
            ("wustl", "provider-p01", ["--provider-id", "A", "--role", "p01"]),
            ("uiuc", "provider-p02", ["--provider-id", "B", "--role", "p02"]),
            ("csu", "provider-p10", ["--provider-id", "C", "--role", "p10"]),
            ("arizona", "provider-p11", ["--provider-id", "D", "--role", "p11"]),
            ("caida", "provider-p12", ["--provider-id", "E", "--role", "p12"]),
            ("neu", "provider-p20", ["--provider-id", "F", "--role", "p20"]),
            ("umich", "provider-p21", ["--provider-id", "G", "--role", "p21"]),
            ("pku", "provider-p22", ["--provider-id", "H", "--role", "p22"]),
        ]
        for node_name, name, argv in providers:
            _, lp = start(ndn.net[node_name], name, perf.managed_cmd(APP_PROVIDER, argv), env, procs)
            if not wait_log(lp, "[AI_Provider]", 20):
                raise RuntimeError(f"{name} did not start; see {lp}")
            time.sleep(0.5)

        time.sleep(3)
        user_proc, user_log = start(
            ndn.net["memphis"],
            "user",
            perf.managed_cmd(APP_USER, ["--ack-timeout-ms", "2000", "--timeout-ms", "30000"]),
            env,
            procs,
        )
        user_proc.wait(timeout=45)
        text = user_log.read_text(errors="replace")
        print(text)
        if user_proc.returncode != 0 or "AI_COLLAB_RESULT" not in text:
            raise RuntimeError(f"AI collaboration failed rc={user_proc.returncode}; log={user_log}")
        print(f"AI_COLLAB_MININDN_OK log={user_log}")
    finally:
        stop(procs)
        ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    main()
