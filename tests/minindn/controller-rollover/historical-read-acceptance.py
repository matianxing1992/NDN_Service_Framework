#!/usr/bin/env python3
"""Revoke one service, cold-read retained history and verify a new ordinary NAC publication."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
from contextlib import contextmanager, ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from ndn.encoding import Name, parse_data
from ndnsf_distributed_inference.ops.deployment_config import check_deployment


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def write(path, obj):
    with path.open("x") as stream:
        json.dump(obj, stream, indent=2)
    return path


def fields(output, marker):
    lines = [line for line in output.splitlines() if line.startswith(marker + " ")]
    require(len(lines) == 1, f"expected one {marker}, got {len(lines)}")
    return dict(item.split("=", 1) for item in lines[0].split()[1:] if "=" in item)


def run(argv, log, *, env=None, expect=0, timeout=60):
    with log.open("xb") as stream:
        result = subprocess.run(argv, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout,
                                env=env, check=False)
    require(result.returncode == expect, f"{argv[0]} exit {result.returncode}, see {log}")
    return log.read_text(errors="replace")


def wait_for(check, process, deadline, reason):
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        require(process.poll() is None, f"{reason}: deployment exited {process.returncode}")
        time.sleep(0.1)
    raise RuntimeError(reason)


@contextmanager
def committed_origin(publisher, config, log, environment):
    # After commit this PID only owns immutable original packets. Its RepoClient
    # has closed, and its lifetime is bounded in both the native app and this driver.
    with log.open("xb") as stream:
        proc = subprocess.Popen([publisher, "--serve-committed", config], stdout=stream,
                                stderr=subprocess.STDOUT, env=dict(environment, NDN_LOG="ndn.Face=DEBUG"))
    try:
        def started():
            output = log.read_text(errors="replace")
            return output if "\nHISTORICAL_ORIGIN_STARTED identity=" in output else None

        yield wait_for(started, proc, time.monotonic() + 40, "committed origin did not start")
    finally:
        failed = sys.exc_info()[1] is not None
        if proc.poll() is None:
            proc.terminate()
        try:
            code = proc.wait(timeout=6)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
            if not failed:
                raise RuntimeError("committed origin required forced cleanup")
            print("committed origin required forced cleanup", file=sys.stderr)
        else:
            if not failed:
                require(code == 0, f"committed origin exit {code}")


@contextmanager
def phase(config, work, name, launcher, reader):
    data = copy.deepcopy(config)
    request = data["requester"]
    ready = work / (name + "-requester.ready")
    release = work / (name + "-requester.release")
    # One bounded barrier holds the real deployment while independent native probes run.
    # It never grants service access, changes the history objects, or decides a read verdict.
    request.update(test_only=True, test_stream_barrier={
        "ready_file": str(ready), "release_file": str(release), "timeout_ms": 60000})
    if name == "cold":
        data["security"].pop("controller_recovery_bootstrap", None)
        journal = request.get("conversation", {}).get("journal")
        if journal:
            journal["state_root"] = str(work / "cold-journal")
        for turn in request["turns"]:
            for key in ("output_file", "checkpoint_output_file"):
                if key in turn:
                    old = Path(turn[key])
                    turn[key] = str(old.with_name("cold-" + old.name))
    # Relative deployment inputs must resolve against the same root as the original configuration.
    path = write(work.parent / ("historical-read-" + name + ".json"), data)
    check_deployment(path)
    args = [launcher, "cluster"]
    state = Path(data["storage"]["state_root"]) / data["deployment_id"] / "state.json"
    evidence = Path(data["storage"]["evidence_root"]) / data["deployment_id"]
    before = set(evidence.iterdir()) if evidence.exists() else set()
    with (work / (name + "-cluster.log")).open("xb") as log:
        proc = subprocess.Popen([*args, "up", "--config", path, "--debug", "--timeout", "180"],
                                stdout=log, stderr=subprocess.STDOUT)
        try:
            def current_boot():
                if not ready.is_file() or not state.is_file():
                    return None
                current = json.loads(state.read_text())
                boot = evidence / current["boot_id"]
                if boot not in before and boot.is_dir() and current["status"] == "REQUEST_RUNNING":
                    return boot
                return None

            boot = wait_for(current_boot, proc, time.monotonic() + 120,
                            f"{name} native requester did not reach the first-event barrier")
            yield boot, proc
            release.touch(exist_ok=False)
            require(proc.wait(timeout=100) == 0, f"{name} launcher failed, see {log.name}")
            operation = json.loads((boot / "operation.json").read_text())
            actors = {a["id"] for a in data["actors"] if not a.get("external", False)}
            rows = operation["cleanup"]
            by_id = {row["id"]: row for row in rows}
            require(operation["status"] == "STOPPED" and operation.get("execution") == "COMPLETE"
                    and not operation.get("failure") and len(by_id) == len(rows)
                    and reader["id"] in actors and actors <= by_id.keys()
                    and all(row["status"] == "ZERO" and row.get("forced") is False for row in rows)
                    and all(type(by_id[i].get("exitCode")) is int and by_id[i]["exitCode"] == 0
                            for i in actors), f"{name} has unqualified actor shutdown")
        finally:
            if proc.poll() is None:
                first_failure = sys.exc_info()[1]
                release.touch(exist_ok=True)
                try:
                    run([*args, "down", "--config", path], work / (name + "-stop.log"), timeout=80)
                    proc.wait(timeout=80)
                except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                    if first_failure is None:
                        raise
                    print(f"{name} cleanup also failed: {error}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("--producer", required=True, help="dedicated, provisioned Producer identity")
    parser.add_argument("--kept-service", required=True)
    parser.add_argument("--revoked-service", required=True)
    args = parser.parse_args()
    source = args.config.resolve(strict=True)
    root = source.parent
    os.chdir(root)
    check_deployment(source)
    cfg = json.loads(source.read_text())
    require(cfg["platform"]["kind"] == "minindn", "MiniNDN deployment required")
    require(args.kept_service != args.revoked_service and
            args.revoked_service != cfg["network"]["service"], "distinct authorization scopes required")
    controller, = (a for a in cfg["actors"] if a["kind"] == "controller")
    reader, = (a for a in cfg["actors"] if a["kind"] == "requester")
    repo, = (a for a in cfg["actors"] if a["kind"] == "repo")
    producer, = (a for a in cfg["actors"] if a["identity"] == args.producer and a["kind"] == "provider")
    nodes = {n["id"]: Path(n["nfd"]["socket"]).resolve() for n in cfg["nodes"]}
    subjects = (controller, reader, repo, producer)
    require(len({a["node"] for a in subjects}) == len(subjects)
            and len({a["credential_home"] for a in subjects}) == len(subjects)
            and all(root in Path(a["credential_home"]).resolve().parents for a in subjects)
            and all(root in nodes[a["node"]].parents and not nodes[a["node"]].exists() for a in subjects),
            "require isolated, stopped actors on four distinct nodes")
    store = Path(cfg["storage"]["repo_root"]).resolve()
    checkpoint = cfg["security"].get("controller_recovery_checkpoint")
    controller_home = Path(controller["credential_home"])
    require(root in store.parents and root in Path(cfg["storage"]["state_root"]).resolve().parents
            and root in Path(cfg["storage"]["evidence_root"]).resolve().parents
            and (not store.exists() or not any(store.iterdir()))
            and not (controller_home / ".ndn/controller-generation.state").exists()
            and (not checkpoint or not Path(checkpoint).exists()), "fresh isolated state required")
    placement = cfg["requester"].get("fixed_placement", {}).get("role_providers", {})
    required = set(cfg["requester"]["request"].get("provider_names", [])) | set(placement.values())
    require(args.producer not in required, "Producer is needed by a managed inference rank")
    target = cfg["requester"].get("model_input", {}).get("repo_target", {})
    route = repo.get("parameters", {}).get("repo-node") or target.get("repo_node") or repo["identity"]
    network = cfg["network"]
    cfg["actors"].remove(producer)
    for src, dst, prefix in ((reader, repo, route), (producer, repo, route),
                             (repo, producer, args.producer), (reader, producer, args.producer)):
        row = {"from": src["node"], "to": dst["node"], "prefix": prefix}
        if row not in network.setdefault("routes", []):
            network["routes"].append(row)
    bindir = Path(cfg["release"]["prefix"]) / "bin"
    launcher = bindir / "ndnsf-di"
    publisher = bindir / "ndnsf-historical-publish"
    probe = bindir / "ndnsf-historical-read-probe"
    manifest = Path(cfg["release"]["manifest"])
    manifest_wire = manifest.read_bytes()
    ledger = json.loads(manifest_wire)["files"]
    for binary in (publisher, probe, bindir / "DistributedRepoServer"):
        require(ledger[str(binary)]["installed"]["sha256"] == hashlib.sha256(binary.read_bytes()).hexdigest(),
                f"candidate binary mismatch: {binary}")
    os.umask(0o077)
    work = root / "historical-acceptance"
    work.mkdir()
    # Keep the Repo's ordinary lease refresh inside the five-second NAC public
    # parameter freshness window. The live lease and transport checks still apply.
    cfg["security"]["controller_policy_lease_ms"] = 5000
    trigger = work / "revocation.trigger"
    controller.setdefault("parameters", {}).update({
        "revoke-trigger-file": str(trigger), "revoke-kind": "service",
        "revoke-identity": reader["identity"], "revoke-service": args.revoked_service,
        "revoke-attribute": "/PERMISSION" + args.revoked_service})

    def env(actor):
        home = actor["credential_home"]
        security = cfg["security"]
        bound = security.get(actor["kind"] + "_policy_max_lease_ms",
                             security.get("controller_policy_max_lease_ms",
                                          security.get("controller_policy_lease_ms", 60000)))
        values = dict(os.environ, HOME=home, NDN_CLIENT_TRANSPORT="unix://" + str(nodes[actor["node"]]),
                      NDN_CLIENT_PIB="pib-sqlite3:" + str(Path(home) / ".ndn"), NDN_CLIENT_TPM="tpm-file:",
                      NDNSF_CONTROLLER_CERT_FILE=security["controller_certificate"],
                      NDNSF_CONTROLLER_POLICY_MAX_LEASE_MS=str(bound),
                      NDNSF_CONTROLLER_POLICY_CLOCK_SKEW_MS=str(security.get("controller_policy_clock_skew_ms", 250)))
        if "controller_min_version" in security:
            values["NDNSF_CONTROLLER_MIN_VERSION"] = security["controller_min_version"]
        return values

    def publish(label, service, origin=None):
        payload = work / (label + ".bin")
        data = (hashlib.sha256(label.encode()).digest() * 222)[:7100]
        payload.write_bytes(data)
        expected = "sha256:" + hashlib.sha256(data).hexdigest()
        options = {"pib": str(Path(producer["credential_home"]) / ".ndn"), "identity": args.producer,
                   "controller": controller["identity"], "service": service,
                   "object_prefix": args.producer + "/NDNSF/OBJECT/" + label,
                   "payload_file": str(payload), "repo_provider": repo["identity"], "repo_route": route,
                   "repo_service_prefix": network["repo_service"], "group": network["group"]}
        for key in ("controller_certificate", "trust_schema"):
            options[key] = cfg["security"][key]
        writer_config = write(work / (label + "-writer.json"), options)
        writer_log = work / (label + "-write.log")
        if origin is None:
            writer = run([publisher, "--config", writer_config], writer_log, env=env(producer), timeout=40)
        else:
            writer = origin.enter_context(committed_origin(publisher, writer_config, writer_log, env(producer)))
        obj = fields(writer, "HISTORICAL_OBJECT")
        original = fields(writer, "HISTORICAL_REPO_COMMITTED")
        receipt = fields(writer, "HISTORICAL_RECEIPT")
        require(Name.normalize(obj["producer"]) == Name.normalize(args.producer)
                and Name.normalize(obj["service"]) == Name.normalize(service)
                and obj["attribute"] == "/PERMISSION" + service and obj["segments"] == "2"
                and obj["bytes"] == str(len(data)) and obj["plaintextDigest"].lower() == expected
                and receipt["object"] == obj["payload"] and receipt["name"].startswith("/")
                and Name.normalize(original["object"]) == Name.normalize(obj["publication"]),
                "publication reference mismatch")
        reader_pib = str(Path(reader["credential_home"]) / ".ndn")
        read = {"identity": reader["identity"], "pib": reader_pib, "controller": controller["identity"],
                "controller_pib": reader_pib, "producer_pib": reader_pib,
                "trust_schema": cfg["security"]["trust_schema"], "original_data_forwarding_hint": route,
                "object": obj["payload"], "policy": obj["attribute"]}
        read.update({key: obj[key] for key in ("ck", "producer", "payload", "publication", "service",
                                               "generation", "epoch")})
        probe_cfg = write(work / (label + "-reader.json"), read)
        return label, obj, original["wireDigest"], expected, probe_cfg

    def old_parameters(obj, local_only=False):
        # HopLimit 1 is decremented at the Producer's NFD. A local-only
        # Interest cannot reach the Controller on its separate node.
        argv = ["ndnpeek", "-f", "-w", "1500", "-l", "1200"]
        if local_only:
            argv.extend(["-H", "1"])
        result = subprocess.run([*argv, obj["parameters"]], env=env(producer),
                                stdout=subprocess.PIPE, timeout=3, check=False)
        require(result.returncode == 0 and result.stdout,
                "prior parameters missing from Producer's fresh Content Store" if local_only else
                "could not prime Producer's Content Store with prior parameters")
        name, _, content, _ = parse_data(result.stdout)
        require(Name.to_bytes(name) == Name.to_bytes(Name.normalize(obj["parameters"])) and
                content is not None and "sha256:" + hashlib.sha256(content).hexdigest() ==
                obj["parametersDigest"].lower(), "prior public-parameter packet mismatch")
        return hashlib.sha256(result.stdout).hexdigest()

    def forwarder_control(actor):
        state = Path(cfg["storage"]["state_root"]) / cfg["deployment_id"]
        node = actor["node"]
        home = state / ("nfdc-" + node)
        require(home.is_dir(), "missing managed forwarder control home")
        return dict(os.environ, HOME=str(home), NDN_CLIENT_PIB="pib-sqlite3:" + str(home / ".ndn"),
                    NDN_CLIENT_TPM="tpm-file:", NDN_CLIENT_TRANSPORT="unix://" + str(nodes[node]))

    def reset_parameter_cache(obj):
        control = forwarder_control(producer)
        # The managed deployment is cold by default. Enable a bounded Content
        # Store only at this Producer for the explicit stale-material contrast.
        run(["nfdc", "cs", "config", "capacity", "1024"], work / "cache-config.log", env=control, timeout=5)
        measured = run(["nfdc", "cs", "info"], work / "cache-info.log", env=control, timeout=5)
        require("capacity=1024" in measured, "Producer Content Store capacity was not applied")
        run(["nfdc", "cs", "erase", obj["parameters"]], work / "cache-erase-params.log",
            env=control, timeout=5)

    def new_log(path, offset):
        with path.open("rb") as stream:
            stream.seek(offset)
            return stream.read().decode(errors="replace")

    def original_names(obj):
        return (obj["publication"], obj["ck"], obj["payload"] + "/seg=0", obj["payload"] + "/seg=1")

    def other_reader(probe_cfg, label, hint):
        options = json.loads(probe_cfg.read_text())
        pib = str(Path(producer["credential_home"]) / ".ndn")
        options.update(identity=producer["identity"], pib=pib, producer_pib=pib, controller_pib=pib,
                       original_data_forwarding_hint=hint)
        return write(work / (label + ".json"), options)

    objects = []
    with phase(cfg, work, "publication", launcher, reader) as (first, process), ExitStack() as origin:
        for label, service in (("kept", args.kept_service), ("revoked", args.revoked_service)):
            entry = publish(label, service, origin if label == "kept" else None)
            prime = run([probe, "--prime", entry[-1]], work / (label + "-prime.log"), env=env(reader))
            fields(prime, "HISTORICAL_PRIME_OK")
            objects.append(entry)
        require(len({row[1]["publication"] for row in objects}) == 2, "reused mapping name")
        _, paired_object, _, paired_digest, withdrawn_cfg = objects[1]
        retained_cfg = other_reader(withdrawn_cfg, "same-service-retained", route)
        for actor, config in ((reader, withdrawn_cfg), (producer, retained_cfg)):
            enrolled = fields(run([probe, "--current", config],
                                  work / (actor["id"] + "-before-withdrawal.log"), env=env(actor)),
                              "CURRENT_NAC_READ_OK")
            require(enrolled["plaintextDigest"].lower() == paired_digest
                    and enrolled["bytes"] == paired_object["bytes"],
                    f"{actor['id']} could not read the same service object before withdrawal")
        previous = objects[0][1]
        reset_parameter_cache(previous)
        cached_wire = old_parameters(previous)
        cache_observed = time.monotonic()
        require(old_parameters(previous, local_only=True) == cached_wire,
                "Producer forwarder did not cache the seeded original parameters")
        log = first / (controller["id"] + ".log")
        offset = log.stat().st_size
        repo_log = first / (repo["id"] + ".log")
        repo_offset = repo_log.stat().st_size
        trigger.touch(exist_ok=False)

        def rotated():
            text = new_log(log, offset)
            events = re.findall(r"NDNSF_CONTROLLER_REVOKED kind=3 identity=(\S+) service=(\S+) "
                                r"generation=(\d+) epoch=(\d+)", text)
            applied = set(re.findall(r"NDNSF_REVOCATION_APPLIED success=1 kind=3 "
                                     r"generation=(\d+) epoch=(\d+)", text))
            return next(((generation, epoch) for identity, service, generation, epoch in events
                         if Name.normalize(identity) == Name.normalize(reader["identity"])
                         and Name.normalize(service) == Name.normalize(args.revoked_service)
                         and (generation, epoch) in applied), None)

        version = wait_for(rotated, process, time.monotonic() + 15, "targeted service revocation absent")
        storage_scope = network["repo_service"] + "/USER_STORAGE_GRANT"

        def repo_refreshed():
            text = new_log(repo_log, repo_offset)
            marker = (f"NDNSF_CONTROLLER_CACHE_INVALIDATED role=provider serviceName={storage_scope} "
                      f"generation={version[0]} epoch={version[1]} ")
            return marker in text and "[nacabe.Consumer] SegmentFetcher completed" in text

        wait_for(repo_refreshed, process, cache_observed + 5, "Repo did not refresh after rotation")
        # The Repo writers have closed. Only the kept object's read-only origin
        # remains. A fresh writer returns to the same NFD and obtains the new CK.
        receipt_prefix = "[ndn.Face] <D " + controller["identity"] + "/NDNSF/RPC/PUBLISH-HISTORY/"
        receipt_offset = log.stat().st_size
        with ThreadPoolExecutor(max_workers=1) as publisher_task:
            write_new = publisher_task.submit(publish, "kept-after-rotation", args.kept_service)
            wait_for(lambda: receipt_prefix in new_log(log, receipt_offset) or write_new.done(),
                     process, cache_observed + 5, "new publication receipt did not arrive before cache expiry")
            if write_new.done():
                write_new.result()
            require(receipt_prefix in new_log(log, receipt_offset), "missing new publication acknowledgement")
            require(old_parameters(previous, local_only=True) == cached_wire,
                    "Producer's fresh Content Store returned different old parameter wire")
            cache_interval_ms = round(1000 * (time.monotonic() - cache_observed))
            _, new_obj, _, new_digest, probe_cfg = write_new.result()
        old_material = tuple(int(previous[key]) for key in ("generation", "epoch"))
        new_material = tuple(int(new_obj[key]) for key in ("generation", "epoch"))
        require(new_material > old_material and previous["parameters"] != new_obj["parameters"]
                and previous["parametersDigest"].lower() != new_obj["parametersDigest"].lower(),
                "post-rotation writer did not use new material")
        current = fields(run([probe, "--current", probe_cfg], work / "current-read.log",
                             env=env(reader), timeout=30), "CURRENT_NAC_READ_OK")
        require(current["plaintextDigest"].lower() == new_digest and current["bytes"] == new_obj["bytes"]
                and tuple(int(current[k]) for k in ("generation", "epoch")) == new_material,
                "normal current-DKEY read did not recover the new plaintext")

        # The topology directly links this Reader to the Producer. The hint is
        # the origin's exact mapping registration on that Producer's Face. Its
        # other filters on the same Face serve the exact CK and the two segments.
        _, old_obj, old_wire, old_plain, old_cfg = objects[0]
        origin_cfg = json.loads(old_cfg.read_text())
        origin_cfg["original_data_forwarding_hint"] = old_obj["publication"]
        origin_cfg = write(work / "kept-origin-reader.json", origin_cfg)
        for actor in (reader, producer):
            run(["nfdc", "cs", "config", "serve", "off"],
                work / (actor["id"] + "-origin-no-cache.log"), env=forwarder_control(actor))
        origin_log = work / "kept-write.log"
        sent_from = origin_log.stat().st_size
        repo_from = repo_log.stat().st_size
        direct = fields(run([probe, origin_cfg], work / "kept-origin-read.log", env=env(reader)),
                        "HISTORICAL_READ_OK")
        require(direct["plaintextDigest"].lower() == old_plain and direct["bytes"] == old_obj["bytes"],
                "direct Producer plaintext mismatch after rotation")
        names = original_names(old_obj)
        delivered = {Name.to_bytes(Name.normalize(n))
                     for n in re.findall(r"\[ndn\.Face\] <D (\S+)", new_log(origin_log, sent_from))}
        require(all(Name.to_bytes(Name.normalize(n)) in delivered for n in names),
                "origin did not put every original during the native read")
        origin_digest = hashlib.sha256()
        for name in names:
            wire = subprocess.run(["ndnpeek", "-F", old_obj["publication"], "-w", "3000", "-l", "2500", name],
                                  env=env(reader), capture_output=True, timeout=5, check=True).stdout
            origin_digest.update(wire)
        require("sha256:" + origin_digest.hexdigest() == old_wire.lower(),
                "direct Producer packet set differs from committed originals")
        repo_sent = {Name.to_bytes(Name.normalize(n)) for n in re.findall(
            r"(?:original packet put|repo-original-data-served) name=(\S+)", new_log(repo_log, repo_from))}
        require(not any(Name.to_bytes(Name.normalize(n)) in repo_sent for n in names),
                "Repo served a packet during the direct Producer comparison")
    trigger.unlink()
    recovered = {}
    with phase(cfg, work, "cold", launcher, reader) as (cold, _):
        for label, obj, expected_wire, expected_plain, probe_cfg in objects:
            read_env = env(reader)
            text = run([probe, probe_cfg], work / (label + "-read.log"), env=read_env,
                       expect=0 if label == "kept" else 1, timeout=30)
            if label == "kept":
                require(fields(text, "HISTORICAL_READ_OK")["plaintextDigest"].lower() == expected_plain,
                        "cold plaintext differs from original input")
            else:
                denial = fields(text, "HISTORICAL_READ_FAILED")
                require(denial.get("stage") == "object" and denial.get("reason") == "not-authorised",
                        "missing authenticated scoped denial")
            aggregate = hashlib.sha256()
            for name in original_names(obj):
                wire = subprocess.run(["ndnpeek", "-F", route, "-w", "3000", "-l", "2500", name],
                                      env=read_env, stdout=subprocess.PIPE, timeout=5, check=True).stdout
                key = Name.to_bytes(Name.normalize(name))
                require(wire and key not in recovered, "missing or reused original name")
                aggregate.update(wire)
                recovered[key] = hashlib.sha256(wire).hexdigest()
            require("sha256:" + aggregate.hexdigest() == expected_wire.lower(),
                    "restarted Repo changed original packet-set wire")
        without = run([probe, "--without-ck-evidence", objects[0][-1]], work / "no-ck.log",
                      env=dict(env(reader), NDNSF_HISTORICAL_READ_DEBUG="1"), timeout=20)
        require("HISTORICAL_NO_EVIDENCE_KEY_YIELDED=0 " in without and
                "key request denied by the controller: ck-evidence-required" in without,
                "missing authenticated CK-evidence refusal")

        # The original writer has exited. A fresh process holding the other reader's
        # independent credential must still obtain the historical DKEY for the very
        # object whose first reader just received a signed refusal.
        retained = fields(run([probe, retained_cfg], work / "same-service-retained.log",
                              env=env(producer)), "HISTORICAL_READ_OK")
        require(retained["plaintextDigest"].lower() == paired_digest
                and retained["bytes"] == paired_object["bytes"], "retained same-service read failed")

        # Give only the application-original Interests a forwarding hint whose
        # sole next hop drops packets. Authorization and DKEY Interests retain
        # their normal routes to the live Controller.
        control = forwarder_control(producer)
        blackhole = "/localhost/spec243/history-cache-only"
        run(["nfdc", "route", "add", blackhole, "255", "capture"],
            work / "cache-only-route.log", env=control)
        names = original_names(paired_object)
        run(["nfdc", "cs", "config", "capacity", "1024", "admit", "off", "serve", "off"],
            work / "cache-only-isolate.log", env=control)
        for name in names:
            denied = subprocess.run(["ndnpeek", "-F", blackhole, "-w", "800", "-l", "500", name],
                                    env=env(producer), capture_output=True, timeout=3)
            require(denied.returncode in (3, 4), f"cache-only hint reached upstream {name}")

        # This node forwards all seven spokes. Finish the slow negative probes
        # first, then admit the four originals in one burst and freeze the store
        # so unrelated traffic cannot evict them during the native cold read.
        run(["nfdc", "cs", "config", "admit", "on"], work / "cache-only-prime.log", env=control)
        for name in names:
            key = Name.to_bytes(Name.normalize(name))
            seeded = subprocess.run(["ndnpeek", "-F", route, "-w", "3000", "-l", "2500", name],
                                    env=env(producer), capture_output=True, timeout=5, check=True).stdout
            require(hashlib.sha256(seeded).hexdigest() == recovered[key],
                    f"cache seed differs from restarted Repo original {name}")
        run(["nfdc", "cs", "config", "admit", "off", "serve", "on"], work / "cache-only-serve.log", env=control)

        def cache_snapshot(label):
            output = run(["nfdc", "cs", "info"], work / ("cache-only-" + label + ".log"), env=control)
            measured = dict(line.strip().split("=", 1) for line in output.splitlines() if "=" in line)
            require(measured["serve"] == "on" and measured["admit"] == "off"
                    and int(measured["nEntries"]) >= len(names), "forwarder did not retain the seed")
            return int(measured["nHits"])

        initial_hits = cache_snapshot("frozen")
        snapshot_time = time.monotonic()
        for name in names:
            wire = subprocess.run(["ndnpeek", "-H", "1", "-w", "1500", "-l", "1000", name],
                                  env=env(producer), capture_output=True, timeout=3, check=True).stdout
            require(hashlib.sha256(wire).hexdigest() == recovered[Name.to_bytes(Name.normalize(name))],
                    f"local cache returned different original wire for {name}")
        cached_cfg = other_reader(withdrawn_cfg, "same-service-cache-only", blackhole)
        cached = fields(run([probe, cached_cfg], work / "same-service-cache-only.log",
                            env=env(producer)), "HISTORICAL_READ_OK")
        require(cached["plaintextDigest"].lower() == paired_digest and cached["bytes"] == paired_object["bytes"],
                "fresh native reader did not recover historical plaintext through the isolated cache")
        # NFD's management dispatcher can serve the previous status dataset for one second.
        time.sleep(max(0.0, snapshot_time + 1.1 - time.monotonic()))
        cache_hits = cache_snapshot("after-read") - initial_hits
        require(cache_hits >= 2 * len(names), "missing local and native original-packet cache hits")
        run(["nfdc", "cs", "config", "capacity", "0", "admit", "on", "serve", "on"],
            work / "cache-only-reset.log", env=control)

    def repo_instance(path):
        operation = json.loads((path / "operation.json").read_text())
        launched, = (row for row in operation["launches"] if row["kind"] == "repo")
        closed, = (row for row in operation["cleanup"] if row["id"] == launched["id"])
        return operation["boot_id"], closed["pid"], launched["argv"][launched["argv"].index("--database") + 1]

    first_id, first_pid, database = repo_instance(first)
    cold_id, cold_pid, reopened = repo_instance(cold)
    require(first_id != cold_id and first_pid != cold_pid and database == reopened, "Repo restart mismatch")
    served = set()
    for line in (cold / (repo["id"] + ".log")).read_text(errors="replace").splitlines():
        for marker in ("original packet put name=", "repo-original-data-served name="):
            if marker in line:
                served.add(Name.to_bytes(Name.normalize(line.partition(marker)[2].split()[0])))
    require(recovered.keys() <= served, "cold Repo did not serve every original name")
    require(manifest.read_bytes() == manifest_wire, "installed candidate changed during acceptance")
    write(work / "result.json", {"result": "PASS", "installationSha256": hashlib.sha256(manifest_wire).hexdigest(),
          "controllerLeaseMs": cfg["security"]["controller_policy_lease_ms"],
          "publishBoot": first_id, "coldBoot": cold_id, "repoDatabase": database,
          "originAfterRotation": {"producer": args.producer, "reader": reader["identity"],
                                  "object": old_obj["payload"], "originalPacketCount": len(original_names(old_obj)),
                                  "wireDigest": old_wire, "plaintextSha256": old_plain},
          "sameServiceWithdrawal": {"service": args.revoked_service, "object": paired_object["payload"],
                                    "retainedReader": producer["identity"], "revokedReader": reader["identity"],
                                    "plaintextSha256": paired_digest, "localCacheOriginals": len(names),
                                    "forwarderCacheHitsDuringReads": cache_hits},
          "postRotationCurrentRead": {"oldMaterial": old_material, "newMaterial": new_material,
                                      "oldParameters": previous["parameters"],
                                      "oldParametersDigest": previous["parametersDigest"],
                                      "freshOldParametersWireSha256": cached_wire,
                                      "oldCacheProbeIntervalMs": cache_interval_ms,
                                      "newParameters": new_obj["parameters"],
                                      "newParametersDigest": new_obj["parametersDigest"],
                                      "plaintextSha256": new_digest},
          "wireSha256": {Name.to_str(Name.from_bytes(k)): v for k, v in recovered.items()},
          "plaintextSha256": {label: digest for label, _, _, digest, _ in objects}})
    print(f"HISTORICAL_READ_ACCEPTANCE=PASS historicalObjects=2 historicalReads=3 "
          f"revoked=1 coldWires={len(recovered)} currentObjects=1 currentReads=1 sameServiceReaders=2 cacheWires=4")


if __name__ == "__main__":
    main()
