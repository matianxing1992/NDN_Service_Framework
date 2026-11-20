#!/usr/bin/env python3
"""Provision a local, installed Spec 242 deployment with fresh actor identities."""

import argparse
from datetime import datetime, timedelta, timezone
import json
import os
import re
import site
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import uuid

def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def child_pythonpath():
    """Python search path that survives an isolated HOME for child processes."""
    user_site = site.getusersitepackages()
    inherited = os.environ.get("PYTHONPATH", "")
    return os.pathsep.join(value for value in (user_site, inherited) if value)


def capture(command, env=None):
    return subprocess.run(command, env=env, check=True, timeout=30,
                          stdout=subprocess.PIPE).stdout


def prepare_identities(root, prefix, namespace, group, actors):
    ca = root / "ca"
    ca.mkdir(mode=0o700)
    environments = {}
    certificates = {}

    # The fixture isolates each actor's HOME for ndnsec; that also disables the
    # invoking user's site-packages, so carry the effective Python search path
    # (and runtime library path) into the child processes explicitly.
    def environment(home):
        env = dict(PATH=os.environ["PATH"], HOME=str(home),
                   NDN_CLIENT_PIB="pib-sqlite3:" + str(home / ".ndn"),
                   NDN_CLIENT_TPM="tpm-file:", PYTHONPATH=child_pythonpath())
        if os.environ.get("LD_LIBRARY_PATH"):
            env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]
        return env

    ca_env = environment(ca)
    capture(["ndnsec", "key-gen", "-t", "e", namespace], ca_env)
    now = datetime.now(timezone.utc)
    not_before = (now - timedelta(minutes=1)).strftime("%Y%m%d%H%M%S")
    not_after = (now + timedelta(days=1)).strftime("%Y%m%d%H%M%S")
    for actor in actors:
        actor_id, identity = actor["id"], actor["identity"]
        home = root / "homes" / actor_id
        home.mkdir(mode=0o700, parents=True)
        env = environment(home)
        capture(["ndnsec", "key-gen", "-t", "r", identity], env)
        request = root / (actor_id + ".req")
        request.write_bytes(capture(["ndnsec", "sign-req", identity], env))
        certificate = root / (actor_id + ".cert")
        certificate.write_bytes(capture(["ndnsec", "cert-gen", "--sign-id", namespace,
                                         "--issuer-id", "spec242", "--not-before", not_before,
                                         "--not-after", not_after, "--request", str(request)],
                                        ca_env))
        request.unlink()
        capture(["ndnsec", "cert-install", "--key-default", str(certificate)], env)
        actor["credential_home"] = str(home)
        environments[actor_id] = env
        certificates[actor_id] = str(certificate)
    for actor in actors:
        home = Path(actor["credential_home"])
        capture([str(prefix / "bin/spec226-public-trust-installer"), str(home / ".ndn"),
                 "tpm-file:", *certificates.values()], environments[actor["id"]])
        capture([str(prefix / "bin/ndnsf-identity-check"), str(home), actor["identity"]],
                environments[actor["id"]])
    from native_probe_validation_links import render_anchored_probe_validation_schema
    trust = root / "trust-schema.conf"
    trust.write_text(render_anchored_probe_validation_schema(
        namespace, certificates.values(), sync_prefix=group), encoding="utf-8")
    return environments, certificates, trust


def secure_random(path):
    with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as stream:
        stream.write(os.urandom(32))


def topology_hosts(path):
    hosts = []
    section = None
    for line in Path(path).read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith("["):
            section = stripped
            continue
        if section == "[nodes]" and stripped.endswith(":") and stripped[:-1]:
            hosts.append(stripped[:-1])
    return hosts


def prepare_deployment(root, prefix, ledger, journal_root, source=None, source_sha256=None,
                       tokenizer=None, layer_cut=None, rank_weights=None,
                       prepared_workload=None, link_bw=None, link_delay=None, probe=True,
                       link_loss=None, max_new_tokens=None, kv_capture=True,
                       storage_budget=None, provider_cpus=None, dynamic_shaping=False,
                       topology=None, route_metric=None):
    checkout = Path(__file__).resolve().parents[3]
    explicit_topology = topology is not None
    if route_metric is not None and not explicit_topology:
        raise ValueError("an explicit topology is required for a route metric")
    if explicit_topology and (dynamic_shaping or
                              any(value is not None for value in (link_bw, link_delay, link_loss))):
        raise ValueError("derive network cells separately when using an explicit topology")
    sys.path.insert(0, str(checkout / "Experiments"))
    from NDNSF_DI_Qwen06B_Native_Minindn import (
        digest_bytes as digest, make_external_key, preparation_cache_budget,
    )
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat, load_pem_public_key

    root.mkdir(mode=0o700)
    real_model = source is not None or prepared_workload is not None
    # The frozen N6 cell assembles the model material under 1% loss (>15 min),
    # so the request window covers the lossy assembly plus all four rounds.
    # The native conversation retention is capped at one hour and must not be
    # shorter than the request window; 27 minutes covers the lossy N6 assembly
    # (about 18 minutes) plus all four rounds with the 3.54 M ms retention.
    request_timeout_ms = 1620000 if real_model else 240000
    # Leave room for both turns and strictly outlive either signed Selection.
    retention_ms = min(3600000, 2 * request_timeout_ms + 300000) if real_model else 600000
    namespace = "/spec234" if real_model else "/spec242"
    group = namespace + "/group"
    service = namespace + "/Infer"
    grant_service = namespace + "/ArtifactGrant"
    repo_service = "/NDNSF/DistributedRepo/Object/v1"
    epoch = "spec242-epoch"
    bundle = root / "workload"
    if prepared_workload:
        bundle.mkdir(mode=0o700)
        # A frozen multi-round transcript adds one delta-<n>.bin per later round
        # and a transcript digest file; copy every maintained input present.
        names = ["catalog.json", "generation.json", "tokenizer.json", "prompt.bin",
                 "delta.bin", "frozen-transcript.json"]
        names += sorted(path.name for path in prepared_workload.glob("delta-*.bin"))
        for name in names:
            path = prepared_workload / name
            if not path.exists():
                if name in ("delta.bin", "frozen-transcript.json"):
                    continue
                raise ValueError(f"prepared workload file is missing: {name}")
            path = prepared_workload / name
            limit = 32 << 20
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            with os.fdopen(fd, "rb") as stream:
                info = os.fstat(stream.fileno())
                if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
                    raise ValueError(f"invalid prepared workload file: {name}")
                data = stream.read(limit + 1)
                if len(data) != info.st_size:
                    raise ValueError(f"prepared workload file size changed: {name}")
            (bundle / name).write_bytes(data)
    else:
        with tempfile.TemporaryDirectory(dir=root, prefix="catalog-home-") as home:
            env = os.environ.copy()
            env.update(HOME=home, NDN_CLIENT_PIB="pib-sqlite3:" + str(Path(home) / ".ndn"),
                       NDN_CLIENT_TPM="tpm-file:", PYTHONPATH=child_pythonpath())
            prepare = [sys.executable, str(Path(__file__).with_name("prepare-workload.py")),
                       "--output", str(bundle), "--requester", namespace + "/requester",
                       "--protection-epoch", epoch]
            if source:
                prepare += ["--source", str(source), "--source-sha256", source_sha256,
                            "--tokenizer", str(tokenizer), "--layer-cut", str(layer_cut)]
                if rank_weights is not None:
                    prepare += ["--rank-weights", *map(str, rank_weights)]
                if max_new_tokens is not None:
                    prepare += ["--max-new-tokens", str(max_new_tokens)]
            subprocess.run(prepare, env=env, check=True, timeout=300 if source else 30)
    catalog = json.loads((bundle / "catalog.json").read_bytes())
    options = json.loads((bundle / "generation.json").read_bytes())
    split = catalog["splitter"]
    # Pure layer-split plans (every stage degree one) register the stage
    # identities directly; mixed plans keep the per-rank role names for their
    # sharded stages and the Tensor/Rank/0 convention for the unsharded ones.
    pure_pp = all(degree == 1 for degree in split["tensor_degrees"])
    roles = (list(split["roles"]) if pure_pp else
             [stage + "/Tensor/Rank/" + str(rank)
              for stage, degree in zip(split["roles"], split["tensor_degrees"])
              for rank in range(degree)])
    actor_nodes = [("controller", "controller", "h242ctl"),
                   ("authority", "artifact-authority", "h242auth"),
                   ("repo", "repo", "h242repo"),
                   ("requester", "requester", "h242user")]
    actor_nodes += [(f"provider-{rank}", "provider", f"h242p{rank}") for rank in range(len(roles))]
    if explicit_topology:
        declared = set(topology_hosts(topology))
        missing = {node for _, _, node in actor_nodes} - declared
        if missing:
            raise ValueError("explicit topology is missing actor hosts: " + ", ".join(sorted(missing)))
    if provider_cpus is not None and len(provider_cpus) != len(actor_nodes) - 4:
        raise ValueError("provider CPU quotas must match the provider count")
    actors = []
    for index, (name, kind, node) in enumerate(actor_nodes):
        cpus = 2 if kind == "provider" else 1
        if kind == "provider" and provider_cpus is not None:
            cpus = provider_cpus[index - 4]
        actors.append(dict(id=name, kind=kind, identity=namespace + "/" + name, node=node,
                           resources=dict(cpus=cpus), parameters={}))
    providers = actors[4:]
    environments, certificates, trust = prepare_identities(root, prefix, namespace, group, actors)
    by_id = {actor["id"]: actor for actor in actors}

    policy = root / "controller-policy.info"
    repo_operations = [repo_service + "/" + operation for operation in (
        "STORE", "FETCH", "STATUS", "INSERT", "MANIFEST", "DATA_RECEIPT",
        "USER_STORAGE_GRANT", "USER_STORAGE_USAGE")]
    policy_rows = [(by_id["authority"]["identity"], [grant_service]),
                   (by_id["repo"]["identity"], repo_operations)]
    policy_rows += [(actor["identity"], [service, service + "/ROLE" + role])
                    for role, actor in zip(roles, providers)]
    policy.write_text(
        "name " + by_id["controller"]["identity"] + "/NDNSF/ControllerPolicy/v1\n" +
        "provider-policies {\n" + "\n".join(
            "provider-policy {\nfor " + identity + "\nallow {\n" + "\n".join(services) + "\n}\n}"
            for identity, services in policy_rows) +
        "\n}\nuser-policies {\nuser-policy {\nfor " + by_id["requester"]["identity"] +
        "\nallow {\n" + "\n".join([service, grant_service, *repo_operations]) + "\n}\n}\n}\n",
        encoding="utf-8")

    key_root = root / "grant-keys"
    key_root.mkdir(mode=0o700)
    auth_private, auth_public = make_external_key(key_root, "authority", timeout=30)
    requester_private, requester_public = make_external_key(key_root, "requester", timeout=30)
    content = key_root / "content.key"
    journal = key_root / "conversation.key"
    conversation_id = "spec242-" + uuid.uuid4().hex
    secure_random(content)
    secure_random(journal)
    model_uri = "/Model/Spec234/Qwen" if real_model else "/Model/Spec242/Qwen"
    write_json(key_root / "trust-root-registry-v1.json", {
        "schemaVersion": 1, "status": "CONFIGURED",
        "artifactPolicyAuthority": {
            "acceptedModelFamilies": [model_uri], "authorityId": by_id["authority"]["identity"],
            "grantSchema": "ndnsf-di-key-grant-v1", "keyId": "spec242-artifact-policy",
            "protectionEpochs": [epoch], "publicKeyAlgorithm": "ed25519",
            "publicKeyPath": str(auth_public.relative_to(root)),
            "publicKeySha256": digest(auth_public.read_bytes()),
            "signatureAlgorithm": "ed25519"}})
    offer_entries, offer_keys, recipients = [], {}, {}
    candidate_digest = digest(ledger.read_bytes())
    for role, actor in zip(roles, providers):
        name, identity = actor["id"], actor["identity"]
        own_keys = key_root / name
        own_keys.mkdir(mode=0o700)
        recipient_private, recipient_public = make_external_key(own_keys, "recipient", timeout=30)
        offer_private, offer_public = make_external_key(own_keys, "offer", timeout=30)
        recipient_map = own_keys / "recipients.json"
        write_json(recipient_map, {identity: str(recipient_private)})
        recipients[identity] = str(recipient_public)
        raw = load_pem_public_key(offer_public.read_bytes()).public_bytes(Encoding.Raw, PublicFormat.Raw)
        signer = digest(raw)
        offer_keys[signer] = str(offer_public)
        offer_entries.append(dict(provider=identity, service=service, signerKeyId=signer,
            keyLocatorPrefix=capture(["ndnsec", "get-default", "-k", "-i", identity],
                                     environments[name]).decode().strip(),
            certificateName=capture(["ndnsec", "get-default", "-c", "-i", identity],
                                    environments[name]).decode().strip()))
        actor["parameters"] = {
            "roles": role, "offer-backend": "onnxruntime-cpu", "offer-can-provision": True,
            "selection-offer-key-file": str(offer_private), "grant-authority-public-key": str(auth_public),
            "recipient-key-map": str(recipient_map), "allow-selection-generation-options": True,
            "capture-complete-kv-state": kv_capture,
            # The N6 provider link carries the model material under the frozen
            # packet loss; the lossy fetch legitimately exceeds ten minutes, so
            # the assembly budget covers the shaped transfer (still bounded by
            # the request deadline).
            "assembly-timeout-ms": 2400000 if real_model else 120000,
            "repo-fetch-timeout-ms": 480000 if real_model else 60000,
            "conversation-retention-ms": retention_ms}
    if kv_capture:
        providers[-1]["parameters"]["capture-logits-output"] = "logits"
    by_id["repo"]["parameters"]["request-timeout-ms"] = 480000 if real_model else 60000

    source = catalog["source"]
    max_source = catalog["recipe"]["max_source_bytes"]
    max_assembled = catalog["recipe"]["max_assembled_bytes"]
    service_fields = dict(dependencies=[], dtype="int4", model=model_uri, modelFamily="qwen",
                          modelFormat="onnx", modelRepository=catalog["model"]["model_name"],
                          modelRevision=catalog["model"]["source_revision"],
                          plannerKind="native-qwen-layer", quantization="weight_only_int4",
                          roles=roles, runtimeBackend="onnxruntime")
    plan, manifest = root / "plan.json", root / "manifest.json"
    write_json(plan, dict(version=2, services=[dict(service=service, executionPolicy="DATA_DRIVEN_V2",
                                                    schemaVersion=2, **service_fields)]))
    artifacts = [dict(artifact=model_uri + "/role" + role, backend="onnxruntime",
                      exportArtifactDigest=source["digest"], filename="model.onnx", kind="model",
                      materialization="post-selection-canonical-assembly", role=role,
                      metadata={"executionProvider": "cpu", "deviceId": "cpu0",
                                "allowCpuFallback": "false"}) for role in roles]
    write_json(manifest, dict(services=[dict(name=service, artifacts=artifacts, **service_fields)]))
    repo_identity = by_id["repo"]["identity"]
    requester = by_id["requester"]["identity"]
    security_digest = digest(policy.read_bytes())
    options_file = str(bundle / "generation.json")
    transcript = catalog.get("transcript")
    if transcript and real_model:
        # One frozen conversation: round 1 opens it and every later round adds
        # exactly its delta tokens because the runtime keeps the historical KV.
        inputs = tuple(transcript["inputFiles"])
        turns = [dict(input_file=str(bundle / input_name), options_file=options_file,
                      output_file=str(root / f"output-{i}.json"),
                      checkpoint_output_file=str(root / f"checkpoint-{i}.json"))
                 for i, input_name in enumerate(inputs)]
    else:
        inputs = ("prompt.bin", "delta.bin")
        turns = [dict(input_file=str(bundle / input_name), options_file=options_file,
                      output_file=str(root / f"output-{i}.json"),
                      checkpoint_output_file=str(root / f"checkpoint-{i}.json"))
                 for i, input_name in enumerate(inputs * (2 if real_model else 1))]
    turn_options = dict(mode="FULL_CONTEXT", generation_id=uuid.uuid4().hex,
                        parent_context_epoch=0, retention_duration_ms=retention_ms)
    if transcript and real_model:
        # One frozen conversation keeps appending: the Maintained Requester
        # continues an opened conversation across turns, so the per-turn
        # conversation_id grouping (two turns each) must not be used here.
        continuation_options = bundle / "continuation.json"
        write_json(continuation_options, {**options,
                                          "maxNewTokens": transcript.get("tokensPerRound", 32)})
        for turn in turns[1:]:
            turn["options_file"] = str(continuation_options)
    elif real_model:
        # The maintained requester opens independent conversations as pairs.
        # Turns 0 and 2 both start from the same prompt with empty KV.
        continuation_options = bundle / "continuation.json"
        write_json(continuation_options, {**options, "maxNewTokens": 2})
        for first in range(0, len(turns), 2):
            own_id = uuid.uuid4().hex
            for turn in turns[first:first + 2]:
                turn["conversation_id"] = own_id
            turns[first + 1]["options_file"] = str(continuation_options)
    else:
        turn_options["conversation_id"] = conversation_id
    placement = {role: actor["identity"] for role, actor in zip(roles, providers)}
    if not explicit_topology:
        topology = checkout / "tests/fixtures/spec242/minindn-topology.conf"
    if not explicit_topology and len(providers) == 4:
        # The three-rank TP stage and singleton PP stage need four distinct hosts.
        topology_text = topology.read_text()
        topology = root / "minindn-topology.conf"
        topology.write_text(topology_text.replace("\n[links]", "h242p3:\n\n[links]")
                            .replace("\n[faces]", "h242relay:h242p3 delay=2ms bw=1000\n\n[faces]")
                            + "h242relay:h242p3 cost=10\n", encoding="utf-8")
    shaping_block = None
    if dynamic_shaping:
        # The Provider data links are shaped after the model material was
        # fetched and assembled; the topology itself keeps the baseline so the
        # 919 MB fetch is not throttled (E2 pressure applies to the inference
        # window, not to the setup transfer).
        shaping_block = dict(bandwidth_mbps=link_bw if link_bw is not None else 100,
                             delay_ms=link_delay if link_delay is not None else 2,
                             marker='NDNSF_DI_ONNX_TIMING',
                             timeout_seconds=900)
        if link_loss is not None:
            shaping_block['loss_percent'] = link_loss
    if not dynamic_shaping and (link_bw is not None or link_delay is not None or link_loss is not None):
        # E2 shapes the Provider data links; control/Repo budget links keep the
        # baseline. Rewriting every link would throttle the source publication
        # and control plane as well, which N1's 10 Mbps PROVIDER profile must
        # not do.
        provider_nodes = {actor['node'] for actor in actors if actor['kind'] == 'provider'}
        lines = []
        for line in Path(topology).read_text().splitlines():
            stripped = line.strip()
            shaped = (':' in stripped and 'delay=' in stripped and
                      any(node in stripped.split()[0].split(':') for node in provider_nodes))
            if shaped:
                if link_bw is not None:
                    line = re.sub(r"bw=[0-9.]+", f"bw={link_bw:g}", line)
                if link_delay is not None:
                    line = re.sub(r"delay=[0-9.]+ms", f"delay={link_delay:g}ms", line)
                if link_loss is not None:
                    if re.search(r"loss=[0-9.]+", line):
                        line = re.sub(r"loss=[0-9.]+", f"loss={link_loss:g}", line)
                    else:
                        # MiniNDN link parameters accept an optional loss percentage.
                        line = re.sub(r"(delay=[0-9.]+ms)", r"\1 loss=" + f"{link_loss:g}", line)
            lines.append(line)
        topology = root / "minindn-topology.conf"
        topology.write_text("\n".join(lines) + "\n", encoding="utf-8")
    config = {
        "schema": "ndnsf-di-deployment-v1",
        "probes": {"enabled": bool(probe), "ping_count": 3},
        **({"shaping": shaping_block} if shaping_block is not None else {}),
        "deployment_id": "spec234-qwen-int4" if real_model else "spec242-hybrid",
        "release": {"prefix": str(prefix), "manifest": str(ledger)},
        "platform": {"kind": "minindn", "topology": str(topology),
                     "topology_sha256": digest(topology.read_bytes()),
                     **({"route_metric": route_metric} if route_metric is not None else {})},
        # Every host the topology declares must be present, including a
        # forwarding-only host that no actor uses (for example the third rank
        # host in a two-provider layout).
        "nodes": [dict(id=host, host=host, cpus=3,
                       nfd=dict(mode="managed", address="127.0.0.1", port=6363,
                                socket=str(root / (host + ".sock"))))
                  for host in topology_hosts(topology)],
        "actors": actors,
        "network": dict(group=group, service=service, artifact_service=grant_service,
                        repo_service=repo_service, routes=[]),
        "security": dict(controller_certificate=certificates["controller"],
                         controller_policy=str(policy), trust_schema=str(trust)),
        "storage": {**{key + "_root": str(root / key) for key in
                       ("cache", "repo", "scratch", "state", "evidence")},
                    "max_bytes": (storage_budget if storage_budget is not None
                                  else ((8 << 30) if real_model else (128 << 20)))},
        "model": dict(manifest=str(manifest), plan=str(plan), tokenizer=str(bundle / "tokenizer.json")),
        "requester": dict(
            catalog=catalog, fixed_placement={"role_providers": placement},
            model_input=dict(mode="source", repo_target=dict(repo_node=repo_identity,
                provider_identity=repo_identity, service_prefix=repo_service),
                preparation_state_file=str(root / "prepare-state.json"),
                private_handle_file=str(root / "model-handle.json")),
            grant=dict(requester_private_key_file=str(requester_private),
                       authority_public_key_file=str(auth_public), protection_epoch=epoch),
            offer_admission=dict(candidate_digest=candidate_digest, public_key_files=offer_keys,
                policy=dict(candidateDigest=candidate_digest, candidateId=candidate_digest[7:],
                            trustSchema=namespace + "/trust", entries=offer_entries)),
            limits=dict(bootstrap_ms=600000 if real_model else 120000,
                        max_source_bytes=max_source, max_assembled_bytes=max_assembled,
                        max_prepared_bytes=preparation_cache_budget(max_source, max_assembled)),
            request=dict(service=service, task="text-generation", generation_mode="TOKEN_STREAMING",
                         tokenizer_digest=options["tokenizerDigest"], security_policy_digest=security_digest,
                         options_file=options_file, provider_names=list(placement.values()),
                         timeout_ms=request_timeout_ms, ack_timeout_ms=5000,
                         no_progress_ms=600000 if real_model else 120000,
                         max_candidates=1, max_policy_ms=5000,
                         **({"interest_lifetime_ms": 5000, "max_event_retries": 8}
                            if real_model else {})),
            conversation=dict(journal=dict(identity=conversation_id, state_root=str(journal_root),
                keys=[dict(id="active", file=str(journal))]),
                owner=dict(requester_identity=requester, service_name=service,
                           security_domain_digest=security_digest),
                turn=turn_options),
            turns=turns),
        "artifact_authority": dict(authority=dict(authority_private_key_file=str(auth_private),
            requester_public_key_files={requester: str(requester_public)},
            content_key_file=str(content), content_key_id="spec242-content", protection_epoch=epoch,
            recipient_public_key_files=recipients, allowed_model_manifests=[source["model_manifest_digest"]],
            publication_sources={source["model_manifest_digest"]: dict(
                artifact_profile_digest=catalog["recipe"]["artifact_profile_digest"],
                canonical_source_digest=source["digest"],
                model_content_digest=catalog["model"]["content_digest"],
                model_name=catalog["model"]["model_name"])}), max_grant_ttl_ms=retention_ms)}
    write_json(root / "deployment.json", config)
    print(root / "deployment.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--release-manifest", required=True, type=Path)
    parser.add_argument("--journal-root", required=True, type=Path,
                        help="Durable target filesystem directory for the native journal")
    workload = parser.add_mutually_exclusive_group()
    workload.add_argument("--source", type=Path)
    workload.add_argument("--prepared-workload", type=Path,
                          help="Model inputs produced by prepare-workload.py before root deployment")
    parser.add_argument("--source-sha256")
    parser.add_argument("--tokenizer", type=Path)
    parser.add_argument("--layer-cut", type=int)
    parser.add_argument("--rank-weights", nargs="+", type=int)
    parser.add_argument("--link-bw", type=float, help="Per-link bandwidth Mbps for every topology link")
    parser.add_argument("--link-delay", type=float, help="Per-link one-way delay ms for every topology link")
    parser.add_argument("--no-probe", action="store_false", dest="probe",
                        help="Skip the post-READY datapath/CPU probes")
    parser.add_argument("--link-loss", type=float,
                        help="Per-link loss percentage for every topology link")
    parser.add_argument("--max-new-tokens", type=int,
                        help="Generation length recorded in the workload options")
    parser.add_argument("--no-kv-capture", action="store_false", dest="kv_capture",
                        help="Disable the complete KV/logits capture for timed matrix runs")
    parser.add_argument("--storage-budget", type=int,
                        help="Deployment storage budget in bytes (default 8 GiB real model)")
    parser.add_argument("--provider-cpus", nargs="+", type=int,
                        help="Per-provider CPU quota for placement-interaction cells")
    parser.add_argument("--topology", type=Path,
                        help="Explicit MiniNDN topology; derive network treatments separately")
    parser.add_argument("--route-metric", choices=("face-cost",),
                        help="Route on fixed [faces] costs in the explicit topology")
    parser.add_argument("--dynamic-shaping", action="store_true",
                        help="Shape the Provider data links after material assembly instead of at topology creation")
    args = parser.parse_args()
    source_options = (args.source_sha256, args.tokenizer, args.layer_cut)
    if args.source and any(value is None for value in source_options):
        parser.error("--source requires --source-sha256, --tokenizer and --layer-cut")
    if not args.source and (any(value is not None for value in source_options) or args.rank_weights):
        parser.error("model source options require --source")
    if args.rank_weights is not None and len(args.rank_weights) not in (2, 3):
        parser.error("the MiniNDN qualification layout requires two or three rank weights")
    if args.route_metric is not None and args.topology is None:
        parser.error("--route-metric requires --topology")
    if args.topology is not None and (args.dynamic_shaping or
                                      any(value is not None for value in (
                                          args.link_bw, args.link_delay, args.link_loss))):
        parser.error("derive network cells separately when using --topology")
    prepare_deployment(args.output.resolve(), args.prefix.resolve(strict=True),
                       args.release_manifest.resolve(strict=True), args.journal_root.resolve(),
                       args.source, args.source_sha256, args.tokenizer, args.layer_cut,
                       args.rank_weights, args.prepared_workload,
                       args.link_bw, args.link_delay, args.probe,
                       args.link_loss, args.max_new_tokens, args.kv_capture,
                       args.storage_budget, args.provider_cpus, args.dynamic_shaping,
                       args.topology.resolve(strict=True) if args.topology else None, args.route_metric)
