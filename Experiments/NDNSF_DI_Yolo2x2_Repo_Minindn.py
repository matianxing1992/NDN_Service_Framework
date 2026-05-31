#!/usr/bin/env python3
"""MiniNDN smoke test for a real YOLO 2x2 DistributedRepo deployment."""

from __future__ import annotations

import os
import re
import subprocess
import time
import sys
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from mininet.log import info, setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.apps.nlsr import Nlsr  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import getPopen  # noqa: E402

TOPO = REPO / "Experiments/Topology/testbed(loss=0%).conf"
OUT = REPO / "results/yolo_2x2_distributed_repo_minindn"
PY_DIR = REPO / "examples/python/NDNSF-DistributedInference/yolo_2x2"
MININDN_ROOT = Path("/tmp/minindn")


class Args(SimpleNamespace):
    pass


def log(message: str) -> None:
    info(message + "\n")


class CleanNlsr(Nlsr):
    def createConfigFile(self):
        super().createConfigFile()
        conf = Path(self.confFile)
        text = conf.read_text(encoding="utf-8")
        clean_block = (
            "advertising\n"
            "{\n"
            f"    /ndn/{self.node.name}-site/{self.node.name} 0\n"
            "}\n"
        )
        text = re.sub(
            r"advertising\s*\{.*?\}\n",
            clean_block,
            text,
            count=1,
            flags=re.S,
        )
        conf.write_text(text, encoding="utf-8")


def normalize_nlsr_link_costs(ndn) -> None:
    for host in ndn.net.hosts:
        for intf in host.intfList():
            delay = intf.params.get("delay")
            if not delay or not str(delay).endswith("ms"):
                continue
            value = str(delay)[:-2]
            try:
                intf.params["delay"] = f"{max(1, int(round(float(value))))}ms"
            except ValueError:
                pass


def main() -> None:
    setLogLevel("info")
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "pkill", "-f",
        "examples/python/NDNSF-DistributedInference/yolo_2x2/(repo_client|repo_node|controller)\\.py",
    ], check=False)
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPO))
    try:
        ndn.start()
        normalize_nlsr_link_costs(ndn)
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        AppManager(ndn, ndn.net.hosts, CleanNlsr, sync="psync", security=False,
                   faceType="udp", nFaces=3, routingType="link-state",
                   logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, OUT)

        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin([ndn.net["csu"]], ["/example/hello/controller"])
        rh.addOrigin([ndn.net["memphis"]], ["/example/hello/user"])
        rh.addOrigin([ndn.net["ucla"]], ["/example/hello/provider/repoA"])
        rh.addOrigin([ndn.net["wustl"]], ["/example/hello/provider/repoB"])
        rh.addOrigin([ndn.net["uiuc"]], ["/example/hello/provider/repoC"])
        rh.addOrigin(
            [ndn.net["ucla"], ndn.net["wustl"], ndn.net["uiuc"]],
            ["/NDNSF/DistributedRepo/Object"],
        )
        rh.addOrigin(ndn.net.hosts, ["/example/hello/group"])
        rh.calculateRoutes()
        log("Waiting 15s for NLSR base convergence")
        time.sleep(15.0)
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/example/hello", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/example/hello/group", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF/DistributedRepo/Object", Nfdc.STRATEGY_MULTICAST)

        identities = {
            "csu": "/example/hello/controller",
            "memphis": "/example/hello/user",
            "ucla": "/example/hello/provider/repoA",
            "wustl": "/example/hello/provider/repoB",
            "uiuc": "/example/hello/provider/repoC",
        }
        homes = {}
        for host_name, identity in identities.items():
            home = MININDN_ROOT / host_name
            ndn_dir = home / ".ndn"
            subprocess.run(["rm", "-rf", str(ndn_dir)], check=False)
            ndn_dir.mkdir(parents=True, exist_ok=True)
            (ndn_dir / "client.conf").write_text(
                f"transport=unix:///run/nfd/{host_name}.sock\n",
                encoding="utf-8",
            )
            homes[host_name] = home

        key_source = OUT / "identity-source"
        subprocess.run(["rm", "-rf", str(key_source)], check=False)
        key_source.mkdir(parents=True, exist_ok=True)
        (key_source / ".ndn").mkdir(parents=True, exist_ok=True)
        (key_source / ".ndn/client.conf").write_text("transport=unix:///run/nfd/csu.sock\n",
                                                     encoding="utf-8")
        root_identity = "/example/hello"
        root_cert = OUT / "root.cert"
        subprocess.run(
            "HOME={} NDN_CLIENT_CONF={} ndnsec key-gen -t r {} > {}; "
            "HOME={} NDN_CLIENT_CONF={} ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                perf.shell_quote(key_source),
                perf.shell_quote(key_source / ".ndn/client.conf"),
                perf.shell_quote(root_identity),
                perf.shell_quote(root_cert),
                perf.shell_quote(key_source),
                perf.shell_quote(key_source / ".ndn/client.conf"),
                perf.shell_quote(root_cert),
            ),
            shell=True,
            check=True,
        )
        certs = {}
        for host_name, identity in identities.items():
            req = OUT / f"{host_name}.req"
            cert = OUT / f"{host_name}.cert"
            perf.node_cmd(
                ndn.net[host_name],
                "HOME={} NDN_CLIENT_CONF={} ndnsec key-gen -n -t r {} > {}".format(
                    perf.shell_quote(homes[host_name]),
                    perf.shell_quote(homes[host_name] / ".ndn/client.conf"),
                    perf.shell_quote(identity),
                    perf.shell_quote(req)))
            subprocess.run(
                "HOME={} NDN_CLIENT_CONF={} ndnsec cert-gen -s {} -i ROOT {} > {}".format(
                    perf.shell_quote(key_source),
                    perf.shell_quote(key_source / ".ndn/client.conf"),
                    perf.shell_quote(root_identity),
                    perf.shell_quote(req),
                    perf.shell_quote(cert)),
                shell=True,
                check=True,
            )
            certs[host_name] = cert

        # Install the trust anchor and all public identity certificates
        # everywhere. Private keys stay in the keychain of the node that
        # generated each certificate request.
        for target_host in identities:
            perf.node_cmd(
                ndn.net[target_host],
                "HOME={} NDN_CLIENT_CONF={} ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(homes[target_host]),
                    perf.shell_quote(homes[target_host] / ".ndn/client.conf"),
                    perf.shell_quote(root_cert)))
            for cert in certs.values():
                perf.node_cmd(
                    ndn.net[target_host],
                    "HOME={} NDN_CLIENT_CONF={} ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                        perf.shell_quote(homes[target_host]),
                        perf.shell_quote(homes[target_host] / ".ndn/client.conf"),
                        perf.shell_quote(cert)))

        env = {
            **os.environ,
            "PYTHONPATH": ":".join([
                str(REPO / "NDNSF-DistributedInference"),
                str(REPO / "pythonWrapper"),
                os.environ.get("PYTHONPATH", ""),
            ]),
        }
        env.pop("NDN_CLIENT_TRANSPORT", None)
        processes = []

        def node_env(host_name: str):
            env_with_home = dict(env)
            # MiniNDN's Nfd app rewrites the Unix socket as /run/nfd/<node>.sock.
            # Some shells inherit a host-level NDN_CLIENT_TRANSPORT pointing at
            # /run/nfd/nfd.sock, so set the per-node value explicitly.
            env_with_home["NDN_CLIENT_TRANSPORT"] = f"unix:///run/nfd/{host_name}.sock"
            return {
                **env_with_home,
                "HOME": str(homes[host_name]),
                "NDN_CLIENT_CONF": str(homes[host_name] / ".ndn/client.conf"),
            }

        def start(host_name: str, label: str, cmd: str):
            log_path = OUT / f"{label}.log"
            log(f"start {label} on {host_name}: {cmd}")
            out = log_path.open("wb")
            proc = getPopen(ndn.net[host_name], cmd, envDict=node_env(host_name), shell=True,
                            stdout=out, stderr=subprocess.STDOUT)
            processes.append((proc, out, log_path))
            return proc, log_path

        base = f"cd {perf.shell_quote(REPO)} && exec python3 "
        controller, _ = start(
            "csu",
            "controller",
            base + perf.shell_quote(PY_DIR / "controller.py"),
        )
        time.sleep(5.0)
        start(
            "ucla",
            "repoA",
            base + perf.shell_quote(PY_DIR / "repo_node.py") +
            " --provider-id repoA --repo-node /example/hello/provider/repoA "
            "--failure-domain rack-a "
            f"--storage-dir {MININDN_ROOT}/ucla/repo-store "
            "--advertise-stored-prefixes",
        )
        start(
            "wustl",
            "repoB",
            base + perf.shell_quote(PY_DIR / "repo_node.py") +
            " --provider-id repoB --repo-node /example/hello/provider/repoB "
            "--failure-domain rack-b "
            f"--storage-dir {MININDN_ROOT}/wustl/repo-store "
            "--advertise-stored-prefixes",
        )
        start(
            "uiuc",
            "repoC",
            base + perf.shell_quote(PY_DIR / "repo_node.py") +
            " --provider-id repoC --repo-node /example/hello/provider/repoC "
            "--failure-domain rack-c "
            f"--storage-dir {MININDN_ROOT}/uiuc/repo-store "
            "--advertise-stored-prefixes",
        )
        time.sleep(15.0)
        client_cmd = (
            base + perf.shell_quote(PY_DIR / "repo_client.py") +
            " --model yolo26n.pt --replication-factor 2 "
            "--ack-timeout-ms 5000 --timeout-ms 60000"
        )
        client_log = OUT / "repo-client.log"
        out = client_log.open("wb")
        client = getPopen(ndn.net["memphis"], client_cmd, envDict=node_env("memphis"),
                          shell=True, stdout=out, stderr=subprocess.STDOUT)
        processes.append((client, out, client_log))
        client.wait(timeout=900)
        text = client_log.read_text(errors="replace")
        print(text)
        if client.returncode != 0 or "YOLO_2X2_REAL_REPO_OK" not in text:
            raise RuntimeError(
                f"YOLO 2x2 real DistributedRepo failed rc={client.returncode}; "
                f"log={client_log}")
        print(f"YOLO_2X2_DISTRIBUTED_REPO_MININDN_OK log={client_log}")
        for proc, out, _ in processes:
            proc.terminate()
            out.close()
    finally:
        for proc, out, _ in locals().get("processes", []):
            try:
                proc.terminate()
            except Exception:
                pass
            try:
                out.close()
            except Exception:
                pass
        ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    main()
