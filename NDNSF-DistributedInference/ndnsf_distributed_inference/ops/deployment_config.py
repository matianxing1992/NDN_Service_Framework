"""Read-only deployment configuration checks and native JSON path translation.

This module does not authorize actors, plan inference, create runtime state or
probe remote credentials. Native runtime admission remains authoritative.
"""

from __future__ import annotations

import configparser
import copy
import hashlib
import json
import os
import re
import runpy
import stat
from pathlib import Path

from .allocation_topology import DIGEST, PROCESS_ID


class DeploymentConfigError(ValueError):
    pass


def _fail(field, reason):
    raise DeploymentConfigError(f"DEPLOYMENT_CONFIG_INVALID:{field}:{reason}")


def _object(value, field, allowed=None, required=()):
    if not isinstance(value, dict):
        _fail(field, "expected object")
    if allowed is not None and value.keys() - set(allowed):
        _fail(field, "unknown fields " + ",".join(sorted(value.keys() - set(allowed))))
    if set(required) - value.keys():
        _fail(field, "missing fields " + ",".join(sorted(set(required) - value.keys())))
    return value


def _string(value, field):
    if not isinstance(value, str) or not value or any(ord(c) < 32 for c in value):
        _fail(field, "expected nonempty text without control characters")
    return value


def _identifier(value, field):
    if not PROCESS_ID.fullmatch(_string(value, field)) or len(value) > 128:
        _fail(field, "invalid identifier")
    return value


def _integer(value, field, minimum=1):
    if type(value) is not int or value < minimum:
        _fail(field, f"expected integer >= {minimum}")
    return value


def _boolean(value, field):
    if type(value) is not bool:
        _fail(field, "expected boolean")
    return value


def _array(value, field, nonempty=False):
    if not isinstance(value, list) or (nonempty and not value):
        _fail(field, "expected " + ("nonempty " if nonempty else "") + "array")
    return value


def _name(value, field):
    # Reuse the declared python-ndn wire codec, not a second NDN Name parser.
    from ndn.encoding import Name

    if not _string(value, field).startswith("/") or value == "/":
        _fail(field, "expected non-root absolute NDN Name")
    try:
        return bytes(Name.to_bytes(Name.from_str(value)))
    except (ValueError, IndexError, TypeError):
        _fail(field, "invalid NDN Name")


def _pairs(items):
    result = {}
    for key, value in items:
        if key in result:
            _fail(key, "duplicate JSON key")
        result[key] = value
    return result


def _read_json(path):
    if not path.is_file():
        _fail("config", "expected regular JSON file")
    with path.open("rb") as stream:
        raw = stream.read(4 * 1024 * 1024 + 1)
    if len(raw) > 4 * 1024 * 1024:
        _fail("config", "JSON exceeds 4 MiB")
    try:
        return json.loads(raw, object_pairs_hook=_pairs)
    except (UnicodeError, json.JSONDecodeError, RecursionError):
        _fail("config", "malformed JSON")


def _digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def provider_recipient_map(path, identity):
    """Select this Provider's file reference; native code admits the key material."""
    source = Path(path)
    mapping = _object(_read_json(source), "provider.recipient-key-map")
    expected = _name(identity, "provider.identity")
    names = set()
    selected = None
    for name, value in mapping.items():
        wire = _name(name, "provider.recipient-key-map.identity")
        if wire in names:
            _fail("provider.recipient-key-map", "duplicate canonical Provider identity")
        names.add(wire)
        value = _string(value, "provider.recipient-key-map.path")
        if wire == expected:
            selected = (source.parent / value).resolve()
    if selected is None:
        _fail("provider.recipient-key-map", "configured Provider has no recipient key")
    if not selected.is_file() or not os.access(selected, os.R_OK):
        _fail("provider.recipient-key-map", "recipient private key file is unavailable")
    # Retain the actor's configured spelling. Native admission compares Names,
    # independently of the process's chosen URI display format.
    return {identity: str(selected)}


def _bind(target, key, expected, field, name=False):
    if key in target:
        matches = (_name(target[key], field) == _name(expected, field)
                   if name else target[key] == expected)
        if not matches:
            _fail(field, "conflicts with common deployment binding")
    target[key] = expected


def provider_cache_path(cache_root, identity):
    """Project a stable cache purpose from the full canonical Provider Name."""
    digest = hashlib.sha256(_name(identity, 'provider.identity')).hexdigest()
    return str(Path(cache_root) / ('provider-' + digest))


def preparation_deployment(config):
    """Project prepare intent before inference-only inputs are validated."""
    result = copy.deepcopy(config)
    result['operation'] = 'prepare'
    requester = _object(result['requester'], 'requester')
    model_input = _object(requester.get('model_input', {}), 'requester.model_input')
    publish_only = model_input.get('mode') == 'publish-stored'
    excluded = {'provider', 'artifact-authority'} if publish_only else {'provider'}
    result['actors'] = [actor for actor in _array(result['actors'], 'actors')
                        if _string(_object(actor, 'actor').get('kind'), 'actor.kind') not in excluded]
    for node in _array(result['nodes'], 'nodes', True):
        _object(node, 'node')
        node['gpu_devices'] = []
    for actor in result['actors']:
        if not _boolean(actor.get('external', False), 'actor.external'):
            _object(actor.get('resources'), 'actor.resources')['gpu_devices'] = []
    platform = _object(result['platform'], 'platform')
    if platform.get('kind') == 'slurm-apptainer':
        slurm = _object(platform.get('slurm'), 'platform.slurm')
        _object(slurm.get('gpu'), 'platform.slurm.gpu')['count'] = 0
    request = _object(requester.setdefault('request', {}), 'requester.request')
    request.pop('provider_names', None)
    requester.pop('fixed_placement', None)
    if publish_only:
        result['model'] = {}
        result['artifact_authority'] = {}
        for key in ('turns', 'grant', 'offer_admission', 'conversation', 'kv_handoff',
                    'test_stream_barrier', 'test_handoff_barrier'):
            requester.pop(key, None)
        for key in ('options_file', 'placement_strategy', 'placement_score_mode'):
            request.pop(key, None)
    return result


def load_deployment(path, *, prepare_only=False):
    """Return normalized management/native inputs; never mutate the filesystem."""
    source = Path(path).resolve(strict=True)
    config = _object(_read_json(source), "deployment", {
        "schema", "deployment_id", "release", "platform", "nodes", "actors",
        "network", "security", "storage", "model", "requester", "artifact_authority", "operation",
    }, {"schema", "deployment_id", "release", "platform", "nodes", "actors",
        "network", "security", "storage", "model", "requester", "artifact_authority"})
    if config["schema"] != "ndnsf-di-deployment-v1":
        _fail("schema", "unsupported schema")
    _identifier(config["deployment_id"], "deployment_id")
    if prepare_only:
        config = preparation_deployment(config)
    if config.get('operation', 'run') not in ('run', 'prepare'):
        _fail('operation', 'expected run or prepare')
    requester_input = _object(config['requester'], 'requester')
    model_input = _object(requester_input.get('model_input', {}), 'requester.model_input')
    publish_only = config.get('operation') == 'prepare' and model_input.get('mode') == 'publish-stored'
    platform = _object(config["platform"], "platform", {
        "kind", "sif", "sif_sha256", "build_record", "apptainer", "slurm",
        "topology", "topology_sha256"}, {"kind"})
    if not isinstance(platform["kind"], str) or platform["kind"] not in (
            "host", "minindn", "slurm-apptainer"):
        _fail("platform.kind", "unsupported platform")
    namespaced = platform["kind"] == "minindn"
    local = platform["kind"] != "slurm-apptainer"
    if platform["kind"] == "host" and platform.keys() != {"kind"}:
        _fail("platform", "host cannot declare container/scheduler inputs")
    if namespaced and platform.keys() != {"kind", "topology", "topology_sha256"}:
        _fail("platform", "minindn requires only kind, topology and topology_sha256")
    if not local and platform.keys() & {"topology", "topology_sha256"}:
        _fail("platform", "Slurm cannot declare MiniNDN inputs")
    read_paths = {source}

    def resolve(value, field, *, control=False, exists=False, directory=False, optional=False):
        candidate = Path(_string(value, field))
        if not local and not control:
            if not candidate.is_absolute() or str(candidate).startswith('//') or ".." in candidate.parts:
                _fail(field, "node/runtime path must be absolute without parent traversal")
            if exists and not directory:
                read_paths.add(candidate)
            return str(candidate)
        candidate = source.parent / candidate
        if field in {'requester.model_input.private_handle_file',
                     'requester.model_input.preparation_state_file'} and any(
                item.is_symlink() for item in (candidate, *candidate.parents)):
            _fail(field, 'private file path must not use symlinks')
        candidate = candidate.resolve()
        if exists and (not optional or candidate.exists()) and not (
                candidate.is_dir() if directory else candidate.is_file()):
            _fail(field, "required input is unavailable")
        if exists and (not optional or candidate.exists()) and not os.access(
                candidate, os.R_OK | (os.X_OK if directory else 0)):
            _fail(field, "required input is unreadable")
        if directory and candidate.exists() and not candidate.is_dir():
            _fail(field, "expected directory path")
        if exists and not directory:
            read_paths.add(candidate)
        return str(candidate)

    topology_hosts = set()
    if namespaced:
        platform['topology'] = resolve(platform['topology'], 'platform.topology', exists=True)
        expected = _string(platform['topology_sha256'], 'platform.topology_sha256')
        if not DIGEST.fullmatch(expected) or 'sha256:' + _digest_file(
                Path(platform['topology'])) != expected:
            _fail('platform.topology_sha256', 'topology digest mismatch')
        topology = configparser.ConfigParser(delimiters=' ', allow_no_value=True)
        try:
            topology.read(platform['topology'])
            if not topology.has_section('links'):
                _fail('platform.topology', 'missing links section')
            for key in topology.options('nodes'):
                host = _identifier(key.split(':', 1)[0], 'platform.topology.node')
                if host in topology_hosts:
                    _fail('platform.topology', 'duplicate host')
                topology_hosts.add(host)
        except (configparser.Error, UnicodeError) as error:
            _fail('platform.topology', str(error))

    if not local:
        _object(platform, "platform", required={"sif", "sif_sha256", "build_record", "apptainer", "slurm"})
        for key in ("sif", "build_record", "apptainer"):
            platform[key] = resolve(platform[key], "platform." + key, control=True, exists=True)
        if not DIGEST.fullmatch(_string(platform["sif_sha256"], "platform.sif_sha256")):
            _fail("platform.sif_sha256", "expected sha256 digest")
        # Reuse the exact wrapper admission implementation shipped in this wheel.
        # Full verification here replaces the previous image-only hash pass.
        validator_path = (Path(__file__).parent / '_container/adapters/slurm-apptainer/scripts'
                          / 'validate-local-sif-build-record.py')
        validator = runpy.run_path(str(validator_path))
        _read_json(Path(platform['build_record']))
        try:
            validator['validate'](platform['build_record'], platform['sif'],
                                  platform['sif_sha256'], purpose='deployment')
        except (validator['BuildRecordError'], OSError) as error:
            _fail('platform.build_record', str(error))
        slurm = _object(platform["slurm"], "platform.slurm", {
            "jobName", "partition", "account", "qos", "wallTime", "nodes", "tasksPerNode",
            "cpusPerTask", "memory", "gpu"},
            {"partition", "account", "wallTime", "tasksPerNode", "cpusPerTask", "memory", "gpu"})
        for key in ("jobName", "partition", "account", "qos", "wallTime", "memory"):
            if key in slurm:
                _string(slurm[key], "platform.slurm." + key)
        for key in ("nodes", "tasksPerNode", "cpusPerTask"):
            if key in slurm:
                _integer(slurm[key], "platform.slurm." + key)
        gpu = _object(slurm["gpu"], "platform.slurm.gpu", {"type", "count"}, {"type", "count"})
        _string(gpu["type"], "platform.slurm.gpu.type")
        _integer(gpu["count"], "platform.slurm.gpu.count", 0)

    release = _object(config["release"], "release", {"prefix", "manifest"}, {"prefix", "manifest"})
    release["prefix"] = resolve(release["prefix"], "release.prefix", exists=local, directory=True)
    release["manifest"] = resolve(release["manifest"], "release.manifest", control=True, exists=True)
    ledger = _object(_read_json(Path(release["manifest"])), "release.manifest")
    if ledger.get("schema") != 1 or not isinstance(ledger.get("files"), dict):
        _fail("release.manifest", "unsupported installer ledger")

    security = _object(config["security"], "security", {
        "trust_schema", "controller_certificate", "controller_policy"},
        {"trust_schema", "controller_certificate"})
    model = _object(config["model"], "model", {"plan", "manifest", "tokenizer"},
                    () if config.get('operation') == 'prepare' else {"plan", "manifest", "tokenizer"})
    for key in model:
        model[key] = resolve(model[key], "model." + key, exists=True)
    storage = _object(config["storage"], "storage", {
        "state_root", "evidence_root", "scratch_root", "cache_root", "repo_root", "max_bytes"},
        {"state_root", "evidence_root", "scratch_root", "cache_root", "repo_root", "max_bytes"})
    _integer(storage["max_bytes"], "storage.max_bytes")
    roots = []
    for key in storage.keys() - {"max_bytes"}:
        storage[key] = resolve(storage[key], "storage." + key)
        candidate = Path(storage[key])
        if candidate == Path("/") or any(
                candidate == other or candidate in other.parents or other in candidate.parents
                for other in roots):
            _fail("storage." + key, "storage roots must be disjoint and non-root")
        roots.append(candidate)

    nodes = {}
    nfd_endpoints = set()
    for node in _array(config["nodes"], "nodes", True):
        _object(node, "node", {"id", "host", "cpus", "gpu_devices", "nfd"},
                {"id", "host", "cpus", "nfd"})
        node_id = _identifier(node["id"], "node.id")
        if node_id in nodes:
            _fail("nodes", "duplicate node ID")
        _string(node["host"], "node.host")
        if platform['kind'] == 'host' and (node["host"] != "local" or nodes):
            _fail("nodes", "host platform requires one local node")
        if namespaced:
            _identifier(node['host'], 'node.host')
            if node['host'] not in topology_hosts or any(
                    node['host'] == previous['host'] for previous in nodes.values()):
                _fail('node.host', 'expected a distinct declared topology host')
        _integer(node["cpus"], "node.cpus")
        devices = _array(node.setdefault("gpu_devices", []), "node.gpu_devices")
        for device in devices:
            _string(device, "node.gpu_devices")
            if device.startswith("allocated:"):
                if local or not re.fullmatch(r"allocated:(?:[0-9]|[1-5][0-9]|6[0-3])", device):
                    _fail("node.gpu_devices", "allocation-relative device requires Slurm index 0-63")
                if int(device[len("allocated:"):]) >= platform["slurm"]["gpu"]["count"]:
                    _fail("node.gpu_devices", "allocation-relative index exceeds reservation")
        if len(set(devices)) != len(devices):
            _fail("node.gpu_devices", "duplicate device")
        nfd = _object(node["nfd"], "node.nfd", {"mode", "socket", "address", "port"},
                      {"mode", "socket", "address", "port"})
        if not isinstance(nfd["mode"], str) or nfd["mode"] not in {"managed", "external"}:
            _fail("node.nfd.mode", "expected managed or external")
        nfd["socket"] = resolve(nfd["socket"], "node.nfd.socket")
        if namespaced and (nfd['mode'] != 'managed' or any(
                nfd['socket'] == previous['nfd']['socket'] for previous in nodes.values())):
            _fail('node.nfd', 'MiniNDN requires managed NFDs with distinct local sockets')
        _string(nfd["address"], "node.nfd.address")
        if _integer(nfd["port"], "node.nfd.port") > 65535:
            _fail("node.nfd.port", "port exceeds 65535")
        endpoint = (node["host"], nfd["address"], nfd["port"])
        if endpoint in nfd_endpoints:
            _fail("nodes", "duplicate NFD endpoint")
        nfd_endpoints.add(endpoint)
        nodes[node_id] = node

    if namespaced and {node['host'] for node in nodes.values()} != topology_hosts:
        _fail('nodes', 'declare every topology host, including forwarding-only hosts')

    actors = {}
    identities = set()
    homes = set()
    by_kind = {key: [] for key in ("controller", "artifact-authority", "requester", "provider", "repo")}
    reservations = {key: [] for key in nodes}
    for actor in _array(config["actors"], "actors", True):
        _object(actor, "actor", {"id", "kind", "identity", "external", "node",
                               "credential_home", "resources", "parameters"},
                {"id", "kind", "identity"})
        actor_id = _identifier(actor["id"], "actor.id")
        identity = _name(actor["identity"], "actor.identity")
        if actor_id in actors or identity in identities:
            _fail("actors", "duplicate actor ID or canonical NDN identity")
        if not isinstance(actor["kind"], str) or actor["kind"] not in by_kind:
            _fail("actor.kind", "unsupported actor kind")
        external = _boolean(actor.setdefault("external", False), "actor.external")
        if external:
            if actor.keys() & {"node", "credential_home", "resources", "parameters"}:
                _fail("actor", "external references cannot own process inputs")
        else:
            _object(actor, "actor", required={"node", "credential_home", "resources"})
            if not isinstance(actor["node"], str) or actor["node"] not in nodes:
                _fail("actor.node", "unknown node")
            actor["credential_home"] = resolve(actor["credential_home"], "actor.credential_home",
                                                exists=True, directory=True)
            home = (actor["node"], actor["credential_home"])
            if any((local or node == home[0]) and
                   (Path(previous) == Path(home[1]) or
                    Path(previous) in Path(home[1]).parents or
                    Path(home[1]) in Path(previous).parents)
                   for node, previous in homes):
                _fail("actor.credential_home", "owned actors require separate disjoint credential homes")
            homes.add(home)
            resources = _object(actor["resources"], "actor.resources", {
                "cpus", "gpu_devices", "allow_sharing"}, {"cpus"})
            _integer(resources["cpus"], "actor.resources.cpus")
            _boolean(resources.setdefault("allow_sharing", False), "actor.resources.allow_sharing")
            devices = _array(resources.setdefault("gpu_devices", []), "actor.resources.gpu_devices")
            for device in devices:
                _string(device, "actor.resources.gpu_devices")
            if len(set(devices)) != len(devices) or not set(devices) <= set(nodes[actor["node"]]["gpu_devices"]):
                _fail("actor.resources.gpu_devices", "duplicate or unavailable node device")
            reservations[actor["node"]].append(resources)
            _object(actor.setdefault("parameters", {}), "actor.parameters")
        actors[actor_id] = actor
        identities.add(identity)
        by_kind[actor["kind"]].append(actor)
    for kind in ("controller", "requester"):
        if len(by_kind[kind]) != 1:
            _fail("actors", "requires exactly one " + kind)
    if len(by_kind['artifact-authority']) > 1 or (
            not publish_only and len(by_kind['artifact-authority']) != 1):
        _fail('actors', 'requires exactly one artifact-authority except publish-only preparation')
    if by_kind["controller"][0]["external"]:
        if "controller_policy" in security:
            _fail("security.controller_policy", "external Controller cannot own policy inputs")
    elif "controller_policy" not in security:
        _fail("security.controller_policy", "owned Controller requires policy input")
    for key in security:
        security[key] = resolve(security[key], "security." + key, exists=True)
    if ((not by_kind["provider"] and config.get('operation') != 'prepare') or
            len(by_kind["repo"]) > 1):
        _fail("actors", "requires providers and at most one Repo")
    if config.get('operation') == 'prepare' and (by_kind['provider'] or
            any(node['gpu_devices'] for node in nodes.values()) or
            any(actor.get('resources', {}).get('gpu_devices') for actor in actors.values()) or
            (platform['kind'] == 'slurm-apptainer' and platform['slurm']['gpu']['count'])):
        _fail('operation', 'prepare cannot launch Providers or reserve GPUs')
    for node_id, rows in reservations.items():
        forwarder_cpus = int(not local and nodes[node_id]['nfd']['mode'] == 'managed')
        if forwarder_cpus + sum(row["cpus"] for row in rows) > nodes[node_id]["cpus"] and not all(
                row["allow_sharing"] for row in rows):
            _fail("actor.resources.cpus", "CPU overcommit requires explicit sharing by all actors; "
                  "managed Slurm NFD reserves one CPU")
        for device in nodes[node_id]["gpu_devices"]:
            owners = [row for row in rows if device in row["gpu_devices"]]
            if len(owners) > 1 and not all(row["allow_sharing"] for row in owners):
                _fail("actor.resources.gpu_devices", "shared device requires explicit sharing by all owners")
    if namespaced:
        rows = [row for group in reservations.values() for row in group]
        devices = {device for row in rows for device in row['gpu_devices']}
        for device in devices:
            owners = [row for row in rows if device in row['gpu_devices']]
            if len(owners) > 1 and not all(row['allow_sharing'] for row in owners):
                _fail('actor.resources.gpu_devices',
                      'MiniNDN shares one physical host; device sharing must be explicit')

    network = _object(config["network"], "network", {
        "group", "service", "artifact_service", "repo_service", "routes"},
        {"group", "service", "artifact_service"})
    for key in network.keys() - {"routes"}:
        _name(network[key], "network." + key)
    service_names = [_name(network[key], "network." + key)
                     for key in ("service", "artifact_service", "repo_service") if key in network]
    if len(set(service_names)) != len(service_names):
        _fail("network", "distinct services require distinct canonical Names")
    if by_kind["repo"] and "repo_service" not in network:
        _fail("network.repo_service", "Repo requires explicit service Name")
    for route in _array(network.setdefault("routes", []), "network.routes"):
        _object(route, "route", {"from", "to", "prefix"}, {"from", "to", "prefix"})
        if not isinstance(route["from"], str) or not isinstance(route["to"], str) or (
                route["from"] not in nodes or route["to"] not in nodes or route["from"] == route["to"]):
            _fail("route", "expected two distinct declared nodes")
        _name(route["prefix"], "route.prefix")

    if not local:
        slurm = platform["slurm"]
        _bind(slurm, "nodes", len(nodes), "platform.slurm.nodes")
        if len({node["host"] for node in nodes.values()}) != len(nodes):
            _fail("nodes", "duplicate allocation node binding")
        for node in nodes.values():
            if node["cpus"] > slurm["tasksPerNode"] * slurm["cpusPerTask"]:
                _fail("node.cpus", "exceeds scheduler CPU reservation per node")
            if len(node["gpu_devices"]) > slurm["gpu"]["count"]:
                _fail("node.gpu_devices", "exceeds scheduler GPU reservation per node")

    writes = _translate_native(config, by_kind, resolve)
    protected_roots = [Path(release["prefix"])] + [
        Path(actor["credential_home"]) for actor in actors.values() if not actor["external"]]
    journal = config["requester"].get("conversation", {}).get("journal")
    journal_root = Path(journal["state_root"]) / journal["identity"] if journal else None
    for value, directory, field in writes:
        output = Path(value)
        if any(output == item or (directory and output in item.parents) for item in read_paths):
            _fail(field, "output overlaps protected input")
        if any(output == root or root in output.parents or (directory and output in root.parents)
               for root in protected_roots):
            _fail(field, "output overlaps installed runtime or credentials")
        if journal_root is not None and field != "requester.conversation.journal.state_root" and (
                output == journal_root or journal_root in output.parents or
                (directory and output in journal_root.parents)):
            _fail(field, "output overlaps native conversation journal")
    if local:
        _check_installed_files(release, ledger, by_kind)
    return config


def _translate_native(config, by_kind, resolve):
    local = config['platform']['kind'] != 'slurm-apptainer'
    network, security = config["network"], config["security"]
    controller = by_kind["controller"][0]["identity"]
    requester_identity = by_kind["requester"][0]["identity"]
    authority_actor = next(iter(by_kind['artifact-authority']), None)
    authority_identity = authority_actor['identity'] if authority_actor else None

    writes = []

    def paths(obj, fields, location, *, exists=True, directory=False, optional=False):
        _object(obj, location)
        for key in fields:
            if key in obj:
                obj[key] = resolve(obj[key], location + "." + key, exists=exists,
                                   directory=directory, optional=optional)
                if not exists and not optional:
                    target = Path(obj[key])
                    if local and not directory and target.exists() and not target.is_file():
                        _fail(location + "." + key, "output must be a regular file path")
                    writes.append((obj[key], directory, location + "." + key))

    def bind(obj, values, location, names=True):
        _object(obj, location)
        for key, value in values.items():
            _bind(obj, key, value, location + "." + key, name=names)

    requester = _object(config["requester"], "requester")
    requester_external = by_kind["requester"][0]["external"]
    authority_external = authority_actor is None or authority_actor['external']
    if requester_external and requester:
        _fail("requester", "external Requester cannot own native process inputs")
    if authority_external and config["artifact_authority"]:
        _fail("artifact_authority", "external Authority cannot own native process inputs")
    if not requester_external:
        _translate_requester(requester, network, security, controller, requester_identity,
                             authority_identity, paths, bind,
                             operation=config.get('operation', 'run'),
                             allow_test_barriers=config['platform']['kind'] == 'minindn')
        if "model_repository" in requester:
            target = requester["model_repository"]
            matches = [a for a in by_kind["repo"] if _name(a["identity"], "actor.identity") ==
                       _name(target["provider_identity"], "requester.model_repository.provider_identity")]
            if len(matches) != 1:
                _fail("requester.model_repository", "requires one declared Repo provider")
            if not matches[0]["external"]:
                expected_node = matches[0]['parameters'].get('repo-node', matches[0]['identity'])
                _bind(target, "repo_node", expected_node,
                      "requester.model_repository.repo_node", name=True)
        if 'repo_target' in requester.get('model_input', {}):
            target = requester['model_input']['repo_target']
            matches = [a for a in by_kind['repo'] if _name(a['identity'], 'actor.identity') ==
                       _name(target['provider_identity'], 'requester.model_input.repo_target.provider_identity')]
            if len(matches) != 1:
                _fail('requester.model_input.repo_target', 'requires one declared Repo provider')
            if not matches[0]['external'] and 'repo-node' in matches[0]['parameters']:
                _bind(target, 'repo_node', matches[0]['parameters']['repo-node'],
                      'requester.model_input.repo_target.repo_node', name=True)
        known_providers = {_name(actor["identity"], "provider.identity") for actor in by_kind["provider"]}
        request = requester["request"]
        if "provider_names" in request:
            references = [_name(value, "requester.request.provider_names") for value in
                          _array(request["provider_names"], "requester.request.provider_names")]
            if len(set(references)) != len(references) or not set(references) <= known_providers:
                _fail("requester.request.provider_names", "duplicate or undeclared Provider reference")
        if "fixed_placement" in requester:
            placement = _object(requester["fixed_placement"], "requester.fixed_placement")
            references = _object(placement.get("role_providers"), "requester.fixed_placement.role_providers")
            if any(_name(value, "requester.fixed_placement.role_providers") not in known_providers
                   for value in references.values()):
                _fail("requester.fixed_placement.role_providers", "undeclared Provider reference")
    authority_config = _object(config["artifact_authority"], "artifact_authority")
    if not authority_external:
        _translate_authority(authority_config, network, security, controller, requester_identity,
                             authority_identity, paths, bind)
    if not requester_external and not authority_external and 'grant' in requester and (
            requester["grant"]["protection_epoch"] != authority_config["authority"]["protection_epoch"]):
        _fail("authority.protection_epoch", "conflicts with requester grant epoch")

    for actor in config["actors"]:
        if actor["external"]:
            continue
        parameters = actor["parameters"]
        if actor["kind"] == "provider":
            _object(parameters, "provider.parameters", {
                "roles", "selection-offer-key-file", "offer-backend", "offer-can-provision",
                "offer-has-model", "permission-wait-ms", "workers", "handler-threads", "ack-threads",
                "artifact-cache-dir", "artifact-cache-max-mib", "statistics-file",
                "allow-selection-generation-options", "sample-kv-state-values",
                "enable-admission-lease", "require-execution-lease", "execution-policy", "admission-lease-ttl-ms",
                "grant-authority-public-key", "recipient-key-map", "repo-fetch-timeout-ms",
                "assembly-timeout-ms",
                "conversation-retention-ms", "kv-handoff-config"})
            grant_fields = {"grant-authority-public-key", "recipient-key-map"}
            if parameters.keys() & grant_fields and not grant_fields <= parameters.keys():
                _fail("provider.parameters", "grant public hint and recipient key map are required together")
            paths(parameters, tuple(sorted(grant_fields)), "provider.parameters")
            if grant_fields <= parameters.keys() and local:
                hint = Path(parameters["grant-authority-public-key"])
                resolve(str(hint.parent / "trust-root-registry-v1.json"),
                        "provider.grant.registry", exists=True)
                selected = provider_recipient_map(parameters["recipient-key-map"], actor["identity"])
                resolve(selected[actor["identity"]], "provider.recipient-private-key", exists=True)
            for key in ("offer-can-provision", "offer-has-model", "allow-selection-generation-options",
                        "enable-admission-lease", "require-execution-lease", "sample-kv-state-values"):
                if key in parameters:
                    _boolean(parameters[key], "provider.parameters." + key)
            for key in ("permission-wait-ms", "workers", "handler-threads", "ack-threads",
                        "admission-lease-ttl-ms", "repo-fetch-timeout-ms", "conversation-retention-ms",
                        "assembly-timeout-ms", "artifact-cache-max-mib"):
                if key in parameters:
                    _integer(parameters[key], "provider.parameters." + key)
            if parameters.get("repo-fetch-timeout-ms", 1) > 2147483647:
                _fail("provider.parameters.repo-fetch-timeout-ms", "exceeds native signed int timeout")
            if parameters.get("artifact-cache-max-mib", 1) > 2147483647:
                _fail("provider.parameters.artifact-cache-max-mib", "exceeds native signed int MiB limit")
            if parameters.get("conversation-retention-ms", 1) > 3600000:
                _fail("provider.parameters.conversation-retention-ms", "exceeds native retention limit")
            if parameters.get("assembly-timeout-ms", 1) > 3600000:
                _fail("provider.parameters.assembly-timeout-ms", "exceeds native assembly limit")
            for key in ("roles", "offer-backend", "execution-policy"):
                if key in parameters:
                    _string(parameters[key], "provider.parameters." + key)
            if "execution-policy" in parameters and parameters["execution-policy"] not in {
                    "DATA_DRIVEN_V2", "LEGACY_READY_SET_V1"}:
                _fail("provider.parameters.execution-policy", "unsupported native execution policy")
            if parameters.get("execution-policy") == "LEGACY_READY_SET_V1" and not parameters.get("require-execution-lease"):
                _fail("provider.parameters.execution-policy", "legacy policy requires execution lease")
            paths(parameters, ("selection-offer-key-file", "kv-handoff-config"), "provider.parameters")
            paths(parameters, ("artifact-cache-dir",), "provider.parameters", exists=False, directory=True)
            paths(parameters, ("statistics-file",), "provider.parameters", exists=False)
        elif actor['kind'] == 'repo':
            _object(parameters, 'repo.parameters', {'request-timeout-ms', 'repo-node'})
            if 'repo-node' in parameters:
                _name(parameters['repo-node'], 'repo.parameters.repo-node')
            if 'request-timeout-ms' in parameters:
                timeout = _integer(parameters['request-timeout-ms'],
                                   'repo.parameters.request-timeout-ms', 251)
                if timeout > 2147483647:
                    _fail('repo.parameters.request-timeout-ms', 'exceeds native signed int')
        elif parameters:
            _fail("actor.parameters", "role parameters belong in common/native configuration")
    barrier_outputs = {requester[key][field]
                       for key in ('test_stream_barrier', 'test_handoff_barrier') if key in requester
                       for field in ('ready_file', 'release_file')}
    barrier_locations = {'requester.' + key + '.' + field
                         for key in ('test_stream_barrier', 'test_handoff_barrier')
                         for field in ('ready_file', 'release_file')}
    for path, _, location in writes:
        if path in barrier_outputs and location not in barrier_locations:
            _fail(location, 'barrier path overlaps another output')
    return writes


def _translate_requester(requester, network, security, controller, requester_identity,
                         authority_identity, paths, bind, *, operation='run',
                         allow_test_barriers=False):
    model_input = _object(requester.get('model_input', {'mode': 'source'}), 'requester.model_input')
    mode = model_input.get('mode')
    if mode not in ('source', 'publish-stored', 'stored'):
        _fail('requester.model_input.mode', 'expected source, publish-stored or stored')
    barrier_paths = set()
    for key in ('test_stream_barrier', 'test_handoff_barrier'):
        if key not in requester:
            continue
        location = 'requester.' + key
        if not allow_test_barriers or requester.get('test_only') is not True:
            _fail(location, 'experiment harness field requires explicit MiniNDN test_only=true')
        barrier = _object(requester[key], location,
                          {'ready_file', 'release_file', 'timeout_ms'},
                          {'ready_file', 'release_file', 'timeout_ms'})
        handoff = _object(requester.get('kv_handoff', {}), 'requester.kv_handoff')
        maximum = (650000 if key == 'test_handoff_barrier' and
                   handoff.get('wait_for_departure') is True else 60000)
        if _integer(barrier['timeout_ms'], location + '.timeout_ms') > maximum:
            _fail(location + '.timeout_ms', 'exceeds native barrier limit')
        paths(barrier, ('ready_file', 'release_file'), location, exists=False)
        for field in ('ready_file', 'release_file'):
            path = barrier[field]
            if path in barrier_paths:
                _fail(location + '.' + field, 'barrier output paths must be distinct')
            barrier_paths.add(path)
    _bind(requester, "schema", "ndnsf-di-native-requester-v1", "requester.schema")
    fields = {'mode', 'repo_target', 'private_handle_file'}
    if mode in ('source', 'publish-stored'):
        fields.add('preparation_state_file')
        if mode == 'publish-stored' and operation != 'prepare':
            _fail('requester.model_input.mode', 'publish-stored requires prepare operation')
    if mode == 'stored' and operation == 'prepare':
        _fail('requester.model_input.mode', 'stored mode cannot prepare or republish')
    _object(model_input, 'requester.model_input', fields, fields)
    if mode in ('source', 'publish-stored', 'stored'):
        if requester.keys() & {'model_repository', 'encrypted_repository', 'cache_compatibility'}:
            _fail('requester.model_input', 'cannot combine model publication modes')
        target = _object(model_input['repo_target'], 'requester.model_input.repo_target',
                         {'repo_node', 'provider_identity', 'service_prefix'},
                         {'repo_node', 'provider_identity', 'service_prefix'})
        for key, value in target.items():
            _name(value, 'requester.model_input.repo_target.' + key)
        _bind(target, 'service_prefix', network.get('repo_service'),
              'requester.model_input.repo_target.service_prefix', name=True)
        paths(model_input, ('private_handle_file',), 'requester.model_input', exists=mode == 'stored')
        catalog = _object(requester.get('catalog'), 'requester.catalog')
        source = _object(catalog.get('source'), 'requester.catalog.source')
        if mode in ('source', 'publish-stored'):
            paths(model_input, ('preparation_state_file',), 'requester.model_input', exists=False)
            if model_input['private_handle_file'] == model_input['preparation_state_file']:
                _fail('requester.model_input', 'preparation state and private handle must be distinct')
        elif source.keys() & {'file', 'initializer_file'}:
            _fail('requester.catalog.source', 'stored mode cannot read source files')
    core = requester.setdefault("core", {})
    paths(core, ("trust_schema_file",), "requester.core")
    bind(core, {"requester_identity": requester_identity, "authority_identity": controller,
                "group": network["group"]}, "requester.core")
    bind(core, {"trust_schema_file": security["trust_schema"]}, "requester.core", False)
    request = requester.setdefault('request', {})
    bind(request, {'service': network['service']}, 'requester.request')
    if mode == 'publish-stored':
        if 'catalog' in requester:
            catalog = _object(requester['catalog'], 'requester.catalog')
            if 'source' in catalog:
                paths(catalog['source'], ('file', 'initializer_file'), 'requester.catalog.source',
                      optional=True)
        paths(requester, ('statistics_file',), 'requester', exists=False)
        return
    _array(requester.get("turns"), "requester.turns", True)
    grant = requester.setdefault("grant", {})
    _object(grant, "requester.grant", required={
        "requester_private_key_file", "authority_public_key_file", "protection_epoch"})
    _string(grant["protection_epoch"], "requester.grant.protection_epoch")
    if grant.keys() & {"authority_private_key_file", "content_key_file", "content_key_id"}:
        _fail("requester.grant", "issuer private inputs cannot belong to requester")
    bind(grant, {"authority_identity": authority_identity,
                 "authority_service": network["artifact_service"]}, "requester.grant")
    paths(grant, ("requester_private_key_file", "authority_public_key_file"), "requester.grant")
    paths(request, ("options_file",), "requester.request")
    if "offer_admission" in requester:
        admission = _object(requester["offer_admission"], "requester.offer_admission")
        keys = _object(admission.get("public_key_files", {}), "requester.offer_admission.public_key_files")
        paths(keys, tuple(keys), "requester.offer_admission.public_key_files")
    if "catalog" in requester:
        catalog = _object(requester["catalog"], "requester.catalog")
        if "source" in catalog:
            paths(catalog["source"], ("file", "initializer_file"), "requester.catalog.source",
                  optional=mode == 'source')
    if "model_repository" in requester:
        target = _object(requester["model_repository"], "requester.model_repository",
                         {"repo_node", "provider_identity", "service_prefix"},
                         {"repo_node", "provider_identity", "service_prefix"})
        for key, value in target.items():
            _name(value, "requester.model_repository." + key)
        _bind(target, "service_prefix", network.get("repo_service"),
              "requester.model_repository.service_prefix", name=True)
    for key in ("repository", "encrypted_repository"):
        if key in requester:
            paths(requester[key], ("path",), "requester." + key, exists=False, directory=True)
    if "source_cache_path" in requester.get("repository", {}):
        paths(requester["repository"], ("source_cache_path",), "requester.repository",
              exists=False, directory=True)
    paths(requester, ("statistics_file",), "requester", exists=False)
    for turn in requester["turns"]:
        _object(turn, "requester.turns", required={"input_file", "options_file", "output_file"})
        paths(turn, ("input_file", "options_file"), "requester.turns")
        paths(turn, ("output_file", "checkpoint_output_file"), "requester.turns", exists=False)
    if "conversation" in requester:
        conversation = _object(requester["conversation"], "requester.conversation",
                               required={"journal", "owner"})
        _bind(conversation, "schema", "ndnsf-di-native-conversation-v1", "requester.conversation.schema")
        journal = _object(conversation["journal"], "requester.conversation.journal",
                          required={"state_root", "identity", "keys"})
        _string(journal["identity"], "requester.conversation.journal.identity")
        if journal["identity"] in (".", "..") or any(c in journal["identity"] for c in "/\\"):
            _fail("requester.conversation.journal.identity", "expected one directory component")
        paths(journal, ("state_root",), "requester.conversation.journal", exists=False, directory=True)
        for key in _array(journal["keys"], "requester.conversation.journal.keys", True):
            _object(key, "requester.conversation.journal.keys", required={"id", "file"})
            _string(key["id"], "requester.conversation.journal.keys.id")
            paths(key, ("file",), "requester.conversation.journal.keys")
        owner = _object(conversation["owner"], "requester.conversation.owner")
        bind(owner, {"requester_identity": requester_identity,
                     "service_name": network["service"]}, "requester.conversation.owner")
        digest = _string(request.get("security_policy_digest"), "requester.request.security_policy_digest")
        if not DIGEST.fullmatch(digest):
            _fail("requester.request.security_policy_digest", "expected sha256 digest")
        bind(owner, {"security_domain_digest": digest}, "requester.conversation.owner", False)
        if "turn" in conversation:
            turn = _object(conversation["turn"], "requester.conversation.turn")
            if "retention_deadline_ms" in turn and "retention_duration_ms" in turn:
                _fail("requester.conversation.turn", "retention deadline and duration are mutually exclusive")
            for key in ("retention_deadline_ms", "retention_duration_ms"):
                if key in turn:
                    value = _integer(turn[key], "requester.conversation.turn." + key)
                    maximum = 3600000 if key == "retention_duration_ms" else 2**64 - 1
                    if value > maximum:
                        _fail("requester.conversation.turn." + key, "exceeds native retention limit")
            paths(turn, ("parent_state_file",), "requester.conversation.turn")
        paths(conversation, ("checkpoint_output_file",), "requester.conversation", exists=False)


def _translate_authority(authority_config, network, security, controller, requester_identity,
                         authority_identity, paths, bind):
    _bind(authority_config, "schema", "ndnsf-di-native-authority-v1", "artifact_authority.schema")
    authority = authority_config.setdefault("authority", {})
    _object(authority, "artifact_authority.authority", required={
        "authority_private_key_file", "content_key_file",
        "recipient_public_key_files", "allowed_model_manifests", "content_key_id", "protection_epoch"})
    _string(authority["protection_epoch"], "authority.protection_epoch")
    paths(authority, ("trust_schema_file", "authority_private_key_file", "requester_public_key_file",
                      "content_key_file"), "artifact_authority.authority")
    if 'requester_public_key_files' in authority:
        if 'requester_identity' in authority or 'requester_public_key_file' in authority:
            _fail('artifact_authority.authority', 'requester registry conflicts with legacy fields')
        requesters = _object(authority['requester_public_key_files'],
                             'authority.requester_public_key_files')
        if not requesters or len(requesters) > 4096:
            _fail('authority.requester_public_key_files', 'expected 1 to 4096 requesters')
        identities = {_name(name, 'authority.requester_public_key_files.identity')
                      for name in requesters}
        if len(identities) != len(requesters):
            _fail('authority.requester_public_key_files', 'duplicate canonical requester identity')
        if _name(requester_identity, 'requester.identity') not in identities:
            _fail('authority.requester_public_key_files', 'current requester is not authorized')
        paths(requesters, tuple(requesters), 'authority.requester_public_key_files')
    else:
        if 'requester_public_key_file' not in authority:
            _fail('artifact_authority.authority', 'missing requester_public_key_file')
        bind(authority, {'requester_identity': requester_identity}, 'artifact_authority.authority')
    bind(authority, {"identity": authority_identity,
                     "controller_identity": controller, "group": network["group"],
                     "service": network["artifact_service"]}, "artifact_authority.authority")
    bind(authority, {"trust_schema_file": security["trust_schema"]}, "artifact_authority.authority", False)
    if "recipient_public_key_files" in authority:
        keys = _object(authority["recipient_public_key_files"], "authority.recipient_public_key_files")
        paths(keys, tuple(keys), "authority.recipient_public_key_files")


def _check_installed_files(release, ledger, by_kind, *, image_root=None, extra_programs=()):
    programs = {"controller": "App_ServiceController", "artifact-authority": "DI_NativeArtifactAuthority",
                "requester": "DI_NativeRequester", "provider": "di-native-provider",
                "repo": "DistributedRepoServer"}
    prefix = Path(release["prefix"])
    def observed(path):
        return image_root / path.relative_to('/') if image_root is not None else path
    required = {prefix / "bin" / executable for kind, executable in programs.items()
                if any(not actor["external"] for actor in by_kind[kind])}
    required.update(prefix / program for program in extra_programs)
    if any(not actor["external"] for actors in by_kind.values() for actor in actors):
        required.add(prefix / "bin/ndnsf-identity-check")
    if any(not actor["external"] for actor in by_kind["provider"]):
        required.add(prefix / "libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker")
    if any(not actor['external'] and actor.get('resources', {}).get('gpu_devices')
           for actors in by_kind.values() for actor in actors):
        required.add(prefix / 'libexec/ndnsf-di/ndnsf-di-device-query')
    for identity in required:
        row = ledger["files"].get(str(identity), {})
        path = observed(identity)
        installed = row.get("installed", {}) if isinstance(row, dict) else {}
        if installed.get("kind") != "file" or not path.is_file() or path.is_symlink():
            _fail("release.manifest", "required installed executable missing: " + path.name)
        if (_digest_file(path) != installed.get("sha256") or not os.access(path, os.X_OK) or
                stat.S_IMODE(path.stat().st_mode) != installed.get("mode")):
            _fail("release.manifest", "installed executable identity/mode mismatch: " + path.name)
    for library in ("libndn-service-framework.so", "libndnsf-distributed-inference.so"):
        identity = prefix / "lib" / library
        path = observed(identity)
        row = ledger["files"].get(str(identity), {})
        installed = row.get("installed", {}) if isinstance(row, dict) else {}
        if installed.get("kind") == "symlink":
            target = installed.get("target")
            if not isinstance(target, str) or Path(target).name != target or not path.is_symlink():
                _fail("release.manifest", "invalid library link: " + library)
            if Path(os.readlink(path)) != Path(target):
                _fail("release.manifest", "library link identity mismatch: " + library)
            path = path.parent / target
            row = ledger["files"].get(str(identity.parent / target), {})
            installed = row.get("installed", {}) if isinstance(row, dict) else {}
        if installed.get("kind") != "file" or not path.is_file() or path.is_symlink():
            _fail("release.manifest", "required installed library missing: " + library)
        if (_digest_file(path) != installed.get("sha256") or not os.access(path, os.R_OK) or
                stat.S_IMODE(path.stat().st_mode) != installed.get("mode")):
            _fail("release.manifest", "installed library identity mismatch: " + library)


def check_deployment(path):
    """Return a nonsecret structural check summary, never a runtime READY claim."""
    config = load_deployment(path)
    encoded = json.dumps(config, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return {
        "schema": "ndnsf-di-deployment-check-v1",
        "status": "CONFIGURATION_CHECKED",
        "deployment_id": config["deployment_id"],
        "configuration_digest": "sha256:" + hashlib.sha256(encoded).hexdigest(),
        "platform": config["platform"]["kind"],
        "nodes": len(config["nodes"]),
        "owned_actors": sum(not actor["external"] for actor in config["actors"]),
        "external_actors": sum(actor["external"] for actor in config["actors"]),
        "native_admission": "NOT_RUN",
        "readiness": "NOT_RUN",
    }
