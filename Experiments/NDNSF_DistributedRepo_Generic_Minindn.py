#!/usr/bin/env python3
"""MiniNDN smoke test for the generic NDNSF-DistributedRepo API."""

from __future__ import annotations

import os
import re
import subprocess
import time
import sys
import argparse
from pathlib import Path
import yaml  # type: ignore

REPO = Path(__file__).resolve().parents[1]
MININDN_ROOT = Path("/tmp/minindn")
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

TOPO = REPO / "Experiments/Topology/AI_Lab.conf"
OUT = REPO / "results/distributed_repo_generic_minindn"
PY_DIR = REPO / "examples/python/NDNSF-DistributedRepo/generic_object_store"
CONFIG = PY_DIR / "repo_policy.yaml"
RUNTIME_CONFIG = OUT / "repo_policy.yaml"
GEN_POLICY = "/tmp/ndnsf-distributed-repo-generic-policy"
CONTROLLER_NODE = "memphis"
USER_NODE = "neu"
REPO_A_NODE = "ucla"
REPO_B_NODE = "wustl"
REPO_C_NODE = "arizona"


def log(message: str) -> None:
    info(message + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="MiniNDN smoke test for the generic NDNSF-DistributedRepo API")
    parser.add_argument("--topology-file", default=str(TOPO),
                        help="MiniNDN topology file. Defaults to AI_Lab.conf.")
    parser.add_argument("--output-dir", default=str(OUT))
    parser.add_argument("--quick-smoke", action="store_true",
                        help="Run only the core store/fetch/catalog/delete smoke.")
    parser.add_argument("--nlsr-wait-s", type=float, default=15.0)
    parser.add_argument("--repo-start-wait-s", type=float, default=25.0)
    return parser


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


def key_name_from_certificate_name(cert_name: str) -> str:
    return cert_name.rsplit("/", 2)[0]


def main() -> None:
    global OUT, RUNTIME_CONFIG
    args = build_parser().parse_args()
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    OUT = Path(args.output_dir).expanduser().resolve()
    RUNTIME_CONFIG = OUT / "repo_policy.yaml"
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        "pkill", "-f",
        "examples/python/NDNSF-DistributedRepo/generic_object_store/"
        "(client|repo_node|controller)\\.py",
    ], check=False)
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    ndn = Minindn(topoFile=args.topology_file)
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
            [ndn.net[CONTROLLER_NODE]],
            [
                "/example/repo/controller",
                "/example/repo/controller/DKEY",
                "/example/repo/controller/KEY",
                "/example/repo",
                "/example/repo/KEY",
            ],
        )
        rh.addOrigin(
            [ndn.net[USER_NODE]],
            ["/example/repo/user", "/example/repo/user/KEY"],
        )
        rh.addOrigin(
            [ndn.net[REPO_A_NODE]],
            ["/example/repo/provider/repoA", "/example/repo/provider/repoA/KEY"],
        )
        rh.addOrigin(
            [ndn.net[REPO_B_NODE]],
            ["/example/repo/provider/repoB", "/example/repo/provider/repoB/KEY"],
        )
        rh.addOrigin(
            [ndn.net[REPO_C_NODE]],
            ["/example/repo/provider/repoC", "/example/repo/provider/repoC/KEY"],
        )
        rh.addOrigin(
            [ndn.net[REPO_A_NODE], ndn.net[REPO_B_NODE], ndn.net[REPO_C_NODE]],
            ["/NDNSF/DistributedRepo/Object"],
        )
        rh.addOrigin(ndn.net.hosts, ["/example/repo/group"])
        rh.calculateRoutes()
        log(f"Waiting {args.nlsr_wait_s:.1f}s for NLSR base convergence")
        time.sleep(args.nlsr_wait_s)
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/example/repo", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/example/repo/group", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(node, "/NDNSF/DistributedRepo/Object", Nfdc.STRATEGY_MULTICAST)

        node_identities = [
            (CONTROLLER_NODE, "/example/repo/controller"),
            (USER_NODE, "/example/repo/user"),
            (REPO_A_NODE, "/example/repo/provider/repoA"),
            (REPO_B_NODE, "/example/repo/provider/repoB"),
            (REPO_C_NODE, "/example/repo/provider/repoC"),
        ]
        identities = dict(node_identities)
        homes = {}
        for host_name in sorted(set(identities)):
            home = MININDN_ROOT / host_name
            ndn_dir = home / ".ndn"
            subprocess.run(["rm", "-rf", str(ndn_dir)], check=False)
            ndn_dir.mkdir(parents=True, exist_ok=True)
            client_conf = ndn_dir / "client.conf"
            client_conf.write_text(
                f"transport=unix:///run/nfd/{host_name}.sock\n",
                encoding="utf-8",
            )
            homes[host_name] = home

        passphrase = "ndnsf-minindn"
        root_identity = "/example/repo"
        root_cert = OUT / "root.cert"
        controller_node = ndn.net[CONTROLLER_NODE]
        for node in ndn.net.hosts:
            for identity in [root_identity] + [item[1] for item in node_identities]:
                perf.node_cmd(node, "ndnsec delete {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(identity)))
        perf.node_cmd(controller_node, "ndnsec key-gen -t r {} > {}".format(
            perf.shell_quote(root_identity), perf.shell_quote(root_cert)))
        perf.node_cmd(controller_node,
                      "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                          perf.shell_quote(root_cert)))

        exported_keys = []
        cert_names = {}
        for index, (host_name, identity) in enumerate(node_identities):
            req = OUT / f"{host_name}-{index}.req"
            cert = OUT / f"{host_name}.cert"
            key = OUT / f"{host_name}-{index}.ndnkey"
            perf.node_cmd(controller_node, "ndnsec key-gen -t r {} > {}".format(
                perf.shell_quote(identity), perf.shell_quote(req)))
            perf.node_cmd(controller_node, "ndnsec cert-gen -s {} -i ROOT {} > {}".format(
                perf.shell_quote(root_identity), perf.shell_quote(req), perf.shell_quote(cert)))
            perf.node_cmd(controller_node,
                          "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(cert)))
            cert_name = perf.certificate_name_from_file(cert)
            cert_names[host_name] = cert_name
            key_name = key_name_from_certificate_name(cert_name)
            perf.node_cmd(controller_node,
                          "ndnsec set-default -k -n {} >/dev/null 2>&1 || true".format(
                              perf.shell_quote(key_name)))
            perf.node_cmd(controller_node, "ndnsec-export -P {} -o {} -k {}".format(
                perf.shell_quote(passphrase), perf.shell_quote(key), perf.shell_quote(key_name)))
            exported_keys.append((host_name, identity, key, key_name))

        for target_host in sorted(set(identities)):
            perf.node_cmd(
                ndn.net[target_host],
                "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(root_cert)))
            for _, _, key, _ in exported_keys:
                perf.node_cmd(
                    ndn.net[target_host],
                    "ndnsec import -P {} {} >/dev/null 2>&1 || true".format(
                        perf.shell_quote(passphrase), perf.shell_quote(key)))
            perf.node_cmd(
                ndn.net[target_host],
                "ndnsec set-default -n {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(identities[target_host])))
            perf.node_cmd(
                ndn.net[target_host],
                "ndnsec set-default -c -n {} >/dev/null 2>&1 || true".format(
                    perf.shell_quote(cert_names[target_host])))

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
            "NDNSF_RESPONSE_LARGE_DATA_THRESHOLD": "1024",
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
        start(CONTROLLER_NODE, "controller", base + perf.shell_quote(PY_DIR / "controller.py") + common)
        time.sleep(15.0)
        repoA_proc, _ = start(
            REPO_A_NODE,
            "repoA",
            base + perf.shell_quote(PY_DIR / "repo_node.py") + common +
            " --provider-id repoA --repo-node /example/repo/provider/repoA "
            "--failure-domain rack-a "
            f"--storage-dir {MININDN_ROOT}/{REPO_A_NODE}/repo-store "
            "--advertise-stored-prefixes",
        )
        repoB_proc, _ = start(
            REPO_B_NODE,
            "repoB",
            base + perf.shell_quote(PY_DIR / "repo_node.py") + common +
            " --provider-id repoB --repo-node /example/repo/provider/repoB "
            "--failure-domain rack-b "
            f"--storage-dir {MININDN_ROOT}/{REPO_B_NODE}/repo-store "
            "--advertise-stored-prefixes",
        )
        repoC_proc, _ = start(
            REPO_C_NODE,
            "repoC",
            base + perf.shell_quote(PY_DIR / "repo_node.py") + common +
            " --provider-id repoC --repo-node /example/repo/provider/repoC "
            "--failure-domain rack-c "
            f"--storage-dir {MININDN_ROOT}/{REPO_C_NODE}/repo-store "
            "--advertise-stored-prefixes",
        )
        catalogA_proc, _ = start(
            REPO_A_NODE,
            "catalogA",
            base + perf.shell_quote(PY_DIR / "catalog_sync.py") + common +
            " --repo-node /example/repo/provider/repoA "
            "--peer-repo-node /example/repo/provider/repoB "
            "--peer-repo-node /example/repo/provider/repoC "
            "--interval-s 10",
        )
        catalogB_proc, _ = start(
            REPO_B_NODE,
            "catalogB",
            base + perf.shell_quote(PY_DIR / "catalog_sync.py") + common +
            " --repo-node /example/repo/provider/repoB "
            "--peer-repo-node /example/repo/provider/repoA "
            "--peer-repo-node /example/repo/provider/repoC "
            "--interval-s 10",
        )
        catalogC_proc, _ = start(
            REPO_C_NODE,
            "catalogC",
            base + perf.shell_quote(PY_DIR / "catalog_sync.py") + common +
            " --repo-node /example/repo/provider/repoC "
            "--peer-repo-node /example/repo/provider/repoA "
            "--peer-repo-node /example/repo/provider/repoB "
            "--interval-s 10",
        )
        log(f"Waiting {args.repo_start_wait_s:.1f}s for repo providers")
        time.sleep(args.repo_start_wait_s)
        client_log = OUT / "client.log"
        out = client_log.open("wb")
        quick_client_args = (
            " --use-local-config --quick-core-smoke"
            if args.quick_smoke else
            " --test-delete"
        )
        client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --trust-schema {} --ack-timeout-ms 8000{}".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf"),
                quick_client_args,
            ),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((client, out, client_log))
        client.wait(timeout=360)
        text = client_log.read_text(errors="replace")
        print(text)
        if args.quick_smoke:
            if (client.returncode != 0 or
                    "GENERIC_DISTRIBUTED_REPO_QUICK_CORE_OK" not in text):
                raise RuntimeError(
                    f"generic DistributedRepo quick smoke failed "
                    f"rc={client.returncode}; log={client_log}")
            print(f"GENERIC_DISTRIBUTED_REPO_QUICK_MININDN_OK log={client_log}")
            return
        if (client.returncode != 0 or
                "GENERIC_DISTRIBUTED_REPO_CATALOG_GOSSIP_OK" not in text or
                "GENERIC_DISTRIBUTED_REPO_CATALOG_TOMBSTONE_OK" not in text or
                "GENERIC_DISTRIBUTED_REPO_OK" not in text):
            raise RuntimeError(
                f"generic DistributedRepo failed rc={client.returncode}; "
                f"log={client_log}")
        policy_log = OUT / "client-object-policy.log"
        out = policy_log.open("wb")
        policy_client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
            "--object-policy-smoke".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf")),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((policy_client, out, policy_log))
        policy_client.wait(timeout=240)
        policy_text = policy_log.read_text(errors="replace")
        print(policy_text)
        if (policy_client.returncode != 0 or
                "GENERIC_DISTRIBUTED_REPO_OBJECT_POLICY_OK" not in policy_text):
            raise RuntimeError(
                f"generic DistributedRepo object policy failed "
                f"rc={policy_client.returncode}; log={policy_log}")
        tombstone_gossip_log = OUT / "client-tombstone-gossip.log"
        out = tombstone_gossip_log.open("wb")
        tombstone_gossip_client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
            "--catalog-tombstone-gossip-smoke "
            "--catalog-tombstone-source-repo-node /example/repo/provider/repoA "
            "--catalog-tombstone-peer-repo-node /example/repo/provider/repoB "
            "--catalog-tombstone-peer-repo-node /example/repo/provider/repoC".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf")),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((tombstone_gossip_client, out, tombstone_gossip_log))
        tombstone_gossip_client.wait(timeout=240)
        tombstone_gossip_text = tombstone_gossip_log.read_text(errors="replace")
        print(tombstone_gossip_text)
        if (tombstone_gossip_client.returncode != 0 or
                "GENERIC_DISTRIBUTED_REPO_TOMBSTONE_GOSSIP_OK"
                not in tombstone_gossip_text):
            raise RuntimeError(
                f"generic DistributedRepo tombstone gossip failed "
                f"rc={tombstone_gossip_client.returncode}; "
                f"log={tombstone_gossip_log}")
        tombstone_epoch_log = OUT / "client-tombstone-epoch-conflict.log"
        out = tombstone_epoch_log.open("wb")
        tombstone_epoch_client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
            "--catalog-tombstone-epoch-conflict-smoke "
            "--catalog-tombstone-source-repo-node /example/repo/provider/repoA "
            "--catalog-tombstone-peer-repo-node /example/repo/provider/repoB "
            "--catalog-tombstone-peer-repo-node /example/repo/provider/repoC".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf")),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((tombstone_epoch_client, out, tombstone_epoch_log))
        tombstone_epoch_client.wait(timeout=240)
        tombstone_epoch_text = tombstone_epoch_log.read_text(errors="replace")
        print(tombstone_epoch_text)
        if (tombstone_epoch_client.returncode != 0 or
                "GENERIC_DISTRIBUTED_REPO_TOMBSTONE_EPOCH_CONFLICT_OK"
                not in tombstone_epoch_text):
            raise RuntimeError(
                f"generic DistributedRepo tombstone epoch conflict failed "
                f"rc={tombstone_epoch_client.returncode}; "
                f"log={tombstone_epoch_log}")
        uav_data_log = OUT / "client-uav-data-product.log"
        out = uav_data_log.open("wb")
        uav_data_client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
            "--uav-data-product-smoke".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf")),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((uav_data_client, out, uav_data_log))
        uav_data_client.wait(timeout=240)
        uav_data_text = uav_data_log.read_text(errors="replace")
        print(uav_data_text)
        if (uav_data_client.returncode != 0 or
                "GENERIC_DISTRIBUTED_REPO_UAV_DATA_PRODUCT_OK"
                not in uav_data_text or
                "GENERIC_DISTRIBUTED_REPO_CATALOG_QUERY_OK"
                not in uav_data_text):
            raise RuntimeError(
                f"generic DistributedRepo UAV data product failed "
                f"rc={uav_data_client.returncode}; log={uav_data_log}")
        uav_browse_log = OUT / "client-uav-browse.log"
        out = uav_browse_log.open("wb")
        uav_browse_client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
            "--uav-browse-smoke --uav-browse-repo-node /example/repo/provider/repoA".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf")),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((uav_browse_client, out, uav_browse_log))
        uav_browse_client.wait(timeout=180)
        uav_browse_text = uav_browse_log.read_text(errors="replace")
        print(uav_browse_text)
        if (uav_browse_client.returncode != 0 or
                "GENERIC_DISTRIBUTED_REPO_UAV_BROWSE_OK"
                not in uav_browse_text):
            raise RuntimeError(
                f"generic DistributedRepo UAV browse failed "
                f"rc={uav_browse_client.returncode}; log={uav_browse_log}")
        snapshot_log = OUT / "client-catalog-snapshot.log"
        out = snapshot_log.open("wb")
        snapshot_client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
            "--catalog-snapshot-large-response-smoke "
            "--catalog-snapshot-repo-node /example/repo/provider/repoA".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf")),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((snapshot_client, out, snapshot_log))
        snapshot_client.wait(timeout=180)
        snapshot_text = snapshot_log.read_text(errors="replace")
        print(snapshot_text)
        repo_a_text = (OUT / "repoA.log").read_text(errors="replace")
        if (snapshot_client.returncode != 0 or
                "GENERIC_DISTRIBUTED_REPO_CATALOG_SNAPSHOT_LARGE_RESPONSE_OK"
                not in snapshot_text):
            raise RuntimeError(
                f"generic DistributedRepo catalog snapshot failed "
                f"rc={snapshot_client.returncode}; log={snapshot_log}")
        if "GENERIC_DISTRIBUTED_REPO_CORE_10KB_RESPONSE_CALLBACK_OK" not in snapshot_text:
            raise RuntimeError(
                "snapshot client did not receive a 10KB+ original response payload "
                f"through the user callback; log={snapshot_log}")
        if "LARGE_RESPONSE_REFERENCE_PUBLISHED" not in repo_a_text:
            raise RuntimeError(
                "repoA did not publish catalog snapshot through Core "
                f"large-response reference; log={OUT / 'repoA.log'}")
        if "LARGE_RESPONSE_REFERENCE_RESOLVED" not in snapshot_text:
            raise RuntimeError(
                "snapshot client did not resolve Core large-response reference; "
                f"log={snapshot_log}")
        print("GENERIC_DISTRIBUTED_REPO_CORE_LARGE_RESPONSE_REFERENCE_OK")
        for proc in (repoC_proc, catalogC_proc):
            proc.terminate()
        log("Waiting 35s for repoC catalog entries to become stale")
        time.sleep(35.0)
        health_log = OUT / "client-catalog-health.log"
        out = health_log.open("wb")
        health_client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
            "--catalog-health-smoke "
            "--catalog-health-repo-node /example/repo/provider/repoA "
            "--catalog-health-stale-repo /example/repo/provider/repoC".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf")),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((health_client, out, health_log))
        health_client.wait(timeout=240)
        health_text = health_log.read_text(errors="replace")
        print(health_text)
        if (health_client.returncode != 0 or
                "GENERIC_DISTRIBUTED_REPO_CATALOG_REPAIR_OK" not in health_text or
                "GENERIC_DISTRIBUTED_REPO_CATALOG_HEALTH_OK" not in health_text):
            raise RuntimeError(
                "generic DistributedRepo catalog health check failed "
                f"rc={health_client.returncode}; log={health_log}")
        seed_log = OUT / "client-catalog-auto-repair-seed.log"
        out = seed_log.open("wb")
        seed_client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
            "--catalog-auto-repair-seed-smoke "
            "--catalog-auto-source-repo-node /example/repo/provider/repoB "
            "--catalog-auto-target-repo-node /example/repo/provider/repoA".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf")),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((seed_client, out, seed_log))
        seed_client.wait(timeout=180)
        seed_text = seed_log.read_text(errors="replace")
        print(seed_text)
        seed_match = re.search(
            r"GENERIC_DISTRIBUTED_REPO_AUTO_REPAIR_SEED_OK object=(\S+)",
            seed_text,
        )
        if seed_client.returncode != 0 or seed_match is None:
            raise RuntimeError(
                f"generic DistributedRepo auto repair seed failed "
                f"rc={seed_client.returncode}; log={seed_log}")
        auto_repair_object = seed_match.group(1)
        catalog_auto_proc, catalog_auto_log = start(
            REPO_A_NODE,
            "catalogA-auto",
            base + perf.shell_quote(PY_DIR / "catalog_sync.py") + common +
            " --repo-node /example/repo/provider/repoA "
            "--peer-repo-node /example/repo/provider/repoB "
            "--interval-s 2 --auto-repair "
            "--repair-object-name {}".format(
                perf.shell_quote(auto_repair_object)),
        )
        deadline = time.time() + 90.0
        while time.time() < deadline:
            auto_text = catalog_auto_log.read_text(errors="replace")
            if ("catalog_sync repaired repo=/example/repo/provider/repoA" in auto_text and
                    auto_repair_object in auto_text):
                break
            time.sleep(2.0)
        else:
            raise RuntimeError(
                "catalog auto-repair sidecar did not repair seeded object; "
                f"log={catalog_auto_log} object={auto_repair_object}")
        verify_log = OUT / "client-catalog-auto-repair-verify.log"
        out = verify_log.open("wb")
        verify_client = getPopen(
            ndn.net[USER_NODE],
            base + perf.shell_quote(PY_DIR / "client.py") +
            common +
            " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
            "--catalog-auto-repair-verify-object {}".format(
                perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf"),
                perf.shell_quote(auto_repair_object),
            ),
            envDict=node_env(USER_NODE),
            shell=True,
            stdout=out,
            stderr=subprocess.STDOUT,
        )
        processes.append((verify_client, out, verify_log))
        verify_client.wait(timeout=180)
        verify_text = verify_log.read_text(errors="replace")
        print(verify_text)
        if (verify_client.returncode != 0 or
                "GENERIC_DISTRIBUTED_REPO_AUTO_REPAIR_OK" not in verify_text):
            raise RuntimeError(
                "generic DistributedRepo auto repair verification failed "
                f"rc={verify_client.returncode}; log={verify_log}")
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
