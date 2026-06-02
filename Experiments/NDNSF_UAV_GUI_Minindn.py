#!/usr/bin/env python3
"""Interactive MiniNDN launcher for the NDNSF UAV GUI demo.

The script starts a controller node, one drone GUI, and one ground-station GUI
inside MiniNDN. The GUI windows are displayed through the host X11 session while
each process talks to its own MiniNDN NFD socket.
"""

from __future__ import annotations

import argparse
import glob
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
import urllib.request
import math

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
APP_BUILD_DIR = Path(os.environ.get("NDNSF_UAV_APP_BUILD_DIR",
                                    str(REPO / "build/examples")))
APP_CONTROLLER = APP_BUILD_DIR / "App_ServiceController"
APP_DRONE = APP_BUILD_DIR / "UavDroneApp"
APP_GS = APP_BUILD_DIR / "UavGroundStationApp"
POLICY = REPO / "NDNSF-UAV-APP/configs/uav_demo.policies"
DEFAULT_RUNTIME_CONFIG = REPO / "NDNSF-UAV-APP/configs/uav_runtime.conf"
DEFAULT_VIDEO_SOURCE = REPO / "NDNSF-UAV-APP/videos/drone.mp4"
CONTROLLER_READY_MARKERS = [
    "ServiceController started...",
    "ServiceController started",
    "App_ServiceController started",
]
JMAVSIM_READY_MARKERS = [
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
    parser.add_argument("--runtime-config", default=str(DEFAULT_RUNTIME_CONFIG),
                        help="UAV namespace/service config passed to Drone and Ground Station apps.")
    parser.add_argument("--app-config-dir", default=str(REPO / "NDNSF-UAV-APP/configs"),
                        help="Directory containing per-instance drone-<id>.conf and ground-station.conf.")
    parser.add_argument("--gs-app-config", default="",
                        help="Explicit per-instance config for the ground station.")
    parser.add_argument("--video-source", default=str(DEFAULT_VIDEO_SOURCE),
                        help="Video file used as virtual-camera input or direct file fallback.")
    parser.add_argument("--camera-mode", default="auto", choices=["auto", "device", "file"],
                        help="auto: use a real /dev/video* camera or create a virtual camera from --video-source; "
                             "device: require --camera-device; file: pass --video-source directly.")
    parser.add_argument("--camera-device", default="",
                        help="Explicit V4L2 camera device, for example /dev/video0.")
    parser.add_argument("--virtual-camera-device", default="/dev/video42",
                        help="V4L2 loopback device used when no physical camera is available.")
    parser.add_argument("--no-virtual-camera", action="store_true",
                        help="Do not create a v4l2loopback camera from --video-source when no camera exists.")
    parser.add_argument("--flight-controller-backend", default="mock",
                        choices=["mock", "udp"],
                        help="Drone flight-controller backend. Use udp for PX4/jMAVSim SITL.")
    parser.add_argument("--mavlink-udp-host", default="127.0.0.1")
    parser.add_argument("--mavlink-udp-port", default="18570",
                        help="PX4 SITL GCS MAVLink UDP local port for instance 0.")
    parser.add_argument("--mavlink-udp-listen-port", default="14550",
                        help="Local MAVLink GCS UDP port where PX4 sends command ACKs/telemetry.")
    parser.add_argument("--start-jmavsim", action="store_true",
                        help="Start PX4 SITL with jMAVSim on the same MiniNDN node as the drone.")
    parser.add_argument("--no-start-jmavsim", action="store_true",
                        help="Do not auto-start PX4 SITL/jMAVSim in interactive GUI mode.")
    parser.add_argument("--px4-dir", default=str(Path.home() / "PX4-Autopilot"))
    parser.add_argument("--px4-cmake-args", default="-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
                        help="Extra CMAKE_ARGS passed to PX4 make when starting jMAVSim.")
    parser.add_argument("--sim-home-lat", default="35.1186",
                        help="PX4/jMAVSim home latitude. Defaults to University of Memphis.")
    parser.add_argument("--sim-home-lon", default="-89.9375",
                        help="PX4/jMAVSim home longitude. Defaults to University of Memphis.")
    parser.add_argument("--sim-home-alt", default="100",
                        help="PX4/jMAVSim home altitude AMSL in meters.")
    parser.add_argument("--jmavsim-headless", action="store_true",
                        help="Run jMAVSim without its GUI.")
    parser.add_argument("--drone-headless", action="store_true",
                        help="Run Drone apps without local GTK windows; useful for board/airframe smoke tests.")
    parser.add_argument("--jmavsim-ready-timeout-seconds", type=int, default=90,
                        help="How long to wait for PX4/jMAVSim readiness before starting the Drone app.")
    parser.add_argument("--no-configure-px4-sitl-demo-params", action="store_true",
                        help="Do not let the Drone app set PX4 SITL demo failsafe parameters.")
    parser.add_argument("--video-bitrate-kbps", type=int, default=8000,
                        help="Requested video bitrate passed to the ground-station control request.")
    parser.add_argument("--video-width", type=int, default=480,
                        help="Requested encoded frame width passed to the drone video service.")
    parser.add_argument("--output-dir", default=str(REPO / "results/uav_gui_minindn"))
    parser.add_argument("--nfd-log-level", default="WARN")
    parser.add_argument("--enable-ndnsd", action="store_true",
                        help="Reserved for NDNSD service discovery experiments; not enabled by default.")
    parser.add_argument("--auto-video-test", action="store_true",
                        help="Have the GS auto-start and auto-stop video for smoke testing.")
    parser.add_argument("--auto-repeat-stop-test", action="store_true",
                        help="With --auto-video-test, delay the first Stop response and "
                             "auto-click Stop again to verify timeout/retry UI behavior.")
    parser.add_argument("--auto-mavlink-test", action="store_true",
                        help="Have the GS send Arm/Takeoff/Land over Targeted NDNSF for smoke testing.")
    parser.add_argument("--auto-telemetry-test", action="store_true",
                        help="Have the GS verify PX4/jMAVSim telemetry fields and state changes.")
    parser.add_argument("--auto-link-state-test", action="store_true",
                        help="Have the GS verify local telemetry stale/lost link state aging.")
    parser.add_argument("--auto-keyboard-test", action="store_true",
                        help="Have the GS trigger the same keyboard shortcuts as a/t/l for smoke testing.")
    parser.add_argument("--auto-manual-control-test", action="store_true",
                        help="Have the GS hold manual-control keys and send MAVLink MANUAL_CONTROL.")
    parser.add_argument("--auto-two-drone-switch-test", action="store_true",
                        help="Have the GS switch between two drones and send Targeted MANUAL_CONTROL to each.")
    parser.add_argument("--auto-video-selection-test", action="store_true",
                        help="Have the GS verify video Start/Stop controls follow the selected drone.")
    parser.add_argument("--auto-mission-controls-test", action="store_true",
                        help="Have the GS verify mission buttons follow typed MissionState.")
    parser.add_argument("--auto-flight-controls-test", action="store_true",
                        help="Have the GS verify flight action buttons follow typed ReadinessState.")
    parser.add_argument("--auto-recording-playback-test", action="store_true",
                        help="Have the drone record to its local repo and the GS discover/replay the recording.")
    parser.add_argument("--auto-patrol-test", action="store_true",
                        help="Run the GS patrol compensation smoke test instead of the video GUI smoke.")
    parser.add_argument("--auto-single-mission-test", action="store_true",
                        help="Run a one-drone mission upload smoke test instead of the video GUI smoke.")
    parser.add_argument("--auto-single-mission-start-test", action="store_true",
                        help="After the single-drone mission upload smoke test, arm/takeoff/start mission.")
    parser.add_argument("--multi-drone-gui", action="store_true",
                        help="Start the patrol-drone set for an interactive multi-drone ground-station GUI.")
    parser.add_argument("--auto-stop-seconds", type=int, default=10)
    parser.add_argument("--auto-start-delay-ms", type=int, default=3000,
                        help="Delay before auto video start; useful for reproducing early manual clicks.")
    parser.add_argument("--link-stale-ms", type=int, default=3500,
                        help="GS local telemetry age threshold for stale link diagnostics.")
    parser.add_argument("--link-lost-ms", type=int, default=8000,
                        help="GS local telemetry age threshold for lost link diagnostics.")
    parser.add_argument("--lost-link-action", default="notify",
                        help="GS local diagnostic action label for lost-link state.")
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


def load_runtime_config(path: str) -> dict[str, str]:
    config = {
        "group-prefix": "/example/uav/group",
        "controller-prefix": "/example/uav/controller",
        "ground-station-identity": "/example/uav/gs",
        "drone-prefix": "/example/uav/drone",
        "root-identity": "/example/uav",
    }
    if not path:
        return config
    config_path = Path(path)
    if not config_path.exists():
        return config
    for raw_line in config_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" in line:
            key, value = [part.strip() for part in line.split("=", 1)]
        else:
            parts = line.split(None, 1)
            if len(parts) != 2:
                continue
            key, value = parts[0], parts[1].strip()
        if key and value:
            config[key] = value
    return config


def append_name(prefix: str, component: str) -> str:
    return prefix.rstrip("/") + "/" + component


def app_config_path(args: argparse.Namespace, filename: str) -> str:
    path = Path(filename)
    if path.is_absolute():
        return str(path)
    return str((Path(args.app_config_dir) / filename).resolve())


def active_drones(args: argparse.Namespace) -> list[tuple[str, str]]:
    interactive_default = not args.no_cli
    if not (args.auto_patrol_test or args.auto_two_drone_switch_test or
            args.auto_video_selection_test or
            args.auto_mission_controls_test or
            args.auto_flight_controls_test or
            args.multi_drone_gui or interactive_default):
        return [(args.drone_id, args.drone_node)]
    ids = csv_values(args.patrol_drone_ids)
    nodes = csv_values(args.patrol_drone_nodes)
    if len(ids) != len(nodes) or len(ids) < 2:
        raise ValueError("--auto-patrol-test requires matching --patrol-drone-ids and --patrol-drone-nodes with at least two entries")
    return list(zip(ids, nodes))


def prefetch_default_map_tile() -> None:
    """Fetch Memphis OSM tiles before MiniNDN isolates node networks."""
    lat = 35.1186
    lon = -89.9375
    for zoom in (14, 15, 16):
        lat_rad = lat * math.pi / 180.0
        n = 2.0 ** zoom
        center_x = int(math.floor((lon + 180.0) / 360.0 * n))
        center_y = int(math.floor((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n))
        for x in range(center_x - 1, center_x + 2):
            for y in range(center_y - 1, center_y + 2):
                offline_path = REPO / "NDNSF-UAV-APP/maps/osm" / str(zoom) / str(x) / f"{y}.png"
                if offline_path.exists() and offline_path.stat().st_size > 0:
                    continue
                path = Path(f"/tmp/ndnsf-uav-map-{zoom}-{x}-{y}.png")
                if path.exists() and path.stat().st_size > 0:
                    continue
                url = f"https://tile.openstreetmap.org/{zoom}/{x}/{y}.png"
                try:
                    request = urllib.request.Request(url, headers={"User-Agent": "ndnsf-uav-app"})
                    with urllib.request.urlopen(request, timeout=5) as response:
                        path.write_bytes(response.read())
                except Exception as e:
                    log(f"warning: could not prefetch map tile {url}: {e}")


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
                "ndn_service_framework.*=WARN:"
                "ndn_service_framework.examples.*=INFO:"
                "nacabe.*=WARN:ndnsvs.*=WARN:ndnsd.*=WARN",
            ),
        ),
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
    env["NDNSF_DISABLE_NDNSD"] = os.environ.get("NDNSF_DISABLE_NDNSD", "1")
    if args.enable_ndnsd:
        env["NDNSF_UAV_NDNSD_EXPERIMENT_REQUESTED"] = "1"
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


def video_devices() -> list[str]:
    devices = sorted(glob.glob("/dev/video*"))
    usable = []
    for device in devices:
        if not os.access(device, os.R_OK):
            continue
        info_result = subprocess.run(
            ["v4l2-ctl", "--device", device, "--info"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        info_text = info_result.stdout
        if "NDNSF-UAV-Camera" in info_text:
            continue
        caps_result = subprocess.run(
            ["v4l2-ctl", "--device", device, "--all"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        caps_text = caps_result.stdout
        if ("Video Capture" in info_text or "Video Capture" in caps_text or
                "Video Capture Multiplanar" in caps_text):
            usable.append(device)
    return usable


def start_virtual_camera(args: argparse.Namespace, output_dir: Path, processes):
    if args.no_virtual_camera:
        return None
    device = args.virtual_camera_device
    video_source = resolve_repo_path(args.video_source)
    if not Path(video_source).exists():
        log(f"warning: virtual camera source missing: {video_source}")
        return None
    if not Path(device).exists():
        video_nr = device[len("/dev/video"):] if device.startswith("/dev/video") else device
        if not video_nr.isdigit():
            log(f"warning: unsupported virtual camera device name: {device}")
            return None
        subprocess.run([
            "modprobe", "v4l2loopback",
            f"video_nr={video_nr}",
            "card_label=NDNSF-UAV-Camera",
            "exclusive_caps=1",
        ], check=False)
    deadline = time.time() + 3
    while time.time() < deadline and not Path(device).exists():
        time.sleep(0.1)
    if not Path(device).exists():
        log("warning: v4l2loopback is unavailable; no virtual camera created")
        return None

    log_path = output_dir / "virtual-camera.log"
    log_file = log_path.open("wb")
    command = [
        "ffmpeg", "-hide_banner", "-loglevel", "warning",
        "-re", "-stream_loop", "-1", "-i", video_source,
        "-vf", "fps=30,scale=640:-2,format=yuyv422",
        "-f", "v4l2", device,
    ]
    log(f"start virtual camera {device} from {video_source}")
    proc = subprocess.Popen(command, stdout=log_file, stderr=subprocess.STDOUT)
    processes.append((proc, log_file, log_path))
    time.sleep(1.0)
    if proc.poll() is not None:
        log(f"warning: virtual camera ffmpeg exited; see {log_path}")
        return None
    return device


def select_camera_source(args: argparse.Namespace, output_dir: Path, processes) -> tuple[str, str]:
    if args.camera_mode == "file":
        return resolve_repo_path(args.video_source), "file"
    if args.camera_device:
        if Path(args.camera_device).exists():
            return args.camera_device, "camera-device"
        raise RuntimeError(f"requested camera device does not exist: {args.camera_device}")
    devices = video_devices()
    if devices:
        return devices[0], "camera-device"
    if args.camera_mode == "device":
        raise RuntimeError("no V4L2 camera device is available")
    virtual_device = start_virtual_camera(args, output_dir, processes)
    if virtual_device:
        return virtual_device, "virtual-camera"
    log("warning: no camera available; falling back to direct file input")
    return resolve_repo_path(args.video_source), "file-fallback"


def setup_node_keychains(ndn, args: argparse.Namespace, output_dir: Path) -> dict[str, Path]:
    drones = active_drones(args)
    runtime = load_runtime_config(args.runtime_config)
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
        args.controller_node: runtime["controller-prefix"],
        args.gs_node: runtime["ground-station-identity"],
    }
    for drone_id, node_name in drones:
        identities[node_name] = append_name(runtime["drone-prefix"], drone_id)
    bags = []
    root_identity = runtime["root-identity"]
    root_cert = output_dir / "root.cert"
    run_shell(
        "HOME={} NDN_CLIENT_CONF={} ndnsec key-gen -t r {} > {}; "
        "HOME={} NDN_CLIENT_CONF={} ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
            shell_quote(key_source),
            shell_quote(key_source_conf),
            shell_quote(root_identity),
            shell_quote(root_cert),
            shell_quote(key_source),
            shell_quote(key_source_conf),
            shell_quote(root_cert),
        )
    )
    for node_name, identity in identities.items():
        bag = output_dir / f"{node_name}.safebag"
        req = output_dir / f"{node_name}.req"
        cert = output_dir / f"{node_name}.cert"
        run_shell(
            "HOME={} NDN_CLIENT_CONF={} ndnsec key-gen -n -t r {} > {}; "
            "HOME={} NDN_CLIENT_CONF={} ndnsec cert-gen -s {} -i ROOT {} > {}; "
            "HOME={} NDN_CLIENT_CONF={} ndnsec cert-install -f {} >/dev/null 2>&1 || true; "
            "HOME={} NDN_CLIENT_CONF={} ndnsec export -P {} -o {} -i {}".format(
                shell_quote(key_source),
                shell_quote(key_source_conf),
                shell_quote(identity),
                shell_quote(req),
                shell_quote(key_source),
                shell_quote(key_source_conf),
                shell_quote(root_identity),
                shell_quote(req),
                shell_quote(cert),
                shell_quote(key_source),
                shell_quote(key_source_conf),
                shell_quote(cert),
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
        perf.node_cmd(
            ndn.net[target_node],
            "HOME={} NDN_CLIENT_CONF={} ndnsec cert-install -f {} >/dev/null 2>&1 || true".format(
                shell_quote(home),
                shell_quote(client_conf),
                shell_quote(root_cert),
            ))
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
    runtime = load_runtime_config(args.runtime_config)
    controller_prefix = runtime["controller-prefix"]
    gs_prefix = runtime["ground-station-identity"]
    group_prefix = runtime["group-prefix"]
    root_prefix = runtime["root-identity"]
    routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
    routing.addOrigin(
        [ndn.net[args.controller_node]],
        [controller_prefix, f"{controller_prefix}/KEY", f"{controller_prefix}/DKEY"],
    )
    routing.addOrigin(
        [ndn.net[args.gs_node]],
        [gs_prefix, f"{gs_prefix}/KEY", group_prefix],
    )
    for drone_id, node_name in drones:
        drone_prefix = append_name(runtime["drone-prefix"], drone_id)
        routing.addOrigin(
            [ndn.net[node_name]],
            [drone_prefix, f"{drone_prefix}/KEY", group_prefix],
        )
    routing.calculateRoutes()
    for node in ndn.net.hosts:
        for prefix in (root_prefix, group_prefix, f"{group_prefix}/sync"):
            Nfdc.setStrategy(node, prefix, Nfdc.STRATEGY_MULTICAST)


def dump_uav_routes(ndn, args: argparse.Namespace, output_dir: Path) -> None:
    runtime = load_runtime_config(args.runtime_config)
    root_prefix = runtime["root-identity"]
    drone_prefixes = [append_name(runtime["drone-prefix"], drone_id)
                      for drone_id, _ in active_drones(args)]
    for node_name in [args.gs_node] + [node_name for _, node_name in active_drones(args)]:
        node = ndn.net[node_name]
        route_log = output_dir / f"{node_name}-nfd-routes.log"
        text = perf.node_cmd(
            node,
            "nfdc fib list | grep -E '({}|{})' || true".format(
                "|".join(shell_quote(prefix).strip("'") for prefix in drone_prefixes),
                shell_quote(root_prefix).strip("'")))
        route_log.write_text(text, encoding="utf-8")


def start(node, name: str, command: str, env: dict[str, str], output_dir: Path, processes):
    log_path = output_dir / f"{name}.log"
    log_file = log_path.open("wb")
    log(f"start {name} on {node.name}: {command}")
    proc = getPopen(node, command, envDict=env, shell=True,
                    stdout=log_file, stderr=subprocess.STDOUT)
    processes.append((proc, log_file, log_path))
    return proc, log_path


def mavlink_instance_port(base_port: str, instance: int) -> str:
    return str(int(base_port) + instance)


def start_jmavsim(ndn, args: argparse.Namespace, drone_node: str,
                  drone_id: str, instance: int, env: dict[str, str], output_dir: Path,
                  processes) -> tuple[subprocess.Popen, Path, Path]:
    px4_dir = Path(args.px4_dir).resolve()
    if not (px4_dir / "Tools/simulation/jmavsim/jmavsim_run.sh").exists():
        raise RuntimeError(f"PX4 jMAVSim script not found under {px4_dir}")
    px4_build = px4_dir / "build/px4_sitl_default"
    px4_bin = px4_build / "bin/px4"
    px4_etc = px4_build / "etc"
    instance_dir = px4_build / f"instance_{instance}"
    simulator_port = 4560 + instance
    status_file = output_dir / f"jmavsim-{drone_id}.status"
    status_file.write_text("starting\n", encoding="utf-8")
    sim_env = dict(env)
    sim_env["NDNSF_UAV_JMAVSIM_STATUS_FILE"] = str(status_file)
    headless_value = "1" if args.jmavsim_headless else ""
    # Do not invoke `make px4_sitl jmavsim` per drone: that target always starts
    # PX4 instance 0 and the jmavsim_iris init script kills other jMAVSim Java
    # processes. Run PX4 with an explicit instance id in external-simulator mode
    # and start the matching jMAVSim process ourselves.
    command = (
        f"cd {shell_quote(px4_dir)} && "
        f"export CMAKE_ARGS={shell_quote(args.px4_cmake_args)} && "
        f"if [ ! -x {shell_quote(px4_bin)} ]; then make px4_sitl_default; fi && "
        f"mkdir -p {shell_quote(instance_dir)} && "
        f"cd {shell_quote(instance_dir)} && "
        f"echo NDNSF_UAV_SIM_HOME lat={shell_quote(args.sim_home_lat)} "
        f"lon={shell_quote(args.sim_home_lon)} alt={shell_quote(args.sim_home_alt)} && "
        f"( export PX4_SIM_MODEL=none_iris SYS_AUTOSTART=10016 "
        f"PX4_HOME_LAT={shell_quote(args.sim_home_lat)} "
        f"PX4_HOME_LON={shell_quote(args.sim_home_lon)} "
        f"PX4_HOME_ALT={shell_quote(args.sim_home_alt)}; "
        f"trap 'kill ${{PX4_PID:-}} ${{JMAVSIM_PID:-}} 2>/dev/null || true; exit 0' INT TERM EXIT; "
        f"{shell_quote(px4_bin)} -i {instance} -d {shell_quote(px4_etc)} & "
        f"PX4_PID=$!; "
        f"sleep 1; "
        f"env HEADLESS={shell_quote(headless_value)} "
        f"{shell_quote(px4_dir / 'Tools/simulation/jmavsim/jmavsim_run.sh')} "
        f"-p {simulator_port} -l -r 250 & "
        f"JMAVSIM_PID=$!; "
        f"wait -n $PX4_PID $JMAVSIM_PID; "
        f"kill $PX4_PID $JMAVSIM_PID 2>/dev/null || true; "
        f"wait $PX4_PID $JMAVSIM_PID 2>/dev/null || true ) 2>&1 | "
        f"python3 {shell_quote(REPO / 'Experiments/uav_jmavsim_log_filter.py')}"
    )
    proc, log_path = start(ndn.net[drone_node], f"jmavsim-{drone_id}",
                           command, sim_env, output_dir, processes)
    return proc, log_path, status_file


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
    time.sleep(0.25)
    for pattern in patterns:
        subprocess.run(["pkill", "-KILL", "-f", pattern], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def should_start_jmavsim(args: argparse.Namespace) -> bool:
    if args.no_start_jmavsim:
        return False
    if args.start_jmavsim:
        return True
    # Interactive GUI demos should show the simulator with DroneAPP by default.
    # Non-interactive smoke tests stay light unless --start-jmavsim is explicit.
    return not args.no_cli


def stop(processes) -> None:
    for proc, log_file, _ in reversed(processes):
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=0.8)
            except Exception:
                proc.kill()
                try:
                    proc.wait(timeout=0.5)
                except Exception:
                    pass
        log_file.close()


def interrupt_when_process_exits(proc, label: str) -> None:
    def _watch() -> None:
        proc.wait()
        print(f"{label} exited; stopping MiniNDN demo and cleaning up.")
        os.kill(os.getpid(), signal.SIGINT)

    threading.Thread(target=_watch, daemon=True).start()


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
    runtime = load_runtime_config(args.runtime_config)
    if not os.environ.get("DISPLAY"):
        raise RuntimeError("DISPLAY is not set; run from a graphical session, e.g. sudo -E ...")

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    processes = []
    maybe_allow_x11(args)
    prefetch_default_map_tile()
    video_source, video_source_kind = select_camera_source(args, output_dir, processes)
    log(f"UAV video source selected: {video_source_kind} {video_source}")

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
        if args.auto_repeat_stop_test:
            gs_env["NDNSF_UAV_SIMULATE_STOP_DELAY_MS"] = "4000"

        controller_cmd = app_cmd(APP_CONTROLLER, [
            "--controller-prefix", runtime["controller-prefix"],
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
        simulator_status_files = {}
        if should_start_jmavsim(args):
            cleanup_px4_jmavsim(args.px4_dir)
        for drone_index, (drone_id, node_name) in enumerate(drones):
            start_simulator = should_start_jmavsim(args)
            if start_simulator:
                jmavsim_proc, jmavsim_log, status_file = start_jmavsim(
                    ndn, args, node_name, drone_id, drone_index,
                    drone_envs[drone_id], output_dir, processes)
                simulator_status_files[drone_id] = status_file
            mavlink_udp_port = mavlink_instance_port(args.mavlink_udp_port, drone_index)
            drone_app_config = app_config_path(args, f"drone-{drone_id}.conf")
            drone_cmd = app_cmd(APP_DRONE, [
                "--app-config", drone_app_config,
                "--runtime-config", str(Path(args.runtime_config).resolve()),
                "--drone-id", drone_id,
                "--video-source", video_source,
                "--flight-controller-backend",
                "udp" if start_simulator else args.flight_controller_backend,
                "--mavlink-udp-host", args.mavlink_udp_host,
                "--mavlink-udp-port", mavlink_udp_port,
                "--mavlink-udp-listen-port", args.mavlink_udp_listen_port,
            ])
            if args.auto_recording_playback_test:
                repo_path = output_dir / f"drone-{drone_id}-camera-recording.sqlite3"
                try:
                    repo_path.unlink()
                except FileNotFoundError:
                    pass
                drone_cmd += (
                    " --camera-capture-on-start"
                    " --camera-record-to-local-repo"
                    " --camera-record-repo-path " + shell_quote(repo_path) +
                    " --camera-record-chunk-limit 240"
                )
            if start_simulator:
                drone_cmd += " --fc-status-file " + shell_quote(simulator_status_files[drone_id])
                if not args.no_configure_px4_sitl_demo_params:
                    drone_cmd += " --configure-px4-sitl-demo-params"
            if args.drone_headless:
                drone_cmd += " --headless"
            label = "drone" if len(drones) == 1 else f"drone-{drone_id}"
            drone_proc, drone_log = start(ndn.net[node_name], label,
                                          drone_cmd, drone_envs[drone_id],
                                          output_dir, processes)
            drone_logs[drone_id] = drone_log
            drone_ready_markers = ["DRONE_HEADLESS_READY"] if args.drone_headless else ["DRONE_GUI_READY"]
            if not wait_log_any(drone_log, drone_ready_markers, 30, proc=drone_proc):
                mode = "headless runtime" if args.drone_headless else "GUI"
                raise RuntimeError(f"drone {drone_id} {mode} did not start; see {drone_log}")

            if (start_simulator and
                (args.auto_mavlink_test or args.auto_keyboard_test or
                 args.auto_telemetry_test or
                 args.auto_manual_control_test or args.auto_two_drone_switch_test or
                 args.auto_recording_playback_test or
                 args.auto_patrol_test or args.auto_single_mission_test)):
                if not wait_log_any(jmavsim_log, JMAVSIM_READY_MARKERS,
                                    args.jmavsim_ready_timeout_seconds,
                                    proc=jmavsim_proc):
                    raise RuntimeError(
                        f"PX4/jMAVSim did not become ready; see {jmavsim_log}\n"
                        f"--- tail ---\n{read_tail(jmavsim_log)}"
                    )

        gs_argv = [
            "--app-config", str(Path(args.gs_app_config).resolve())
            if args.gs_app_config else app_config_path(args, "ground-station.conf"),
            "--runtime-config", str(Path(args.runtime_config).resolve()),
            "--target-drone", args.drone_id,
            "--video-bitrate-kbps", str(args.video_bitrate_kbps),
            "--video-width", str(args.video_width),
            "--patrol-drones", ",".join(drone_id for drone_id, _ in drones),
            "--no-cert-dialog",
        ]
        if args.auto_video_test:
            gs_argv += [
                "--auto-video-test",
                "--auto-stop-seconds", str(args.auto_stop_seconds),
                "--auto-start-delay-ms", str(args.auto_start_delay_ms),
            ]
            if args.auto_repeat_stop_test:
                gs_argv += [
                    "--auto-repeat-stop-test",
                    "--timeout-ms", "2500",
                ]
        if args.auto_mavlink_test:
            gs_argv += ["--auto-mavlink-test"]
        if args.auto_telemetry_test:
            gs_argv += ["--auto-telemetry-test", "--timeout-ms", "30000"]
        if args.auto_link_state_test:
            gs_argv += [
                "--auto-link-state-test",
                "--link-stale-ms", str(args.link_stale_ms),
                "--link-lost-ms", str(args.link_lost_ms),
                "--lost-link-action", args.lost_link_action,
            ]
        if args.auto_keyboard_test:
            gs_argv += ["--auto-keyboard-test"]
        if args.auto_manual_control_test:
            gs_argv += ["--auto-manual-control-test"]
        if args.auto_two_drone_switch_test:
            gs_argv += ["--auto-two-drone-switch-test"]
        if args.auto_video_selection_test:
            gs_argv += [
                "--auto-video-selection-test",
                "--auto-start-delay-ms", str(args.auto_start_delay_ms),
            ]
        if args.auto_mission_controls_test:
            gs_argv += [
                "--auto-mission-controls-test",
                "--ack-timeout-ms", "700",
                "--timeout-ms", "3000",
            ]
        if args.auto_flight_controls_test:
            gs_argv += ["--auto-flight-controls-test"]
        if args.auto_recording_playback_test:
            gs_argv += [
                "--auto-recording-playback-test",
                "--ack-timeout-ms", "700",
                "--timeout-ms", "12000",
            ]
        if args.auto_patrol_test:
            patrol_timeout_ms = "30000" if should_start_jmavsim(args) else "3000"
            gs_argv += [
                "--auto-patrol-test",
                "--ack-timeout-ms", "700",
                "--timeout-ms", patrol_timeout_ms,
            ]
        if args.auto_single_mission_test:
            gs_argv += [
                "--auto-single-mission-test",
                "--ack-timeout-ms", "700",
                "--timeout-ms", "30000",
            ]
            if args.auto_single_mission_start_test:
                gs_argv += ["--auto-single-mission-start-test"]
        gs_proc, gs_log = start(ndn.net[args.gs_node], "ground-station",
                                app_cmd(APP_GS, gs_argv), gs_env, output_dir, processes)
        if not (args.auto_patrol_test or args.auto_single_mission_test or
                args.auto_telemetry_test or args.auto_link_state_test) and not wait_log(gs_log, "GS_GUI_READY", 30, gs_proc):
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
        if not args.no_cli:
            interrupt_when_process_exits(gs_proc, "ground-station GUI")

        if args.auto_patrol_test and args.no_cli:
            try:
                gs_proc.wait(timeout=70)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station patrol smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "PATROL_TASK_START")
            require_log(gs_log, "PATROL_TASK_DONE")
            if should_start_jmavsim(args):
                require_log(gs_log, "attempt=1 part=part0 provider=A")
                require_log(gs_log, "attempt=1 part=part1 provider=B")
                for drone_id, _ in drones[:2]:
                    require_log(drone_logs[drone_id], "UDP_FC_MISSION_ACK drone=" + drone_id + " result=accepted")
                print("NDNSF_UAV_PATROL_JMAVSIM_MININDN_SMOKE_OK")
            else:
                require_log(gs_log, "PATROL_PART_MISSING")
                require_log(gs_log, "PATROL_COMPENSATION")
                require_log(gs_log, "PATROL_PROGRESS MissionProgress")
                require_log(gs_log, "phase=waiting-compensation")
                require_log(gs_log, "phase=compensating")
                require_log(gs_log, "phase=completed")
                require_log(gs_log, "compensated_parts=1")
                require_log(gs_log, "return_home=true")
                require_log(gs_log, "attempt=1 part=part1 provider=B")
                require_log(gs_log, "attempt=2 part=part0 provider=B")
                require_log(drone_logs[drones[0][0]], "mission response delayed")
                print("NDNSF_UAV_PATROL_MININDN_SMOKE_OK")
        elif args.auto_single_mission_test and args.no_cli:
            try:
                gs_proc.wait(timeout=45)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station single mission smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "SINGLE_MISSION_START")
            require_log(gs_log, "SINGLE_MISSION_DONE")
            if args.auto_single_mission_start_test:
                require_log(gs_log, "SINGLE_MISSION_COMMAND command=start_mission ok=true")
            require_log(gs_log, "GS_SINGLE_MISSION_EXIT ok=true")
            print("NDNSF_UAV_SINGLE_MISSION_MININDN_SMOKE_OK")
        elif args.auto_telemetry_test and args.no_cli:
            try:
                gs_proc.wait(timeout=70)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station telemetry smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "TELEMETRY_LIVE_RESULT ok=true")
            require_log(gs_log, "GS_TELEMETRY_EXIT ok=true")
            require_log(gs_log, "gps_fix_name=")
            require_log(gs_log, "ekf_ready=true")
            require_log(gs_log, "landed_state_name=")
            require_log(gs_log, "battery_voltage_v=")
            require_log(gs_log, "armed=true")
            require_log(gs_log, "lat=")
            require_log(gs_log, "lon=")
            print("NDNSF_UAV_TELEMETRY_MININDN_SMOKE_OK")
        elif args.auto_link_state_test and args.no_cli:
            try:
                gs_proc.wait(timeout=45)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station link-state smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "LINK_STATE_AGING sample=initial")
            require_log(gs_log, "LINK_STATE_AGING sample=stale")
            require_log(gs_log, "LINK_STATE_AGING sample=lost")
            require_log(gs_log, "state=stale")
            require_log(gs_log, "state=lost")
            require_log(gs_log, "LINK_STATE_AGING_RESULT ok=true")
            require_log(gs_log, "GS_LINK_STATE_EXIT ok=true")
            print("NDNSF_UAV_LINK_STATE_MININDN_SMOKE_OK")
        elif args.auto_video_selection_test and args.no_cli:
            try:
                gs_proc.wait(timeout=75)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station video selection smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            drone_ids = [drone_id for drone_id, _ in drones]
            require_log(gs_log, "VIDEO_SELECTION_STATE phase=initial-first selected=" + drone_ids[0])
            require_log(gs_log, "VIDEO_SELECTION_STATE phase=second-after-first-streaming selected=" + drone_ids[1] + " can_start=true can_stop=false")
            require_log(gs_log, "VIDEO_SELECTION_STATE phase=first-streaming selected=" + drone_ids[0] + " can_start=false can_stop=true")
            require_log(gs_log, "VIDEO_SELECTION_STATE phase=after-stop selected=" + drone_ids[0] + " can_start=true can_stop=false")
            require_log(drone_logs[drone_ids[0]], "DRONE_STATUS drone=" + drone_ids[0] + " video streaming")
            require_log(drone_logs[drone_ids[0]], "DRONE_STATUS drone=" + drone_ids[0] + " video stopped")
            print("NDNSF_UAV_VIDEO_SELECTION_MININDN_SMOKE_OK")
        elif args.auto_mission_controls_test and args.no_cli:
            try:
                gs_proc.wait(timeout=70)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station mission controls smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "MISSION_CONTROL_STATE phase=initial can_upload=true can_start=false can_stop=false")
            require_log(gs_log, "MISSION_CONTROL_STATE phase=uploading can_upload=false can_start=false can_stop=false upload_pending=true")
            require_log(gs_log, "MISSION_CONTROL_STATE phase=after-upload can_upload=true can_start=false can_stop=true")
            require_log(gs_log, "start_blocked_count=2")
            require_log(gs_log, "start_blocked=A:waiting-heartbeat,B:waiting-heartbeat")
            require_log(gs_log, "MISSION_CONTROL_STATE phase=after-ready can_upload=true can_start=true can_stop=true")
            require_log(gs_log, "start_eligible_count=2")
            require_log(gs_log, "MISSION_CONTROL_STATE phase=progress-active can_upload=false can_start=false can_stop=true")
            require_log(gs_log, "progress_phase=compensating progress_active=true")
            require_log(gs_log, "DRONE_ROW_STATE phase=progress-active selected=A")
            require_log(gs_log, "mission_progress=compensating")
            require_log(gs_log, "progress=compensating")
            require_log(gs_log, "SELECTED_VIEW_STATE phase=progress-active selected=A")
            require_log_any(gs_log, ["marker=A P", "marker=A U P"])
            require_log(gs_log, "MISSION_CONTROL_STATE phase=progress-completed can_upload=true can_start=true can_stop=true")
            require_log(gs_log, "progress_phase=completed progress_active=false")
            require_log(gs_log, "DRONE_ROW_STATE phase=progress-completed selected=A")
            require_log(gs_log, "mission_progress=completed")
            require_log(gs_log, "progress=completed")
            require_log(gs_log, "SELECTED_VIEW_STATE phase=progress-completed selected=A")
            require_log_any(gs_log, ["marker=A C", "marker=A U C"])
            require_log(gs_log, "MISSION_CONTROL_STATE phase=final can_upload=true can_start=true can_stop=true")
            print("NDNSF_UAV_MISSION_CONTROLS_MININDN_SMOKE_OK")
        elif args.auto_flight_controls_test and args.no_cli:
            try:
                gs_proc.wait(timeout=45)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station flight controls smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "FLIGHT_ACTION_STATE phase=not-ready selected=")
            require_log(gs_log, "has_safety=true safety_attention=false link=connected manual_state=idle")
            require_log(gs_log, "can_arm=false arm_reason=waiting-heartbeat can_takeoff=false takeoff_reason=waiting-heartbeat")
            require_log(gs_log, "FLIGHT_ACTION_STATE phase=ready-unarmed selected=")
            require_log(gs_log, "can_arm=true arm_reason=ok can_takeoff=false takeoff_reason=not-armed")
            require_log(gs_log, "FLIGHT_ACTION_STATE phase=armed-ready selected=")
            require_log(gs_log, "can_arm=false arm_reason=already-armed can_takeoff=true takeoff_reason=ok can_land=true land_reason=ok can_manual=true")
            require_log(gs_log, "SELECTED_ACTION_STATE phase=not-ready selected=")
            require_log(gs_log, "SELECTED_ACTION_STATE phase=manual-enabled selected=")
            require_log(gs_log, "can_manual=true can_panel=true mission_can_start=false mission_can_stop=false")
            require_log(gs_log, "manual_mode=true manual_active=false emergency_stop=true")
            require_log(gs_log, "SELECTED_ACTION_STATE phase=mission-uploaded selected=")
            require_log(gs_log, "mission_can_start=true mission_can_stop=true mission_phases=A:uploaded")
            require_log(gs_log, "SELECTED_VIEW_STATE phase=not-ready selected=")
            require_log(gs_log, "readiness=not-ready mission=idle mission_progress=idle video=stopped")
            require_log(gs_log, "SELECTED_VIEW_STATE phase=mission-uploaded selected=")
            require_log(gs_log, "readiness=ready mission=uploaded mission_progress=idle video=stopped")
            require_log(gs_log, "marker=A U")
            require_log(gs_log, "DRONE_ROW_STATE phase=not-ready selected=A")
            require_log(gs_log, "readiness=not-ready armed=false gps=false mission=idle video=stopped")
            require_log(gs_log, "DRONE_ROW_STATE phase=mission-uploaded selected=A")
            require_log(gs_log, "readiness=ready armed=false gps=true mission=uploaded video=stopped")
            print("NDNSF_UAV_FLIGHT_CONTROLS_MININDN_SMOKE_OK")
        elif (args.auto_mavlink_test or args.auto_keyboard_test or
              args.auto_manual_control_test or args.auto_two_drone_switch_test) and args.no_cli:
            try:
                gs_proc.wait(timeout=70)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station MAVLink smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            if not args.auto_two_drone_switch_test:
                require_log(gs_log, "GS_TARGETED_RESPONSE service=/UAV/MAVLink/Execute")
                require_log(gs_log, "MAVLink arm drone=" + args.drone_id + " accepted=true")
                require_log(gs_log, "MAVLink takeoff drone=" + args.drone_id + " accepted=true")
                require_log(gs_log, "MAVLink land drone=" + args.drone_id + " accepted=true")
            if args.auto_manual_control_test:
                require_log(gs_log, "MAVLink manual_control drone=" + args.drone_id + " accepted=true")
            if args.auto_two_drone_switch_test:
                drone_ids = [drone_id for drone_id, _ in drones]
                require_log(gs_log, "Selected drone " + drone_ids[1])
                require_log(gs_log, "Selected drone " + drone_ids[0])
                for drone_id in drone_ids[:2]:
                    require_log(gs_log, "MAVLink manual_control drone=" + drone_id + " accepted=true")
                    require_log_any(drone_logs[drone_id], [
                        "MOCK_FC_FORWARD drone=" + drone_id,
                        "MAVLINK_FC_FORWARD drone=" + drone_id,
                    ])
                print("NDNSF_UAV_TWO_DRONE_SWITCH_MININDN_SMOKE_OK")
            else:
                require_log_any(drone_logs[args.drone_id], [
                    "MOCK_FC_FORWARD drone=" + args.drone_id,
                    "MAVLINK_FC_FORWARD drone=" + args.drone_id,
                ])
                print("NDNSF_UAV_MAVLINK_TARGETED_MININDN_SMOKE_OK")
        elif args.auto_recording_playback_test and args.no_cli:
            try:
                gs_proc.wait(timeout=75)
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station recording playback smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "Recording manifest drone=" + args.drone_id)
            require_log(gs_log, "Recording playback drone=" + args.drone_id)
            require_log(gs_log, "Recording playback fetched drone=" + args.drone_id)
            require_log(gs_log, "GS_DECODED_FRAMES count=1")
            require_log(drone_logs[args.drone_id], "camera_recording=on")
            print("NDNSF_UAV_RECORDING_PLAYBACK_MININDN_SMOKE_OK")
        elif args.auto_video_test and args.no_cli:
            try:
                gs_proc.wait(timeout=max(45, args.auto_stop_seconds + 35))
            except subprocess.TimeoutExpired as e:
                raise RuntimeError(f"ground station auto smoke did not finish; see {gs_log}") from e
            if gs_proc.returncode != 0:
                raise RuntimeError(f"ground station exited with {gs_proc.returncode}; see {gs_log}")
            require_log(gs_log, "GS_STATUS Video packet stream")
            require_log(gs_log, "GS_VIDEO_ADAPTIVE_STATE reason=configured VideoAdaptive drone=" + args.drone_id)
            require_log(gs_log, "VIDEO_ADAPTIVE_VIEW_STATE phase=auto-video-active selected=" + args.drone_id + " has_adaptive=true")
            require_log(gs_log, "window=")
            require_log(gs_log, "missing_timeout_ms=")
            require_log(gs_log, "suggested_bitrate_kbps=")
            require_log(gs_log, "bitrate_action=")
            require_log(gs_log, "GS_DECODED_FRAMES count=30")
            require_log(gs_log, "GS_GUI_EXIT rc=0")
            require_log(gs_log, "VIDEO_ADAPTIVE_VIEW_STATE phase=auto-video-stopped selected=" + args.drone_id + " has_adaptive=true")
            require_log(drone_logs[args.drone_id], "DRONE_STATUS drone=" + args.drone_id + " video streaming")
            require_log(drone_logs[args.drone_id], "DRONE_STATUS drone=" + args.drone_id + " video stopped")
            if args.drone_headless:
                require_log(drone_logs[args.drone_id], "DRONE_HEADLESS_STATUS")
                require_log(drone_logs[args.drone_id], "video=stopped")
            if args.auto_repeat_stop_test:
                require_log(gs_log, "Video stop timed out for drone " + args.drone_id)
                require_log(gs_log, "Video stopped drone=" + args.drone_id)
                require_log(drone_logs[args.drone_id],
                            "DRONE_VIDEO_STOP_SIMULATED_DELAY_MS drone=" + args.drone_id)
            print("NDNSF_UAV_GUI_MININDN_SMOKE_OK")
        elif args.no_cli:
            while True:
                time.sleep(1)
        else:
            MiniNDNCLI(ndn.net)
        return 0
    finally:
        stop(processes)
        if 'args' in locals() and should_start_jmavsim(args):
            cleanup_px4_jmavsim(args.px4_dir)
        if ndn is not None:
            ndn.stop()
        Minindn.cleanUp()


if __name__ == "__main__":
    raise SystemExit(main())
