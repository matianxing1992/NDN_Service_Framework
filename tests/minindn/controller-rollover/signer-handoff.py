#!/usr/bin/env python3
"""Qualify planned Controller retirement between two adjacent MiniNDN nodes.

Prepare a fresh tests/fixtures/spec242/prepare-deployment.py --historical deployment.
Pass its deployment.json and --successor-node h242relay. The nodes use distinct NFDs,
credentials and state. The retired runtime loses its configured seal and its signer loses
current client trust. This emulates operator fencing on one Linux host.
"""

import argparse
import base64
import copy
import hashlib
import json
import os
from contextlib import closing
from datetime import datetime, timezone
from pathlib import Path
import re
import shutil
import sqlite3
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


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def version(text, marker):
    rows = list(records(text.splitlines(), marker))
    require(len(rows) == 1, f"missing or duplicate {marker}")
    values = rows[0][1]
    if marker == "NDNSF_REVOCATION_APPLIED":
        require(values.get("success") == "1" and values.get("kind") == "3", "service revocation failed")
    result = int(values["generation"]), int(values["epoch"])
    require(min(result) > 0, "empty Controller version")
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("config", type=Path)
    p.add_argument("--successor-node", required=True)
    p.add_argument("--historical-service", help="retained service from the --historical fixture policy")
    args = p.parse_args()
    source = args.config.resolve(strict=True)
    root = source.parent
    cfg = load_deployment(source)
    old, = (a for a in cfg["actors"] if a["kind"] == "controller")
    reader, = (a for a in cfg["actors"] if a["kind"] == "requester")
    repo, = (a for a in cfg["actors"] if a["kind"] == "repo")
    producer, = (a for a in cfg["actors"] if a["id"] == "historical-producer")
    nodes = {n["id"]: n for n in cfg["nodes"]}
    sec = cfg["security"]
    check = Path(sec["controller_recovery_checkpoint"]).resolve()
    require(cfg["platform"]["kind"] == "minindn" and old["node"] != args.successor_node
            and args.successor_node in nodes and sec.get("controller_recovery_bootstrap") is True
            and root in check.parents and not check.exists() and (root / "ca").is_dir()
            and all(n["nfd"]["mode"] == "managed" and root in Path(n["nfd"]["socket"]).resolve().parents
                    and not Path(n["nfd"]["socket"]).exists() for n in nodes.values()),
            "fresh, owned fixture with managed NFDs and an independent checkpoint required")
    app_id = old["identity"]
    service = cfg["network"]["service"]
    retained_service = args.historical_service or Name.to_str(
        Name.normalize(app_id)[:-1] + [Component.from_str("HistoricalRead")])
    route = cfg["requester"]["model_input"]["repo_target"]["repo_node"]
    require(Name.normalize(retained_service) != Name.normalize(service) and
            len({a["node"] for a in (old, reader, repo, producer)}) == 4,
            "require separate old signer's, requester's, Repo's and Producer's nodes and a retained service")
    old_cert = Path(sec["controller_certificate"]).resolve(strict=True)
    old_home = Path(old["credential_home"]).resolve(strict=True)
    reader_home = Path(reader["credential_home"]).resolve(strict=True)
    require(all(root in path.parents for path in (old_cert, old_home, reader_home))
            and old_home != reader_home, "independent fixture-owned actors required")
    prefix = Path(cfg["release"]["prefix"]) / "bin"
    cli = prefix / "App_ServiceController"
    probe = prefix / "ndnsf-historical-read-probe"
    publisher = prefix / "ndnsf-historical-publish"
    manifest = Path(cfg["release"]["manifest"])
    ledger = json.loads(manifest.read_bytes())["files"]
    for exe in (cli, probe, publisher, prefix / "DistributedRepoServer"):
        require(ledger[str(exe)]["installed"]["sha256"] == sha(exe), f"installed candidate mismatch: {exe}")

    os.umask(0o077)
    work = root / "signer-handoff"
    work.mkdir(mode=0o700)
    old_path = work / "old-state" / "controller.state"
    new_path = work / "new-state" / old_path.name
    new_home = work / "successor-home"
    pin_home = work / "successor-public-pin"
    for directory in (old_path.parent, new_path.parent, new_home, pin_home):
        directory.mkdir(mode=0o700)
    new_cert, handoff = work / "successor.cert", work / "handoff.ndn"
    new = dict(copy.deepcopy(old), id="handoff-successor", node=args.successor_node,
               credential_home=str(new_home), parameters={})
    original_digest = sha(manifest)
    state = {"schema": "ndnsf-di-host-operation-v1", "deployment_id": cfg["deployment_id"],
             "configuration_sha256": sha(source), "boot_id": uuid.uuid4().hex,
             "status": "STARTING", "launches": [], "evidence_root": str(work)}
    result = {"status": "FAIL", "scope": "single-host MiniNDN planned retirement",
              "configSha256": sha(source), "manifestSha256": original_digest}
    cfg["actors"] = [old, repo, producer, reader]
    cfg["network"]["routes"] = [{"from": reader["node"], "to": repo["node"], "prefix": route}]
    repo_store = work / "repo"
    repo_store.mkdir(mode=0o700)
    cfg["storage"]["repo_root"] = str(repo_store)
    children = []
    end = time.monotonic() + 180
    facility = MiniNdnFacility(cfg, work, state, end)

    def guard():
        cluster._check_running(children)
        require(time.monotonic() < end, "handoff deadline expired")

    def wait(check_fn, reason):
        until = min(time.monotonic() + 25, end)
        while time.monotonic() < until:
            guard()
            if check_fn():
                return
            time.sleep(0.05)
        raise RuntimeError(reason)

    def env(home, node, identity):
        return cluster._environment(home, nodes[node]["nfd"]["socket"], identity, state)

    def spawn(actor_id, kind, argv, environment, node):
        return cluster._spawn(state, work, actor_id, kind, argv, environment, guard,
                              launcher=facility.launcher(node), network_namespace=facility.namespace(node))

    def command(node, argv, environment, label, expected=0, timeout=15):
        guard()
        proc = facility.launcher(node)([str(v) for v in argv], env=environment,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            out, err = proc.communicate(timeout=min(timeout, max(0.1, end - time.monotonic())))
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=3)
        (work / (label + ".stderr")).write_bytes(err)
        require(proc.returncode == expected, f"{label} exit {proc.returncode}: {err[-256:]!r}")
        return out, err.decode(errors="replace")

    def actor_launch(actor, config, path, ring):
        argv, environment = cluster._native_launch(config, actor, work, state)
        environment.update(NDN_LOG="*=NONE:ndn_service_framework.ServiceController=INFO",
                           NDNSF_CONTROLLER_GENERATION_STATE=str(path), NDNSF_FILE_KEYRING=str(ring))
        return argv, environment

    def start(actor, argv, environment):
        proc, row = spawn(actor["id"], actor["kind"], argv, environment, actor["node"])
        owned = actor, proc, row
        children.append(owned)
        wait(lambda: cluster._ready(row, actor, state), actor["id"] + " did not become ready")
        return owned

    def stop(owned):
        stopped = cluster._stop_launch(owned[2])
        require(stopped.get("status") == "ZERO" and stopped.get("forced") is False
                and stopped.get("exitCode") == 0, owned[0]["id"] + " did not retire normally")
        state.setdefault("cleanup", []).append(dict(stopped, id=owned[0]["id"]))
        children.remove(owned)
        return stopped

    def start_repo(config, label):
        # Both process IDs use the same native --database derived from the original actor ID.
        argv, environment = cluster._native_launch(config, repo, work, state)
        database = Path(argv[argv.index("--database") + 1])
        require(database.parent == repo_store, "Repo database is outside the owned store")
        environment.update(NDNSF_DEPLOYMENT_ACTOR_ID=label,
                           NDN_LOG="*=NONE:ndnsf_distributed_repo.RepoNode=DEBUG")
        return start(dict(repo, id=label), argv, environment)

    def marker(output, name):
        matches = [values for _, values in records(output.decode().splitlines(), name)]
        require(len(matches) == 1, "missing or duplicate " + name)
        return matches[0]

    def publish_original(version):
        payload = work / "original.bin"
        payload.write_bytes((b"historical handoff\x00" * 400)[:7100])
        pib = str(Path(producer["credential_home"]) / ".ndn")
        config = work / "historical-writer.json"
        write_json(config, dict(pib=pib, identity=producer["identity"], controller=app_id,
            service=retained_service, object_prefix=producer["identity"] + "/NDNSF/OBJECT/handoff",
            payload_file=str(payload), repo_provider=repo["identity"], repo_route=route,
            repo_service_prefix=cfg["network"]["repo_service"], group=cfg["network"]["group"],
            controller_certificate=str(old_cert), trust_schema=sec["trust_schema"]))
        environment = env(producer["credential_home"], producer["node"], producer["id"])
        environment["NDNSF_CONTROLLER_CERT_FILE"] = str(old_cert)
        output, _ = command(producer["node"], [publisher, "--config", config], environment,
                            "historical-publish", timeout=45)
        (work / "historical-publish.stdout").write_bytes(output)
        obj = marker(output, "HISTORICAL_OBJECT")
        commit = marker(output, "HISTORICAL_REPO_COMMITTED")
        receipt = marker(output, "HISTORICAL_RECEIPT")
        require((int(obj["generation"]), int(obj["epoch"])) == version
                and obj["producer"] == producer["identity"] and obj["service"] == retained_service
                and obj["attribute"] == "/PERMISSION" + retained_service and obj["segments"] == "2"
                and obj["bytes"] == str(payload.stat().st_size)
                and obj["plaintextDigest"].lower() == "sha256:" + sha(payload)
                and Name.normalize(commit["object"]) == Name.normalize(obj["publication"])
                and receipt["object"] == obj["payload"], "original publication or Repo commit mismatch")
        return obj, commit["wireDigest"].lower(), receipt["name"]

    def read_original(obj, pin, floor, schema, label, prime=False):
        pib = str(reader_home / ".ndn")
        config = work / (label + ".json")
        options = {k: obj[k] for k in ("ck", "producer", "payload", "publication", "service",
                                       "generation", "epoch")}
        options.update(identity=reader["identity"], pib=pib, controller=app_id,
                       controller_pib=str(pin), producer_pib=pib, trust_schema=str(schema),
                       original_data_forwarding_hint=route, object=obj["payload"], policy=obj["attribute"])
        write_json(config, options)
        environment = env(reader_home, reader["node"], reader["id"])
        environment.update(NDNSF_CONTROLLER_MIN_VERSION="%d:%d" % floor, NDNSF_HISTORICAL_READ_DEBUG="1")
        output, _ = command(reader["node"], [probe, *(["--prime"] if prime else []), config],
                            environment, label, timeout=30)
        (work / (label + ".stdout")).write_bytes(output)
        row = marker(output, "HISTORICAL_PRIME_OK" if prime else "HISTORICAL_READ_OK")
        if not prime:
            require(row["bytes"] == obj["bytes"] and row["attribute"] == obj["attribute"]
                    and row["plaintextDigest"].lower() == obj["plaintextDigest"].lower()
                    and (row["generation"], row["epoch"]) == (obj["generation"], obj["epoch"]),
                    "successor cold read did not decrypt the original plaintext")

    def receipt_wire(full_name, predecessor_cert, label):
        wire, _ = command(reader["node"], ["ndnpeek", "-w", "3000", "-l", "2500", full_name],
                          env(reader_home, reader["node"], reader["id"]), label)
        name, _, _, signature = parse_data(wire)
        require(Name.normalize(full_name) ==
                name + [Component.from_bytes(hashlib.sha256(wire).digest(), Component.TYPE_IMPLICIT_SHA256)]
                and signature.signature_info.key_locator.name in (predecessor_cert.name, predecessor_cert.name[:-2])
                and verify_rsa(RSA.import_key(bytes(predecessor_cert.content)), signature),
                "historical receipt did not match the predecessor signature and original FullName")
        (work / (label + ".ndn")).write_bytes(wire)
        return wire

    def archive_read(node, pinned_pib, floor, material, label, expected=0):
        pib = str(reader_home / ".ndn")
        read = {"pib": pib, "controller_pib": str(pinned_pib), "identity": reader["identity"],
                "controller": app_id, "service": service, "generation": str(material[0]),
                "epoch": str(material[1]), "producer": reader["identity"], "producer_pib": pib,
                "object": "/", "ck": "/", "payload": "/", "publication": "/", "policy": "/"}
        path = work / (label + ".json")
        write_json(path, read)
        environment = env(reader_home, node, "cold-reader")
        environment.update(NDNSF_CONTROLLER_MIN_VERSION="%d:%d" % floor, NDNSF_HISTORICAL_READ_DEBUG="1")
        out, err = command(node, [probe, "--parameters-only", path], environment, label, expected)
        (work / (label + ".stdout")).write_bytes(out)
        if expected:
            require(b"HISTORICAL_READ_FAILED stage=parameters" in out and "signature did not verify" in err
                    and any("POLICY-STATUS" in line and "signature did not verify" in line
                            for line in err.splitlines()), "old pin did not reject a received current status")
            return None
        match = re.search(rb"^HISTORICAL_PARAMETERS_OK generation=(\d+) epoch=(\d+) digest=sha256:([A-Fa-f0-9]{64})$",
                          out, re.M)
        require(match and (int(match[1]), int(match[2])) == material, "verified archive response missing")
        return match[3].decode().lower()

    try:
        facility.start(guard)
        for n in cfg["nodes"]:
            directory = work / ("forwarder-" + n["id"])
            directory.mkdir()
            cluster._start_forwarder(cfg, n, state, work, directory, children, end, guard, spawn, disable_cs=True)
        left, right = (facility.hosts[nodes[node]["host"]] for node in (old["node"], new["node"]))
        require(left.connectionsTo(right) and facility.namespace(old["node"]) != facility.namespace(new["node"]),
                "choose adjacent nodes in distinct network namespaces")
        facility.routes(work, guard)

        _, original_env = cluster._native_launch(cfg, old, work, state)
        seal_source = Path(original_env["NDNSF_FILE_KEYRING"]).resolve(strict=True)
        require(root in seal_source.parents, "requires fixture-owned seal")
        old_ring, new_ring = work / "old-runtime.ring", work / "new-runtime.ring"
        shutil.copy2(seal_source, old_ring)
        shutil.copy2(seal_source, new_ring)
        successor_env = env(new_home, new["node"], new["id"])
        ca_env = env(root / "ca", new["node"], "fixture-ca")
        old_node, new_node = old["node"], new["node"]
        ca = Name.to_str(Name.normalize(app_id)[:-1])
        command(new_node, ["ndnsec", "key-gen", "-t", "r", app_id], successor_env, "new-key")
        req = work / "successor.req"
        req.write_bytes(command(new_node, ["ndnsec", "sign-req", app_id], successor_env, "cert-request")[0])
        # Reuse the fixture's already-open CA-issued validity window. ndn-cxx rounds an
        # implicit "not before now" up to the next second, which can race immediate handoff.
        previous_cert = parse_certificate(base64.b64decode(old_cert.read_bytes()))
        validity = previous_cert.signature_info.validity_period
        begin = bytes(validity.not_before).decode("ascii")
        end_date = bytes(validity.not_after).decode("ascii")
        require(begin <= datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") < end_date,
                "predecessor credential outside fixture validity window")
        new_cert.write_bytes(command(new_node, ["ndnsec", "cert-gen", "--sign-id", ca,
                                                "--issuer-id", "handoff", "--not-before", begin.replace("T", ""),
                                                "--not-after", end_date.replace("T", ""), "--request", req],
                                     ca_env, "cert-issue")[0])
        issued = parse_certificate(base64.b64decode(new_cert.read_bytes()))
        require(issued.name[:-4] == previous_cert.name[:-4] == Name.normalize(app_id)
                and bytes(issued.content) != bytes(previous_cert.content)
                and bytes(issued.signature_info.validity_period.not_before) == bytes(validity.not_before)
                and bytes(issued.signature_info.validity_period.not_after) == bytes(validity.not_after),
                "successor credential does not bind new key, same identity and fixture validity")
        command(new_node, ["ndnsec", "cert-install", "--key-default", new_cert], successor_env, "cert-install")
        # The PIB contains public identities, keys and certificates. A SQLite backup captures
        # committed WAL pages without copying the successor's TPM or provisioning a signing key.
        public_pib = pin_home / ".ndn"
        public_pib.mkdir(mode=0o700)
        source_db = (new_home / ".ndn" / "pib.db").resolve(strict=True)
        with closing(sqlite3.connect(source_db.as_uri() + "?mode=ro", uri=True)) as signed_pib:
            with closing(sqlite3.connect(public_pib / "pib.db")) as pinned_pib:
                signed_pib.backup(pinned_pib)
        pinned, _ = command(new_node, ["ndnsec", "cert-dump", "-i", app_id],
                            env(pin_home, new_node, "public-pin"), "new-reader-pin")
        require(base64.b64decode(pinned) == base64.b64decode(new_cert.read_bytes())
                and not any((public_pib / "ndnsec-key-file").glob("*")),
                "cold reader pin must match the successor certificate and contain no private key")
        # Preserve every other trust rule; replace the fixture's exact Controller anchor.
        trust = Path(sec["trust_schema"]).read_text()
        require(trust.count(json.dumps(str(old_cert))) == 1, "missing exact predecessor schema anchor")
        new_schema = work / "successor-trust.conf"
        new_schema.write_text(trust.replace(json.dumps(str(old_cert)), json.dumps(str(new_cert))))
        trigger = work / "revoke"
        old.setdefault("parameters", {}).update({"revoke-trigger-file": str(trigger), "revoke-kind": "service",
             "revoke-identity": reader["identity"], "revoke-service": service,
             "revoke-attribute": "/PERMISSION" + service})
        old_argv, old_env = actor_launch(old, cfg, old_path, old_ring)
        predecessor = start(old, old_argv, old_env)
        old_log = Path(predecessor[2]["log"])
        first = version(old_log.read_text(errors="replace"), "NDNSF_CONTROLLER_VERSION")
        original_repo = start_repo(cfg, "original-repo")
        obj, wire_digest, receipt_name = publish_original(first)
        original_receipt = receipt_wire(receipt_name, previous_cert, "predecessor-receipt")
        # This process records the historical permission. It never requests a historical DKEY.
        read_original(obj, reader_home / ".ndn", first, sec["trust_schema"], "prime-old-permission", prime=True)
        stop(original_repo)
        trigger.touch(exist_ok=False)
        wait(lambda: "NDNSF_REVOCATION_APPLIED " in old_log.read_text(errors="replace"), "no committed revocation")
        revoked = version(old_log.read_text(errors="replace"), "NDNSF_REVOCATION_APPLIED")
        events = [fields for _, fields in records(old_log.read_text(errors="replace").splitlines(),
                  "NDNSF_CONTROLLER_REVOKED") if fields.get("kind") == "3"
                  and Name.normalize(fields["identity"]) == Name.normalize(reader["identity"])
                  and Name.normalize(fields["service"]) == Name.normalize(service)]
        require(revoked > first and len(events) == 1 and int(events[0]["generation"]) == revoked[0]
                and re.fullmatch(str(revoked[1]) + r"(?:NDNSF_REVOCATION_TRIGGERED)?", events[0]["epoch"]),
                "targeted revocation did not advance authorization version")
        prior_digest = archive_read(new_node, reader_home / ".ndn", revoked, first, "previous-signer-read")
        prepare = [cli, "--controller-prefix", app_id, "--prepare-signer-handoff", new_cert]
        out, error = command(old_node, prepare, old_env, "live-writer", expected=1)
        require(not out and "while the current writer is active" in error, "wrong busy-writer boundary")
        stopped = stop(predecessor)
        frozen = (sha(old_path), sha(check))
        handoff.write_bytes(command(old_node, prepare, old_env, "prepare-handoff")[0])
        require(handoff.stat().st_size > 0 and (sha(old_path), sha(check)) == frozen,
                "handoff preparation altered the committed state/checkpoint")

        # Keep backups separate from the independent checkpoint. Local flock files never transfer.
        for path in old_path.parent.iterdir():
            if path.name.endswith(".lock"):
                continue
            dest = new_path.parent / path.name
            if path.is_dir():
                shutil.copytree(path, dest)
            else:
                shutil.copy2(path, dest)
        require(sha(new_path) == frozen[0], "state transfer differs")
        os.rename(old_ring, work / "retired-runtime.ring")
        require(not old_ring.exists(), "old runtime still owns its configured seal")
        output, error = command(old_node, old_argv, old_env, "retired-runtime-restart", expected=1)
        require(b"NDNSF_CONTROLLER_READY" not in output and "NDNSF_FILE_KEYRING_UNREADABLE" in error
                and not old_ring.exists() and (sha(old_path), sha(check)) == frozen,
                "retired runtime did not refuse startup at the missing seal")
        new_cfg = copy.deepcopy(cfg)
        new_cfg["actors"] = [new, repo, producer, reader]
        new_cfg["security"].update(controller_certificate=str(new_cert), trust_schema=str(new_schema),
                                   controller_min_version="%d:%d" % revoked)
        new_cfg["security"].pop("controller_recovery_bootstrap")
        new["parameters"]["signer-handoff-file"] = str(handoff)
        new["file_keyring"] = str(new_ring)
        facility.config["actors"] = new_cfg["actors"]
        facility.routes(work, guard)
        new_argv, new_env = actor_launch(new, new_cfg, new_path, new_ring)
        live = start(new, new_argv, new_env)
        current = version(Path(live[2]["log"]).read_text(errors="replace"), "NDNSF_CONTROLLER_VERSION")
        require(current > revoked and sha(check) != frozen[1], "successor did not advance authenticated state")
        archive_read(old_node, reader_home / ".ndn", current, first, "retired-signer-read", expected=1)
        after = archive_read(old_node, pin_home / ".ndn", current, first, "current-signer-read")
        require(after == prior_digest, "archived public parameter bytes changed")
        reopened = start_repo(new_cfg, "reopened-repo")
        require(reopened[1].pid != original_repo[1].pid, "Repo was not restarted")
        require(receipt_wire(receipt_name, previous_cert, "successor-served-receipt") == original_receipt,
                "successor did not serve the exact witnessed predecessor receipt")
        read_original(obj, public_pib, current, new_schema, "successor-cold-read")
        names = [obj["publication"], obj["ck"], *(obj["payload"] + "/seg=" + str(i) for i in range(2))]
        served = {Name.to_bytes(Name.normalize(name)) for name in re.findall(
            r"(?:original packet put|repo-original-data-served) name=(\S+)",
            Path(reopened[2]["log"]).read_text(errors="replace"))}
        require(all(Name.to_bytes(Name.normalize(name)) in served for name in names),
                "reopened Repo did not serve each original during the cold read")
        aggregate = hashlib.sha256()
        original_digests = {}
        for i, expected_name in enumerate(names):
            wire, _ = command(reader["node"], ["ndnpeek", "-F", route, "-w", "3000", "-l", "2500",
                                               expected_name], env(reader_home, reader["node"], reader["id"]),
                              "original-" + str(i))
            require(parse_data(wire)[0] == Name.normalize(expected_name), "wrong original packet name")
            aggregate.update(wire)
            original_digests[expected_name] = hashlib.sha256(wire).hexdigest()
        require("sha256:" + aggregate.hexdigest() == wire_digest, "original Repo wires changed across handoff")
        result["historicalColdRead"] = dict(reader=reader["identity"], readerNode=reader["node"],
            producer=producer["identity"], object=obj["payload"], oldVersion=first,
            receiptFullName=receipt_name, receiptWireSha256=hashlib.sha256(original_receipt).hexdigest(),
            originalPacketWireSha256=original_digests, originalSetDigest=wire_digest,
            plaintextDigest=obj["plaintextDigest"].lower(), bytes=int(obj["bytes"]),
            repoDatabase=str(repo_store / (repo["id"] + ".sqlite")),
            originalRepoPid=original_repo[1].pid, reopenedRepoPid=reopened[1].pid)

        # Stress the old state with the real seal restored explicitly for this negative probe only.
        # An independent copy of the advanced checkpoint avoids mistaking lock contention for fencing.
        witness = work / "recovery-witness"
        witness.mkdir(mode=0o700)
        witness_file = witness / "current.checkpoint"
        shutil.copy2(check, witness_file)
        stale_env = dict(old_env, NDNSF_FILE_KEYRING=str(work / "retired-runtime.ring"),
                         NDNSF_CONTROLLER_RECOVERY_CHECKPOINT=str(witness_file),
                         NDNSF_CONTROLLER_MIN_VERSION="%d:%d" % current)
        stale_env.pop("NDNSF_CONTROLLER_RECOVERY_BOOTSTRAP", None)
        output, error = command(old_node, old_argv, stale_env, "stale-restart", expected=1)
        require(b"NDNSF_CONTROLLER_READY" not in output and
                ("recovery checkpoint has no matching state" in error or
                 "differs from the recovery checkpoint" in error), "stale recovery rejected at wrong boundary")
        require(sha(witness_file) == sha(check) and sha(old_path) == frozen[0],
                "stale restart altered state or checkpoint")
        require(sha(manifest) == original_digest, "release changed")
        result.update(predecessorNode=old_node, successorNode=new_node, previousVersion=first,
            revocationVersion=revoked, successorVersion=current, predecessorCertSha256=sha(old_cert),
            successorCertSha256=sha(new_cert), checkpointBefore=frozen[1], checkpointAfter=sha(check),
            handoffSha256=sha(handoff), verifiedArchivedParametersSha256=after,
            predecessorStopped=stopped, oldRuntimeSealWithdrawn=not old_ring.exists(),
            retiredRuntimeRestartRefused=True, staleStateRefused=True, retiredCurrentSignerRejected=True,
            physicalHostFencing="outside single-host simulation")
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        try:
            closed = cluster._close(state, work, facility)
            require(closed["status"] == "STOPPED" and state["facility"]["status"] == "STOPPED"
                    and all(row["status"] == "ZERO" and row.get("forced") is False for row in
                            [*state["cleanup"], *state["facility"]["cleanup"]]), "failed graceful cleanup")
            if "failure" not in result:
                exits = {row["id"]: row.get("exitCode") for row in state["cleanup"]}
                require(exits.get(old["id"]) == exits.get(new["id"]) == 0, "missing normal Controller exits")
                require(exits.get("original-repo") == exits.get("reopened-repo") == 0, "missing normal Repo exits")
                result.update(status="PASS", controllerExitCodes={k: exits[k] for k in (old["id"], new["id"])},
                              repoExitCodes={k: exits[k] for k in ("original-repo", "reopened-repo")})
        except Exception as error:
            result["cleanupFailure"] = str(error)
            if "failure" not in result:
                raise
        finally:
            write_json(work / "result.json", result)


if __name__ == "__main__":
    main()
