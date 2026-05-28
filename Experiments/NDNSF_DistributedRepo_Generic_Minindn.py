#!/usr/bin/env python3
"""MiniNDN smoke test for the generic NDNSF-DistributedRepo API."""

from __future__ import annotations

import os
import re
import subprocess
import time
import sys
from pathlib import Path

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
OUT = REPO / "results/distributed_repo_generic_minindn"
PY_DIR = REPO / "examples/python/NDNSF-DistributedRepo/generic_object_store"


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
        "examples/python/NDNSF-DistributedRepo/generic_object_store/"
        "(client|repo_node|controller)\\.py",
    ], check=False)
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=str(TOPO))
    processes = []
    try:
        ndn.start()
        normalize_nlsr_link_costs(ndn)
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        AppManager(ndn, ndn.net.hosts, CleanNlsr, sync="psync", security=False,
                   faceType="udp", nFaces=3, routingType="link-state",
                   logLevel="INFO")
        perf.wait_for_nfd_sockets(ndn, OUT)

        rh = NdnRoutingHelper(ndn.net, "udp", "link-state")
        rh.addOrigin([ndn.net["csu"]], ["/example/repo/controller"])
        rh.addOrigin([ndn.net["memphis"]], ["/example/repo/user"])
        rh.addOrigin([ndn.net["ucla"]], ["/example/repo/provider/repoA"])
        rh.addOrigin([ndn.net["wustl"]], ["/example/repo/provider/repoB"])
        rh.addOrigin([ndn.net["uiuc"]], ["/example/repo/provider/repoC"])
        rh.addOrigin(
            [ndn.net["ucla"], ndn.net["wustl"], ndn.net["uiuc"]],
            ["/NDNSF/DistributedRepo/Object"],
        )
        rh.addOrigin(ndn.net.hosts, ["/example/repo/group"])
        rh.calculateRoutes()
        log("Waiting 15s for NLSR base convergence")
        time.sleep(15.0)
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/example/repo", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/example/repo/group", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF/DistributedRepo/Object", Nfdc.STRATEGY_MULTICAST)

        identities = {
            "csu": "/example/repo/controller",
            "memphis": "/example/repo/user",
            "ucla": "/example/repo/provider/repoA",
            "wustl": "/example/repo/provider/repoB",
            "uiuc": "/example/repo/provider/repoC",
        }
        homes = {}
        for host_name, identity in identities.items():
            home = Path("/tmp/minindn") / host_name
            ndn_dir = home / ".ndn"
            subprocess.run(["rm", "-rf", str(ndn_dir)], check=False)
            ndn_dir.mkdir(parents=True, exist_ok=True)
            client_conf = ndn_dir / "client.conf"
            client_conf.write_text(
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
        passphrase = "ndnsf-minindn"
        for host_name, identity in identities.items():
            bag = OUT / f"{host_name}.safebag"
            subprocess.run(
                "HOME={} NDN_CLIENT_CONF={} ndnsec key-gen -t r {} >/dev/null 2>&1 || true; "
                "HOME={} NDN_CLIENT_CONF={} ndnsec export -P {} -o {} -i {}".format(
                    perf.shell_quote(key_source),
                    perf.shell_quote(key_source / ".ndn/client.conf"),
                    perf.shell_quote(identity),
                    perf.shell_quote(key_source),
                    perf.shell_quote(key_source / ".ndn/client.conf"),
                    perf.shell_quote(passphrase),
                    perf.shell_quote(bag),
                    perf.shell_quote(identity),
                ),
                shell=True,
                check=True,
            )
            target_hosts = set(identities) if host_name == "csu" else {"csu", host_name}
            for target_host in target_hosts:
                perf.node_cmd(
                    ndn.net[target_host],
                    "HOME={} NDN_CLIENT_CONF={} ndnsec import -P {} {} >/dev/null 2>&1 || true".format(
                        perf.shell_quote(homes[target_host]),
                        perf.shell_quote(homes[target_host] / ".ndn/client.conf"),
                        perf.shell_quote(passphrase),
                        perf.shell_quote(bag)))

        env = {
            **os.environ,
            "PYTHONFAULTHANDLER": "1",
            "PYTHONPATH": ":".join([
                str(REPO / "NDNSF-DistributedInference"),
                str(REPO / "pythonWrapper"),
                os.environ.get("PYTHONPATH", ""),
            ]),
        }
        env.pop("NDN_CLIENT_TRANSPORT", None)

        def node_env(host_name: str):
            return {
                **env,
                "HOME": str(homes[host_name]),
                "NDN_CLIENT_CONF": str(homes[host_name] / ".ndn/client.conf"),
                "NDN_CLIENT_TRANSPORT": f"unix:///run/nfd/{host_name}.sock",
            }

        def start(host_name: str, label: str, cmd: str):
            log_path = OUT / f"{label}.log"
            log(f"start {label} on {host_name}: {cmd}")
            out = log_path.open("wb")
            proc = getPopen(ndn.net[host_name], cmd, envDict=node_env(host_name),
                            shell=True, stdout=out, stderr=subprocess.STDOUT)
            processes.append((proc, out, log_path))
            return proc, log_path

        base = f"cd {perf.shell_quote(REPO)} && exec python3 "
        start("csu", "controller", base + perf.shell_quote(PY_DIR / "controller.py"))
        time.sleep(5.0)
        start(
            "ucla",
            "repoA",
            base + perf.shell_quote(PY_DIR / "repo_node.py") +
            " --provider-id repoA --repo-node /example/repo/provider/repoA "
            "--failure-domain rack-a "
            "--storage-dir /tmp/minindn/ucla/repo-store "
            "--advertise-stored-prefixes",
        )
        start(
            "wustl",
            "repoB",
            base + perf.shell_quote(PY_DIR / "repo_node.py") +
            " --provider-id repoB --repo-node /example/repo/provider/repoB "
            "--failure-domain rack-b "
            "--storage-dir /tmp/minindn/wustl/repo-store "
            "--advertise-stored-prefixes",
        )
        start(
            "uiuc",
            "repoC",
            base + perf.shell_quote(PY_DIR / "repo_node.py") +
            " --provider-id repoC --repo-node /example/repo/provider/repoC "
            "--failure-domain rack-c "
            "--storage-dir /tmp/minindn/uiuc/repo-store "
            "--advertise-stored-prefixes",
        )
        time.sleep(15.0)
        client_log = OUT / "client.log"
        out = client_log.open("wb")
        client = getPopen(
            ndn.net["memphis"],
            base + perf.shell_quote(PY_DIR / "client.py") +
            " --ack-timeout-ms 5000",
            envDict=node_env("memphis"),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((client, out, client_log))
        client.wait(timeout=180)
        text = client_log.read_text(errors="replace")
        print(text)
        if client.returncode != 0 or "GENERIC_DISTRIBUTED_REPO_OK" not in text:
            raise RuntimeError(
                f"generic DistributedRepo failed rc={client.returncode}; "
                f"log={client_log}")
        print(f"GENERIC_DISTRIBUTED_REPO_MININDN_OK log={client_log}")
    finally:
        for proc, out, _ in processes:
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
