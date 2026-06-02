#!/usr/bin/env python3
"""MiniNDN smoke test for YOLO two-stage auto split inference."""

from __future__ import annotations

import os
import signal
import shutil
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from mininet.log import info, setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import getPopen  # noqa: E402

TOPO = REPO / "Experiments/Topology/testbed(loss=0%).conf"
OUT = REPO / "results/yolo_split_minindn_auto"
PY_DIR = REPO / "examples/python/NDNSF-DistributedInference/yolo_split"
CONFIG = OUT / "yolo_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-di-yolo-split-policy"


class Args(SimpleNamespace):
    pass


def log(message: str) -> None:
    info(message + "\n")


def python_cmd(script: str, argv: list[str]) -> str:
    args = " ".join([perf.shell_quote(str(PY_DIR / script))] +
                    [perf.shell_quote(arg) for arg in argv])
    return f"cd {perf.shell_quote(REPO)} && exec python3 {args}"


def start(node, name, cmd, env, procs):
    path = OUT / f"{name}.log"
    f = path.open("wb")
    log(f"start {name} on {node.name}: {cmd}")
    node_env = dict(env)
    node_env["NDNSF_ARTIFACT_CACHE_DIR"] = str(OUT / "artifact-cache" / node.name)
    p = getPopen(node, cmd, envDict=node_env, shell=True,
                 stdout=f, stderr=subprocess.STDOUT)
    procs.append((p, f, path))
    return p, path


def wait_log(path: Path, needle: str, timeout: int = 30, proc=None) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def stop(procs) -> None:
    for p, f, _ in reversed(procs):
        if p.poll() is None:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(timeout=3)
            except Exception:
                p.kill()
        f.close()


def build_python_path() -> str:
    return ":".join([
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        str(PY_DIR),
        "/home/tianxing/.local/lib/python3.8/site-packages",
        "/usr/local/lib/python3.8/dist-packages",
        "/usr/lib/python3/dist-packages",
        os.environ.get("PYTHONPATH", ""),
    ])


def generate_auto_split_policy() -> None:
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, "PYTHONPATH": build_python_path()}
    subprocess.run([
        "python3",
        str(PY_DIR / "split_model.py"),
        "--model",
        "yolo26n.pt",
        "--input-size",
        "32",
        "--auto-split",
        "--out-dir",
        str(OUT / "model"),
        "--policy",
        str(CONFIG),
    ], cwd=str(REPO), env=env, check=True)


def main() -> None:
    setLogLevel("info")
    generate_auto_split_policy()
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPO))
    procs = []
    args = Args(
        controller_node="csu",
        user_node="memphis",
        providers=2,
        provider_nodes="ucla,wustl",
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
        svs_sync_publish=True,
        svs_disable_parallel_production=False,
        svs_parallel_production_workers=None,
        svs_disable_parallel_production_signing=False,
        svs_parallel_production_signing=False,
        svs_disable_parallel_production_extra_block=False,
        svs_parallel_production_extra_block=False,
        svs_sync_batching=False,
        svs_sync_batch_ms=0,
        ack_threads=1,
        performance_mode=False,
    )
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, OUT)

        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin([ndn.net["csu"]], [
            "/example/hello/controller",
            "/example/hello/controller/DKEY",
            "/example/hello/controller/KEY",
            "/example/hello/group",
        ])
        rh.addOrigin([ndn.net["memphis"]], [
            "/example/hello/user",
            "/example/hello/group",
        ])
        rh.addOrigin([ndn.net["ucla"]], [
            "/example/hello/provider",
            "/example/hello/provider/KEY",
            "/example/hello/group",
        ])
        rh.addOrigin([ndn.net["wustl"]], [
            "/example/hello/provider/A",
            "/example/hello/provider/A/KEY",
            "/example/hello/group",
        ])
        rh.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/example/hello", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/example/hello/group", Nfdc.STRATEGY_MULTICAST)

        perf.initialize_example_keychains(ndn, args, OUT)
        session = int(time.time()) + os.getpid()
        env = perf.app_env(OUT, session, args)
        env["NDNSF_HANDLER_THREADS"] = "1"
        env["NDNSF_ACK_THREADS"] = "1"
        env["NDNSF_SVS_ASYNC_PUBLISH"] = "0"
        env["NDNSF_SVS_PARALLEL_SYNC"] = "0"
        env["NDNSF_SVS_PARALLEL_PRODUCTION"] = "0"
        env["NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING"] = "0"
        env["NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK"] = "0"
        env["PYTHONPATH"] = build_python_path()

        common = ["--config", str(CONFIG), "--generated-policy-dir", GEN_POLICY]
        _, controller_log = start(ndn.net["csu"], "controller",
                                  python_cmd("controller.py", common), env, procs)
        if not wait_log(controller_log, "ServiceController listening", 20):
            raise RuntimeError(f"controller did not become ready; see {controller_log}")
        time.sleep(4)

        providers = [
            ("ucla", "provider-stage0", ["--role", "/Stage/0"]),
            ("wustl", "provider-stage1", ["--provider-id", "A", "--role", "/Stage/1"]),
        ]
        for node_name, name, argv in providers:
            _, provider_log = start(ndn.net[node_name], name,
                                    python_cmd("provider.py", common + argv + [
                                        "--temp-dir",
                                        f"/tmp/{name}",
                                    ]),
                                    env, procs)
            if not wait_log(provider_log, "Installed provider permission", 30):
                raise RuntimeError(f"{name} did not install permissions; see {provider_log}")
            time.sleep(0.5)

        user_proc, user_log = start(ndn.net["memphis"], "user",
                                    python_cmd("user.py", common + [
                                        "--ack-timeout-ms", "1500",
                                        "--timeout-ms", "60000",
                                        "--input-size", "32",
                                    ]),
                                    env, procs)
        user_proc.wait(timeout=90)
        user_text = user_log.read_text(errors="replace")
        print(user_text)
        if "YOLO_SPLIT_RESULT" not in user_text or "ok=true" not in user_text:
            raise RuntimeError(
                f"YOLO split auto MiniNDN smoke failed rc={user_proc.returncode}; "
                f"log={user_log}")
        print(f"YOLO_SPLIT_MININDN_OK user={user_log}")
    finally:
        stop(procs)
        ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    main()
