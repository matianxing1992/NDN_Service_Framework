#!/usr/bin/env python3
"""Provision a local, installed Spec 242 deployment with fresh actor identities."""

import argparse
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def capture(command, env=None):
    return subprocess.run(command, env=env, check=True, timeout=30,
                          stdout=subprocess.PIPE).stdout


def prepare_identities(root, prefix, namespace, group, actors):
    ca = root / "ca"
    ca.mkdir(mode=0o700)
    environments = {}
    certificates = {}

    def environment(home):
        return dict(PATH=os.environ["PATH"], HOME=str(home),
                    NDN_CLIENT_PIB="pib-sqlite3:" + str(home / ".ndn"),
                    NDN_CLIENT_TPM="tpm-file:")

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
        capture([str(prefix / "bin/public-trust-installer"), str(home / ".ndn"),
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


def prepare_deployment(root, prefix, ledger, journal_root, scenario="hybrid"):
    checkout = Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(checkout / "Experiments"))
    from NDNSF_DI_Qwen06B_Native_Minindn import digest_bytes as digest, make_external_key
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat, load_pem_public_key

    root.mkdir(mode=0o700)
    namespace = "/spec242"
    group = namespace + "/group"
    service = namespace + "/Infer"
    grant_service = namespace + "/ArtifactGrant"
    repo_service = "/NDNSF/DistributedRepo/Object/v1"
    epoch = "spec242-epoch"
    bundle = root / "workload"
    with tempfile.TemporaryDirectory(dir=root, prefix="catalog-home-") as home:
        env = os.environ.copy()
        env.update(HOME=home, NDN_CLIENT_PIB="pib-sqlite3:" + str(Path(home) / ".ndn"),
                   NDN_CLIENT_TPM="tpm-file:")
        capture([sys.executable, str(Path(__file__).with_name("prepare-workload.py")),
                 "--output", str(bundle), "--requester", namespace + "/requester",
                 "--protection-epoch", epoch, "--scenario", scenario], env)
    catalog = json.loads((bundle / "catalog.json").read_bytes())
    options = json.loads((bundle / "generation.json").read_bytes())
    split = catalog["splitter"]
    roles = [stage + "/Tensor/Rank/" + str(rank)
             for stage, degree in zip(split["roles"], split["tensor_degrees"])
             for rank in range(degree)]
    actor_nodes = [("controller", "controller", "h242ctl"),
                   ("authority", "artifact-authority", "h242auth"),
                   ("repo", "repo", "h242repo"),
                   ("requester", "requester", "h242user")]
    actor_nodes += [(f"provider-{rank}", "provider", f"h242p{rank}") for rank in range(len(roles))]
    actors = [dict(id=name, kind=kind, identity=namespace + "/" + name, node=node,
                   resources=dict(cpus=2 if kind == "provider" else 1), parameters={})
              for name, kind, node in actor_nodes]
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
    model_uri = "/Model/Spec242/Qwen"
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
            "capture-complete-kv-state": True, "assembly-timeout-ms": 120000,
            "repo-fetch-timeout-ms": 60000, "conversation-retention-ms": 600000}
    providers[-1]["parameters"]["capture-logits-output"] = "logits"
    by_id["repo"]["parameters"]["request-timeout-ms"] = 60000

    source = catalog["source"]
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
    turns = [dict(input_file=str(bundle / input_name), options_file=options_file,
                  output_file=str(root / f"output-{i}.json"),
                  checkpoint_output_file=str(root / f"checkpoint-{i}.json"))
             for i, input_name in enumerate(("prompt.bin", "delta.bin"))]
    placement = {role: actor["identity"] for role, actor in zip(roles, providers)}
    topology = checkout / "tests/fixtures/spec242/minindn-topology.conf"
    config = {
        "schema": "ndnsf-di-deployment-v1", "deployment_id": f"spec242-{scenario}",
        "release": {"prefix": str(prefix), "manifest": str(ledger)},
        "platform": {"kind": "minindn", "topology": str(topology),
                     "topology_sha256": digest(topology.read_bytes())},
        "nodes": [dict(id=node, host=node, cpus=3, nfd=dict(mode="managed", address="127.0.0.1",
                         port=6363, socket=str(root / (node + ".sock"))))
                  for node in ["h242relay", *(actor["node"] for actor in actors)]],
        "actors": actors,
        "network": dict(group=group, service=service, artifact_service=grant_service,
                        repo_service=repo_service, routes=[]),
        "security": dict(controller_certificate=certificates["controller"],
                         controller_policy=str(policy), trust_schema=str(trust)),
        "storage": {**{key + "_root": str(root / key) for key in
                       ("cache", "repo", "scratch", "state", "evidence")}, "max_bytes": 128 << 20},
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
            limits=dict(bootstrap_ms=120000, max_source_bytes=8 << 20,
                        max_assembled_bytes=8 << 20),
            request=dict(service=service, task="text-generation", generation_mode="TOKEN_STREAMING",
                         tokenizer_digest=options["tokenizerDigest"], security_policy_digest=security_digest,
                         options_file=options_file, provider_names=list(placement.values()),
                         timeout_ms=240000, ack_timeout_ms=5000, no_progress_ms=120000,
                         max_candidates=1, max_policy_ms=5000),
            conversation=dict(journal=dict(identity=conversation_id, state_root=str(journal_root),
                keys=[dict(id="active", file=str(journal))]),
                owner=dict(requester_identity=requester, service_name=service,
                           security_domain_digest=security_digest),
                turn=dict(mode="FULL_CONTEXT", conversation_id=conversation_id,
                          generation_id=uuid.uuid4().hex, parent_context_epoch=0,
                          retention_duration_ms=600000)),
            turns=turns),
        "artifact_authority": dict(authority=dict(authority_private_key_file=str(auth_private),
            requester_public_key_files={requester: str(requester_public)},
            content_key_file=str(content), content_key_id="spec242-content", protection_epoch=epoch,
            recipient_public_key_files=recipients, allowed_model_manifests=[source["model_manifest_digest"]],
            publication_sources={source["model_manifest_digest"]: dict(
                artifact_profile_digest=catalog["recipe"]["artifact_profile_digest"],
                canonical_source_digest=source["digest"],
                model_content_digest=catalog["model"]["content_digest"],
                model_name=catalog["model"]["model_name"])}), max_grant_ttl_ms=600000)}
    write_json(root / "deployment.json", config)
    print(root / "deployment.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--release-manifest", required=True, type=Path)
    parser.add_argument("--journal-root", required=True, type=Path,
                        help="Durable target filesystem directory for the native journal")
    parser.add_argument("--scenario", choices=("hybrid", "tp3"), default="hybrid")
    args = parser.parse_args()
    prepare_deployment(args.output.resolve(), args.prefix.resolve(strict=True),
                       args.release_manifest.resolve(strict=True), args.journal_root.resolve(),
                       args.scenario)
