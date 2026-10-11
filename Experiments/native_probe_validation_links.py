"""Opt-in link fixture for measuring packet trains during native inference."""

import json
import itertools
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def render_anchored_probe_validation_schema(application_root, certificates):
    """Validate experiment packets against explicitly provisioned certificates."""
    components = application_root.strip("/").split("/")
    if (not application_root.startswith("/") or
            any(not re.fullmatch(r"[A-Za-z0-9_.-]+", item) for item in components)):
        raise ValueError("invalid probe validation application namespace")
    anchors = list(dict.fromkeys(str(Path(path).resolve()) for path in certificates))
    if not anchors:
        raise ValueError("probe validation requires certificate anchors")
    key_pattern = "^" + "".join("<" + re.escape(item) + "|8=" + re.escape(item) + ">" for item in components)
    key_pattern += "[^<KEY|8=KEY>]*<KEY|8=KEY><>{1,3}$"
    checkers = "\n".join(
        'checker { type customized sig-type %s '
        'key-locator { type name regex %s } }' % (kind, json.dumps(key_pattern))
        for kind in ("rsa-sha256", "ecdsa-sha256"))
    # Data stays beneath its actual original Producer. Cross-actor RPC
    # Interests target the responder and retain explicit caller admission.
    # Installed RegexComponentMatcher uses the default URI display. Reproduce
    # hierarchical identity-prefix admission with both Generic KEY displays.
    relation = ('key-locator { type name hyper-relation { '
        'k-regex %s k-expand %s h-relation is-prefix-of p-regex %s p-expand %s } }' %
        (json.dumps("^(<>*)<KEY|8=KEY><>{1,3}$"), json.dumps("\\1"),
         json.dumps("^(<>*)$"), json.dumps("\\1")))
    data_checkers = "\n".join(
        'checker { type customized sig-type %s %s }' % (kind, relation)
        for kind in ("rsa-sha256", "ecdsa-sha256"))
    rules = ['rule { id "anchored experiment data" for data %s }' % data_checkers,
             'rule { id "anchored experiment interest" for interest %s }' % checkers]
    rules.extend('trust-anchor { type file file-name %s }' % json.dumps(path)
                 for path in anchors)
    return "\n".join(rules) + "\n"


def configure_probe_validation_links(ndn, output, bandwidth_mbps, providers):
    if bandwidth_mbps not in (1, 4, 10):
        raise ValueError("probe validation bandwidth must be 1, 4, or 10 Mbps")
    tc = shutil.which("tc")
    mnexec = shutil.which("mnexec")
    if tc is None or mnexec is None:
        raise RuntimeError("probe validation requires tc and mnexec")
    nfdc = shutil.which("nfdc")
    nodes = {node.name: node for node in ndn.net.hosts}
    if (nfdc is None or not isinstance(providers, dict) or not 2 <= len(providers) <= 4 or
            not set(providers).issubset(nodes) or
            any(not isinstance(p, str) or not p.startswith("/") or p.endswith("/") or
                len(p) > 256 for p in providers.values()) or
            len(set(providers.values())) != len(providers)):
        raise ValueError("probe validation requires two to four unique Provider bindings")
    from mininet.link import TCLink
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def command(node, argv, timeout=15):
        env = os.environ.copy()
        env["HOME"] = node.params["params"]["homeDir"]
        result = subprocess.run([mnexec, "-a", str(node.pid), *argv],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=timeout, check=True, env=env)
        return result.stdout.decode(errors="replace")

    original_fibs = {name: command(nodes[name], [nfdc, "fib", "list"])
                     for name in providers}
    original_interfaces = {name: [(i, i.IP()) for i in nodes[name].intfList()]
                           for name in providers}
    routes = {name: [] for name in providers}
    for index, (left, right) in enumerate(itertools.combinations(sorted(providers), 2), 1):
        link = ndn.net.addLink(nodes[left], nodes[right], cls=TCLink,
                              bw=bandwidth_mbps, delay="1ms", max_queue_size=100)
        ips = ["10.252." + str(index) + ".1", "10.252." + str(index) + ".2"]
        for intf, ip in zip((link.intf1, link.intf2), ips):
            intf.setIP(ip + "/30")
        for intf, ip, remote_ip in ((link.intf1, ips[0], ips[1]), (link.intf2, ips[1], ips[0])):
            node = intf.node
            mtu = int(command(node, ["cat", "/sys/class/net/" + intf.name + "/mtu"]).strip())
            if mtu <= 0:
                raise RuntimeError("invalid probe validation interface MTU")
            rate = str(bandwidth_mbps) + "Mbit"
            command(node, [tc, "class", "replace", "dev", intf.name,
                           "parent", "5:", "classid", "5:1", "htb",
                           "rate", rate, "ceil", rate,
                           "burst", str(mtu), "cburst", str(mtu)])
            classes = command(node, [tc, "-s", "-d", "class", "show", "dev", intf.name])
            qdiscs = command(node, [tc, "-j", "qdisc", "show", "dev", intf.name])
            if ("class htb 5:1" not in classes or "rate " + rate not in classes or
                    "ceil " + rate not in classes or
                    not re.search(r"\bburst " + str(mtu) + r"b(?:/|\s)", classes) or
                    not re.search(r"\bcburst " + str(mtu) + r"b(?:/|\s)", classes)):
                raise RuntimeError("probe validation HTB class readback mismatch")
            peer = intf.link.intf2 if intf.link.intf1 is intf else intf.link.intf1
            record = {"node": node.name, "interface": intf.name,
                      "peer_node": peer.node.name, "peer_interface": peer.name,
                      "direction": "egress", "bandwidth_mbps": bandwidth_mbps,
                      "delay": "1ms", "local_ip": ip, "remote_ip": remote_ip,
                      "mtu": mtu, "requested_burst_bytes": mtu,
                      "requested_cburst_bytes": mtu,
                      "classes": classes, "qdiscs": json.loads(qdiscs)}
            remote_uri = "udp4://" + remote_ip + ":6363"
            # Keep UDP datagrams within the interface MTU so packet trains
            # use NDNLP fragmentation rather than IPv4 fragmentation.
            face_mtu = mtu - 28
            if face_mtu < 64:
                raise RuntimeError("probe validation interface MTU is too small")
            face_reply = command(node, [nfdc, "face", "create", "remote", remote_uri,
                                        "mtu", str(face_mtu)])
            match = re.search(r"\bid=(\d+)\b", face_reply)
            if match is None:
                raise RuntimeError("probe validation face creation failed")
            face = match.group(1)
            face_state = command(node, [nfdc, "face", "list", "remote", remote_uri,
                                       "local", "udp4://" + ip + ":6363"])
            entries = [line for line in face_state.splitlines() if "faceid=" in line]
            if (len(entries) != 1 or
                    not re.search(r"\bfaceid=" + face + r"(?:\s|$)", entries[0]) or
                    not re.search(r"\bremote=" + re.escape(remote_uri) + r"(?:\s|$)", entries[0]) or
                    not re.search(r"\blocal=" + re.escape("udp4://" + ip + ":6363") +
                                  r"(?:\s|$)", entries[0]) or
                    not re.search(r"\bmtu=" + str(face_mtu) + r"(?:\s|$)", entries[0])):
                raise RuntimeError("probe validation face endpoint/MTU readback mismatch")
            record.update(face_id=int(face), face=face_state, face_mtu=face_mtu)
            for suffix in ("PROBE-RTT", "PROBE-GOODPUT", "PROBE-READY"):
                prefix = providers[peer.node.name] + "/NDNSF/RPC/" + suffix
                command(node, [nfdc, "route", "add", "prefix", prefix,
                               "nexthop", face, "cost", "0", "capture"])
                # RIB acceptance precedes the asynchronous FIB update.
                deadline = time.monotonic() + 3
                fib = ""
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise RuntimeError("probe validation FIB is not exclusive: node=" + node.name +
                                           " prefix=" + prefix + " expected_face=" + face +
                                           " readback=" + repr(fib))
                    fib = command(node, [nfdc, "fib", "list"], timeout=remaining)
                    entries = [line for line in fib.splitlines()
                               if line.split() and line.split()[0] == prefix]
                    if len(entries) == 1 and re.findall(r"faceid=(\d+)", entries[0]) == [face]:
                        break
                    time.sleep(min(0.05, max(0, deadline - time.monotonic())))
                routes[node.name].append(prefix)
            records.append(record)
    after_fibs = {name: command(nodes[name], [nfdc, "fib", "list"]) for name in providers}
    for name in providers:
        after = [line for line in after_fibs[name].splitlines()
                 if not line.split() or line.split()[0] not in routes[name]]
        if sorted(after) != sorted(original_fibs[name].splitlines()):
            raise RuntimeError("probe validation modified an existing FIB entry")
        if any(intf.IP() != ip for intf, ip in original_interfaces[name]):
            raise RuntimeError("probe validation modified an existing interface IP")
    (output / "links.json").write_text(json.dumps({
        "schema": "ndnsf-probe-validation-links-v2", "links": records,
        "original_fibs": original_fibs, "after_fibs": after_fibs
    }, sort_keys=True, indent=2) + "\n")
    return records
