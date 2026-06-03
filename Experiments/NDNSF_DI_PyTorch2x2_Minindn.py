#!/usr/bin/env python3
"""MiniNDN smoke test for PyTorch-defined fully connected ONNX 2x2 collaboration."""

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
OUT = REPO / "results/pytorch_eager_2x2_minindn_quick"
PY_DIR = REPO / "examples/python/NDNSF-DistributedInference/pytorch_eager_2x2"
CONFIG = OUT / "pytorch_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-di-pytorch-2x2-policy"
APP_ROOT = "/NDNSF-DistributeInference/example"
CONTROLLER_IDENTITY = APP_ROOT + "/controller"
USER_IDENTITY = APP_ROOT + "/user"
PROVIDER_PREFIX = APP_ROOT + "/provider"


class Args(SimpleNamespace):
    pass


def log(message: str) -> None:
    info(message + "\n")


def python_cmd(script: str, argv: list[str]) -> str:
    args = " ".join([perf.shell_quote(str(PY_DIR / script))] +
                    [perf.shell_quote(arg) for arg in argv])
    return f"cd {perf.shell_quote(REPO)} && exec python3 {args}"


def python_gdb_cmd(script: str, argv: list[str]) -> str:
    args = " ".join([perf.shell_quote(str(PY_DIR / script))] +
                    [perf.shell_quote(arg) for arg in argv])
    return (
        f"cd {perf.shell_quote(REPO)} && exec gdb -batch "
        "-ex 'set pagination off' "
        "-ex run "
        "-ex 'thread apply all bt full' "
        f"--args python3 {args}"
    )


def start(node, name, cmd, env, procs):
    path = OUT / f"{name}.log"
    f = path.open("wb")
    log(f"start {name} on {node.name}: {cmd}")
    p = getPopen(node, cmd, envDict=env, shell=True, stdout=f, stderr=subprocess.STDOUT)
    procs.append((p, f, path))
    return p, path


def wait_log(path: Path, needle: str, timeout: int = 30, proc=None) -> bool:
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


def initialize_di_keychains(ndn, output_dir: Path) -> None:
    """Install root-signed keys that match the generated DI policy namespace."""
    log("Installing root-signed DI keychain material on MiniNDN nodes")
    security_dir = output_dir / "security"
    security_dir.mkdir(parents=True, exist_ok=True)
    identities = [
        CONTROLLER_IDENTITY,
        PROVIDER_PREFIX,
        PROVIDER_PREFIX + "/A",
        PROVIDER_PREFIX + "/B",
        PROVIDER_PREFIX + "/C",
        USER_IDENTITY,
    ]

    for node in ndn.net.hosts:
        for identity in [APP_ROOT] + identities:
            perf.node_cmd(node, "ndnsec delete {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(identity)))

    controller = ndn.net["memphis"]
    root_cert_path = security_dir / "root.cert"
    perf.node_cmd(controller, "ndnsec key-gen -t r {} > {}".format(
        perf.shell_quote(APP_ROOT), perf.shell_quote(root_cert_path)))
    perf.node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
        perf.shell_quote(root_cert_path)))
    log("di_root_cert identity={} name={} file={}".format(
        APP_ROOT, perf.certificate_name_from_file(root_cert_path), root_cert_path))

    exported_keys = []
    for index, identity in enumerate(identities):
        cert_path = security_dir / f"di-identity-{index}.cert"
        req_path = security_dir / f"di-identity-{index}.req"
        key_path = security_dir / f"di-identity-{index}.ndnkey"
        perf.node_cmd(controller, "ndnsec key-gen -n -t r {} > {}".format(
            perf.shell_quote(identity), perf.shell_quote(req_path)))
        perf.node_cmd(controller, "ndnsec cert-gen -s {} -i ROOT {} > {}".format(
            perf.shell_quote(APP_ROOT), perf.shell_quote(req_path), perf.shell_quote(cert_path)))
        perf.node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            perf.shell_quote(cert_path)))
        perf.node_cmd(controller, "ndnsec-export -P 123456 -o {} -i {}".format(
            perf.shell_quote(key_path), perf.shell_quote(identity)))
        exported_keys.append(key_path)

    for node in ndn.net.hosts:
        perf.node_cmd(node, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            perf.shell_quote(root_cert_path)))
        for key_path in exported_keys:
            perf.node_cmd(node, "ndnsec import -P 123456 {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(key_path)))


def main() -> None:
    setLogLevel("info")
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "python3",
        str(PY_DIR / "split_model.py"),
        "--out-dir",
        str(OUT / "model"),
        "--policy",
        str(CONFIG),
    ], cwd=str(REPO), check=True)
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPO))
    procs = []
    args = Args(
        controller_node="memphis",
        user_node="memphis",
        providers=4,
        provider_nodes="ucla,wustl,uiuc,csu",
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
        svs_disable_parallel_production=True,
        svs_parallel_production_workers=None,
        svs_disable_parallel_production_signing=True,
        svs_parallel_production_signing=False,
        svs_disable_parallel_production_extra_block=True,
        svs_parallel_production_extra_block=False,
        svs_sync_batching=False,
        svs_sync_batch_ms=0,
        ack_threads=-1,
        performance_mode=False,
    )
    try:
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, OUT)

        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin(
            [ndn.net["memphis"]],
            ["/NDNSF-DistributeInference/example/controller", "/NDNSF-DistributeInference/example/user", "/NDNSF-DistributeInference/example/group"],
        )
        origins = [
            ("ucla", "/NDNSF-DistributeInference/example/provider"),
            ("wustl", "/NDNSF-DistributeInference/example/provider/A"),
            ("uiuc", "/NDNSF-DistributeInference/example/provider/B"),
            ("csu", "/NDNSF-DistributeInference/example/provider/C"),
        ]
        for node_name, prefix in origins:
            rh.addOrigin([ndn.net[node_name]], [prefix, prefix + "/KEY", "/NDNSF-DistributeInference/example/group"])
        rh.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/NDNSF-DistributeInference/example", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF-DistributeInference/example/group", Nfdc.STRATEGY_MULTICAST)

        initialize_di_keychains(ndn, OUT)
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
        env.setdefault("OMP_NUM_THREADS", "1")
        env.setdefault("OPENBLAS_NUM_THREADS", "1")
        env.setdefault("MKL_NUM_THREADS", "1")
        env.setdefault("NUMEXPR_NUM_THREADS", "1")
        env.setdefault("TORCH_NUM_THREADS", "1")
        env["NDNSF_SVS_PARALLEL_SYNC"] = "0"

        common = ["--config", str(CONFIG), "--generated-policy-dir", GEN_POLICY]
        start(ndn.net["memphis"], "controller", python_cmd("controller.py", common), env, procs)
        time.sleep(2)

        providers = [
            ("ucla", "provider-s00", ["--role", "/Stage/0/Shard/0"]),
            ("wustl", "provider-s01", ["--provider-id", "A", "--role", "/Stage/0/Shard/1"]),
            ("uiuc", "provider-s10", ["--provider-id", "B", "--role", "/Stage/1/Shard/0"]),
            ("csu", "provider-s11", ["--provider-id", "C", "--role", "/Stage/1/Shard/1"]),
        ]
        for node_name, name, argv in providers:
            _, lp = start(ndn.net[node_name], name,
                          python_cmd("provider.py", common + argv + ["--temp-dir", f"/tmp/{name}"]),
                          env, procs)
            if not wait_log(lp, "permission", 8) and not lp.exists():
                raise RuntimeError(f"{name} did not start; see {lp}")
            time.sleep(0.5)

        time.sleep(4)
        async_requests = os.environ.get("NDNSF_DI_ASYNC_REQUESTS", "1")
        user_argv = common + [
            "--ack-timeout-ms", "1500",
            "--timeout-ms", "30000",
            "--async-requests", async_requests,
        ]
        user_command = python_cmd("user.py", user_argv)
        if os.environ.get("NDNSF_DI_GDB_USER") == "1":
            user_command = python_gdb_cmd("user.py", user_argv)
        user_proc, user_log = start(
            ndn.net["memphis"],
            "user",
            user_command,
            env,
            procs,
        )
        user_proc.wait(timeout=60)
        text = user_log.read_text(errors="replace")
        print(text)
        success = "PYTORCH_2X2_RESULT" in text and "ok=true" in text
        if not success:
            raise RuntimeError(f"PyTorch 2x2 failed rc={user_proc.returncode}; log={user_log}")
        if user_proc.returncode != 0:
            print(f"PYTORCH_2X2_MININDN_TEARDOWN_WARNING rc={user_proc.returncode} log={user_log}")
        print(f"PYTORCH_2X2_MININDN_OK log={user_log}")
    finally:
        stop(procs)
        ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    main()
