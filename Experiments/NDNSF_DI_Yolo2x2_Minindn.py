#!/usr/bin/env python3
"""MiniNDN smoke test for YOLO-style NDNSF-DI 2x2 split inference."""

from __future__ import annotations

import os
import signal
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
OUT = REPO / "results/yolo_2x2_minindn_quick"
PY_DIR = REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2"
CONFIG = OUT / "yolo_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-di-yolo-2x2-policy"
REPO_MANIFEST = OUT / "repo-manifests.json"


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
    p = getPopen(node, cmd, envDict=node_env, shell=True, stdout=f, stderr=subprocess.STDOUT)
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


def stop(procs):
    for p, f, _ in reversed(procs):
        if p.poll() is None:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(timeout=3)
            except Exception:
                p.kill()
        f.close()


def main() -> None:
    setLogLevel("info")
    OUT.mkdir(parents=True, exist_ok=True)
    py_path = ":".join([
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        str(PY_DIR),
        os.environ.get("PYTHONPATH", ""),
    ])
    subprocess.run([
        "python3",
        str(PY_DIR / "split_model.py"),
        "--out-dir",
        str(OUT / "model"),
        "--policy",
        str(CONFIG),
        "--dynamic-provisioning",
        "--trust-anchor-file",
        str(OUT / "security/root.cert"),
    ], cwd=str(REPO), env={**os.environ, "PYTHONPATH": py_path}, check=True)
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPO))
    procs = []
    args = Args(
        controller_node="csu",
        user_node="memphis",
        providers=5,
        provider_nodes="ucla,wustl,uiuc,umich,neu",
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
        rh.addOrigin([ndn.net["csu"]], [
            "/example/hello/controller",
            "/example/hello/controller/DKEY",
            "/example/hello/controller/KEY",
            "/example/hello/group",
        ])
        rh.addOrigin([ndn.net["memphis"]], ["/example/hello/user", "/example/hello/group"])
        origins = [
            ("ucla", "/example/hello/provider"),
            ("wustl", "/example/hello/provider/A"),
            ("uiuc", "/example/hello/provider/B"),
            ("umich", "/example/hello/provider/C"),
            ("neu", "/example/hello/provider/D"),
        ]
        for node_name, prefix in origins:
            rh.addOrigin([ndn.net[node_name]], [prefix, prefix + "/KEY", "/example/hello/group"])
        rh.addOrigin([ndn.net["neu"]], ["/NDNSF/DistributedRepo/Object"])
        rh.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/example/hello", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/example/hello/group", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF/DistributedRepo/Object", Nfdc.STRATEGY_MULTICAST)

        perf.initialize_example_keychains(ndn, args, OUT)
        subprocess.run(["rm", "-rf", str(OUT / "artifact-cache")], check=False)
        session = int(time.time()) + os.getpid()
        env = perf.app_env(OUT, session, args)
        env["PYTHONPATH"] = ":".join([
            str(REPO / "NDNSF-DistributedInference"),
            str(REPO / "pythonWrapper"),
            str(PY_DIR),
            "/home/tianxing/.local/lib/python3.8/site-packages",
            "/usr/local/lib/python3.8/dist-packages",
            "/usr/lib/python3/dist-packages",
            os.environ.get("PYTHONPATH", ""),
        ])

        common = ["--config", str(CONFIG), "--generated-policy-dir", GEN_POLICY]
        _, controller_log = start(ndn.net["csu"], "controller",
                                  python_cmd("controller.py", common), env, procs)
        if not wait_log(controller_log, "ServiceController listening", 20):
            raise RuntimeError(f"controller did not become ready; see {controller_log}")
        time.sleep(4)
        start(
            ndn.net["neu"],
            "repo",
            python_cmd("repo_node.py", common + [
                "--provider-id", "D",
                "--repo-node", "/example/hello/provider/D",
                "--failure-domain", "repo-rack",
                "--storage-dir", "/tmp/yolo-2x2-repo-store",
            ]),
            env,
            procs,
        )
        deployer_proc, deployer_log = start(ndn.net["csu"], "controller-deployer",
                                            python_cmd("controller.py", common + [
                                                "--deploy-only",
                                                "--deploy-to-repo-manifest",
                                                str(REPO_MANIFEST),
                                                "--replication-factor",
                                                "1",
                                            ]), env, procs)
        if not wait_log(deployer_log, "YOLO_2X2_CONTROLLER_REPO_DEPLOYED", 120,
                        proc=deployer_proc):
            raise RuntimeError(
                f"controller did not deploy repo artifacts; see {deployer_log}")

        providers = [
            ("ucla", "provider-s00", ["--role", "/Stage/0/Shard/0"]),
            ("wustl", "provider-s01", ["--provider-id", "A", "--role", "/Stage/0/Shard/1"]),
            ("uiuc", "provider-s10", ["--provider-id", "B", "--role", "/Stage/1/Shard/0"]),
            ("umich", "provider-s11", ["--provider-id", "C", "--role", "/Stage/1/Shard/1"]),
        ]
        for node_name, name, argv in providers:
            _, lp = start(ndn.net[node_name], name,
                          python_cmd("provider.py", common + argv + [
                              "--dynamic-provisioning",
                              "--temp-dir",
                              f"/tmp/{name}",
                          ]),
                          env, procs)
            if not wait_log(lp, "Installed provider permission", 20):
                raise RuntimeError(f"{name} did not install permissions; see {lp}")
            time.sleep(0.5)

        time.sleep(2)
        user_common = common + [
            "--repo-manifest-file",
            str(REPO_MANIFEST),
            "--ack-timeout-ms", "1500",
            "--timeout-ms", "60000",
            "--sequential-requests", "1",
        ]
        user_proc, user_log = start(
            ndn.net["memphis"],
            "user-cold",
            python_cmd("user.py", user_common),
            env,
            procs,
        )
        user_proc.wait(timeout=90)
        cold_text = user_log.read_text(errors="replace")
        print(cold_text)
        if "YOLO_2X2_RESULT" not in cold_text or "ok=true" not in cold_text:
            raise RuntimeError(f"YOLO 2x2 cold provisioning failed rc={user_proc.returncode}; log={user_log}")

        warm_proc, warm_log = start(
            ndn.net["memphis"],
            "user-warm",
            python_cmd("user.py", common + [
                "--repo-manifest-file",
                str(REPO_MANIFEST),
                "--ack-timeout-ms", "1500",
                "--timeout-ms", "60000",
                "--sequential-requests", "1",
            ]),
            env,
            procs,
        )
        warm_proc.wait(timeout=90)
        warm_text = warm_log.read_text(errors="replace")
        print(warm_text)
        provider_text = "\n".join(
            (OUT / f"{name}.log").read_text(errors="replace")
            for name in ("provider-s00", "provider-s01", "provider-s10", "provider-s11")
        )
        success = (
            "YOLO_2X2_RESULT" in warm_text and
            "ok=true" in warm_text and
            "NDNSF_EXECUTION_ARTIFACT_CACHE_MISS" in provider_text and
            "NDNSF_EXECUTION_ARTIFACT_CACHE_HIT" in provider_text
        )
        if not success:
            raise RuntimeError(
                f"YOLO 2x2 dynamic provisioning/cache validation failed; "
                f"cold={user_log} warm={warm_log}")
        print(f"YOLO_2X2_DYNAMIC_PROVISIONING_MININDN_OK cold={user_log} warm={warm_log}")
    finally:
        stop(procs)
        ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    main()
