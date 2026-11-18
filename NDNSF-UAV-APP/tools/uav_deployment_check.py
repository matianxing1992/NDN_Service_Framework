#!/usr/bin/env python3
"""Preflight checks for deploying NDNSF-UAV-APP on real machines.

The script is intentionally conservative: it does not modify system state, and
it reports actionable failures before an operator starts the service container.
"""

from __future__ import annotations

import argparse
import re
import os
from pathlib import Path
import shutil
import socket
import stat
import subprocess
import sys
import uuid


def run(cmd: list[str], timeout: int = 10) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout, check=False)


def read_fields(path: Path) -> dict[str, str]:
    fields: dict[str, str] = {}
    if not path.exists():
        return fields
    for raw in path.read_text(errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2:
            fields[parts[0]] = parts[1].strip()
    return fields


def resolve_path(path_text: str, base: Path) -> Path:
    path = Path(path_text).expanduser()
    if path.is_absolute():
        return path
    return (base / path).resolve()


def parse_expected_cert(value: str) -> tuple[str, Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected cert must be IDENTITY=FILE")
    identity, cert_file = value.split("=", 1)
    if not identity.startswith("/"):
        raise argparse.ArgumentTypeError("expected cert identity must start with /")
    return identity, Path(cert_file).expanduser()


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


def check_nfd_rib_authorization(reporter: Reporter, namespace: str) -> None:
    """Verify NFD accepts a prefix registration, before any actor depends on one.

    A reachable forwarder is not enough: without a matching management
    authorization the first actor fails with
    "ServiceController prefix registration failed: ...: authorization rejected"
    after the service container has already started. Probe one unused prefix
    under the deployment namespace and remove it again; a rejected registration
    is reported as a deployment failure rather than discovered at runtime.
    """
    if not shutil.which("nfdc"):
        reporter.warn("nfdc is unavailable; NFD RIB authorization was not checked")
        return
    probe = namespace.rstrip("/") + "/deployment-check/" + uuid.uuid4().hex
    added = False
    try:
        result = run(["nfdc", "route", "add", probe, "1"], timeout=10)
        text = result.stdout or ""
        if result.returncode == 0:
            added = True
            # This proves the forwarder accepts registration from the invoking identity. It does not prove a
            # specific role identity is authorised; that needs the role's own credential home.
            reporter.ok("NFD accepts prefix registration under " + namespace +
                        " from the invoking identity")
        elif "authorization rejected" in text or "403" in text:
            reporter.fail(
                "NFD rejects prefix registration under " + namespace +
                "; grant the deployment identity rib/route management in the NFD "
                "authorization configuration before starting the actors")
        else:
            reporter.warn("NFD prefix-registration probe was inconclusive: " +
                          text.strip()[:200])
    except subprocess.TimeoutExpired:
        reporter.warn("NFD prefix-registration probe timed out")
    finally:
        if added:
            try:
                run(["nfdc", "route", "remove", probe, "1"], timeout=10)
            except subprocess.TimeoutExpired:
                reporter.warn("could not remove the probe route " + probe)


def get_ndnsec_list(reporter: Reporter) -> str:
    if shutil.which("ndnsec"):
        cmd = ["ndnsec", "list", "-c"]
        reporter.ok("found ndnsec")
    else:
        reporter.fail("ndnsec is not in PATH")
        return ""
    try:
        result = run(cmd, timeout=10)
    except subprocess.TimeoutExpired:
        reporter.fail("ndnsec list timed out")
        return ""
    if result.returncode != 0:
        reporter.fail("ndnsec list failed")
        return ""
    return result.stdout


def cert_name_from_file(cert_file: Path) -> str:
    if not cert_file.exists():
        return ""
    result = run(["ndnsec", "cert-dump", "-f", str(cert_file), "-p"], timeout=10)
    if result.returncode != 0:
        return ""
    lines = result.stdout.splitlines()
    for idx, line in enumerate(lines):
        if line.lower().startswith("certificate name:"):
            value = line.split(":", 1)[1].strip()
            if value:
                return value
            if idx + 1 < len(lines):
                return lines[idx + 1].strip()
    return ""


def default_cert_name(identity: str) -> str:
    result = run(["ndnsec", "cert-dump", "-i", identity, "-p"], timeout=10)
    if result.returncode != 0:
        return ""
    lines = result.stdout.splitlines()
    for idx, line in enumerate(lines):
        if line.lower().startswith("certificate name:"):
            value = line.split(":", 1)[1].strip()
            if value:
                return value
            if idx + 1 < len(lines):
                return lines[idx + 1].strip()
    return ""


def identity_key_names(identity: str, ndnsec_list: str) -> set[str]:
    # ndnsec list output is tree-formatted. Match only key names for the exact
    # identity, not children such as /example/uav/drone/A/foo.
    escaped = re.escape(identity.rstrip("/"))
    pattern = re.compile(rf"({escaped}/KEY/[^\s/]+)")
    return set(pattern.findall(ndnsec_list))


def check_identity(reporter: Reporter, identity: str, ndnsec_list: str,
                   allow_multiple_certs: bool = False,
                   expected_cert_file: Path | None = None,
                   required: bool = True) -> None:
    if not identity:
        if required:
            reporter.fail("identity is empty")
        return
    if not ndnsec_list:
        return
    cert_name = default_cert_name(identity) if shutil.which("ndnsec") else ""
    if cert_name:
        reporter.ok(f"identity has an installed certificate: {identity}")
        reporter.ok(f"default certificate for {identity}: {cert_name}")
    else:
        if required:
            reporter.fail(f"identity certificate not found in local keychain: {identity}")
        else:
            reporter.warn(f"identity certificate not found in local keychain: {identity}")
        return

    keys = identity_key_names(identity, ndnsec_list)
    if len(keys) > 1 and not allow_multiple_certs:
        reporter.fail("multiple key/certificate choices for "
                      f"{identity}; set the intended default cert or remove stale keys: "
                      + ", ".join(sorted(keys)))
    elif len(keys) > 1:
        reporter.warn("multiple key/certificate choices for "
                      f"{identity}: " + ", ".join(sorted(keys)))

    if expected_cert_file is not None:
        expected_name = cert_name_from_file(expected_cert_file)
        if not expected_name:
            reporter.fail(f"cannot read expected certificate file for {identity}: {expected_cert_file}")
        elif expected_name == cert_name:
            reporter.ok(f"{identity} default certificate matches {expected_cert_file}")
        else:
            reporter.fail(f"{identity} default certificate mismatch: local={cert_name} expected={expected_name}")


def identities_from_policy(policy_file: Path) -> set[str]:
    if not policy_file.exists():
        return set()
    identities: set[str] = set()
    for raw in policy_file.read_text(errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("for "):
            parts = line.split()
            if len(parts) >= 2 and parts[1].startswith("/"):
                identities.add(parts[1])
    return identities


FILE_ANCHOR = re.compile(r"type\s+file\s*;?[\s\S]{0,200}?file-name\s+\"?([^\"\s;]+)\"?", re.IGNORECASE)


def check_trust_schema(reporter: Reporter, trust_schema: Path) -> None:
    """Verify the schema pins a file anchor and that the anchor resolves where the runtime looks.

    A file anchor is resolved relative to the schema's own directory, not relative to the working
    directory or to a ``--root-cert`` option: an otherwise correct deployment fails with
    "NDNSF trust anchor is missing, malformed, or outside validity" when ``root.cert`` sits anywhere else.
    Both facts were observed while deploying the release bundle, so the check reports them together.
    """
    if not trust_schema.exists():
        return
    text = trust_schema.read_text(errors="replace")
    if "type any" in text:
        reporter.warn("trust schema uses 'type any'; replace it with a root certificate file for real "
                      "deployment")
    match = FILE_ANCHOR.search(text)
    if match is None:
        if "type file" in text:
            reporter.fail("trust schema declares a file anchor without a usable file-name")
        return
    reporter.ok("trust schema uses a file trust anchor")
    anchor_name = match.group(1)
    anchor_path = Path(anchor_name)
    if not anchor_path.is_absolute():
        anchor_path = trust_schema.parent / anchor_path
    if not anchor_path.is_file():
        reporter.fail("trust anchor is not readable next to the schema: "
                      f"{anchor_path}; a file anchor is resolved relative to the schema directory, "
                      "so an otherwise valid --root-cert path elsewhere does not help")
        return
    reporter.ok(f"trust anchor resolves: {anchor_path}")
    # Only existence is checked here. Multi-anchor schemas, paths containing spaces and certificate validity
    # windows are decided by the native validator, so a deployment that passes this check can still be
    # refused there.


def check_yolo(reporter: Reporter, model: str, worker_script: str) -> None:
    model_path = Path(model)
    worker = Path(worker_script)
    if not model_path.is_file():
        reporter.fail(f"YOLO model file not found: {model_path}")
        return
    if not worker.is_file():
        reporter.fail(f"YOLO worker script not found: {worker}")
        return
    check_binary(reporter, "python3")
    if not shutil.which("python3"):
        return
    try:
        result = subprocess.run(
            ["python3", str(worker), "--model", str(model_path), "--classes", "car,truck"],
            input="__quit__\n", capture_output=True, text=True, timeout=35, check=False)
        line = result.stdout.partition("\n")[0]
        if result.returncode == 0 and line.startswith("ready=true"):
            reporter.ok(f"YOLO worker can load model: {model_path}")
        else:
            reporter.fail(f"YOLO worker did not become ready (exit {result.returncode}): {line}")
    except (OSError, subprocess.TimeoutExpired) as exc:
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


def check_recording_config(reporter: Reporter, fields: dict[str, str]) -> None:
    retired = {
        "camera-record-packet-limit",
        "camera-recording-encryption",
        "camera-recording-content-key",
        "camera-recording-key-file",
    }
    present = sorted(retired.intersection(fields))
    if present:
        reporter.fail("retired recording-only configuration is present: " +
                      ", ".join(present))
    if "camera-retention-packet-limit" in fields:
        try:
            value = int(fields["camera-retention-packet-limit"])
            if value < 0:
                raise ValueError
            reporter.ok("canonical packet-retention bound is valid")
        except ValueError:
            reporter.fail("camera-retention-packet-limit must be a non-negative integer")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=["controller", "ground-station", "drone"], required=True)
    parser.add_argument("--identity", default="")
    parser.add_argument("--runtime-config", default="NDNSF-UAV-APP/configs/uav_runtime.conf")
    parser.add_argument("--app-config", default="")
    parser.add_argument("--policy-file", default="")
    parser.add_argument("--trust-schema", default="examples/trust-schema.conf")
    parser.add_argument("--expected-cert", action="append", type=parse_expected_cert,
                        default=[], metavar="IDENTITY=FILE",
                        help="require IDENTITY's default certificate to match FILE")
    parser.add_argument("--expected-identity", action="append", default=[],
                        help="extra identity that must be installed locally")
    parser.add_argument("--allow-multiple-certs", action="store_true",
                        help="warn instead of failing when an identity has multiple keys")
    parser.add_argument("--skip-nfd", action="store_true")
    parser.add_argument("--video-source")
    parser.add_argument("--yolo-model")
    parser.add_argument("--yolo-worker-script")
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
    cwd = Path.cwd()
    runtime_config = resolve_path(args.runtime_config, cwd)
    check_file(reporter, str(runtime_config), "runtime config")
    runtime_fields = read_fields(runtime_config)
    app_config = resolve_path(args.app_config, cwd) if args.app_config else None
    if app_config:
        check_file(reporter, str(app_config), "app config")
    app_fields = read_fields(app_config) if app_config else {}

    if not args.skip_nfd:
        check_nfd(reporter)
        check_nfd_rib_authorization(
            reporter, runtime_fields.get("root-identity", "/example/uav"))

    if args.role == "controller":
        identity = args.identity or runtime_fields.get("controller-prefix", "")
    elif args.role == "ground-station":
        identity = args.identity or app_fields.get("ground-station-identity") or \
            runtime_fields.get("ground-station-identity", "")
    else:
        drone_id = app_fields.get("drone-id", "A")
        drone_prefix = runtime_fields.get("drone-prefix", "/example/uav/drone").rstrip("/")
        identity = args.identity or f"{drone_prefix}/{drone_id}"

    ndnsec_list = get_ndnsec_list(reporter)
    expected = {identity: None}
    for expected_identity in args.expected_identity:
        expected[expected_identity] = None
    for expected_identity, cert_file in args.expected_cert:
        expected[expected_identity] = resolve_path(str(cert_file), cwd)

    for expected_identity, cert_file in expected.items():
        check_identity(reporter, expected_identity, ndnsec_list,
                       allow_multiple_certs=args.allow_multiple_certs,
                       expected_cert_file=cert_file)

    policy_file = resolve_path(args.policy_file, cwd) if args.policy_file else \
        resolve_path("uav_demo.policies", runtime_config.parent)
    if args.role == "controller":
        check_file(reporter, str(policy_file), "controller policy file")
        checked = set(expected.keys())
        for target in sorted(identities_from_policy(policy_file)):
            if target == identity or target in checked:
                continue
            check_identity(reporter, target, ndnsec_list,
                           allow_multiple_certs=args.allow_multiple_certs,
                           required=False)

    trust_schema_text = args.trust_schema
    if trust_schema_text == "examples/trust-schema.conf":
        trust_schema_text = runtime_fields.get("trust-schema", trust_schema_text)
    trust_schema = check_file(reporter, str(resolve_path(trust_schema_text, cwd)), "trust schema")
    check_trust_schema(reporter, trust_schema)

    check_binary(reporter, "ffmpeg")
    if args.role == "drone":
        check_recording_config(reporter, app_fields)
        video_source = args.video_source or app_fields.get("video-source", "auto")
        if video_source == "auto":
            video_fallback = app_fields.get("video-fallback")
            if video_fallback:
                check_file(reporter, str(resolve_path(video_fallback, app_config.parent)),
                           "video fallback")
            else:
                reporter.ok("video source is auto; no sample is configured if V4L2 is unavailable")
        else:
            check_file(reporter, str(resolve_path(video_source, cwd)), "video source")
        backend = app_fields.get("flight-controller-backend", args.flight_controller_backend)
        if backend in {"udp", "mavlink-router"}:
            check_udp_port(reporter,
                           app_fields.get("mavlink-udp-listen-port", args.mavlink_udp_listen_port),
                           "MAVLink listen UDP port")
            reporter.ok("MAVLink target configured as " +
                        f"{app_fields.get('mavlink-udp-host', args.mavlink_udp_host)}:"
                        f"{app_fields.get('mavlink-udp-port', args.mavlink_udp_port)}")
        elif backend == "serial":
            check_serial_device(reporter,
                                app_fields.get("mavlink-serial-device", args.mavlink_serial_device),
                                app_fields.get("mavlink-serial-baud", args.mavlink_serial_baud))
    elif args.role == "ground-station":
        yolo_model = args.yolo_model or app_fields.get("yolo-model", "yolo26n.pt")
        yolo_worker = args.yolo_worker_script or app_fields.get(
            "yolo-worker-script", "NDNSF-UAV-APP/tools/yolo_detect_worker.py")
        check_yolo(reporter, yolo_model, yolo_worker)

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
