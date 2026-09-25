#!/usr/bin/env python3
"""Prepare and run the Spec191 five-node UAV tracking demonstration.

The command is intentionally offline by default. ``--prepare`` creates only a
small manifest and process plan; it never downloads videos/models or writes
private keys. ``--run`` requires a fully built native application and root
MiniNDN privileges, then runs the exact manifest through MiniNDN/NFD. It does
not fall back to a host-process launch because that cannot prove network
faces, routes, or node isolation.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import shlex
import shutil
import subprocess
import sys
import time
from typing import Any, Iterable, Sequence

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE = Path(os.environ.get(
    "NDNSF_SPEC191_ASSET_ROOT", "/home/tianxing/.cache/ndnsf/spec191-assets"))
DEFAULT_MODEL = DEFAULT_SOURCE / "3UAVs.pt"
DEFAULT_OUTPUT = ROOT / "results/uav-tracking/spec191-local"
TOPOLOGY = Path(__file__).with_name("topology.conf")
REQUIRED_NATIVE_PROGRAMS = (
    "UavTrackingNode",
    "App_ServiceController",
    "UavTrackingGroundStation",
)
DEFAULT_TRACKING_LIBEXEC = Path(
    os.environ.get("NDNSF_UAV_TRACKING_LIBEXEC",
                   "/usr/local/libexec/ndnsf-uav/tracking"))

sys.path.insert(0, str(Path(__file__).parent))
from tracking_artifacts import ArtifactError, validate_real_assets  # noqa: E402
from tracking_topology import TopologyError, load_topology  # noqa: E402
from motion_fixture import MOTION_PROFILES, write_fixture  # noqa: E402


class RunnerError(RuntimeError):
    """Raised when setup or lifecycle preconditions fail."""


@dataclass(frozen=True)
class ProcessSpec:
    name: str
    node: str
    identity: str
    cwd: Path
    command: tuple[str, ...]
    private_dir: Path

    def public_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "node": self.node,
            "identity": self.identity,
            "cwd": str(self.cwd),
            "command": list(self.command),
            # This is a directory contract, not a key or credential.
            "privateDir": str(self.private_dir),
        }


def _sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _effective_paths(source: Path, model: Path, output: Path) -> tuple[Path, Path, Path]:
    source = source.expanduser().resolve()
    model = model.expanduser().resolve()
    output = output.expanduser().resolve()
    if not source.is_dir():
        raise RunnerError(f"source directory is not available: {source}")
    if not model.is_file():
        raise RunnerError(f"model checkpoint is not available: {model}")
    output.mkdir(parents=True, exist_ok=True)
    return source, model, output


def _installed_program(name: str) -> str:
    """Resolve a program from the installed PATH, never from the checkout."""
    return shutil.which(name) or name


def _installed_tracking_script(name: str) -> Path:
    return DEFAULT_TRACKING_LIBEXEC / name


FAULTS = frozenset({"none", "missing-view", "bad-content", "bad-identity", "stale-epoch",
                    "worker-failure", "response-loss", "late-response-loss", "cancel", "display-failure",
                    "display-loss-after-ready", "window-close", "eos"})


def _process_specs(root: Path, output: Path, *, source: Path, model: Path,
                   headless: bool, window_count: int, fault: str,
                   request_timeout_ms: int, motion_dir: Path) -> list[ProcessSpec]:
    common = ("--run-id", output.name, "--topology", str(TOPOLOGY),
              "--window-count", str(window_count))
    display = "headless" if headless else "windows"
    fault_args = () if fault == "none" else ("--fault", fault)
    controller = _installed_program("App_ServiceController")
    ground_station = _installed_program("UavTrackingGroundStation")
    tracking_node = _installed_program("UavTrackingNode")
    display_runner = str(_installed_tracking_script("tracking_display.py"))
    worker_root = output / "private/compute/worker"
    return [
        # Controller and GS are separate identities/processes in the same
        # MiniNDN node. Their private application state stays separate, while
        # the launcher supplies one campaign-scoped PIB/TPM below so the
        # Controller can resolve and encrypt to the exact certificates used by
        # the providers and ground station.
        ProcessSpec("controller", "gs", "/example/uav/controller", root,
                    (controller, "--node", "gs", "--controller-prefix",
                     "/example/uav/controller", "--policy-file",
                     str(root / "Experiments/UAV/spec191.policies"),
                     "--trust-schema", "examples/trust-any.conf",
                     "--ensure-identities",
                     "/example/uav/drone/UAV1,/example/uav/drone/UAV2,"
                     "/example/uav/drone/UAV3,/example/uav/compute,/example/uav/gs",
                     *common),
                    output / "private/controller"),
        ProcessSpec("ground-station", "gs", "/example/uav/gs", root,
                    (ground_station, "--node", "gs", "--display", display,
                     "--group-prefix", "/example/uav", "--controller-prefix",
                     "/example/uav/controller", "--trust-schema",
                     "examples/trust-any.conf", "--motion-dir", str(motion_dir),
                     "--request-timeout-ms",
                     str(request_timeout_ms), *common, *fault_args),
                    output / "private/ground-station"),
        ProcessSpec("uav1-camera", "uav1", "/example/uav/drone/UAV1", root,
                    (tracking_node, "--role", "source", "--camera", "UAV1",
                     "--video-source", str(source / "uav1_1min.mp4"),
                     "--frame-dir", str(output / "private/frames/UAV1"),
                     "--motion-dir", str(motion_dir),
                     "--model", str(model), *common, *fault_args),
                    output / "private/uav1"),
        ProcessSpec("uav2-camera", "uav2", "/example/uav/drone/UAV2", root,
                    (tracking_node, "--role", "source", "--camera", "UAV2",
                     "--video-source", str(source / "uav2_1min.mp4"),
                     "--frame-dir", str(output / "private/frames/UAV2"),
                     "--motion-dir", str(motion_dir),
                     "--model", str(model), *common, *fault_args),
                    output / "private/uav2"),
        ProcessSpec("uav3-camera", "uav3", "/example/uav/drone/UAV3", root,
                    (tracking_node, "--role", "source", "--camera", "UAV3",
                     "--video-source", str(source / "uav3_1min.mp4"),
                     "--frame-dir", str(output / "private/frames/UAV3"),
                     "--motion-dir", str(motion_dir),
                     "--model", str(model), *common, *fault_args),
                    output / "private/uav3"),
        ProcessSpec("compute", "compute", "/example/uav/compute", root,
                    (tracking_node, "--role", "compute", "--display", display,
                     "--model", str(model), "--worker-script",
                     str(_installed_tracking_script("tracking_worker.py")),
                     "--motion-dir", str(motion_dir),
                     *common, *fault_args),
                    output / "private/compute"),
        ProcessSpec("compute-display", "compute", "/example/uav/compute/display", root,
                    (sys.executable, display_runner, "--socket",
                     str(output / "private/compute-display/display.sock"),
                     "--mode", display, "--replay-json",
                     str(worker_root / "tracking-result.json"), "--replay-dir",
                     str(worker_root / "annotated"), *fault_args),
                    output / "private/compute-display"),
    ]


def _check_python_dependencies() -> list[str]:
    missing: list[str] = []
    for module in ("cv2", "numpy", "ultralytics", "lap"):
        try:
            __import__(module)
        except Exception as error:  # pragma: no cover - host-dependent
            missing.append(f"python:{module}:{type(error).__name__}")
    return missing


def _child_pythonpath() -> str:
    """Preserve the launcher's Python runtime after HOME isolation.

    MiniNDN application processes use private HOME/XDG roots so that one
    campaign cannot reuse another campaign's caches or credentials.  Python's
    user-site directory is derived from HOME, however; changing HOME would
    otherwise hide packages (for example cv2 and ultralytics) that the
    launcher already verified.  Export the launcher's real import roots
    explicitly instead of relying on the child's user-site discovery.
    """
    paths: list[str] = []
    for item in sys.path:
        if not item:
            continue
        candidate = str(Path(item).resolve())
        if Path(candidate).is_dir() and candidate not in paths:
            paths.append(candidate)
    inherited = os.environ.get("PYTHONPATH", "").strip()
    if inherited:
        for item in inherited.split(os.pathsep):
            if item and item not in paths:
                paths.append(item)
    return os.pathsep.join(paths)


def build_plan(*, source: Path, model: Path, output: Path, license_text: str,
               headless: bool = False, run_seconds: float = 60.0,
               window_count: int = 3, fault: str = "none",
               global_deadline_ms: int = 60000,
               motion_fixture: Path | None = None,
               motion_profile: str = "nominal") -> dict[str, Any]:
    source, model, output = _effective_paths(source, model, output)
    try:
        topology = load_topology(TOPOLOGY)
    except (OSError, TopologyError) as error:
        raise RunnerError(f"topology preflight failed: {error}") from error
    try:
        passport = validate_real_assets(source, model, license_text)
    except (ArtifactError, OSError, json.JSONDecodeError) as error:
        raise RunnerError(f"artifact preflight failed: {error}") from error
    if run_seconds <= 0 or window_count <= 0 or window_count > 60:
        raise RunnerError("run_seconds must be positive and window_count must be in 1..60")
    if global_deadline_ms <= 1000:
        raise RunnerError("global_deadline_ms must exceed the 1-second ACK timeout")
    if fault not in FAULTS:
        raise RunnerError(f"unsupported Spec191 fault: {fault}")
    if motion_profile not in MOTION_PROFILES:
        raise RunnerError(f"unsupported Spec191 motion profile: {motion_profile}")
    motion_dir = (motion_fixture.expanduser().resolve() if motion_fixture is not None
                  else output / "private/motion")
    if motion_fixture is not None:
        motion_manifest_path = motion_dir / "manifest.json"
        if not motion_manifest_path.is_file():
            raise RunnerError(f"motion fixture manifest is not available: {motion_manifest_path}")
        motion_manifest = json.loads(motion_manifest_path.read_text(encoding="utf-8"))
        if motion_manifest.get("schema") != "spec191-motion-fixture-v1":
            raise RunnerError("motion fixture schema is not Spec191 v1")
        if int(motion_manifest.get("windowCount", 0)) < window_count:
            raise RunnerError("motion fixture has fewer windows than requested")
    else:
        motion_manifest = write_fixture(motion_dir, window_count, profile=motion_profile)
    motion_manifest_digest = _sha256_bytes((motion_dir / "manifest.json").read_bytes())
    specs = _process_specs(ROOT, output, source=source, model=model,
                           headless=headless, window_count=window_count, fault=fault,
                           request_timeout_ms=global_deadline_ms, motion_dir=motion_dir)
    nodes = sorted(topology.nodes)
    if {item.node for item in specs} != set(nodes):
        raise RunnerError("process plan does not cover the exact topology nodes")
    if len(nodes) != 5 or "controller" in nodes:
        raise RunnerError("Controller must remain a gs-local process, not a MiniNDN node")
    return {
        "schema": "spec191-uav-tracking-run/v1",
        "mode": "headless" if headless else "windows",
        "topology": {
            "path": str(TOPOLOGY),
            "nodes": nodes,
            "links": [list(link) for link in sorted(topology.links)],
            "controllerHost": topology.controller_host,
            "computeHost": topology.compute_host,
        },
        "assets": passport,
        "motion": {
            "schema": motion_manifest.get("schema"),
            "provenance": motion_manifest.get("provenance", "simulated-input"),
            "profile": motion_manifest.get("profile", "nominal"),
            "windowCount": int(motion_manifest.get("windowCount", 0)),
            "manifestDigest": motion_manifest_digest,
            "directory": str(motion_dir),
        },
        "processes": [item.public_dict() for item in specs],
        "settings": {
            "sampleFps": 2.0,
            "sourceSeconds": 60.0,
            "windowSeconds": 1.0,
            "windowCount": window_count,
            "fault": fault,
            "ackTimeoutMs": 1000,
            "globalDeadlineMs": global_deadline_ms,
            "runSeconds": run_seconds,
            "retryReselection": False,
            "cpuOnly": True,
        },
        "scientificAccuracyClaimAllowed": False,
    }


def _write_json(path: Path, value: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _prepare_source_frame_files(source: Path, output: Path, *, window_count: int) -> dict[str, list[str]]:
    """Materialize a bounded source-owned JPEG sequence per camera before launch.

    The native source Provider reads only these run-scoped replay frames. The
    full videos remain in the immutable asset cache; compute never receives
    their paths. Every window is still published and fetched through NDNSF.
    """
    import cv2

    frame_dir = output / "private/frames"
    frame_dir.mkdir(parents=True, exist_ok=True)
    if window_count <= 0 or window_count > 60:
        raise RunnerError("window_count must be in the range 1..60")
    records: dict[str, list[str]] = {}
    sampling: dict[str, dict[str, Any]] = {}
    for camera in ("UAV1", "UAV2", "UAV3"):
        video = source / f"uav{camera[-1]}_1min.mp4"
        capture = cv2.VideoCapture(str(video))
        camera_dir = frame_dir / camera
        camera_dir.mkdir(parents=True, exist_ok=True)
        paths: list[str] = []
        try:
            fps = float(capture.get(cv2.CAP_PROP_FPS))
            if not math.isfinite(fps) or fps <= 0.0:
                raise RunnerError(f"video has no usable FPS metadata for {camera}: {video}")
            sampling[camera] = {"fps": fps, "periodUs": 1_000_000,
                               "frameIndices": []}
            decoded_index = -1
            for window in range(window_count):
                target_index = int(round(window * fps))
                while decoded_index < target_index:
                    ok, frame = capture.read()
                    if not ok or frame is None:
                        raise RunnerError(
                            f"cannot decode 1-second replay frame {window} "
                            f"(source index {target_index}) for {camera}: {video}")
                    decoded_index += 1
                target = camera_dir / f"window-{window}.jpg"
                temporary = target.with_suffix(".jpg.tmp")
                encoded, data = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 85])
                if not encoded or data.nbytes == 0:
                    raise RunnerError(f"cannot encode replay frame {window} for {camera}")
                temporary.write_bytes(data.tobytes())
                os.replace(temporary, target)
                paths.append(str(target))
                sampling[camera]["frameIndices"].append(target_index)
        finally:
            capture.release()
        records[camera] = paths
    _write_json(output / "source-frame-inputs.json", {
        "schema": "spec191-source-frame-inputs/v2",
        "frames": records,
        "windowCount": window_count,
        "sampling": sampling,
        "note": "bounded source-owned replay frames sampled at one-second PTS intervals; every frame is published through NDNSF",
    })
    return records


def preflight(plan: dict[str, Any], *, run: bool, headless: bool) -> dict[str, Any]:
    errors: list[str] = []
    errors.extend(_check_python_dependencies())
    if run:
        try:
            import minindn  # noqa: F401
        except Exception as error:  # pragma: no cover - host dependent
            errors.append(f"MiniNDN import unavailable: {type(error).__name__}: {error}")
        errors.extend(
            f"missing installed native binary/program: {name}"
            for name in REQUIRED_NATIVE_PROGRAMS
            if shutil.which(name) is None)
        errors.extend(
            f"missing installed tracking script: {path}"
            for path in (_installed_tracking_script("tracking_worker.py"),
                         _installed_tracking_script("tracking_display.py"),
                         _installed_tracking_script("motion_estimator.py"),
                         _installed_tracking_script("motion_schema.py"))
            if not path.is_file())
        if os.geteuid() != 0:
            errors.append("MiniNDN run requires root privileges")
        if not headless and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
            errors.append("windows display requested but DISPLAY/WAYLAND_DISPLAY is unavailable")
    return {
        "schema": "spec191-uav-preflight/v1",
        "ok": not errors,
        "errors": errors,
        "run": run,
        "display": "headless" if headless else "windows",
        "nodes": plan["topology"]["nodes"],
        "processCount": len(plan["processes"]),
    }


class ProcessSupervisor:
    """Own child processes and guarantee bounded, run-scoped cleanup."""

    def __init__(self, output: Path, terminate_timeout: float = 5.0) -> None:
        self.output = output
        self.terminate_timeout = terminate_timeout
        self.processes: list[tuple[str, subprocess.Popen[str], Any]] = []

    def start(self, spec: ProcessSpec, env: dict[str, str]) -> subprocess.Popen[str]:
        log = (self.output / f"{spec.name}.log").open("w", encoding="utf-8")
        cwd = spec.cwd.resolve()
        command = list(spec.command)
        executable = shutil.which(command[0], path=env.get("PATH"))
        if executable is None:
            log.close()
            raise RunnerError(f"executable not found for {spec.name}: {command[0]}")
        command[0] = executable
        child_env = dict(env)
        spec.private_dir.mkdir(parents=True, exist_ok=True)
        # Keep process-owned configuration/cache roots separate even when the
        # Controller and GS share the same MiniNDN node.  The native apps may
        # additionally use their own key-chain flags; these environment roots
        # are the launcher-level isolation floor and are recorded in the
        # manifest for post-run verification.
        child_env["NDNSF_SPEC191_PRIVATE_DIR"] = str(spec.private_dir)
        child_env["HOME"] = str(spec.private_dir / "home")
        child_env["XDG_CONFIG_HOME"] = str(spec.private_dir / "config")
        child_env["XDG_DATA_HOME"] = str(spec.private_dir / "data")
        child_env["XDG_CACHE_HOME"] = str(spec.private_dir / "cache")
        for name in ("home", "config", "data", "cache"):
            (spec.private_dir / name).mkdir(parents=True, exist_ok=True)
        process = subprocess.Popen(command, cwd=str(cwd), env=child_env, stdout=log,
                                   stderr=subprocess.STDOUT, text=True,
                                   start_new_session=True)
        self.processes.append((spec.name, process, log))
        return process

    def stop_all(self) -> None:
        for _name, process, _log in reversed(self.processes):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        deadline = time.monotonic() + self.terminate_timeout
        while time.monotonic() < deadline and any(p.poll() is None for _, p, _ in self.processes):
            time.sleep(0.05)
        for _name, process, _log in reversed(self.processes):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        for _name, _process, log in self.processes:
            log.close()


def _run_minindn(plan: dict[str, Any], output: Path, *, wall_timeout_seconds: float) -> int:
    """Run the exact five-node topology through MiniNDN/NFD.

    The host launcher is intentionally not a qualification path: it cannot
    prove that the NDN faces, routes, or process-local private roots are wired
    correctly.  This function fails closed when MiniNDN cannot be imported or
    a required readiness/result marker is absent.
    """
    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn
    from minindn.util import getPopen

    output.mkdir(parents=True, exist_ok=True)
    saved_argv = sys.argv
    sys.argv = [saved_argv[0]]
    try:
        ndn = Minindn(topoFile=str(TOPOLOGY), workDir=str(output / "minindn"))
    finally:
        sys.argv = saved_argv

    processes: list[tuple[str, Any, Any]] = []
    nfd_apps: list[Any] = []
    started = time.monotonic()

    def launch(spec: ProcessSpec) -> Any:
        node = ndn.net[spec.node]
        log_path = output / f"{spec.name}.log"
        log = log_path.open("wb")
        private = spec.private_dir.resolve()
        shared_keychain = (output / "private" / "keychain").resolve()
        generation_state = (output / "private" / "controller" /
                            "controller-generation.state").resolve()
        mpl_config = private / "mpl"
        yolo_config = private / "config" / "Ultralytics"
        for name in ("home", "config", "data", "cache", "pib", "tpm"):
            (private / name).mkdir(parents=True, exist_ok=True)
        mpl_config.mkdir(parents=True, exist_ok=True)
        yolo_config.mkdir(parents=True, exist_ok=True)
        for name in ("pib", "tpm"):
            (shared_keychain / name).mkdir(parents=True, exist_ok=True)
        socket = Path(node.params["params"]["homeDir"]) / f"{node.name}.sock"
        child_ndn_log = os.environ.get("SPEC191_CHILD_NDN_LOG", "*=WARN").strip() or "*=WARN"
        child_pythonpath = _child_pythonpath()
        env = [
            f"export NDNSF_SPEC191_PRIVATE_DIR={shlex.quote(str(private))}",
            f"export HOME={shlex.quote(str(private / 'home'))}",
            f"export XDG_CONFIG_HOME={shlex.quote(str(private / 'config'))}",
            f"export XDG_DATA_HOME={shlex.quote(str(private / 'data'))}",
            f"export XDG_CACHE_HOME={shlex.quote(str(private / 'cache'))}",
            f"export MPLCONFIGDIR={shlex.quote(str(mpl_config))}",
            f"export YOLO_CONFIG_DIR={shlex.quote(str(yolo_config))}",
            # A campaign-scoped keychain is required for cross-process
            # certificate resolution. Application scratch remains process
            # private; this does not merge MiniNDN nodes or NFD transports.
            f"export NDN_CLIENT_PIB={shlex.quote('pib-sqlite3:' + str(shared_keychain / 'pib'))}",
            f"export NDN_CLIENT_TPM={shlex.quote('tpm-file:' + str(shared_keychain / 'tpm'))}",
            # Do not reuse ServiceController's process-global /tmp state lock
            # across campaigns. The generation lease is campaign-scoped and
            # therefore remains auditable and cannot be poisoned by an older
            # interrupted MiniNDN run.
            f"export NDNSF_CONTROLLER_GENERATION_STATE={shlex.quote(str(generation_state))}",
            f"export NDN_CLIENT_TRANSPORT={shlex.quote('unix://' + str(socket))}",
            # Installed binaries carry the $ORIGIN/.. RPATH.  Keep only the
            # system installation closure here; a checkout/build path would
            # violate the MiniNDN runtime boundary.
            f"export LD_LIBRARY_PATH={shlex.quote(os.environ.get('NDNSF_UAV_LIBRARY_PATH', '/usr/local/lib'))}",
            # Keep application logging separate from MiniNDN/NFD startup. A
            # global NDN_LOG would be inherited by NFD and can make NFD reject
            # an application logger selector as malformed configuration.
            f"export NDN_LOG={shlex.quote(child_ndn_log)}",
            f"export NDNSF_SPEC191_RUN_ROOT={shlex.quote(str(output))}",
        ]
        if child_pythonpath:
            env.append(f"export PYTHONPATH={shlex.quote(child_pythonpath)}")
        command = "; ".join(env + [
            f"cd {shlex.quote(str(spec.cwd.resolve()))}",
            f"exec {shlex.join(list(spec.command))}",
        ])
        process = getPopen(node, command, shell=True, stdout=log, stderr=subprocess.STDOUT)
        processes.append((spec.name, process, log))
        return process

    def wait_for(name: str, markers: tuple[str, ...], timeout: float) -> str:
        path = output / f"{name}.log"
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
            for marker in markers:
                if marker in text:
                    return marker
            for process_name, process, _log in processes:
                if process_name == name and process.poll() is not None:
                    raise RunnerError(
                        f"{name} exited before readiness marker; returncode={process.returncode}")
                if name != "compute-display" and process_name == "compute-display" and \
                        process.poll() is not None:
                    raise RunnerError(
                        "compute-display exited before campaign completion; "
                        f"returncode={process.returncode}")
            time.sleep(0.1)
        raise RunnerError(f"{name} did not emit any marker {markers!r}")

    def configure_ndn() -> None:
        ndn.start()
        app_nodes = list(ndn.net.hosts)
        if {node.name for node in app_nodes} != set(plan["topology"]["nodes"]):
            raise RunnerError("MiniNDN host set does not match the five-node manifest")
        for node in app_nodes:
            nfd = Nfd(node)
            home_dir = Path(node.params["params"]["homeDir"])
            old_socket = f"/run/nfd/{node.name}.sock"
            new_socket = str(home_dir / f"{node.name}.sock")
            conf_path = Path(nfd.confFile)
            conf_path.write_text(conf_path.read_text(encoding="utf-8").replace(
                old_socket, new_socket), encoding="utf-8")
            Path(nfd.clientConf).write_text(f"transport=unix://{new_socket}\n",
                                            encoding="utf-8")
            nfd.start()
            nfd_apps.append(nfd)
        for node in app_nodes:
            socket = Path(node.params["params"]["homeDir"]) / f"{node.name}.sock"
            deadline = time.monotonic() + 10
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            if not socket.exists():
                raise RunnerError(f"NFD socket not ready: {socket}")
        time.sleep(1.0)
        created_faces = ndn.setupFaces()
        face_target_ips = {
            (node.name, peer_name): peer_ip
            for node, links in created_faces.items()
            for peer_name, peer_ip, _cost in links
        }
        next_hops = {
            "uav1": ["compute"], "uav2": ["compute"], "uav3": ["compute"],
            "compute": ["uav1", "uav2", "uav3", "gs"], "gs": ["compute"],
        }
        route_records = []
        for node in app_nodes:
            transport = f"unix://{Path(node.params['params']['homeDir']) / f'{node.name}.sock'}"
            for peer_name in next_hops[node.name]:
                peer_ip = face_target_ips[(node.name, peer_name)]
                face_list = node.cmd(f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} nfdc face list")
                match = re.search(
                    rf"faceid=(\d+)\s+remote=(?:udp|udp4)://{re.escape(peer_ip)}:6363",
                    face_list)
                if match is None:
                    raise RunnerError(
                        f"NFD face missing node={node.name} peer={peer_name}: {face_list!r}")
                face_id = match.group(1)
                command = (f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} nfdc route add "
                           f"/example/uav nexthop {face_id}")
                route_records.append({"node": node.name, "peer": peer_name,
                                      "faceId": int(face_id), "command": command,
                                      "result": node.cmd(command)})
            strategy = (f"NDN_CLIENT_TRANSPORT={shlex.quote(transport)} nfdc strategy set "
                        "/example/uav /localhost/nfd/strategy/multicast")
            route_records.append({"node": node.name, "peer": None, "command": strategy,
                                  "result": node.cmd(strategy)})
        _write_json(output / "routes.json", {"schema": "spec191-routes/v1",
                                              "routes": route_records})

    try:
        configure_ndn()
        specs = [ProcessSpec(item["name"], item["node"], item["identity"],
                             Path(item["cwd"]), tuple(item["command"]),
                             Path(item["privateDir"])) for item in plan["processes"]]
        by_name = {spec.name: spec for spec in specs}
        launch(by_name["controller"])
        # The process-start marker is not sufficient: App_ServiceController
        # publishes NDNSF_CONTROLLER_READY only after prefix registration and
        # its PUBPARAMS readiness barrier have completed.
        wait_for("controller", ("NDNSF_CONTROLLER_READY",), 45)
        for name in ("uav1-camera", "uav2-camera", "uav3-camera", "compute"):
            launch(by_name[name])
        for name in ("uav1-camera", "uav2-camera", "uav3-camera", "compute"):
            wait_for(name, ("SPEC191_TRACKING_NODE_READY",), 60)
        launch(by_name["compute-display"])
        wait_for("compute-display", ("DISPLAY_READY",), 30)
        launch(by_name["ground-station"])
        wait_for("ground-station", ("SPEC191_TRACKING_GS_READY",), 30)
        result_marker = wait_for("ground-station",
                                 ("SPEC191_TRACKING_RESULT status=success",
                                  "SPEC191_TRACKING_RESULT status=failure",
                                  "SPEC191_TRACKING_PLAN_FAILED",
                                  "SPEC191_TRACKING_TIMEOUT",
                                  "SPEC191_TRACKING_CANCELLED"),
                                 wall_timeout_seconds)
        success = result_marker == "SPEC191_TRACKING_RESULT status=success"
        _write_json(output / "lifecycle.json", {
            "schema": "spec191-uav-lifecycle/v2",
            "terminal": success,
            "outcome": "collaboration-result" if success else "collaboration-failed",
            "marker": result_marker,
            "elapsedSeconds": round(time.monotonic() - started, 3),
        })
        return 0 if success else 1
    except Exception as error:
        _write_json(output / "lifecycle.json", {
            "schema": "spec191-uav-lifecycle/v2",
            "terminal": False,
            "outcome": "launcher-failed",
            "error": f"{type(error).__name__}: {error}",
        })
        return 1
    finally:
        for _name, process, _log in reversed(processes):
            if process.poll() is None:
                try:
                    process.send_signal(signal.SIGINT)
                    process.wait(timeout=5)
                except Exception:
                    process.kill()
        for _name, _process, log in processes:
            log.close()
        try:
            ndn.stop()
            Minindn.cleanUp()
        except Exception:
            pass


def run(plan: dict[str, Any], output: Path, *, headless: bool,
        wall_timeout_seconds: float) -> int:
    check = preflight(plan, run=True, headless=headless)
    _write_json(output / "preflight.json", check)
    if not check["ok"]:
        return 2
    source_root = Path(plan["assets"]["videos"][0]["path"]).parent
    _prepare_source_frame_files(source_root, output,
                                window_count=int(plan["settings"]["windowCount"]))
    if plan["settings"].get("fault") == "missing-view":
        try:
            (output / "private/frames/UAV2/window-1.jpg").unlink()
        except FileNotFoundError:
            pass
    return _run_minindn(plan, output, wall_timeout_seconds=wall_timeout_seconds)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--license", default="")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--run-seconds", type=float, default=60.0)
    parser.add_argument("--window-count", type=int, default=3)
    parser.add_argument("--global-deadline-ms", type=int, default=60000)
    parser.add_argument("--fault", choices=sorted(FAULTS), default="none")
    parser.add_argument("--motion-fixture", type=Path,
                        help="use an existing Spec191 motion-fixture-v1 directory")
    parser.add_argument("--motion-profile", choices=MOTION_PROFILES, default="nominal",
                        help="generate a nominal or fail-closed motion validation profile")
    args = parser.parse_args(argv)
    if not (args.prepare or args.preflight or args.run):
        parser.error("one of --prepare, --preflight, or --run is required")
    try:
        plan = build_plan(source=args.source, model=args.model, output=args.output_dir,
                          license_text=args.license, headless=args.headless,
                          run_seconds=args.run_seconds,
                          window_count=args.window_count, fault=args.fault,
                          global_deadline_ms=args.global_deadline_ms,
                          motion_fixture=args.motion_fixture,
                          motion_profile=args.motion_profile)
        _write_json(args.output_dir / "manifest.json", plan)
        if args.prepare:
            print(json.dumps({"ok": True, "manifest": str(args.output_dir / "manifest.json")},
                             indent=2))
        if args.preflight or args.run:
            check = preflight(plan, run=args.run, headless=args.headless)
            _write_json(args.output_dir / "preflight.json", check)
            print(json.dumps(check, indent=2))
            if not check["ok"]:
                return 2
        return (run(plan, args.output_dir, headless=args.headless,
                    wall_timeout_seconds=args.run_seconds) if args.run else 0)
    except (RunnerError, OSError, ValueError) as error:
        print(f"Spec191 runner failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
