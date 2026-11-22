#!/usr/bin/env python3
"""Prepare fresh CPU experiment inputs using a selected installed native prefix.

This creates credentials and configuration, then runs read-only cluster check.
It never starts actors, copies an earlier deployment, or grants runtime permission.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import signal
import stat
import sys
import tempfile

from model_assets import contained_file, file_digest, read_json, validate_manifest
from native_probe_validation_links import render_anchored_probe_validation_schema
from TigerCluster.runtime.processes import run_finite_application

REPO_OPERATIONS = ("STORE", "FETCH", "STATUS", "MANIFEST", "STORE_RANGE", "COMMIT_RANGES",
                   "ABORT_RANGES", "INSERT", "FETCH_RANGE", "USER_STORAGE_GRANT",
                   "USER_STORAGE_USAGE", "USER_STORAGE_LIST", "USER_STORAGE_DELETE")
NATIVE_PROGRAMS = ("App_ServiceController", "DI_NativeArtifactAuthority", "DI_NativeRequester",
                   "di-native-provider", "DistributedRepoServer", "ndnsf-identity-check",
                   "ndnsf-di-tokenize-input", "ndnsf-di")
TP_PROFILE = "qwen06b-tp-cpu"
PIPELINE_PROFILES = ("qwen06b-pp-cpu-colocated", "qwen06b-pp-cpu-distributed",
                     "qwen06b-single-cpu")
PIPELINE_PROFILE_FILE = "profiles/qwen06b-pp-cpu.json"
WIRED_PROFILE = "profiles/qwen06b-wired-star-cpu.json"
WIRED_SINGLE = "qwen06b-wired-star-full-cpu"
WIRED_TP = "qwen06b-wired-star-tp-cpu"
WIRED_VARIANTS = (WIRED_TP, "qwen06b-wired-star-pp-cpu", WIRED_SINGLE)
WIRED_FULL_CATALOG = "fixtures/native-deployment/qwen06b-full-catalog.json"
PREPARATION_RESOURCES = (
    "prepare-native-deployment.py", "model_assets.py", "native_probe_validation_links.py",
    "TigerCluster/runtime/processes.py", "profiles/qwen06b-tp-cpu.json",
    PIPELINE_PROFILE_FILE,
    "recipes/qwen3-0.6b-int4.json", "fixtures/native-deployment/qwen06b-catalog.json",
    "fixtures/native-deployment/qwen06b-generation-options.json",
    "fixtures/native-deployment/two-provider.conf",
    "fixtures/native-deployment/three-node-pipeline.conf",
)


def digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def write_json(path, value):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with path.open("x") as stream:
        json.dump(value, stream, sort_keys=True, separators=(',', ':'))
        stream.write("\n")


def installed_inputs(prefix, profile_resources=()):
    """Early delivery check against the installer's existing receipt, before key creation."""
    receipt = prefix / "share/ndnsf/stack-install-manifest.json"
    ledger = read_json(receipt, max_bytes=64 * 1024 * 1024)
    if ledger.get("schema") != 1 or not isinstance(ledger.get("files"), dict):
        raise ValueError("DEPLOYMENT_INSTALL_RECEIPT_INVALID")
    paths = [prefix / "bin" / name for name in NATIVE_PROGRAMS]
    paths += [prefix / "libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker"]
    paths += [prefix / "lib" / name for name in (
        "libndn-service-framework.so", "libndnsf-distributed-inference.so")]
    paths += [prefix / "share/ndnsf/Experiments" / name for name in (*PREPARATION_RESOURCES, *profile_resources)]
    if (prefix / 'libexec/ndnsf-python') in (prefix / 'bin/ndnsf-di').resolve().parents:
        paths.append(prefix / 'libexec/ndnsf-python/pyvenv.cfg')
    for path in paths:
        row = ledger["files"].get(str(path), {}).get("installed", {})
        if path.is_symlink():
            if row.get("kind") != "symlink" or os.readlink(str(path)) != row.get("target"):
                raise ValueError("DEPLOYMENT_INSTALLED_LINK_MISMATCH: " + str(path))
            path = path.resolve(strict=True)
            path.relative_to(prefix)
            row = ledger["files"].get(str(path), {}).get("installed", {})
        if (row.get("kind") != "file" or not path.is_file() or
                stat.S_IMODE(path.stat().st_mode) != row.get("mode") or
                file_digest(path) != row.get("sha256")):
            raise ValueError("DEPLOYMENT_INSTALLED_FILE_MISMATCH: " + str(path))
        if path.parent.name in ("bin", "ndnsf-di") and not os.access(str(path), os.X_OK):
            raise ValueError("DEPLOYMENT_INSTALLED_EXECUTABLE_REQUIRED: " + str(path))
    return receipt


def operator_interpreter(prefix):
    with (prefix / 'bin/ndnsf-di').open('rb') as console:
        line = console.readline(4096).decode()
    if not line.startswith('#!'):
        raise ValueError('DEPLOYMENT_OPS_CONSOLE_SHEBANG_MISSING')
    command = shlex.split(line[2:])
    if (len(command) != 1 or not Path(command[0]).is_absolute() or
            not Path(command[0]).name.startswith('python') or not os.access(command[0], os.X_OK)):
        raise ValueError('DEPLOYMENT_OPS_CONSOLE_REQUIRES_ABSOLUTE_PYTHON_INTERPRETER')
    # Preserve the venv executable path rather than resolving it to base Python.
    return Path(command[0])


def platform_facilities(interpreter, env):
    code = '''
import hashlib,json,sys
from pathlib import Path
import mininet,minindn
from mininet.node import Host
from minindn.minindn import Minindn
from importlib import metadata
rows=[]
for name,module in [('mininet',mininet),('minindn',minindn)]:
    root=Path(module.__file__).resolve().parent
    files=sorted(root.rglob('*.py'))
    size=sum(path.stat().st_size for path in files)
    if len(files)>512 or size>8*1024*1024:
        raise RuntimeError('PLATFORM_SOURCE_BOUND_EXCEEDED:'+name)
    identity=[(str(path.relative_to(root)),hashlib.sha256(path.read_bytes()).hexdigest()) for path in files]
    try:version=metadata.version(name)
    except metadata.PackageNotFoundError:version=None
    rows.append(dict(module=name,directory=str(root),distributionVersion=version,fileCount=len(files),
        sourceBytes=size,treeSha256=hashlib.sha256(json.dumps(identity,separators=(',',':')).encode()).hexdigest()))
print(json.dumps(dict(python=sys.version.split()[0],modules=rows,rootRequiredForLaunch=True)))
'''
    with tempfile.TemporaryDirectory(prefix='ndnsf-platform-check-') as directory:
        root = Path(directory)
        return json.loads(run([interpreter, '-I', '-B', '-c', code],
                              dict(env, HOME=str(root)), root))


def run(argv, env, cwd, *, binary=False):
    cleanup = []
    with tempfile.TemporaryFile(dir=str(cwd)) as output, tempfile.TemporaryFile(dir=str(cwd)) as errors:
        try:
            run_finite_application(Path(argv[0]).name, [str(item) for item in argv], errors,
                cleanup, seconds=60, env=env, stdout=output, cwd=str(cwd))
        except Exception as error:
            errors.seek(max(0, errors.seek(0, os.SEEK_END) - 4000))
            raise ValueError("DEPLOYMENT_INPUT_COMMAND_FAILED {}: {}: {}".format(
                Path(argv[0]).name, error, errors.read().decode(errors="replace"))) from error
        if any(row['forced'] or not row['reaped'] or row['error'] for row in cleanup):
            raise ValueError("DEPLOYMENT_INPUT_CHILD_NOT_CLEANLY_DRAINED")
        output.seek(0)
        value = output.read(4 * 1024 * 1024 + 1)
        if len(value) > 4 * 1024 * 1024:
            raise ValueError("DEPLOYMENT_INPUT_OUTPUT_BOUND_EXCEEDED")
        return value if binary else value.decode()


def ed25519_key(openssl, stem, env, root):
    private = stem.with_name(stem.name + "-private.pem")
    public = stem.with_name(stem.name + "-public.pem")
    run([openssl, "genpkey", "-algorithm", "ED25519", "-out", private], env, root)
    run([openssl, "pkey", "-in", private, "-pubout", "-out", public], env, root)
    der = run([openssl, "pkey", "-in", private, "-pubout", "-outform", "DER"],
              env, root, binary=True)
    # RFC 8410 Ed25519 SubjectPublicKeyInfo, with its exact 32-byte public key.
    if len(der) != 44 or der[:12] != bytes.fromhex("302a300506032b6570032100"):
        raise ValueError("DEPLOYMENT_ED25519_PUBLIC_KEY_ENCODING_INVALID")
    return private, public, digest(der[12:])


def policy_text(identities, network, provider_roles, tp_base_role):
    repo_services = [network["repo_service"] + "/" + op for op in REPO_OPERATIONS]
    def rule(kind, identity, services):
        return "{}-policy {{ for {} allow {{ {} }} }}".format(kind, identity, "\n".join(services))
    providers = [rule("provider", identities["repo"], repo_services),
                 rule("provider", identities["authority"], [network["artifact_service"]])]
    for actor, roles in provider_roles.items():
        scoped_roles = ([tp_base_role] if tp_base_role else []) + roles
        providers.append(rule("provider", identities[actor], [network["service"]] +
            [network["service"] + "/ROLE" + role for role in scoped_roles] +
            ["/NDNSF/Network/Probe/v1"]))
    users = [rule("user", identities["requester"], repo_services + [
        network["service"], network["artifact_service"], "/NDNSF/Network/Probe/v1"])]
    users += [rule("user", identities[actor], repo_services) for actor in provider_roles]
    return ("name " + identities["controller"] + "/NDNSF/ControllerPolicy/v1\n" +
            "provider-policies {\n" + "\n".join(providers) + "\n}\n" +
            "user-policies {\n" + "\n".join(users) + "\n}\n")


def load_profile(resources, name):
    profile = read_json(resources / "profiles/qwen06b-tp-cpu.json")
    if name == TP_PROFILE:
        return profile, None
    variants = read_json(contained_file(resources, PIPELINE_PROFILE_FILE))
    if (variants["schema"] != "ndnsf-qwen-pipeline-experiment-profiles-v1" or
            variants["baseProfile"] != profile["id"]):
        raise ValueError("DEPLOYMENT_PIPELINE_PROFILE_BASE_MISMATCH")
    layout = variants["profiles"][name]
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", layout.get("planningGraphDigest", "")):
        raise ValueError("DEPLOYMENT_PIPELINE_GRAPH_DIGEST_INVALID")
    turns = variants["turnCount"]
    profile["id"] = name
    profile["budgets"]["storageBytes"] = variants["storageBytes"]
    profile["turns"] = profile["turns"][:turns]
    for field in ("inputTokens", "maxNewTokens", "expectedCommittedPrefix"):
        profile["rendering"][field] = profile["rendering"][field][:turns]
    return profile, layout


def apply_pipeline_layout(catalog, layout, graph_row, weight_row, generation_options):
    split = catalog["splitter"]
    base_role = split["roles"][0]
    ranges = layout["layerRanges"]
    # The native Qwen planning graph includes the layer ranges in its identity.
    # Keep the reviewed digest with those ranges; the native catalog verifies it.
    catalog["model"]["graph_digest"] = layout["planningGraphDigest"]
    roles = [base_role.rsplit("/", 1)[0] + "/" + str(index) for index in range(len(ranges))]
    resource_ceiling = split["resources_by_role"][base_role]
    # These identities name a source-bound role, not the assembled ONNX bytes.
    # The native preparer admits the graph and actual initializer digests.
    artifacts = {role: digest(json.dumps([
        graph_row["sha256"], weight_row["sha256"], catalog["recipe"]["artifact_profile_digest"],
        layer_range], separators=(",", ":")).encode())
        for role, layer_range in zip(roles, ranges)}
    split.update(roles=roles, layer_ranges=ranges, tensor_degrees=[1] * len(roles),
                 artifact_digests_by_role=artifacts,
                 weight_bytes_by_role=dict.fromkeys(roles, weight_row["bytes"]),
                 resources_by_role=dict.fromkeys(roles, resource_ceiling),
                 input_ingress_role=roles[0], result_egress_role=roles[-1])
    del split["rank_artifact_digests_by_role"]
    # The TP fixture's stage map describes the rank boundary. The generation
    # options carry the frozen source tensor names for all 28 layers.
    for field, source, semantic in (("state_inputs", "stateInputNames", "past"),
                                    ("state_outputs", "stateOutputNames", "present")):
        names = set(generation_options[source])
        prefix = "past_key_values" if semantic == "past" else "present"
        role_map = {}
        for role, (begin, end) in zip(roles, ranges):
            bindings = {}
            for layer in range(begin, end):
                for kind in ("key", "value"):
                    name = "{}.{}.{}".format(prefix, layer, kind)
                    if name not in names:
                        raise ValueError("DEPLOYMENT_PIPELINE_STATE_TENSOR_MISSING: " + name)
                    bindings["{}_{}.{}".format(semantic, kind, layer)] = [name]
            role_map[role] = bindings
        catalog[field] = role_map
    return roles


def prepare_provider_signer(prefix, ndnsec, identity, rsa_metadata, home, inputs, env, root):
    key_id = "ecdsa-signing"
    signing_key = identity + "/KEY/" + key_id
    request = home / "ecdsa-request.cert"
    request.write_bytes(run(
        [ndnsec, "key-gen", "-n", "-t", "e", "--keyid", key_id, identity],
        env, root, binary=True))
    set_default = [ndnsec, "set-default", "-k"]
    run([*set_default, rsa_metadata["keyName"]], env, root)
    certificate = inputs / (home.name + "-ecdsa.cert")
    not_before = datetime.now(timezone.utc).strftime("%Y%m%d%H%M%S")
    certificate.write_bytes(run(
        [ndnsec, "cert-gen", "-s", identity, "-i", "rsa-issuer", "-S", not_before, "-r", request],
        env, root, binary=True))
    run([ndnsec, "cert-install", "-K", "-f", certificate], env, root)
    check = [prefix / "bin/ndnsf-identity-check", "--credential-metadata", home, identity]
    try:
        run([*set_default, signing_key], env, root)
        metadata = json.loads(run(check, env, root))
    finally:
        run([*set_default, rsa_metadata["keyName"]], env, root)
    if (metadata["identity"] != identity or
            metadata["keyName"] != signing_key or
            json.loads(run(check, env, root)) != rsa_metadata):
        raise ValueError("DEPLOYMENT_PROVIDER_SIGNING_CERTIFICATE_MISMATCH")
    return metadata


def prepare(profile, layout, recipe, assets, asset_manifest, prefix, root, namespace,
            install_receipt, ndnsec, openssl, env, resources, interpreter, facilities):
    inputs = root / "inputs"
    inputs.mkdir(mode=0o700)
    credentials = root / "credentials"
    credentials.mkdir(mode=0o700)
    operator_home = root / "operator-home"
    operator_home.mkdir(mode=0o700)
    # Native statistics sinks open their file directly; the input owner creates
    # this private parent before any actor is launched.
    (root / "statistics").mkdir(mode=0o700)
    env = dict(env, HOME=str(operator_home))
    catalog = read_json(contained_file(resources, profile["catalog"]))
    generation_options = read_json(contained_file(resources, profile["generationOptions"]))
    graph_row, weight_row = recipe["files"]["canonicalGraph"], recipe["files"]["canonicalWeights"]
    splitter = catalog["splitter"]
    base_role = splitter["roles"][0]
    wired = profile["id"] in WIRED_VARIANTS
    tensor_parallel = layout is None and (not wired or profile["id"] == WIRED_TP)
    if wired:
        actor_nodes = profile["actorNodes"]
        node_names = profile["nodes"]
        provider_count = profile["providers"]
        if tensor_parallel and provider_count == 2 and splitter["tensor_degrees"] == [2] \
                and len(splitter["roles"]) == 1:
            roles = [base_role + "/Tensor/Rank/" + str(rank) for rank in range(provider_count)]
        elif not tensor_parallel and splitter["tensor_degrees"] == [1] * provider_count \
                and len(splitter["roles"]) == provider_count:
            roles = splitter["roles"]
        else:
            raise ValueError("DEPLOYMENT_PROFILE_SPLITTER_UNSUPPORTED")
        stage_providers = range(provider_count)
        provider_nodes = [actor_nodes["provider-" + str(index)] for index in stage_providers]
        topology_resource = profile["topology"]
    elif layout is None:
        roles = [base_role + "/Tensor/Rank/" + str(rank) for rank in range(2)]
        provider_nodes = ["control", "worker"]
        stage_providers = [0, 1]
        topology_resource = "fixtures/native-deployment/two-provider.conf"
    else:
        roles = apply_pipeline_layout(catalog, layout, graph_row, weight_row, generation_options)
        provider_nodes = layout["providerNodes"]
        stage_providers = layout["stageProviders"]
        topology_resource = layout["topology"]
        if (len(roles) != len(stage_providers) or
                set(stage_providers) != set(range(len(provider_nodes)))):
            raise ValueError("DEPLOYMENT_PIPELINE_ROLE_MAP_INVALID")
    provider_roles = {"provider-" + str(index): [] for index in range(len(provider_nodes))}
    for role, index in zip(roles, stage_providers):
        provider_roles["provider-" + str(index)].append(role)
    identities = {name: namespace + "/" + name
                  for name in ("controller", "authority", "requester", "repo", *provider_roles)}
    if not wired:
        actor_nodes = dict.fromkeys(identities, "control")
        actor_nodes.update(zip(provider_roles, provider_nodes))
        node_names = dict.fromkeys(["control", *provider_nodes])
    if set(actor_nodes) != set(identities) or not set(actor_nodes.values()) <= set(node_names):
        raise ValueError("DEPLOYMENT_PROFILE_ACTOR_NODES_INVALID")
    network = dict(group=namespace + "/group", service=namespace + "/inference",
                   artifact_service=namespace + "/artifact-grants", repo_service=namespace + "/repo-service")
    certificates, metadata, homes, offer_signers = {}, {}, {}, {}
    for actor, identity in identities.items():
        home = credentials / actor
        home.mkdir(mode=0o700)
        actor_env = dict(env, HOME=str(home))
        certificates[actor] = inputs / (actor + ".cert")
        certificates[actor].write_bytes(run(
            [ndnsec, "key-gen", "-t", "r", identity], actor_env, root, binary=True))
        metadata[actor] = json.loads(run([
            prefix / "bin/ndnsf-identity-check", "--credential-metadata", home, identity],
            actor_env, root))
        if metadata[actor]["identity"] != identity:
            raise ValueError("DEPLOYMENT_CREDENTIAL_IDENTITY_MISMATCH")
        homes[actor] = home
        if actor in provider_roles:
            offer_signers[actor] = metadata[actor]
            if profile.get("providerSigning") == "ecdsa":
                offer_signers[actor] = prepare_provider_signer(
                    prefix, ndnsec, identity, metadata[actor], home, inputs, actor_env, root)
    authority_key = ed25519_key(openssl, homes["authority"] / "grant", env, root)
    requester_key = ed25519_key(openssl, homes["requester"] / "requester", env, root)
    content_key = homes["authority"] / "content-key.bin"
    content_key.write_bytes(secrets.token_bytes(32))
    journal_key = homes["requester"] / "conversation.key"
    journal_key.write_bytes(secrets.token_bytes(32))
    content_key_id = "content-" + secrets.token_hex(16)
    recipients, offers, parameters = {}, {}, {}
    for actor, actor_roles in provider_roles.items():
        recipient = ed25519_key(openssl, homes[actor] / "recipient", env, root)
        offer = ed25519_key(openssl, homes[actor] / "offer", env, root)
        recipients[identities[actor]] = str(recipient[1])
        offers[actor] = offer
        public = homes[actor] / "public"
        public.mkdir(mode=0o700)
        hint = public / "authority-public.pem"
        shutil.copyfile(authority_key[1], hint)
        write_json(public / "trust-root-registry-v1.json", {
            "schemaVersion": 1, "status": "CONFIGURED", "artifactPolicyAuthority": {
                "publicKeyAlgorithm": "ed25519", "signatureAlgorithm": "ed25519",
                # This profile's nested /Model/... name is the protected-grant
                # identity; the adapter family "qwen" is a different field.
                "grantSchema": "ndnsf-di-key-grant-v1", "acceptedModelFamilies": [profile["model"]["name"]],
                "protectionEpochs": ["epoch-1"], "publicKeyPath": "public/authority-public.pem",
                "publicKeySha256": "sha256:" + file_digest(hint),
                "authorityId": identities["authority"], "keyId": content_key_id}})
        key_map = homes[actor] / "recipient-map.json"
        write_json(key_map, {identities[actor]: str(recipient[0])})
        parameters[actor] = {
            "grant-authority-public-key": str(hint), "recipient-key-map": str(key_map),
            "selection-offer-key-file": str(offer[0]), "offer-backend": profile["backend"],
            "offer-can-provision": True, "allow-selection-generation-options": True,
            "permission-wait-ms": profile["budgets"]["permissionWaitMs"],
            "assembly-timeout-ms": profile["budgets"]["requestTimeoutMs"],
            "repo-fetch-timeout-ms": profile["budgets"]["requestTimeoutMs"],
            "conversation-retention-ms": profile["budgets"]["conversationRetentionMs"],
            "statistics-file": str(root / "statistics" / (actor + ".jsonl")),
            "roles": ",".join(actor_roles)}
        if wired:
            parameters[actor]["capture-complete-kv-state"] = False
    trust = inputs / "trust-schema.conf"
    trust.write_text(render_anchored_probe_validation_schema(
        namespace, certificates.values(), sync_prefix=network["group"]))
    topology = inputs / "topology.conf"
    shutil.copyfile(contained_file(resources, topology_resource), topology)
    policy = inputs / "controller-policy.info"
    policy.write_text(policy_text(identities, network, provider_roles,
                                  base_role if tensor_parallel else None))
    model = profile["model"]
    service = dict(model=model["name"], modelFamily=model["family"], modelFormat="onnx",
                   modelRepository=model["repository"], modelRevision=model["revision"],
                   dtype=model["precision"], quantization=model["quantization"],
                   runtimeBackend="onnxruntime",
                   plannerKind="native-qwen-tensor-parallel" if tensor_parallel else "native-qwen-layer",
                   roles=roles, dependencies=[])
    plan_path, service_manifest = inputs / "plan.json", inputs / "service-manifest.json"
    write_json(plan_path, {"version": 2, "services": [dict(service, service=network["service"],
        executionPolicy="DATA_DRIVEN_V2", schemaVersion=3)]})
    write_json(service_manifest, {"version": 2, "services": [dict(service, name=network["service"], artifacts=[])]})
    package_digest = "sha256:" + file_digest(asset_manifest)
    catalog["source"] = {
        "file": str(assets["canonicalGraph"]), "bytes": graph_row["bytes"],
        "digest": "sha256:" + graph_row["sha256"],
        "canonical_graph_digest": profile["canonicalGraphIdentity"],
        "initializer_file": str(assets["canonicalWeights"]), "initializer_bytes": weight_row["bytes"],
        "initializer_digest": "sha256:" + weight_row["sha256"], "model_manifest_digest": package_digest,
        "data_name": identities["requester"] + "/source/" + graph_row["sha256"]}
    catalog["publication"] = {
        "artifact_root": identities["requester"] + "/model-artifacts/" + graph_row["sha256"],
        "package_manifest_digest": package_digest,
        "max_publication_bytes": catalog["recipe"]["max_source_bytes"] +
            catalog["recipe"]["max_assembled_bytes"] + profile["budgets"]["publicationMarginBytes"]}
    rendering = profile["rendering"]
    turns, frozen = [], []
    for index, text in enumerate(profile["turns"]):
        rendered = (rendering["priorTurnSuffix"] if index else "") + rendering["userPrefix"] + text + rendering["assistantSuffix"]
        text_path = inputs / ("turn-" + str(index) + "-prompt.txt")
        text_path.write_text(rendered)
        encoded = json.loads(run([prefix / "bin/ndnsf-di-tokenize-input", "--tokenizer", assets["tokenizer"],
            "--tokenizer-digest", "sha256:" + recipe["files"]["tokenizer"]["sha256"],
            "--text-file", text_path, "--no-special-tokens"], env, root))
        if (len(encoded["tokenIds"]) != rendering["inputTokens"][index] or
                encoded["decodedText"] != rendered or encoded["addSpecialTokens"] is not False):
            raise ValueError("DEPLOYMENT_PROMPT_TOKENIZATION_MISMATCH: turn " + str(index))
        input_path = inputs / ("turn-" + str(index) + "-input.bin")
        input_path.write_bytes(bytes.fromhex(encoded["tensorBundleHex"]))
        options = generation_options.copy()
        options.update(generationId=secrets.token_hex(16), tokenizerDigest=encoded["tokenizerDigest"],
                       maxNewTokens=rendering["maxNewTokens"][index])
        options_path = inputs / ("turn-" + str(index) + "-options.json")
        write_json(options_path, options)
        turns.append(dict(input_file=str(input_path), options_file=str(options_path),
                          output_file=str(root / "evidence" / ("turn-" + str(index) + "-output.json")),
                          checkpoint_output_file=str(root / "evidence" / ("turn-" + str(index) + "-checkpoint.json"))))
        frozen.append(dict(text=text, rendered=rendered, token_ids=encoded["tokenIds"],
                           input_sha256=file_digest(input_path), options_sha256=file_digest(options_path)))
    candidate_inputs = profile if layout is None else dict(profile=profile, layout=layout)
    candidate_digest = digest(json.dumps(candidate_inputs, sort_keys=True, separators=(",", ":")).encode())
    security_digest = digest((namespace + "/security-domain/v1").encode())
    request = dict(service=network["service"], task="text-generation", generation_mode="TOKEN_STREAMING",
        options_file=turns[0]["options_file"], provider_names=[identities[actor] for actor in provider_roles],
        max_candidates=1, max_reentries=1, max_policy_ms=60000, max_event_retries=8,
        ack_timeout_ms=1000, interest_lifetime_ms=5000,
        timeout_ms=profile["budgets"]["requestTimeoutMs"], no_progress_ms=profile["budgets"]["noProgressMs"],
        task_descriptor_digest=profile["requestIdentities"]["taskDescriptor"],
        adapter_composition_digest=profile["requestIdentities"]["adapterComposition"],
        input_layout_digest=profile["requestIdentities"]["inputLayout"], security_policy_digest=security_digest,
        tokenizer_digest="sha256:" + recipe["files"]["tokenizer"]["sha256"])
    actors = []
    for actor, identity in identities.items():
        kind = "provider" if actor in provider_roles else "artifact-authority" if actor == "authority" else actor
        actors.append(dict(id=actor, kind=kind, identity=identity, node=actor_nodes[actor],
            credential_home=str(homes[actor]), external=False,
            resources=dict(cpus=len(provider_roles.get(actor, ())) or 1, gpu_devices=[], allow_sharing=True),
            parameters=parameters.get(actor, {})))
    authority = dict(authority_private_key_file=str(authority_key[0]),
        requester_public_key_files={identities["requester"]: str(requester_key[1])},
        content_key_file=str(content_key), content_key_id=content_key_id, protection_epoch="epoch-1",
        recipient_public_key_files=recipients, allowed_model_manifests=[package_digest],
        publication_sources={package_digest: dict(model_name=catalog["model"]["model_name"],
            model_content_digest=catalog["model"]["content_digest"], canonical_source_digest="sha256:" + graph_row["sha256"],
            initializer_object_digest="sha256:" + weight_row["sha256"], artifact_profile_digest=catalog["recipe"]["artifact_profile_digest"])})
    admission_entries = [dict(provider=identities[actor], service=network["service"], signerKeyId=offers[actor][2],
        keyLocatorPrefix=offer_signers[actor]["keyName"],
        certificateName=offer_signers[actor]["certificateName"])
        for actor in provider_roles]
    requester = dict(schema="ndnsf-di-native-requester-v1", catalog=catalog, request=request, turns=turns,
        model_input=dict(mode="source", repo_target=dict(repo_node=identities["repo"], provider_identity=identities["repo"],
            service_prefix=network["repo_service"]), preparation_state_file=str(root / "private-model/preparation.json"),
            private_handle_file=str(root / "private-model/handle.json")),
        grant=dict(requester_private_key_file=str(requester_key[0]), authority_public_key_file=str(authority_key[1]),
            authority_identity=identities["authority"], authority_service=network["artifact_service"], protection_epoch="epoch-1"),
        limits=dict(bootstrap_ms=profile["budgets"]["bootstrapMs"], max_prepared_entries=8,
            large_data_ims_limit=profile["budgets"]["largeDataImsLimit"],
            max_prepared_bytes=6417469728, max_assembled_bytes=catalog["recipe"]["max_assembled_bytes"],
            max_source_bytes=catalog["recipe"]["max_source_bytes"]),
        statistics_file=str(root / "statistics/requester.jsonl"),
        offer_admission=dict(candidate_digest=candidate_digest, public_key_files={key[2]: str(key[1]) for key in offers.values()},
            policy=dict(schema="spec180-provider-offer-trust-v1", candidateId=candidate_digest[7:],
                candidateDigest=candidate_digest, trustSchema=namespace + "/trust", entries=admission_entries)),
        conversation=dict(schema="ndnsf-di-native-conversation-v1",
            owner=dict(requester_identity=identities["requester"], service_name=network["service"], security_domain_digest=security_digest),
            journal=dict(identity="qwen-cpu-continuous-generation", state_root=str(root / "journal"),
                quota_bytes=profile["budgets"]["journalBytes"], keys=[dict(id="active", file=str(journal_key))]),
            turn=dict(mode="FULL_CONTEXT", retention_duration_ms=profile["budgets"]["conversationRetentionMs"])))
    if layout is not None or wired:
        requester["fixed_placement"] = {"role_providers": {
            role: identities[actor] for actor, actor_roles in provider_roles.items() for role in actor_roles}}
    config = dict(schema="ndnsf-di-deployment-v1", deployment_id=profile["id"] + "-" + secrets.token_hex(4),
        release=dict(prefix=str(prefix), manifest=str(install_receipt)),
        platform=dict(kind="minindn", topology=str(topology), topology_sha256="sha256:" + file_digest(topology)),
        nodes=[dict(id=name, host=name, cpus=6, gpu_devices=[], nfd=dict(mode="managed",
            socket=str(root / "sockets" / (name + ".sock")), address="127.0.0.1", port=6363)) for name in node_names],
        actors=actors, network=network,
        security=dict(trust_schema=str(trust), controller_certificate=str(certificates["controller"]), controller_policy=str(policy)),
        storage=dict(state_root=str(root / "state"), evidence_root=str(root / "evidence"), scratch_root=str(root / "scratch"),
            cache_root=str(root / "cache"), repo_root=str(root / "repo"), max_bytes=profile["budgets"]["storageBytes"]),
        model=dict(plan=str(plan_path), manifest=str(service_manifest), tokenizer=str(assets["tokenizer"])),
        requester=requester, artifact_authority=dict(schema="ndnsf-di-native-authority-v1", authority=authority,
            max_grant_ttl_ms=profile["budgets"]["conversationRetentionMs"], permission_bootstrap_ms=profile["budgets"]["permissionWaitMs"]))
    output = root / "deployment.json"
    write_json(output, config)
    run([prefix / "bin/ndnsf-di", "cluster", "check", "--config", output], env, root)
    # Reuse the installed owner and native schema enumerator, not a Python
    # interpretation of trust rules or an experiment-only PIB bypass.
    trust_code = '''
import json,sys
import ndnsf_di_launcher
ndnsf_di_launcher._bind_package_root()
from ndnsf_distributed_inference.ops.deployment_config import load_deployment
from ndnsf_distributed_inference.ops.cluster import _inspect_trust_resources
print(json.dumps(_inspect_trust_resources(load_deployment(sys.argv[1]),30)))
'''
    trust_proof = json.loads(run([interpreter, '-I', '-B', '-c', trust_code, output], env, root))
    write_json(inputs / 'trust-resource-proof.json', trust_proof)
    write_json(root / "frozen-inputs.json", dict(tokenizer_sha256=recipe["files"]["tokenizer"]["sha256"], turns=frozen,
        new_tokens=rendering["inputTokens"], output_budget=rendering["maxNewTokens"], expected_committed_prefix=rendering["expectedCommittedPrefix"]))
    write_json(root / "preparation-receipt.json", dict(schema="ndnsf-deployment-preparation-v1",
        status="CONFIGURATION_AND_NATIVE_INPUTS_CHECKED", namespace=namespace, profile=profile["id"],
        provider_signing=profile.get("providerSigning", "rsa"),
        asset_manifest=str(asset_manifest), asset_manifest_sha256=file_digest(asset_manifest),
        deployment_sha256=file_digest(output), install_manifest_sha256=file_digest(install_receipt),
        platform_facilities=facilities, actors_launched=False,
        qualification="Configuration, credentials and native resource inputs checked; runtime admission/inference NOT_RUN."))
    return output


def interrupted(signum, frame):
    raise KeyboardInterrupt("deployment preparation cancelled by signal " + str(signum))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, choices=(TP_PROFILE, *PIPELINE_PROFILES, *WIRED_VARIANTS))
    parser.add_argument("--provider-signing", choices=("rsa", "ecdsa"), default="rsa",
                        help="use separate ECDSA signing certificates in the wired TP profile")
    parser.add_argument("--prefix", required=True, type=Path)
    parser.add_argument("--asset-manifest", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--namespace", required=True)
    parser.add_argument("--offline", action="store_true", help="all inputs must be local; this tool never downloads")
    parser.add_argument("--plan", action="store_true", help="validate prerequisites and print the preparation plan without writes")
    args = parser.parse_args(argv)
    owned = False
    root = args.work_root.absolute()
    try:
        if args.provider_signing == "ecdsa" and args.profile != WIRED_TP:
            raise ValueError("DEPLOYMENT_ECDSA_REQUIRES_WIRED_TP_PROFILE")
        if re.fullmatch(r"/(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+", args.namespace) is None:
            raise ValueError("DEPLOYMENT_NAMESPACE_INVALID")
        if not args.prefix.is_absolute() or not args.asset_manifest.is_absolute() or not args.work_root.is_absolute():
            raise ValueError("DEPLOYMENT_PATHS_MUST_BE_ABSOLUTE")
        if root.exists() or root.is_symlink() or not root.parent.is_dir():
            raise ValueError("DEPLOYMENT_WORK_ROOT_MUST_BE_NEW_WITH_EXISTING_PARENT")
        if any(path.is_symlink() for path in (root, *root.parents)):
            raise ValueError("DEPLOYMENT_WORK_ROOT_SYMLINK_REJECTED")
        # This profile includes a durable Conversation journal. Match its
        # native admission contract before generating credentials, without a
        # test-only ephemeral-root override in an experiment configuration.
        if any(root == base or base in root.parents for base in
               (Path('/tmp'), Path('/run'), Path('/dev/shm'))):
            raise ValueError("DEPLOYMENT_PERSISTENT_WORK_ROOT_REQUIRED: conversation journal")
        prefix = args.prefix.resolve(strict=True)
        resources = prefix / "share/ndnsf/Experiments"
        extra = ()
        if args.profile in WIRED_VARIANTS:
            extra = (WIRED_PROFILE, "Topology/spec270-wired-star.conf",
                     WIRED_FULL_CATALOG if args.profile == WIRED_SINGLE
                     else "fixtures/native-deployment/qwen06b-pp14-catalog.json")
        receipt = installed_inputs(prefix, extra)
        recipe = read_json(resources / "recipes/qwen3-0.6b-int4.json")
        assets = validate_manifest(args.asset_manifest, recipe)
        if extra:
            profile = read_json(contained_file(resources, WIRED_PROFILE))
            layout = None
            profile.update(id=args.profile, catalog=profile.pop("catalogs")[args.profile])
            if args.profile == WIRED_SINGLE:
                profile["providers"] = 1
                del profile["actorNodes"]["provider-1"]
            if (profile["topology"] not in extra or
                    any(profile[field] not in (*extra, *PREPARATION_RESOURCES)
                        for field in ("catalog", "generationOptions"))):
                raise ValueError("DEPLOYMENT_PROFILE_RESOURCE_UNVERIFIED")
        else:
            profile, layout = load_profile(resources, args.profile)
        if args.provider_signing == "ecdsa":
            profile["providerSigning"] = args.provider_signing
        for field in ("catalog", "generationOptions"):
            contained_file(resources, profile[field])
        tool_path = str(prefix / "bin") + ":/usr/local/bin:/usr/bin:/bin"
        ndnsec = shutil.which("ndnsec", path=tool_path)
        openssl = shutil.which("openssl", path=tool_path)
        if not ndnsec or not openssl:
            raise ValueError("DEPLOYMENT_TOOL_MISSING: ndnsec and OpenSSL are required")
        if args.plan:
            print(json.dumps(dict(profile=args.profile, prefix=str(prefix), work_root=str(root),
                provider_signing=args.provider_signing,
                namespace=args.namespace, asset_manifest=str(args.asset_manifest),
                steps=["ndnsec isolated actor identities", "OpenSSL isolated Ed25519 keys", "native prompt encoding",
                       "render anchored trust and native configuration", "installed cluster check"], actors_launched=False)))
            return 0
        env = dict(PATH=tool_path, LANG="C.UTF-8",
                   LC_ALL="C.UTF-8", PYTHONNOUSERSITE="1")
        interpreter = operator_interpreter(prefix)
        facilities = platform_facilities(interpreter, env)
        mask = os.umask(0o077)
        previous = signal.signal(signal.SIGTERM, interrupted)
        try:
            root.mkdir(mode=0o700)
            owned = True
            output = prepare(profile, layout, recipe, assets, args.asset_manifest, prefix, root,
                             args.namespace, receipt, ndnsec, openssl, env, resources,
                             interpreter, facilities)
        finally:
            signal.signal(signal.SIGTERM, previous)
            os.umask(mask)
        print(output)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        if owned:
            shutil.rmtree(root)
        print("DEPLOYMENT_PREPARATION_FAILED: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
