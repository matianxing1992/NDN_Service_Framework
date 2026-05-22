#!/usr/bin/env python3
#
# Usage:
#   --dry-run              Validate arguments and print the execution plan only.
#   --connectivity-check   Run MiniNDN/NFD/ndnping diagnostics only.
#   --svs-only-smoke       Run a minimal ndn-svs pub/sub diagnostic only.
#   --svs-smoke            Run an isolated NDNSF/SVS smoke diagnostic only.
#   normal perf run        Omit the diagnostic flags to run the benchmark.
#
# Startup note:
#   Providers are launched before users, and readiness is detected from provider
#   logs before App_User starts. This avoids false timeouts caused by the user
#   sending before provider service registration and permission setup finish.

import argparse
import base64
import csv
import json
import os
import pwd
import re
import shlex
import statistics
import subprocess
import sys
import time
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path

from mininet.log import setLogLevel, info
from mininet.topo import Topo


def add_sudo_user_site_packages():
    sudo_user = os.environ.get("SUDO_USER")
    if not sudo_user:
        return
    try:
        user_home = pwd.getpwnam(sudo_user).pw_dir
    except KeyError:
        return
    user_site = os.path.join(
        user_home, ".local", "lib",
        "python{}.{}".format(sys.version_info.major, sys.version_info.minor),
        "site-packages")
    if os.path.isdir(user_site) and user_site not in sys.path:
        sys.path.append(user_site)


add_sudo_user_site_packages()
from ndn.encoding import Name, parse_data

try:
    from minindn.minindn import Minindn
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.apps.nlsr import Nlsr
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.helpers.nfdc import Nfdc
    from minindn.util import getPopen
except ImportError as e:
    print("MiniNDN is not installed or not importable: {}".format(e), file=sys.stderr)
    sys.exit(2)


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SERVICE = "/HELLO"
CONNECTIVITY_PREFIX = "/connectivity/providerA"
SVS_GROUP_PREFIX = "/example/hello/group"
SVS_SYNC_PREFIX = SVS_GROUP_PREFIX
SVS_LOG_PATTERNS = [
    "/example/hello/group",
    "sync",
    "Interest",
    "Data",
    "timeout",
    "multicast",
]
DK_LOG_PATTERNS = [
    "DKEY",
    "DKEY/",
    "/DKEY",
    "/KEY",
    "PUBPARAMS",
    "DK_INTEREST_EXPRESSED",
    "DK_DATA_RECEIVED",
    "DK_DECRYPT_SUCCESS",
    "DK_DECRYPT_FAILURE",
    "DK_TIMEOUT",
    "KpAA Got DKEY request",
    "CpAA Got DKEY request",
    "For DKEY segment",
]
BOOTSTRAP_INTEREST_KEYWORDS = [
    "permission",
    "permissions",
    "decryption key",
    "nac",
    "abe",
    "controller",
    "token",
    "key",
    "ck",
    "kdk",
    "kek",
]
APP_TARGETS = {
    "controller": REPO_ROOT / "build/examples/App_ServiceController",
    "provider": REPO_ROOT / "build/examples/App_Provider",
    "user": REPO_ROOT / "build/examples/App_User",
}
PROCESS_TERMINATE_TIMEOUT_SECONDS = 5
PROCESS_KILL_TIMEOUT_SECONDS = 2


def log(message):
    info("{}\n".format(message))


def now_us():
    return int(time.time() * 1000000)


def shell_quote(value):
    return shlex.quote(str(value))


def build_parser():
    parser = argparse.ArgumentParser(
        description="MiniNDN performance experiment for the new NDNSF dynamic API")
    parser.add_argument("--duration", type=int, default=120,
                        help="Measurement duration in seconds")
    parser.add_argument("--warmup", type=int, default=10,
                        help="Warmup duration in seconds")
    parser.add_argument("--interval-ms", type=int, default=200,
                        help="Delay between completed requests")
    parser.add_argument("--providers", type=int, default=2, choices=list(range(1, 11)),
                        help="Number of provider nodes")
    parser.add_argument("--output-dir", default="",
                        help="Directory for logs and result files")
    parser.add_argument("--topology", default="",
                        help="Optional MiniNDN topology file with nodes controller, user, provider1..N")
    parser.add_argument("--topology-file", default="",
                        help="Optional MiniNDN topology file; enables explicit node placement mode")
    parser.add_argument("--user-node", default="user",
                        help="MiniNDN node that runs App_User")
    parser.add_argument("--controller-node", default="controller",
                        help="MiniNDN node that runs App_ServiceController")
    parser.add_argument("--provider-nodes", default="",
                        help="Comma-separated MiniNDN nodes that run App_Provider instances")
    parser.add_argument("--rate-series", default="",
                        help="Comma-separated request rates to run sequentially, e.g. 1,50,100,150")
    parser.add_argument("--workload-mode", default="closed-loop",
                        choices=["closed-loop", "open-loop"],
                        help="Workload generator mode")
    parser.add_argument("--rate-rps", type=float, default=None,
                        help="Open-loop offered request rate")
    parser.add_argument("--max-outstanding", type=int, default=512,
                        help="Open-loop maximum concurrent outstanding requests")
    parser.add_argument("--adaptive-admission-control", action="store_true",
                        help="Enable App_User adaptive local queue admission control")
    parser.add_argument("--adaptive-min-window", type=int, default=1)
    parser.add_argument("--adaptive-max-window", type=int, default=512)
    parser.add_argument("--adaptive-initial-window", type=int, default=16)
    parser.add_argument("--adaptive-hard-inflight-limit", type=int, default=None)
    parser.add_argument("--adaptive-ai-step", type=int, default=4)
    parser.add_argument("--adaptive-md-factor", type=float, default=0.85)
    parser.add_argument("--adaptive-severe-md-factor", type=float, default=0.75)
    parser.add_argument("--adaptive-control-interval-ms", type=int, default=500)
    parser.add_argument("--adaptive-target-latency-ms", type=int, default=350)
    parser.add_argument("--adaptive-hard-target-latency-ms", type=int, default=500)
    parser.add_argument("--adaptive-soft-queue-limit", type=int, default=None,
                        help="Pass --adaptive-soft-queue-limit to App_User; default keeps App_User latency-first default")
    parser.add_argument("--adaptive-hard-queue-limit", type=int, default=None,
                        help="Pass --adaptive-hard-queue-limit to App_User; default keeps App_User latency-first default")
    parser.add_argument("--adaptive-warning-backoff-ms", type=int, default=None,
                        help="Pass --adaptive-warning-backoff-ms to App_User")
    parser.add_argument("--adaptive-reject-backoff-ms", type=int, default=None,
                        help="Pass --adaptive-reject-backoff-ms to App_User")
    parser.add_argument("--disable-adaptive-queue-aware-pause", action="store_true",
                        help="Disable App_User queue-depth based pause/resume after admission warning/reject")
    parser.add_argument("--disable-adaptive-recommended-rate", action="store_true",
                        help="Disable App_User pacing by ServiceUser recommended request rate")
    parser.add_argument("--adaptive-warning-resume-queue-depth", type=int, default=None,
                        help="Pass --adaptive-warning-resume-queue-depth to App_User")
    parser.add_argument("--adaptive-reject-resume-queue-depth", type=int, default=None,
                        help="Pass --adaptive-reject-resume-queue-depth to App_User")
    parser.add_argument("--adaptive-queue-pause-poll-ms", type=int, default=None,
                        help="Pass --adaptive-queue-pause-poll-ms to App_User")
    parser.add_argument("--adaptive-provider-ack", action="store_true",
                        help="Enable provider-side adaptive ACK admission")
    parser.add_argument("--provider-ack-max-pending", type=int, default=1000)
    parser.add_argument("--provider-ack-max-event-loop-lag-ms", type=int, default=0)
    parser.add_argument("--provider-ack-max-coordination-lag-ms", type=int, default=0)
    parser.add_argument("--provider-request-delay-ms-series", default="",
                        help="Comma-separated provider processing delays in ms, e.g., 5,20,40")
    parser.add_argument("--handler-threads", type=int, default=-1,
                        help="Pass --handler-threads to App_User and App_Provider; -1 keeps app default")
    parser.add_argument("--request-timeout-ms", type=int, default=5000,
                        help="Open-loop per-request timeout")
    parser.add_argument("--drain-seconds", type=int, default=10,
                        help="Open-loop seconds to drain responses after request generation stops")
    parser.add_argument("--per-rate-duration", type=int, default=30,
                        help="Measurement duration in seconds for each --rate-series subtest")
    parser.add_argument("--max-total-runtime-seconds", type=int, default=180,
                        help="Maximum wall-clock runtime budget for --rate-series workload subtests")
    parser.add_argument("--strategy", default="custom-selection",
                        choices=["first-responding", "custom-selection", "random-selection",
                                 "no-coordination", "load-balancing"],
                        help="App_User benchmark strategy")
    parser.add_argument("--ack-timeout-ms", type=int, default=1000,
                        help="ACK collection timeout for custom selection")
    parser.add_argument("--timeout-ms", type=int, default=5000,
                        help="Per-request timeout")
    parser.add_argument("--max-requests", type=int, default=300,
                        help="Cap measured requests so the experiment finishes quickly")
    parser.add_argument("--dry-run", action="store_true",
                        help="Validate arguments and print the execution plan without starting MiniNDN")
    parser.add_argument("--connectivity-check", action="store_true",
                        help="Run MiniNDN/NFD/ndnping diagnostics without the NDNSF workload")
    parser.add_argument("--dump-nfd-state", action="store_true",
                        help="Save nfdc face/route/fib/strategy/cs output before and after workload")
    parser.add_argument("--nlsr-converge-seconds", type=int, default=None,
                        help="Seconds to wait after NLSR prefix advertisement before starting apps; defaults to 30 with a topology file and 0 with generated topology")
    parser.add_argument("--svs-smoke", action="store_true",
                        help="Run an isolated SVS multicast namespace diagnostic and skip the workload")
    parser.add_argument("--svs-settle-seconds", type=int, default=10,
                        help="Seconds to let SVS sync settle in --svs-smoke before starting providers")
    parser.add_argument("--controller-settle-seconds", type=int, default=10,
                        help="Seconds to wait after App_ServiceController starts before providers")
    parser.add_argument("--provider-start-gap-seconds", type=int, default=5,
                        help="Seconds to wait between sequential provider starts")
    parser.add_argument("--post-ready-settle-seconds", type=int, default=5,
                        help="Seconds to wait after all providers are ready before App_User starts")
    parser.add_argument("--provider-ready-wait", type=int, default=None,
                        help="Override seconds to wait after all providers are ready before App_User starts; default keeps --post-ready-settle-seconds behavior")
    parser.add_argument("--provider-ready-timeout-seconds", type=int, default=20,
                        help="Seconds to wait for each provider to register service, install permission, and initialize SVS")
    parser.add_argument("--startup-settle-seconds", type=int, default=2,
                        help="Seconds to settle after providers are ready before starting App_User")
    parser.add_argument("--svs-only-smoke", action="store_true",
                        help="Build and run a minimal ndn-svs-only pub/sub diagnostic")
    parser.add_argument("--nfd-log-level", default="INFO",
                        choices=["NONE", "ERROR", "WARN", "INFO", "DEBUG", "TRACE", "ALL"],
                        help="NFD default log level; --svs-smoke upgrades this to TRACE unless overridden")
    parser.add_argument("--debug-ack", action="store_true",
                        help="Enable verbose NDNSF ACK/SVS logging in App_* processes")
    parser.add_argument("--performance-mode", action="store_true",
                        help="Enable App_User reduced per-request benchmark logging")
    parser.add_argument("--disable-tokens", action="store_true",
                        help="Pass --disable-tokens to App_User and App_Provider")
    parser.add_argument("--hybrid-message-crypto", action="store_true",
                        help="Pass --hybrid-message-crypto to App_User and App_Provider")
    parser.add_argument("--disable-hybrid-message-crypto", action="store_true",
                        help="Pass --disable-hybrid-message-crypto to App_User and App_Provider")
    parser.add_argument("--crypto-diagnostics", action="store_true",
                        help="Enable harness-only NDNSF_CRYPTO_DIAG timing logs")
    parser.add_argument("--timeline-trace", action="store_true",
                        help="Pass --timeline-trace to App_User and App_Provider and enable timeline trace logs")
    parser.add_argument("--svs-parallel-sync-processing", action="store_true",
                        help="Enable experimental ndn-svs parallel Sync Interest processing in App_User/App_Provider")
    parser.add_argument("--svs-parallel-workers", type=int, default=4,
                        help="Worker count for --svs-parallel-sync-processing")
    parser.add_argument("--svs-parallel-queue", type=int, default=128,
                        help="Bounded queue size for --svs-parallel-sync-processing")
    parser.add_argument("--svs-sync-publish", action="store_true",
                        help="Use synchronous SVSPubSub::publish instead of default publishAsync")
    parser.add_argument("--svs-disable-parallel-production", action="store_true",
                        help="Disable default ndn-svs parallel Sync Interest production")
    parser.add_argument("--svs-parallel-production-workers", type=int, default=None,
                        help="Override NDNSF_SVS_PARALLEL_PRODUCTION worker count")
    parser.add_argument("--svs-parallel-production-signing", action="store_true",
                        help="Enable signing parallel Sync production Interests in worker threads")
    parser.add_argument("--svs-disable-parallel-production-signing", action="store_true",
                        help="Disable default worker-thread signing for parallel Sync production")
    parser.add_argument("--svs-parallel-production-extra-block", action="store_true",
                        help="Build Sync production ApplicationParameters in worker threads")
    parser.add_argument("--svs-disable-parallel-production-extra-block", action="store_true",
                        help="Disable default worker-thread ApplicationParameters construction")
    parser.add_argument("--svs-sync-batching", action="store_true",
                        help="Enable experimental ndn-svs local publication-triggered sync batching")
    parser.add_argument("--svs-sync-batch-ms", type=int, default=5,
                        help="Batching window in milliseconds for --svs-sync-batching")
    parser.add_argument("--provider-ack-payload", default=None,
                        help="Override benchmark provider ACK payload text for focused diagnostics")
    parser.add_argument("--diag-plaintext-ack", action="store_true",
                        help="Diagnostic-only: publish/decode ACK payloads as plaintext in the harness run")
    parser.add_argument("--diag-plaintext-response", action="store_true",
                        help="Diagnostic-only: publish/decode response payloads as plaintext in the harness run")
    parser.add_argument("--skip-post-run-diagnostics", action="store_true",
                        help="Skip heavyweight post-run NFD/certificate diagnostics after summary generation")
    parser.add_argument("--dk-forwarding-check", action="store_true",
                        help="Bootstrap provider before and after explicit multicast strategy setup for permission/DK prefixes, then skip workload")
    parser.add_argument("--dk-diagnostic-node", default="ucla",
                        help="MiniNDN node on which to dump route/FIB/strategy state for --dk-forwarding-check")
    parser.add_argument("--dk-bootstrap-check", action="store_true",
                        help="Start controller/AA and one provider with provider permission disabled, then diagnose NAC-ABE DKEY bootstrap")
    parser.add_argument("--dk-bootstrap-only", action="store_true",
                        help="With --dk-bootstrap-check, stop after the provider DKEY bootstrap diagnostic")
    cert_group = parser.add_mutually_exclusive_group()
    cert_group.add_argument("--serve-provider-certs", dest="serve_provider_certs",
                            action="store_true", default=None,
                            help="Start legacy harness cert_server.py fallback for provider certificate Data")
    cert_group.add_argument("--no-serve-provider-certs", dest="serve_provider_certs",
                            action="store_false",
                            help="Disable provider certificate Data server")
    return parser


def ensure_repo_root():
    if Path.cwd().resolve() != REPO_ROOT:
        raise RuntimeError("Run this experiment from the repository root: {}".format(REPO_ROOT))


def ensure_binaries():
    missing = [str(path) for path in APP_TARGETS.values() if not os.access(path, os.X_OK)]
    if missing:
        raise RuntimeError("Missing built example binaries. Run ./waf build first. Missing: {}".format(
            ", ".join(missing)))


def ensure_runtime(args):
    ensure_repo_root()
    ensure_binaries()
    if args.interval_ms <= 0 or args.duration <= 0:
        raise RuntimeError("--duration and --interval-ms must be positive")
    if args.warmup < 0 or args.max_requests <= 0:
        raise RuntimeError("--warmup must be non-negative and --max-requests must be positive")
    if args.provider_ready_timeout_seconds <= 0:
        raise RuntimeError("--provider-ready-timeout-seconds must be positive")
    if args.controller_settle_seconds < 0:
        raise RuntimeError("--controller-settle-seconds must be non-negative")
    if args.provider_start_gap_seconds < 0:
        raise RuntimeError("--provider-start-gap-seconds must be non-negative")
    if args.post_ready_settle_seconds < 0:
        raise RuntimeError("--post-ready-settle-seconds must be non-negative")
    if args.provider_ready_wait is not None and args.provider_ready_wait < 0:
        raise RuntimeError("--provider-ready-wait must be non-negative")
    if args.startup_settle_seconds < 0:
        raise RuntimeError("--startup-settle-seconds must be non-negative")
    if args.nlsr_converge_seconds is None:
        args.nlsr_converge_seconds = 30 if topology_file(args) else 0
    if args.serve_provider_certs is None:
        args.serve_provider_certs = False
    if args.nlsr_converge_seconds < 0:
        raise RuntimeError("--nlsr-converge-seconds must be non-negative")
    if args.per_rate_duration <= 0:
        raise RuntimeError("--per-rate-duration must be positive")
    if args.max_total_runtime_seconds <= 0:
        raise RuntimeError("--max-total-runtime-seconds must be positive")
    if args.handler_threads < -1:
        raise RuntimeError("--handler-threads must be >= -1")
    if args.workload_mode == "open-loop":
        if args.rate_rps is None:
            args.rate_rps = 1.0
        if args.rate_rps <= 0.0:
            raise RuntimeError("--rate-rps must be positive in open-loop mode")
        if args.max_outstanding <= 0 or args.request_timeout_ms <= 0:
            raise RuntimeError("--max-outstanding and --request-timeout-ms must be positive")
        if args.drain_seconds < 0:
            raise RuntimeError("--drain-seconds must be non-negative")
        if args.adaptive_min_window <= 0 or args.adaptive_max_window <= 0:
            raise RuntimeError("--adaptive-min-window and --adaptive-max-window must be positive")
        if args.adaptive_hard_inflight_limit is not None and args.adaptive_hard_inflight_limit <= 0:
            raise RuntimeError("--adaptive-hard-inflight-limit must be positive")
        if args.adaptive_target_latency_ms <= 0:
            raise RuntimeError("--adaptive-target-latency-ms must be positive")
    if args.topology and args.topology_file and args.topology != args.topology_file:
        raise RuntimeError("Use either --topology or --topology-file, not both with different paths")
    topology = topology_file(args)
    if topology and not Path(topology).exists():
        raise RuntimeError("Topology file does not exist: {}".format(topology))
    rates = parse_rate_series(args.rate_series)
    if rates and len(rates) * args.per_rate_duration > args.max_total_runtime_seconds:
        raise RuntimeError(
            "--rate-series duration budget exceeds --max-total-runtime-seconds: "
            "{} rates x {}s > {}s".format(
                len(rates), args.per_rate_duration, args.max_total_runtime_seconds))
    if len(selected_provider_nodes(args)) < 1 or len(selected_provider_nodes(args)) > 10:
        raise RuntimeError("This harness supports 1 to 10 providers")
    if topology and args.dry_run:
        validate_node_placement(set(parse_topology_file_nodes(topology)), args)


def ensure_minindn_root():
    if os.geteuid() != 0:
        raise RuntimeError("MiniNDN real execution requires sudo/root; use --dry-run without sudo")


def topology_file(args):
    value = args.topology_file or args.topology
    if not value:
        return ""
    path = Path(value)
    if path.exists():
        return str(path)
    experiment_relative = REPO_ROOT / "Experiments" / value
    if experiment_relative.exists():
        return str(experiment_relative)
    return value


def parse_csv_list(value):
    return [item.strip() for item in (value or "").split(",") if item.strip()]


def parse_rate_series(value):
    rates = []
    for item in parse_csv_list(value):
        try:
            rate = float(item)
        except ValueError:
            raise RuntimeError("Invalid --rate-series value: {}".format(item))
        if rate <= 0:
            raise RuntimeError("--rate-series values must be positive")
        rates.append(rate)
    return rates


def rate_label(rate):
    if float(rate).is_integer():
        return str(int(rate))
    return str(rate).replace(".", "_")


def interval_for_rate(rate):
    return 1000.0 / float(rate)


def app_interval_ms_for_rate(rate):
    return max(1, int(round(interval_for_rate(rate))))


def provider_nodes(provider_count):
    return ["provider{}".format(i + 1) for i in range(provider_count)]


def selected_provider_nodes(args):
    explicit = parse_csv_list(args.provider_nodes)
    if explicit:
        return explicit
    return provider_nodes(args.providers)


def provider_ids(provider_count):
    return [chr(ord("A") + i) for i in range(provider_count)]


def selected_provider_ids(args):
    return provider_ids(len(selected_provider_nodes(args)))


def parse_topology_file_nodes(path):
    nodes = []
    in_nodes = False
    for raw_line in Path(path).read_text(errors="replace").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            in_nodes = line.lower() == "[nodes]"
            continue
        if in_nodes and ":" in line:
            nodes.append(line.split(":", 1)[0].strip())
    return nodes


def placement_node_names(args):
    return [args.controller_node, args.user_node] + selected_provider_nodes(args)


def validate_node_placement(available_names, args):
    available = sorted(available_names)
    missing = [name for name in placement_node_names(args) if name not in available_names]
    if missing:
        raise RuntimeError(
            "MiniNDN node placement references missing node(s): {}. Available nodes: {}".format(
                ", ".join(missing), ", ".join(available)))


def normalize_placement_args(args):
    explicit_providers = selected_provider_nodes(args)
    args.providers = len(explicit_providers)
    if getattr(args, "serve_provider_certs", None) is None:
        args.serve_provider_certs = False
    return args


def measured_count(args):
    if getattr(args, "rate_rps", None):
        return max(1, int(round(float(args.rate_rps) * float(args.duration))))
    worst_case_interval_ms = max(args.interval_ms, 0) + max(args.timeout_ms, 1)
    return min(args.max_requests, max(1, int(args.duration * 1000 / worst_case_interval_ms)))


def warmup_count(args):
    worst_case_interval_ms = max(args.interval_ms, 0) + max(args.timeout_ms, 1)
    return max(0, int(args.warmup * 1000 / worst_case_interval_ms))


def build_default_topology(provider_count):
    topo = Topo()
    controller = topo.addHost("controller")
    user = topo.addHost("user")
    router = topo.addHost("router")
    topo.addLink(controller, router, delay="2ms", loss=0, bw=1000)
    topo.addLink(user, router, delay="2ms", loss=0, bw=1000)
    for node_name in provider_nodes(provider_count):
        provider = topo.addHost(node_name)
        topo.addLink(provider, router, delay="2ms", loss=0, bw=1000)
    return topo


def configure_static_routes(ndn, args):
    # Matches MiniNDN's static_routing_experiment.py pattern:
    # build origins, then ask NdnRoutingHelper to calculate routes.
    log("Adding static routes to NFD")
    routing_helper = NdnRoutingHelper(ndn.net, "udp", "link-state")
    routing_helper.addOrigin([ndn.net[args.controller_node]], ["/example/hello/controller"])
    routing_helper.addOrigin([ndn.net[args.user_node]], ["/example/hello/user",
                                                          "/example/hello/group"])
    provider_node_names = selected_provider_nodes(args)
    for node_name, provider_id in zip(provider_node_names, selected_provider_ids(args)):
        provider_prefix = "/example/hello/provider/{}".format(provider_id)
        routing_helper.addOrigin([ndn.net[node_name]],
                                 [provider_prefix,
                                  ndn_name_join(provider_prefix, "KEY"),
                                  "/example/hello/group"])
    routing_helper.addOrigin([ndn.net[provider_node_names[0]]], [CONNECTIVITY_PREFIX])
    # Use deterministic shortest-path routes for performance experiments.
    # calculateNPossibleRoutes() installs multiple next hops per prefix; with
    # multicast strategy on the SVS/NDNSF prefixes, that makes the effective
    # memphis<->ucla path vary between runs and adds tens of ms of latency noise.
    routing_helper.calculateRoutes()

    for node in ndn.net.hosts:
        for prefix in dict.fromkeys([
            SVS_GROUP_PREFIX,
            SVS_SYNC_PREFIX,
            ndn_name_join(SVS_GROUP_PREFIX, "sync"),
            ndn_name_join(SVS_GROUP_PREFIX, "s"),
            ndn_name_join(SVS_GROUP_PREFIX, "d"),
        ]):
            Nfdc.setStrategy(node, prefix, Nfdc.STRATEGY_MULTICAST)
        Nfdc.setStrategy(node, "/example/hello", Nfdc.STRATEGY_MULTICAST)


def advertise_nlsr_prefixes(ndn, args):
    log("Advertising NDNSF prefixes with NLSR")
    ndn.net[args.controller_node].cmd("nlsrc advertise /example/hello/controller")
    ndn.net[args.user_node].cmd("nlsrc advertise /example/hello/user")
    ndn.net[args.user_node].cmd("nlsrc advertise /example/hello/group")
    provider_node_names = selected_provider_nodes(args)
    for node_name, provider_id in zip(provider_node_names, selected_provider_ids(args)):
        provider_prefix = "/example/hello/provider/{}".format(provider_id)
        ndn.net[node_name].cmd("nlsrc advertise {}".format(provider_prefix))
        ndn.net[node_name].cmd("nlsrc advertise {}".format(ndn_name_join(provider_prefix, "KEY")))
        ndn.net[node_name].cmd("nlsrc advertise /example/hello/group")
    ndn.net[provider_node_names[0]].cmd("nlsrc advertise {}".format(CONNECTIVITY_PREFIX))


def node_cmd(node, command):
    home_dir = node.params["params"]["homeDir"]
    return node.cmd("HOME={} {}".format(shell_quote(home_dir), command))


def result_file(output_dir, label, node_name, suffix):
    return output_dir / "{}-{}{}".format(label, node_name, suffix)


def dump_nfd_state(ndn, output_dir, label):
    log("Dumping NFD state: {}".format(label))
    commands = [
        ("face-list", "nfdc face list"),
        ("route-list", "nfdc route list"),
        ("fib-list", "nfdc fib list"),
        ("strategy-list", "nfdc strategy list"),
        ("cs-info", "nfdc cs info"),
    ]
    for node in ndn.net.hosts:
        for suffix, command in commands:
            path = result_file(output_dir, label, node.name, "-{}.txt".format(suffix))
            output = node_cmd(node, "{} 2>&1".format(command))
            path.write_text(output)


def dump_nfd_route_fib_for_nodes(ndn, output_dir, label, node_names):
    available = {node.name: node for node in ndn.net.hosts}
    selected = []
    for node_name in node_names:
        if node_name in available and node_name not in selected:
            selected.append(node_name)
    if not selected:
        return
    log("Dumping NFD route/FIB state: {} nodes={}".format(label, selected))
    for node_name in selected:
        node = available[node_name]
        for suffix, command in [
            ("route-list", "nfdc route list"),
            ("fib-list", "nfdc fib list"),
        ]:
            path = result_file(output_dir, label, node.name, "-{}.txt".format(suffix))
            path.write_text(node_cmd(node, "{} 2>&1".format(command)))


def nlsr_diagnostic_nodes(args):
    nodes = [args.controller_node]
    providers = selected_provider_nodes(args)
    if providers:
        nodes.append(providers[0])
    nodes.extend(["memphis", "ucla"])
    return list(dict.fromkeys(nodes))


def wait_for_nlsr_convergence(ndn, args, output_dir):
    dump_nfd_route_fib_for_nodes(
        ndn, output_dir, "nlsr-convergence-before-wait", nlsr_diagnostic_nodes(args))
    wait_s = max(0, int(args.nlsr_converge_seconds))
    if wait_s:
        log("Waiting {}s for NLSR convergence before starting App_ServiceController".format(
            wait_s))
        time.sleep(wait_s)
    dump_nfd_route_fib_for_nodes(
        ndn, output_dir, "nlsr-convergence-after-wait", nlsr_diagnostic_nodes(args))


def nfd_table_lines(text):
    return [line.strip() for line in text.splitlines() if line.strip()]


def line_prefix(line):
    match = re.search(r"(?:^|\s)prefix=([^ ]+)", line)
    if match:
        return match.group(1)
    match = re.match(r"([^ ]+)\s+nexthops=", line)
    if match:
        return match.group(1)
    return ""


def prefix_matches(candidate, name):
    if not candidate:
        return False
    return name == candidate or name.startswith(candidate.rstrip("/") + "/")


def longest_matching_line(text, name):
    matches = []
    for line in nfd_table_lines(text):
        prefix = line_prefix(line)
        if prefix_matches(prefix, name):
            matches.append((len([c for c in prefix.split("/") if c]), line, prefix))
    if not matches:
        return None, ""
    _, line, prefix = max(matches, key=lambda item: item[0])
    return line, prefix


def exact_prefix_lines(text, prefix):
    lines = []
    for line in nfd_table_lines(text):
        if line_prefix(line) == prefix:
            lines.append(line)
    return lines


def related_prefix_lines(text, prefixes):
    related = []
    for line in nfd_table_lines(text):
        haystack = line
        if any(prefix in haystack or haystack.startswith(prefix) for prefix in prefixes):
            related.append(line)
    return related


def nexthop_count(line):
    if not line:
        return 0
    return len(re.findall(r"(?:faceid|nexthop)=", line))


def write_dk_forwarding_state(ndn, output_dir, label, args, provider_id):
    available = {node.name: node for node in ndn.net.hosts}
    node_name = args.dk_diagnostic_node
    if node_name not in available:
        raise RuntimeError("--dk-diagnostic-node {} is not in topology; available={}".format(
            node_name, sorted(available)))

    node = available[node_name]
    permission_interest = provider_permission_interest(provider_id)
    prefixes = dk_multicast_prefixes(args, provider_id)
    commands = {
        "fib": "nfdc fib list",
        "strategy": "nfdc strategy list",
        "route": "nfdc route list",
    }
    raw = {}
    for suffix, command in commands.items():
        text = node_cmd(node, "{} 2>&1".format(command))
        raw[suffix] = text
        (output_dir / "{}-{}-{}.txt".format(label, node_name, suffix)).write_text(text)
        related = related_prefix_lines(text, prefixes + [permission_interest])
        (output_dir / "{}-{}-{}-around-dk-prefix.txt".format(
            label, node_name, suffix)).write_text(
                "\n".join(related) + ("\n" if related else ""))

    fib_longest_line, fib_longest_prefix = longest_matching_line(raw["fib"], permission_interest)
    strategy_longest_line, strategy_longest_prefix = longest_matching_line(
        raw["strategy"], permission_interest)
    route_longest_line, route_longest_prefix = longest_matching_line(
        raw["route"], permission_interest)
    exact_fib = exact_prefix_lines(raw["fib"], permission_interest)
    exact_route = exact_prefix_lines(raw["route"], permission_interest)
    exact_strategy = exact_prefix_lines(raw["strategy"], permission_interest)
    summary = {
        "label": label,
        "node": node_name,
        "provider_id": provider_id,
        "exact_permission_interest": permission_interest,
        "exact_permission_parent": parent_name(permission_interest),
        "multicast_prefixes_checked": prefixes,
        "exact_fib_entry": exact_fib,
        "exact_route_entries": exact_route,
        "exact_strategy_entry": exact_strategy,
        "longest_matching_fib": {
            "prefix": fib_longest_prefix,
            "line": fib_longest_line or "",
            "nexthop_count": nexthop_count(fib_longest_line),
        },
        "longest_matching_route": {
            "prefix": route_longest_prefix,
            "line": route_longest_line or "",
            "nexthop_count": nexthop_count(route_longest_line),
        },
        "longest_matching_strategy": {
            "prefix": strategy_longest_prefix,
            "line": strategy_longest_line or "",
            "is_multicast": bool(strategy_longest_line and "multicast" in strategy_longest_line),
            "is_best_route": bool(strategy_longest_line and "best-route" in strategy_longest_line),
        },
    }
    (output_dir / "{}-dk-forwarding-state.json".format(label)).write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n")

    lines = [
        "DK/Permission Forwarding State ({})".format(label),
        "=" * 44,
        "node={}".format(node_name),
        "exact_permission_interest={}".format(permission_interest),
        "exact_permission_parent={}".format(parent_name(permission_interest)),
        "exact_fib_entry={}".format(bool(exact_fib)),
        "exact_route_entries={}".format(len(exact_route)),
        "exact_strategy_entry={}".format(bool(exact_strategy)),
        "longest_fib_prefix={} nexthops={} line={}".format(
            fib_longest_prefix, nexthop_count(fib_longest_line), fib_longest_line or ""),
        "longest_route_prefix={} nexthops={} line={}".format(
            route_longest_prefix, nexthop_count(route_longest_line), route_longest_line or ""),
        "longest_strategy_prefix={} multicast={} best-route={} line={}".format(
            strategy_longest_prefix,
            summary["longest_matching_strategy"]["is_multicast"],
            summary["longest_matching_strategy"]["is_best_route"],
            strategy_longest_line or ""),
    ]
    (output_dir / "{}-dk-forwarding-state.txt".format(label)).write_text(
        "\n".join(lines) + "\n")
    return summary


def write_prefix_forwarding_state(ndn, output_dir, label, node_names, exact_prefix):
    available = {node.name: node for node in ndn.net.hosts}
    report = {
        "label": label,
        "exact_prefix": exact_prefix,
        "nodes": {},
    }
    prefixes = [exact_prefix]
    parent = exact_prefix
    while parent and parent != "/":
        parent = parent_name(parent)
        prefixes.append(parent)
        if parent == "/":
            break
    for node_name in node_names:
        if node_name not in available:
            continue
        node = available[node_name]
        raw = {}
        for suffix, command in {
            "fib": "nfdc fib list",
            "strategy": "nfdc strategy list",
            "route": "nfdc route list",
        }.items():
            text = node_cmd(node, "{} 2>&1".format(command))
            raw[suffix] = text
            (output_dir / "{}-{}-{}.txt".format(label, node_name, suffix)).write_text(text)
            related = related_prefix_lines(text, prefixes)
            (output_dir / "{}-{}-{}-around-prefix.txt".format(
                label, node_name, suffix)).write_text(
                    "\n".join(related) + ("\n" if related else ""))
        fib_line, fib_prefix = longest_matching_line(raw["fib"], exact_prefix)
        route_line, route_prefix = longest_matching_line(raw["route"], exact_prefix)
        strategy_line, strategy_prefix = longest_matching_line(raw["strategy"], exact_prefix)
        report["nodes"][node_name] = {
            "exact_fib_entry": exact_prefix_lines(raw["fib"], exact_prefix),
            "exact_route_entries": exact_prefix_lines(raw["route"], exact_prefix),
            "exact_strategy_entry": exact_prefix_lines(raw["strategy"], exact_prefix),
            "longest_matching_fib": {
                "prefix": fib_prefix,
                "line": fib_line or "",
                "nexthop_count": nexthop_count(fib_line),
            },
            "longest_matching_route": {
                "prefix": route_prefix,
                "line": route_line or "",
                "nexthop_count": nexthop_count(route_line),
            },
            "longest_matching_strategy": {
                "prefix": strategy_prefix,
                "line": strategy_line or "",
                "is_multicast": bool(strategy_line and "multicast" in strategy_line),
                "is_best_route": bool(strategy_line and "best-route" in strategy_line),
            },
        }
    (output_dir / "{}-prefix-forwarding-state.json".format(label)).write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")
    lines = ["Prefix Forwarding State ({})".format(label), "=" * 36,
             "exact_prefix={}".format(exact_prefix)]
    for node_name, node_report in sorted(report["nodes"].items()):
        lines.append("[{}]".format(node_name))
        lines.append("longest_fib={}".format(node_report["longest_matching_fib"]))
        lines.append("longest_strategy={}".format(node_report["longest_matching_strategy"]))
        lines.append("longest_route={}".format(node_report["longest_matching_route"]))
    (output_dir / "{}-prefix-forwarding-state.txt".format(label)).write_text(
        "\n".join(lines) + "\n")
    return report


def set_dk_multicast_strategies(ndn, args, provider_id):
    prefixes = dk_multicast_prefixes(args, provider_id)
    log("Setting multicast strategy for DK/permission prefixes: {}".format(prefixes))
    for node in ndn.net.hosts:
        for prefix in prefixes:
            Nfdc.setStrategy(node, prefix, Nfdc.STRATEGY_MULTICAST)
    return prefixes


def dump_face_counters(ndn, output_dir, label):
    log("Dumping NFD face counters: {}".format(label))
    snapshots = {}
    for node in ndn.net.hosts:
        output = node_cmd(node, "nfdc face list 2>&1")
        path = result_file(output_dir, label, node.name, "-face-list.txt")
        path.write_text(output)
        snapshots[node.name] = parse_face_list(output)
    json_path = output_dir / "{}-face-counters.json".format(label)
    json_path.write_text(json.dumps(snapshots, indent=2, sort_keys=True) + "\n")
    return snapshots


def parse_face_list(output):
    faces = []
    current = None
    for line in output.splitlines():
        stripped = line.strip()
        match = re.match(r"faceid=(\d+)\s+remote=([^\s]+)\s+local=([^\s]+)", stripped)
        if match:
            current = {
                "faceid": match.group(1),
                "remote": match.group(2),
                "local": match.group(3),
                "counters": {},
                "raw": [stripped],
            }
            faces.append(current)
            continue
        if current is None:
            continue
        current["raw"].append(stripped)
        for key, value in re.findall(r"([a-zA-Z-]+)=([0-9]+)", stripped):
            current["counters"][key] = int(value)
    return faces


def compare_face_counter_snapshots(output_dir, labels):
    snapshots = {}
    for label in labels:
        path = output_dir / "{}-face-counters.json".format(label)
        if path.exists():
            snapshots[label] = json.loads(path.read_text())
    report = {
        "labels": labels,
        "nodes": {},
        "notes": [
            "nfdc face list counters are per face, not per prefix.",
            "SVS-related faces are inferred from non-local udp/tcp faces on user/provider nodes.",
        ],
    }
    interesting_nodes = {"user", "provider1", "provider2", "provider3"}
    counter_names = ["nInInterests", "nOutInterests", "nInData", "nOutData"]
    for node in interesting_nodes:
        node_report = []
        previous = None
        previous_label = None
        for label in labels:
            faces = snapshots.get(label, {}).get(node, [])
            inferred = [
                face for face in faces
                if not face.get("remote", "").startswith("internal://") and
                not face.get("remote", "").startswith("fd://")
            ]
            totals = {name: 0 for name in counter_names}
            for face in inferred:
                counters = face.get("counters", {})
                for name in counter_names:
                    totals[name] += counters.get(name, 0)
            delta = None
            if previous is not None:
                delta = {name: totals[name] - previous.get(name, 0)
                         for name in counter_names}
            node_report.append({
                "label": label,
                "inferred_svs_faces": [
                    {
                        "faceid": face.get("faceid"),
                        "remote": face.get("remote"),
                        "local": face.get("local"),
                        "counters": face.get("counters", {}),
                    }
                    for face in inferred
                ],
                "totals": totals,
                "delta_from_previous_label": delta,
                "previous_label": previous_label,
            })
            previous = totals
            previous_label = label
        if node_report:
            report["nodes"][node] = node_report
    (output_dir / "face-counter-comparison.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")
    lines = ["Face Counter Comparison", "=" * 24]
    for node, items in sorted(report["nodes"].items()):
        lines.append("[{}]".format(node))
        for item in items:
            lines.append("{} totals={} delta={}".format(
                item["label"], item["totals"], item["delta_from_previous_label"]))
    (output_dir / "face-counter-comparison.txt").write_text("\n".join(lines) + "\n")
    return report


def collect_nfd_packet_logs(ndn, output_dir, label):
    log("Collecting NFD packet logs: {}".format(label))
    summary = {}
    grep_dir = output_dir / "nfd-grep"
    grep_dir.mkdir(parents=True, exist_ok=True)
    pattern = re.compile("|".join(re.escape(item) for item in SVS_LOG_PATTERNS), re.I)
    for node in ndn.net.hosts:
        home_dir = Path(node.params["params"]["homeDir"])
        source = home_dir / "nfd.log"
        node_summary = {"source": str(source), "exists": source.exists(), "matches": 0}
        if source.exists():
            text = source.read_text(errors="replace")
            copy_path = output_dir / "{}-{}-nfd.log".format(label, node.name)
            copy_path.write_text(text)
            matches = [line for line in text.splitlines() if pattern.search(line)]
            grep_path = grep_dir / "{}-{}-nfd-svs-grep.log".format(label, node.name)
            grep_path.write_text("\n".join(matches) + ("\n" if matches else ""))
            node_summary.update({
                "copied_log": str(copy_path),
                "grep_log": str(grep_path),
                "matches": len(matches),
                "has_group": any(SVS_GROUP_PREFIX in line for line in matches),
                "has_interest": any("Interest" in line for line in matches),
                "has_data": any("Data" in line for line in matches),
                "has_timeout": any("timeout" in line.lower() for line in matches),
                "has_multicast": any("multicast" in line.lower() for line in matches),
            })
        summary[node.name] = node_summary
    path = output_dir / "{}-nfd-svs-log-summary.json".format(label)
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return summary


def collect_dk_packet_logs(ndn, output_dir, label, node_names=None):
    log("Collecting NFD DK packet logs: {}".format(label))
    selected = set(node_names or [node.name for node in ndn.net.hosts])
    pattern = re.compile("|".join(re.escape(item) for item in DK_LOG_PATTERNS), re.I)
    summary = {}
    grep_dir = output_dir / "nfd-dk-grep"
    grep_dir.mkdir(parents=True, exist_ok=True)
    for node in ndn.net.hosts:
        if node.name not in selected:
            continue
        home_dir = Path(node.params["params"]["homeDir"])
        source = home_dir / "nfd.log"
        item = {"source": str(source), "exists": source.exists(), "matches": 0}
        if source.exists():
            text = source.read_text(errors="replace")
            copy_path = output_dir / "{}-{}-nfd.log".format(label, node.name)
            copy_path.write_text(text)
            matches = [line for line in text.splitlines() if pattern.search(line)]
            grep_path = grep_dir / "{}-{}-nfd-dk-grep.log".format(label, node.name)
            grep_path.write_text("\n".join(matches) + ("\n" if matches else ""))
            item.update({
                "copied_log": str(copy_path),
                "grep_log": str(grep_path),
                "matches": len(matches),
                "has_dkey": any("DKEY" in line for line in matches),
                "has_interest": any("Interest" in line for line in matches),
                "has_data": any("Data" in line for line in matches),
            })
        summary[node.name] = item
    (output_dir / "{}-nfd-dk-log-summary.json".format(label)).write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return summary


def extract_dkey_names_from_text(text):
    names = []
    for match in re.finditer(r"(/[A-Za-z0-9%._~:;=+\\-]+(?:/[A-Za-z0-9%._~:;=+\\-]+)*)", text):
        name = match.group(1)
        if "/DKEY" in name or name.endswith("/DKEY"):
            names.append(name.rstrip(",;"))
    return list(dict.fromkeys(names))


def infer_dkey_names(output_dir):
    names = []
    for path in sorted(output_dir.glob("**/*")):
        if not path.is_file():
            continue
        if path.suffix not in (".log", ".txt"):
            continue
        if "nfd" not in path.name and "provider" not in path.name and "controller" not in path.name:
            continue
        names.extend(extract_dkey_names_from_text(path.read_text(errors="replace")))
    return list(dict.fromkeys(names))


def choose_exact_dkey_interest(names):
    dkey_names = [name for name in names if "/DKEY/" in name]
    if not dkey_names:
        return ""
    return max(dkey_names, key=lambda item: (len(item.split("/")), len(item)))


def log_tail(path, line_count=80):
    p = Path(path)
    if not p.exists():
        return ""
    return "\n".join(p.read_text(errors="replace").splitlines()[-line_count:])


def wait_for_nfd_sockets(ndn, output_dir, timeout_s=12):
    nodes = list(ndn.net.hosts)
    deadline = time.time() + timeout_s
    missing = []
    while time.time() < deadline:
        missing = [
            node.name for node in nodes
            if not Path("/run/nfd/{}.sock".format(node.name)).exists()
        ]
        if not missing:
            return
        time.sleep(0.25)

    diagnostics = {}
    for node in nodes:
        home = Path(node.params["params"]["homeDir"])
        nfd_log = home / "log" / "nfd.log"
        copied_log = ""
        if nfd_log.exists():
            copied = output_dir / "nfd-startup-{}-nfd.log".format(node.name)
            copied.write_text(nfd_log.read_text(errors="replace"))
            copied_log = str(copied)
        diagnostics[node.name] = {
            "socket": "/run/nfd/{}.sock".format(node.name),
            "socket_exists": Path("/run/nfd/{}.sock".format(node.name)).exists(),
            "home": str(home),
            "nfd_log": str(nfd_log),
            "copied_log": copied_log,
            "nfd_log_tail": log_tail(nfd_log, 80),
        }
    (output_dir / "nfd-startup-failure.json").write_text(
        json.dumps(diagnostics, indent=2, sort_keys=True) + "\n")
    raise RuntimeError(
        "NFD socket startup failure: missing sockets for {}".format(
            ",".join(missing)))


def node_home_report(ndn, node_names):
    available = {node.name: node for node in ndn.net.hosts}
    report = {}
    for node_name in node_names:
        node = available.get(node_name)
        if node is None:
            continue
        home = Path(node.params["params"]["homeDir"])
        report[node_name] = {
            "HOME": str(home),
            "PIB": str(home / ".ndn" / "pib"),
            "TPM": str(home / ".ndn" / "ndnsec-key-file"),
        }
    return report


def diagnostic_prefixes(provider_count):
    prefixes = {
        "service_request_prefix": "/example/hello/user/NDNSF/REQUEST",
        "svs_group_prefix": SVS_GROUP_PREFIX,
        "svs_sync_prefix": SVS_SYNC_PREFIX,
        "controller_prefix": "/example/hello/controller",
        "controller_permission_prefix": "/example/hello/controller/NDNSF/PERMISSIONS",
        "connectivity_prefix": CONNECTIVITY_PREFIX,
    }
    for provider_id in provider_ids(provider_count):
        prefixes["provider_{}_prefix".format(provider_id)] = "/example/hello/provider/{}".format(provider_id)
        prefixes["ack_data_{}_prefix".format(provider_id)] = "/example/hello/provider/{}/NDNSF/ACK".format(provider_id)
    return prefixes


def provider_identity_prefix(provider_id):
    return "/example/hello/provider/{}".format(provider_id)


def provider_bootstrap_prefixes(provider_id):
    provider_prefix = provider_identity_prefix(provider_id)
    return {
        "provider_{}_prefix".format(provider_id): provider_prefix,
        "provider_{}_ndnsf_prefix".format(provider_id): ndn_name_join(provider_prefix, "NDNSF"),
        "provider_{}_ck_prefix".format(provider_id): ndn_name_join(provider_prefix, "CK"),
        "provider_{}_key_prefix".format(provider_id): ndn_name_join(provider_prefix, "KEY"),
    }


def controller_bootstrap_prefixes():
    return {
        "controller_prefix": "/example/hello/controller",
        "controller_ndnsf_prefix": "/example/hello/controller/NDNSF",
        "controller_permission_prefix": "/example/hello/controller/NDNSF/PERMISSIONS",
    }


def permission_interest_name(kind, identity):
    return ndn_name_join("/example/hello/controller/NDNSF/PERMISSIONS/{}".format(kind),
                         identity)


def parent_name(name):
    components = [component for component in name.strip("/").split("/") if component]
    if len(components) <= 1:
        return "/"
    return "/" + "/".join(components[:-1])


def provider_permission_interest(provider_id):
    return permission_interest_name("PROVIDER", provider_identity_prefix(provider_id))


def provider_dkey_prefix(provider_id):
    return "/example/hello/controller/DKEY"


def dk_multicast_prefixes(args, provider_id):
    provider_prefix = provider_identity_prefix(provider_id)
    permission_interest = provider_permission_interest(provider_id)
    prefixes = [
        "/example/hello/controller",
        "/example/hello/controller/NDNSF",
        "/example/hello/controller/NDNSF/PERMISSIONS",
        "/example/hello/controller/NDNSF/PERMISSIONS/PROVIDER",
        parent_name(permission_interest),
        permission_interest,
        ndn_name_join(provider_prefix, "CK"),
        ndn_name_join(provider_prefix, "KEY"),
    ]
    return list(dict.fromkeys(prefixes))


def bootstrap_prefixes(args):
    prefixes = controller_bootstrap_prefixes()
    prefixes["user_prefix"] = "/example/hello/user"
    prefixes["user_permission_interest"] = permission_interest_name(
        "USER", "/example/hello/user")
    for provider_id in selected_provider_ids(args):
        provider_prefix = provider_identity_prefix(provider_id)
        prefixes.update(provider_bootstrap_prefixes(provider_id))
        prefixes["provider_{}_permission_interest".format(provider_id)] = (
            permission_interest_name("PROVIDER", provider_prefix))
    return prefixes


def svs_node_prefix(role, provider_id=None):
    if role == "user":
        return "/example/hello/user/user"
    if role == "provider":
        suffix = provider_id or "A"
        return "/example/hello/provider/{}/provider".format(suffix)
    return ""


def svs_data_prefix(role, provider_id=None):
    return ndn_name_join(svs_node_prefix(role, provider_id), SVS_GROUP_PREFIX)


def ndn_name_join(prefix, suffix):
    return "{}/{}".format(prefix.rstrip("/"), suffix.lstrip("/"))


def read_ndnsf_config(output_dir):
    config_path = output_dir / "ndnsf.conf"
    entries = {}
    if not config_path.exists():
        return entries
    for line in config_path.read_text(errors="replace").splitlines():
        parts = line.split(",")
        if len(parts) == 3:
            entries[(parts[0], parts[1])] = parts[2]
    return entries


def write_svs_prefix_report(output_dir, provider_count, session_base, label):
    entries = read_ndnsf_config(output_dir)
    home_notes = {
        "HOME": "MiniNDN per-node homeDir; see per-node environment in this report",
        "PIB": "$HOME/.ndn/pib",
        "TPM": "$HOME/.ndn/ndnsec-key-file",
    }
    nodes = {
        "user": {
            "configured_svs_group_prefix": SVS_GROUP_PREFIX,
            "actual_svs_sync_prefix": SVS_SYNC_PREFIX,
            "node_name_without_session": svs_node_prefix("user"),
            "session_id": entries.get((SVS_GROUP_PREFIX, svs_node_prefix("user")), str(session_base)),
            "data_prefix": svs_data_prefix("user"),
            "signing_identity": "/example/hello/user",
            "validator_config_path": "examples/trust-any.conf",
            "paths": home_notes,
        }
    }
    for provider_id in provider_ids(provider_count):
        node_name = svs_node_prefix("provider", provider_id)
        nodes["provider-{}".format(provider_id)] = {
            "configured_svs_group_prefix": SVS_GROUP_PREFIX,
            "actual_svs_sync_prefix": SVS_SYNC_PREFIX,
            "node_name_without_session": node_name,
            "session_id": entries.get((SVS_GROUP_PREFIX, node_name), str(session_base)),
            "data_prefix": svs_data_prefix("provider", provider_id),
            "signing_identity": "/example/hello/provider/{}".format(provider_id),
            "validator_config_path": "examples/trust-any.conf",
            "paths": home_notes,
        }
    report = {
        "label": label,
        "note": "Derived from App_User/App_Provider and ndn-svs SVSPubSub/SVSync construction; no protocol change.",
        "sync_prefix_identity_requirement": "All apps must share actual_svs_sync_prefix.",
        "nodes": nodes,
    }
    path = output_dir / "{}-svs-prefix-report.json".format(label)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    txt = output_dir / "{}-svs-prefix-report.txt".format(label)
    lines = ["SVS Prefix Report ({})".format(label), "=" * 32]
    for name, item in nodes.items():
        lines.append("[{}]".format(name))
        for key, value in item.items():
            lines.append("{}={}".format(key, value))
    txt.write_text("\n".join(lines) + "\n")
    return report


def write_routing_diagnostics(ndn, output_dir, provider_count, label):
    prefixes = diagnostic_prefixes(provider_count)
    report = {
        "label": label,
        "prefixes": prefixes,
        "nodes": {},
        "strategy_notes": [],
    }
    for node in ndn.net.hosts:
        route_list = node_cmd(node, "nfdc route list 2>&1")
        fib_list = node_cmd(node, "nfdc fib list 2>&1")
        strategy_list = node_cmd(node, "nfdc strategy list 2>&1")
        node_report = {}
        for name, prefix in prefixes.items():
            node_report[name] = {
                "prefix": prefix,
                "in_route_list": prefix in route_list,
                "in_fib_list": prefix in fib_list,
            }
        report["nodes"][node.name] = node_report

        strategy_prefixes = [
            "/example/hello",
            SVS_GROUP_PREFIX,
            SVS_SYNC_PREFIX,
            ndn_name_join(SVS_GROUP_PREFIX, "sync"),
            ndn_name_join(SVS_GROUP_PREFIX, "s"),
            ndn_name_join(SVS_GROUP_PREFIX, "d"),
        ]
        for prefix in dict.fromkeys(strategy_prefixes):
            strategy_lines = [line for line in strategy_list.splitlines() if prefix in line]
            uses_multicast = any("multicast" in line for line in strategy_lines)
            report["strategy_notes"].append({
                "node": node.name,
                "prefix": prefix,
                "strategy_lines": strategy_lines,
                "uses_multicast": uses_multicast,
                "smallest_safe_change": None if uses_multicast else
                    "nfdc strategy set {} /localhost/nfd/strategy/multicast".format(prefix),
            })

    json_path = output_dir / "{}-routing-diagnostics.json".format(label)
    txt_path = output_dir / "{}-routing-diagnostics.txt".format(label)
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")

    lines = ["MiniNDN Routing Diagnostics ({})".format(label), "=" * 40]
    for item in report["strategy_notes"]:
        lines.append("node={} prefix={} multicast={} lines={}".format(
            item["node"], item["prefix"], item["uses_multicast"], item["strategy_lines"]))
        if item["smallest_safe_change"]:
            lines.append("  proposed: {}".format(item["smallest_safe_change"]))
    lines.append("")
    for node_name, node_report in report["nodes"].items():
        lines.append("[{}]".format(node_name))
        for name, status in node_report.items():
            lines.append("{} route={} fib={} prefix={}".format(
                name, status["in_route_list"], status["in_fib_list"], status["prefix"]))
    txt_path.write_text("\n".join(lines) + "\n")
    return report


def ndnping_succeeded(returncode, output):
    packet_match = re.search(r"([1-9][0-9]*)\s+packets?\s+received", output, re.I)
    content_match = re.search(r"content\s+from", output, re.I)
    return returncode == 0 and bool(packet_match or content_match)


def run_connectivity_smoke(ndn, args, output_dir):
    # Mirrors MiniNDN's official ping-demo.py pattern: start ndnpingserver on
    # the producer node and run ndnping from the consumer node, saving outputs.
    provider_node_name = selected_provider_nodes(args)[0]
    provider = ndn.net[provider_node_name]
    user = ndn.net[args.user_node]
    server_log_path = output_dir / "connectivity-{}-ndnpingserver.log".format(provider_node_name)
    client_log_path = output_dir / "connectivity-{}-ndnping.log".format(args.user_node)

    log("Running ndnping connectivity smoke test")
    processes = []
    server_log = server_log_path.open("wb")
    client_log = client_log_path.open("wb")
    try:
        server = getPopen(provider,
                          "exec ndnpingserver {}".format(CONNECTIVITY_PREFIX),
                          shell=True,
                          stdout=server_log,
                          stderr=subprocess.STDOUT)
        processes.append((server, server_log))
        time.sleep(1)

        client = getPopen(user,
                          "exec timeout 15s ndnping {} -c 3".format(CONNECTIVITY_PREFIX),
                          shell=True,
                          stdout=client_log,
                          stderr=subprocess.STDOUT)
        processes.append((client, client_log))
        try:
            client.wait(timeout=20)
        except subprocess.TimeoutExpired:
            client.kill()
            client.wait(timeout=PROCESS_KILL_TIMEOUT_SECONDS)
            raise RuntimeError("MiniNDN ndnping connectivity check timed out; see {}".format(
                client_log_path))
        time.sleep(0.5)
    finally:
        stop_processes(processes)

    output = client_log_path.read_text(errors="replace")
    success = ndnping_succeeded(client.returncode, output)
    summary = {
        "prefix": CONNECTIVITY_PREFIX,
        "server_node": provider_node_name,
        "client_node": args.user_node,
        "client_returncode": client.returncode,
        "success": bool(success),
        "server_log": str(server_log_path),
        "client_log": str(client_log_path),
    }
    (output_dir / "connectivity-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n")
    if not success:
        raise RuntimeError("MiniNDN ndnping connectivity check failed; see {}".format(client_log_path))
    log("ndnping connectivity smoke test passed")


def initialize_example_keychains(ndn, args, output_dir):
    # The App_* examples were originally exercised against one local keychain.
    # MiniNDN gives each node an isolated HOME, so pre-install the same example
    # identities on every node before the apps call getOrCreateIdentity().
    log("Installing example keychain material on MiniNDN nodes")
    security_dir = output_dir / "security"
    security_dir.mkdir(parents=True, exist_ok=True)

    identities = [
        "/example/hello/controller",
        "/example/hello/provider",
        "/example/hello/user",
    ] + ["/example/hello/provider/{}".format(pid) for pid in selected_provider_ids(args)]

    for node in ndn.net.hosts:
        for identity in identities:
            node_cmd(node, "ndnsec delete {} >/dev/null 2>&1 || true".format(
                shell_quote(identity)))

    controller = ndn.net[args.controller_node]
    exported_keys = []
    provider_cert_records = []
    for index, identity in enumerate(identities):
        cert_path = security_dir / "identity-{}.cert".format(index)
        key_path = security_dir / "identity-{}.ndnkey".format(index)
        node_cmd(controller, "ndnsec key-gen -t r {} > {}".format(
            shell_quote(identity), shell_quote(cert_path)))
        node_cmd(controller, "ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            shell_quote(cert_path)))
        node_cmd(controller, "ndnsec-export -P 123456 -o {} -i {}".format(
            shell_quote(key_path), shell_quote(identity)))
        exported_keys.append(key_path)
        if identity.startswith("/example/hello/provider/") and identity.count("/") == 4:
            cert_name = certificate_name_from_file(cert_path)
            provider_id = identity.rsplit("/", 1)[1]
            provider_cert_records.append({
                "provider_id": provider_id,
                "identity": identity,
                "cert_name": cert_name,
                "cert_file": str(cert_path),
            })

    for node in ndn.net.hosts:
        for key_path in exported_keys:
            node_cmd(node, "ndnsec import -P 123456 {} >/dev/null 2>&1 || true".format(
                shell_quote(key_path)))

    if getattr(args, "serve_provider_certs", False) and getattr(args, "dk_bootstrap_check", False):
        for record in provider_cert_records:
            node_cmd(controller, "ndnsec delete {} >/dev/null 2>&1 || true".format(
                shell_quote(record["identity"])))
            log("provider_cert controller_local_identity_removed identity={} cert={}".format(
                record["identity"], record["cert_name"]))

    args.provider_cert_records = provider_cert_records
    write_provider_cert_manifest(output_dir, provider_cert_records)
    for record in provider_cert_records:
        log("provider_cert provider={} name={} file={}".format(
            record["provider_id"], record["cert_name"], record["cert_file"]))


def certificate_name_from_file(cert_path):
    raw = base64.b64decode("".join(
        line.strip()
        for line in Path(cert_path).read_text(errors="replace").splitlines()
        if line and not line.startswith("-----")))
    cert_name, _, _, _ = parse_data(raw)
    return Name.to_str(cert_name)


def write_provider_cert_manifest(output_dir, records):
    path = output_dir / "provider-certificates.json"
    path.write_text(json.dumps(records, indent=2, sort_keys=True) + "\n")
    txt_path = output_dir / "provider-certificates.txt"
    lines = ["Provider Certificates", "====================="]
    for record in records:
        lines.append("provider-{} identity={} cert={}".format(
            record["provider_id"], record["identity"], record["cert_name"]))
        lines.append("file={}".format(record["cert_file"]))
    txt_path.write_text("\n".join(lines) + "\n")


def start_provider_cert_servers(ndn, args, output_dir, processes):
    records = getattr(args, "provider_cert_records", [])
    if not getattr(args, "serve_provider_certs", False):
        return []
    if not records:
        raise RuntimeError("--serve-provider-certs requested but no provider certificates were exported")

    by_provider = {record["provider_id"]: record for record in records}
    servers = []
    for node_name, provider_id in zip(selected_provider_nodes(args), selected_provider_ids(args)):
        record = by_provider.get(provider_id)
        if record is None:
            raise RuntimeError("Missing certificate record for provider {}".format(provider_id))
        node = ndn.net[node_name]
        log_path = output_dir / "cert-server-{}.log".format(provider_id)
        log("Starting provider certificate server provider-{} on {} cert={}".format(
            provider_id, node_name, record["cert_name"]))
        log_file = log_path.open("wb")
        argv = " ".join([
            shell_quote(sys.executable),
            shell_quote(REPO_ROOT / "Experiments" / "cert_server.py"),
            "--cert-file", shell_quote(record["cert_file"]),
            "--expected-name", shell_quote(record["cert_name"]),
        ])
        cmd = "cd {} && exec {}".format(shell_quote(REPO_ROOT), argv)
        process = getPopen(
            node,
            cmd,
            envDict={
                "HOME": node.params["params"]["homeDir"],
                "PYTHONUNBUFFERED": "1",
            },
            shell=True,
            stdout=log_file,
            stderr=subprocess.STDOUT)
        processes.append((process, log_file))
        ready = wait_for_log(log_path, r"CERT_SERVER_READY", 5, process)
        servers.append({
            "provider_id": provider_id,
            "node": node_name,
            "cert_name": record["cert_name"],
            "cert_file": record["cert_file"],
            "log_path": str(log_path),
            "ready": ready,
            "process_exit_status": process.poll(),
        })
        if not ready:
            raise RuntimeError("Provider certificate server failed to start; see {}".format(log_path))
    write_provider_cert_server_summary(output_dir, servers, "startup")
    return servers


def cert_server_state(output_dir, records):
    def scan(path, provider_cert):
        state = {
            "publisher_ready": False,
            "server_ready": False,
            "interest_seen": False,
            "data_served": False,
            "interest_count": 0,
            "data_count": 0,
            "tail": [],
        }
        if not path.exists():
            return state
        with path.open(errors="replace") as f:
            for line in f:
                stripped = line.rstrip("\n")
                state["tail"].append(stripped)
                if len(state["tail"]) > 40:
                    state["tail"].pop(0)
                has_cert = provider_cert in line
                if "NDNSF_CERT_PUBLISHER_READY" in line:
                    state["publisher_ready"] = True
                if "CERT_SERVER_READY" in line or "NDNSF_CERT_PUBLISHER_READY" in line:
                    state["server_ready"] = True
                if "CERT_SERVER_INTEREST" in line or "NDNSF_CERT_PUBLISHER_INTEREST" in line:
                    state["interest_count"] += 1
                    if has_cert:
                        state["interest_seen"] = True
                if "CERT_SERVER_DATA" in line or "NDNSF_CERT_PUBLISHER_DATA" in line:
                    state["data_count"] += 1
                    if has_cert:
                        state["data_served"] = True
        return state

    states = []
    for record in records:
        provider_id = record["provider_id"]
        log_path = output_dir / "cert-server-{}.log".format(provider_id)
        if not log_path.exists() and (output_dir.parent / "cert-server-{}.log".format(provider_id)).exists():
            log_path = output_dir.parent / "cert-server-{}.log".format(provider_id)
        app_log_candidates = [
            output_dir / "provider-{}.log".format(provider_id),
            output_dir / "dk-bootstrap-provider-{}.log".format(provider_id),
            output_dir.parent / "provider-{}.log".format(provider_id),
            output_dir.parent / "dk-bootstrap-provider-{}.log".format(provider_id),
        ]
        server_state = scan(log_path, record["cert_name"])
        app_states = [scan(path, record["cert_name"]) for path in app_log_candidates if path.exists()]
        all_states = [server_state] + app_states
        tail = []
        for state in all_states:
            tail.extend(state["tail"])
        tail = tail[-40:]
        states.append({
            "provider_id": provider_id,
            "identity": record["identity"],
            "cert_name": record["cert_name"],
            "cert_file": record["cert_file"],
            "server_log": str(log_path),
            "app_certificate_publisher": any(state["publisher_ready"] for state in app_states),
            "server_ready": any(state["server_ready"] for state in all_states),
            "aa_cert_interest_seen": any(state["interest_seen"] for state in all_states),
            "cert_data_served": any(state["data_served"] for state in all_states),
            "interest_count": sum(state["interest_count"] for state in all_states),
            "data_count": sum(state["data_count"] for state in all_states),
            "log_tail": "\n".join(tail),
        })
    return states


def write_provider_cert_server_summary(output_dir, servers, label):
    path = output_dir / "{}-provider-cert-servers.json".format(label)
    path.write_text(json.dumps(servers, indent=2, sort_keys=True) + "\n")
    lines = ["Provider Certificate Servers ({})".format(label), "=" * 42]
    for server in servers:
        lines.append("provider-{} node={} ready={} cert={}".format(
            server["provider_id"], server["node"], server["ready"], server["cert_name"]))
        lines.append("log={}".format(server["log_path"]))
    txt_path = output_dir / "{}-provider-cert-servers.txt".format(label)
    txt_path.write_text("\n".join(lines) + "\n")
    return txt_path


def write_provider_cert_runtime_diagnostics(output_dir, args, label):
    states = cert_server_state(output_dir, getattr(args, "provider_cert_records", []))
    if not states:
        return []
    path = output_dir / "{}-provider-cert-runtime.json".format(label)
    path.write_text(json.dumps(states, indent=2, sort_keys=True) + "\n")
    lines = ["Provider Certificate Runtime ({})".format(label), "=" * 42]
    for state in states:
        lines.append("provider-{} server_ready={} aa_cert_interest_seen={} cert_data_served={}".format(
            state["provider_id"], state["server_ready"],
            state["aa_cert_interest_seen"], state["cert_data_served"]))
        lines.append("cert={}".format(state["cert_name"]))
    (output_dir / "{}-provider-cert-runtime.txt".format(label)).write_text(
        "\n".join(lines) + "\n")
    return states


def app_env(output_dir, session_base, args):
    ndn_log = os.environ.get("NDN_LOG", "ndn_service_framework.*=INFO")
    if args.debug_ack:
        ndn_log = os.environ.get("NDN_LOG", "ndn_service_framework.*=TRACE:ndnsvs.*=INFO")
    if getattr(args, "timeline_trace", False):
        ndn_log = os.environ.get(
            "NDN_LOG",
            "ndn_service_framework.*=DEBUG:ndn_service_framework.TimelineTrace=DEBUG:ndnsvs.*=INFO")
    if getattr(args, "dk_bootstrap_check", False):
        ndn_log = os.environ.get(
            "NDN_LOG",
            "ndn_service_framework.*=TRACE:nacabe.*=TRACE:ndn.nacabe.*=TRACE")
    env = {
        "LD_LIBRARY_PATH": "{}:{}".format(REPO_ROOT / "build",
                                          os.environ.get("LD_LIBRARY_PATH", "")),
        "NDNSF_DISABLE_NDNSD": "1",
        "NDNSF_CONFIG": str(output_dir / "ndnsf.conf"),
        "NDNSF_SESSION_BASE": str(session_base),
        "NDN_LOG": ndn_log,
    }
    if getattr(args, "crypto_diagnostics", False):
        env["NDNSF_CRYPTO_DIAG"] = "1"
    if getattr(args, "timeline_trace", False):
        env["NDNSF_TIMELINE_TRACE"] = "1"
    if getattr(args, "diag_plaintext_ack", False):
        env["NDNSF_CRYPTO_DIAG"] = "1"
        env["NDNSF_DIAG_PLAINTEXT_ACK"] = "1"
    if getattr(args, "diag_plaintext_response", False):
        env["NDNSF_CRYPTO_DIAG"] = "1"
        env["NDNSF_DIAG_PLAINTEXT_RESPONSE"] = "1"
    if getattr(args, "svs_parallel_sync_processing", False):
        env["NDNSF_SVS_PARALLEL_SYNC"] = "1"
        env["NDNSF_SVS_PARALLEL_WORKERS"] = str(max(1, args.svs_parallel_workers))
        env["NDNSF_SVS_PARALLEL_QUEUE"] = str(max(1, args.svs_parallel_queue))
    if getattr(args, "svs_sync_publish", False):
        env["NDNSF_SVS_ASYNC_PUBLISH"] = "0"
    if getattr(args, "svs_disable_parallel_production", False):
        env["NDNSF_SVS_PARALLEL_PRODUCTION"] = "0"
    elif getattr(args, "svs_parallel_production_workers", None) is not None:
        env["NDNSF_SVS_PARALLEL_PRODUCTION"] = str(
            max(1, args.svs_parallel_production_workers))
    if getattr(args, "svs_disable_parallel_production_signing", False):
        env["NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING"] = "0"
    elif getattr(args, "svs_parallel_production_signing", False):
        env["NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING"] = "1"
    if getattr(args, "svs_disable_parallel_production_extra_block", False):
        env["NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK"] = "0"
    elif getattr(args, "svs_parallel_production_extra_block", False):
        env["NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK"] = "1"
    if getattr(args, "svs_sync_batching", False):
        env["NDNSF_SVS_SYNC_BATCHING"] = "1"
        env["NDNSF_SVS_SYNC_BATCH_MS"] = str(max(0, args.svs_sync_batch_ms))
    return env


def managed_cmd(binary, args):
    argv = " ".join([shell_quote(binary)] + [shell_quote(arg) for arg in args])
    return "cd {} && exec {}".format(shell_quote(REPO_ROOT), argv)


def start_process(node, name, binary, argv, output_dir, session_base, processes, args):
    log_path = output_dir / "{}.log".format(name)
    log("Starting {} on {} -> {}".format(name, node.name, log_path))
    log_file = log_path.open("wb")
    started_us = now_us()
    write_app_launch_diagnostics(log_file, node, name, session_base, started_us)
    process = getPopen(node,
                       managed_cmd(binary, argv),
                       envDict=app_env(output_dir, session_base, args),
                       shell=True,
                       stdout=log_file,
                       stderr=subprocess.STDOUT)
    processes.append((process, log_file))
    return process, log_path


def write_app_launch_diagnostics(log_file, node, name, session_base, started_us):
    if name == "user":
        role = "user"
        provider_id = None
        signing_identity = "/example/hello/user"
    elif name.startswith("provider-"):
        role = "provider"
        provider_id = name.split("-", 1)[1]
        signing_identity = "/example/hello/provider/{}".format(provider_id)
    elif name == "controller":
        role = "controller"
        provider_id = None
        signing_identity = "/example/hello/controller"
    else:
        return
    node_prefix = svs_node_prefix(role, provider_id)
    event_name = "{}_process_start".format(role)
    lines = [
        "[MiniNDN Harness] event={} timestamp_us={} node={} name={} provider_id={}".format(
            event_name, started_us, node.name, name, provider_id or ""),
        "[MiniNDN Harness] configured SVS group prefix={}".format(SVS_GROUP_PREFIX),
        "[MiniNDN Harness] actual ndn-svs sync prefix={}".format(SVS_SYNC_PREFIX),
        "[MiniNDN Harness] node/session name={} / sessionBase={}".format(node_prefix, session_base),
        "[MiniNDN Harness] data prefix={}".format(svs_data_prefix(role, provider_id)),
        "[MiniNDN Harness] signing identity={}".format(signing_identity),
        "[MiniNDN Harness] validator config path=examples/trust-any.conf",
        "[MiniNDN Harness] HOME={}".format(node.params["params"]["homeDir"]),
        "[MiniNDN Harness] PIB={}".format(
            Path(node.params["params"]["homeDir"]) / ".ndn" / "pib"),
        "[MiniNDN Harness] TPM={}".format(
            Path(node.params["params"]["homeDir"]) / ".ndn" / "ndnsec-key-file"),
    ]
    log_file.write(("\n".join(lines) + "\n").encode("utf-8"))
    log_file.flush()


def stop_processes(processes):
    for process, _ in reversed(processes):
        if process.poll() is None:
            process.terminate()
    deadline = time.time() + PROCESS_TERMINATE_TIMEOUT_SECONDS
    for process, log_file in reversed(processes):
        while process.poll() is None and time.time() < deadline:
            try:
                process.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                pass
        if process.poll() is None:
            process.kill()
            try:
                process.wait(timeout=PROCESS_KILL_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                log("WARNING: process pid={} did not exit after SIGKILL".format(process.pid))
        try:
            log_file.flush()
        except ValueError:
            pass
        log_file.close()


def wait_for_log(log_path, pattern, timeout_s, process=None):
    deadline = time.time() + timeout_s
    compiled = re.compile(pattern)
    while time.time() < deadline:
        if log_path.exists() and compiled.search(log_path.read_text(errors="replace")):
            return True
        if process is not None and process.poll() is not None:
            return False
        time.sleep(0.2)
    return False


class ProviderReadinessError(RuntimeError):
    def __init__(self, summary_path, readiness):
        super().__init__("Provider readiness failed; see {}".format(summary_path))
        self.summary_path = summary_path
        self.readiness = readiness


def provider_ready_state(log_path, provider_id):
    text = log_path.read_text(errors="replace") if log_path.exists() else ""
    event_patterns = {
        "provider_process_start": r"\[MiniNDN Harness\] event=provider_process_start timestamp_us=(\d+)",
        "provider_cert_server_ready": r"^([0-9]+\.[0-9]+)\s+.*Serving certificate name=/example/hello/provider/{}".format(
            re.escape(provider_id)),
        "provider_svs_ready": r"^([0-9]+\.[0-9]+)\s+.*Register NDNSF Messages in ndn-svs",
        "provider_service_registered": r"^([0-9]+\.[0-9]+)\s+.*Registered service handler for /HELLO",
    }
    timestamps = {}
    for key, pattern in event_patterns.items():
        match = re.search(pattern, text, re.M)
        if match:
            value = match.group(1)
            timestamps[key] = int(value) if value.isdigit() else int(float(value) * 1000000)
    checks = {
        "service_registered": bool(re.search(
            r"Provider {} registered service /HELLO|Registered service handler for /HELLO".format(
                re.escape(provider_id)), text)),
        "provider_permission_installed": bool(re.search(
            r"Installed provider permission provider=/example/hello/provider/{} service=/HELLO".format(
                re.escape(provider_id)), text)),
        "svs_initialized_or_subscribed": bool(re.search(
            r"Register NDNSF Messages in ndn-svs|SVS request subscription regex|"
            r"\^\(<>.*\)<NDNSF><REQUEST><1><HELLO>", text)),
    }
    diagnostics = {
        "dkey_interest_expressed": "DK_INTEREST_EXPRESSED" in text,
        "dkey_data_received": "DK_DATA_RECEIVED" in text,
        "dkey_decrypt_success": "DK_DECRYPT_SUCCESS" in text,
        "provider_permission_interest_expressed": "Fetch provider permissions:" in text,
        "provider_permission_installed": checks["provider_permission_installed"],
    }
    return {
        "provider_id": provider_id,
        "log_path": str(log_path),
        "ready": all(checks.values()),
        "checks": checks,
        "diagnostics": diagnostics,
        "timestamps_us": timestamps,
        "log_tail": "\n".join(text.splitlines()[-80:]),
    }


def wait_for_provider_ready(log_path, provider_id, timeout_s, process=None):
    deadline = time.time() + timeout_s
    state = provider_ready_state(log_path, provider_id)
    while time.time() < deadline:
        state = provider_ready_state(log_path, provider_id)
        if state["ready"]:
            return state
        if process is not None and process.poll() is not None:
            state["process_exit_status"] = process.returncode
            return state
        time.sleep(0.2)
    state = provider_ready_state(log_path, provider_id)
    if process is not None:
        state["process_exit_status"] = process.poll()
    return state


def write_provider_readiness_summary(output_dir, readiness, label, success, timeout_s):
    summary = {
        "label": label,
        "success": bool(success),
        "provider_ready_timeout_seconds": timeout_s,
        "providers": readiness,
    }
    path = output_dir / "{}-provider-readiness-summary.json".format(label)
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    lines = [
        "Provider Readiness Summary ({})".format(label),
        "=" * 38,
        "overall success: {}".format(bool(success)),
    ]
    for item in readiness:
        lines.append("[provider-{}] ready={}".format(item["provider_id"], item["ready"]))
        for key, value in sorted(item["checks"].items()):
            lines.append("{}={}".format(key, value))
        for key, value in sorted(item.get("diagnostics", {}).items()):
            lines.append("{}={}".format(key, value))
        if not item["ready"]:
            lines.append("log: {}".format(item["log_path"]))
            lines.append("log tail:")
            lines.extend("  {}".format(line) for line in item["log_tail"].splitlines())
    txt_path = output_dir / "{}-provider-readiness-summary.txt".format(label)
    txt_path.write_text("\n".join(lines) + "\n")
    return txt_path


def fail_provider_readiness(output_dir, readiness, label):
    timeout_s = max(item.get("timeout_s", 0) for item in readiness) if readiness else 0
    txt_path = write_provider_readiness_summary(output_dir, readiness, label, False, timeout_s)
    raise ProviderReadinessError(txt_path, readiness)


def provider_ready_wait_seconds(args):
    if args.provider_ready_wait is not None:
        return max(0, int(args.provider_ready_wait))
    return max(0, int(args.post_ready_settle_seconds))


def confirm_provider_route_registered(ndn, args, output_dir, label, provider_id, node_name):
    node = ndn.net[node_name]
    provider_prefix = "/example/hello/provider/{}".format(provider_id)
    route_list = node_cmd(node, "nfdc route list 2>&1")
    fib_list = node_cmd(node, "nfdc fib list 2>&1")
    path_base = "{}-provider-{}-{}".format(label, provider_id, node_name)
    (output_dir / "{}-route-list.txt".format(path_base)).write_text(route_list)
    (output_dir / "{}-fib-list.txt".format(path_base)).write_text(fib_list)
    return {
        "provider_id": provider_id,
        "node": node_name,
        "provider_prefix": provider_prefix,
        "route_registered": provider_prefix in route_list or provider_prefix in fib_list,
        "svs_group_route_registered": SVS_GROUP_PREFIX in route_list or SVS_GROUP_PREFIX in fib_list,
        "timestamp_us": now_us(),
    }


def write_startup_barrier_report(ndn, args, output_dir, label, readiness):
    route_checks = []
    for provider_id, node_name in zip(selected_provider_ids(args), selected_provider_nodes(args)):
        route_checks.append(confirm_provider_route_registered(
            ndn, args, output_dir, label, provider_id, node_name))
    cert_checks = cert_server_state(output_dir, getattr(args, "provider_cert_records", []))
    report = {
        "label": label,
        "timestamp_us": now_us(),
        "provider_ready_wait_seconds": provider_ready_wait_seconds(args),
        "provider_readiness": readiness,
        "route_checks": route_checks,
        "provider_certificate_runtime": cert_checks,
        "all_providers_ready": all(item.get("ready", False) for item in readiness),
        "all_provider_routes_registered": all(item.get("route_registered", False)
                                              for item in route_checks),
        "all_provider_certificates_advertised": all(
            item.get("server_ready", False) for item in cert_checks) if cert_checks else None,
    }
    report["barrier_ready"] = (
        report["all_providers_ready"] and
        (report["all_provider_routes_registered"] or not route_checks) and
        (report["all_provider_certificates_advertised"] is not False))
    (output_dir / "{}-startup-barrier.json".format(label)).write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")
    lines = [
        "Startup Readiness Barrier ({})".format(label),
        "=" * 40,
        "provider_ready_wait_seconds={}".format(report["provider_ready_wait_seconds"]),
        "all_providers_ready={}".format(report["all_providers_ready"]),
        "all_provider_routes_registered={}".format(report["all_provider_routes_registered"]),
        "all_provider_certificates_advertised={}".format(
            report["all_provider_certificates_advertised"]),
        "barrier_ready={}".format(report["barrier_ready"]),
    ]
    for item in route_checks:
        lines.append("provider-{} route_registered={} svs_group_route_registered={} node={}".format(
            item["provider_id"], item["route_registered"],
            item["svs_group_route_registered"], item["node"]))
    (output_dir / "{}-startup-barrier.txt".format(label)).write_text(
        "\n".join(lines) + "\n")
    return report


def percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = int((pct / 100.0) * len(ordered) + 0.999999) - 1
    return ordered[max(0, min(index, len(ordered) - 1))]


def parse_key_values(line):
    result = {}
    for part in line.strip().split():
        if "=" in part:
            key, value = part.split("=", 1)
            result[key] = value
    return result


def workload_notes(mode, adaptive_admission=False):
    if mode == "closed-loop":
        return [
            "App_User sends the next request only after the previous response or timeout plus interval_ms.",
            "The offered rate is an interval-derived target, not a guaranteed generated request rate.",
            "Use actual_requests_per_second, computed from PERF_REQUEST_SENT timestamps, as the achieved send rate.",
        ]
    if adaptive_admission:
        return [
            "App_User uses open-loop rate ticks as an upper bound; ServiceUser owns the adaptive admission window.",
            "When the runtime window is full, App_User retries without catch-up bursting or applying a second local window controller.",
            "Use actual_requests_per_second, adaptive_window_* and outstanding_limit_skips to inspect admitted load.",
            "Low success rate at high rps means saturation/timeout, not necessarily protocol correctness failure.",
        ]
    return [
        "App_User sends on a fixed rate schedule and does not wait for previous requests to finish.",
        "Multiple pending calls are tracked by request id; responses are drained after sending stops.",
        "Use actual_requests_per_second and outstanding_limit_skips to confirm generated load.",
        "50/100/150 rps are real offered rates only when actual_requests_per_second is close to offered_rate_rps.",
        "Low success rate at high rps means saturation/timeout, not necessarily protocol correctness failure.",
    ]


def offered_rate_rps(args):
    if args.workload_mode == "open-loop" and getattr(args, "rate_rps", None) is not None:
        return float(args.rate_rps)
    interval_ms = max(float(args.interval_ms), 1.0)
    return 1000.0 / interval_ms


def request_timestamp_metrics(sent_timestamps, success_count, requested_duration_s):
    timestamps = sorted(int(ts) for ts in sent_timestamps if ts)
    requested_duration_s = float(requested_duration_s or 0.0)
    if len(timestamps) >= 2:
        span_s = max(0.0, (timestamps[-1] - timestamps[0]) / 1000.0)
    else:
        span_s = 0.0

    sampling_duration_s = span_s if span_s > 0.0 else requested_duration_s
    if len(timestamps) >= 2 and span_s > 0.0:
        actual_requests_per_second = (len(timestamps) - 1) / span_s
    elif sampling_duration_s > 0.0:
        actual_requests_per_second = len(timestamps) / sampling_duration_s
    else:
        actual_requests_per_second = 0.0

    actual_successful_responses_per_second = (
        success_count / sampling_duration_s) if sampling_duration_s > 0.0 else 0.0
    return {
        "sampling_duration_s": sampling_duration_s,
        "request_timestamp_span_s": span_s,
        "actual_requests_per_second": actual_requests_per_second,
        "actual_successful_responses_per_second": actual_successful_responses_per_second,
        "perf_request_sent_count": len(timestamps),
        "first_request_sent_ts": timestamps[0] if timestamps else None,
        "last_request_sent_ts": timestamps[-1] if timestamps else None,
    }


def achieved_rate_warning(summary):
    offered = float(summary.get("offered_rate_rps") or 0.0)
    actual = float(summary.get("actual_requests_per_second") or 0.0)
    if offered > 0.0 and actual < offered * 0.8:
        return (
            "WARNING: actual_requests_per_second={:.3f} is below 80% of "
            "offered_rate_rps={:.3f}; do not claim the offered rate was generated."
        ).format(actual, offered)
    return ""


def parse_int_like(value, fallback=0):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return fallback


def parse_float_like(value, fallback=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return fallback


def first_timestamp_us_from_log(path, patterns):
    if not path.exists():
        return None
    compiled = [(re.compile(pattern), scale) for pattern, scale in patterns]
    with path.open(errors="replace") as f:
        for line in f:
            for pattern, scale in compiled:
                match = pattern.search(line)
                if match:
                    value = match.group(1)
                    if scale == "us":
                        return int(value)
                    if scale == "ms":
                        return int(float(value) * 1000)
                    return int(float(value) * 1000000)
    return None


def collect_startup_timestamps(output_dir):
    result = {}
    controller_startup = output_dir / "controller-startup.json"
    if not controller_startup.exists():
        controller_startup = output_dir.parent / "controller-startup.json"
    if controller_startup.exists():
        try:
            result["controller_ready"] = json.loads(
                controller_startup.read_text()).get("timestamp_us")
        except json.JSONDecodeError:
            result["controller_ready"] = None
    else:
        controller_log = output_dir / "controller.log"
        if not controller_log.exists():
            controller_log = output_dir.parent / "controller.log"
        result["controller_ready"] = first_timestamp_us_from_log(controller_log, [
            (r"\[MiniNDN Harness\] event=controller_ready timestamp_us=(\d+)", "us"),
            (r"^([0-9]+\.[0-9]+).*ServiceController listening on:", "sec"),
        ])
    user_log = output_dir / "user.log"
    result["user_process_start"] = first_timestamp_us_from_log(user_log, [
        (r"\[MiniNDN Harness\] event=user_process_start timestamp_us=(\d+)", "us"),
    ])
    result["user_first_request_sent"] = first_timestamp_us_from_log(user_log, [
        (r"PERF_REQUEST_SENT .*?ts=(\d+)", "ms"),
        (r"event=REQUEST_CREATED timestamp_us=(\d+)", "us"),
    ])
    result["first_ack_received_by_user"] = first_timestamp_us_from_log(user_log, [
        (r"event=ACK_RECEIVED timestamp_us=(\d+)", "us"),
    ])
    result["first_coordination_sent"] = first_timestamp_us_from_log(user_log, [
        (r"event=COORDINATION_PUBLISHED timestamp_us=(\d+)", "us"),
    ])
    result["first_response_received"] = first_timestamp_us_from_log(user_log, [
        (r"event=RESPONSE_RECEIVED timestamp_us=(\d+)", "us"),
    ])

    provider_events = {}
    first_ack_published = None
    for log_path in sorted(output_dir.glob("provider-*.log")):
        provider_id = log_path.stem.split("-", 1)[1]
        events = {
            "provider_process_start": first_timestamp_us_from_log(log_path, [
                (r"\[MiniNDN Harness\] event=provider_process_start timestamp_us=(\d+)", "us"),
            ]),
            "provider_cert_server_ready": first_timestamp_us_from_log(log_path, [
                (r"^([0-9]+\.[0-9]+).*Serving certificate name=/example/hello/provider/{}".format(
                    re.escape(provider_id)), "sec"),
            ]),
            "provider_svs_ready": first_timestamp_us_from_log(log_path, [
                (r"^([0-9]+\.[0-9]+).*Register NDNSF Messages in ndn-svs", "sec"),
            ]),
            "provider_service_registered": first_timestamp_us_from_log(log_path, [
                (r"^([0-9]+\.[0-9]+).*Registered service handler for /HELLO", "sec"),
            ]),
            "first_ack_published": first_timestamp_us_from_log(log_path, [
                (r"event=ACK_PUBLISHED timestamp_us=(\d+)", "us"),
            ]),
        }
        provider_events[provider_id] = events
        if events["first_ack_published"] is not None:
            first_ack_published = min(first_ack_published, events["first_ack_published"]) if first_ack_published else events["first_ack_published"]
    result["providers"] = provider_events
    result["first_ack_published"] = first_ack_published

    barrier_path = output_dir / "pre-user-startup-barrier.json"
    if barrier_path.exists():
        try:
            barrier = json.loads(barrier_path.read_text())
            result["startup_barrier"] = barrier
            route_times = [
                item.get("timestamp_us")
                for item in barrier.get("route_checks", [])
                if item.get("route_registered")
            ]
            result["provider_route_registered"] = min(route_times) if route_times else None
        except json.JSONDecodeError:
            result["startup_barrier"] = {}
    return result


def selected_provider_skew(selected_distribution):
    total = sum(int(v) for v in selected_distribution.values())
    if total == 0:
        return {
            "skewed": False,
            "dominant_provider": "",
            "dominant_share": 0.0,
            "concentrated_on_C": False,
        }
    dominant_provider, dominant_count = max(
        selected_distribution.items(), key=lambda item: int(item[1]))
    dominant_share = float(dominant_count) / float(total)
    return {
        "skewed": total >= 10 and dominant_share >= 0.70,
        "dominant_provider": dominant_provider,
        "dominant_share": dominant_share,
        "concentrated_on_C": dominant_provider.endswith("/C") or dominant_provider == "C",
    }


def classify_bottlenecks(summary):
    classes = []
    reasons = []
    sent_count = int(summary.get("sent_count", 0) or 0)
    completed_count = int(summary.get("completed_count", 0) or 0)
    timed_out_count = int(summary.get("timed_out_count", 0) or 0)
    skipped = int(summary.get("outstanding_limit_skip_count", 0) or 0)
    pending = int(summary.get("pending_at_shutdown", 0) or 0)
    achieved_offered = float(summary.get("achieved_offered_ratio", 0.0) or 0.0)
    success_rate = float(summary.get("success_rate", 0.0) or 0.0)
    max_observed = int(summary.get("max_outstanding_requests", 0) or 0)
    configured_max = int(summary.get("max_outstanding", 0) or 0)
    total_acks = sum(int(v) for v in summary.get("ack_count_per_provider", {}).values())
    selected_total = sum(int(v) for v in summary.get("selected_provider_distribution", {}).values())
    avg_acks_per_sent = (float(total_acks) / sent_count) if sent_count else 0.0
    skew = summary.get("provider_selection_skew", {})

    if skipped > 0 or (configured_max > 0 and max_observed >= int(configured_max * 0.95)):
        classes.append("outstanding-limit constrained")
        reasons.append("request generation hit max_outstanding {} times; max observed outstanding was {}/{}".format(
            skipped, max_observed, configured_max))

    if timed_out_count > 0 and (timed_out_count >= max(3, completed_count * 0.25) or success_rate < 0.80):
        classes.append("timeout dominated")
        reasons.append("{} requests timed out; completed_count={} success_rate={:.2%}".format(
            timed_out_count, completed_count, success_rate))

    if achieved_offered < 0.80 and skipped == 0 and timed_out_count < max(3, sent_count * 0.10):
        classes.append("app scheduling limited")
        reasons.append("actual request generation was {:.2%} of offered rate without many skips/timeouts".format(
            achieved_offered))

    if sent_count > 0 and avg_acks_per_sent < 0.50 and skipped == 0:
        classes.append("network/SVS delivery limited")
        reasons.append("only {:.3f} ACK log entries per sent request".format(avg_acks_per_sent))
    elif selected_total < completed_count and timed_out_count > 0 and skipped == 0:
        classes.append("network/SVS delivery limited")
        reasons.append("some completed/timeout requests had no parseable provider selection")

    if skew.get("skewed"):
        classes.append("provider selection skew")
        reasons.append("provider {} handled {:.2%} of selections".format(
            skew.get("dominant_provider", ""), float(skew.get("dominant_share", 0.0))))

    if not classes and pending > 0:
        classes.append("timeout dominated")
        reasons.append("{} requests were still pending at shutdown".format(pending))
    if not classes:
        classes.append("no clear bottleneck")
        reasons.append("no dominant skip, timeout, delivery, scheduling, or provider-skew signal")

    return {
        "primary": classes[0],
        "all": classes,
        "reasons": reasons,
    }


def provider_readiness_status(readiness):
    return {
        "ready": bool(readiness) and all(item.get("ready", False) for item in readiness),
        "providers": {
            item.get("provider_id", ""): {
                "ready": bool(item.get("ready", False)),
                "checks": item.get("checks", {}),
                "log_path": item.get("log_path", ""),
            }
            for item in readiness
        },
    }


def collect_crypto_diagnostics(output_dir):
    rows = []
    for log_path in sorted(output_dir.glob("*.log")):
        with log_path.open(errors="replace") as f:
            for line in f:
                if "[NDNSF_CRYPTO_DIAG]" not in line:
                    continue
                kv = parse_key_values(line[line.index("[NDNSF_CRYPTO_DIAG]"):])
                try:
                    duration_us = int(kv.get("duration_us", "0") or 0)
                    start_us = int(kv.get("start_us", "0") or 0)
                    end_us = int(kv.get("end_us", "0") or 0)
                except ValueError:
                    continue
                row = {
                    "log": log_path.name,
                    "role": kv.get("role", ""),
                    "stage": kv.get("stage", ""),
                    "op": kv.get("op", ""),
                    "mode": kv.get("mode", ""),
                    "status": kv.get("status", ""),
                    "start_us": start_us,
                    "end_us": end_us,
                    "duration_us": duration_us,
                    "name": kv.get("name", ""),
                    "bytes": parse_int_like(kv.get("bytes"), 0),
                }
                rows.append(row)

    groups = defaultdict(list)
    for row in rows:
        groups[(row["role"], row["stage"], row["op"], row["mode"])].append(row)

    summary = {}
    for key, items in sorted(groups.items()):
        role, stage, op, mode = key
        durations = [item["duration_us"] for item in items]
        starts = [item["start_us"] for item in items if item["start_us"]]
        ends = [item["end_us"] for item in items if item["end_us"]]
        first_ts = min(starts) if starts else None
        last_ts = max(ends) if ends else None
        span_s = ((last_ts - first_ts) / 1000000.0) if first_ts and last_ts and last_ts > first_ts else 0.0
        group_key = "{}.{}.{}.{}".format(role, stage, op, mode)
        summary[group_key] = {
            "role": role,
            "stage": stage,
            "op": op,
            "mode": mode,
            "total_count": len(items),
            "success_count": sum(1 for item in items if item["status"] == "success"),
            "failure_count": sum(1 for item in items if item["status"] != "success"),
            "p50_duration_us": percentile(durations, 50),
            "p95_duration_us": percentile(durations, 95),
            "p99_duration_us": percentile(durations, 99),
            "throughput_per_second": (len(items) / span_s) if span_s > 0.0 else 0.0,
            "first_timestamp_us": first_ts,
            "last_timestamp_us": last_ts,
            "span_s": span_s,
        }

    csv_path = output_dir / "crypto_diagnostics.csv"
    with csv_path.open("w", newline="") as f:
        fieldnames = [
            "log", "role", "stage", "op", "mode", "status",
            "start_us", "end_us", "duration_us", "bytes", "name",
        ]
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    json_path = output_dir / "crypto_diagnostics_summary.json"
    json_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return {
        "summary": summary,
        "csv": str(csv_path),
        "json": str(json_path),
    }


def collect_post_ack_pipeline(output_dir, sent_request_ids=None):
    user_log = output_dir / "user.log"
    sent_request_ids = set(sent_request_ids or [])
    per_request = defaultdict(lambda: {
        "request_id": "",
        "pending_created_us": "",
        "pending_erased_us": "",
        "first_ack_arrival_us": "",
        "ack_match_attempt_pre_decrypt": 0,
        "ack_match_attempt_decoded": 0,
        "ack_match_failed_no_pending": 0,
        "ack_match_failed_expired_call": 0,
        "ack_match_failed_request_id_mismatch": 0,
        "ack_match_skipped_pre_decrypt": 0,
        "ack_matched_pending_call": 0,
        "ack_matched_us": "",
        "selection_invoked_us": "",
        "selection_completed_us": "",
        "selected_provider": "",
        "coordination_scheduling_attempt_us": "",
        "coordination_scheduled_us": "",
        "coordination_publish_attempt_us": "",
        "coordination_publish_us": "",
        "coordination_publish_status": "",
        "coordination_to_provider_coordination_received_us": "",
        "response_completion_us": "",
        "response_observed_us": "",
        "response_decrypt_start_us": "",
        "response_decrypt_done_us": "",
        "response_decrypt_failed_us": "",
        "response_validation_start_us": "",
        "response_validation_done_us": "",
        "response_validation_failed_us": "",
        "callback_attempt_us": "",
        "provider_coordination_received_us": "",
        "provider_coordination_decrypt_start_us": "",
        "provider_coordination_decrypt_done_us": "",
        "provider_coordination_decrypt_failed_us": "",
        "provider_coordination_no_pending_us": "",
        "provider_execute_start_us": "",
        "provider_execute_done_us": "",
        "provider_response_publish_attempt_us": "",
        "provider_response_dispatched_us": "",
        "provider_response_published_us": "",
        "provider_response_publish_failed_us": "",
        "callback_fired_us": "",
        "callback_skipped_no_pending_us": "",
        "callback_skipped_timeout_us": "",
        "timeout_us": "",
        "timeout_stage": "",
        "final_classification": "",
        "lat_request_created_to_ack_matched_us": "",
        "lat_ack_matched_to_provider_selected_us": "",
        "lat_provider_selected_to_coordination_published_us": "",
        "lat_coordination_published_to_provider_coordination_received_us": "",
        "lat_provider_coordination_received_to_response_published_us": "",
        "lat_response_published_to_response_observed_us": "",
        "lat_response_observed_to_callback_fired_us": "",
        "ack_processed": 0,
    })
    counters = Counter()

    def row_for(request_id):
        row = per_request[request_id]
        row["request_id"] = request_id
        return row

    def set_once(row, key, value):
        if value and not row.get(key):
            row[key] = value

    if user_log.exists():
        with user_log.open(errors="replace") as f:
            for line in f:
                if "[NDNSF_TRACE]" not in line:
                    if "PERF_RESPONSE_RECEIVED " in line:
                        kv = parse_key_values(line[line.index("PERF_RESPONSE_RECEIVED "):])
                        request_id = kv.get("id", "")
                        timestamp_ms = kv.get("ts", "")
                        if request_id and timestamp_ms:
                            set_once(row_for(request_id), "callback_fired_us",
                                     str(int(timestamp_ms) * 1000))
                    continue
                kv = parse_key_values(line)
                event = kv.get("event", "")
                request_id = kv.get("requestId", "")
                timestamp_us = kv.get("timestamp_us", "")
                if not request_id:
                    continue
                row = row_for(request_id)
                if event in ("REQUEST_PENDING_CREATED", "REQUEST_CREATED"):
                    counters["pending_created"] += 1
                    set_once(row, "pending_created_us", timestamp_us)
                elif event in ("REQUEST_PENDING_COMPLETED",
                               "REQUEST_PENDING_TIMEOUT",
                               "REQUEST_PENDING_ERASED"):
                    if event != "REQUEST_PENDING_ERASED" or not row.get("pending_erased_us"):
                        counters[event.lower()] += 1
                    set_once(row, "pending_erased_us", timestamp_us)
                elif event == "ACK_MATCH_ATTEMPT":
                    phase = kv.get("phase", "")
                    counters["ack_match_attempt"] += 1
                    if phase == "pre_decrypt":
                        counters["ack_match_attempt_pre_decrypt"] += 1
                        row["ack_match_attempt_pre_decrypt"] += 1
                    elif phase == "decoded_ack":
                        counters["ack_match_attempt_decoded"] += 1
                        row["ack_match_attempt_decoded"] += 1
                elif event == "ACK_MATCHED_PENDING_CALL":
                    counters["ack_matched_pending_call"] += 1
                    row["ack_matched_pending_call"] += 1
                    set_once(row, "ack_matched_us", timestamp_us)
                elif event == "ACK_MATCH_FAILED_NO_PENDING_CALL":
                    counters["ack_match_failed_no_pending"] += 1
                    row["ack_match_failed_no_pending"] += 1
                elif event == "ACK_MATCH_FAILED_EXPIRED_CALL":
                    counters["ack_match_failed_expired_call"] += 1
                    row["ack_match_failed_expired_call"] += 1
                elif event == "ACK_MATCH_FAILED_REQUEST_ID_MISMATCH":
                    counters["ack_match_failed_request_id_mismatch"] += 1
                    row["ack_match_failed_request_id_mismatch"] += 1
                elif event == "ACK_MATCH_SKIPPED_PRE_DECRYPT":
                    counters["ack_match_skipped_pre_decrypt"] += 1
                    row["ack_match_skipped_pre_decrypt"] += 1
                elif event == "ACK_RECEIVED":
                    counters["ack_processed"] += 1
                    row["ack_processed"] += 1
                    set_once(row, "first_ack_arrival_us", timestamp_us)
                elif event.startswith("ACK_IGNORED_") or event.startswith("ACK_REJECTED_"):
                    counters["ack_processed"] += 1
                    row["ack_processed"] += 1
                elif event == "FIRST_ACK_OBSERVED":
                    set_once(row, "first_ack_arrival_us", timestamp_us)
                elif event == "ACK_SELECTION_BEGIN":
                    counters["selection_invoked"] += 1
                    set_once(row, "selection_invoked_us", timestamp_us)
                elif event == "ACK_SELECTION_END":
                    counters["selection_completed"] += 1
                    set_once(row, "selection_completed_us", timestamp_us)
                    provider = kv.get("selectedProvider", "")
                    if provider and provider != "-":
                        row["selected_provider"] = provider
                elif event == "PROVIDER_SELECTED":
                    provider = kv.get("selectedProvider", "")
                    if provider and provider != "-":
                        row["selected_provider"] = provider
                    set_once(row, "selection_completed_us", timestamp_us)
                elif event == "COORDINATION_SCHEDULE_ATTEMPT":
                    counters["coordination_attempted"] += 1
                    set_once(row, "coordination_scheduling_attempt_us", timestamp_us)
                elif event == "COORDINATION_SCHEDULED_CALLBACK":
                    counters["coordination_scheduled"] += 1
                    set_once(row, "coordination_scheduled_us", timestamp_us)
                elif event == "COORDINATION_PUBLISH_ATTEMPT":
                    set_once(row, "coordination_publish_attempt_us", timestamp_us)
                elif event == "COORDINATION_PUBLISHED":
                    counters["coordination_published"] += 1
                    set_once(row, "coordination_publish_us", timestamp_us)
                    row["coordination_publish_status"] = "success"
                elif event == "COORDINATION_PUBLISH_FAILED":
                    counters["coordination_publish_failed"] += 1
                    set_once(row, "coordination_publish_us", timestamp_us)
                    row["coordination_publish_status"] = "failure"
                elif event == "COORDINATION_SKIPPED":
                    counters["coordination_skipped"] += 1
                elif event == "COORDINATION_REJECTED":
                    counters["coordination_rejected"] += 1
                elif event == "RESPONSE_RECEIVED":
                    set_once(row, "response_completion_us", timestamp_us)
                elif event == "RESPONSE_OBSERVED":
                    set_once(row, "response_observed_us", timestamp_us)
                elif event == "RESPONSE_DECRYPT_START":
                    set_once(row, "response_decrypt_start_us", timestamp_us)
                elif event == "RESPONSE_DECRYPT_DONE":
                    set_once(row, "response_decrypt_done_us", timestamp_us)
                elif event == "RESPONSE_DECRYPT_FAILED":
                    set_once(row, "response_decrypt_failed_us", timestamp_us)
                elif event == "RESPONSE_DECRYPTED_NO_PENDING":
                    set_once(row, "callback_skipped_no_pending_us", timestamp_us)
                elif event == "RESPONSE_VALIDATION_START":
                    set_once(row, "response_validation_start_us", timestamp_us)
                elif event == "RESPONSE_VALIDATION_DONE":
                    set_once(row, "response_validation_done_us", timestamp_us)
                elif event == "RESPONSE_VALIDATION_FAILED":
                    set_once(row, "response_validation_failed_us", timestamp_us)
                elif event == "CALLBACK_ATTEMPT":
                    set_once(row, "callback_attempt_us", timestamp_us)
                elif event == "CALLBACK_FIRED":
                    set_once(row, "callback_fired_us", timestamp_us)
                elif event == "CALLBACK_SKIPPED_NO_PENDING":
                    counters["callback_skipped_no_pending"] += 1
                    set_once(row, "callback_skipped_no_pending_us", timestamp_us)
                elif event == "CALLBACK_SKIPPED_TIMEOUT":
                    counters["callback_skipped_timeout"] += 1
                    set_once(row, "callback_skipped_timeout_us", timestamp_us)
                elif event == "TIMEOUT_FIRED":
                    set_once(row, "timeout_us", timestamp_us)

    for log_path in output_dir.glob("provider-*.log"):
        with log_path.open(errors="replace") as f:
            for line in f:
                if "[NDNSF_TRACE]" not in line:
                    continue
                kv = parse_key_values(line)
                event = kv.get("event", "")
                request_id = kv.get("requestId", "")
                timestamp_us = kv.get("timestamp_us", "")
                if not request_id:
                    continue
                row = row_for(request_id)
                if event == "COORDINATION_RECEIVED":
                    set_once(row, "provider_coordination_received_us", timestamp_us)
                elif event == "COORDINATION_DECRYPT_START":
                    set_once(row, "provider_coordination_decrypt_start_us", timestamp_us)
                elif event == "COORDINATION_DECRYPT_DONE":
                    set_once(row, "provider_coordination_decrypt_done_us", timestamp_us)
                elif event == "COORDINATION_DECRYPT_FAILED":
                    set_once(row, "provider_coordination_decrypt_failed_us", timestamp_us)
                elif event == "COORDINATION_NO_PENDING":
                    counters["coordination_no_pending"] += 1
                    set_once(row, "provider_coordination_no_pending_us", timestamp_us)
                elif event == "PROVIDER_EXECUTE_START":
                    set_once(row, "provider_execute_start_us", timestamp_us)
                elif event == "PROVIDER_EXECUTE_DONE":
                    set_once(row, "provider_execute_done_us", timestamp_us)
                elif event == "RESPONSE_PUBLISH_ATTEMPT":
                    set_once(row, "provider_response_publish_attempt_us", timestamp_us)
                elif event == "RESPONSE_DISPATCHED":
                    set_once(row, "provider_response_dispatched_us", timestamp_us)
                elif event == "RESPONSE_PUBLISHED":
                    set_once(row, "provider_response_published_us", timestamp_us)
                elif event == "RESPONSE_PUBLISH_FAILED":
                    set_once(row, "provider_response_publish_failed_us", timestamp_us)

    all_request_ids = sorted(sent_request_ids | set(per_request.keys()))

    def set_latency(row, dest, start_key, end_key):
        start = row.get(start_key, "")
        end = row.get(end_key, "")
        if not start or not end:
            return
        try:
            row[dest] = str(int(end) - int(start))
        except ValueError:
            pass

    for request_id in all_request_ids:
        row = row_for(request_id)
        set_latency(row, "lat_request_created_to_ack_matched_us",
                    "pending_created_us", "ack_matched_us")
        set_latency(row, "lat_ack_matched_to_provider_selected_us",
                    "ack_matched_us", "selection_completed_us")
        set_latency(row, "lat_provider_selected_to_coordination_published_us",
                    "selection_completed_us", "coordination_publish_us")
        set_latency(row, "lat_coordination_published_to_provider_coordination_received_us",
                    "coordination_publish_us", "provider_coordination_received_us")
        set_latency(row, "lat_provider_coordination_received_to_response_published_us",
                    "provider_coordination_received_us", "provider_response_published_us")
        set_latency(row, "lat_response_published_to_response_observed_us",
                    "provider_response_published_us", "response_observed_us")
        set_latency(row, "lat_response_observed_to_callback_fired_us",
                    "response_observed_us", "callback_fired_us")
        if row["callback_fired_us"]:
            row["final_classification"] = "success"
        elif not row["pending_created_us"]:
            row["final_classification"] = "1_request_never_created_pending_call"
            counters["bucket_1_request_never_created_pending_call"] += 1
        elif row["ack_match_skipped_pre_decrypt"]:
            row["final_classification"] = "3_ack_received_but_skipped_pre_decrypt"
            counters["bucket_3_ack_received_but_skipped_pre_decrypt"] += 1
        elif row["ack_match_failed_no_pending"] and not row["ack_matched_pending_call"]:
            if row["pending_erased_us"]:
                row["final_classification"] = "2_pending_call_created_but_erased_before_ack"
                counters["bucket_2_pending_call_created_but_erased_before_ack"] += 1
            else:
                row["final_classification"] = "4_ack_decoded_but_no_pending_call_matched"
                counters["bucket_4_ack_decoded_but_no_pending_call_matched"] += 1
        elif row["ack_match_attempt_decoded"] and not row["ack_matched_pending_call"]:
            row["final_classification"] = "4_ack_decoded_but_no_pending_call_matched"
            counters["bucket_4_ack_decoded_but_no_pending_call_matched"] += 1
        elif row["ack_matched_pending_call"] and not row["selected_provider"]:
            row["final_classification"] = "5_ack_matched_pending_call_but_no_provider_selected"
            counters["bucket_5_ack_matched_pending_call_but_no_provider_selected"] += 1
        elif row["selected_provider"] and not row["coordination_scheduling_attempt_us"]:
            row["final_classification"] = "6_provider_selected_but_coordination_not_scheduled"
            counters["bucket_6_provider_selected_but_coordination_not_scheduled"] += 1
        elif row["coordination_scheduling_attempt_us"] and not row["coordination_publish_us"]:
            row["final_classification"] = "7_coordination_scheduled_but_not_published"
            counters["bucket_7_coordination_scheduled_but_not_published"] += 1
        elif row["coordination_publish_us"] and not row["provider_response_published_us"]:
            row["final_classification"] = "8_coordination_published_but_provider_did_not_execute_respond"
            counters["bucket_8_coordination_published_but_provider_did_not_execute_respond"] += 1
        elif row["provider_response_published_us"] and not row["callback_fired_us"]:
            row["final_classification"] = "9_response_published_but_user_callback_not_triggered"
            counters["bucket_9_response_published_but_user_callback_not_triggered"] += 1
        else:
            row["final_classification"] = "10_real_network_svs_nac_loss_or_timeout"
            counters["bucket_10_real_network_svs_nac_loss_or_timeout"] += 1

        if row["timeout_us"]:
            if not row["selection_invoked_us"] and not row["selection_completed_us"]:
                row["timeout_stage"] = "before_selection"
                counters["requests_timed_out_before_selection"] += 1
            elif not row["selected_provider"]:
                row["timeout_stage"] = "after_selection_no_provider"
            elif not row["coordination_publish_us"]:
                row["timeout_stage"] = "after_selection_before_coordination"
                counters["requests_timed_out_after_selection_before_coordination"] += 1
            elif not row["response_completion_us"]:
                row["timeout_stage"] = "after_coordination_before_response"
        if not row["selected_provider"]:
            counters["requests_with_no_selection"] += 1

    counters.setdefault("selection_invoked", 0)
    counters.setdefault("selection_completed", 0)
    counters.setdefault("pending_created", 0)
    counters.setdefault("request_pending_completed", 0)
    counters.setdefault("request_pending_timeout", 0)
    counters.setdefault("request_pending_erased", 0)
    counters.setdefault("ack_match_attempt", 0)
    counters.setdefault("ack_match_attempt_pre_decrypt", 0)
    counters.setdefault("ack_match_attempt_decoded", 0)
    counters.setdefault("ack_matched_pending_call", 0)
    counters.setdefault("ack_match_failed_no_pending", 0)
    counters.setdefault("ack_match_failed_expired_call", 0)
    counters.setdefault("ack_match_failed_request_id_mismatch", 0)
    counters.setdefault("ack_match_skipped_pre_decrypt", 0)
    counters.setdefault("coordination_attempted", 0)
    counters.setdefault("coordination_scheduled", 0)
    counters.setdefault("coordination_published", 0)
    counters.setdefault("coordination_publish_failed", 0)
    counters.setdefault("coordination_skipped", 0)
    counters.setdefault("coordination_rejected", 0)
    counters.setdefault("coordination_no_pending", 0)
    counters.setdefault("callback_skipped_no_pending", 0)
    counters.setdefault("callback_skipped_timeout", 0)
    counters.setdefault("requests_with_no_selection", 0)
    counters.setdefault("requests_timed_out_before_selection", 0)
    counters.setdefault("requests_timed_out_after_selection_before_coordination", 0)
    bucket_keys = [
        "bucket_1_request_never_created_pending_call",
        "bucket_2_pending_call_created_but_erased_before_ack",
        "bucket_3_ack_received_but_skipped_pre_decrypt",
        "bucket_4_ack_decoded_but_no_pending_call_matched",
        "bucket_5_ack_matched_pending_call_but_no_provider_selected",
        "bucket_6_provider_selected_but_coordination_not_scheduled",
        "bucket_7_coordination_scheduled_but_not_published",
        "bucket_8_coordination_published_but_provider_did_not_execute_respond",
        "bucket_9_response_published_but_user_callback_not_triggered",
        "bucket_10_real_network_svs_nac_loss_or_timeout",
    ]
    for key in bucket_keys:
        counters.setdefault(key, 0)
    failed_count = sum(counters[key] for key in bucket_keys)
    failure_buckets = {
        key.replace("bucket_", "", 1): {
            "count": counters[key],
            "percent_of_failed": ((100.0 * counters[key] / failed_count)
                                  if failed_count else 0.0),
        }
        for key in bucket_keys
    }

    csv_path = output_dir / "post_ack_pipeline.csv"
    fieldnames = [
        "request_id",
        "pending_created_us",
        "pending_erased_us",
        "first_ack_arrival_us",
        "ack_match_attempt_pre_decrypt",
        "ack_match_attempt_decoded",
        "ack_match_failed_no_pending",
        "ack_match_failed_expired_call",
        "ack_match_failed_request_id_mismatch",
        "ack_match_skipped_pre_decrypt",
        "ack_matched_pending_call",
        "ack_matched_us",
        "selection_invoked_us",
        "selection_completed_us",
        "selected_provider",
        "coordination_scheduling_attempt_us",
        "coordination_scheduled_us",
        "coordination_publish_attempt_us",
        "coordination_publish_us",
        "coordination_publish_status",
        "response_completion_us",
        "response_observed_us",
        "response_decrypt_start_us",
        "response_decrypt_done_us",
        "response_decrypt_failed_us",
        "response_validation_start_us",
        "response_validation_done_us",
        "response_validation_failed_us",
        "callback_attempt_us",
        "provider_coordination_received_us",
        "provider_coordination_decrypt_start_us",
        "provider_coordination_decrypt_done_us",
        "provider_coordination_decrypt_failed_us",
        "provider_coordination_no_pending_us",
        "provider_execute_start_us",
        "provider_execute_done_us",
        "provider_response_publish_attempt_us",
        "provider_response_dispatched_us",
        "provider_response_published_us",
        "provider_response_publish_failed_us",
        "callback_fired_us",
        "callback_skipped_no_pending_us",
        "callback_skipped_timeout_us",
        "timeout_us",
        "timeout_stage",
        "final_classification",
        "lat_request_created_to_ack_matched_us",
        "lat_ack_matched_to_provider_selected_us",
        "lat_provider_selected_to_coordination_published_us",
        "lat_coordination_published_to_provider_coordination_received_us",
        "lat_provider_coordination_received_to_response_published_us",
        "lat_response_published_to_response_observed_us",
        "lat_response_observed_to_callback_fired_us",
        "ack_processed",
    ]
    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for request_id in all_request_ids:
            writer.writerow({field: row_for(request_id).get(field, "")
                             for field in fieldnames})

    json_path = output_dir / "post_ack_pipeline_summary.json"
    summary = dict(counters)
    summary["request_count"] = len(all_request_ids)
    summary["failed_request_count_classified"] = failed_count
    summary["failure_buckets"] = failure_buckets
    json_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    return {
        "summary": summary,
        "csv": str(csv_path),
        "json": str(json_path),
    }


def parse_results(output_dir, app_csv, request_csv, args, readiness=None):
    sent = {}
    timed_out = set()
    selected = {}
    ack_counts_by_request = defaultdict(Counter)
    ack_counts_by_provider = Counter()
    perf_response = {}
    provider_responses = Counter()
    provider_ack_counts = Counter()
    provider_ack_suppression_count = 0
    provider_ack_suppression_reasons = Counter()
    provider_lifecycle_counts = Counter()
    provider_pending_samples = []
    outstanding_limit_skips = 0
    late_response_count = 0
    open_loop_summary = {}

    user_log = output_dir / "user.log"
    if user_log.exists():
        with user_log.open(errors="replace") as f:
            for line in f:
                if "OPEN_LOOP_SUMMARY " in line:
                    open_loop_summary = parse_key_values(line[line.index("OPEN_LOOP_SUMMARY "):])
                if "PERF_REQUEST_SENT " in line:
                    kv = parse_key_values(line[line.index("PERF_REQUEST_SENT "):])
                    sent[kv.get("id", "")] = int(kv.get("ts", "0") or 0)
                if "PERF_ACK_RECEIVED " in line:
                    kv = parse_key_values(line[line.index("PERF_ACK_RECEIVED "):])
                    provider = kv.get("provider", "")
                    request_id = kv.get("id", "")
                    if request_id and provider:
                        ack_counts_by_request[request_id][provider] += 1
                        ack_counts_by_provider[provider] += 1
                if "PERF_PROVIDER_SELECTED " in line:
                    kv = parse_key_values(line[line.index("PERF_PROVIDER_SELECTED "):])
                    selected[kv.get("id", "")] = kv.get("provider", "")
                if "PERF_RESPONSE_RECEIVED " in line:
                    kv = parse_key_values(line[line.index("PERF_RESPONSE_RECEIVED "):])
                    perf_response[kv.get("id", "")] = kv
                if "PERF_REQUEST_TIMEOUT " in line:
                    kv = parse_key_values(line[line.index("PERF_REQUEST_TIMEOUT "):])
                    if kv.get("id", ""):
                        timed_out.add(kv.get("id", ""))
                if "PERF_LATE_RESPONSE " in line:
                    late_response_count += 1
                if "PERF_OUTSTANDING_LIMIT_REACHED " in line:
                    outstanding_limit_skips += 1

    for log_path in output_dir.glob("provider-*.log"):
        provider = log_path.stem.replace("provider-", "")
        with log_path.open(errors="replace") as f:
            for line in f:
                if "publishing HELLO ACK" in line:
                    provider_ack_counts[provider] += 1
                if "publishing final response" in line:
                    provider_responses[provider] += 1
                if "event=ACK_SUPPRESSED " in line:
                    provider_ack_suppression_count += 1
                    kv = parse_key_values(line)
                    provider_ack_suppression_reasons[kv.get("reason", "unknown")] += 1
                    pending = kv.get("pendingRequests", "")
                    if pending:
                        provider_pending_samples.append(parse_int_like(pending, 0))
                if "event=PROVIDER_LIFECYCLE_STATE " in line:
                    kv = parse_key_values(line)
                    state = kv.get("state", "")
                    if state:
                        provider_lifecycle_counts[state] += 1
                    pending = kv.get("pendingAtDecision", "")
                    if pending:
                        provider_pending_samples.append(parse_int_like(pending, 0))

    rows = []
    rows_by_id = {}
    latencies = []
    success_count = 0
    timeout_count = 0
    failed_count = 0
    csv_rows = []
    if app_csv.exists() and app_csv.stat().st_size > 0:
        with app_csv.open(newline="") as f:
            csv_rows = list(csv.DictReader(f))
    for row in csv_rows:
        request_id = row.get("request_id", "")
        success = row.get("success", "0") == "1"
        latency = float(row.get("latency_ms", "0") or 0)
        response_payload = row.get("response_payload", "")
        provider = selected.get(request_id, perf_response.get(request_id, {}).get("provider", ""))
        if provider in ("", "-"):
            match = re.match(r"HELLO_FROM_(.+)", response_payload)
            if match:
                provider = "/example/hello/provider/{}".format(match.group(1))
        if success:
            success_count += 1
            latencies.append(latency)
        else:
            timeout_count += 1
            failed_count += 1
        parsed_row = {
            "request_id": request_id,
            "success": int(success),
            "latency_ms": "{:.3f}".format(latency),
            "response_payload": response_payload,
            "selected_provider": provider,
            "ack_count": sum(ack_counts_by_request.get(request_id, {}).values()),
            "sent_ts": sent.get(request_id, ""),
        }
        rows.append(parsed_row)
        rows_by_id[request_id] = parsed_row

    for request_id, kv in perf_response.items():
        if request_id in rows_by_id:
            continue
        latency = float(kv.get("latency_ms", "0") or 0)
        success_count += 1
        latencies.append(latency)
        provider = selected.get(request_id, kv.get("provider", ""))
        parsed_row = {
            "request_id": request_id,
            "success": 1,
            "latency_ms": "{:.3f}".format(latency),
            "response_payload": "",
            "selected_provider": provider if provider != "-" else selected.get(request_id, ""),
            "ack_count": sum(ack_counts_by_request.get(request_id, {}).values()),
            "sent_ts": sent.get(request_id, ""),
        }
        rows.append(parsed_row)
        rows_by_id[request_id] = parsed_row

    # Rate-series runs stop the user at a wall-clock sampling deadline. Preserve
    # sent-but-unfinished requests so aggregate success rates are based on work
    # actually issued, not only on rows the app had time to flush.
    for request_id, sent_ts in sent.items():
        if request_id in rows_by_id:
            continue
        failed_count += 1
        if request_id in timed_out:
            timeout_count += 1
        parsed_row = {
            "request_id": request_id,
            "success": 0,
            "latency_ms": "0.000",
            "response_payload": "",
            "selected_provider": selected.get(request_id, ""),
            "ack_count": sum(ack_counts_by_request.get(request_id, {}).values()),
            "sent_ts": sent_ts,
        }
        rows.append(parsed_row)
        rows_by_id[request_id] = parsed_row

    with request_csv.open("w", newline="") as f:
        fieldnames = ["request_id", "success", "latency_ms", "response_payload",
                      "selected_provider", "ack_count", "sent_ts"]
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    total = len(rows)
    timestamp_metrics = request_timestamp_metrics(sent.values(), success_count, args.duration)
    sent_count = parse_int_like(open_loop_summary.get("sent"), total)
    timed_out_count = parse_int_like(open_loop_summary.get("timeout"), timeout_count)
    completed_count = success_count + timed_out_count
    pending_at_shutdown = parse_int_like(open_loop_summary.get("remaining_outstanding"), 0)
    outstanding_limit_skip_count = parse_int_like(
        open_loop_summary.get("outstanding_limit_skips"), outstanding_limit_skips)
    median_outstanding = parse_float_like(open_loop_summary.get("median_outstanding"), 0.0)
    p95_outstanding = parse_float_like(open_loop_summary.get("p95_outstanding"), 0.0)
    max_outstanding_observed = parse_int_like(
        open_loop_summary.get("max_outstanding_observed"), 0)
    late_response_count = parse_int_like(open_loop_summary.get("late_response"),
                                         late_response_count)
    user_delayed_publications = parse_int_like(
        open_loop_summary.get("user_delayed_publications"), 0)
    user_queued_task_p50 = parse_float_like(open_loop_summary.get("queued_p50"), 0.0)
    user_queued_task_p95 = parse_float_like(open_loop_summary.get("queued_p95"), 0.0)
    user_queued_task_max = parse_int_like(open_loop_summary.get("queued_max"), 0)
    adaptive_window_p50 = parse_float_like(open_loop_summary.get("adaptive_window_p50"), 0.0)
    adaptive_window_p95 = parse_float_like(open_loop_summary.get("adaptive_window_p95"), 0.0)
    adaptive_window_min = parse_int_like(open_loop_summary.get("adaptive_window_min"), 0)
    adaptive_window_max = parse_int_like(open_loop_summary.get("adaptive_window_max"), 0)
    hard_inflight_limit = parse_int_like(open_loop_summary.get("hard_inflight_limit"), 0)
    admission_control_warnings = parse_int_like(
        open_loop_summary.get("admission_control_warnings"), 0)
    admission_control_rejects = parse_int_like(
        open_loop_summary.get("admission_control_rejects"), 0)
    admission_queue_pause_events = parse_int_like(
        open_loop_summary.get("admission_queue_pause_events"), 0)
    admission_queue_pause_resumes = parse_int_like(
        open_loop_summary.get("admission_queue_pause_resumes"), 0)
    admission_queue_pause_skips = parse_int_like(
        open_loop_summary.get("admission_queue_pause_skips"), 0)
    total_admission_queue_paused_ms = parse_int_like(
        open_loop_summary.get("total_admission_queue_paused_ms"), 0)
    admission_warning_resume_queue_depth = parse_int_like(
        open_loop_summary.get("admission_warning_resume_queue_depth"), 0)
    admission_reject_resume_queue_depth = parse_int_like(
        open_loop_summary.get("admission_reject_resume_queue_depth"), 0)
    admission_recommended_rate_skips = parse_int_like(
        open_loop_summary.get("admission_recommended_rate_skips"), 0)
    admission_recommended_rate_min = parse_float_like(
        open_loop_summary.get("admission_recommended_rate_min"), 0.0)
    admission_recommended_rate_max = parse_float_like(
        open_loop_summary.get("admission_recommended_rate_max"), 0.0)
    admission_recommended_rate_final = parse_float_like(
        open_loop_summary.get("admission_recommended_rate_final"), 0.0)
    pause_count = parse_int_like(open_loop_summary.get("pause_count"), 0)
    total_paused_ms = parse_int_like(open_loop_summary.get("total_paused_ms"), 0)
    paused_state_transitions = parse_int_like(
        open_loop_summary.get("paused_state_transitions"), 0)
    pause_reason_counters = open_loop_summary.get("pause_reason_counters", "")
    in_flight_p50 = parse_float_like(
        open_loop_summary.get("in_flight_p50"), median_outstanding)
    in_flight_p95 = parse_float_like(
        open_loop_summary.get("in_flight_p95"), p95_outstanding)
    in_flight_max = parse_int_like(
        open_loop_summary.get("in_flight_max"), max_outstanding_observed)
    ack_p95_ms = parse_float_like(open_loop_summary.get("ack_p95_ms"), 0.0)
    provider_pending_p50 = percentile(provider_pending_samples, 50)
    provider_pending_p95 = percentile(provider_pending_samples, 95)
    provider_pending_max = max(provider_pending_samples) if provider_pending_samples else 0
    offered = offered_rate_rps(args)
    achieved = timestamp_metrics["actual_requests_per_second"]
    if achieved == 0.0 and open_loop_summary:
        achieved = parse_float_like(open_loop_summary.get("achieved_rps"), 0.0)
        timestamp_metrics["actual_requests_per_second"] = achieved
    completion_throughput = timestamp_metrics["actual_successful_responses_per_second"]
    if completion_throughput == 0.0 and open_loop_summary:
        configured_duration = float(args.duration or 0)
        summary_success = parse_int_like(open_loop_summary.get("success"), 0)
        completion_throughput = (
            float(summary_success) / configured_duration) if configured_duration > 0.0 else 0.0
        timestamp_metrics["actual_successful_responses_per_second"] = completion_throughput
    selected_distribution = dict(Counter(
        row["selected_provider"] for row in rows if row["selected_provider"]))
    provider_skew = selected_provider_skew(selected_distribution)
    post_ack_pipeline = collect_post_ack_pipeline(output_dir, sent.keys())
    summary = {
        "workload_mode": args.workload_mode,
        "workload_notes": workload_notes(
            args.workload_mode, getattr(args, "adaptive_admission_control", False)),
        "strategy": args.strategy,
        "providers": len(selected_provider_nodes(args)),
        "user_node": args.user_node,
        "controller_node": args.controller_node,
        "provider_nodes": selected_provider_nodes(args),
        "duration_s": args.duration,
        "sampling_duration_s": timestamp_metrics["sampling_duration_s"],
        "interval_ms": args.interval_ms,
        "app_interval_ms": int(round(float(args.interval_ms))),
        "rate_rps": getattr(args, "rate_rps", None),
        "offered_rate_rps": offered,
        "actual_requests_per_second": timestamp_metrics["actual_requests_per_second"],
        "actual_successful_responses_per_second": (
            timestamp_metrics["actual_successful_responses_per_second"]),
        "achieved_offered_ratio": (achieved / offered) if offered > 0.0 else 0.0,
        "completion_throughput_rps": completion_throughput,
        "request_timestamp_span_s": timestamp_metrics["request_timestamp_span_s"],
        "perf_request_sent_count": timestamp_metrics["perf_request_sent_count"],
        "first_request_sent_ts": timestamp_metrics["first_request_sent_ts"],
        "last_request_sent_ts": timestamp_metrics["last_request_sent_ts"],
        "total_requests_sent": total,
        "total_successful_responses": success_count,
        "sent_count": sent_count,
        "completed_count": completed_count,
        "timed_out_count": timed_out_count,
        "pending_at_shutdown": pending_at_shutdown,
        "outstanding_limit_skip_count": outstanding_limit_skip_count,
        "median_outstanding_requests": median_outstanding,
        "p95_outstanding_requests": p95_outstanding,
        "max_outstanding_requests": max_outstanding_observed,
        "user_queued_task_p50": user_queued_task_p50,
        "user_queued_task_p95": user_queued_task_p95,
        "user_queued_task_max": user_queued_task_max,
        "adaptive_window_p50": adaptive_window_p50,
        "adaptive_window_p95": adaptive_window_p95,
        "adaptive_window_min": adaptive_window_min,
        "adaptive_window_max": adaptive_window_max,
        "hard_inflight_limit": hard_inflight_limit,
        "admission_control_warnings": admission_control_warnings,
        "admission_control_rejects": admission_control_rejects,
        "admission_queue_pause_events": admission_queue_pause_events,
        "admission_queue_pause_resumes": admission_queue_pause_resumes,
        "admission_queue_pause_skips": admission_queue_pause_skips,
        "total_admission_queue_paused_ms": total_admission_queue_paused_ms,
        "admission_warning_resume_queue_depth": admission_warning_resume_queue_depth,
        "admission_reject_resume_queue_depth": admission_reject_resume_queue_depth,
        "admission_recommended_rate_skips": admission_recommended_rate_skips,
        "admission_recommended_rate_min": admission_recommended_rate_min,
        "admission_recommended_rate_max": admission_recommended_rate_max,
        "admission_recommended_rate_final": admission_recommended_rate_final,
        "pause_count": pause_count,
        "total_paused_ms": total_paused_ms,
        "paused_state_transitions": paused_state_transitions,
        "pause_reason_counters": pause_reason_counters,
        "in_flight_p50": in_flight_p50,
        "in_flight_p95": in_flight_p95,
        "in_flight_max": in_flight_max,
        "ack_p95_ms": ack_p95_ms,
        "user_delayed_publications": user_delayed_publications,
        "provider_ack_suppression_count": provider_ack_suppression_count,
        "provider_ack_suppression_reasons": dict(provider_ack_suppression_reasons),
        "provider_lifecycle_counts": dict(provider_lifecycle_counts),
        "provider_request_observed_count": provider_lifecycle_counts.get("REQUEST_OBSERVED", 0),
        "provider_ack_admission_checked_count": provider_lifecycle_counts.get("ACK_ADMISSION_CHECKED", 0),
        "provider_ack_published_count": provider_lifecycle_counts.get("ACK_PUBLISHED", 0),
        "provider_ack_suppressed_count": provider_lifecycle_counts.get("ACK_SUPPRESSED_OVERLOAD", provider_ack_suppression_count),
        "provider_pending_p50": provider_pending_p50,
        "provider_pending_p95": provider_pending_p95,
        "provider_pending_max": provider_pending_max,
        "provider_coordination_no_pending_count": post_ack_pipeline.get("summary", {}).get("coordination_no_pending", 0),
        "provider_response_published_count": provider_lifecycle_counts.get("RESPONSE_PUBLISHED", 0),
        "late_response_count": late_response_count,
        "max_outstanding": getattr(args, "max_outstanding", None),
        "request_timeout_ms": getattr(args, "request_timeout_ms", None),
        "drain_seconds": getattr(args, "drain_seconds", None),
        "app_user_exit_status": getattr(args, "app_user_exit_status", None),
        "app_user_terminated_after_duration": bool(
            getattr(args, "app_user_terminated_after_duration", False)),
        "provider_readiness_status": provider_readiness_status(readiness or []),
        "provider_certificates": getattr(args, "provider_cert_records", []),
        "provider_certificate_runtime": cert_server_state(
            output_dir, getattr(args, "provider_cert_records", [])),
        "provider_ready_wait_seconds": provider_ready_wait_seconds(args),
        "startup_timestamps_us": collect_startup_timestamps(output_dir),
        "success_rate": (success_count / total) if total else 0.0,
        "average_latency_ms": statistics.mean(latencies) if latencies else 0.0,
        "p50_latency_ms": percentile(latencies, 50),
        "p95_latency_ms": percentile(latencies, 95),
        "p99_latency_ms": percentile(latencies, 99),
        "timeout_count": timeout_count,
        "failed_request_count": failed_count,
        "outstanding_limit_skips": outstanding_limit_skips,
        "selected_provider_distribution": selected_distribution,
        "provider_selection_skew": provider_skew,
        "ack_count_per_provider": dict(ack_counts_by_provider or provider_ack_counts),
        "provider_final_response_count": dict(provider_responses),
        "post_ack_pipeline": post_ack_pipeline,
        "crypto_diagnostics": collect_crypto_diagnostics(output_dir),
        "diagnostic_only": bool(getattr(args, "crypto_diagnostics", False) or
                                getattr(args, "diag_plaintext_ack", False) or
                                getattr(args, "diag_plaintext_response", False)),
        "diagnostic_plaintext_ack": bool(getattr(args, "diag_plaintext_ack", False)),
        "diagnostic_plaintext_response": bool(getattr(args, "diag_plaintext_response", False)),
        "output_dir": str(output_dir),
        "requests_csv": str(request_csv),
    }
    summary["achieved_rate_warning"] = achieved_rate_warning(summary)
    summary["failure_breakdown"] = {
        "request_generation_skipped": outstanding_limit_skip_count,
        "request_timeout": timed_out_count,
        "response_received_too_late": late_response_count,
        "provider_selection_concentrated_on_C": bool(provider_skew.get("concentrated_on_C") and
                                                     provider_skew.get("skewed")),
        "ack_coordination_bottleneck": (
            timed_out_count > 0 and outstanding_limit_skip_count == 0 and
            sum(int(v) for v in summary["ack_count_per_provider"].values()) > 0 and
            sum(int(v) for v in selected_distribution.values()) < sent_count * 0.50),
    }
    summary["bottleneck_classification"] = classify_bottlenecks(summary)
    return summary


def collect_svs_smoke_diagnostics(output_dir, app_csv, user_exit_status):
    logs = {}
    markers = Counter()
    for log_path in sorted(output_dir.glob("*.log")):
        text = log_path.read_text(errors="replace")
        logs[log_path.name] = {
            "bytes": log_path.stat().st_size,
            "perf_request_sent": len(re.findall(r"^PERF_REQUEST_SENT ", text, re.M)),
            "perf_ack_received": len(re.findall(r"^PERF_ACK_RECEIVED ", text, re.M)),
            "perf_provider_selected": len(re.findall(r"^PERF_PROVIDER_SELECTED ", text, re.M)),
            "perf_response_received": len(re.findall(r"^PERF_RESPONSE_RECEIVED ", text, re.M)),
            "perf_request_timeout": len(re.findall(r"^PERF_REQUEST_TIMEOUT ", text, re.M)),
            "svs_lines": len(re.findall(r"SVS|ndnsvs|sync", text, re.I)),
            "ack_lines": len(re.findall(r"ACK|ack", text)),
            "error_lines": len(re.findall(r"error|exception|failed", text, re.I)),
        }
        for marker in logs[log_path.name]:
            if marker != "bytes":
                markers[marker] += logs[log_path.name][marker]

    csv_rows = []
    if app_csv.exists() and app_csv.stat().st_size > 0:
        with app_csv.open(newline="") as f:
            csv_rows = list(csv.DictReader(f))

    success_rows = [row for row in csv_rows if row.get("success", "0") == "1"]
    summary = {
        "app_user_exit_status": user_exit_status,
        "app_csv": str(app_csv),
        "csv_rows": len(csv_rows),
        "successful_rows": len(success_rows),
        "success": user_exit_status == 0 and len(success_rows) > 0,
        "markers": dict(markers),
        "logs": logs,
    }
    (output_dir / "svs-smoke-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n")

    lines = [
        "NDNSF New API MiniNDN SVS Smoke Summary",
        "=======================================",
        "App_User exit status: {}".format(user_exit_status),
        "CSV rows: {}".format(len(csv_rows)),
        "successful rows: {}".format(len(success_rows)),
        "overall success: {}".format(summary["success"]),
        "markers: {}".format(dict(markers)),
        "app csv: {}".format(app_csv),
    ]
    (output_dir / "svs-smoke-summary.txt").write_text("\n".join(lines) + "\n")
    return summary


def analyze_app_user_exit(output_dir, user_log_path, exit_status, label):
    patterns = {
        "timeout_lines": re.compile(r"(timeout|PERF_REQUEST_TIMEOUT)", re.I),
        "exception_lines": re.compile(r"(exception|error:|App_User error)", re.I),
        "validation_lines": re.compile(r"(validation|validate|validator|reject)", re.I),
        "identity_lines": re.compile(r"(identity|certificate|keychain|pib|tpm)", re.I),
        "route_lines": re.compile(r"(NoRoute|no route|Nack|missing route)", re.I),
        "svs_init_lines": re.compile(r"(SVS|ndnsvs|sync)", re.I),
    }
    reasons = {key: [] for key in patterns}
    if user_log_path.exists():
        with user_log_path.open(errors="replace") as f:
            for line in f:
                stripped = line.rstrip("\n")
                for key, pattern in patterns.items():
                    if len(reasons[key]) < 40 and pattern.search(stripped):
                        reasons[key].append(stripped)
    trimmed = reasons
    report = {
        "label": label,
        "exit_status": exit_status,
        "log_path": str(user_log_path),
        "inference": infer_app_user_exit_reason(exit_status, reasons),
        "evidence": trimmed,
    }
    path = output_dir / "{}-app-user-exit-analysis.json".format(label)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    lines = [
        "App_User Exit Analysis ({})".format(label),
        "=" * 36,
        "exit_status={}".format(exit_status),
        "inference={}".format(report["inference"]),
        "log={}".format(user_log_path),
    ]
    for key, values in trimmed.items():
        lines.append("{}={}".format(key, len(values)))
        for value in values[:8]:
            lines.append("  {}".format(value))
    (output_dir / "{}-app-user-exit-analysis.txt".format(label)).write_text(
        "\n".join(lines) + "\n")
    return report


def infer_app_user_exit_reason(exit_status, reasons):
    if exit_status == 0:
        return "normal_success"
    if reasons["exception_lines"]:
        return "exception_or_reported_error"
    if reasons["validation_lines"]:
        return "possible_validation_rejection"
    if reasons["identity_lines"]:
        return "possible_identity_or_keychain_issue"
    if reasons["route_lines"]:
        return "possible_route_or_forwarding_issue"
    if reasons["timeout_lines"]:
        return "request_timeout_without_reported_exception"
    if reasons["svs_init_lines"]:
        return "svs_activity_present_but_no_success_marker"
    return "unknown_nonzero_exit"


def svs_only_source():
    return r'''
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/scheduler.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
ndn::security::Certificate
getOrCreateIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  }
}
} // namespace

int
main(int argc, char** argv)
{
  try {
    const std::string mode = argc > 1 ? argv[1] : "";
    const ndn::Name syncPrefix("/example/hello/group");
    const ndn::Name testName("/SVS_TEST_MESSAGE");
    const ndn::Name identity(mode == "provider" ? "/example/hello/provider/A" : "/example/hello/user");
    const ndn::Name nodePrefix(mode == "provider" ? "/example/hello/provider/A/svs-only" : "/example/hello/user/svs-only");

    ndn::Face face;
    ndn::KeyChain keyChain;
    auto cert = getOrCreateIdentity(keyChain, identity);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(identity));

    ndn::svs::SecurityOptions secOpts(keyChain);
    secOpts.interestSigner->signingInfo.setSigningCertName(cert.getName());
    secOpts.dataSigner->signingInfo.setSigningCertName(cert.getName());
    secOpts.pubSigner->signingInfo.setSigningCertName(cert.getName());
    ndn::svs::SVSPubSubOptions opts;
    opts.useTimestamp = false;

    bool received = false;
    std::cout << "SVS_ONLY mode=" << mode
              << " group=" << syncPrefix
              << " syncPrefix=" << syncPrefix
              << " nodePrefix=" << nodePrefix
              << " dataPrefix=" << ndn::Name(nodePrefix).append(syncPrefix)
              << " signingIdentity=" << identity
              << std::endl;

    ndn::svs::SVSPubSub pubsub(
      syncPrefix,
      nodePrefix,
      face,
      [](const std::vector<ndn::svs::MissingDataInfo>& info) {
        std::cout << "SVS_ONLY missingData count=" << info.size() << std::endl;
      },
      opts,
      secOpts);

    if (mode == "user") {
      pubsub.subscribe(testName, [&received](const ndn::svs::SVSPubSub::SubscriptionData& sub) {
        std::string payload(reinterpret_cast<const char*>(sub.data.data()), sub.data.size());
        std::cout << "SVS_ONLY_RECEIVED name=" << sub.name
                  << " producer=" << sub.producerPrefix
                  << " seq=" << sub.seqNo
                  << " payload=" << payload << std::endl;
        received = payload == "SVS_TEST_MESSAGE";
      });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
      while (!received && std::chrono::steady_clock::now() < deadline) {
        face.processEvents(ndn::time::milliseconds(100));
      }
      return received ? 0 : 3;
    }

    if (mode == "provider") {
      for (int i = 0; i < 30; ++i) {
        face.processEvents(ndn::time::milliseconds(100));
      }
      const std::string payload = "SVS_TEST_MESSAGE";
      auto seq = pubsub.publish(testName, ndn::make_span(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
      std::cout << "SVS_ONLY_PUBLISHED name=" << testName
                << " seq=" << seq
                << " payload=" << payload << std::endl;
      for (int i = 0; i < 100; ++i) {
        face.processEvents(ndn::time::milliseconds(100));
      }
      return 0;
    }

    std::cerr << "Usage: svs-only-smoke user|provider" << std::endl;
    return 2;
  }
  catch (const std::exception& e) {
    std::cerr << "SVS_ONLY_ERROR " << e.what() << std::endl;
    return 1;
  }
}
'''


def build_svs_only_probe(output_dir):
    source = output_dir / "svs_only_smoke.cpp"
    binary = output_dir / "svs_only_smoke"
    source.write_text(svs_only_source())
    pkg_commands = [
        "pkg-config --cflags --libs libndn-svs libndn-cxx",
        "pkg-config --cflags --libs ndn-svs ndn-cxx",
    ]
    errors = []
    for pkg in pkg_commands:
        cmd = "g++ -std=c++17 -O0 -g {} -o {} {} 2>&1".format(
            shell_quote(source), shell_quote(binary), "$({})".format(pkg))
        proc = subprocess.run(cmd, cwd=str(REPO_ROOT), shell=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True)
        if proc.returncode == 0:
            return binary, proc.stdout
        errors.append({"command": cmd, "output": proc.stdout})
    (output_dir / "svs-only-build-errors.json").write_text(
        json.dumps(errors, indent=2, sort_keys=True) + "\n")
    return None, "\n".join(item["output"] for item in errors)


def run_svs_only_smoke(ndn, args, output_dir):
    log("Running minimal ndn-svs-only smoke diagnostic")
    binary, build_output = build_svs_only_probe(output_dir)
    (output_dir / "svs-only-build.log").write_text(build_output or "")
    if binary is None:
        raise RuntimeError("Could not build svs-only smoke probe; see svs-only-build-errors.json")

    session_base = int(time.time()) + os.getpid()
    probe_args = argparse.Namespace(**vars(args))
    processes = []
    user_log_path = output_dir / "svs-only-user.log"
    provider_log_path = output_dir / "svs-only-provider.log"
    user_log = user_log_path.open("wb")
    provider_log = provider_log_path.open("wb")
    user_proc = getPopen(ndn.net[args.user_node],
                         "exec {} user".format(shell_quote(binary)),
                         envDict=app_env(output_dir, session_base, probe_args),
                         shell=True,
                         stdout=user_log,
                         stderr=subprocess.STDOUT)
    processes.append((user_proc, user_log))
    dump_face_counters(ndn, output_dir, "svs-only-after-user-start")
    time.sleep(max(0, args.svs_settle_seconds))
    provider_proc = getPopen(ndn.net[selected_provider_nodes(args)[0]],
                             "exec {} provider".format(shell_quote(binary)),
                             envDict=app_env(output_dir, session_base, probe_args),
                             shell=True,
                             stdout=provider_log,
                             stderr=subprocess.STDOUT)
    processes.append((provider_proc, provider_log))
    dump_face_counters(ndn, output_dir, "svs-only-after-provider-start")
    try:
        deadline = time.time() + 45
        while user_proc.poll() is None and time.time() < deadline:
            time.sleep(0.5)
        if user_proc.poll() is None:
            raise RuntimeError("svs-only user exceeded timeout; see {}".format(user_log_path))
        if provider_proc.poll() is None:
            provider_proc.terminate()
        collect_nfd_packet_logs(ndn, output_dir, "svs-only-final")
        summary = {
            "user_exit_status": user_proc.returncode,
            "provider_exit_status": provider_proc.poll(),
            "success": user_proc.returncode == 0,
            "user_log": str(user_log_path),
            "provider_log": str(provider_log_path),
        }
        (output_dir / "svs-only-summary.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n")
        if not summary["success"]:
            raise RuntimeError("svs-only smoke failed; see {}".format(user_log_path))
        log("svs-only smoke passed")
    finally:
        stop_processes(processes)


def run_svs_smoke(ndn, args, output_dir):
    log("Running isolated NDNSF/SVS smoke diagnostic")
    smoke_args = argparse.Namespace(**vars(args))
    smoke_args.duration = max(1, min(args.duration, 10))
    smoke_args.warmup = 0
    smoke_args.interval_ms = 1
    smoke_args.max_requests = 1
    smoke_args.timeout_ms = max(args.timeout_ms, 5000)
    smoke_args.ack_timeout_ms = max(args.ack_timeout_ms, 1000)

    initialize_example_keychains(ndn, smoke_args, output_dir)
    processes, user_proc, user_log, app_csv, session_base = launch_apps_providers_first_for_svs(
        ndn, smoke_args, output_dir)
    try:
        dump_face_counters(ndn, output_dir, "svs-smoke-after-user-start")
        collect_nfd_packet_logs(ndn, output_dir, "svs-smoke-after-user-start")

        deadline = time.time() + 90
        while user_proc.poll() is None and time.time() < deadline:
            time.sleep(0.5)
        if user_proc.poll() is None:
            raise RuntimeError("SVS smoke App_User exceeded 90 seconds; see {}".format(user_log))
        summary = collect_svs_smoke_diagnostics(output_dir, app_csv, user_proc.returncode)
        analyze_app_user_exit(output_dir, user_log, user_proc.returncode, "svs-smoke")
        dump_face_counters(ndn, output_dir, "svs-smoke-after-user-exit")
        collect_nfd_packet_logs(ndn, output_dir, "svs-smoke-after-user-exit")
        compare_face_counter_snapshots(output_dir, [
            "pre-workload",
            "svs-smoke-after-provider-ready",
            "svs-smoke-after-user-start",
            "svs-smoke-after-user-exit",
        ])
        log("SVS smoke success={}".format(summary["success"]))
        log("svs-smoke-summary.txt={}".format(output_dir / "svs-smoke-summary.txt"))
        if not summary["success"]:
            raise RuntimeError("SVS smoke did not receive a successful response; see {}".format(user_log))
    finally:
        stop_processes(processes)


def write_summary(output_dir, summary):
    summary_json = output_dir / "summary.json"
    summary_txt = output_dir / "summary.txt"
    summary_json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    lines = [
        "NDNSF New API MiniNDN Performance Summary",
        "=========================================",
        "workload mode: {}".format(summary["workload_mode"]),
        "workload notes:",
    ]
    lines.extend("  - {}".format(note) for note in summary["workload_notes"])
    lines.extend([
        "strategy: {}".format(summary["strategy"]),
        "App_User exit status: {}".format(summary["app_user_exit_status"]),
        "App_User stopped at duration: {}".format(summary["app_user_terminated_after_duration"]),
        "providers: {}".format(summary["providers"]),
        "provider readiness: {}".format(summary["provider_readiness_status"]),
        "provider-ready wait seconds: {}".format(summary.get("provider_ready_wait_seconds")),
        "provider certificates: {}".format([
            item.get("cert_name") for item in summary.get("provider_certificates", [])]),
        "provider cert runtime: {}".format({
            item.get("provider_id"): {
                "aa_cert_interest_seen": item.get("aa_cert_interest_seen"),
                "cert_data_served": item.get("cert_data_served"),
            }
            for item in summary.get("provider_certificate_runtime", [])
        }),
        "offered rate rps: {:.3f}".format(summary["offered_rate_rps"]),
        "interval ms: {:.3f}".format(float(summary["interval_ms"])),
        "sampling duration s: {:.3f}".format(summary["sampling_duration_s"]),
        "actual requests per second: {:.3f}".format(summary["actual_requests_per_second"]),
        "achieved/offered ratio: {:.2%}".format(summary["achieved_offered_ratio"]),
        "actual successful responses per second: {:.3f}".format(
            summary["actual_successful_responses_per_second"]),
        "sent count: {}".format(summary["sent_count"]),
        "completed count: {}".format(summary["completed_count"]),
        "successful responses: {}".format(summary["total_successful_responses"]),
        "success rate: {:.2%}".format(summary["success_rate"]),
        "avg latency ms: {:.3f}".format(summary["average_latency_ms"]),
        "p50 latency ms: {:.3f}".format(summary["p50_latency_ms"]),
        "p95 latency ms: {:.3f}".format(summary["p95_latency_ms"]),
        "p99 latency ms: {:.3f}".format(summary["p99_latency_ms"]),
        "timed out count: {}".format(summary["timed_out_count"]),
        "pending at shutdown: {}".format(summary["pending_at_shutdown"]),
        "median outstanding requests: {:.3f}".format(summary["median_outstanding_requests"]),
        "p95 outstanding requests: {:.3f}".format(summary["p95_outstanding_requests"]),
        "max outstanding requests: {}".format(summary["max_outstanding_requests"]),
        "user queued task p50/p95/max: {:.3f}/{:.3f}/{}".format(
            summary.get("user_queued_task_p50", 0.0),
            summary.get("user_queued_task_p95", 0.0),
            summary.get("user_queued_task_max", 0)),
        "adaptive window p50/p95/min/max: {:.3f}/{:.3f}/{}/{}".format(
            summary.get("adaptive_window_p50", 0.0),
            summary.get("adaptive_window_p95", 0.0),
            summary.get("adaptive_window_min", 0),
            summary.get("adaptive_window_max", 0)),
        "hard inflight limit: {}".format(summary.get("hard_inflight_limit", 0)),
        "pause count / transitions / paused ms: {} / {} / {}".format(
            summary.get("pause_count", 0),
            summary.get("paused_state_transitions", 0),
            summary.get("total_paused_ms", 0)),
        "pause reasons: {}".format(summary.get("pause_reason_counters", "")),
        "in-flight p50/p95/max: {:.3f}/{:.3f}/{}".format(
            summary.get("in_flight_p50", 0.0),
            summary.get("in_flight_p95", 0.0),
            summary.get("in_flight_max", 0)),
        "ACK p95 latency ms: {:.3f}".format(summary.get("ack_p95_ms", 0.0)),
        "user delayed publications: {}".format(summary.get("user_delayed_publications", 0)),
        "provider ACK suppressions: {}".format(
            summary.get("provider_ack_suppression_count", 0)),
        "failed requests: {}".format(summary["failed_request_count"]),
        "outstanding-limit skips: {}".format(summary["outstanding_limit_skip_count"]),
        "late responses: {}".format(summary["late_response_count"]),
        "bottleneck classification: {}".format(summary["bottleneck_classification"]),
        "failure breakdown: {}".format(summary["failure_breakdown"]),
        "selected providers: {}".format(summary["selected_provider_distribution"]),
        "provider selection skew: {}".format(summary["provider_selection_skew"]),
        "ACKs per provider: {}".format(summary["ack_count_per_provider"]),
        "final responses per provider: {}".format(summary["provider_final_response_count"]),
        "post-ACK pipeline counters: {}".format(
            summary.get("post_ack_pipeline", {}).get("summary", {})),
        "post-ACK pipeline csv: {}".format(
            summary.get("post_ack_pipeline", {}).get("csv", "")),
        "startup timestamps us: {}".format(summary.get("startup_timestamps_us", {})),
        "requests csv: {}".format(summary["requests_csv"]),
    ])
    if summary.get("achieved_rate_warning"):
        lines.append(summary["achieved_rate_warning"])
    summary_txt.write_text("\n".join(lines) + "\n")
    return summary_txt, summary_json


def print_summary(summary):
    log("")
    log("NDNSF New API MiniNDN Performance Summary")
    log("-----------------------------------------")
    log("{:<32} {}".format("workload mode", summary["workload_mode"]))
    log("{:<32} {}".format("strategy", summary["strategy"]))
    log("{:<32} {}".format("App_User exit status", summary["app_user_exit_status"]))
    log("{:<32} {}".format("App_User stopped at duration",
                            summary["app_user_terminated_after_duration"]))
    log("{:<32} {}".format("providers", summary["providers"]))
    log("{:<32} {}".format("provider readiness", summary["provider_readiness_status"]))
    log("{:<32} {}".format("provider-ready wait s",
                            summary.get("provider_ready_wait_seconds")))
    log("{:<32} {}".format("provider certs", [
        item.get("cert_name") for item in summary.get("provider_certificates", [])]))
    log("{:<32} {:.3f}".format("offered rate rps", summary["offered_rate_rps"]))
    log("{:<32} {:.3f}".format("interval ms", float(summary["interval_ms"])))
    log("{:<32} {:.3f}".format("sampling duration s", summary["sampling_duration_s"]))
    log("{:<32} {:.3f}".format("actual requests/s", summary["actual_requests_per_second"]))
    log("{:<32} {:.2%}".format("achieved/offered ratio", summary["achieved_offered_ratio"]))
    log("{:<32} {:.3f}".format(
        "actual success responses/s", summary["actual_successful_responses_per_second"]))
    log("{:<32} {}".format("sent count", summary["sent_count"]))
    log("{:<32} {}".format("completed count", summary["completed_count"]))
    log("{:<32} {}".format("successful responses", summary["total_successful_responses"]))
    log("{:<32} {:.2%}".format("success rate", summary["success_rate"]))
    log("{:<32} {:.3f}".format("average latency ms", summary["average_latency_ms"]))
    log("{:<32} {:.3f}".format("p50 latency ms", summary["p50_latency_ms"]))
    log("{:<32} {:.3f}".format("p95 latency ms", summary["p95_latency_ms"]))
    log("{:<32} {:.3f}".format("p99 latency ms", summary["p99_latency_ms"]))
    log("{:<32} {}".format("timed out count", summary["timed_out_count"]))
    log("{:<32} {}".format("pending at shutdown", summary["pending_at_shutdown"]))
    log("{:<32} {:.3f}".format("median outstanding", summary["median_outstanding_requests"]))
    log("{:<32} {:.3f}".format("p95 outstanding", summary["p95_outstanding_requests"]))
    log("{:<32} {}".format("max outstanding", summary["max_outstanding_requests"]))
    log("{:<32} {}".format("failed request count", summary["failed_request_count"]))
    log("{:<32} {}".format("outstanding-limit skips", summary["outstanding_limit_skip_count"]))
    log("{:<32} {}".format("late responses", summary["late_response_count"]))
    log("{:<32} {}".format("bottleneck", summary["bottleneck_classification"]["primary"]))
    log("{:<32} {}".format("failure breakdown", summary["failure_breakdown"]))
    log("{:<32} {}".format("selected providers", summary["selected_provider_distribution"]))
    log("{:<32} {}".format("provider skew", summary["provider_selection_skew"]))
    log("{:<32} {}".format("ACKs per provider", summary["ack_count_per_provider"]))
    if summary.get("achieved_rate_warning"):
        log(summary["achieved_rate_warning"])
    log("{:<32} {}".format("output dir", summary["output_dir"]))


def output_directory(args):
    if args.output_dir:
        return Path(args.output_dir).resolve()
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    if parse_rate_series(args.rate_series):
        return REPO_ROOT / "results" / "newapi_testbed_rate_series_{}".format(timestamp)
    return REPO_ROOT / "results" / "newapi_minindn_perf_{}".format(timestamp)


def dry_run(args):
    normalize_placement_args(args)
    ensure_runtime(args)
    out_dir = output_directory(args)
    rates = parse_rate_series(args.rate_series)
    topology = topology_file(args)
    available_nodes = parse_topology_file_nodes(topology) if topology else [
        "controller", "user", "router"] + provider_nodes(args.providers)
    log("DRY RUN: NDNSF New API MiniNDN performance experiment")
    log("repo={}".format(REPO_ROOT))
    log("output_dir={}".format(out_dir))
    log("topology={}".format(topology or "generated Topo(controller,user,router,providers)"))
    log("topology_file_loaded={}".format(bool(topology)))
    log("available_nodes={}".format(available_nodes))
    log("node_placement={}".format({
        "user": args.user_node,
        "controller": args.controller_node,
        "providers": selected_provider_nodes(args),
    }))
    log("providers={}".format(args.providers))
    log("workload_mode={}".format(args.workload_mode))
    if args.workload_mode == "open-loop":
        log("rate_rps={}".format(args.rate_rps))
        log("max_outstanding={}".format(args.max_outstanding))
        log("request_timeout_ms={}".format(args.request_timeout_ms))
        log("drain_seconds={}".format(args.drain_seconds))
    for note in workload_notes(
            args.workload_mode, getattr(args, "adaptive_admission_control", False)):
        log("workload_note={}".format(note))
    log("strategy={}".format(args.strategy))
    if rates:
        plan = []
        for rate in rates:
            interval_ms = interval_for_rate(rate)
            plan.append({
                "offered_rate_rps": rate,
                "interval_ms": interval_ms,
                "app_interval_ms": app_interval_ms_for_rate(rate),
                "duration_s": args.per_rate_duration,
                "planned_request_capacity": int(round(rate * args.per_rate_duration)),
                "result_dir": "rate_{}_rps".format(rate_label(rate)),
            })
        log("rate_series={}".format(plan))
        log("planned_measurement_seconds={}".format(len(rates) * args.per_rate_duration))
        log("max_total_runtime_seconds={}".format(args.max_total_runtime_seconds))
    else:
        log("measured_requests={}".format(measured_count(args)))
        log("warmup_requests={}".format(warmup_count(args)))
    log("apps={}".format({name: str(path) for name, path in APP_TARGETS.items()}))
    log("connectivity_check={}".format(args.connectivity_check))
    log("dump_nfd_state={}".format(args.dump_nfd_state))
    log("svs_smoke={}".format(args.svs_smoke))
    log("svs_settle_seconds={}".format(args.svs_settle_seconds))
    log("controller_settle_seconds={}".format(args.controller_settle_seconds))
    log("provider_start_gap_seconds={}".format(args.provider_start_gap_seconds))
    log("post_ready_settle_seconds={}".format(args.post_ready_settle_seconds))
    log("provider_ready_wait={}".format(args.provider_ready_wait))
    log("effective_provider_ready_wait_seconds={}".format(provider_ready_wait_seconds(args)))
    log("provider_ready_timeout_seconds={}".format(args.provider_ready_timeout_seconds))
    log("startup_settle_seconds={}".format(args.startup_settle_seconds))
    log("nlsr_converge_seconds={}".format(args.nlsr_converge_seconds))
    log("startup_order=MiniNDN -> NFD/NLSR -> prefix advertisement -> NLSR convergence wait -> controller -> controller settle -> sequential providers -> provider readiness logs -> post-ready settle -> user")
    log("svs_only_smoke={}".format(args.svs_only_smoke))
    log("nfd_log_level={}".format(args.nfd_log_level))
    log("debug_ack={}".format(args.debug_ack))
    log("disable_tokens={}".format(args.disable_tokens))
    log("hybrid_message_crypto={}".format(not args.disable_hybrid_message_crypto))
    log("dk_forwarding_check={}".format(args.dk_forwarding_check))
    log("dk_diagnostic_node={}".format(args.dk_diagnostic_node))
    log("dk_bootstrap_check={}".format(args.dk_bootstrap_check))
    log("dk_bootstrap_only={}".format(args.dk_bootstrap_only))
    log("framework_example_cert_serving=enabled_by_default")
    log("legacy_harness_cert_server_fallback={}".format(args.serve_provider_certs))


def launch_apps(ndn, args, output_dir):
    processes = []
    session_base = int(time.time()) + os.getpid()

    try:
        if not getattr(args, "controller_already_running", False):
            start_controller(ndn, output_dir, session_base, processes, args)
            settle_s = max(0, args.controller_settle_seconds)
            if settle_s:
                log("Controller ready; settling {}s before starting providers".format(settle_s))
                time.sleep(settle_s)
        readiness = start_providers(ndn, args, output_dir, session_base, processes, "workload")
        barrier = write_startup_barrier_report(ndn, args, output_dir, "pre-user", readiness)
        settle_s = provider_ready_wait_seconds(args)
        if settle_s:
            log("All providers ready; provider-ready wait {}s before starting App_User".format(
                settle_s))
            time.sleep(settle_s)
        barrier["user_start_release_timestamp_us"] = now_us()
        (output_dir / "pre-user-startup-barrier.json").write_text(
            json.dumps(barrier, indent=2, sort_keys=True) + "\n")

        app_csv = output_dir / "app_user_requests.csv"
        user_proc, user_log = start_process(
            ndn.net[args.user_node], "user", APP_TARGETS["user"], app_user_argv(args, app_csv),
            output_dir, session_base, processes, args)
        return processes, user_proc, user_log, app_csv, readiness
    except Exception:
        stop_processes(processes)
        raise


def benchmark_timeout_seconds(args, remaining_budget_s=None):
    provider_start_budget_s = (
        len(selected_provider_nodes(args)) *
        (args.provider_ready_timeout_seconds + args.provider_start_gap_seconds))
    startup_budget_s = (
        args.controller_settle_seconds + provider_ready_wait_seconds(args) +
        args.startup_settle_seconds + provider_start_budget_s)
    if args.workload_mode == "open-loop":
        timeout_s = int(args.duration + startup_budget_s +
                        max(args.request_timeout_ms / 1000.0, 1) +
                        max(args.ack_timeout_ms / 1000.0, 0) +
                        max(args.drain_seconds, 0) + 20)
    elif getattr(args, "rate_rps", None):
        timeout_s = int(args.duration + startup_budget_s +
                        max(args.timeout_ms / 1000.0, 1) + 20)
    else:
        timeout_s = min(285, max(90, int((measured_count(args) + warmup_count(args)) *
                                         (float(args.interval_ms) + 1000) / 1000) + 45))
    if remaining_budget_s is not None:
        timeout_s = min(timeout_s, max(1, int(remaining_budget_s)))
    return timeout_s


def run_workload_once(ndn, args, output_dir, label="workload", remaining_budget_s=None):
    processes = []
    try:
        processes, user_proc, user_log, app_csv, readiness = launch_apps(ndn, args, output_dir)
        write_provider_cert_runtime_diagnostics(output_dir, args, "{}-after-provider-ready".format(label))
        dump_face_counters(ndn, output_dir, "{}-after-app-startup".format(label))
        collect_nfd_packet_logs(ndn, output_dir, "{}-after-app-startup".format(label))
        timeout_s = benchmark_timeout_seconds(args, remaining_budget_s)
        rate_mode = getattr(args, "rate_rps", None) is not None
        if rate_mode:
            if args.workload_mode == "open-loop":
                log("Running open-loop rate subtest; offered_rate={}rps duration={}s max_outstanding={} request_timeout_ms={} drain_seconds={}".format(
                    offered_rate_rps(args), args.duration, args.max_outstanding,
                    args.request_timeout_ms, args.drain_seconds))
                deadline = time.time() + timeout_s
            else:
                log("Sampling closed-loop rate subtest for {}s; offered_rate={}rps interval={:.3f}ms planned_capacity={}".format(
                    args.duration, offered_rate_rps(args), float(args.interval_ms),
                    measured_count(args)))
                deadline = time.time() + min(timeout_s, args.duration)
        else:
            log("Waiting for benchmark to finish; timeout={}s measured_requests={} warmup_requests={}".format(
                timeout_s, measured_count(args), warmup_count(args)))
            deadline = time.time() + timeout_s
        while user_proc.poll() is None and time.time() < deadline:
            time.sleep(0.5)
        if user_proc.poll() is None:
            if rate_mode and args.workload_mode != "open-loop":
                log("Rate sampling window ended; stopping App_User for {}".format(label))
                user_proc.terminate()
                stop_deadline = time.time() + 5
                while user_proc.poll() is None and time.time() < stop_deadline:
                    time.sleep(0.1)
                if user_proc.poll() is None:
                    user_proc.kill()
                args.app_user_terminated_after_duration = True
            else:
                raise RuntimeError("App_User benchmark exceeded {} seconds; see {}".format(
                    timeout_s, user_log))
        if not app_csv.exists() and not user_log.exists():
            raise RuntimeError("Benchmark CSV was not generated: {}".format(app_csv))
        args.app_user_exit_status = user_proc.returncode
        analyze_app_user_exit(output_dir, user_log, user_proc.returncode, label)

        request_csv = output_dir / "requests.csv"
        summary = parse_results(output_dir, app_csv, request_csv, args, readiness)
        summary_txt, summary_json = write_summary(output_dir, summary)
        print_summary(summary)
        log("summary.txt={}".format(summary_txt))
        log("summary.json={}".format(summary_json))
        log("requests.csv={}".format(request_csv))
        if not getattr(args, "skip_post_run_diagnostics", False):
            dump_face_counters(ndn, output_dir, "{}-after-user-exit".format(label))
            collect_nfd_packet_logs(ndn, output_dir, "{}-after-user-exit".format(label))
            write_provider_cert_runtime_diagnostics(output_dir, args, "{}-after-user-exit".format(label))
            compare_face_counter_snapshots(output_dir, [
                "pre-workload",
                "{}-after-app-startup".format(label),
                "{}-after-user-exit".format(label),
            ])
        return summary
    finally:
        stop_processes(processes)


def make_rate_args(args, rate):
    rate_args = argparse.Namespace(**vars(args))
    rate_args.rate_rps = float(rate)
    rate_args.duration = args.per_rate_duration
    rate_args.warmup = 0
    rate_args.interval_ms = interval_for_rate(rate)
    if args.workload_mode == "open-loop":
        rate_args.timeout_ms = args.request_timeout_ms
    return rate_args


def effective_rate_duration(args, rate_count, elapsed_s):
    remaining = args.max_total_runtime_seconds - elapsed_s
    estimated_per_rate_overhead_s = max(
        5,
        provider_ready_wait_seconds(args) +
        len(selected_provider_nodes(args)) * args.provider_start_gap_seconds + 13)
    budgeted = int((remaining - rate_count * estimated_per_rate_overhead_s) / rate_count)
    if budgeted <= 0:
        raise RuntimeError(
            "Insufficient runtime budget for {} rate subtests after setup: remaining={:.1f}s, "
            "estimated overhead={}s/rate".format(
                rate_count, remaining, estimated_per_rate_overhead_s))
    return min(args.per_rate_duration, budgeted)


def aggregate_csv_row(summary):
    post_ack = summary.get("post_ack_pipeline", {}).get("summary", {})
    return {
        "rate_rps": summary.get("rate_rps", ""),
        "workload_mode": summary.get("workload_mode", ""),
        "offered_rate_rps": "{:.3f}".format(float(summary.get("offered_rate_rps", 0.0))),
        "interval_ms": "{:.3f}".format(float(summary.get("interval_ms", 0.0))),
        "sampling_duration_s": "{:.3f}".format(
            float(summary.get("sampling_duration_s", 0.0))),
        "actual_requests_per_second": "{:.6f}".format(
            float(summary.get("actual_requests_per_second", 0.0))),
        "achieved_offered_ratio": "{:.6f}".format(
            float(summary.get("achieved_offered_ratio", 0.0))),
        "actual_successful_responses_per_second": "{:.6f}".format(
            float(summary.get("actual_successful_responses_per_second", 0.0))),
        "completion_throughput_rps": "{:.6f}".format(
            float(summary.get("completion_throughput_rps", 0.0))),
        "duration_s": summary.get("duration_s", ""),
        "provider_ready_wait_seconds": summary.get("provider_ready_wait_seconds", ""),
        "total_requests_sent": summary.get("total_requests_sent", 0),
        "sent_count": summary.get("sent_count", 0),
        "completed_count": summary.get("completed_count", 0),
        "total_successful_responses": summary.get("total_successful_responses", 0),
        "success_rate": "{:.6f}".format(float(summary.get("success_rate", 0.0))),
        "avg_latency_ms": "{:.3f}".format(float(summary.get("average_latency_ms", 0.0))),
        "p50_latency_ms": "{:.3f}".format(float(summary.get("p50_latency_ms", 0.0))),
        "p95_latency_ms": "{:.3f}".format(float(summary.get("p95_latency_ms", 0.0))),
        "p99_latency_ms": "{:.3f}".format(float(summary.get("p99_latency_ms", 0.0))),
        "timeout_count": summary.get("timeout_count", 0),
        "timed_out_count": summary.get("timed_out_count", 0),
        "pending_at_shutdown": summary.get("pending_at_shutdown", 0),
        "failed_request_count": summary.get("failed_request_count", 0),
        "outstanding_limit_skips": summary.get("outstanding_limit_skips", 0),
        "outstanding_limit_skip_count": summary.get("outstanding_limit_skip_count", 0),
        "median_outstanding_requests": "{:.3f}".format(
            float(summary.get("median_outstanding_requests", 0.0))),
        "p95_outstanding_requests": "{:.3f}".format(
            float(summary.get("p95_outstanding_requests", 0.0))),
        "max_outstanding_requests": summary.get("max_outstanding_requests", 0),
        "user_queued_task_p50": "{:.3f}".format(
            float(summary.get("user_queued_task_p50", 0.0))),
        "user_queued_task_p95": "{:.3f}".format(
            float(summary.get("user_queued_task_p95", 0.0))),
        "user_queued_task_max": summary.get("user_queued_task_max", 0),
        "adaptive_window_p50": "{:.3f}".format(
            float(summary.get("adaptive_window_p50", 0.0))),
        "adaptive_window_p95": "{:.3f}".format(
            float(summary.get("adaptive_window_p95", 0.0))),
        "adaptive_window_min": summary.get("adaptive_window_min", 0),
        "adaptive_window_max": summary.get("adaptive_window_max", 0),
        "hard_inflight_limit": summary.get("hard_inflight_limit", 0),
        "admission_control_warnings": summary.get("admission_control_warnings", 0),
        "admission_control_rejects": summary.get("admission_control_rejects", 0),
        "admission_queue_pause_events": summary.get("admission_queue_pause_events", 0),
        "admission_queue_pause_resumes": summary.get("admission_queue_pause_resumes", 0),
        "admission_queue_pause_skips": summary.get("admission_queue_pause_skips", 0),
        "total_admission_queue_paused_ms": summary.get(
            "total_admission_queue_paused_ms", 0),
        "admission_warning_resume_queue_depth": summary.get(
            "admission_warning_resume_queue_depth", 0),
        "admission_reject_resume_queue_depth": summary.get(
            "admission_reject_resume_queue_depth", 0),
        "admission_recommended_rate_skips": summary.get(
            "admission_recommended_rate_skips", 0),
        "admission_recommended_rate_min": "{:.3f}".format(
            float(summary.get("admission_recommended_rate_min", 0.0))),
        "admission_recommended_rate_max": "{:.3f}".format(
            float(summary.get("admission_recommended_rate_max", 0.0))),
        "admission_recommended_rate_final": "{:.3f}".format(
            float(summary.get("admission_recommended_rate_final", 0.0))),
        "pause_count": summary.get("pause_count", 0),
        "total_paused_ms": summary.get("total_paused_ms", 0),
        "paused_state_transitions": summary.get("paused_state_transitions", 0),
        "pause_reason_counters": summary.get("pause_reason_counters", ""),
        "in_flight_p50": "{:.3f}".format(float(summary.get("in_flight_p50", 0.0))),
        "in_flight_p95": "{:.3f}".format(float(summary.get("in_flight_p95", 0.0))),
        "in_flight_max": summary.get("in_flight_max", 0),
        "ack_p95_ms": "{:.3f}".format(float(summary.get("ack_p95_ms", 0.0))),
        "user_delayed_publications": summary.get("user_delayed_publications", 0),
        "provider_ack_suppression_count": summary.get("provider_ack_suppression_count", 0),
        "provider_ack_suppression_reasons": json.dumps(
            summary.get("provider_ack_suppression_reasons", {}), sort_keys=True),
        "provider_request_observed_count": summary.get("provider_request_observed_count", 0),
        "provider_ack_admission_checked_count": summary.get("provider_ack_admission_checked_count", 0),
        "provider_ack_published_count": summary.get("provider_ack_published_count", 0),
        "provider_ack_suppressed_count": summary.get("provider_ack_suppressed_count", 0),
        "provider_pending_p50": "{:.3f}".format(float(summary.get("provider_pending_p50", 0.0))),
        "provider_pending_p95": "{:.3f}".format(float(summary.get("provider_pending_p95", 0.0))),
        "provider_pending_max": summary.get("provider_pending_max", 0),
        "provider_coordination_no_pending_count": summary.get("provider_coordination_no_pending_count", 0),
        "provider_response_published_count": summary.get("provider_response_published_count", 0),
        "late_response_count": summary.get("late_response_count", 0),
        "bottleneck_primary": summary.get("bottleneck_classification", {}).get("primary", ""),
        "failure_breakdown": json.dumps(summary.get("failure_breakdown", {}),
                                        sort_keys=True),
        "achieved_rate_warning": summary.get("achieved_rate_warning", ""),
        "ack_count_per_provider": json.dumps(summary.get("ack_count_per_provider", {}),
                                             sort_keys=True),
        "selected_provider_distribution": json.dumps(
            summary.get("selected_provider_distribution", {}), sort_keys=True),
        "post_ack_pending_created": post_ack.get("pending_created", 0),
        "post_ack_pending_completed": post_ack.get("request_pending_completed", 0),
        "post_ack_pending_timeout": post_ack.get("request_pending_timeout", 0),
        "post_ack_pending_erased": post_ack.get("request_pending_erased", 0),
        "post_ack_match_attempt": post_ack.get("ack_match_attempt", 0),
        "post_ack_match_attempt_pre_decrypt": post_ack.get(
            "ack_match_attempt_pre_decrypt", 0),
        "post_ack_match_attempt_decoded": post_ack.get("ack_match_attempt_decoded", 0),
        "post_ack_matched_pending_call": post_ack.get("ack_matched_pending_call", 0),
        "post_ack_match_failed_no_pending": post_ack.get(
            "ack_match_failed_no_pending", 0),
        "post_ack_match_failed_expired_call": post_ack.get(
            "ack_match_failed_expired_call", 0),
        "post_ack_match_failed_request_id_mismatch": post_ack.get(
            "ack_match_failed_request_id_mismatch", 0),
        "post_ack_match_skipped_pre_decrypt": post_ack.get(
            "ack_match_skipped_pre_decrypt", 0),
        "post_ack_bucket_1": post_ack.get(
            "bucket_1_request_never_created_pending_call", 0),
        "post_ack_bucket_2": post_ack.get(
            "bucket_2_pending_call_created_but_erased_before_ack", 0),
        "post_ack_bucket_3": post_ack.get(
            "bucket_3_ack_received_but_skipped_pre_decrypt", 0),
        "post_ack_bucket_4": post_ack.get(
            "bucket_4_ack_decoded_but_no_pending_call_matched", 0),
        "post_ack_bucket_5": post_ack.get(
            "bucket_5_ack_matched_pending_call_but_no_provider_selected", 0),
        "post_ack_bucket_6": post_ack.get(
            "bucket_6_provider_selected_but_coordination_not_scheduled", 0),
        "post_ack_bucket_7": post_ack.get(
            "bucket_7_coordination_scheduled_but_not_published", 0),
        "post_ack_bucket_8": post_ack.get(
            "bucket_8_coordination_published_but_provider_did_not_execute_respond", 0),
        "post_ack_bucket_9": post_ack.get(
            "bucket_9_response_published_but_user_callback_not_triggered", 0),
        "post_ack_bucket_10": post_ack.get(
            "bucket_10_real_network_svs_nac_loss_or_timeout", 0),
        "post_ack_selection_invoked": post_ack.get("selection_invoked", 0),
        "post_ack_selection_completed": post_ack.get("selection_completed", 0),
        "post_ack_coordination_attempted": post_ack.get("coordination_attempted", 0),
        "post_ack_coordination_scheduled": post_ack.get("coordination_scheduled", 0),
        "post_ack_coordination_published": post_ack.get("coordination_published", 0),
        "post_ack_coordination_no_pending": post_ack.get("coordination_no_pending", 0),
        "post_ack_coordination_publish_failed": post_ack.get("coordination_publish_failed", 0),
        "post_ack_coordination_skipped": post_ack.get("coordination_skipped", 0),
        "post_ack_coordination_rejected": post_ack.get("coordination_rejected", 0),
        "post_ack_requests_with_no_selection": post_ack.get("requests_with_no_selection", 0),
        "post_ack_requests_timed_out_before_selection": post_ack.get(
            "requests_timed_out_before_selection", 0),
        "post_ack_requests_timed_out_after_selection_before_coordination": post_ack.get(
            "requests_timed_out_after_selection_before_coordination", 0),
        "post_ack_callback_skipped_no_pending": post_ack.get(
            "callback_skipped_no_pending", 0),
        "post_ack_callback_skipped_timeout": post_ack.get("callback_skipped_timeout", 0),
        "post_ack_pipeline_csv": summary.get("post_ack_pipeline", {}).get("csv", ""),
    }


def write_aggregate_results(output_dir, summaries, started_at, finished_at, args):
    warnings = [summary.get("achieved_rate_warning", "") for summary in summaries
                if summary.get("achieved_rate_warning", "")]
    aggregate = {
        "topology_file": topology_file(args) or "",
        "topology_file_loaded": bool(topology_file(args)),
        "node_placement": {
            "user": args.user_node,
            "controller": args.controller_node,
            "providers": selected_provider_nodes(args),
        },
        "workload_mode": args.workload_mode,
        "workload_notes": workload_notes(
            args.workload_mode, getattr(args, "adaptive_admission_control", False)),
        "strategy": args.strategy,
        "max_total_runtime_seconds": args.max_total_runtime_seconds,
        "wall_clock_runtime_seconds": finished_at - started_at,
        "stayed_under_3_minutes": (finished_at - started_at) <= args.max_total_runtime_seconds,
        "warnings": warnings,
        "rates": summaries,
    }
    (output_dir / "aggregate-summary.json").write_text(
        json.dumps(aggregate, indent=2, sort_keys=True) + "\n")

    lines = [
        "NDNSF New API Testbed Rate Series Summary",
        "=========================================",
        "topology file loaded: {}".format(aggregate["topology_file_loaded"]),
        "topology file: {}".format(aggregate["topology_file"] or "generated"),
        "node placement: {}".format(aggregate["node_placement"]),
        "workload mode: {}".format(args.workload_mode),
        "workload notes:",
    ]
    lines.extend("  - {}".format(note) for note in workload_notes(
        args.workload_mode, getattr(args, "adaptive_admission_control", False)))
    lines.extend([
        "strategy: {}".format(args.strategy),
        "wall clock runtime seconds: {:.3f}".format(aggregate["wall_clock_runtime_seconds"]),
        "stayed under 3 minutes: {}".format(aggregate["stayed_under_3_minutes"]),
    ])
    if warnings:
        lines.append("warnings:")
        lines.extend("  - {}".format(warning) for warning in warnings)
    for summary in summaries:
        lines.extend([
            "",
            "[offered {} rps]".format(summary.get("offered_rate_rps")),
            "result dir: {}".format(summary.get("output_dir")),
            "interval ms: {:.3f}".format(float(summary.get("interval_ms", 0.0))),
            "sampling duration s: {:.3f}".format(summary.get("sampling_duration_s", 0.0)),
            "actual requests per second: {:.3f}".format(
                summary.get("actual_requests_per_second", 0.0)),
            "achieved/offered ratio: {:.2%}".format(
                summary.get("achieved_offered_ratio", 0.0)),
            "actual successful responses per second: {:.3f}".format(
                summary.get("actual_successful_responses_per_second", 0.0)),
            "sent count: {}".format(summary.get("sent_count")),
            "completed count: {}".format(summary.get("completed_count")),
            "successful responses: {}".format(summary.get("total_successful_responses")),
            "success rate: {:.2%}".format(summary.get("success_rate", 0.0)),
            "avg latency ms: {:.3f}".format(summary.get("average_latency_ms", 0.0)),
            "p50 latency ms: {:.3f}".format(summary.get("p50_latency_ms", 0.0)),
            "p95 latency ms: {:.3f}".format(summary.get("p95_latency_ms", 0.0)),
            "p99 latency ms: {:.3f}".format(summary.get("p99_latency_ms", 0.0)),
            "timed out count: {}".format(summary.get("timed_out_count")),
            "pending at shutdown: {}".format(summary.get("pending_at_shutdown")),
            "median outstanding requests: {:.3f}".format(
                summary.get("median_outstanding_requests", 0.0)),
            "p95 outstanding requests: {:.3f}".format(
                summary.get("p95_outstanding_requests", 0.0)),
            "max outstanding requests: {}".format(summary.get("max_outstanding_requests")),
            "in-flight p50/p95/max: {:.3f}/{:.3f}/{}".format(
                summary.get("in_flight_p50", 0.0),
                summary.get("in_flight_p95", 0.0),
                summary.get("in_flight_max", 0)),
            "ACK p95 latency ms: {:.3f}".format(summary.get("ack_p95_ms", 0.0)),
            "hard inflight limit: {}".format(summary.get("hard_inflight_limit", 0)),
            "pause count / transitions / paused ms: {} / {} / {}".format(
                summary.get("pause_count", 0),
                summary.get("paused_state_transitions", 0),
                summary.get("total_paused_ms", 0)),
            "pause reasons: {}".format(summary.get("pause_reason_counters", "")),
            "failed requests: {}".format(summary.get("failed_request_count")),
            "outstanding-limit skips: {}".format(summary.get("outstanding_limit_skip_count")),
            "late responses: {}".format(summary.get("late_response_count")),
            "bottleneck classification: {}".format(summary.get("bottleneck_classification")),
            "failure breakdown: {}".format(summary.get("failure_breakdown")),
            "ACKs per provider: {}".format(summary.get("ack_count_per_provider")),
            "selected providers: {}".format(summary.get("selected_provider_distribution")),
        ])
        if summary.get("achieved_rate_warning"):
            lines.append(summary["achieved_rate_warning"])
    (output_dir / "aggregate-summary.txt").write_text("\n".join(lines) + "\n")

    fieldnames = [
        "rate_rps", "workload_mode", "offered_rate_rps", "interval_ms",
        "sampling_duration_s", "actual_requests_per_second",
        "achieved_offered_ratio", "actual_successful_responses_per_second",
        "completion_throughput_rps", "duration_s",
        "provider_ready_wait_seconds",
        "total_requests_sent", "sent_count", "completed_count",
        "total_successful_responses", "success_rate",
        "avg_latency_ms", "p50_latency_ms", "p95_latency_ms", "p99_latency_ms", "timeout_count",
        "timed_out_count", "pending_at_shutdown", "failed_request_count",
        "outstanding_limit_skips", "outstanding_limit_skip_count",
        "median_outstanding_requests", "p95_outstanding_requests", "max_outstanding_requests",
        "user_queued_task_p50", "user_queued_task_p95", "user_queued_task_max",
        "adaptive_window_p50", "adaptive_window_p95",
        "adaptive_window_min", "adaptive_window_max",
        "hard_inflight_limit",
        "admission_control_warnings", "admission_control_rejects",
        "admission_queue_pause_events", "admission_queue_pause_resumes",
        "admission_queue_pause_skips", "total_admission_queue_paused_ms",
        "admission_warning_resume_queue_depth",
        "admission_reject_resume_queue_depth",
        "admission_recommended_rate_skips",
        "admission_recommended_rate_min",
        "admission_recommended_rate_max",
        "admission_recommended_rate_final",
        "pause_count", "total_paused_ms",
        "paused_state_transitions", "pause_reason_counters",
        "in_flight_p50", "in_flight_p95", "in_flight_max", "ack_p95_ms",
        "user_delayed_publications", "provider_ack_suppression_count",
        "provider_ack_suppression_reasons",
        "provider_request_observed_count",
        "provider_ack_admission_checked_count",
        "provider_ack_published_count",
        "provider_ack_suppressed_count",
        "provider_pending_p50",
        "provider_pending_p95",
        "provider_pending_max",
        "provider_coordination_no_pending_count",
        "provider_response_published_count",
        "late_response_count", "bottleneck_primary", "failure_breakdown",
        "achieved_rate_warning",
        "ack_count_per_provider",
        "selected_provider_distribution",
        "post_ack_pending_created", "post_ack_pending_completed",
        "post_ack_pending_timeout", "post_ack_pending_erased",
        "post_ack_match_attempt", "post_ack_match_attempt_pre_decrypt",
        "post_ack_match_attempt_decoded", "post_ack_matched_pending_call",
        "post_ack_match_failed_no_pending",
        "post_ack_match_failed_expired_call",
        "post_ack_match_failed_request_id_mismatch",
        "post_ack_match_skipped_pre_decrypt",
        "post_ack_bucket_1", "post_ack_bucket_2", "post_ack_bucket_3",
        "post_ack_bucket_4", "post_ack_bucket_5", "post_ack_bucket_6",
        "post_ack_bucket_7", "post_ack_bucket_8", "post_ack_bucket_9",
        "post_ack_bucket_10",
        "post_ack_selection_invoked", "post_ack_selection_completed",
        "post_ack_coordination_attempted", "post_ack_coordination_scheduled",
        "post_ack_coordination_published", "post_ack_coordination_no_pending",
        "post_ack_coordination_publish_failed",
        "post_ack_coordination_skipped", "post_ack_coordination_rejected",
        "post_ack_requests_with_no_selection",
        "post_ack_requests_timed_out_before_selection",
        "post_ack_requests_timed_out_after_selection_before_coordination",
        "post_ack_callback_skipped_no_pending", "post_ack_callback_skipped_timeout",
        "post_ack_pipeline_csv",
    ]
    with (output_dir / "aggregate-rates.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for summary in summaries:
            writer.writerow(aggregate_csv_row(summary))
    return aggregate


def run_rate_series(ndn, args, output_dir):
    rates = parse_rate_series(args.rate_series)
    started_at = getattr(args, "experiment_started_at", time.time())
    summaries = []
    controller_processes = []
    controller_session_base = int(time.time()) + os.getpid()
    try:
        start_controller(ndn, output_dir, controller_session_base, controller_processes, args)
        settle_s = max(0, args.controller_settle_seconds)
        if settle_s:
            log("Controller ready; settling {}s before starting rate subtests".format(settle_s))
            time.sleep(settle_s)
        elapsed_after_setup = time.time() - started_at
        series_duration = effective_rate_duration(args, len(rates), elapsed_after_setup)
        if series_duration < args.per_rate_duration:
            log("Reducing per-rate duration from {}s to {}s to stay within {}s total runtime budget".format(
                args.per_rate_duration, series_duration, args.max_total_runtime_seconds))
        for rate in rates:
            elapsed = time.time() - started_at
            remaining = args.max_total_runtime_seconds - elapsed
            needed_floor = series_duration + args.startup_settle_seconds + 5
            if remaining <= needed_floor:
                raise RuntimeError(
                    "Insufficient runtime budget before {} rps subtest: remaining={:.1f}s, needed at least {:.1f}s".format(
                        rate, remaining, needed_floor))
            rate_args = make_rate_args(args, rate)
            rate_args.duration = series_duration
            rate_args.controller_already_running = True
            subdir = output_dir / "rate_{}_rps".format(rate_label(rate))
            subdir.mkdir(parents=True, exist_ok=True)
            dump_face_counters(ndn, subdir, "pre-workload")
            log("Starting rate subtest: rate={} rps interval={:.3f}ms app_interval={}ms duration={}s output={}".format(
                rate, interval_for_rate(rate), app_interval_ms_for_rate(rate),
                rate_args.duration, subdir))
            summary = run_workload_once(ndn, rate_args, subdir,
                                        "rate-{}-rps".format(rate_label(rate)),
                                        remaining_budget_s=remaining)
            summaries.append(summary)
    finally:
        stop_processes(controller_processes)
    finished_at = time.time()
    return write_aggregate_results(output_dir, summaries, started_at, finished_at, args)


def start_controller(ndn, output_dir, session_base, processes, args):
    controller_proc, controller_log = start_process(
        ndn.net[args.controller_node], "controller", APP_TARGETS["controller"], [],
        output_dir, session_base, processes, args)
    if not wait_for_log(controller_log, r"ServiceController listening on:", 20, controller_proc):
        raise RuntimeError("Controller did not become ready; see {}".format(controller_log))
    (output_dir / "controller-startup.json").write_text(json.dumps({
        "event": "controller_ready",
        "timestamp_us": now_us(),
        "log_path": str(controller_log),
        "node": args.controller_node,
    }, indent=2, sort_keys=True) + "\n")
    return controller_proc, controller_log


def provider_argv(args, provider_id, index, output_dir=None):
    rank = index + 1
    queue = (index + 1) * 3
    delay_series = [
        int(float(value)) for value in parse_csv_list(
            getattr(args, "provider_request_delay_ms_series", ""))
    ]
    argv = [
        "--benchmark",
        "--provider-id", provider_id,
        "--ack-status", "true",
        "--ack-message", "Provider {} ready".format(provider_id),
        "--ack-payload", (args.provider_ack_payload
                          if args.provider_ack_payload is not None
                          else "queue={};gpu=idle;rank={}".format(queue, rank)),
        "--response-payload", "HELLO_FROM_{}".format(provider_id),
        "--provider-lifecycle-csv",
        str((output_dir or Path(".")).resolve() / "provider-{}-lifecycle.csv".format(provider_id)),
    ]
    if delay_series:
        delay_ms = delay_series[min(index, len(delay_series) - 1)]
        argv.extend(["--provider-request-delay-ms", str(delay_ms)])
    if getattr(args, "adaptive_provider_ack", False):
        argv.append("--adaptive-provider-ack")
        argv.extend([
            "--provider-ack-max-pending", str(int(args.provider_ack_max_pending)),
            "--provider-ack-max-event-loop-lag-ms",
            str(int(args.provider_ack_max_event_loop_lag_ms)),
            "--provider-ack-max-coordination-lag-ms",
            str(int(args.provider_ack_max_coordination_lag_ms)),
        ])
    if getattr(args, "handler_threads", -1) >= 0:
        argv.extend(["--handler-threads", str(int(args.handler_threads))])
    if getattr(args, "disable_tokens", False):
        argv.append("--disable-tokens")
    if getattr(args, "disable_hybrid_message_crypto", False):
        argv.append("--disable-hybrid-message-crypto")
    elif getattr(args, "hybrid_message_crypto", False):
        argv.append("--hybrid-message-crypto")
    if getattr(args, "timeline_trace", False):
        argv.append("--timeline-trace")
    return argv


def provider_dk_bootstrap_argv(args, provider_id, index, output_dir=None):
    argv = provider_argv(args, provider_id, index, output_dir)
    argv.append("--dk-bootstrap-only")
    return argv


def start_providers(ndn, args, output_dir, session_base, processes, label="provider-startup"):
    readiness = []
    provider_nodes_for_run = selected_provider_nodes(args)
    provider_ids_for_run = selected_provider_ids(args)
    gap_s = max(0, args.provider_start_gap_seconds)
    for index, node_name in enumerate(provider_nodes_for_run):
        provider_id = provider_ids_for_run[index]
        log("Starting provider {} of {}: provider-{} on {}".format(
            index + 1, len(provider_nodes_for_run), provider_id, node_name))
        proc, log_path = start_process(
            ndn.net[node_name], "provider-{}".format(provider_id),
            APP_TARGETS["provider"], provider_argv(args, provider_id, index, output_dir),
            output_dir, session_base, processes, args)
        state = wait_for_provider_ready(
            log_path, provider_id, args.provider_ready_timeout_seconds, proc)
        state["timeout_s"] = args.provider_ready_timeout_seconds
        readiness.append(state)
        write_provider_readiness_summary(
            output_dir, readiness, "{}-partial".format(label),
            all(item["ready"] for item in readiness),
            args.provider_ready_timeout_seconds)
        if not state["ready"]:
            write_provider_readiness_summary(
                output_dir, readiness, label, False,
                args.provider_ready_timeout_seconds)
            fail_provider_readiness(output_dir, readiness, label)
        if gap_s and index + 1 < len(provider_nodes_for_run):
            log("Provider-{} ready; waiting {}s before starting next provider".format(
                provider_id, gap_s))
            time.sleep(gap_s)
    write_provider_readiness_summary(
        output_dir, readiness, label, all(item["ready"] for item in readiness),
        args.provider_ready_timeout_seconds)
    if not all(item["ready"] for item in readiness):
        fail_provider_readiness(output_dir, readiness, label)
    return readiness


def start_single_provider_bootstrap(ndn, args, output_dir, session_base, provider_index,
                                    process_name, processes):
    provider_node_names = selected_provider_nodes(args)
    provider_ids_for_run = selected_provider_ids(args)
    node_name = provider_node_names[provider_index]
    provider_id = provider_ids_for_run[provider_index]
    log("Starting {}: provider-{} on {}".format(process_name, provider_id, node_name))
    proc, log_path = start_process(
        ndn.net[node_name], process_name, APP_TARGETS["provider"],
        provider_argv(args, provider_id, provider_index, output_dir),
        output_dir, session_base, processes, args)
    state = wait_for_provider_ready(
        log_path, provider_id, args.provider_ready_timeout_seconds, proc)
    state["timeout_s"] = args.provider_ready_timeout_seconds
    state["node"] = node_name
    state["process_name"] = process_name
    state["permission_interest"] = provider_permission_interest(provider_id)
    return state


def provider_permission_retrieved(state):
    checks = state.get("checks", {})
    return bool(checks.get("provider_permission_installed"))


def run_dk_bootstrap_check(ndn, args, output_dir):
    processes = []
    controller_processes = []
    session_base = int(time.time()) + os.getpid()
    provider_id = selected_provider_ids(args)[0]
    provider_node = selected_provider_nodes(args)[0]
    controller_node = args.controller_node
    diagnostic_nodes = list(dict.fromkeys([provider_node, controller_node]))
    dkey_parent = provider_dkey_prefix(provider_id)
    report = {
        "provider_id": provider_id,
        "provider_node": provider_node,
        "controller_node": controller_node,
        "authority_prefix": "/example/hello/controller",
        "dkey_prefix_parent": dkey_parent,
        "producer": "ServiceController KpAttributeAuthority",
        "node_homes": node_home_report(ndn, diagnostic_nodes),
        "note": "Provider runs with --dk-bootstrap-only, so it exits after NAC-ABE DKEY readiness and before provider permission fetch.",
    }
    try:
        start_controller(ndn, output_dir, session_base, controller_processes, args)
        settle_s = max(0, args.controller_settle_seconds)
        if settle_s:
            log("Controller/AA ready; settling {}s before DK bootstrap provider".format(
                settle_s))
            time.sleep(settle_s)

        dump_face_counters(ndn, output_dir, "dk-bootstrap-before-provider")
        report["before_dkey_parent_forwarding"] = write_prefix_forwarding_state(
            ndn, output_dir, "dk-bootstrap-before-dkey-parent", diagnostic_nodes, dkey_parent)

        proc, provider_log = start_process(
            ndn.net[provider_node], "dk-bootstrap-provider-{}".format(provider_id),
            APP_TARGETS["provider"], provider_dk_bootstrap_argv(args, provider_id, 0, output_dir),
            output_dir, session_base, processes, args)

        timeout_s = max(args.provider_ready_timeout_seconds, 20)
        deadline = time.time() + timeout_s
        success = False
        while time.time() < deadline:
            if provider_log.exists():
                text = provider_log.read_text(errors="replace")
                if "DK_BOOTSTRAP_ONLY success=1" in text:
                    success = True
                    break
            if proc.poll() is not None:
                break
            time.sleep(0.2)
        if proc.poll() is None:
            proc.terminate()
            stop_deadline = time.time() + 5
            while proc.poll() is None and time.time() < stop_deadline:
                time.sleep(0.1)
            if proc.poll() is None:
                proc.kill()

        dump_face_counters(ndn, output_dir, "dk-bootstrap-after-provider")
        collect_dk_packet_logs(ndn, output_dir, "dk-bootstrap-after-provider", diagnostic_nodes)
        dkey_names = infer_dkey_names(output_dir)
        exact_dkey = choose_exact_dkey_interest(dkey_names)
        report["dkey_names_observed"] = dkey_names
        report["exact_dkey_interest"] = exact_dkey
        report["provider_process_exit_status"] = proc.poll()
        report["dk_bootstrap_success"] = success
        report["provider_log"] = str(provider_log)
        report["provider_log_tail"] = log_tail(provider_log)
        controller_log = output_dir / "controller.log"
        report["controller_log"] = str(controller_log)
        report["controller_log_tail"] = log_tail(controller_log)

        provider_text = provider_log.read_text(errors="replace") if provider_log.exists() else ""
        controller_text = controller_log.read_text(errors="replace") if controller_log.exists() else ""
        nfd_grep_text = "\n".join(
            path.read_text(errors="replace")
            for path in sorted((output_dir / "nfd-dk-grep").glob("*"))
            if path.is_file())
        report["signals"] = {
            "dk_interest_expressed_by_provider_log": "DK_INTEREST_EXPRESSED" in provider_text,
            "waiting_for_decryption_key_present": "Waiting for decryption key" in provider_text,
            "provider_received_dkey_data": "DK_DATA_RECEIVED" in provider_text,
            "dk_decrypt_success_logged": "DK_DECRYPT_SUCCESS" in provider_text or success,
            "provider_permission_interest_expressed": "Fetch provider permissions:" in provider_text,
            "controller_or_aa_received_dkey": (
                "KpAA Got DKEY request" in controller_text or
                "CpAA Got DKEY request" in controller_text or
                "KpAA Got DKEY request" in nfd_grep_text or
                "CpAA Got DKEY request" in nfd_grep_text),
            "dkey_data_seen_in_nfd_logs": "Data" in nfd_grep_text and "DKEY" in nfd_grep_text,
            "dkey_interest_seen_in_nfd_logs": "Interest" in nfd_grep_text and "DKEY" in nfd_grep_text,
        }
        report["provider_cert_servers"] = write_provider_cert_runtime_diagnostics(
            output_dir, args, "dk-bootstrap")
        report["signals"]["aa_fetched_provider_cert"] = any(
            item.get("aa_cert_interest_seen") for item in report["provider_cert_servers"])
        report["signals"]["provider_cert_data_served"] = any(
            item.get("cert_data_served") for item in report["provider_cert_servers"])
        report["signals"]["dkey_data_returned"] = (
            report["signals"]["provider_received_dkey_data"] or
            report["signals"]["dk_decrypt_success_logged"] or
            report["signals"]["dkey_data_seen_in_nfd_logs"])

        if exact_dkey:
            report["exact_dkey_forwarding"] = write_prefix_forwarding_state(
                ndn, output_dir, "dk-bootstrap-exact-dkey", diagnostic_nodes, exact_dkey)
        else:
            report["exact_dkey_forwarding"] = {}

        compare_face_counter_snapshots(output_dir, [
            "dk-bootstrap-before-provider",
            "dk-bootstrap-after-provider",
        ])

        (output_dir / "dk-bootstrap-check.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n")
        lines = [
            "NAC-ABE DK Bootstrap Check",
            "==========================",
            "provider_node={}".format(provider_node),
            "controller_node={}".format(controller_node),
            "authority_prefix={}".format(report["authority_prefix"]),
            "dkey_prefix_parent={}".format(dkey_parent),
            "provider_cert_names={}".format([
                item["cert_name"] for item in report["provider_cert_servers"]]),
            "aa_fetched_provider_cert={}".format(
                report["signals"]["aa_fetched_provider_cert"]),
            "exact_dkey_interest={}".format(exact_dkey or ""),
            "dkey_data_returned={}".format(report["signals"]["dkey_data_returned"]),
            "provider_imported_decrypted_dkey={}".format(
                report["signals"]["dk_decrypt_success_logged"]),
            "dk_bootstrap_success={}".format(success),
            "provider_permission_interest_expressed={}".format(
                report["signals"]["provider_permission_interest_expressed"]),
            "signals={}".format(report["signals"]),
        ]
        (output_dir / "dk-bootstrap-check.txt").write_text("\n".join(lines) + "\n")
        log("dk-bootstrap-check.txt={}".format(output_dir / "dk-bootstrap-check.txt"))
        return report
    finally:
        stop_processes(processes)
        stop_processes(controller_processes)


def run_dk_forwarding_check(ndn, args, output_dir):
    processes = []
    controller_processes = []
    session_base = int(time.time()) + os.getpid()
    provider_id = selected_provider_ids(args)[0]
    report = {
        "provider_id": provider_id,
        "provider_node": selected_provider_nodes(args)[0],
        "diagnostic_node": args.dk_diagnostic_node,
        "exact_permission_interest": provider_permission_interest(provider_id),
        "note": "Experiment-harness diagnostic only; no NDNSF protocol changes.",
    }
    try:
        start_controller(ndn, output_dir, session_base, controller_processes, args)
        settle_s = max(0, args.controller_settle_seconds)
        if settle_s:
            log("Controller ready; settling {}s before baseline provider bootstrap".format(
                settle_s))
            time.sleep(settle_s)

        report["before_state"] = write_dk_forwarding_state(
            ndn, output_dir, "dk-before-multicast", args, provider_id)
        baseline_processes = []
        try:
            baseline = start_single_provider_bootstrap(
                ndn, args, output_dir, session_base, 0,
                "dk-baseline-provider-{}".format(provider_id), baseline_processes)
            report["baseline_bootstrap"] = baseline
        finally:
            stop_processes(baseline_processes)

        report["multicast_prefixes_set"] = set_dk_multicast_strategies(ndn, args, provider_id)
        time.sleep(1)
        report["after_strategy_state"] = write_dk_forwarding_state(
            ndn, output_dir, "dk-after-multicast", args, provider_id)

        multicast_processes = []
        try:
            rerun = start_single_provider_bootstrap(
                ndn, args, output_dir, session_base, 0,
                "dk-multicast-provider-{}".format(provider_id), multicast_processes)
            report["multicast_bootstrap"] = rerun
        finally:
            stop_processes(multicast_processes)

        baseline_ok = provider_permission_retrieved(report["baseline_bootstrap"])
        multicast_ok = provider_permission_retrieved(report["multicast_bootstrap"])
        report["multicast_changed_dk_retrieval"] = baseline_ok != multicast_ok
        report["summary"] = {
            "baseline_permission_retrieved": baseline_ok,
            "multicast_permission_retrieved": multicast_ok,
            "baseline_ready": bool(report["baseline_bootstrap"].get("ready")),
            "multicast_ready": bool(report["multicast_bootstrap"].get("ready")),
        }
        (output_dir / "dk-forwarding-check.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n")
        lines = [
            "DK/Permission Forwarding Check",
            "==============================",
            "provider_id={}".format(provider_id),
            "provider_node={}".format(report["provider_node"]),
            "diagnostic_node={}".format(args.dk_diagnostic_node),
            "exact_permission_interest={}".format(report["exact_permission_interest"]),
            "baseline_permission_retrieved={}".format(baseline_ok),
            "multicast_permission_retrieved={}".format(multicast_ok),
            "multicast_changed_dk_retrieval={}".format(
                report["multicast_changed_dk_retrieval"]),
            "before_longest_strategy={}".format(
                report["before_state"]["longest_matching_strategy"]),
            "after_longest_strategy={}".format(
                report["after_strategy_state"]["longest_matching_strategy"]),
        ]
        (output_dir / "dk-forwarding-check.txt").write_text("\n".join(lines) + "\n")
        log("dk-forwarding-check.txt={}".format(output_dir / "dk-forwarding-check.txt"))
        return report
    finally:
        stop_processes(processes)
        stop_processes(controller_processes)


def app_user_argv(args, app_csv):
    app_interval_ms = int(round(float(args.interval_ms)))
    user_args = [
        "--benchmark",
        "--workload-mode", args.workload_mode,
        "--count", str(measured_count(args)),
        "--warmup", str(warmup_count(args)),
        "--interval-ms", str(max(1, app_interval_ms)),
        "--service", DEFAULT_SERVICE,
        "--strategy", args.strategy,
        "--ack-timeout-ms", str(args.ack_timeout_ms),
        "--timeout-ms", str(args.timeout_ms),
        "--output-csv", str(app_csv),
    ]
    if args.workload_mode == "open-loop":
        user_args.extend([
            "--rate-rps", str(float(args.rate_rps)),
            "--duration", str(int(args.duration)),
            "--max-outstanding", str(int(args.max_outstanding)),
            "--request-timeout-ms", str(int(args.request_timeout_ms)),
            "--drain-seconds", str(int(args.drain_seconds)),
        ])
        if getattr(args, "adaptive_admission_control", False):
            user_args.append("--adaptive-admission-control")
            adaptive_max = (args.adaptive_max_window
                            if args.adaptive_max_window is not None
                            else args.max_outstanding)
            adaptive_hard = (args.adaptive_hard_inflight_limit
                             if args.adaptive_hard_inflight_limit is not None
                             else adaptive_max)
            user_args.extend([
                "--adaptive-min-window", str(int(args.adaptive_min_window)),
                "--adaptive-max-window", str(int(adaptive_max)),
                "--adaptive-initial-window", str(int(args.adaptive_initial_window)),
                "--adaptive-hard-inflight-limit", str(int(adaptive_hard)),
                "--adaptive-ai-step", str(int(args.adaptive_ai_step)),
                "--adaptive-md-factor", str(float(args.adaptive_md_factor)),
                "--adaptive-severe-md-factor", str(float(args.adaptive_severe_md_factor)),
                "--adaptive-control-interval-ms",
                str(int(args.adaptive_control_interval_ms)),
                "--adaptive-target-latency-ms",
                str(int(args.adaptive_target_latency_ms)),
                "--adaptive-hard-target-latency-ms",
                str(int(args.adaptive_hard_target_latency_ms)),
            ])
            if args.adaptive_soft_queue_limit is not None:
                user_args.extend([
                    "--adaptive-soft-queue-limit",
                    str(int(args.adaptive_soft_queue_limit)),
                ])
            if args.adaptive_hard_queue_limit is not None:
                user_args.extend([
                    "--adaptive-hard-queue-limit",
                    str(int(args.adaptive_hard_queue_limit)),
                ])
            if args.adaptive_warning_backoff_ms is not None:
                user_args.extend([
                    "--adaptive-warning-backoff-ms",
                    str(int(args.adaptive_warning_backoff_ms)),
                ])
            if args.adaptive_reject_backoff_ms is not None:
                user_args.extend([
                    "--adaptive-reject-backoff-ms",
                    str(int(args.adaptive_reject_backoff_ms)),
                ])
            if args.disable_adaptive_queue_aware_pause:
                user_args.append("--disable-adaptive-queue-aware-pause")
            if args.disable_adaptive_recommended_rate:
                user_args.append("--disable-adaptive-recommended-rate")
            if args.adaptive_warning_resume_queue_depth is not None:
                user_args.extend([
                    "--adaptive-warning-resume-queue-depth",
                    str(int(args.adaptive_warning_resume_queue_depth)),
                ])
            if args.adaptive_reject_resume_queue_depth is not None:
                user_args.extend([
                    "--adaptive-reject-resume-queue-depth",
                    str(int(args.adaptive_reject_resume_queue_depth)),
                ])
            if args.adaptive_queue_pause_poll_ms is not None:
                user_args.extend([
                    "--adaptive-queue-pause-poll-ms",
                    str(int(args.adaptive_queue_pause_poll_ms)),
                ])
    if args.strategy == "custom-selection":
        user_args.append("--custom-selection")
    if getattr(args, "performance_mode", False):
        user_args.append("--performance-mode")
    if getattr(args, "handler_threads", -1) >= 0:
        user_args.extend(["--handler-threads", str(int(args.handler_threads))])
    if getattr(args, "disable_tokens", False):
        user_args.append("--disable-tokens")
    if getattr(args, "disable_hybrid_message_crypto", False):
        user_args.append("--disable-hybrid-message-crypto")
    elif getattr(args, "hybrid_message_crypto", False):
        user_args.append("--hybrid-message-crypto")
    if getattr(args, "timeline_trace", False):
        user_args.append("--timeline-trace")
    return user_args


def launch_apps_providers_first_for_svs(ndn, args, output_dir):
    processes = []
    session_base = int(time.time()) + os.getpid()
    try:
        write_svs_prefix_report(output_dir, args.providers, session_base, "pre-app")
        start_controller(ndn, output_dir, session_base, processes, args)
        start_providers(ndn, args, output_dir, session_base, processes, "svs-smoke")
        write_svs_prefix_report(output_dir, args.providers, session_base, "post-provider-ready")
        dump_face_counters(ndn, output_dir, "svs-smoke-after-provider-ready")
        collect_nfd_packet_logs(ndn, output_dir, "svs-smoke-after-provider-ready")
        settle_s = max(args.svs_settle_seconds, args.startup_settle_seconds, 0)
        if settle_s:
            log("Providers ready; settling {}s before starting App_User".format(settle_s))
            time.sleep(settle_s)

        app_csv = output_dir / "app_user_requests.csv"
        user_proc, user_log = start_process(
            ndn.net[args.user_node], "user", APP_TARGETS["user"], app_user_argv(args, app_csv),
            output_dir, session_base, processes, args)
        if not wait_for_log(user_log, r"Starting local NFD service latency benchmark|PERF_REQUEST_SENT|Waiting for decryption key", 20, user_proc):
            log("App_User did not emit benchmark/SVS startup marker before settle; continuing with diagnostics")
        return processes, user_proc, user_log, app_csv, session_base
    except Exception:
        stop_processes(processes)
        raise


def run_experiment(parser, pre_args):
    experiment_started_at = time.time()
    normalize_placement_args(pre_args)
    ensure_runtime(pre_args)
    ensure_minindn_root()

    log("Cleaning MiniNDN state")
    Minindn.cleanUp()
    Minindn.verifyDependencies()

    output_dir = output_directory(pre_args)
    output_dir.mkdir(parents=True, exist_ok=True)

    selected_topology = topology_file(pre_args)
    if selected_topology:
        ndn = Minindn(parser=parser, topoFile=selected_topology)
    else:
        log("Setup")
        ndn = Minindn(parser=parser, topo=build_default_topology(pre_args.providers))

    # MiniNDN owns final parsed args when using the official parser= pattern.
    args = normalize_placement_args(ndn.args)
    if args.nlsr_converge_seconds is None:
        args.nlsr_converge_seconds = 30 if topology_file(args) else 0
    args.experiment_started_at = experiment_started_at
    processes = []
    try:
        ndn.start()
        validate_node_placement({host.name for host in ndn.net.hosts}, args)
        log("MiniNDN nodes available: {}".format(sorted(host.name for host in ndn.net.hosts)))
        log("Node placement: user={} controller={} providers={}".format(
            args.user_node, args.controller_node, selected_provider_nodes(args)))

        log("Starting NFD on nodes")
        nfd_log_level = args.nfd_log_level
        if (args.svs_smoke or args.svs_only_smoke or args.dump_nfd_state or
                args.dk_bootstrap_check) and nfd_log_level == "INFO":
            nfd_log_level = "TRACE"
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel=nfd_log_level)
        wait_for_nfd_sockets(ndn, output_dir)
        log("NFD log level={}".format(nfd_log_level))
        log("Starting NLSR on nodes")
        AppManager(ndn, ndn.net.hosts, Nlsr)
        time.sleep(2)

        configure_static_routes(ndn, args)
        advertise_nlsr_prefixes(ndn, args)
        wait_for_nlsr_convergence(ndn, args, output_dir)

        dump_face_counters(ndn, output_dir, "pre-workload")
        if args.dump_nfd_state or args.connectivity_check or args.svs_smoke or args.svs_only_smoke:
            dump_nfd_state(ndn, output_dir, "pre-workload")
            write_routing_diagnostics(ndn, output_dir, args.providers, "pre-workload")

        if args.connectivity_check:
            run_connectivity_smoke(ndn, args, output_dir)
            log("Connectivity diagnostics completed; skipping NDNSF workload")
            return

        if args.svs_only_smoke:
            initialize_example_keychains(ndn, args, output_dir)
            dump_face_counters(ndn, output_dir, "pre-svs-only-smoke")
            write_svs_prefix_report(output_dir, args.providers, int(time.time()) + os.getpid(),
                                    "pre-svs-only-smoke")
            run_svs_only_smoke(ndn, args, output_dir)
            compare_face_counter_snapshots(output_dir, [
                "pre-svs-only-smoke",
                "svs-only-after-user-start",
                "svs-only-after-provider-start",
            ])
            log("SVS-only diagnostics completed; skipping performance workload")
            return

        if args.svs_smoke:
            run_svs_smoke(ndn, args, output_dir)
            if args.dump_nfd_state:
                dump_nfd_state(ndn, output_dir, "post-svs-smoke")
                dump_face_counters(ndn, output_dir, "post-svs-smoke")
                write_routing_diagnostics(ndn, output_dir, args.providers, "post-svs-smoke")
            log("SVS smoke diagnostics completed; skipping performance workload")
            return

        initialize_example_keychains(ndn, args, output_dir)
        start_provider_cert_servers(ndn, args, output_dir, processes)

        if args.dk_forwarding_check:
            run_dk_forwarding_check(ndn, args, output_dir)
            log("DK forwarding diagnostics completed; skipping performance workload")
            return

        if args.dk_bootstrap_check:
            run_dk_bootstrap_check(ndn, args, output_dir)
            log("DK bootstrap diagnostics completed; skipping performance workload")
            return

        if parse_rate_series(args.rate_series):
            aggregate = run_rate_series(ndn, args, output_dir)
            log("aggregate-summary.txt={}".format(output_dir / "aggregate-summary.txt"))
            log("aggregate-summary.json={}".format(output_dir / "aggregate-summary.json"))
            log("aggregate-rates.csv={}".format(output_dir / "aggregate-rates.csv"))
            log("stayed_under_3_minutes={}".format(aggregate["stayed_under_3_minutes"]))
        else:
            run_workload_once(ndn, args, output_dir)
        if args.dump_nfd_state:
            dump_nfd_state(ndn, output_dir, "post-workload")
            dump_face_counters(ndn, output_dir, "post-workload")
            write_routing_diagnostics(ndn, output_dir, args.providers, "post-workload")
    finally:
        stop_processes(processes)
        ndn.stop()
        Minindn.cleanUp()


def main():
    setLogLevel("info")
    parser = build_parser()
    pre_args, _ = parser.parse_known_args()
    if pre_args.dry_run:
        dry_run(pre_args)
        return
    run_experiment(parser, pre_args)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("NDNSF_NewAPI_Minindn_Perf failed: {}".format(e), file=sys.stderr)
        try:
            Minindn.cleanUp()
        except Exception:
            pass
        sys.exit(1)
