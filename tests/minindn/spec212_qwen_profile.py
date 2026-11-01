"""Real whole-model Qwen profile for the bounded Spec212 process campaign.

Only prepares inputs for the maintained native executables. Inference, grants,
publication, recovery and conversation transitions remain in production C++.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Experiments"))
import NDNSF_DI_Qwen06B_Native_Minindn as qwen
import run_spec192_real_cpp_repo as repo


def write_json(path: Path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def prepare(output: Path, model_dir: Path, *, work_root: Path, service: str, group: str,
            controller: str, authority: str, owner: str, providers: list[str],
            certificates: dict[str, Path], schema: Path, repo_target: dict,
            event: str, candidate_digest: str, withdrawal_scope: str = "provider"):
    import onnx

    conversation_id = "spec212-kv-" + event
    if len(conversation_id) < 16 or "/" in conversation_id or "\\" in conversation_id:
        raise ValueError("Spec212 conversation ID violates the native runtime contract")

    source = model_dir / "model_quantized.onnx"
    tokenizer = model_dir / "tokenizer.json"
    source_digest = qwen.digest_file(source)
    tokenizer_digest = qwen.digest_file(tokenizer)
    if source_digest != "sha256:5f939904362dc83215b0d029280a4b68aa4a97ba82ec9f23aaa8044efecfeb25":
        raise ValueError("Spec212 model does not match the pinned Qwen candidate")
    if tokenizer_digest != "sha256:aeb13307a71acd8fe81861d94ad54ab689df773318809eed3cbe794b4492dae4":
        raise ValueError("Spec212 tokenizer does not match the pinned candidate")
    model_config = json.loads((model_dir / "config.json").read_text())
    layers = int(model_config["num_hidden_layers"])
    graph = onnx.load(str(source), load_external_data=False).graph
    input_names = [item.name for item in graph.input]
    output_names = [item.name for item in graph.output]
    mapping = {name: [] for name in ["embedding", *[
        f"layer-{i:02d}" for i in range(layers)], "final-norm-head"]}
    for index, node in enumerate(graph.node):
        match = re.search(r"/layers\.(\d+)/", node.name)
        key = (f"layer-{int(match.group(1)):02d}" if match else
               "final-norm-head" if node.name.startswith(("/model/norm/", "/lm_head/", "logits_"))
               else "embedding")
        mapping[key].append(index)
    if any(not indices for indices in mapping.values()):
        raise ValueError("Qwen source semantic mapping has an empty group")
    initializer_bytes = sum(len(item.raw_data) for item in graph.initializer)
    del graph
    canonical_digest = qwen.canonical_source_graph_digest(source)
    role = "/LLM/Pipeline/Stage/0"
    stage = {"role": role, "path": str(source), "sha256": source_digest,
             "bytes": source.stat().st_size, "inputNames": input_names,
             "outputNames": output_names,
             "cacheInputs": [f"past_{name.rsplit('.', 1)[1]}.{name.split('.')[1]}"
                             for name in input_names if name.startswith("past_key_values.")],
             "cacheOutputs": [f"present_{name.rsplit('.', 1)[1]}.{name.split('.')[1]}"
                              for name in output_names if name.startswith("present.")],
             "layerRange": [0, layers]}
    if len(stage["cacheInputs"]) != 2 * layers or len(stage["cacheOutputs"]) != 2 * layers:
        raise ValueError("Qwen cache input/output coverage mismatch")
    model_name = "liodon-ai/Qwen3-0.6B-ONNX"
    revision = "ebb496b1ff353ed44e946b5ae61605593da3b6a6"
    manifest = {"model": model_name, "modelRevision": revision,
                "dtype": "float32", "quantization": "int8",
                "eosTokenIds": [int(model_config["eos_token_id"])], "stages": [stage]}
    write_json(output / "model-profile.json", manifest)
    manifest_digest = qwen.digest_file(output / "model-profile.json")
    qwen.SERVICE = service
    plan, service_manifest = qwen.stage_plan_and_manifest(
        output, manifest, [stage], 2, tokenizer_digest, resident_session=True)
    service_value = json.loads(service_manifest.read_text())
    metadata = service_value["services"][0]["artifacts"][0]["metadata"]
    metadata.update({"workTokenInputName": "input_ids", "workTokenAxis": "1"})
    service_value["services"][0]["artifacts"][0].pop("path", None)
    write_json(service_manifest, service_value)
    digest = qwen.digest_text
    adapter = {"name": "qwen", "version": "1", "state_digest": digest("qwen-native-state"),
               "abi": "qwen-native-onnxruntime-cpu-v1", "model_formats": ["onnx"],
               "tasks": ["text-generation"], "backends": ["onnxruntime"], "precisions": ["float32"],
               **{key + "_schema_digest": digest("qwen-" + key + "-schema")
                  for key in ("input", "options", "result", "graph", "split", "state")},
               "graph_inspectable": True, "splittable": True, "deterministic_analysis": True}
    descriptor = {"model_name": model_name, "content_digest": source_digest,
                  "semantics_digest": manifest_digest,
                  "graph_digest": qwen.qwen_graph_digest(model_name, revision, "float32", [[0, layers]]),
                  "model_format": "onnx", "precision": "float32", "adapter": adapter,
                  "source_revision": revision, "quantization_subtype": "dynamic_int8"}
    source_limit = source.stat().st_size + (64 << 20)
    assembled_limit = qwen.assembled_model_budget([stage], source.stat().st_size, 0)
    catalog = {"schema": "ndnsf-di-native-request-catalog-v1", "model": descriptor,
               "source": {"file": str(source), "data_name": owner + "/Model/Qwen/source",
                          "digest": source_digest, "bytes": source.stat().st_size,
                          "initializer_bytes": 0, "model_manifest_digest": manifest_digest,
                          "canonical_graph_digest": canonical_digest},
               "recipe": {"artifact_profile_digest": digest("qwen-profile:dynamic_int8"),
                          "assembler_descriptor_digest": digest("qwen-assembler:dynamic_int8"),
                          "backend_abi": "onnxruntime-cpu-v1", "precision": "float32",
                          "quantization": "dynamic_int8", "layout": "native", "padding": "none",
                          "protection_epoch": "epoch-1", "max_source_bytes": source_limit,
                          "max_assembled_bytes": assembled_limit, "max_nodes": 10000},
               "publication": {"artifact_root": qwen.MODEL_URI + "/artifacts",
                               "package_manifest_digest": manifest_digest,
                               "max_publication_bytes": source_limit + assembled_limit + (64 << 20)},
               "input_format": "OPAQUE", "max_payload_bytes": 4 << 20,
               "conversation_input": {"kind": "TENSOR_BUNDLE_TOKEN_IDS", "tensor_name": "input_ids"},
               "splitter": {"kind": "QWEN", "model_family": "qwen", "layer_ranges": [[0, layers]],
                            "artifact_digests_by_role": {role: source_digest},
                            "weight_bytes_by_role": {role: initializer_bytes}, "roles": [role],
                            "tensor_degrees": [1], "input_ingress_role": role, "result_egress_role": role},
               "node_mapping": mapping,
               "state_inputs": {role: {name: [qwen.canonical_qwen_state_name(name)]
                                       for name in stage["cacheInputs"]}},
               "state_outputs": {role: {name: [qwen.canonical_qwen_state_name(name)]
                                        for name in stage["cacheOutputs"]}}}
    requester = output / "requester"
    authority_dir = output / "authority"
    provider_dirs = [output / f"provider-{i}" for i in range(2)]
    for directory in [requester, authority_dir, *provider_dirs]:
        directory.mkdir(mode=0o700)
    authority_private, authority_public = qwen.make_external_key(authority_dir, "authority")
    requester_private, requester_public = qwen.make_external_key(requester, "requester")
    provider_keys = [(qwen.make_external_key(directory, "recipient"),
                      qwen.make_external_key(directory, "offer")) for directory in provider_dirs]
    key_path = authority_dir / "content.key"
    key_path.write_bytes(os.urandom(32))
    key_path.chmod(0o600)
    registry = {"schemaVersion": 1, "status": "CONFIGURED", "artifactPolicyAuthority": {
        "acceptedModelFamilies": [qwen.MODEL_URI], "authorityId": authority,
        "grantSchema": "ndnsf-di-key-grant-v1", "keyId": "qwen-artifact-policy-ed25519-local",
        "protectionEpochs": ["epoch-1"], "publicKeyAlgorithm": "ed25519",
        "publicKeyPath": "authority/authority-public.pem", "publicKeySha256": qwen.digest_file(authority_public),
        "signatureAlgorithm": "ed25519"}}
    public_keys, offers = {}, []
    for i, provider in enumerate(providers):
        directory = provider_dirs[i]
        recipient, offer = provider_keys[i]
        offer_id = qwen.public_key_digest(offer[1])
        public_keys[offer_id] = str(offer[1])
        cert_name = repo.perf.certificate_name_from_file(certificates[provider])
        offers.append({"provider": provider, "service": service,
                       "keyLocatorPrefix": cert_name.rsplit("/", 2)[0],
                       "signerKeyId": offer_id, "certificateName": cert_name})
        write_json(directory / "recipient-key-map.json", {provider: str(recipient[0])})
        (directory / "authority-public.pem").write_bytes(authority_public.read_bytes())
        write_json(directory / "trust-root-registry-v1.json", registry)
        scratch = work_root / f"provider-{i}-scratch"
        scratch.mkdir(mode=0o700)
        write_json(directory / "recovery.json", {
            "privateScratchRoot": str(scratch), "repoForwardingHints": [repo_target["provider_identity"]],
            "maxPayloadBytes": 16 << 20, "maxEncodedBytes": 20 << 20,
            "maxMetadataBytes": 1 << 20, "maxCheckpointBytes": 24 << 20,
            "maxProtectedPlaintextBytes": 25 << 20, "maxProtectedWireBytes": 32 << 20,
            "maxTransportEnvelopeBytes": 40 << 20})
        write_json(directory / "handoff.json", {
            "schema": "ndnsf-di-native-kv-handoff-v1", "withdrawal_scope": withdrawal_scope,
            "policy_id": "spec212-two-event", "failure_policy": "abort", "max_conversations": 1,
            "save_timeout_ms": 600000, "drain_timeout_ms": 605000, "max_concurrent_events": 1,
            "repo_ack_collection_ms": 3000,
            "max_packets_per_role": 4096, "max_packet_wire_bytes_per_role": 40 << 20,
            "max_withdrawal_bytes": 40 << 20, "repo": repo_target,
            "owner_certificates": [str(certificates[owner])],
            "owner_issuer_certificate": str(output / "identity/root.cert")})
    authority_config = {"schema": "ndnsf-di-native-authority-v1", "run_for_ms": 12300000,
                        "permission_bootstrap_ms": 60000, "max_grant_ttl_ms": 3600000,
                        "authority": {"identity": authority, "service": "/HELLO", "group": group,
                                      "controller_identity": controller, "requester_identity": owner,
                                      "protection_epoch": "epoch-1", "content_key_id": "spec212-content",
                                      "trust_schema_file": str(schema),
                                      "authority_private_key_file": str(authority_private),
                                      "requester_public_key_file": str(requester_public),
                                      "content_key_file": str(key_path),
                                      "allowed_model_manifests": [manifest_digest],
                                      "recipient_public_key_files": {
                                          provider: str(provider_keys[i][0][1]) for i, provider in enumerate(providers)},
                                      "publication_sources": {manifest_digest: {
                                          "model_name": model_name, "model_content_digest": source_digest,
                                          "canonical_source_digest": source_digest,
                                          "artifact_profile_digest": catalog["recipe"]["artifact_profile_digest"]}}}}
    write_json(authority_dir / "config.json", authority_config)
    first_ids, delta_ids = [151644, 872, 198, 9707, 151645, 198, 151644, 77091, 198], [198, 791, 1667]
    (requester / "input.bin").write_bytes(qwen.tensor_bundle(first_ids))
    (requester / "delta.bin").write_bytes(qwen.tensor_bundle(delta_ids))
    journal_key = requester / "conversation.key"
    journal_key.write_bytes(os.urandom(32))
    journal_key.chmod(0o600)
    options = {"useCache": True, "outputMode": "TOKEN_STREAMING",
               "generationId": "0123456789abcdef0123456789abcdef", "maxNewTokens": 2,
               "tokenizerDigest": tokenizer_digest, "eosTokenIds": manifest["eosTokenIds"],
               "sampling": {"mode": "Greedy", "temperature": 0.0, "topK": 1, "topP": 1.0,
                            "repetitionPenalty": 1.0, "seed": 212}, "stopStrings": [],
               "tokenInputName": "input_ids",
               "stateInputNames": [qwen.canonical_qwen_state_name(name) for name in stage["cacheInputs"]],
               "stateOutputNames": [qwen.canonical_qwen_state_name(name) for name in stage["cacheOutputs"]],
               "stateSuccessorMap": ",".join(f"{a}={b}" for a, b in qwen.canonical_qwen_state_successor_pairs([stage])),
               "positionInputPolicy": "qwen-causal-position-v1", "attentionMaskInputName": "attention_mask",
               "positionIdsInputName": "position_ids"}
    write_json(requester / "options.json", options)
    write_json(requester / "options-1.json", qwen.continuation_options(options, 1))
    import time
    config = {"schema": "ndnsf-di-native-requester-v1", "catalog": catalog,
              "test_only": True,
              "test_handoff_barrier": {"ready_file": "handoff.ready", "release_file": "handoff.release",
                                       "timeout_ms": (650000 if event == "departure" and
                                                      withdrawal_scope == "provider" else 60000)},
              "core": {"requester_identity": owner, "authority_identity": controller,
                       "group": group, "trust_schema_file": str(schema)},
              "grant": {"authority_identity": authority, "authority_service": "/HELLO",
                        "authority_public_key_file": str(authority_public),
                        "requester_private_key_file": str(requester_private), "protection_epoch": "epoch-1"},
              "offer_admission": {"policy": {"schema": "spec180-provider-offer-trust-v1",
                         "candidateId": candidate_digest[7:],
                                             "candidateDigest": candidate_digest,
                                             "trustSchema": "/example/repo/trust", "entries": offers},
                                  "public_key_files": public_keys, "candidate_digest": candidate_digest},
              "repository": {"path": str(work_root / "source-repository"), "max_bytes": 4 << 30},
              "encrypted_repository": {"path": str(work_root / "encrypted-repository"), "max_bytes": 4 << 30},
              "limits": {"bootstrap_ms": 100000, "preparation_timeout_ms": 300000,
                         "large_data_ims_limit": qwen.large_data_ims_limit(source.stat().st_size, 0),
                         "max_source_bytes": source_limit, "max_assembled_bytes": assembled_limit,
                         "max_prepared_bytes": qwen.preparation_cache_budget(source_limit, assembled_limit),
                         "max_prepared_entries": 2},
              "request": {"service": service, "task": "text-generation",
                          "adapter_composition_digest": qwen.digest_bytes(qwen.canonical_bytes(adapter)),
                          "task_descriptor_digest": digest("qwen-task"), "generation_mode": "TOKEN_STREAMING",
                          "tokenizer_digest": tokenizer_digest, "input_layout_digest": digest("qwen-input-layout"),
                          "security_policy_digest": digest("qwen-security"), "interest_lifetime_ms": 5000,
                          "max_event_retries": 8, "max_candidates": 1, "max_policy_ms": 60000,
                          "provider_names": [providers[0]], "max_reentries": 1, "no_progress_ms": 300000,
                          "timeout_ms": 1200000, "ack_timeout_ms": 120000, "options_file": "options.json",
                          "placement_strategy": "native-pre-split-first"},
              "conversation": {"schema": "ndnsf-di-native-conversation-v1",
                               "journal": {"state_root": "conversation-state", "identity": "spec212-user",
                                           "keys": [{"id": "active", "file": "conversation.key"}], "quota_bytes": 64 << 20},
                               "owner": {"requester_identity": owner, "service_name": service,
                                         "security_domain_digest": digest("qwen-security")},
                               "turn": {"conversation_id": conversation_id,
                                        "parent_context_epoch": 0, "service_name": service,
                                         "retention_deadline_ms": int(time.time() * 1000) + 3600000,
                                        "mode": "FULL_CONTEXT", "generation_id": options["generationId"],
                                        "canonical_token_ids": first_ids}},
              "turns": [{"input_file": "input.bin", "options_file": "options.json", "output_file": "output-0.json",
                         "checkpoint_output_file": "checkpoint-0.json"},
                        {"input_file": "delta.bin", "options_file": "options-1.json", "output_file": "output-1.json",
                         "checkpoint_output_file": "checkpoint-1.json"}],
              # This is the protocol handoff budget. Cold destination model
              # preparation is a separate pre-event conversation in the
              # MiniNDN runner and must not consume or extend this deadline.
              "kv_handoff": {"after_turn": 0, "timeout_ms": 600000,
                             "wait_for_departure": event == "departure",
                             "roles": {role: {"producer_identity": providers[0],
                                              "producer_certificate": str(certificates[providers[0]]),
                                              "repo_node": repo_target["repo_node"],
                                              "repo_provider_identity": repo_target["provider_identity"],
                                              "repo_service_prefix": repo_target["service_prefix"],
                                              **({"destination_provider": providers[1]} if event == "switch" else {})}}}}
    write_json(requester / "config.json", config)
    return {"plan": plan, "manifest": service_manifest, "provider_dirs": provider_dirs,
            "requester": requester, "authority": authority_dir, "role": role,
            "first_ids": first_ids, "delta_ids": delta_ids,
            "source_digest": source_digest, "tokenizer_digest": tokenizer_digest}
