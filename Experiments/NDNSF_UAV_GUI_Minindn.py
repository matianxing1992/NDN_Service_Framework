#!/usr/bin/env python3
"""Interactive MiniNDN launcher for the NDNSF UAV GUI demo.

The script starts a controller node, one drone GUI, and one ground-station GUI
inside MiniNDN. The GUI windows are displayed through the host X11 session while
each process talks to its own MiniNDN NFD socket.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
MININDN_ROOT = Path("/tmp/minindn")
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
DEFAULT_VIDEO_SOURCE = REPO / "NDNSF-UAV-APP/videos/drone.mp4"
CONTROLLER_READY_MARKERS = [
    "ServiceController started...",
    "ServiceController started",
    "App_ServiceController started",
]
JMAVSIM_READY_MARKERS = [
    "INFO  [simulator_mavlink] Simulator connected",
    "INFO  [mavlink] mode:",
    "INFO  [px4] Startup script returned successfully",
    "INFO  [commander] Ready for takeoff!",
]


def log(message: str) -> None:
    info(message + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run NDNSF UAV GUI apps inside MiniNDN")
    parser.add_argument("--topology-file", default=str(DEFAULT_TOPOLOGY))
    parser.add_argument("--controller-node", default="csu")
    parser.add_argument("--gs-node", default="memphis")
    parser.add_argument("--drone-node", default="ucla")
    parser.add_argument("--drone-id", default="A")
    parser.add_argument("--patrol-drone-nodes", default="ucla,wustl",
                        help="Comma-separated MiniNDN nodes for the auto patrol demo.")
    parser.add_argument("--patrol-drone-ids", default="A,B",
                        help="Comma-separated drone IDs for the auto patrol demo.")
    parser.add_argument("--video-source", default=str(DEFAULT_VIDEO_SOURCE))
    parser.add_argument("--flight-controller-backend", default="mock",
                        choices=["mock", "udp"],
                        help="Drone flight-controller backend. Use udp for PX4/jMAVSim SITL.")
    parser.add_argument("--mavlink-udp-host", default="127.0.0.1")
    parser.add_argument("--mavlink-udp-port", default="18570",
                        help="PX4 SITL GCS MAVLink UDP local port for instance 0.")
    parser.add_argument("--start-jmavsim", action="store_true",
                        help="Start PX4 SITL with jMAVSim on the same MiniNDN node as the drone.")
    parser.add_argument("--px4-dir", default=str(Path.home() / "PX4-Autopilot"))
    parser.add_argument("--px4-cmake-args", default="-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
                        help="Extra CMAKE_ARGS passed to PX4 make when starting jMAVSim.")
    parser.add_argument("--jmavsim-headless", action="store_true",
                        help="Run jMAVSim without its GUI.")
    parser.add_argument("--jmavsim-ready-timeout-seconds", type=int, default=90,
                        help="How long to wait for PX4/jMAVSim readiness before starting the Drone app.")
    parser.add_argument("--video-bitrate-kbps", type=int, default=8000,
                        help="Requested video bitrate passed to the ground-station control request.")
    parser.add_argument("--video-width", type=int, default=480,
                        help="Requested encoded frame width passed to the drone video service.")
    parser.add_argument("--output-dir", default=str(REPO / "results/uav_gui_minindn"))
    parser.add_argument("--nfd-log-level", default="WARN")
    parser.add_argument("--auto-video-test", action="store_true",
                        help="Have the GS auto-start and auto-stop video for smoke testing.")
    parser.add_argument("--auto-mavlink-test", action="store_true",
                        help="Have the GS send Arm/Takeoff/Land over Targeted NDNSF for smoke testing.")
    parser.add_argument("--auto-keyboard-test", action="store_true",
                        help="Have the GS trigger the same keyboard shortcuts as a/t/l for smoke testing.")
    parser.add_argument("--auto-patrol-test", action="store_true",
                        help="Run the GS patrol compensation smoke test instead of the video GUI smoke.")
    parser.add_argument("--auto-stop-seconds", type=int, default=10)
    parser.add_argument("--auto-start-delay-ms", type=int, default=3000,
                        help="Delay before auto video start; useful for reproducing early manual clicks.")
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


def csv_values(value: str) -> list[str]:
    return [item for item in (part.strip() for part in value.split(",")) if item]


def active_drones(args: argparse.Namespace) -> list[tuple[str, str]]:
    if not args.auto_patrol_test:
        return [(args.drone_id, args.drone_node)]
    ids = csv_values(args.patrol_drone_ids)
    nodes = csv_values(args.patrol_drone_nodes)
    if len(ids) != len(nodes) or len(ids) < 2:
        raise ValueError("--auto-patrol-test requires matching --patrol-drone-ids and --patrol-drone-nodes with at least two entries")
    return list(zip(ids, nodes))


def resolve_repo_path(value: str) -> str:
    path = Path(value)
    if path.is_absolute():
        return str(path)
    return str((REPO / path).resolve())


def node_home(ndn, node_name: str) -> Path:
    node = ndn.net[node_name]
    return Path(node.params.get("params", {}).get("homeDir", str(MININDN_ROOT / node_name)))


def write_node_client_conf(home: Path, node_name: str) -> Path:
    ndn_dir = home / ".ndn"
    ndn_dir.mkdir(parents=True, exist_ok=True)
    conf = ndn_dir / "client.conf"
    conf.write_text(f"transport=unix:///run/nfd/{node_name}.sock\n", encoding="utf-8")
    return conf


def make_env(args: argparse.Namespace, node_name: str, home: Path) -> dict[str, str]:
    client_conf = write_node_client_conf(home, node_name)
    env = {
        "HOME": str(home),
        "NDN_CLIENT_CONF": str(client_conf),
        "NDN_CLIENT_TRANSPORT": f"unix:///run/nfd/{node_name}.sock",
        "NDN_LOG": os.environ.get(
            "NDNSF_APP_NDN_LOG",
            os.environ.get(
                "NDN_LOG",
                "ndn_service_framework.*=INFO:nacabe.*=WARN:ndnsvs.*=WARN:ndnsd.*=WARN",
            ),
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
    if os.environ.get("PATH"):
        env["PATH"] = os.environ["PATH"]
    python_paths = []
    if os.environ.get("PYTHONPATH"):
        python_paths.extend(part for part in os.environ["PYTHONPATH"].split(":") if part)
    kconfig_spec = importlib.util.find_spec("kconfiglib")
    if kconfig_spec and kconfig_spec.origin:
        python_paths.append(str(Path(kconfig_spec.origin).resolve().parent))
    if python_paths:
        deduped = []
        for path in python_paths:
            if path not in deduped:
                deduped.append(path)
        env["PYTHONPATH"] = ":".join(deduped)

    # MiniNDN demos in this checkout should exercise the freshly built framework
    # instead of an older system install under /usr/local/lib.
    ld_paths = [str(REPO / "build")]
    if os.environ.get("LD_LIBRARY_PATH"):
        ld_paths.append(os.environ["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(ld_paths)
    for name in ("DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"):
        if os.environ.get(name):
            env[name] = os.environ[name]

    # AddressSanitizer can report false new-delete mismatches from gtkmm/glibmm in
    # some GUI stacks. Keep ASAN checks but disable this specific check for the app.
    asan_options = os.environ.get("ASAN_OPTIONS", "")
    if "new_delete_type_mismatch=0" not in asan_options:
        env["ASAN_OPTIONS"] = (
            asan_options + "," if asan_options else ""
        ) + "new_delete_type_mismatch=0"

    return env


def run_shell(command: str) -> None:
    subprocess.run(command, shell=True, check=True)


def setup_node_keychains(ndn, args: argparse.Namespace, output_dir: Path) -> dict[str, Path]:
    drones = active_drones(args)
    homes = {
        args.controller_node: node_home(ndn, args.controller_node),
        args.gs_node: node_home(ndn, args.gs_node),
    }
    for _, node_name in drones:
        homes[node_name] = node_home(ndn, node_name)
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
    }
    for drone_id, node_name in drones:
        identities[node_name] = f"/example/uav/drone/{drone_id}"
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
    drones = active_drones(args)
    routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
    routing.addOrigin(
        [ndn.net[args.controller_node]],
        ["/example/uav/controller", "/example/uav/controller/KEY", "/example/uav/controller/DKEY"],
    )
    routing.addOrigin(
        [ndn.net[args.gs_node]],
        ["/example/uav/gs", "/example/uav/gs/KEY", "/example/uav/group"],
    )
    for drone_id, node_name in drones:
        drone_prefix = f"/example/uav/drone/{drone_id}"
        routing.addOrigin(
            [ndn.net[node_name]],
            [drone_prefix, f"{drone_prefix}/KEY", "/example/uav/group"],
        )
    routing.calculateRoutes()
    for node in ndn.net.hosts:
        for prefix in ("/example/uav", "/example/uav/group", "/example/uav/group/sync"):
            Nfdc.setStrategy(node, prefix, Nfdc.STRATEGY_MULTICAST)


def dump_uav_routes(ndn, args: argparse.Namespace, output_dir: Path) -> None:
    drone_prefixes = [f"/example/uav/drone/{drone_id}" for drone_id, _ in active_drones(args)]
    for node_name in [args.gs_node] + [node_name for _, node_name in active_drones(args)]:
        node = ndn.net[node_name]
        route_log = output_dir / f"{node_name}-nfd-routes.log"
        text = perf.node_cmd(
            node,
            "nfdc fib list | grep -E '({}|/example/uav)' || true".format(
                "|".join(shell_quote(prefix).strip("'") for prefix in drone_prefixes)))
        route_log.write_text(text, encoding="utf-8")


def start(node, name: str, command: str, env: dict[str, str], output_dir: Path, processes):
    log_path = output_dir / f"{name}.log"
    log_file = log_path.open("wb")
    log(f"start {name} on {node.name}: {command}")
    proc = getPopen(node, command, envDict=env, shell=True,
                    stdout=log_file, stderr=subprocess.STDOUT)
    processes.append((proc, log_file, log_path))
    return proc, log_path


def start_jmavsim(ndn, args: argparse.Namespace, drone_node: str,
                  drone_id: str, env: dict[str, str], output_dir: Path,
                  processes) -> tuple[subprocess.Popen, Path]:
    px4_dir = Path(args.px4_dir).resolve()
    if not (px4_dir / "Tools/simulation/jmavsim/jmavsim_run.sh").exists():
        raise RuntimeError(f"PX4 jMAVSim script not found under {px4_dir}")
    headless = "HEADLESS=1 " if args.jmavsim_headless else ""
    # Use the existing PX4 make target so PX4 and jMAVSim share the same node
    # namespace. If the PX4 build already exists this normally starts quickly;
    # otherwise PX4 may build first.
    command = (
        f"cd {shell_quote(px4_dir)} && "
        f"export PX4_SIM_MODEL=iris && "
        f"export CMAKE_ARGS={shell_quote(args.px4_cmake_args)} && "
        f"{headless}exec make px4_sitl jmavsim"
    )
    return start(ndn.net[drone_node], f"jmavsim-{drone_id}",
                 command, env, output_dir, processes)


def cleanup_px4_jmavsim(px4_dir: str) -> None:
    px4_root = str(Path(px4_dir).resolve())
    patterns = [
        f"{px4_root}/build/px4_sitl_default/bin/px4",
        "jmavsim_run.jar",
        "jmavsim_run.sh",
    ]
    for pattern in patterns:
        subprocess.run(["pkill", "-TERM", "-f", pattern], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop(processes) -> None:
    for proc, log_file, _ in reversed(processes):
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
        log_file.close()


def read_tail(path: Path, lines: int = 120) -> str:
    if not path.exists():
        return ""
    return "\n".join(path.read_text(errors="replace").splitlines()[-lines:])


def wait_log_any(path: Path, needles: list[str], timeout_s: float, proc=None) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            return False
        if path.exists():
            text = path.read_text(errors="replace")
            if any(needle in text for needle in needles):
                return True
        time.sleep(0.2)
    return False


def wait_log(path: Path, needle: str, timeout_s: float, proc=None) -> bool:
    return wait_log_any(path, [needle], timeout_s, proc=proc)


def wait_for_nfd_socket(node_name: str, timeout_s: float = 3.0) -> bool:
    socket_path = Path(f"/run/nfd/{node_name}.sock")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if socket_path.exists():
            return True
        time.sleep(0.1)
    return False


def require_log(path: Path, needle: str) -> None:
    text = path.read_text(errors="replace") if path.exists() else ""
    if needle not in text:
        tail = "\n".join(text.splitlines()[-40:])
        raise RuntimeError(f"missing '{needle}' in {path}\n--- tail ---\n{tail}")


def require_log_any(path: Path, needles: list[str]) -> None:
    text = path.read_text(errors="replace") if path.exists() else ""
    if not any(needle in text for needle in needles):
        tail = "\n".join(text.splitlines()[-40:])
        raise RuntimeError(f"missing any of {needles} in {path}\n--- tail ---\n{tail}")


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
        drones = active_drones(args)
        drone_envs = {
            drone_id: make_env(args, node_name, homes[node_name])
            for drone_id, node_name in drones
        }
        gs_env = make_env(args, args.gs_node, homes[args.gs_node])

        controller_cmd = app_cmd(APP_CONTROLLER, [
            "--controller-prefix", "/example/uav/controller",
            "--policy-file", str(POLICY.relative_to(REPO)),
        ])
        if not wait_for_nfd_socket(args.controller_node, 5):
            raise RuntimeError(
                f"NFD socket for controller node not ready: /run/nfd/{args.controller_node}.sock"
            )
        controller_proc, controller_log = start(
            ndn.net[args.controller_node], "controller",
            controller_cmd, controller_env, output_dir, processes
        )
        if not wait_log_any(controller_log, CONTROLLER_READY_MARKERS, 20, proc=controller_proc):
            raise RuntimeError(
                f"controller did not start (proc={controller_proc.pid}, rc={controller_proc.poll()}); "
                f"see {controller_log}\n--- tail ---\n{read_tail(controller_log)}"
            )

        drone_logs = {}
        for drone_id, node_name in drones:
            if args.start_jmavsim:
                cleanup_px4_jmavsim(args.px4_dir)
                jmavsim_proc, jmavsim_log = start_jmavsim(
                    ndn, args, node_name, drone_id,
                    drone_envs[drone_id], output_dir, processes)
                if not wait_log_any(jmavsim_log, JMAVSIM_READY_MARKERS,
                                    args.jmavsim_ready_timeout_seconds,
                                    proc=jmavsim_proc):
                    raise RuntimeError(
                        f"PX4/jMAVSim did not become ready; see {jmavsim_log}\n"
                        f"--- tail ---\n{read_tail(jmavsim_log)}"
                    )
            drone_cmd = app_cmd(APP_DRONE, [
                "--drone-id", drone_id,
                "--video-source", resolve_repo_path(args.video_source),
                "--flight-controller-backend",
                "udp" if args.start_jmavsim else args.flight_controller_backend,
                "--mavlink-udp-host", args.mavlink_udp_host,
                "--mavlink-udp-port", args.mavlink_udp_port,
            ])
            label = "drone" if len(drones) == 1 else f"drone-{drone_id}"
            drone_proc, drone_log = start(ndn.net[node_name], label,
                                          drone_cmd, drone_envs[drone_id],
                                          output_dir, processes)
            drone_logs[drone_id] = drone_log
            if not wait_log(drone_log, "DRONE_GUI_READY", 30, drone_proc):
                raise RuntimeError(f"drone {drone_id} GUI did not start; see {drone_log}")

        gs_argv = [
            "--target-drone", args.drone_id,
            "--video-bitrate-kbps", str(args.video_bitrate_kbps),
            "--video-width", str(args.video_width),
        ]
        if args.auto_video_test:
            gs_argv += [
                "--auto-video-test",
                "--auto-stop-seconds", str(args.auto_stop_seconds),
                "--auto-start-delay-ms", str(args.auto_start_delay_ms),
            ]
        if args.auto_mavlink_test:
            gs_argv += ["--auto-mavlink-test"]
        if args.auto_keyboard_test:
            gs_argv += ["--auto-keyboard-test"]
        if args.auto_patrol_test:
            gs_argv += [
                "--auto-patrol-test",
                "--patrol-drones", ",".join(drone_id for drone_id, _ in drones),
                "--ack-timeout-ms", "700",
                "--timeout-ms", "3000",
            ]
        gs_proc, gs_log = start(ndn.net[args.gs_node], "ground-station",
                                app_cmd(APP_GS, gs_argv), gs_env, output_dir, processes)
        if not args.auto_patrol_test and not wait_log(gs_log, "GS_GUI_READY", 30, gs_proc):
            raise RuntimeError(f"ground station GUI did not start; see {gs_log}")

        print("")
        print("NDNSF_UAV_GUI_MININDN_READY")
        print(f"  controller: {args.controller_node} log={controller_log}")
        for drone_id, node_name in drones:
            print(f"  drone {drone_id}:  {node_name} log={drone_logs[drone_id]}")
        print(f"  gs:         {args.gs_node} log={gs_log}")
        print("Use the Ground Station window buttons to start/stop video.")
        print("Type 'exit' in the MiniNDN CLI, or press Ctrl-C here, to stop the demo.")
        print("")

        if args.auto_patrol_test and args.no_cli:
            try:
                gs_proc.wait(timeout=45)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station patrol smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "PATROL_TASK_START")
            require_log(gs_log, "PATROL_PART_MISSING")
            require_log(gs_log, "PATROL_COMPENSATION")
            require_log(gs_log, "PATROL_ACK_BUSY")
            require_log(gs_log, "attempt=1 part=part1 provider=B")
            require_log(gs_log, "attempt=2 part=part0 provider=B")
            require_log(gs_log, "PATROL_TASK_DONE")
            require_log(drone_logs[drones[0][0]], "mission response delayed")
            print("NDNSF_UAV_PATROL_MININDN_SMOKE_OK")
        elif (args.auto_mavlink_test or args.auto_keyboard_test) and args.no_cli:
            try:
                gs_proc.wait(timeout=45)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station MAVLink smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "GS_TARGETED_RESPONSE service=/UAV/MAVLink/Execute")
            require_log(gs_log, "MAVLink arm accepted=true")
            require_log(gs_log, "MAVLink takeoff accepted=true")
            require_log(gs_log, "MAVLink land accepted=true")
            require_log_any(drone_logs[args.drone_id], [
                "MOCK_FC_FORWARD drone=" + args.drone_id,
                "UDP_FC_FORWARD drone=" + args.drone_id,
            ])
            print("NDNSF_UAV_MAVLINK_TARGETED_MININDN_SMOKE_OK")
        elif args.auto_video_test and args.no_cli:
            try:
                gs_proc.wait(timeout=max(45, args.auto_stop_seconds + 35))
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station auto smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "GS_STATUS Video packet stream")
            require_log(gs_log, "GS_DECODED_FRAMES count=30")
            require_log(gs_log, "GS_GUI_EXIT rc=0")
            require_log(drone_logs[args.drone_id], "DRONE_STATUS drone=" + args.drone_id + " video streaming")
            require_log(drone_logs[args.drone_id], "DRONE_STATUS drone=" + args.drone_id + " video stopped")
            print("NDNSF_UAV_GUI_MININDN_SMOKE_OK")
        elif args.no_cli:
            while True:
                time.sleep(1)
        else:
            MiniNDNCLI(ndn.net)
        return 0
    finally:
        stop(processes)
        if 'args' in locals() and args.start_jmavsim:
            cleanup_px4_jmavsim(args.px4_dir)
        if ndn is not None:
            ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
