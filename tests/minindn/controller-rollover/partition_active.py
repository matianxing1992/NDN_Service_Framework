"""Hold authentic data on the MiniNDN Requester's incoming link beyond its signed lease."""

import json
import os
import re
import subprocess
import time
from contextlib import ExitStack
from decimal import Decimal
from pathlib import Path

from withdrawal import committed_revocation, denied_actor_exits, records, require


LEASE_MS = 5000
SKEW_MS = 250
DELAY_MS = 7200


def configure(config, root, controller):
    user = next(a for a in config["actors"] if a["kind"] == "requester")
    service = config["network"]["service"]
    controller["parameters"].update({"revoke-identity": user["identity"], "revoke-attribute": "/PERMISSION" + service})
    config["security"].update(controller_policy_lease_ms=LEASE_MS, controller_policy_max_lease_ms=LEASE_MS,
                              controller_policy_clock_skew_ms=SKEW_MS)
    # Keep transport retries alive through the signed lease, while individual
    # PIT entries for the retired stream expire before the delayed wire arrives.
    config["requester"]["request"].update(policy_revalidation_period_ms=1000,
                                          interest_lifetime_ms=1000, max_event_retries=8)
    config["requester"].update(test_only=True, test_stream_barrier={
        "ready_file": str(root / "stream-ready"), "release_file": str(root / "stream-release"), "timeout_ms": 45000})


def net_fd(stack, state, node_id):
    from ndnsf_distributed_inference.ops.processes import owned_state

    facility = state["facility"]
    node = facility["nodes"][node_id]
    shell, = [row for row in facility["launches"] if row["id"] == node["shell"]]
    proof = shell["proof"]
    require(proof["bindings"]["NDNSF_DEPLOYMENT_BOOT_ID"] == state["boot_id"] and
            proof["bindings"]["NDNSF_DEPLOYMENT_ACTOR_ID"] == node["shell"] and
            shell["network_namespace"] == node["network_namespace"] and owned_state(proof) == "RUNNING",
            "target namespace owner or boot mismatch")
    fd = os.open("/proc/" + str(proof["pid"]) + "/ns/net", os.O_RDONLY)
    stack.callback(os.close, fd)
    pinned = os.readlink("/proc/self/fd/" + str(fd))
    require(pinned == node["network_namespace"] and pinned != os.readlink("/proc/self/ns/net")
            and owned_state(proof) == "RUNNING", "MiniNDN namespace changed while pinning")
    return fd


def network(fd, *command):
    run = subprocess.run(["nsenter", "--net=/proc/self/fd/" + str(fd), "--", *command],
                         pass_fds=(fd,), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=4)
    require(run.returncode == 0, "MiniNDN link command failed: " + run.stderr.strip())
    return run.stdout


def watcher(path, marker):
    """Read each complete log record once, including records appended between polls."""
    offset = 0
    def poll():
        nonlocal offset
        if not path.exists():
            return []
        result = []
        with path.open("rb") as stream:
            stream.seek(offset)
            while True:
                line = stream.readline()
                if not line.endswith(b"\n"):
                    return result
                offset = stream.tell()
                if marker in line:
                    result.append(line.decode(errors="replace"))
    return poll


def event_name(line, binding):
    nonce = binding["requestId"].rsplit("/", 1)[-1]
    fragment = "/request/" + nonce + "/attempt/" + binding["attemptEpoch"] + "/stream/"
    match = re.search(r"(?:data=|<D )(/[^\s?]+)", line)
    if match and fragment in match[1] and "/NDNSF/event/service/" in match[1] and "/seq=" in match[1]:
        return match[1]
    return None


def timestamp_ns(line):
    match = re.match(r"^(\d{10}\.\d+)\s", line)
    return int(Decimal(match[1]) * 1_000_000_000) if match else None


def partition(root, config, evidence, trigger, binding, actors):
    from ndnsf_distributed_inference.ops.processes import owned_state

    user = next(a for a in config["actors"] if a["kind"] == "requester")
    service = config["network"]["service"]
    state = json.loads((Path(config["storage"]["state_root"]) / config["deployment_id"] / "state.json").read_text())
    require(state["boot_id"] == evidence.name and state["status"] == "REQUEST_RUNNING",
            "active boot identity mismatch")
    app, = [row for row in state["launches"] if row["id"] == user["id"]]
    leaf = user["node"]
    nodes = state["facility"]["nodes"]
    require(app["proof"]["bindings"]["NDNSF_DEPLOYMENT_ACTOR_ID"] == user["id"] and
            app["proof"]["bindings"]["NDNSF_DEPLOYMENT_BOOT_ID"] == state["boot_id"] and
            app["network_namespace"] == nodes[leaf]["network_namespace"] and owned_state(app["proof"]) == "RUNNING",
            "cold Requester PID or leaf does not match managed process")
    topology = Path(config["platform"]["topology"]).read_text().split("[links]", 1)[1].split("[", 1)[0]
    edges = [line.split()[0].split(":") for line in topology.splitlines() if line.strip() and
             not line.lstrip().startswith("#")]
    remote, = [end for edge in edges if nodes[leaf]["host"] in edge for end in edge if end != nodes[leaf]["host"]]
    relay, = [name for name, node in nodes.items() if node["host"] == remote]
    expiry_log = watcher(evidence / (user["id"] + ".log"), b"NDNSF_POLICY_LEASE_EXPIRED ")
    late_log = watcher(evidence / ("nfd." + leaf + ".log"), b"onDataUnsolicited")
    expiry_log()
    late_log()
    with ExitStack() as stack:
        user_fd, relay_fd = net_fd(stack, state, leaf), net_fd(stack, state, relay)
        def links(fd):
            return [row for row in json.loads(network(fd, "ip", "-j", "-d", "link", "show"))
                    if row.get("linkinfo", {}).get("info_kind") == "veth"]
        user_link, = links(user_fd)
        peer, = [row for row in links(relay_fd) if row["ifindex"] == user_link["link_index"] and
                 row["link_index"] == user_link["ifindex"]]
        iface = peer["ifname"]
        show = ["tc", "qdisc", "show", "dev", iface]
        previous = network(relay_fd, *show)
        # This fixture uses Mininet's TCLink leaf with only delay and a queue limit.
        rule = re.search(r"(?m)^qdisc netem (\S+) parent (\S+) limit (\d+) delay "
                         r"(\d+(?:\.\d+)?(?:ns|us|ms|s))\s*$", previous)
        require(rule, "cannot restore this leaf's qdisc exactly: " + previous.strip())
        edit = ["tc", "qdisc", "change", "dev", iface, "parent", rule[2], "handle", rule[1],
                "netem", "limit", rule[3], "delay"]
        network(relay_fd, *edit, str(DELAY_MS) + "ms")
        cut = time.time_ns()
        try:
            trigger.touch(exist_ok=False)
            limit = time.monotonic() + 18
            expired = None
            late = None
            while time.monotonic() < limit:
                for stamp, row in records(expiry_log(), "NDNSF_POLICY_LEASE_EXPIRED"):
                    if stamp and stamp >= cut and row.get("role") == "user" and row.get("service") == service:
                        require(expired is None, "duplicate target lease expiry")
                        expired = (stamp, row)
                for line in late_log():
                    name = event_name(line, binding)
                    stamp = timestamp_ns(line)
                    if expired and name and stamp and stamp >= expired[0] and " decision=drop" in line:
                        late = (stamp, name)
                        break
                if late:
                    break
                time.sleep(0.02)
            require(expired, "no target-scope lease expiry on the isolated Requester")
            require(late, "no genuine late stream Data dropped at the target NFD for this attempt")
        finally:
            try:
                network(relay_fd, *edit, rule[4])
                require(network(relay_fd, *show) == previous, "leaf qdisc was not restored")
            finally:
                (root / "stream-release").touch(exist_ok=False)
    observation = dict(cutNs=cut, expiryEventNs=expired[0], expiryMs=int(expired[1]["expiryMs"]),
                oldVersion=expired[1]["version"], lateDropNs=late[0], lateName=late[1],
                restoreNs=time.time_ns(), leafNode=leaf, leafPid=app["proof"]["pid"],
                leafNamespace=app["network_namespace"], relayNode=relay, relayInterface=iface)
    with (evidence / "partition-observation.json").open("x") as output:
        json.dump(dict(observation, **binding), output)
    return observation


def check_status_blackout(versions, packets, old_version, cut, committed, expired, issued):
    require(any(stamp <= cut and version == old_version for stamp, version in versions) and
            all(version == old_version for stamp, version in versions if cut < stamp <= expired) and
            issued <= cut, "Requester accepted a new target lease during the partition")
    # Face dispatch and validation of a packet already inside the leaf can finish
    # after tc changes the link. Measure external delivery at the leaf forwarder.
    before = [stamp for stamp in packets if stamp <= cut]
    require(before and not any(committed <= stamp <= expired for stamp in packets),
            "Controller policy Data reached the isolated leaf between revocation and expiry")
    return max(before)


def verify(config, evidence, actors, binding, entered, probe):
    from admission import actor_log, json_rows

    user = next(a for a in config["actors"] if a["kind"] == "requester")
    controller = next(a for a in config["actors"] if a["kind"] == "controller")
    service = config["network"]["service"]
    exits = denied_actor_exits(config, json.loads((evidence / "operation.json").read_text()), user["id"])
    generation, epoch, committed = committed_revocation(actor_log(evidence, controller),
        user["identity"], service, probe["cutNs"])
    deadline = (probe["expiryMs"] - SKEW_MS) * 1_000_000
    require(entered < probe["cutNs"] <= committed < deadline and abs(probe["expiryEventNs"] - deadline)
            <= 250_000_000 and probe["oldVersion"] != generation + ":" + epoch,
            "withdrawal/expiry interval does not bind the live old lease")
    observations = actor_log(evidence, user)
    versions = [(t, r["generation"] + ":" + r["epoch"]) for t, r in records(observations, "Installed PolicyStatus")
                if t and r.get("service") == service and t <= probe["expiryEventNs"]]
    require(config["security"]["controller_policy_lease_ms"] == LEASE_MS and
            config["security"]["controller_policy_max_lease_ms"] == LEASE_MS and
            config["security"]["controller_policy_clock_skew_ms"] == SKEW_MS,
            "installed lease or skew differs from the partition fixture")
    prefix = " data=" + controller["identity"] + "/NDNSF/RPC/POLICY-STATUS/"
    with (evidence / ("nfd." + user["node"] + ".log")).open(errors="replace") as stream:
        packets = [timestamp_ns(line) for line in stream if "onIncomingData in=" in line and prefix in line]
    issued = (probe["expiryMs"] - LEASE_MS) * 1_000_000
    last_policy = check_status_blackout(versions, packets, probe["oldVersion"], probe["cutNs"],
                                        committed, probe["expiryEventNs"], issued)
    terminal, = json_rows(observations, "NDNSF_DI_REQUEST_TERMINAL")
    require(terminal["requestId"] == binding["requestId"] and terminal["attemptEpoch"] == int(binding["attemptEpoch"])
            and terminal["code"] == "NATIVE_STREAM_FAILED" and terminal["boundary"] == "stream"
            and "Controller policy lease expired for " + service in terminal["message"]
            and terminal["epochMs"] >= probe["lateDropNs"] // 1_000_000,
            "Requester did not report the original lease terminal after the late delivery")
    with (evidence / (user["id"] + ".log")).open(errors="replace") as stream:
        timing = [line for line in stream if binding["requestId"] in line and
                  ("phase=tokenReceived " in line or "event=CANCELLED_OR_DROPPED " in line)]
    arrivals = [int(row["timestamp_us"]) * 1000 for _, row in records(timing, "NDNSF_PHASE_TIMING")
                if row.get("phase") == "tokenReceived" and row.get("executionRole") == "requester" and
                row.get("requestId") == binding["requestId"] and row.get("attempt") == binding["attemptEpoch"]]
    drops = [row for _, row in records(timing, "NDNSF_CONTROL_TIMING")
             if row.get("requestId") == binding["requestId"] and row.get("serviceName") == service and
             row.get("event") == "CANCELLED_OR_DROPPED"]
    require(len(drops) == 1 and drops[0].get("cleanupReason") == "controller_policy_lease_expired",
            "Requester did not terminate this invocation exactly once for lease expiry")
    cancelled = int(drops[0]["timestamp_us"]) * 1000
    require(arrivals and min(arrivals) <= probe["cutNs"] and max(arrivals) < deadline and
            abs(cancelled - deadline) <= 250_000_000 and cancelled < probe["lateDropNs"],
            "missing exact-attempt last admission or unique lease cancellation before late Data")
    # Match the full name at both ends so that an unrelated dropped packet cannot pass.
    emitted = []
    rank_terminals = {}
    for actor in config["actors"]:
        if actor["kind"] != "provider":
            continue
        stages = [(t, row) for t, row in records(actor_log(evidence, actor), "NDNSF_DI_PROVIDER_STAGE")
                  if row.get("requestId") == binding["requestId"] and row.get("planDigest") == binding["planDigest"]
                  and row.get("attemptEpoch") == binding["attemptEpoch"] and row.get("provider") == actor["identity"]]
        entered_stages = [t for t, row in stages if t and row.get("stage") == "EXECUTION_ENTERED"]
        ends = [(t, row) for t, row in stages if row.get("stage") == "TERMINAL"]
        require(entered_stages and min(entered_stages) <= probe["cutNs"] and len(ends) == 1 and
                ends[0][0] and ends[0][0] >= probe["cutNs"], "rank was not active or did not drain once")
        rank_terminals[actor["id"]] = {"timeNs": ends[0][0], "status": ends[0][1]["status"]}
        if probe["lateName"].startswith(actor["identity"] + "/NDNSF/event/"):
            with (evidence / (actor["id"] + ".log")).open(errors="replace") as stream:
                emitted = [timestamp_ns(line) for line in stream if "<D " in line and
                           event_name(line, binding) == probe["lateName"]]
    require(any(t and t <= probe["lateDropNs"] for t in emitted), "late Data has no original Provider publication")
    require(all(not Path(turn[field]).exists() for turn in config["requester"]["turns"]
                for field in ("output_file", "checkpoint_output_file")), "late Data produced a checkpoint or output")
    return dict(probe, **binding, executionEnteredNs=entered, generation=generation, epoch=epoch, committedNs=committed,
                leaseMs=LEASE_MS, skewMs=SKEW_MS, delayMs=DELAY_MS, actorExitCodes=exits,
                lastPolicyIngressBeforeCutNs=last_policy, lastLeaseIssuedNs=issued,
                lastAdmissionNs=max(arrivals), cancellationNs=cancelled, rankTerminals=rank_terminals,
                requestTerminal=terminal, evidence=str(evidence))
