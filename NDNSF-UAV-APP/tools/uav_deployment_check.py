#!/usr/bin/env python3
"""Preflight checks for deploying NDNSF-UAV-APP on real machines.

The script is intentionally conservative: it does not modify system state, and
it reports actionable failures before an operator starts the service container.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import socket
import stat
import subprocess
import sys
import tempfile


def run(cmd: list[str], timeout: int = 10) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout, check=False)


class Reporter:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.warnings: list[str] = []

    def ok(self, message: str) -> None:
        print(f"[OK]   {message}")

    def warn(self, message: str) -> None:
        self.warnings.append(message)
        print(f"[WARN] {message}")

    def fail(self, message: str) -> None:
        self.failures.append(message)
        print(f"[FAIL] {message}")


def check_binary(reporter: Reporter, name: str, required: bool = True) -> None:
    if shutil.which(name):
        reporter.ok(f"found {name}")
    elif required:
        reporter.fail(f"{name} is not in PATH")
    else:
        reporter.warn(f"{name} is not in PATH")


def check_file(reporter: Reporter, path_text: str, label: str,
               required: bool = True) -> Path:
    path = Path(path_text).expanduser()
    if path.exists():
        reporter.ok(f"{label}: {path}")
    elif required:
        reporter.fail(f"{label} does not exist: {path}")
    else:
        reporter.warn(f"{label} does not exist: {path}")
    return path


def check_nfd(reporter: Reporter) -> None:
    check_binary(reporter, "nfd-status")
    if not shutil.which("nfd-status"):
        return
    try:
        result = run(["nfd-status", "-v"], timeout=5)
    except subprocess.TimeoutExpired:
        reporter.fail("nfd-status timed out; is local NFD responsive?")
        return
    if result.returncode == 0:
        reporter.ok("local NFD is reachable")
    else:
        reporter.fail("local NFD is not reachable; start NFD and check NDN_CLIENT_TRANSPORT")


def check_identity(reporter: Reporter, identity: str) -> None:
    if shutil.which("ndnsec-ls-identity"):
        cmd = ["ndnsec-ls-identity", "-c"]
        reporter.ok("found ndnsec-ls-identity")
    elif shutil.which("ndnsec"):
        cmd = ["ndnsec", "list"]
        reporter.ok("found ndnsec")
    else:
        reporter.fail("neither ndnsec-ls-identity nor ndnsec is in PATH")
        return
    try:
        result = run(cmd, timeout=10)
    except subprocess.TimeoutExpired:
        reporter.fail("ndnsec-ls-identity timed out")
        return
    if identity in result.stdout:
        reporter.ok(f"identity has an installed certificate: {identity}")
    else:
        reporter.fail(f"identity certificate not found in local keychain: {identity}")


def check_trust_schema(reporter: Reporter, trust_schema: Path) -> None:
    if not trust_schema.exists():
        return
    text = trust_schema.read_text(errors="replace")
    if "type any" in text:
        reporter.warn("trust schema uses 'type any'; replace it with a root certificate file for real deployment")
    if "type file" in text and "file-name" in text:
        reporter.ok("trust schema uses a file trust anchor")


def check_yolo(reporter: Reporter, model: str, worker_script: str) -> None:
    worker = check_file(reporter, worker_script, "YOLO worker script")
    if not worker.exists():
        return
    check_binary(reporter, "python3")
    if not shutil.which("python3"):
        return
    try:
        with tempfile.NamedTemporaryFile(suffix=".jpg") as image:
            # A tiny invalid file is enough to test import/model load readiness:
            # the worker prints ready before reading image paths.
            image.write(b"not-a-real-image")
            image.flush()
            proc = subprocess.Popen(
                ["python3", str(worker), "--model", model, "--classes", "car,truck"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True)
            try:
                line = proc.stdout.readline().strip() if proc.stdout else ""
                if line.startswith("ready=true"):
                    reporter.ok(f"YOLO worker can load model: {model}")
                else:
                    reporter.fail(f"YOLO worker did not become ready: {line}")
            finally:
                if proc.stdin:
                    proc.stdin.write("__quit__\n")
                    proc.stdin.flush()
                proc.wait(timeout=5)
    except Exception as exc:  # pragma: no cover - deployment helper
        reporter.fail(f"YOLO worker check failed: {exc}")


def check_udp_port(reporter: Reporter, port_text: str, label: str) -> None:
    try:
        port = int(port_text)
    except ValueError:
        reporter.fail(f"{label} is not a valid UDP port: {port_text}")
        return
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", port))
        reporter.ok(f"{label} is available: {port}")
    except OSError as exc:
        reporter.warn(f"{label} may already be in use: {port} ({exc})")
    finally:
        sock.close()


def check_serial_device(reporter: Reporter, device_text: str, baud_text: str) -> None:
    device = check_file(reporter, device_text, "MAVLink serial device")
    if not device.exists():
        return
    mode = device.stat().st_mode
    if stat.S_ISCHR(mode):
      reporter.ok(f"MAVLink serial device is a character device: {device}")
    else:
      reporter.warn(f"MAVLink serial path is not a character device: {device}")
    if os.access(device, os.R_OK | os.W_OK):
        reporter.ok(f"MAVLink serial device is readable/writable: {device}")
    else:
        reporter.fail(f"MAVLink serial device is not readable/writable by this user: {device}")
    try:
        baud = int(baud_text)
    except ValueError:
        reporter.fail(f"MAVLink serial baud is not numeric: {baud_text}")
        return
    if baud in {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600}:
        reporter.ok(f"MAVLink serial baud is supported: {baud}")
    else:
        reporter.fail(f"MAVLink serial baud is unsupported by DroneAPP: {baud}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=["ground-station", "drone"], required=True)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--trust-schema", default="examples/trust-schema.conf")
    parser.add_argument("--video-source", default="NDNSF-UAV-APP/videos/drone.mp4")
    parser.add_argument("--yolo-model", default="yolo26n.pt")
    parser.add_argument("--yolo-worker-script", default="NDNSF-UAV-APP/tools/yolo_detect_worker.py")
    parser.add_argument("--flight-controller-backend",
                        choices=["mock", "udp", "mavlink-router", "serial"],
                        default="mock")
    parser.add_argument("--mavlink-udp-host", default="127.0.0.1")
    parser.add_argument("--mavlink-udp-port", default="18570")
    parser.add_argument("--mavlink-udp-listen-port", default="14550")
    parser.add_argument("--mavlink-serial-device", default="/dev/ttyAMA0")
    parser.add_argument("--mavlink-serial-baud", default="57600")
    args = parser.parse_args()

    reporter = Reporter()
    print(f"NDNSF-UAV-APP deployment check role={args.role}")
    check_nfd(reporter)
    check_identity(reporter, args.identity)
    trust_schema = check_file(reporter, args.trust_schema, "trust schema")
    check_trust_schema(reporter, trust_schema)

    check_binary(reporter, "ffmpeg")
    if args.role == "drone":
        check_file(reporter, args.video_source, "video source")
        if args.flight_controller_backend in {"udp", "mavlink-router"}:
            check_udp_port(reporter, args.mavlink_udp_listen_port, "MAVLink listen UDP port")
            reporter.ok(f"MAVLink target configured as {args.mavlink_udp_host}:{args.mavlink_udp_port}")
        elif args.flight_controller_backend == "serial":
            check_serial_device(reporter, args.mavlink_serial_device, args.mavlink_serial_baud)
    else:
        check_yolo(reporter, args.yolo_model, args.yolo_worker_script)

    print()
    if reporter.warnings:
        print(f"Warnings: {len(reporter.warnings)}")
    if reporter.failures:
        print(f"Failures: {len(reporter.failures)}")
        return 1
    print("Deployment preflight passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
