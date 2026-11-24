#!/usr/bin/env python3
"""Check a live grant and a retired DKEY continuation from another MiniNDN node.

Prepare a fresh Spec242 --historical deployment and pass its deployment.json.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import uuid

from Cryptodome.PublicKey import RSA
from ndn.app_support.security_v2 import parse_certificate
from ndn.encoding import Component, Name, parse_data
from ndn.security.validator.known_key_validator import verify_rsa
from ndnsf_distributed_inference.ops import cluster
from ndnsf_distributed_inference.ops.deployment_config import load_deployment
from ndnsf_distributed_inference.ops.minindn import MiniNdnFacility
from ndnsf_distributed_inference.ops.processes import write_json
from withdrawal import records, require


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    source = parser.parse_args().config.resolve(strict=True)
    root = source.parent
    cfg = load_deployment(source)
    nodes = {n["id"]: n for n in cfg["nodes"]}
    controller, repo, reader = (next(a for a in cfg["actors"] if a["kind"] == kind)
                                for kind in ("controller", "repo", "requester"))
    actors = (controller, repo, reader)
    security = cfg["security"]
    checkpoint = Path(security["controller_recovery_checkpoint"]).resolve()
    require(cfg["platform"]["kind"] == "minindn" and len({a["node"] for a in actors}) == 3
            and all(not a["external"] and root in Path(a["credential_home"]).resolve().parents for a in actors)
            and all(n["nfd"]["mode"] == "managed" and root in Path(n["nfd"]["socket"]).resolve().parents
                    and not Path(n["nfd"]["socket"]).exists() for n in nodes.values())
            and root in checkpoint.parents and not checkpoint.exists()
            and not (Path(controller["credential_home"]) / ".ndn/controller-generation.state").exists(),
            "fresh fixture-owned nodes, credentials and checkpoint required")
    prefix = Path(cfg["release"]["prefix"]) / "bin"
    probe = prefix / "ndnsf-policy-lease-probe"
    ledger = Path(cfg["release"]["manifest"]).read_bytes()
    files = json.loads(ledger)["files"]
    for exe in (probe, prefix / "App_ServiceController", prefix / "DistributedRepoServer"):
        require(files[str(exe)]["installed"]["sha256"] == digest(exe.read_bytes()), f"candidate mismatch: {exe}")
    peek = shutil.which("ndnpeek")
    require(peek, "ndnpeek is required")
    pin = parse_certificate(base64.b64decode(Path(security["controller_certificate"]).read_bytes()))
    pubkey = RSA.import_key(bytes(pin.content))
    recipient = parse_certificate(base64.b64decode((root / (reader["id"] + ".cert")).read_bytes()))
    require(recipient.name[:-4] == Name.normalize(reader["identity"]), "recipient identity mismatch")
    os.umask(0o077)
    work = root / "grant-refresh"
    work.mkdir(mode=0o700)
    service_root = cfg["network"]["repo_service"]
    status_service = service_root + "/CACHE_STATUS"
    granted = service_root + "/FETCH"
    # Use a real segment after segment zero. A one-segment pre-grant key cannot test continuation.
    original = [status_service, service_root + "/STATUS", service_root + "/STORE"]
    original += [service_root + "/grant-fixture/" + str(i) for i in range(8)]
    policy = work / "policy.info"
    def rule(role, actor, services):
        return f'{role}-policy {{\nfor {actor["identity"]}\nallow {{\n' + "\n".join(services) + "\n}\n}\n"
    policy.write_text("name " + controller["identity"] + "/NDNSF/ControllerPolicy/v1\nprovider-policies {\n"
                      + rule("provider", repo, original + [granted]) + "}\nuser-policies {\n"
                      + rule("user", reader, original) + "}\n")
    cfg["actors"] = list(actors)
    cfg["network"]["routes"] = []
    cfg["storage"]["repo_root"] = str(work / "repo")
    Path(cfg["storage"]["repo_root"]).mkdir(mode=0o700)
    security["controller_policy"] = str(policy)
    grant_delay = 15000
    write_json(work / "configuration.json", cfg)
    state = dict(schema="ndnsf-di-host-operation-v1", deployment_id=cfg["deployment_id"],
                 configuration_sha256=digest((work / "configuration.json").read_bytes()),
                 evidence_root=str(work), boot_id=uuid.uuid4().hex, status="STARTING", launches=[])
    result = dict(status="FAIL", configSha256=state["configuration_sha256"], manifestSha256=digest(ledger),
                  grantDelayMs=grant_delay, grantedService=granted, reader=reader["identity"])
    children = []
    deadline = time.monotonic() + 120
    facility = MiniNdnFacility(cfg, work, state, deadline)
    def guard():
        cluster._check_running(children)
        require(time.monotonic() < deadline, "grant experiment deadline exceeded")
    def spawn(actor_id, kind, argv, env, node):
        return cluster._spawn(state, work, actor_id, kind, argv, env, guard,
                              launcher=facility.launcher(node), network_namespace=facility.namespace(node))
    def marks(actor, marker):
        log = work / (actor["id"] + ".log")
        return [v for _, v in records(log.read_text(errors="replace").splitlines(), marker)] if log.exists() else []
    def wait(check, reason):
        end = min(deadline, time.monotonic() + 32)
        while time.monotonic() < end:
            guard()
            if check():
                return
            time.sleep(0.05)
        raise RuntimeError(reason)
    read_node = reader["node"]
    env = cluster._environment(reader["credential_home"], nodes[read_node]["nfd"]["socket"],
                               reader["id"], state)
    nfd_log = work / ("nfd." + controller["node"] + ".log")
    def forwarded_to(name, offset):
        with nfd_log.open("rb") as log:
            log.seek(offset)
            entries = re.findall(rb"onOutgoingInterest out=(\d+) interest=(\S+)", log.read())
        return {int(face) for face, wire_name in entries if Name.normalize(wire_name.decode()) == name}
    def fetch(label, name, *, discovery=False, missing=False):
        guard()
        argv = [peek, "-f", "-v", "-l", "1100", "-w", "1700"]
        if discovery:
            argv.append("-P")
        with facility.launcher(read_node)([*argv, Name.to_str(name)], env=env,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as proc:
            try:
                raw, stderr = proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                raise
        (work / (label + ".log")).write_bytes(stderr)
        if missing:
            require(proc.returncode == 3 and not raw and b"TIMEOUT" in stderr,
                    f"{label} returned Data, Nack or an unexpected error")
            return
        require(proc.returncode == 0 and raw, f"{label} did not receive Data")
        actual, meta, content, sig = parse_data(raw)
        require(sig.signature_info and sig.signature_info.key_locator and
                sig.signature_info.key_locator.name in (pin.name[:-2], pin.name) and verify_rsa(pubkey, sig)
                and content and (actual[:len(name)] == name if discovery else actual == name),
                f"{label} name, signature or content mismatch")
        (work / (label + ".ndn")).write_bytes(raw)
        return actual, meta, raw

    try:
        facility.start(guard)
        for node in cfg["nodes"]:
            directory = work / ("forwarder-" + node["id"])
            directory.mkdir()
            saved_debug = os.environ.get("NDNSF_NFD_DEBUG")
            if node["id"] == controller["node"]:
                os.environ["NDNSF_NFD_DEBUG"] = "1"
            try:
                cluster._start_forwarder(cfg, node, state, work, directory, children, deadline, guard, spawn,
                                         disable_cs=True)
            finally:
                if saved_debug is None:
                    os.environ.pop("NDNSF_NFD_DEBUG", None)
                else:
                    os.environ["NDNSF_NFD_DEBUG"] = saved_debug
        facility.routes(work, guard)
        for actor in (controller, repo):
            argv, native_env = cluster._native_launch(cfg, actor, work, state)
            native_env["NDNSF_POLICY_REVALIDATION_PERIOD_MS"] = "1000"
            if actor is controller:
                argv += ["--grant-additional-after-ms", str(grant_delay), "--grant-additional-role", "user",
                         "--grant-additional-service", granted, "--grant-additional-identity", reader["identity"]]
                started = time.monotonic()
            child, row = spawn(actor["id"], actor["kind"], argv, native_env, actor["node"])
            children.append((actor, child, row))
            wait(lambda: cluster._ready(row, actor, state), f'{actor["id"]} startup failed')

        release = work / "after-grant"
        env.update(NDNSF_CONTROLLER_CERT_FILE=security["controller_certificate"],
                   NDNSF_POLICY_REVALIDATION_PERIOD_MS="1000", NDN_LOG="*=NONE:ndn_service_framework.ServiceUser=INFO")
        reader_proc, row = spawn(reader["id"], "requester", [str(probe), reader["identity"], cfg["network"]["group"],
            repo["identity"], status_service, security["controller_certificate"], security["trust_schema"],
            str(release), "control"], env, read_node)
        children.append((reader, reader_proc, row))
        wait(lambda: marks(reader, "LEASE_READY"), "reader never completed an authorized pre-grant request")
        old_status, = marks(reader, "LEASE_READY")
        generation, epoch = map(int, old_status["version"].split(":"))
        ctr = Name.normalize(controller["identity"])
        pub = ctr + Name.normalize("/PUBPARAMS")
        key_query = ctr + Name.normalize("/DKEY") + [Component.from_bytes(Name.to_bytes(recipient.name[:-2]))]
        old_pub, _, pub_wire = fetch("before-parameters", pub, discovery=True)
        old_first, meta, _ = fetch("before-key", key_query, discovery=True)
        require(old_first[:-2] == key_query and old_first[-1] == Component.from_segment(0)
                and Component.get_type(old_first[-2]) == Component.TYPE_VERSION and meta.final_block_id
                and Component.get_type(meta.final_block_id) == Component.TYPE_SEGMENT
                and Component.to_number(meta.final_block_id) >= 1, "initial DKEY is not segmented")
        old_next = old_first[:-1] + [Component.from_segment(1)]
        offset = nfd_log.stat().st_size
        fetch("before-continuation", old_next)
        serving_faces = forwarded_to(old_next, offset)
        require(len(serving_faces) == 1, "could not identify the Controller face serving the old continuation")
        require(not marks(controller, "NDNSF_GRANT_ONLY_APPLIED"), "grant preceded pre-grant wire observations")
        wait(lambda: marks(controller, "NDNSF_GRANT_ONLY_APPLIED"), "Controller grant not observed")
        change, = marks(controller, "NDNSF_GRANT_ONLY_APPLIED")
        require(change["success"] == "1" and Name.normalize(change["identity"]) == Name.normalize(reader["identity"])
                and Name.normalize(change["service"]) == Name.normalize(granted)
                and (int(change["generation"]), int(change["epoch"])) == (generation, epoch + 1),
                "grant did not advance exactly one authorization epoch")
        new_pub, _, same_wire = fetch("after-parameters", pub, discovery=True)
        require(new_pub == old_pub and same_wire == pub_wire, "grant rotated ABE material")
        offset = nfd_log.stat().st_size
        fetch("old-continuation-after-grant", old_next, missing=True)
        require(serving_faces == forwarded_to(old_next, offset),
                "post-grant exact Interest was not forwarded to the same serving Controller face")
        # The Controller retains current bundles for 30 s. This whole sequence
        # must finish before the old exact segment could have expired by age.
        require(time.monotonic() - started < 30, "DKEY bundle TTL no longer distinguishes grant retirement")
        new_first, _, _ = fetch("after-key", key_query, discovery=True)
        require(new_first[:-2] == key_query and new_first[:-1] != old_first[:-1]
                and new_first[-1] == Component.from_segment(0), "new DKEY bundle missing")
        fetch("after-continuation", new_first[:-1] + [Component.from_segment(1)])
        def refreshes():
            return [r for r in marks(reader, "NDNSF_NAC_DKEY_REFRESH_REQUESTED")
                    if r.get("epoch") == str(epoch + 1) and r.get("reason") == "grant-only"]
        wait(refreshes, "live ServiceUser did not refresh its DKEY for the grant")
        release.touch(exist_ok=False)
        require(reader_proc.wait(timeout=18) == 0, "reader did not finish its second Repo request")
        require(len(refreshes()) == 1, "expected exactly one grant-only DKEY refresh")
        responses = marks(reader, "LEASE_RESPONSE")
        attempts = marks(reader, "LEASE_ATTEMPT")
        require([r["phase"] for r in responses] == ["before", "after"] and
                all(json.loads(r["payload"]).get("storageBackend") for r in responses), "real Repo replies missing")
        require([(a["phase"], a["version"]) for a in attempts] ==
                [("before", old_status["version"]), ("after", f"{generation}:{epoch + 1}")],
                "ordinary Repo calls did not use the pre- and post-grant authorizations")
        result.update(generation=generation, oldEpoch=epoch, newEpoch=epoch + 1, samePublicWireSha256=digest(pub_wire),
                      oldContinuation=Name.to_str(old_next), currentDkey=Name.to_str(new_first),
                      requesterNode=read_node, controllerNode=controller["node"], grantOnlyRefreshes=1,
                      oldContinuationOutcome="forwarded-to-same-controller-face-no-data",
                      continuationControllerFace=next(iter(serving_faces)),
                      requesterExitCode=reader_proc.returncode)
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        try:
            closed = cluster._close(state, work, facility)
            rows = state["cleanup"] + state["facility"]["cleanup"]
            require(closed["status"] == "STOPPED" and state["facility"]["status"] == "STOPPED"
                    and all(r["status"] == "ZERO" and r.get("forced") is False for r in rows),
                    "deployment did not stop gracefully")
            if "failure" not in result:
                codes = {r["id"]: r.get("exitCode") for r in state["cleanup"] if r["id"] in
                         (controller["id"], repo["id"])}
                require(codes == {controller["id"]: 0, repo["id"]: 0}, "missing clean native exits")
                require(digest(Path(cfg["release"]["manifest"]).read_bytes()) == digest(ledger), "candidate changed")
                result.update(status="PASS", actorExitCodes=codes)
        except Exception as error:
            result["cleanupFailure"] = str(error)
            if "failure" not in result:
                raise
        finally:
            write_json(work / "result.json", result)


if __name__ == "__main__":
    main()
