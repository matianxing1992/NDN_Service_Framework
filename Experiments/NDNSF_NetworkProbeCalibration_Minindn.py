#!/usr/bin/env python3
"""Run the native probe service on an isolated wired calibration topology."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time

from minindn.minindn import Minindn
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
from minindn.helpers.nfdc import Nfdc
from minindn.util import getPopen

from NDNSF_NewAPI_Minindn_Perf import node_cmd, wait_for_nfd_sockets


ROOT = "/example/probe-calibration"
PROBE = "/NDNSF/Network/Probe/v1"
IDENTITIES = {"pc": ROOT + "/controller", "ps": ROOT + "/provider/source",
              "po": ROOT + "/provider/observer", "pu": ROOT + "/user"}
MATRIX = [(1, 10), (4, 10), (10, 10), (4, 2), (4, 30), (4, 70)]


def measurement_cells(smoke, control, authorization_case=None):
    if authorization_case is not None:
        if authorization_case not in ("deny", "revoke") or smoke or control is not None:
            raise ValueError("authorization campaigns must run separately")
        return [(1, 10, None)]
    if control is not None:
        if smoke:
            raise ValueError("control campaigns require repeated samples")
        if control == "high-bandwidth":
            return [(1000, 10, None)]
        if control in ("aligned-face-mtu", "sustained-reference"):
            return [(4, 10, None)]
        if control == "sustained-reference-burst":
            return [(4, 10, 2000)]
        if control == "default-burst":
            # Reproduce Mininet's HTB literal; do not substitute the TBF
            # default's 15000-byte value for tc's parsing of "15k".
            return [(4, 10, "15k")]
        raise ValueError("unknown calibration control")
    return [(bandwidth, delay, "15k") for bandwidth, delay in (MATRIX[:1] if smoke else MATRIX)]


def uses_sustained_reference(control, authorization_case):
    return authorization_case is None and control in (
        None, "sustained-reference", "sustained-reference-burst", "default-burst")


def remaining_campaign_timeout(deadline, phase_limit):
    remaining = min(phase_limit, deadline - time.monotonic())
    if remaining <= 0:
        raise RuntimeError("campaign deadline exhausted")
    return remaining


def parse_iperf_udp_receiver_summary(log):
    # Only the receiver's whole-run UDP summary counts, not interval reports
    # or the sender's offered rate. The maintained command uses -f m.
    summaries = []
    number = r"[0-9]+(?:\.[0-9]+)?"
    pattern = re.compile(r"^\[\s*\d+\]\s+(" + number + r")\s*-\s*(" + number + r")\s+sec\s+"
                         + number + r"\s+(?:[KMG]?Bytes)\s+(" + number + r")\s+Mbits/sec\s+"
                         + number + r"\s+ms\s+\d+\s*/\s*\d+\s+\(" + number + r"%\)\s*$")
    for line in log.splitlines():
        match = pattern.match(line.strip())
        if match is None:
            continue
        start, end, rate = map(float, match.groups())
        if start == 0 and 8 <= end <= 20 and math.isfinite(rate) and rate > 0:
            summaries.append({"duration_s": end, "received_payload_mbps": rate})
    if len(summaries) != 1:
        raise RuntimeError("missing or ambiguous whole-run UDP receiver summary")
    return summaries[0]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def parse_iperf_version(stdout, stderr, returncode):
    text = (stdout + stderr).decode(errors="replace").strip()
    # iperf2 2.0.13 reports --version on stderr with exit status 1.
    if returncode not in (0, 1) or not re.match(r"^iperf version 2\.\d+\.\d+(?:\s|$)", text):
        raise RuntimeError("unexpected iperf2 version response")
    return text


def verify_probe_face_mtu(state, face, remote, mtu):
    entries = [line for line in state.splitlines() if "faceid=" in line]
    if (len(entries) != 1 or
            not re.search(r"\bfaceid=" + str(face) + r"(?:\s|$)", entries[0]) or
            not re.search(r"\bremote=" + re.escape(remote) + r"(?:\s|$)", entries[0]) or
            not re.search(r"\bmtu=" + str(mtu) + r"(?:\s|$)", entries[0])):
        raise RuntimeError("measured face MTU readback mismatch")


def policy_services(role, authorization_case):
    if role == "user" and authorization_case == "deny":
        return "/NDNSF/Calibration"
    return PROBE + "\n/NDNSF/Calibration"


def trust_schema(certificate):
    # Provider certificates are signed by the Controller, whose identity is
    # a sibling of Provider/User identities. Hierarchical checking is wrong
    # for those certificates; all chains must still end at this file anchor.
    rules = []
    for label, kind, pattern, signer in [
            ("certificates", "data", "^<>+<KEY><><><>$",
             "^<example><probe-calibration><controller><KEY><>{1,3}$"),
            ("runtime", "data", "^<example><probe-calibration><>*$",
             "^<example><probe-calibration>[^<KEY>]*<KEY><>{1,3}$"),
            ("sync", "interest", "^<example><probe-calibration><group><>*$",
             "^<example><probe-calibration>[^<KEY>]*<KEY><>{1,3}$"),
            ("peer probes", "interest",
             "^<example><probe-calibration><provider><>*<NDNSF><RPC>[<PROBE-RTT><PROBE-GOODPUT><PROBE-READY>]<>*$",
             "^<example><probe-calibration><provider>[^<KEY>]*<KEY><>{1,3}$")]:
        rules.append('rule { id "%s" for %s filter { type name regex "%s" } '
                     'checker { type customized sig-type rsa-sha256 '
                     'key-locator { type name regex "%s" } } }' %
                     (label, kind, pattern, signer))
    return "\n".join(rules) + '\ntrust-anchor { type file file-name "%s" }\n' % certificate


def stop_process(process):
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
    return process.returncode


def verify_probe_fib(fib, prefix, face):
    # NFD FibModule::formatItemText emits the prefix as the first token.
    entries = [line for line in fib.splitlines()
               if line.split() and line.split()[0] == prefix]
    if len(entries) != 1 or re.findall(r"faceid=(\d+)", entries[0]) != [str(face)]:
        raise RuntimeError("probe FIB does not exclusively select measured edge: " + prefix)


def run(parser, args):
    matrix = measurement_cells(args.smoke, args.control, args.authorization_case)
    if os.geteuid() != 0:
        raise RuntimeError("MiniNDN calibration requires root")
    output = Path(args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = {}
    required = ("DI_NetworkProbeCalibration", "App_ServiceController", "ndnsec", "tcpdump", "tc")
    if uses_sustained_reference(args.control, args.authorization_case):
        required += ("iperf",)
    for name in required:
        tools[name] = shutil.which(name)
        if tools[name] is None:
            raise RuntimeError("missing installed executable: " + name)
    identities = {name: {"path": path, "sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest()}
                  for name, path in tools.items()}
    if "iperf" in tools:
        version = subprocess.run([tools["iperf"], "--version"], capture_output=True, timeout=5)
        identities["iperf"]["version"] = parse_iperf_version(version.stdout, version.stderr, version.returncode)
        identities["iperf"]["version_exit_code"] = version.returncode
    write_json(output / "candidate-tools.json", identities)
    topology = output / "topology.conf"
    topology.write_text("[nodes]\npc: _\nps: _\npo: _\npu: _\n[links]\n"
                        "pc:ps delay=1ms bw=100\npc:po delay=1ms bw=100\n"
                        "pc:pu delay=1ms bw=100\nps:po delay=10ms bw=4\n")
    ndn = Minindn(parser=parser, topoFile=str(topology), workDir=str(output / "net"))
    owned = []
    logs = []
    cleanup = {"processes": [], "network_stopped": False}

    def start(node, label, command):
        handle = (output / (label + ".log")).open("wb")
        logs.append(handle)
        process = getPopen(node, command, envDict={
                           "NDNSF_CONTROLLER_CERT_FILE": str(output / "controller.cert"),
                           "NDNSF_CONTROLLER_GENERATION_STATE": str(output / "controller-generation.state"),
                           "NDN_LOG": os.environ.get("NDNSF_PROBE_APP_NDN_LOG",
                               os.environ.get("NDN_LOG", "ndn_service_framework.*=DEBUG:ndnsvs.*=INFO"))},
                           shell=False, stdout=handle,
                           stderr=subprocess.STDOUT, start_new_session=True,
                           mncmd=["mnexec", "-a", str(node.pid)])
        owned.append((label, process))
        return process

    def command(node, argv, timeout=15):
        process = getPopen(node, argv, shell=False, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, start_new_session=True,
                           mncmd=["mnexec", "-a", str(node.pid)])
        owned.append(("command:" + argv[0], process))
        try:
            text, _ = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            stop_process(process)
            raise RuntimeError("node command timed out: " + argv[0])
        if process.returncode:
            raise RuntimeError("node command failed: " + text.decode(errors="replace"))
        return text.decode()

    cells = []
    nfd_manager = None
    try:
        ndn.start()
        nfd_manager = AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        wait_for_nfd_sockets(ndn, output, timeout_s=20)
        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        for node_name, identity in IDENTITIES.items():
            routing.addOrigin([ndn.net[node_name]], [identity, identity + "/KEY"])
        routing.calculateRoutes()
        # The control star has lower delay than the measured direct edge.
        # Pin only probe names, leaving certificates and targeted invocations
        # on their ordinary routes. Longest-prefix matching selects this face.
        probe_routes = []
        for node_name, peer in (("ps", "po"), ("po", "ps")):
            node = ndn.net[node_name]
            face = Nfdc.createFace(node, NdnRoutingHelper.getNeighborIP(node)[peer], protocol="udp")
            if str(face) == "-1":
                raise RuntimeError("cannot create measured-edge face")
            if args.control == "aligned-face-mtu":
                intf = node.connectionsTo(ndn.net[peer])[0][0]
                interface_mtu = int(command(node, ["cat", "/sys/class/net/" + intf.name + "/mtu"]).strip())
                face_mtu = interface_mtu - 28  # IPv4 and UDP headers.
                if face_mtu < 64:
                    raise RuntimeError("measured interface MTU is too small")
                remote = "udp4://" + NdnRoutingHelper.getNeighborIP(node)[peer] + ":6363"
                command(node, ["nfdc", "face", "create", "remote", remote, "mtu", str(face_mtu)])
                state = command(node, ["nfdc", "face", "list", "remote", remote])
                verify_probe_face_mtu(state, face, remote, face_mtu)
                write_json(output / (node_name + "-probe-face.json"),
                           {"face_id": int(face), "remote": remote, "interface_mtu": interface_mtu,
                            "face_mtu": face_mtu, "readback": state})
            for suffix in ("PROBE-RTT", "PROBE-GOODPUT", "PROBE-READY"):
                prefix = IDENTITIES[peer] + "/NDNSF/RPC/" + suffix
                command(node, ["nfdc", "route", "add", "prefix", prefix,
                               "nexthop", str(face), "cost", "0", "capture"])
                probe_routes.append({"node": node_name, "prefix": prefix, "face": str(face)})
        write_json(output / "probe-routes.json", probe_routes)
        for node_name in ("ps", "po"):
            fib = command(ndn.net[node_name], ["nfdc", "fib", "list"])
            (output / (node_name + "-fib.txt")).write_text(fib)
            for route in (row for row in probe_routes if row["node"] == node_name):
                verify_probe_fib(fib, route["prefix"], route["face"])
        # Keep group traffic on the control star, away from the measured edge.
        for node_name in IDENTITIES:
            node = ndn.net[node_name]
            neighbors = NdnRoutingHelper.getNeighborIP(node)
            destinations = ("ps", "po", "pu") if node_name == "pc" else ("pc",)
            for peer in destinations:
                face = Nfdc.createFace(node, neighbors[peer], protocol="udp")
                if str(face) == "-1":
                    raise RuntimeError("cannot create control face")
                Nfdc.registerRoute(node, ROOT + "/group", str(face))
            Nfdc.setStrategy(node, ROOT + "/group", "multicast")

        certificate = output / "controller.cert"
        certificate.write_text(command(ndn.net["pc"], [tools["ndnsec"], "key-gen", "-t", "r", IDENTITIES["pc"]]))
        schema = output / "trust.conf"
        schema.write_text(trust_schema(certificate))
        policy = output / "policy.info"
        grants = []
        for role, nodes in (("provider", ("ps", "po")), ("user", ("pu",))):
            services = policy_services(role, args.authorization_case)
            entries = ["%s-policy {\nfor %s\nallow {\n%s\n}\n}" %
                       (role, IDENTITIES[node], services) for node in nodes]
            grants.append(role + "-policies {\n" + "\n".join(entries) + "\n}")
        policy.write_text("name " + IDENTITIES["pc"] + "/NDNSF/ControllerPolicy/v1\n" + "\n".join(grants))
        tokens = {node: os.urandom(24).hex() for node in ("ps", "po", "pu")}
        token_file = output / "bootstrap-tokens.txt"
        token_file.write_text("".join("%s %s %s\n" %
                              (IDENTITIES[node], token, "user" if node == "pu" else "provider")
                              for node, token in tokens.items()))
        token_file.chmod(0o600)
        controller_args = [tools["App_ServiceController"],
                           "--controller-prefix", IDENTITIES["pc"], "--policy-file", str(policy),
                           "--trust-schema", str(schema), "--bootstrap-token-file", str(token_file),
                           "--run-for-ms", "900000"]
        trigger = output / "revoke.trigger"
        if args.authorization_case == "revoke":
            controller_args += ["--revoke-trigger-file", str(trigger), "--revoke-kind", "service",
                                "--revoke-identity", IDENTITIES["pu"], "--revoke-service", PROBE,
                                "--revoke-attribute", "/PERMISSION" + PROBE]
        controller = start(ndn.net["pc"], "controller", controller_args)
        deadline = time.monotonic() + 20
        while "NDNSF_CONTROLLER_READY" not in (output / "controller.log").read_text(errors="replace"):
            if controller.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("Controller startup failed")
            time.sleep(0.1)

        common = {"controller": IDENTITIES["pc"], "controller_certificate": str(certificate),
                  "trust_schema": str(schema), "group": ROOT + "/group"}
        providers = []
        for node in ("ps", "po"):
            config = dict(common, role="provider", identity=IDENTITIES[node],
                          bootstrap_token=tokens[node], duration_ms=600000)
            config_path = output / (node + ".json")
            write_json(config_path, config)
            config_path.chmod(0o600)
            providers.append(start(ndn.net[node], node, [tools["DI_NetworkProbeCalibration"], "--config", str(config_path)]))
        deadline = time.monotonic() + 25
        for node, process in zip(("ps", "po"), providers):
            while "NDNSF_PROBE_CALIBRATION_PROVIDER_READY" not in (output / (node + ".log")).read_text(errors="replace"):
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Provider authorization bootstrap failed: " + node)
                time.sleep(0.1)
        campaign_deadline = time.monotonic() + 660
        source_intf, observer_intf = ndn.net["ps"].connectionsTo(ndn.net["po"])[0]
        for index, (bandwidth, delay, configured_burst) in enumerate(matrix):
            if controller.poll() is not None or any(p.poll() is not None for p in providers):
                raise RuntimeError("Controller or Provider exited before measurement")
            cell = output / ("cell-%02d-%dMbps-%dms" % (index + 1, bandwidth, delay))
            cell.mkdir()
            for node, intf in ((ndn.net["ps"], source_intf), (ndn.net["po"], observer_intf)):
                intf.config(bw=bandwidth, delay=str(delay) + "ms", max_queue_size=100)
                mtu = int(command(node, ["cat", "/sys/class/net/" + intf.name + "/mtu"]).strip())
                burst = mtu if configured_burst is None else configured_burst
                command(node, [tools["tc"], "class", "change", "dev", intf.name, "parent", "5:",
                               "classid", "5:1", "htb", "rate", str(bandwidth) + "mbit",
                               "ceil", str(bandwidth) + "mbit", "burst", str(burst), "cburst", str(burst)])
                state = {"interface": intf.name, "mtu": mtu, "bandwidth_mbps": bandwidth,
                         "one_way_delay_ms": delay, "burst": burst, "cburst": burst,
                         "qdisc": json.loads(command(node, [tools["tc"], "-j", "qdisc", "show", "dev", intf.name])),
                         # Ubuntu 20.04 tc supports JSON qdiscs but emits
                         # text for classes. Preserve the detailed readback.
                         "class": {"format": "text", "value": command(node,
                                   [tools["tc"], "-s", "-d", "class", "show", "dev", intf.name])},
                         "link": json.loads(command(node, ["ip", "-j", "-d", "link", "show", "dev", intf.name])),
                         "offload": node_cmd(node, "ethtool -k " + shlex.quote(intf.name) + " 2>&1")}
                write_json(cell / (node.name + "-link.json"), state)
            capture = start(ndn.net["ps"], cell.name + "-capture", [tools["tcpdump"], "--immediate-mode", "-U", "-n", "-i",
                            source_intf.name, "-s", "256", "-w", str(cell / "probe.pcap"), "ip", "proto", "17"])
            observer_capture = start(ndn.net["po"], cell.name + "-observer-capture",
                                     [tools["tcpdump"], "--immediate-mode", "-U", "-n", "-i", observer_intf.name,
                                      "-s", "256", "-w", str(cell / "observer-probe.pcap"),
                                      "ip", "proto", "17"])
            for label, process in ((cell.name + "-capture", capture),
                                   (cell.name + "-observer-capture", observer_capture)):
                capture_deadline = time.monotonic() + 5
                while "listening on" not in (output / (label + ".log")).read_text(errors="replace"):
                    if process.poll() is not None or time.monotonic() >= capture_deadline:
                        raise RuntimeError("probe capture failed to become ready: " + label)
                    time.sleep(0.05)
            result_path = cell / "native-result.json"
            config = dict(common, role="user", identity=IDENTITIES["pu"], bootstrap_token=tokens["pu"],
                          source=IDENTITIES["ps"], observer=IDENTITIES["po"], output=str(result_path),
                          repeats=1 if args.smoke or args.authorization_case else 5,
                          warmup=0 if args.smoke or args.authorization_case else 1,
                          interval_ms=0 if args.smoke or args.authorization_case else 15000,
                          authorization_case=args.authorization_case or "measure",
                          revoke_trigger_file=str(trigger),
                          expected_bandwidth_mbps=bandwidth, require_accuracy=False)
            config_path = cell / "user.json"
            write_json(config_path, config)
            config_path.chmod(0o600)
            user = start(ndn.net["pu"], cell.name + "-user", [tools["DI_NetworkProbeCalibration"], "--config", str(config_path)])
            started = time.monotonic()
            remaining = min(110, campaign_deadline - started)
            if remaining <= 0:
                raise RuntimeError("campaign deadline exhausted")
            try:
                user.wait(timeout=remaining)
            finally:
                stop_process(capture)
                stop_process(observer_capture)
            if user.returncode != 0:
                raise RuntimeError("native calibration failed: " + cell.name)
            result = json.loads(result_path.read_text())
            expected_schema = "ndnsf-network-probe-authorization-v1" if args.authorization_case else "ndnsf-network-probe-calibration-v1"
            if result.get("schema") != expected_schema:
                raise RuntimeError("unexpected native result schema")
            if uses_sustained_reference(args.control, args.authorization_case):
                remaining_campaign_timeout(campaign_deadline, 20)
                source_ip, observer_ip = source_intf.IP(), observer_intf.IP()
                route = command(ndn.net["ps"], ["ip", "-j", "route", "get", observer_ip, "from", source_ip],
                                timeout=remaining_campaign_timeout(campaign_deadline, 10))
                routes = json.loads(route)
                if len(routes) != 1 or routes[0].get("dev") != source_intf.name:
                    raise RuntimeError("reference traffic would not use the measured interface")
                server_argv = [tools["iperf"], "-s", "-u", "-B", observer_ip,
                               "-p", "5001", "-i", "1", "-f", "m"]
                client_argv = [tools["iperf"], "-c", observer_ip, "-u", "-B", source_ip,
                               "-p", "5001", "-l", "1472", "-b", "40M", "-t", "10",
                               "-i", "1", "-f", "m"]
                server = start(ndn.net["po"], cell.name + "-reference-server", server_argv)
                ready_deadline = time.monotonic() + remaining_campaign_timeout(campaign_deadline, 5)
                while True:
                    if server.poll() is not None or time.monotonic() >= ready_deadline:
                        raise RuntimeError("reference UDP server did not become ready")
                    sockets = command(ndn.net["po"], ["ss", "-H", "-u", "-l", "-n"],
                                      timeout=remaining_campaign_timeout(ready_deadline, 5))
                    if observer_ip + ":5001" in sockets.split():
                        break
                    time.sleep(0.05)
                reference_captures = []
                for node, intf in ((ndn.net["ps"], source_intf), (ndn.net["po"], observer_intf)):
                    label = cell.name + "-reference-" + node.name + "-capture"
                    proc = start(node, label, [tools["tcpdump"], "--immediate-mode", "-U", "-n", "-i",
                                 intf.name, "-s", "256", "-w", str(cell / (node.name + "-reference.pcap")),
                                 "ip", "proto", "17"])
                    reference_captures.append(proc)
                    ready_deadline = time.monotonic() + remaining_campaign_timeout(campaign_deadline, 5)
                    while "listening on" not in (output / (label + ".log")).read_text(errors="replace"):
                        if proc.poll() is not None or time.monotonic() >= ready_deadline:
                            raise RuntimeError("reference capture did not become ready")
                        time.sleep(0.05)
                write_json(cell / "reference-config.json", {"schema": "ndnsf-probe-udp-reference-v1",
                           "reference": "UDP_PAYLOAD; separate from NDN_DATA_ONLY",
                           "source_interface": source_intf.name, "observer_interface": observer_intf.name,
                           "source_ip": source_ip, "observer_ip": observer_ip, "route": routes,
                           "server_command": server_argv, "client_command": client_argv})
                client = start(ndn.net["ps"], cell.name + "-reference-client", client_argv)
                try:
                    client.wait(timeout=remaining_campaign_timeout(campaign_deadline, 20))
                    if client.returncode != 0:
                        raise RuntimeError("reference UDP client failed")
                finally:
                    stop_process(server)
                    for proc in reference_captures:
                        stop_process(proc)
                reference = parse_iperf_udp_receiver_summary(
                    (output / (cell.name + "-reference-server.log")).read_text(errors="replace"))
                reference.update({"schema": "ndnsf-probe-udp-reference-result-v1",
                                  "reference": "UDP_PAYLOAD; separate from NDN_DATA_ONLY",
                                  "configured_wire_mbps": bandwidth,
                                  "max_relative_error": 0.10})
                reference["relative_error"] = abs(reference["received_payload_mbps"] / bandwidth - 1)
                reference["fixture_condition_pass"] = reference["relative_error"] <= reference["max_relative_error"]
                write_json(cell / "reference-result.json", reference)
                if args.control is None and not reference["fixture_condition_pass"]:
                    raise RuntimeError("fixture invalid: sustained receiver rate differs from configured HTB rate")
            cells.append({"path": str(cell), "bandwidth_mbps": bandwidth, "delay_ms": delay,
                          "control": args.control, "authorization_case": args.authorization_case,
                          "elapsed_s": time.monotonic() - started, "native_result": result})
            write_json(output / "matrix.json", {"smoke": args.smoke, "control": args.control,
                       "authorization_case": args.authorization_case, "cells": cells,
                       "qualification": "NOT_EVALUATED", "reference": "NDN_DATA_ONLY; outer framing excluded"})
        if not args.smoke and not args.control and not args.authorization_case:
            remaining_campaign_timeout(campaign_deadline, 15)
            matrix_config = output / "matrix-oracle-config.json"
            matrix_result = output / "matrix-oracle-result.json"
            write_json(matrix_config, {"role": "matrix", "cells": cells,
                                       "output": str(matrix_result)})
            oracle = start(ndn.net["pu"], "matrix-oracle",
                           [tools["DI_NetworkProbeCalibration"], "--config", str(matrix_config)])
            oracle.wait(timeout=remaining_campaign_timeout(campaign_deadline, 15))
            result = json.loads(matrix_result.read_text())
            if result.get("schema") != "ndnsf-network-probe-calibration-matrix-v1":
                raise RuntimeError("unexpected native matrix result schema")
            write_json(output / "matrix.json", {"smoke": False, "control": None,
                       "authorization_case": None, "cells": cells,
                       "qualification": "PASS" if result.get("qualification_pass") else "FAIL",
                       "native_matrix_result": result,
                       "reference": "NDN_DATA_ONLY; outer framing excluded"})
            if oracle.returncode != 0 or result.get("qualification_pass") is not True:
                raise RuntimeError("native calibration matrix qualification failed")
    except Exception as error:
        # Observe daemon state before cleanup can alter the failure boundary.
        failure = {"error": str(error), "nfd": []}
        if nfd_manager is not None:
            for app in nfd_manager:
                failure["nfd"].append({"node": app.node.name,
                                       "pid": app.process.pid if app.process else None,
                                       "returncode": app.process.poll() if app.process else None})
        try:
            write_json(output / "failure.json", failure)
        except Exception as evidence_error:
            print("failure evidence write: " + str(evidence_error), file=sys.stderr)
        raise
    finally:
        errors = []
        for label, process in reversed(owned):
            try:
                stop_process(process)
            except Exception as error:
                errors.append(label + ": " + str(error))
            cleanup["processes"].append({"label": label, "pid": process.pid,
                                          "returncode": process.poll()})
        try:
            ndn.stop()
            cleanup["network_stopped"] = True
        except Exception as error:
            errors.append("MiniNDN stop: " + str(error))
        for handle in logs:
            try:
                handle.close()
            except Exception as error:
                errors.append("log close: " + str(error))
        cleanup["errors"] = errors
        try:
            write_json(output / "cleanup.json", cleanup)
        except Exception as error:
            errors.append("cleanup evidence write: " + str(error))
            print(errors[-1], file=sys.stderr)
        if errors and sys.exc_info()[0] is None:
            raise RuntimeError("calibration cleanup failed: " + "; ".join(errors))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--smoke", action="store_true", help="One functional round; no accuracy qualification")
    parser.add_argument("--control", choices=("high-bandwidth", "default-burst", "aligned-face-mtu", "sustained-reference", "sustained-reference-burst"),
                        help="Separate repeated control campaign; cannot combine with --smoke")
    parser.add_argument("--authorization-case", choices=("deny", "revoke"),
                        help="Separate native Controller authorization campaign")
    args = parser.parse_args()
    run(parser, args)
