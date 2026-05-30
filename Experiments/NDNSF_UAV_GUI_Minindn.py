#!/usr/bin/env python3
"""Interactive MiniNDN launcher for the NDNSF UAV GUI demo.

The script starts a controller node, one drone GUI, and one ground-station GUI
inside MiniNDN. The GUI windows are displayed through the host X11 session while
each process talks to its own MiniNDN NFD socket.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from mininet.log import info, setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import MiniNDNCLI, getPopen  # noqa: E402


DEFAULT_TOPOLOGY = REPO / "Experiments/Topology/testbed(loss=0%).conf"
APP_CONTROLLER = REPO / "build/examples/App_ServiceController"
APP_DRONE = REPO / "build/examples/UavDroneApp"
APP_GS = REPO / "build/examples/UavGroundStationApp"
POLICY = REPO / "NDNSF-UAV-APP/configs/uav_demo.policies"


def log(message: str) -> None:
    info(message + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run NDNSF UAV GUI apps inside MiniNDN")
    parser.add_argument("--topology-file", default=str(DEFAULT_TOPOLOGY))
    parser.add_argument("--controller-node", default="csu")
    parser.add_argument("--gs-node", default="memphis")
    parser.add_argument("--drone-node", default="ucla")
    parser.add_argument("--drone-id", default="A")
    parser.add_argument("--video-source", default="/home/tianxing/NDN/drone.mp4")
    parser.add_argument("--video-bitrate-kbps", type=int, default=8000,
                        help="Requested video bitrate passed to the ground-station control request.")
    parser.add_argument("--video-width", type=int, default=480,
                        help="Requested encoded frame width passed to the drone video service.")
    parser.add_argument("--output-dir", default=str(REPO / "results/uav_gui_minindn"))
    parser.add_argument("--nfd-log-level", default="WARN")
    parser.add_argument("--auto-video-test", action="store_true",
                        help="Have the GS auto-start and auto-stop video for smoke testing.")
    parser.add_argument("--auto-stop-seconds", type=int, default=10)
    parser.add_argument("--no-cli", action="store_true",
                        help="Do not open the MiniNDN CLI; wait until interrupted.")
    parser.add_argument("--no-xhost", action="store_true",
                        help="Do not try to grant root access to the current X server.")
    return parser


def shell_quote(value: object) -> str:
    return perf.shell_quote(str(value))


def app_cmd(binary: Path, argv: list[str]) -> str:
    parts = ["cd", shell_quote(REPO), "&&", "exec", shell_quote(binary)]
    parts.extend(shell_quote(arg) for arg in argv)
    return " ".join(parts)


def node_home(ndn, node_name: str) -> Path:
    node = ndn.net[node_name]
    return Path(node.params.get("params", {}).get("homeDir", f"/tmp/minindn/{node_name}"))


def write_node_client_conf(home: Path, node_name: str) -> Path:
    ndn_dir = home / ".ndn"
    ndn_dir.mkdir(parents=True, exist_ok=True)
    conf = ndn_dir / "client.conf"
    conf.write_text(f"transport=unix:///run/nfd/{node_name}.sock\n", encoding="utf-8")
    return conf


def make_env(args: argparse.Namespace, node_name: str, home: Path) -> dict[str, str]:
    client_conf = write_node_client_conf(home, node_name)
    env = {
        "LD_LIBRARY_PATH": f"{REPO / 'build'}:{os.environ.get('LD_LIBRARY_PATH', '')}",
        "HOME": str(home),
        "NDN_CLIENT_CONF": str(client_conf),
        "NDN_CLIENT_TRANSPORT": f"unix:///run/nfd/{node_name}.sock",
        "NDN_LOG": os.environ.get(
            "NDN_LOG",
            "ndn_service_framework.*=INFO:nacabe.*=WARN:ndnsvs.*=WARN:ndnsd.*=WARN",
        ),
        "NDNSF_DISABLE_NDNSD": os.environ.get("NDNSF_DISABLE_NDNSD", "1"),
        "NDNSF_SVS_MAX_SUPPRESSION_MS": os.environ.get("NDNSF_SVS_MAX_SUPPRESSION_MS", "1"),
        "NDNSF_SVS_PARALLEL_SYNC": os.environ.get("NDNSF_SVS_PARALLEL_SYNC", "0"),
        "NDNSF_SVS_PARALLEL_WORKERS": os.environ.get("NDNSF_SVS_PARALLEL_WORKERS", "4"),
        "NDNSF_SVS_PARALLEL_QUEUE": os.environ.get("NDNSF_SVS_PARALLEL_QUEUE", "256"),
        "NDNSF_SVS_PARALLEL_PRODUCTION": os.environ.get("NDNSF_SVS_PARALLEL_PRODUCTION", "0"),
        "NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING": os.environ.get(
            "NDNSF_SVS_PARALLEL_PRODUCTION_SIGNING", "0"),
        "NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK": os.environ.get(
            "NDNSF_SVS_PARALLEL_PRODUCTION_EXTRA_BLOCK", "1"),
    }
    for name in ("DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"):
        if os.environ.get(name):
            env[name] = os.environ[name]
    return env


def run_shell(command: str) -> None:
    subprocess.run(command, shell=True, check=True)


def setup_node_keychains(ndn, args: argparse.Namespace, output_dir: Path) -> dict[str, Path]:
    homes = {
        args.controller_node: node_home(ndn, args.controller_node),
        args.gs_node: node_home(ndn, args.gs_node),
        args.drone_node: node_home(ndn, args.drone_node),
    }
    for node_name, home in homes.items():
        subprocess.run(["rm", "-rf", str(home / ".ndn")], check=False)
        write_node_client_conf(home, node_name)

    key_source = output_dir / "identity-source"
    subprocess.run(["rm", "-rf", str(key_source)], check=False)
    (key_source / ".ndn").mkdir(parents=True, exist_ok=True)
    key_source_conf = write_node_client_conf(key_source, args.controller_node)

    passphrase = "ndnsf-uav-minindn"
    identities = {
        args.controller_node: "/example/uav/controller",
        args.gs_node: "/example/uav/gs",
        args.drone_node: f"/example/uav/drone/{args.drone_id}",
    }
    bags = []
    for node_name, identity in identities.items():
        bag = output_dir / f"{node_name}.safebag"
        run_shell(
            "HOME={} NDN_CLIENT_CONF={} ndnsec key-gen -t r {} >/dev/null; "
            "HOME={} NDN_CLIENT_CONF={} ndnsec export -P {} -o {} -i {}".format(
                shell_quote(key_source),
                shell_quote(key_source_conf),
                shell_quote(identity),
                shell_quote(key_source),
                shell_quote(key_source_conf),
                shell_quote(passphrase),
                shell_quote(bag),
                shell_quote(identity),
            )
        )
        bags.append(bag)

    for target_node, home in homes.items():
        client_conf = home / ".ndn" / "client.conf"
        for bag in bags:
            perf.node_cmd(
                ndn.net[target_node],
                "HOME={} NDN_CLIENT_CONF={} ndnsec import -P {} {} >/dev/null 2>&1 || true".format(
                    shell_quote(home),
                    shell_quote(client_conf),
                    shell_quote(passphrase),
                    shell_quote(bag),
                ))
        perf.node_cmd(
            ndn.net[target_node],
            "HOME={} NDN_CLIENT_CONF={} ndnsec set-default -n {} >/dev/null 2>&1 || true".format(
                shell_quote(home),
                shell_quote(client_conf),
                shell_quote(identities[target_node]),
            ))

    return homes


def maybe_allow_x11(args: argparse.Namespace) -> None:
    if args.no_xhost or not os.environ.get("DISPLAY"):
        return
    subprocess.run(["xhost", "+SI:localuser:root"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def configure_routes(ndn, args: argparse.Namespace) -> None:
    routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
    routing.addOrigin(
        [ndn.net[args.controller_node]],
        ["/example/uav/controller", "/example/uav/controller/KEY", "/example/uav/controller/DKEY"],
    )
    routing.addOrigin(
        [ndn.net[args.gs_node]],
        ["/example/uav/gs", "/example/uav/gs/KEY", "/example/uav/group"],
    )
    drone_prefix = f"/example/uav/drone/{args.drone_id}"
    routing.addOrigin(
        [ndn.net[args.drone_node]],
        [drone_prefix, f"{drone_prefix}/KEY", "/example/uav/group"],
    )
    routing.calculateRoutes()
    for node in ndn.net.hosts:
        for prefix in ("/example/uav", "/example/uav/group", "/example/uav/group/sync"):
            Nfdc.setStrategy(node, prefix, Nfdc.STRATEGY_MULTICAST)


def dump_uav_routes(ndn, args: argparse.Namespace, output_dir: Path) -> None:
    drone_prefix = f"/example/uav/drone/{args.drone_id}"
    for node_name in (args.gs_node, args.drone_node):
        node = ndn.net[node_name]
        route_log = output_dir / f"{node_name}-nfd-routes.log"
        text = perf.node_cmd(
            node,
            "nfdc fib list | grep -E '({}|/example/uav)' || true".format(
                shell_quote(drone_prefix).strip("'")))
        route_log.write_text(text, encoding="utf-8")


def start(node, name: str, command: str, env: dict[str, str], output_dir: Path, processes):
    log_path = output_dir / f"{name}.log"
    log_file = log_path.open("wb")
    log(f"start {name} on {node.name}: {command}")
    proc = getPopen(node, command, envDict=env, shell=True,
                    stdout=log_file, stderr=subprocess.STDOUT)
    processes.append((proc, log_file, log_path))
    return proc, log_path


def stop(processes) -> None:
    for proc, log_file, _ in reversed(processes):
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
        log_file.close()


def wait_log(path: Path, needle: str, timeout_s: float, proc=None) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            return False
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        time.sleep(0.2)
    return False


def require_log(path: Path, needle: str) -> None:
    text = path.read_text(errors="replace") if path.exists() else ""
    if needle not in text:
        tail = "\n".join(text.splitlines()[-40:])
        raise RuntimeError(f"missing '{needle}' in {path}\n--- tail ---\n{tail}")


def main() -> int:
    args = build_parser().parse_args()
    sys.argv = [sys.argv[0]]
    setLogLevel("info")
    if not os.environ.get("DISPLAY"):
        raise RuntimeError("DISPLAY is not set; run from a graphical session, e.g. sudo -E ...")

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    maybe_allow_x11(args)

    processes = []
    ndn = None
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    try:
        ndn = Minindn(topoFile=args.topology_file)
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel=args.nfd_log_level)
        perf.wait_for_nfd_sockets(ndn, output_dir)
        configure_routes(ndn, args)
        dump_uav_routes(ndn, args, output_dir)
        homes = setup_node_keychains(ndn, args, output_dir)

        controller_env = make_env(args, args.controller_node, homes[args.controller_node])
        drone_env = make_env(args, args.drone_node, homes[args.drone_node])
        gs_env = make_env(args, args.gs_node, homes[args.gs_node])

        controller_cmd = app_cmd(APP_CONTROLLER, [
            "--controller-prefix", "/example/uav/controller",
            "--policy-file", str(POLICY.relative_to(REPO)),
        ])
        _, controller_log = start(ndn.net[args.controller_node], "controller",
                                  controller_cmd, controller_env, output_dir, processes)
        if not wait_log(controller_log, "ServiceController started", 20):
            raise RuntimeError(f"controller did not start; see {controller_log}")

        drone_cmd = app_cmd(APP_DRONE, [
            "--drone-id", args.drone_id,
            "--video-source", args.video_source,
        ])
        drone_proc, drone_log = start(ndn.net[args.drone_node], "drone",
                                      drone_cmd, drone_env, output_dir, processes)
        if not wait_log(drone_log, "DRONE_GUI_READY", 30, drone_proc):
            raise RuntimeError(f"drone GUI did not start; see {drone_log}")

        gs_argv = [
            "--target-drone", args.drone_id,
            "--video-bitrate-kbps", str(args.video_bitrate_kbps),
            "--video-width", str(args.video_width),
        ]
        if args.auto_video_test:
            gs_argv += ["--auto-video-test", "--auto-stop-seconds", str(args.auto_stop_seconds)]
        gs_proc, gs_log = start(ndn.net[args.gs_node], "ground-station",
                                app_cmd(APP_GS, gs_argv), gs_env, output_dir, processes)
        if not wait_log(gs_log, "GS_GUI_READY", 30, gs_proc):
            raise RuntimeError(f"ground station GUI did not start; see {gs_log}")

        print("")
        print("NDNSF_UAV_GUI_MININDN_READY")
        print(f"  controller: {args.controller_node} log={controller_log}")
        print(f"  drone:      {args.drone_node} log={drone_log}")
        print(f"  gs:         {args.gs_node} log={gs_log}")
        print("Use the Ground Station window buttons to start/stop video.")
        print("Type 'exit' in the MiniNDN CLI, or press Ctrl-C here, to stop the demo.")
        print("")

        if args.auto_video_test and args.no_cli:
            try:
                gs_proc.wait(timeout=max(45, args.auto_stop_seconds + 35))
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station auto smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "GS_STATUS Video packet stream")
            require_log(gs_log, "GS_DECODED_FRAMES count=30")
            require_log(gs_log, "GS_GUI_EXIT rc=0")
            require_log(drone_log, "DRONE_STATUS drone=" + args.drone_id + " video streaming")
            require_log(drone_log, "DRONE_STATUS drone=" + args.drone_id + " video stopped")
            print("NDNSF_UAV_GUI_MININDN_SMOKE_OK")
        elif args.no_cli:
            while True:
                time.sleep(1)
        else:
            MiniNDNCLI(ndn.net)
        return 0
    finally:
        stop(processes)
        if ndn is not None:
            ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
