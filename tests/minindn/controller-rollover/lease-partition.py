#!/usr/bin/env python3
"""Exercise two real ServiceUsers on distinct MiniNDN leaves, during one isolated lease expiry.

Prepare a fresh deployment with tests/fixtures/spec242/prepare-deployment.py --historical.
Run this file against deployment.json with --target /spec242/historical-producer --leaf h242p0.
Only the Controller, Repo and two probe processes run. The source topology, installed binaries,
trust schema, private identities and four distinct NFD sockets come from the declared deployment.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import uuid

from ndn.encoding import Name
from ndnsf_distributed_inference.ops import cluster
from ndnsf_distributed_inference.ops.deployment_config import load_deployment
from ndnsf_distributed_inference.ops.minindn import MiniNdnFacility
from ndnsf_distributed_inference.ops.processes import write_json
from withdrawal import records, require


def same_name(a, b):
    return bool(a and b and Name.normalize(a) == Name.normalize(b))


def observed(log, marker):
    return list(records(log.read_text(errors="replace").splitlines(), marker))


def single(log, marker):
    rows = observed(log, marker)
    require(len(rows) == 1, f"{log.name}: expected one {marker}")
    return rows[0][1]


def positive(path, phase):
    rows = [m for line in path.read_text(errors="replace").splitlines()
            if (m := re.fullmatch(r"LEASE_RESPONSE phase=(\w+) id=(\S+) payload=(\{.*\})", line))
            and m[1] == phase]
    require(len(rows) == 1 and rows[0][2].startswith("/"), f"{path.name}: missing {phase} reply")
    body = json.loads(rows[0][3])
    require(body.get("storageBackend") and body.get("cachePolicy") and
            type(body.get("backingReads")) is int, "reply is not the Repo cache status")
    return rows[0][2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("--target", required=True, help="separately provisioned probe identity")
    parser.add_argument("--leaf", required=True, help="unused leaf dedicated to the isolated User")
    args = parser.parse_args()
    source = args.config.resolve(strict=True)
    root = source.parent
    cfg = load_deployment(source)
    require(cfg["platform"]["kind"] == "minindn", "require a MiniNDN deployment")
    controller, = (a for a in cfg["actors"] if a["kind"] == "controller")
    repo, = (a for a in cfg["actors"] if a["kind"] == "repo")
    control, = (a for a in cfg["actors"] if a["kind"] == "requester")
    target, = (a for a in cfg["actors"] if same_name(a["identity"], args.target))
    actors = [controller, repo, control, target]
    nodes = {n["id"]: n for n in cfg["nodes"]}
    require(args.leaf in nodes and all(n["nfd"]["mode"] == "managed" and
            root in Path(n["nfd"]["socket"]).resolve().parents and not Path(n["nfd"]["socket"]).exists()
            for n in nodes.values()), "need unused, fixture-owned forwarder sockets")
    require(all(not a["external"] and root in Path(a["credential_home"]).resolve().parents
                for a in actors) and len({a["credential_home"] for a in actors}) == 4
            and len({a["id"] for a in actors}) == 4, "need four independently owned actors")
    target["node"] = args.leaf
    require(len({a["node"] for a in actors}) == 4, "selected leaf is shared by another actor")
    controller_state = Path(controller["credential_home"]) / ".ndn/controller-generation.state"
    require(not controller_state.exists(), "use fresh Controller credentials and state")
    prefix = Path(cfg["release"]["prefix"])
    binary = prefix / "bin/ndnsf-policy-lease-probe"
    manifest = Path(cfg["release"]["manifest"])
    entry = json.loads(manifest.read_bytes())["files"].get(str(binary), {}).get("installed", {})
    require(entry.get("kind") == "file" and not binary.is_symlink() and os.access(binary, os.X_OK)
            and entry.get("sha256") == hashlib.sha256(binary.read_bytes()).hexdigest(),
            "installed experiment probe missing or differs from release manifest")
    os.umask(0o077)
    work = root / "lease-partition"
    work.mkdir(mode=0o700)
    release = work / "after-expiry"
    trigger = release.with_suffix(".cut")
    service = cfg["network"]["repo_service"] + "/CACHE_STATUS"
    security = cfg["security"]
    lease_ms, skew_ms = 12000, 250
    policy = work / "lease-policy.info"
    def rule(kind, actor, services):
        permissions = "\n".join(services)
        return f"{kind}-policy {{\nfor {actor['identity']}\nallow {{\n{permissions}\n}}\n}}\n"
    repo_services = [cfg["network"]["repo_service"] + "/" + suffix
                     for suffix in ("STORE", "FETCH", "STATUS", "CACHE_STATUS")]
    policy.write_text("name " + controller["identity"] + "/NDNSF/ControllerPolicy/v1\n"
                      + "provider-policies {\n" + rule("provider", repo, repo_services)
                      + "}\nuser-policies {\n" + rule("user", target, [service])
                      + rule("user", control, [service]) + "}\n")
    cfg["actors"] = actors
    cfg["network"]["routes"] = []  # The facility originates all four actor identities and the group.
    security.update(controller_policy=str(policy), controller_policy_lease_ms=lease_ms,
                    controller_policy_max_lease_ms=lease_ms, controller_policy_clock_skew_ms=skew_ms)
    cfg["storage"]["repo_root"] = str(work / "repo")
    Path(cfg["storage"]["repo_root"]).mkdir(mode=0o700)
    if "controller_recovery_checkpoint" in security:
        require(root in Path(security["controller_recovery_checkpoint"]).resolve().parents and
                not Path(security["controller_recovery_checkpoint"]).exists(), "fresh recovery state required")
    controller["parameters"].update({"revoke-trigger-file": str(trigger), "revoke-kind": "service",
        "revoke-identity": target["identity"], "revoke-service": service,
        "revoke-attribute": "/PERMISSION" + service})
    write_json(work / "configuration.json", cfg)
    state = {"schema": "ndnsf-di-host-operation-v1", "deployment_id": cfg["deployment_id"],
             "configuration_sha256": hashlib.sha256((work / "configuration.json").read_bytes()).hexdigest(),
             "boot_id": uuid.uuid4().hex, "status": "STARTING", "launches": [],
             "evidence_root": str(work)}
    result = {"status": "FAIL", "configSha256": state["configuration_sha256"],
              "manifestSha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
              "probeSha256": entry["sha256"], "leaseMs": lease_ms, "clockSkewMs": skew_ms}
    children = []
    probes = {}
    deadline = time.monotonic() + 150
    facility = MiniNdnFacility(cfg, work, state, deadline)

    def guard():
        cluster._check_running(children)
        require(time.monotonic() < deadline, "deployment deadline expired")

    def wait(check, seconds, reason):
        end = min(time.monotonic() + seconds, deadline)
        while time.monotonic() < end:
            guard()
            value = check()
            if value:
                return value
            for identity, proc in probes.items():
                require(proc.poll() in (None, 0), f"probe failed: {identity} ({proc.returncode})")
            time.sleep(0.05)
        raise RuntimeError(reason)

    def spawn(actor_id, kind, argv, env, node):
        return cluster._spawn(state, work, actor_id, kind, argv, env, guard,
                              launcher=facility.launcher(node), network_namespace=facility.namespace(node))

    def link(command):
        proc = facility.launcher(args.leaf)(["ip", *command], stdin=subprocess.DEVNULL,
                                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            output, error = proc.communicate(timeout=4)
            require(proc.returncode == 0, f"target link: {error.decode(errors='replace')}")
            return output
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=2)

    try:
        facility.start(guard)
        for node in cfg["nodes"]:
            directory = work / ("forwarder-" + node["id"])
            directory.mkdir()
            cluster._start_forwarder(cfg, node, state, work, directory, children, deadline, guard, spawn)
        facility.routes(work, guard)
        interfaces = [x.name for x in facility.hosts[nodes[args.leaf]["host"]].intfList() if x.link]
        require(len(interfaces) == 1, "target must be a leaf with exactly one external link")
        iface, = interfaces
        def is_up():
            row, = json.loads(link(["-j", "link", "show", "dev", iface]))
            return "UP" in row["flags"]
        require(is_up(), "target interface not up initially")
        for actor in (controller, repo):
            argv, env = cluster._native_launch(cfg, actor, work, state)
            env.update(NDN_LOG="*=NONE:ndn_service_framework.ServiceController=WARN",
                       NDNSF_POLICY_REVALIDATION_PERIOD_MS="1000")
            child, row = spawn(actor["id"], actor["kind"], argv, env, actor["node"])
            children.append((actor, child, row))
            wait(lambda: cluster._ready(row, actor, state), 25, actor["id"] + " never ready")
        for actor, mode in ((target, "isolated"), (control, "control")):
            env = cluster._environment(actor["credential_home"], nodes[actor["node"]]["nfd"]["socket"],
                                       actor["id"], state)
            env.update(NDN_LOG="*=NONE:ndn_service_framework.ServiceUser=WARN",
                       NDNSF_CONTROLLER_POLICY_MAX_LEASE_MS=str(lease_ms),
                       NDNSF_CONTROLLER_POLICY_CLOCK_SKEW_MS=str(skew_ms),
                       NDNSF_POLICY_REVALIDATION_PERIOD_MS="1000")
            if mode == "isolated":
                env.update(NDNSF_PERSIST_RUNTIME_STATE="1", NDNSF_RUNTIME_STATE_DIR=str(work / "accepted"))
            argv = [str(binary), actor["identity"], cfg["network"]["group"], repo["identity"], service,
                    security["controller_certificate"], security["trust_schema"], str(release), mode]
            child, row = spawn(actor["id"], "requester", argv, env, actor["node"])
            probes[actor["id"]] = child
        target_log, control_log, issuer_log = (work / (a["id"] + ".log") for a in (target, control, controller))
        for path in (target_log, control_log):
            wait(lambda: observed(path, "LEASE_READY"), 40, f"no authorized pre-cut request: {path}")
            positive(path, "before")
        old = single(target_log, "LEASE_READY")["version"]
        require(old == single(control_log, "LEASE_READY")["version"], "baseline versions differ")
        link(["link", "set", "dev", iface, "down"])
        try:
            require(not is_up(), "failed to isolate target leaf")
            cut_ns = time.time_ns()
            trigger.touch(exist_ok=False)
            wait(lambda: observed(target_log, "LEASE_SIGNED"), 5, "missing signed installed status")
            signed = single(target_log, "LEASE_SIGNED")
            expiry_ms = int(signed["expiryMs"])
            conservative_end = (expiry_ms - skew_ms) * 1_000_000
            require(signed["version"] == old and expiry_ms - int(signed["fromMs"]) == lease_ms
                    and int(signed["fromMs"]) * 1_000_000 <= cut_ns < conservative_end,
                    "target did not hold a live signed lease at partition")
            def applied():
                for stamp, revoked in observed(issuer_log, "NDNSF_CONTROLLER_REVOKED"):
                    if (stamp is None or stamp < cut_ns or revoked.get("kind") != "3"
                            or not same_name(revoked.get("identity"), target["identity"])
                            or not same_name(revoked.get("service"), service)):
                        continue
                    for _, item in observed(issuer_log, "NDNSF_REVOCATION_APPLIED"):
                        if (item.get("success") == "1" and item.get("kind") == "3" and
                                all(item.get(k) == revoked.get(k) for k in ("generation", "epoch"))):
                            return stamp, item["generation"] + ":" + item["epoch"]
            revoke_ns, new = wait(applied, 12, "Controller did not commit the scoped revocation")
            require(cut_ns <= revoke_ns < conservative_end, "withdrawal arrived after target lease expiry")
            wait(lambda: time.time_ns() >= conservative_end, 16, "signed lease deadline not reached")
            release.touch(exist_ok=False)
            for ident, proc in probes.items():
                require(proc.wait(timeout=20) == 0, "probe exit failed: " + ident)
            after = target_log.read_text(errors="replace").partition("LEASE_ATTEMPT phase=after ")
            attempt = [r for _, r in observed(target_log, "LEASE_ATTEMPT") if r["phase"] == "after"]
            require(after[1] and len(attempt) == 1 and attempt[0]["version"] == old
                    and 0 <= int(attempt[0]["atMs"]) - (expiry_ms - skew_ms) <= 250,
                    "target did not attempt under the old version at the signed deadline")
            # The native return and its log use different streams. Check both
            # observations after the attempt, independent of flush order.
            subsequent = after[2].splitlines()
            refused = subsequent.count("LEASE_REFUSED phase=after")
            reasons = [row for _, row in records(subsequent, "NDNSF_USER_REVOCATION_REJECT")
                       if same_name(row.get("service"), service) and row.get("transition") == "0"]
            require(refused == 1 and len(reasons) == 1 and reasons[0].get("reason") == "controller_status_expired",
                    "ordinary RequestService did not refuse synchronously for the installed lease")
            expiry = [stamp for stamp, row in observed(target_log, "NDNSF_POLICY_LEASE_EXPIRED")
                      if stamp is not None and cut_ns < stamp and same_name(row.get("service"), service)
                      and row.get("role") == "user" and row.get("version") == old
                      and int(row["expiryMs"]) == expiry_ms]
            require(len(expiry) == 1 and abs(expiry[0] - conservative_end) <= 250_000_000,
                    "native timer did not expire the verified signed lease")
            require(not is_up() and any(row.get("phase") == "after" and row.get("version") == new
                    for _, row in observed(control_log, "LEASE_ATTEMPT")),
                    "control did not act under the Controller's advanced version")
            result.update(cutNs=cut_ns, committedNs=revoke_ns, expiryEventNs=expiry[0],
                          signedValidUntilMs=expiry_ms, oldVersion=old, newVersion=new,
                          attemptedMs=int(attempt[0]["atMs"]), signedStatusSha256=hashlib.sha256(
                              (work / (trigger.name + ".status.ndn")).read_bytes()).hexdigest(),
                          controlRequest=positive(control_log, "after"), targetInterface=iface)
        finally:
            link(["link", "set", "dev", iface, "up"])
            require(is_up(), "could not restore isolated interface")
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        try:
            closed = cluster._close(state, work, facility)
            by_id = {row["id"]: row for row in state["cleanup"]}
            require(closed["status"] == "STOPPED" and state["facility"]["status"] == "STOPPED"
                    and all(row["status"] == "ZERO" and row.get("forced") is False
                            for row in [*state["cleanup"], *state["facility"]["cleanup"]]),
                    "deployment failed clean closeout")
            if "failure" not in result:
                codes = {actor["id"]: by_id[actor["id"]].get("exitCode") for actor in (controller, repo)}
                codes.update({actor_id: probe.returncode for actor_id, probe in probes.items()})
                require(len(codes) == 4 and all(code == 0 for code in codes.values()),
                        f"required actors have missing or abnormal exits: {codes}")
                result["actorExitCodes"] = codes
                result["status"] = "PASS"
        except Exception as error:
            if "failure" not in result:
                result["failure"] = str(error)
                raise
            result["cleanupFailure"] = str(error)
        finally:
            write_json(work / "result.json", result)


if __name__ == "__main__":
    main()
