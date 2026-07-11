#!/usr/bin/env python3
"""MiniNDN smoke test for the generic NDNSF-DistributedRepo API."""

from __future__ import annotations

import os
import json
import re
import shutil
import subprocess
import time
import sys
import argparse
import csv
from pathlib import Path
import yaml  # type: ignore

REPO = Path(__file__).resolve().parents[1]
MININDN_ROOT = Path("/tmp/minindn")
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from repo_campaign_evidence import (  # noqa: E402
    correlate_recovered_repairs,
    parse_catalog_sync_metric,
)
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
    parser.add_argument("--tiered-cache-smoke", action="store_true",
                        help="Validate SQLite restart plus bounded Repo hot-cache behavior.")
    parser.add_argument("--tiered-cache-bytes", type=int, default=8192)
    parser.add_argument("--tiered-cache-object-bytes", type=int, default=4096)
    parser.add_argument("--tiered-restart-wait-s", type=float, default=20.0)
    parser.add_argument("--exact-packet-smoke", action="store_true",
                        help="Validate exact Data names and byte-identical wires after restart.")
    parser.add_argument("--exact-packet-failover-smoke", action="store_true",
                        help="Kill Repo A after one packet and validate atomic failover to Repo B.")
    parser.add_argument("--ha-campaign", action="store_true",
                        help="Run the headless Spec 077 read/write campaign.")
    parser.add_argument("--campaign-duration-s", type=float, default=60.0)
    parser.add_argument("--campaign-rps", type=float, default=1.0)
    parser.add_argument("--campaign-concurrency", type=int, default=4)
    parser.add_argument("--campaign-read-ratio", type=float, default=0.8)
    parser.add_argument("--campaign-object-bytes", type=int, default=2048)
    parser.add_argument("--campaign-object-mode", choices=("opaque", "exact"),
                        default="opaque")
    parser.add_argument("--campaign-replication-factor", type=int, default=2)
    parser.add_argument("--campaign-write-consistency",
                        choices=("ONE", "QUORUM", "ALL"), default="ALL")
    parser.add_argument("--campaign-seed", type=int, default=77001)
    parser.add_argument("--campaign-control-mode",
                        choices=("normal", "targeted"), default="targeted")
    parser.add_argument("--campaign-user-handler-threads", type=int, default=2)
    parser.add_argument("--campaign-request-timeout-ms", type=int, default=30000)
    parser.add_argument("--campaign-disable-targeted-fallback",
                        action="store_true")
    parser.add_argument("--campaign-gdb", action="store_true",
                        help="Run only the campaign client under batch gdb.")
    parser.add_argument("--campaign-fail-repo", choices=("", "repoA", "repoB", "repoC"),
                        default="")
    parser.add_argument("--campaign-fail-at-s", type=float, default=0.0)
    parser.add_argument("--campaign-restart-after-s", type=float, default=0.0)
    parser.add_argument(
        "--campaign-auto-repair", action="store_true",
        help="Run durable catalog repair workers during the HA campaign.")
    parser.add_argument("--campaign-repair-workers", type=int, default=1)
    parser.add_argument("--campaign-repair-max-jobs", type=int, default=4)
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
                str(REPO / "NDNSF-DistributedRepo/pythonWrapper"),
                str(REPO / "pythonWrapper"),
                os.environ.get("PYTHONPATH", ""),
            ]),
            "PYTHONUNBUFFERED": "1",
            "NDN_LOG": "ndn_service_framework.*=INFO",
            "NDNSF_RESPONSE_LARGE_DATA_THRESHOLD": os.environ.get(
                "NDNSF_RESPONSE_LARGE_DATA_THRESHOLD", "1024"),
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
        exact_packet_mode = (
            args.exact_packet_smoke or args.exact_packet_failover_smoke)
        if args.tiered_cache_smoke or exact_packet_mode or args.ha_campaign:
            shutil.rmtree(MININDN_ROOT / REPO_A_NODE / "repo-store",
                          ignore_errors=True)
        if args.exact_packet_failover_smoke or args.ha_campaign:
            shutil.rmtree(MININDN_ROOT / REPO_B_NODE / "repo-store",
                          ignore_errors=True)
        if args.ha_campaign:
            shutil.rmtree(MININDN_ROOT / REPO_C_NODE / "repo-store",
                          ignore_errors=True)
        repo_a_command = (
            base + perf.shell_quote(PY_DIR / "repo_node.py") + common +
            " --provider-id repoA --repo-node /example/repo/provider/repoA "
            "--failure-domain rack-a "
            f"--storage-dir {MININDN_ROOT}/{REPO_A_NODE}/repo-store "
            + (f"--memory-cache-bytes {args.tiered_cache_bytes} "
               "--producer-retention-s 120 "
               if args.tiered_cache_smoke or exact_packet_mode else "") +
            "--advertise-stored-prefixes"
        )
        repoA_proc, _ = start(
            REPO_A_NODE,
            "repoA",
            repo_a_command,
        )
        repo_b_command = (
            base + perf.shell_quote(PY_DIR / "repo_node.py") + common +
            " --provider-id repoB --repo-node /example/repo/provider/repoB "
            "--failure-domain rack-b "
            f"--storage-dir {MININDN_ROOT}/{REPO_B_NODE}/repo-store "
            + ("--producer-retention-s 120 " if exact_packet_mode else "") +
            "--advertise-stored-prefixes"
        )
        repoB_proc, _ = start(
            REPO_B_NODE,
            "repoB",
            repo_b_command,
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
        catalog_sidecars = {
            "repoA": (
                REPO_A_NODE, "catalogA", "/example/repo/provider/repoA",
                ("/example/repo/provider/repoB",
                 "/example/repo/provider/repoC")),
            "repoB": (
                REPO_B_NODE, "catalogB", "/example/repo/provider/repoB",
                ("/example/repo/provider/repoA",
                 "/example/repo/provider/repoC")),
            "repoC": (
                REPO_C_NODE, "catalogC", "/example/repo/provider/repoC",
                ("/example/repo/provider/repoA",
                 "/example/repo/provider/repoB")),
        }

        def catalog_sidecar_command(repo_identity, peers, *, auto_repair=False):
            peer_args = "".join(
                f" --peer-repo-node {peer}" for peer in peers)
            repair_arg = " --auto-repair" if auto_repair else ""
            return (
                base + perf.shell_quote(PY_DIR / "catalog_sync.py") + common +
                f" --repo-node {repo_identity}" + peer_args +
                f" --interval-s {campaign_catalog_interval}" + repair_arg +
                f" --repair-workers {args.campaign_repair_workers}" +
                f" --repair-max-jobs {args.campaign_repair_max_jobs}"
            )

        campaign_repair_arg = (
            " --auto-repair"
            if (args.ha_campaign and args.campaign_auto_repair and
                not args.campaign_fail_repo) else "")
        campaign_catalog_interval = (
            2 if args.ha_campaign and args.campaign_auto_repair else 10)
        startup_auto_repair = bool(campaign_repair_arg)
        catalogA_proc, catalog_a_log = start(
            catalog_sidecars["repoA"][0], catalog_sidecars["repoA"][1],
            catalog_sidecar_command(
                catalog_sidecars["repoA"][2], catalog_sidecars["repoA"][3],
                auto_repair=startup_auto_repair))
        catalogB_proc, catalog_b_log = start(
            catalog_sidecars["repoB"][0], catalog_sidecars["repoB"][1],
            catalog_sidecar_command(
                catalog_sidecars["repoB"][2], catalog_sidecars["repoB"][3],
                auto_repair=startup_auto_repair))
        catalogC_proc, catalog_c_log = start(
            catalog_sidecars["repoC"][0], catalog_sidecars["repoC"][1],
            catalog_sidecar_command(
                catalog_sidecars["repoC"][2], catalog_sidecars["repoC"][3],
                auto_repair=startup_auto_repair))
        catalog_logs = [catalog_a_log, catalog_b_log, catalog_c_log]
        log(f"Waiting {args.repo_start_wait_s:.1f}s for repo providers")
        time.sleep(args.repo_start_wait_s)
        if args.ha_campaign:
            campaign_dir = OUT / (
                f"campaign-c{args.campaign_concurrency}-rps{args.campaign_rps:g}-"
                f"seed{args.campaign_seed}")
            shutil.rmtree(campaign_dir, ignore_errors=True)
            ready_file = campaign_dir / "ready.json"
            campaign_base = base
            if args.campaign_gdb:
                campaign_base = (
                    f"cd {perf.shell_quote(REPO)} && exec gdb -batch "
                    "-ex run -ex 'thread apply all bt' --args python3 ")
            command = (
                campaign_base + perf.shell_quote(PY_DIR / "repo_campaign.py") + common +
                " --output-dir {} --duration-s {} --rps {} --concurrency {} "
                "--read-ratio {} --object-bytes {} --object-mode {} "
                "--replication-factor {} --write-consistency {} --seed {} "
                "--ready-file {} --control-mode {} --handler-threads {} {} "
                "--repo-node /example/repo/provider/repoA "
                "--repo-node /example/repo/provider/repoB "
                "--timeout-ms {} "
                "--repo-node /example/repo/provider/repoC".format(
                    perf.shell_quote(campaign_dir), args.campaign_duration_s,
                    args.campaign_rps, args.campaign_concurrency,
                    args.campaign_read_ratio, args.campaign_object_bytes,
                    args.campaign_object_mode, args.campaign_replication_factor,
                    args.campaign_write_consistency, args.campaign_seed,
                    perf.shell_quote(ready_file),
                    args.campaign_control_mode,
                    args.campaign_user_handler_threads,
                    ("--disable-targeted-fallback"
                     if args.campaign_disable_targeted_fallback else ""),
                    args.campaign_request_timeout_ms,
                )
            )
            campaign_proc, campaign_log = start(
                USER_NODE, "repo-ha-campaign", command)
            repo_processes = {
                "repoA": (repoA_proc, REPO_A_NODE, repo_a_command),
                "repoB": (repoB_proc, REPO_B_NODE, repo_b_command),
                "repoC": (repoC_proc, REPO_C_NODE,
                          base + perf.shell_quote(PY_DIR / "repo_node.py") + common +
                          " --provider-id repoC --repo-node /example/repo/provider/repoC "
                          "--failure-domain rack-c "
                          f"--storage-dir {MININDN_ROOT}/{REPO_C_NODE}/repo-store "
                          "--advertise-stored-prefixes"),
            }
            failure_epoch_ms = 0
            restart_epoch_ms = 0
            if args.campaign_fail_repo and args.campaign_fail_at_s > 0:
                ready_deadline = time.time() + 120.0
                while not ready_file.exists() and time.time() < ready_deadline:
                    if campaign_proc.poll() is not None:
                        break
                    time.sleep(0.2)
                if not ready_file.exists():
                    raise RuntimeError(
                        f"Repo HA campaign did not reach ready barrier; "
                        f"log={campaign_log}")
                log(f"HA campaign ready barrier reached {ready_file}")
                time.sleep(args.campaign_fail_at_s)
                failed_proc, failed_host, restart_command = repo_processes[
                    args.campaign_fail_repo]
                failed_proc.terminate()
                try:
                    failed_proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    failed_proc.kill()
                failure_epoch_ms = int(time.time() * 1000)
                log(f"HA campaign terminated {args.campaign_fail_repo}")
                if args.campaign_auto_repair:
                    for catalog_proc in (
                            catalogA_proc, catalogB_proc, catalogC_proc):
                        catalog_proc.terminate()
                        try:
                            catalog_proc.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            catalog_proc.kill()
                    for repo_name, sidecar in catalog_sidecars.items():
                        host, label, repo_identity, peers = sidecar
                        if repo_name == args.campaign_fail_repo:
                            continue
                        _, repair_log = start(
                            host, f"{label}-repair",
                            catalog_sidecar_command(
                                repo_identity, peers, auto_repair=True))
                        catalog_logs.append(repair_log)
                    log("HA campaign enabled auto-repair after failure")
                if args.campaign_restart_after_s > 0:
                    time.sleep(args.campaign_restart_after_s)
                    start(failed_host, f"{args.campaign_fail_repo}-restart",
                          restart_command)
                    restart_epoch_ms = int(time.time() * 1000)
                    if args.campaign_auto_repair:
                        host, label, repo_identity, peers = catalog_sidecars[
                            args.campaign_fail_repo]
                        _, recovered_catalog_log = start(
                            host, f"{label}-recovered-repair",
                            catalog_sidecar_command(
                                repo_identity, peers, auto_repair=True))
                        catalog_logs.append(recovered_catalog_log)
                    log(f"HA campaign restarted {args.campaign_fail_repo}")
            try:
                campaign_rc = campaign_proc.wait(timeout=max(
                    180.0, args.campaign_duration_s + 180.0))
            except subprocess.TimeoutExpired:
                campaign_proc.kill()
                raise RuntimeError(
                    f"Repo HA campaign timed out; log={campaign_log}")
            summary_path = campaign_dir / "summary.json"
            if not summary_path.exists():
                raise RuntimeError(
                    f"Repo HA campaign produced no summary; rc={campaign_rc} "
                    f"log={campaign_log}")
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            lifecycle_rows = []
            lifecycle_path = campaign_dir / "request-lifecycle.csv"
            if lifecycle_path.exists():
                with lifecycle_path.open(newline="", encoding="utf-8") as stream:
                    lifecycle_rows = list(csv.DictReader(stream))

            def percentile(values, quantile):
                if not values:
                    return 0.0
                ordered = sorted(float(value) for value in values)
                index = min(len(ordered) - 1, max(
                    0, int(round(quantile * (len(ordered) - 1)))))
                return ordered[index]

            def summarize_phase(rows):
                successes = [row for row in rows if int(row["success"])]
                writes = [row for row in rows if row["operation"] == "write"]
                successful_writes = [row for row in writes if int(row["success"])]
                latencies = [float(row["latencyMs"]) for row in successes]
                write_latencies = [
                    float(row["latencyMs"]) for row in successful_writes]
                receipts = [
                    int(row["confirmedReplicas"]) for row in successful_writes]
                return {
                    "attempted": len(rows),
                    "succeeded": len(successes),
                    "failed": len(rows) - len(successes),
                    "writes": len(writes),
                    "successfulWrites": len(successful_writes),
                    "latencyP50Ms": percentile(latencies, 0.50),
                    "latencyP95Ms": percentile(latencies, 0.95),
                    "writeP50Ms": percentile(write_latencies, 0.50),
                    "writeP95Ms": percentile(write_latencies, 0.95),
                    "minimumSuccessfulWriteReceipts": min(receipts, default=0),
                    "maximumSuccessfulWriteReceipts": max(receipts, default=0),
                }

            phase_rows = {"preFailure": [], "overlappingFailure": [],
                          "postFailure": []}
            if failure_epoch_ms:
                for row in lifecycle_rows:
                    row_start = int(row.get("startedEpochMs", "0") or 0)
                    row_end = int(row.get("completedEpochMs", "0") or 0)
                    if row_end <= failure_epoch_ms:
                        phase_rows["preFailure"].append(row)
                    elif row_start < failure_epoch_ms < row_end:
                        phase_rows["overlappingFailure"].append(row)
                    else:
                        phase_rows["postFailure"].append(row)
            else:
                phase_rows["preFailure"] = lifecycle_rows
            phase_metrics = {
                phase: summarize_phase(rows)
                for phase, rows in phase_rows.items()
            }
            repair_events = []
            repair_cycle_events = []
            catalog_merge_events = []
            for catalog_log in catalog_logs:
                for line in Path(catalog_log).read_text(
                        encoding="utf-8", errors="replace").splitlines():
                    metric = parse_catalog_sync_metric(line)
                    if metric is not None:
                        metric["log"] = str(catalog_log)
                        if metric["kind"] == "repairCycle":
                            repair_cycle_events.append(metric)
                        elif metric["kind"] == "catalogMerge":
                            catalog_merge_events.append(metric)
                    if "catalog_sync repaired" not in line:
                        continue
                    fields = {}
                    for item in line.split():
                        if "=" in item:
                            key, value = item.split("=", 1)
                            fields[key] = value
                    event = {
                        "repoNode": fields.get("repo", ""),
                        "objectName": fields.get("object", ""),
                        "sourceRepo": fields.get("source", ""),
                        "timestampMs": int(fields.get("timestampMs", "0")),
                    }
                    if (not failure_epoch_ms or
                            event["timestampMs"] >= failure_epoch_ms):
                        repair_events.append(event)
            repair_events.sort(key=lambda item: item["timestampMs"])
            first_repair_latency_ms = None
            if failure_epoch_ms and repair_events:
                first_repair_latency_ms = max(
                    0, repair_events[0]["timestampMs"] - failure_epoch_ms)
            recovered_repo_identity = (
                catalog_sidecars[args.campaign_fail_repo][2]
                if args.campaign_fail_repo else "")
            recovery_evidence = correlate_recovered_repairs(
                lifecycle_rows,
                repair_events,
                recovered_repo=recovered_repo_identity,
                failure_epoch_ms=failure_epoch_ms,
                restart_epoch_ms=restart_epoch_ms,
            )
            repair_window_s = max(
                0.0,
                (int(summary.get("campaignEndEpochMs", 0) or 0) -
                 restart_epoch_ms) / 1000.0,
            ) if restart_epoch_ms else 0.0
            recovery_evidence["repairWindowSeconds"] = repair_window_s
            recovery_evidence["recoveredRepairThroughputPerSecond"] = (
                int(recovery_evidence["recoveredTargetRepairEventCount"]) /
                repair_window_s if repair_window_s > 0 else 0.0
            )
            recovered_cycles = [
                event for event in repair_cycle_events
                if (str(event.get("repo", "")) == recovered_repo_identity and
                    (not restart_epoch_ms or
                     int(event.get("timestampMs", 0) or 0) >= restart_epoch_ms))
            ]
            recovered_merges = [
                event for event in catalog_merge_events
                if (str(event.get("repo", "")) == recovered_repo_identity and
                    (not restart_epoch_ms or
                     int(event.get("timestampMs", 0) or 0) >= restart_epoch_ms))
            ]
            recovery_evidence["repairVisibility"] = {
                "cycleCount": len(recovered_cycles),
                "mergeCount": len(recovered_merges),
                "maxClaimable": max(
                    [int(event.get("claimable", 0) or 0)
                     for event in recovered_cycles] or [0]),
                "totalClaimed": sum(
                    int(event.get("claimed", 0) or 0)
                    for event in recovered_cycles),
                "totalCompleted": sum(
                    int(event.get("completed", 0) or 0)
                    for event in recovered_cycles),
                "scanTotalMs": sum(
                    float(event.get("scanMs", 0) or 0)
                    for event in recovered_cycles),
                "claimTotalMs": sum(
                    float(event.get("claimMs", 0) or 0)
                    for event in recovered_cycles),
                "transferTotalMs": sum(
                    float(event.get("transferMs", 0) or 0)
                    for event in recovered_cycles),
                "mergeTotalMs": sum(
                    float(event.get("durationMs", 0) or 0)
                    for event in recovered_merges),
                "mergeModeCounts": {
                    mode: sum(
                        1 for event in recovered_merges
                        if str(event.get("mode", "legacy")) == mode)
                    for mode in sorted({
                        str(event.get("mode", "legacy"))
                        for event in recovered_merges
                    })
                },
                "mergePayloadBytes": sum(
                    int(event.get("payloadBytes", 0) or 0)
                    for event in recovered_merges),
                "mergeSegments": sum(
                    int(event.get("segments", 0) or 0)
                    for event in recovered_merges),
                "mergeBatches": sum(
                    int(event.get("batches", 0) or 0)
                    for event in recovered_merges),
            }
            fault_evidence = {
                "autoRepair": bool(args.campaign_auto_repair),
                "failedRepo": args.campaign_fail_repo,
                "failureEpochMs": failure_epoch_ms,
                "restartEpochMs": restart_epoch_ms,
                "restartAfterSeconds": args.campaign_restart_after_s,
                "repairWorkers": args.campaign_repair_workers,
                "repairMaxJobs": args.campaign_repair_max_jobs,
                "repairEventCount": len(repair_events),
                "firstRepairLatencyMs": first_repair_latency_ms,
                "phaseMetrics": phase_metrics,
                "repairEvents": repair_events,
                "repairCycleEvents": repair_cycle_events,
                "catalogMergeEvents": catalog_merge_events,
                "recovery": recovery_evidence,
            }
            summary["faultInjection"] = fault_evidence
            summary_path.write_text(
                json.dumps(summary, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
            metadata = {
                "summary": str(summary_path),
                "clientLog": str(campaign_log),
                "failedRepo": args.campaign_fail_repo,
                "failAtSeconds": args.campaign_fail_at_s,
                "restartAfterSeconds": args.campaign_restart_after_s,
                "autoRepair": bool(args.campaign_auto_repair),
                "repairWorkers": args.campaign_repair_workers,
                "repairMaxJobs": args.campaign_repair_max_jobs,
                "repairEventCount": len(repair_events),
                "firstRepairLatencyMs": first_repair_latency_ms,
                "returnCode": campaign_rc,
            }
            (campaign_dir / "minindn-metadata.json").write_text(
                json.dumps(metadata, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
            log(
                f"Repo HA campaign complete success={summary.get('succeeded')} "
                f"failed={summary.get('failed')} summary={summary_path}")
            return
        if args.exact_packet_failover_smoke:
            state_path = OUT / "exact-packet-state.json"
            trigger_path = OUT / "exact-packet-failover-trigger.json"
            resume_path = OUT / "exact-packet-failover-resume.json"
            summary_path = OUT / "exact-packet-failover-summary.json"
            client_log = OUT / "client-exact-packet-failover.log"
            for path in (state_path, trigger_path, resume_path, summary_path):
                path.unlink(missing_ok=True)
            client_out = client_log.open("wb")
            failover_client = getPopen(
                ndn.net[USER_NODE],
                base + perf.shell_quote(PY_DIR / "client.py") + common +
                " --use-local-config --trust-schema {} --ack-timeout-ms 3000 "
                "--timeout-ms 15000 --exact-packet-seed-smoke "
                "--exact-packet-verify-smoke --exact-packet-state-file {} "
                "--exact-packet-summary-file {} "
                "--exact-packet-repo-node /example/repo/provider/repoA "
                "--exact-packet-secondary-repo-node /example/repo/provider/repoB "
                "--exact-packet-failover-trigger-file {} "
                "--exact-packet-failover-resume-file {} "
                "--exact-packet-failover-wait-s 120".format(
                    perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf"),
                    perf.shell_quote(state_path),
                    perf.shell_quote(summary_path),
                    perf.shell_quote(trigger_path),
                    perf.shell_quote(resume_path),
                ),
                envDict=node_env(USER_NODE),
                shell=True,
                stdout=client_out,
                stderr=subprocess.STDOUT,
            )
            processes.append((failover_client, client_out, client_log))
            trigger_deadline = time.monotonic() + 240.0
            while not trigger_path.exists():
                if failover_client.poll() is not None:
                    raise RuntimeError(
                        "exact-packet failover client exited before trigger; "
                        f"log={client_log}")
                if time.monotonic() >= trigger_deadline:
                    raise TimeoutError(
                        "timed out waiting for exact-packet failover trigger")
                time.sleep(0.1)

            trigger = json.loads(trigger_path.read_text(encoding="utf-8"))
            log(
                "Terminating Repo A after first exact packet "
                f"name={trigger.get('packetName', '')}")
            repoA_proc.terminate()
            repoA_proc.wait(timeout=30)
            resume_path.write_text(json.dumps({
                "repoAExited": True,
                "timestampMs": time.time_ns() // 1_000_000,
            }, sort_keys=True) + "\n", encoding="utf-8")

            failover_client.wait(timeout=360)
            client_text = client_log.read_text(errors="replace")
            print(client_text)
            if (failover_client.returncode != 0 or
                    "GENERIC_DISTRIBUTED_REPO_EXACT_PACKET_SEED_OK"
                    not in client_text or
                    "GENERIC_DISTRIBUTED_REPO_EXACT_PACKET_FAILOVER_VERIFY_OK"
                    not in client_text or not summary_path.exists()):
                raise RuntimeError(
                    "generic DistributedRepo exact-packet failover failed "
                    f"rc={failover_client.returncode}; log={client_log}")
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            required_checks = (
                "primaryOnePacketBeforeFailure",
                "primaryFailureObserved",
                "secondaryRestartedWholeSet",
                "exactNames",
                "wireIdentity",
            )
            checks = summary.get("checks", {})
            if (not bool(summary.get("passed")) or
                    not all(bool(checks.get(name)) for name in required_checks)):
                raise RuntimeError(
                    f"exact-packet failover summary failed: {summary}")
            print(
                "GENERIC_DISTRIBUTED_REPO_EXACT_PACKET_FAILOVER_MININDN_OK "
                f"summary={summary_path}",
                flush=True,
            )
            return

        if args.exact_packet_smoke:
            state_path = OUT / "exact-packet-state.json"
            summary_path = OUT / "exact-packet-summary.json"
            seed_log = OUT / "client-exact-packet-seed.log"
            seed_out = seed_log.open("wb")
            seed_client = getPopen(
                ndn.net[USER_NODE],
                base + perf.shell_quote(PY_DIR / "client.py") + common +
                " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
                "--exact-packet-seed-smoke --exact-packet-state-file {} "
                "--exact-packet-repo-node /example/repo/provider/repoA".format(
                    perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf"),
                    perf.shell_quote(state_path),
                ),
                envDict=node_env(USER_NODE),
                shell=True,
                stdout=seed_out,
                stderr=subprocess.STDOUT,
            )
            processes.append((seed_client, seed_out, seed_log))
            seed_client.wait(timeout=300)
            seed_text = seed_log.read_text(errors="replace")
            print(seed_text)
            if (seed_client.returncode != 0 or
                    "GENERIC_DISTRIBUTED_REPO_EXACT_PACKET_SEED_OK" not in seed_text or
                    not state_path.exists()):
                raise RuntimeError(
                    "generic DistributedRepo exact-packet seed failed "
                    f"rc={seed_client.returncode}; log={seed_log}")

            repoA_proc.terminate()
            repoA_proc.wait(timeout=30)
            repoA_proc, _ = start(REPO_A_NODE, "repoA-exact-restart", repo_a_command)
            time.sleep(args.tiered_restart_wait_s)

            verify_log = OUT / "client-exact-packet-verify.log"
            verify_out = verify_log.open("wb")
            verify_client = getPopen(
                ndn.net[USER_NODE],
                base + perf.shell_quote(PY_DIR / "client.py") + common +
                " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
                "--exact-packet-verify-smoke --exact-packet-state-file {} "
                "--exact-packet-summary-file {} "
                "--exact-packet-repo-node /example/repo/provider/repoA".format(
                    perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf"),
                    perf.shell_quote(state_path),
                    perf.shell_quote(summary_path),
                ),
                envDict=node_env(USER_NODE),
                shell=True,
                stdout=verify_out,
                stderr=subprocess.STDOUT,
            )
            processes.append((verify_client, verify_out, verify_log))
            verify_client.wait(timeout=360)
            verify_text = verify_log.read_text(errors="replace")
            print(verify_text)
            if (verify_client.returncode != 0 or
                    "GENERIC_DISTRIBUTED_REPO_EXACT_PACKET_VERIFY_OK" not in verify_text or
                    not summary_path.exists()):
                raise RuntimeError(
                    "generic DistributedRepo exact-packet verification failed "
                    f"rc={verify_client.returncode}; log={verify_log}")
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            checks = summary.get("checks", {})
            if (not bool(summary.get("passed")) or
                    not bool(checks.get("batchPacketConsumer"))):
                raise RuntimeError(f"exact-packet summary failed: {summary}")
            print(
                "GENERIC_DISTRIBUTED_REPO_EXACT_PACKET_MININDN_OK "
                f"summary={summary_path}",
                flush=True,
            )
            return
        if args.tiered_cache_smoke:
            state_path = OUT / "tiered-cache-state.json"
            summary_path = OUT / "tiered-cache-summary.json"
            seed_log = OUT / "client-tiered-cache-seed.log"
            seed_out = seed_log.open("wb")
            seed_client = getPopen(
                ndn.net[USER_NODE],
                base + perf.shell_quote(PY_DIR / "client.py") + common +
                " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
                "--tiered-cache-seed-smoke --tiered-cache-state-file {} "
                "--tiered-cache-repo-node /example/repo/provider/repoA "
                "--tiered-cache-object-bytes {}".format(
                    perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf"),
                    perf.shell_quote(state_path),
                    args.tiered_cache_object_bytes,
                ),
                envDict=node_env(USER_NODE),
                shell=True,
                stdout=seed_out,
                stderr=subprocess.STDOUT,
            )
            processes.append((seed_client, seed_out, seed_log))
            seed_client.wait(timeout=300)
            seed_text = seed_log.read_text(errors="replace")
            print(seed_text)
            if (seed_client.returncode != 0 or
                    "GENERIC_DISTRIBUTED_REPO_TIERED_CACHE_SEED_OK" not in seed_text or
                    not state_path.exists()):
                raise RuntimeError(
                    "generic DistributedRepo tiered-cache seed failed "
                    f"rc={seed_client.returncode}; log={seed_log}")

            log("Stopping Repo A to clear process-local cache")
            repoA_proc.terminate()
            repoA_proc.wait(timeout=30)
            repoA_proc, _ = start(REPO_A_NODE, "repoA-restart", repo_a_command)
            log(f"Waiting {args.tiered_restart_wait_s:.1f}s for Repo A restart")
            time.sleep(args.tiered_restart_wait_s)

            verify_log = OUT / "client-tiered-cache-verify.log"
            verify_out = verify_log.open("wb")
            verify_client = getPopen(
                ndn.net[USER_NODE],
                base + perf.shell_quote(PY_DIR / "client.py") + common +
                " --use-local-config --trust-schema {} --ack-timeout-ms 8000 "
                "--tiered-cache-verify-smoke --tiered-cache-state-file {} "
                "--tiered-cache-summary-file {} "
                "--tiered-cache-repo-node /example/repo/provider/repoA".format(
                    perf.shell_quote(Path(GEN_POLICY) / "trust-schema.conf"),
                    perf.shell_quote(state_path),
                    perf.shell_quote(summary_path),
                ),
                envDict=node_env(USER_NODE),
                shell=True,
                stdout=verify_out,
                stderr=subprocess.STDOUT,
            )
            processes.append((verify_client, verify_out, verify_log))
            verify_client.wait(timeout=360)
            verify_text = verify_log.read_text(errors="replace")
            print(verify_text)
            if (verify_client.returncode != 0 or
                    "GENERIC_DISTRIBUTED_REPO_TIERED_CACHE_VERIFY_OK" not in verify_text or
                    not summary_path.exists()):
                raise RuntimeError(
                    "generic DistributedRepo tiered-cache verification failed "
                    f"rc={verify_client.returncode}; log={verify_log}")
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            if not bool(summary.get("passed")):
                raise RuntimeError(f"tiered-cache summary failed: {summary}")
            print(
                "GENERIC_DISTRIBUTED_REPO_TIERED_CACHE_MININDN_OK "
                f"summary={summary_path}",
                flush=True,
            )
            return
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
