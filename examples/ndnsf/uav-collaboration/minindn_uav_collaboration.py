#!/usr/bin/env python3
"""Spec176 real MiniNDN acceptance launcher.

The launcher is deliberately opt-in.  Without ``--run`` it performs only a
read-only preflight, so a missing MiniNDN installation cannot be mistaken for
application evidence.  ``--run`` starts one controller, one Ground Station and
three Drone containers on the fixed four-node topology, records commands and
logs, and stops them after the measured 60-second mission window.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import signal
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOPOLOGY = Path(__file__).with_name("topology.conf")
CONFIG = ROOT / "NDNSF-UAV-APP/configs/uav_runtime.conf"
TRUST = ROOT / "NDNSF-UAV-APP/configs/uav-stream-trust-schema.conf"
DRONE = ROOT / "build/examples/UavDroneApp"
GROUND = ROOT / "build/examples/UavGroundStationApp"
CONTROLLER = ROOT / "build/examples/App_ServiceController"
MEASURED_WINDOW_SECONDS = 60


def topology_nodes(path: Path) -> set[str]:
    nodes: set[str] = set()
    active = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line == "[nodes]":
            active = True
            continue
        if line.startswith("["):
            active = False
        if active and line and not line.startswith("#"):
            nodes.add(line.rstrip(":"))
    return nodes


def preflight() -> dict[str, object]:
    required = [DRONE, GROUND, CONTROLLER, CONFIG, TRUST, TOPOLOGY]
    missing = [str(item) for item in required if not item.exists()]
    try:
        import minindn  # noqa: F401
        minindn_ok = True
    except Exception as exc:  # pragma: no cover - host dependent
        minindn_ok = False
        minindn_error = f"{type(exc).__name__}: {exc}"
    else:
        minindn_error = ""
    nodes = sorted(topology_nodes(TOPOLOGY)) if TOPOLOGY.exists() else []
    return {
        "schemaVersion": "spec176-uav-minindn-preflight-v1",
        "requiredNodes": ["gs", "scout-a", "scout-b", "compute"],
        "nodes": nodes,
        "missing": missing,
        "minindnAvailable": minindn_ok,
        "minindnError": minindn_error,
        "requiresRoot": True,
        "runningAsRoot": os.geteuid() == 0,
        "measuredWindowSeconds": MEASURED_WINDOW_SECONDS,
        "applicationContract": "named-producer-data-no-transport-endpoints",
    }


def run(output: Path, failure_case: str = "", measured_window_seconds: int = 60,
        ack_timeout_ms: int = 1500, timeout_ms: int = 10000) -> int:
    report = preflight()
    output.mkdir(parents=True, exist_ok=True)
    (output / "preflight.json").write_text(json.dumps(report, indent=2) + "\n")
    if report["missing"] or not report["minindnAvailable"]:
        print(json.dumps(report, indent=2))
        return 2
    if not report["runningAsRoot"]:
        report["runError"] = "MiniNDN/Mininet requires root; rerun this launcher with authorized sudo"
        (output / "preflight.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
        return 3

    from minindn.apps.nfd import Nfd
    from minindn.minindn import Minindn
    from minindn.util import getPopen

    # MiniNDN's constructor parses process-wide argv.  Hide this launcher's
    # --run/--output flags so they are not mistaken for MiniNDN CLI options.
    saved_argv = sys.argv
    sys.argv = [saved_argv[0]]
    try:
        ndn = Minindn(topoFile=str(TOPOLOGY), workDir=str(output / "minindn"))
    finally:
        sys.argv = saved_argv
    processes: list[tuple[object, object]] = []
    started = time.monotonic()

    def launch(node_name: str, name: str, command: str) -> object:
        node = ndn.net[node_name]
        log = (output / f"{name}.log").open("wb")
        proc = getPopen(node, command, shell=True, stdout=log, stderr=subprocess.STDOUT)
        processes.append((proc, log))
        return proc

    def wait_for_log(name: str, marker: str, timeout: float) -> None:
        path = output / f"{name}.log"
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if marker in path.read_text(encoding="utf-8", errors="replace"):
                return
            if any(proc.poll() is not None and proc.returncode != 0
                   for proc, log in processes if log.name == str(path)):
                raise RuntimeError(f"{name} exited before marker {marker!r}")
            time.sleep(0.1)
        raise RuntimeError(f"{name} did not emit marker {marker!r}")

    try:
        ndn.start()
        # Mininet's values() also includes the synthetic controller (c0),
        # which has no Mini-NDN host homeDir/params.  NFD belongs only on the
        # four application hosts in this topology.
        nfd_apps = []
        app_nodes = list(ndn.net.hosts)
        for node in app_nodes:
            nfd = Nfd(node)
            # The packaged Mini-NDN Nfd helper derives its socket directory
            # from the system default (/run/nfd).  That is correct for a
            # privileged host run, but is not writable inside an isolated
            # user namespace.  Keep each node's socket private to its
            # Mini-NDN home and make ndn-cxx use the same path.
            home_dir = Path(node.params["params"]["homeDir"])
            old_socket = f"/run/nfd/{node.name}.sock"
            new_socket = str(home_dir / f"{node.name}.sock")
            conf_path = Path(nfd.confFile)
            conf_path.write_text(conf_path.read_text(encoding="utf-8").replace(
                old_socket, new_socket), encoding="utf-8")
            Path(nfd.clientConf).write_text(
                f"transport=unix://{new_socket}\n", encoding="utf-8")
            nfd.start()
            nfd_apps.append(nfd)
        # Nfd.start() returns after spawning nfd, not after its management
        # socket is accepting commands.  Wait for every relocated socket
        # before setupFaces()/nfdc route operations; otherwise the first node
        # wins a race and later nodes silently miss the controller route.
        for node in app_nodes:
            home_dir = Path(node.params["params"]["homeDir"])
            socket = home_dir / f"{node.name}.sock"
            deadline = time.monotonic() + 10
            while not socket.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            if not socket.exists():
                raise RuntimeError(f"NFD socket not ready: {socket}")
        time.sleep(1.0)
        # The topology file describes both links and the NDN face plan.  A
        # Mini-NDN link alone does not create NFD faces; without this call the
        # controller and UAV processes are isolated despite healthy NFD logs.
        created_faces = ndn.setupFaces()
        face_target_ips = {
            (node.name, peer_name): peer_ip
            for node, links in created_faces.items()
            for peer_name, peer_ip, _cost in links
        }
        # setupFaces creates UDP faces but deliberately does not install RIB
        # routes.  The application uses names below /example/uav, so install a
        # bounded four-node overlay route before any process starts.  Each
        # route is recorded for audit and the multicast strategy tolerates the
        # same prefix being reachable through several UAVs.
        route_records = []
        # topology.conf is a star: gs is the only next hop between UAV hosts.
        # Do not ask nfdc to create impossible direct UAV-to-UAV faces; those
        # failures previously obscured the real application readiness result.
        next_hops = {
            "gs": ["scout-a", "scout-b", "compute"],
            "scout-a": ["gs"],
            "scout-b": ["gs"],
            "compute": ["gs"],
        }
        for node in app_nodes:
            node_name = node.name
            home_dir = Path(node.params["params"]["homeDir"])
            transport = f"unix://{home_dir / f'{node_name}.sock'}"
            for peer_name in next_hops[node_name]:
                peer = ndn.net[peer_name]
                # With a multi-interface host, peer.IP() is not necessarily
                # the address on this particular link.  setupFaces returns the
                # link-local address that its permanent face actually uses.
                peer_ip = face_target_ips[(node_name, peer_name)]
                face_list_command = f"NDN_CLIENT_TRANSPORT={transport} nfdc face list"
                face_list = node.cmd(face_list_command)
                face_match = re.search(
                    rf"faceid=(\d+)\s+remote=(?:udp|udp4)://{re.escape(peer_ip)}:6363",
                    face_list)
                if face_match is None:
                    raise RuntimeError(
                        f"NFD face missing node={node_name} peer={peer_name} "
                        f"ip={peer_ip} faces={face_list!r}")
                face_id = face_match.group(1)
                command = (
                    f"NDN_CLIENT_TRANSPORT={transport} nfdc route add "
                    f"/example/uav nexthop {face_id}"
                )
                result = node.cmd(command)
                route_records.append({"node": node_name, "peer": peer.name,
                                      "faceId": int(face_id),
                                      "command": command, "result": result})
            strategy = (
                f"NDN_CLIENT_TRANSPORT={transport} nfdc strategy set "
                "/example/uav /localhost/nfd/strategy/multicast"
            )
            route_records.append({"node": node_name, "peer": None,
                                  "command": strategy, "result": node.cmd(strategy)})
        (output / "routes.json").write_text(
            json.dumps(route_records, indent=2) + "\n", encoding="utf-8")
        # The controller must be able to encrypt PermissionResponse Data to
        # every participant's certificate.  Run all four applications against
        # one explicitly scoped PIB/TPM, rather than the per-node MiniNDN HOME
        # stores; otherwise the controller cannot resolve newly created UAV
        # identities and providers never receive their service permissions.
        keychain_root = output / "keys" / "shared"
        (keychain_root / "pib").mkdir(parents=True, exist_ok=True)
        (keychain_root / "tpm").mkdir(parents=True, exist_ok=True)
        failure_env = ""
        if failure_case:
            failure_env = (
                f"export NDNSF_SPEC176_FAILURE_CASE={shlex.quote(failure_case)}; "
            )
        runtime_env = (
            f"export LD_LIBRARY_PATH={ROOT / 'build'}:/usr/local/lib; "
            f"export NDN_CLIENT_PIB=pib-sqlite3:{keychain_root / 'pib'}; "
            f"export NDN_CLIENT_TPM=tpm-file:{keychain_root / 'tpm'}; "
            f"{failure_env}"
            "export NDN_LOG='ndn_service_framework.*=TRACE'; "
        )
        controller_command = (
            f"{runtime_env} cd {ROOT} && exec {CONTROLLER} "
            f"--controller-prefix /example/uav/controller "
            f"--policy-file {ROOT / 'NDNSF-UAV-APP/configs/uav_demo.policies'} "
            f"--trust-schema {ROOT / 'NDNSF-UAV-APP/configs/uav-stream-trust-schema.conf'}"
        )
        runtime_argument = f"--runtime-config {CONFIG}"
        launch("gs", "service-controller", controller_command)
        # App_ServiceController creates the authority identity and its
        # bootstrap identities on first use.  Ground Station and UAVs must
        # not open the same gs keychain until that transaction is complete.
        wait_for_log("service-controller", "ServiceController started...", 45)
        scout_a_command = f"{runtime_env} cd {ROOT} && exec {DRONE} --drone-id A --headless {runtime_argument}"
        scout_b_command = f"{runtime_env} cd {ROOT} && exec {DRONE} --drone-id B --headless {runtime_argument}"
        compute_unavailable = failure_case in {
            "no-feasible-detector", "fallback-disabled", "fallback-enabled",
        }
        unavailable_argument = " --unavailable" if compute_unavailable else ""
        compute_command = f"{runtime_env} cd {ROOT} && exec {DRONE} --drone-id C --headless{unavailable_argument} {runtime_argument}"
        ground_command = (
            f"{runtime_env} cd {ROOT} && exec {GROUND} --no-cert-dialog "
            f"{runtime_argument} --auto-incident-collaboration-test "
            f"--incident-collaboration-timeout-seconds {measured_window_seconds} "
            f"--ack-timeout-ms {ack_timeout_ms} --timeout-ms {timeout_ms} "
            f"--spec176-failure-case {shlex.quote(failure_case)}"
        )
        # Serialize first-use identity creation in the shared PIB.  Once each
        # process reports readiness, all providers remain concurrent for the
        # actual collaboration request.
        launch("scout-a", "scout-a", scout_a_command)
        wait_for_log("scout-a", "DRONE_HEADLESS_READY", 45)
        launch("scout-b", "scout-b", scout_b_command)
        wait_for_log("scout-b", "DRONE_HEADLESS_READY", 45)
        compute_proc = launch("compute", "compute", compute_command)
        wait_for_log("compute", "DRONE_HEADLESS_READY", 45)
        if failure_case == "compute-uav-loss":
            compute_proc.send_signal(signal.SIGKILL)
            time.sleep(0.2)
        # Do not let the Ground Station close its ACK window while providers
        # are still bootstrapping NAC-ABE/DKEY state.  Provider readiness is a
        # real application barrier, not a fixed sleep or a synthetic fixture.
        launch("gs", "ground-station", ground_command)
        (output / "manifest.json").write_text(json.dumps({
            "schemaVersion": "spec176-uav-minindn-manifest-v1",
            "topology": str(TOPOLOGY),
            "nodes": ["gs", "scout-a", "scout-b", "compute"],
            "config": str(CONFIG),
            "measuredWindowSeconds": measured_window_seconds,
            "failureCase": failure_case,
            "commands": [controller_command, scout_a_command, scout_b_command,
                         compute_command, ground_command],
            "runtimeLibraryPath": str(ROOT / "build"),
            "contract": "Interest -> verified producer Data; no IP/host/port in application payload",
        }, indent=2) + "\n")
        # The Ground Station exits only after the finite collaboration emits
        # its terminal result.  Keep the other runtimes alive briefly so the
        # final report and status evidence are flushed, then shut down.
        # The marker is emitted after the measured mission window, not after
        # process startup.  The wait must therefore cover that window plus
        # bounded startup/shutdown slack; a shorter wait falsely reports a
        # healthy run as a launcher failure.
        if failure_case == "selected-provider-loss":
            wait_for_log("ground-station", "PROVIDER_SELECTED", 45)
            if compute_proc.poll() is None:
                compute_proc.send_signal(signal.SIGKILL)
        wait_for_log("ground-station", "GS_INCIDENT_COLLABORATION_EXIT",
                     measured_window_seconds + 45)
        time.sleep(2)
    finally:
        for proc, log in reversed(processes):
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=5)
                except Exception:
                    proc.kill()
            log.close()
        ndn.stop()
        Minindn.cleanUp()
    log_paths = sorted(str(path) for path in output.glob("*.log"))
    collaboration_ok = any(
        "GS_INCIDENT_COLLABORATION_EXIT ok=true" in
        Path(path).read_text(encoding="utf-8", errors="replace")
        for path in log_paths)
    summary = {
        "schemaVersion": "spec176-uav-minindn-summary-v1",
        "elapsedSeconds": round(time.monotonic() - started, 3),
        "processReturnCodes": [proc.poll() for proc, _ in processes],
        "logs": log_paths,
        "failureCase": failure_case,
        "measuredWindowSeconds": measured_window_seconds,
        "status": (
            "nominal-collaboration-completed"
            if collaboration_ok
            else "nominal-collaboration-failed"
        ),
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    return 0 if all(code in (0, -signal.SIGINT, None) for code in summary["processReturnCodes"]) else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "results/spec176-uav-minindn")
    parser.add_argument("--failure-case", default="",
                        help="optional Spec176 failure-injection case")
    parser.add_argument("--window-seconds", type=int, default=MEASURED_WINDOW_SECONDS)
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=10000)
    args = parser.parse_args()
    if not args.run:
        report = preflight()
        print(json.dumps(report, indent=2))
        return 0 if not report["missing"] else 2
    if args.window_seconds <= 0:
        parser.error("--window-seconds must be positive")
    if args.ack_timeout_ms <= 0 or args.timeout_ms <= 0 or args.ack_timeout_ms > args.timeout_ms:
        parser.error("timeouts must be positive and ACK timeout cannot exceed request timeout")
    return run(args.output.resolve(), args.failure_case, args.window_seconds,
               args.ack_timeout_ms, args.timeout_ms)


if __name__ == "__main__":
    raise SystemExit(main())
