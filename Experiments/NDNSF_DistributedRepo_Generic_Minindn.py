#!/usr/bin/env python3
"""MiniNDN smoke test for the generic NDNSF-DistributedRepo API."""

from __future__ import annotations

import os
import re
import subprocess
import time
import sys
from pathlib import Path
import yaml  # type: ignore

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
CONFIG = PY_DIR / "repo_policy.yaml"
RUNTIME_CONFIG = OUT / "repo_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-distributed-repo-generic-policy"


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
        rh.addOrigin(
            [ndn.net["csu"]],
            [
                "/example/repo/controller",
                "/example/repo/controller/DKEY",
                "/example/repo/controller/KEY",
                "/example/repo",
                "/example/repo/KEY",
            ],
        )
        rh.addOrigin(
            [ndn.net["memphis"]],
            ["/example/repo/user", "/example/repo/user/KEY"],
        )
        rh.addOrigin(
            [ndn.net["ucla"]],
            ["/example/repo/provider/repoA", "/example/repo/provider/repoA/KEY"],
        )
        rh.addOrigin(
            [ndn.net["wustl"]],
            ["/example/repo/provider/repoB", "/example/repo/provider/repoB/KEY"],
        )
        rh.addOrigin(
            [ndn.net["uiuc"]],
            ["/example/repo/provider/repoC", "/example/repo/provider/repoC/KEY"],
        )
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
        root_identity = "/example/repo"
        root_cert = OUT / "root.cert"
        root_bag = OUT / "root.safebag"
        subprocess.run(
            "HOME={} NDN_CLIENT_CONF={} ndnsec key-gen -t r {} > {}; "
            "HOME={} NDN_CLIENT_CONF={} ndnsec cert-install -f {} >/dev/null 2>&1 || true; "
            "HOME={} NDN_CLIENT_CONF={} ndnsec export -P {} -o {} -i {}".format(
                perf.shell_quote(key_source),
                perf.shell_quote(key_source / ".ndn/client.conf"),
                perf.shell_quote(root_identity),
                perf.shell_quote(root_cert),
                perf.shell_quote(key_source),
                perf.shell_quote(key_source / ".ndn/client.conf"),
                perf.shell_quote(root_cert),
                perf.shell_quote(key_source),
                perf.shell_quote(key_source / ".ndn/client.conf"),
                perf.shell_quote(passphrase),
                perf.shell_quote(root_bag),
                perf.shell_quote(root_identity),
            ),
            shell=True,
            check=True,
        )
        cert_paths = {}
        bag_paths = {}
        for host_name, identity in identities.items():
            bag = OUT / f"{host_name}.safebag"
            req = OUT / f"{host_name}.req"
            cert = OUT / f"{host_name}.cert"
            subprocess.run(
                "HOME={} NDN_CLIENT_CONF={} ndnsec key-gen -n -t r {} > {}; "
                "HOME={} NDN_CLIENT_CONF={} ndnsec cert-gen -s {} -i ROOT {} > {}; "
                "HOME={} NDN_CLIENT_CONF={} ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(key_source),
                    perf.shell_quote(key_source / ".ndn/client.conf"),
                    perf.shell_quote(identity),
                    perf.shell_quote(req),
                    perf.shell_quote(key_source),
                    perf.shell_quote(key_source / ".ndn/client.conf"),
                    perf.shell_quote(root_identity),
                    perf.shell_quote(req),
                    perf.shell_quote(cert),
                    perf.shell_quote(key_source),
                    perf.shell_quote(key_source / ".ndn/client.conf"),
                    perf.shell_quote(cert),
                ),
                shell=True,
                check=True,
            )
            cert_name = perf.certificate_name_from_file(cert)
            subprocess.run(
                "HOME={} NDN_CLIENT_CONF={} ndnsec set-default -c -n {}".format(
                    perf.shell_quote(key_source),
                    perf.shell_quote(key_source / ".ndn/client.conf"),
                    perf.shell_quote(cert_name),
                ),
                shell=True,
                check=True,
            )
            cert_listing = subprocess.check_output(
                "HOME={} NDN_CLIENT_CONF={} ndnsec list -c".format(
                    perf.shell_quote(key_source),
                    perf.shell_quote(key_source / ".ndn/client.conf"),
                ),
                shell=True,
                text=True,
            )
            for match in re.finditer(
                r"({}/KEY/\S+/self/v=\S+)".format(re.escape(identity)),
                cert_listing,
            ):
                subprocess.run(
                    "HOME={} NDN_CLIENT_CONF={} ndnsec delete -c -n {} >/dev/null 2>&1 || true".format(
                        perf.shell_quote(key_source),
                        perf.shell_quote(key_source / ".ndn/client.conf"),
                        perf.shell_quote(match.group(1)),
                    ),
                    shell=True,
                    check=False,
                )
            subprocess.run(
                "HOME={} NDN_CLIENT_CONF={} ndnsec export -P {} -o {} -i {}".format(
                    perf.shell_quote(key_source),
                    perf.shell_quote(key_source / ".ndn/client.conf"),
                    perf.shell_quote(passphrase),
                    perf.shell_quote(bag),
                    perf.shell_quote(identity),
                ),
                shell=True,
                check=True,
            )
            cert_paths[host_name] = cert
            bag_paths[host_name] = bag

        for target_host in identities:
            perf.node_cmd(
                ndn.net[target_host],
                "HOME={} NDN_CLIENT_CONF={} ndnsec import -P {} {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(homes[target_host]),
                    perf.shell_quote(homes[target_host] / ".ndn/client.conf"),
                    perf.shell_quote(passphrase),
                    perf.shell_quote(root_bag)))
            for bag in bag_paths.values():
                perf.node_cmd(
                    ndn.net[target_host],
                    "HOME={} NDN_CLIENT_CONF={} ndnsec import -P {} {} >/dev/null 2>&1 || true".format(
                        perf.shell_quote(homes[target_host]),
                        perf.shell_quote(homes[target_host] / ".ndn/client.conf"),
                        perf.shell_quote(passphrase),
                        perf.shell_quote(bag)))
            perf.node_cmd(
                ndn.net[target_host],
                "HOME={} NDN_CLIENT_CONF={} ndnsec set-default -n {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(homes[target_host]),
                    perf.shell_quote(homes[target_host] / ".ndn/client.conf"),
                    perf.shell_quote(identities[target_host])))

        config_obj = yaml.safe_load(CONFIG.read_text(encoding="utf-8"))
        config_obj.setdefault("trust", {})["anchor_file"] = str(root_cert)
        RUNTIME_CONFIG.write_text(yaml.safe_dump(config_obj, sort_keys=False),
                                  encoding="utf-8")

        env = {
            **os.environ,
            "PYTHONFAULTHANDLER": "1",
            "PYTHONPATH": ":".join([
                str(REPO / "NDNSF-DistributedInference"),
                str(REPO / "pythonWrapper"),
                os.environ.get("PYTHONPATH", ""),
            ]),
            "PYTHONUNBUFFERED": "1",
            "NDN_LOG": "ndn_service_framework.*=INFO",
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
        common = (
            " --config {} --generated-policy-dir {}".format(
                perf.shell_quote(RUNTIME_CONFIG),
                perf.shell_quote(GEN_POLICY),
            )
        )
        start("csu", "controller", base + perf.shell_quote(PY_DIR / "controller.py") + common)
        time.sleep(15.0)
        start(
            "ucla",
            "repoA",
            base + perf.shell_quote(PY_DIR / "repo_node.py") + common +
            " --provider-id repoA --repo-node /example/repo/provider/repoA "
            "--failure-domain rack-a "
            "--storage-dir /tmp/minindn/ucla/repo-store "
            "--advertise-stored-prefixes",
        )
        start(
            "wustl",
            "repoB",
            base + perf.shell_quote(PY_DIR / "repo_node.py") + common +
            " --provider-id repoB --repo-node /example/repo/provider/repoB "
            "--failure-domain rack-b "
            "--storage-dir /tmp/minindn/wustl/repo-store "
            "--advertise-stored-prefixes",
        )
        start(
            "uiuc",
            "repoC",
            base + perf.shell_quote(PY_DIR / "repo_node.py") + common +
            " --provider-id repoC --repo-node /example/repo/provider/repoC "
            "--failure-domain rack-c "
            "--storage-dir /tmp/minindn/uiuc/repo-store "
            "--advertise-stored-prefixes",
        )
        time.sleep(25.0)
        client_log = OUT / "client.log"
        out = client_log.open("wb")
        client = getPopen(
            ndn.net["memphis"],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --trust-schema {} --ack-timeout-ms 5000".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf")),
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
