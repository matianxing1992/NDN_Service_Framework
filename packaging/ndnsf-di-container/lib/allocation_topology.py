"""Frozen Spec 110 allocation topology validation and rendering."""

from __future__ import annotations

import hashlib
import ipaddress
import json
from pathlib import Path
import re
import shlex
from typing import Any, Mapping


class TopologyError(ValueError):
    pass


DIGEST = re.compile(r"^sha256:[a-f0-9]{64}$")
SAFE_TOKEN = re.compile(r"^[A-Za-z0-9_./:@+=,-]+$")
PROCESS_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
HOST_BOUND_PREFIXES = ("/home/", "/project/", "/workspace/", "/build/", "/src/", "/tmp/")
READINESS_PHASES = (
    "scratch-binds-gpu",
    "nfd",
    "routes",
    "controller",
    "providers",
    "user",
    "candidate",
)


def command_digest(command: list[str]) -> str:
    encoded = json.dumps(command, separators=(",", ":")).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def _fail(code: str, detail: object = "") -> None:
    raise TopologyError(code + (":" + str(detail) if detail != "" else ""))


def _safe_command(command: object, process_id: str) -> list[str]:
    if not isinstance(command, list) or not command:
        _fail("TOPOLOGY_COMMAND_INVALID", process_id)
    if any(not isinstance(token, str) or not SAFE_TOKEN.fullmatch(token) for token in command):
        _fail("TOPOLOGY_COMMAND_UNSAFE", process_id)
    return command


def _validate_nfd_config_argv(command: list[str], process_id: str) -> None:
    config_positions = [index for index, token in enumerate(command) if token == "--config"]
    inline_config = [token for token in command if token.startswith("--config=")]
    if len(config_positions) + len(inline_config) != 1 or any(
            index + 1 >= len(command) or command[index + 1].startswith("-")
            for index in config_positions):
        _fail("TOPOLOGY_NFD_CONFIG_INVALID", process_id)


def _validate_runtime_command_paths(command: list[str], process_id: str,
                                    identity: str | None, kind: str) -> None:
    """Reject host/build paths that would diverge on a compute node.

    Process-map commands are executed after the launcher changes HOME and CWD.
    A path from the submit host must therefore never remain in an application
    argument. The one intentional exception is the declared identity source,
    which is copied and rewritten to runtime HOME; NFD config paths are also
    rewritten to the per-job scratch copy.
    """
    for index, token in enumerate(command):
        if kind == "nfd" and (token.startswith("--config=") or
                               (index > 0 and command[index - 1] == "--config")):
            continue
        if identity is not None and (token == identity or
                                     token == "--identity=" + identity):
            continue
        candidate = token.split("=", 1)[1] if "=" in token else token
        if any(candidate == prefix[:-1] or candidate.startswith(prefix)
               for prefix in HOST_BOUND_PREFIXES):
            _fail("TOPOLOGY_HOST_PATH_COMMAND_INVALID", process_id)


def _render_launcher_command(command: list[str], identity: str | None,
                             runtime_config: str | None = None) -> str:
    """Render argv while rebinding an explicit identity path to runtime HOME.

    v1 process maps historically carried the read-only ``identityRef`` as an
    executable argument (for example ``--identity /project/...``). Keeping
    that path would make a multi-node process reopen shared storage and would
    bypass the per-process PIB/TPM copy prepared by the launcher. Only an
    exact token is rewritten; all other argv tokens remain digest-bound to the
    frozen process map.
    """
    rendered: list[str] = []
    for index, token in enumerate(command):
        if identity is not None and token == identity:
            rendered.append('"$runtime_home"')
        elif identity is not None and token == "--identity=" + identity:
            rendered.append('--identity="$runtime_home"')
        elif runtime_config is not None and index > 0 and command[index - 1] == "--config":
            rendered.append('"$runtime_config"')
        elif runtime_config is not None and token.startswith("--config="):
            rendered.append('--config="$runtime_config"')
        else:
            rendered.append(shlex.quote(token))
    return "exec " + " ".join(rendered)


def validate_process_map(value: Mapping[str, Any]) -> dict[str, Any]:
    required = {
        "schemaVersion", "placementClass", "selectedTransport", "nodes", "processes",
        "routes", "readinessPhases", "signals", "zeroSurvivorAudit",
    }
    if not isinstance(value, Mapping) or set(value) != required:
        _fail("TOPOLOGY_FIELDS_INVALID")
    if value["schemaVersion"] != "spec110-process-map-v1":
        _fail("TOPOLOGY_SCHEMA_INVALID")
    placement = value["placementClass"]
    if placement not in {"single-node-multi-gpu", "multi-node"}:
        _fail("TOPOLOGY_PLACEMENT_INVALID")
    selected = value["selectedTransport"]
    if selected not in {"tcp", "udp"}:
        _fail("TOPOLOGY_TRANSPORT_INVALID")
    if value["readinessPhases"] != list(READINESS_PHASES):
        _fail("TOPOLOGY_READINESS_ORDER_INVALID")
    if sorted(value["signals"]) != ["INT", "TERM"] or value["zeroSurvivorAudit"] is not True:
        _fail("TOPOLOGY_TEARDOWN_POLICY_INVALID")

    nodes = value["nodes"]
    if not isinstance(nodes, list) or not nodes:
        _fail("TOPOLOGY_NODES_INVALID")
    node_ranks: set[int] = set()
    node_names: set[str] = set()
    tcp_endpoints: set[tuple[str, int]] = set()
    udp_endpoints: set[tuple[str, int]] = set()
    for node in nodes:
        if not isinstance(node, Mapping) or set(node) != {"nodeRank", "name", "address", "nfdSocket", "tcpPort", "udpPort"}:
            _fail("TOPOLOGY_NODE_FIELDS_INVALID")
        rank = node["nodeRank"]
        if not isinstance(rank, int) or rank < 0 or rank in node_ranks:
            _fail("TOPOLOGY_NODE_RANK_INVALID", rank)
        if not isinstance(node["name"], str) or not SAFE_TOKEN.fullmatch(node["name"]) or node["name"] in node_names:
            _fail("TOPOLOGY_NODE_NAME_INVALID")
        try:
            address = ipaddress.ip_address(node["address"])
        except ValueError:
            _fail("TOPOLOGY_NODE_ADDRESS_INVALID", node["name"])
        # The v1 NFD template and route launcher explicitly use *4 URIs.
        # Accepting IPv6 or per-namespace/unspecified addresses would pass
        # validation but make a multi-node route target the wrong endpoint.
        if (not isinstance(address, ipaddress.IPv4Address) or
                address.is_unspecified or address.is_multicast or
                (placement == "multi-node" and (address.is_loopback or address.is_link_local))):
            _fail("TOPOLOGY_NODE_ADDRESS_INVALID", node["name"])
        if (not str(node["nfdSocket"]).startswith("/tmp/ndnsf-di-") or
                ".." in Path(node["nfdSocket"]).parts):
            _fail("TOPOLOGY_NFD_SOCKET_INVALID", node["name"])
        if any(not isinstance(node[key], int) or not 1024 <= node[key] <= 65535 for key in ("tcpPort", "udpPort")):
            _fail("TOPOLOGY_PORT_INVALID", node["name"])
        tcp_endpoint = (str(address), node["tcpPort"])
        udp_endpoint = (str(address), node["udpPort"])
        if tcp_endpoint in tcp_endpoints or udp_endpoint in udp_endpoints:
            _fail("TOPOLOGY_PORT_ENDPOINT_DUPLICATE", node["name"])
        tcp_endpoints.add(tcp_endpoint)
        udp_endpoints.add(udp_endpoint)
        node_ranks.add(rank)
        node_names.add(node["name"])
    if node_ranks != set(range(len(nodes))):
        _fail("TOPOLOGY_NODE_RANK_NOT_DENSE")
    if placement == "single-node-multi-gpu" and len(nodes) != 1:
        _fail("TOPOLOGY_SINGLE_NODE_COUNT_INVALID")
    if placement == "multi-node" and len(nodes) < 2:
        _fail("TOPOLOGY_MULTINODE_COUNT_INVALID")

    processes = value["processes"]
    if not isinstance(processes, list):
        _fail("TOPOLOGY_PROCESSES_INVALID")
    exact_fields = {
        "processId", "kind", "role", "identityRef", "identityReadOnly", "nodeRank",
        "taskRank", "gpuRank", "gpuUuid", "nfdSocket", "command", "commandDigest",
        "readinessInputs", "readinessOutput", "shutdownOrder",
    }
    ids: set[str] = set()
    task_ranks: set[int] = set()
    identities: set[str] = set()
    outputs: set[str] = {"scratch-binds-gpu-ready"}
    kind_counts = {"nfd": 0, "controller": 0, "user": 0, "provider": 0}
    nfd_nodes: set[int] = set()
    provider_gpus: set[str] = set()
    provider_slots: set[tuple[int, int]] = set()
    for process in processes:
        if not isinstance(process, Mapping) or set(process) != exact_fields:
            _fail("TOPOLOGY_PROCESS_FIELDS_INVALID")
        process_id = process["processId"]
        if not isinstance(process_id, str) or not PROCESS_ID.fullmatch(process_id) or process_id in ids:
            _fail("TOPOLOGY_PROCESS_ID_INVALID")
        ids.add(process_id)
        kind = process["kind"]
        if kind not in kind_counts:
            _fail("TOPOLOGY_PROCESS_KIND_INVALID", process_id)
        kind_counts[kind] += 1
        node_rank = process["nodeRank"]
        if node_rank not in node_ranks:
            _fail("TOPOLOGY_PROCESS_NODE_INVALID", process_id)
        task_rank = process["taskRank"]
        if not isinstance(task_rank, int) or task_rank < 0 or task_rank in task_ranks:
            _fail("TOPOLOGY_TASK_RANK_INVALID", process_id)
        task_ranks.add(task_rank)
        if process["nfdSocket"] != nodes[node_rank]["nfdSocket"]:
            _fail("TOPOLOGY_SOCKET_BINDING_INVALID", process_id)
        command = _safe_command(process["command"], process_id)
        if process["commandDigest"] != command_digest(command):
            _fail("TOPOLOGY_COMMAND_DIGEST_INVALID", process_id)
        inputs = process["readinessInputs"]
        if not isinstance(inputs, list) or any(item not in outputs for item in inputs):
            _fail("TOPOLOGY_READINESS_DEPENDENCY_INVALID", process_id)
        output = process["readinessOutput"]
        if not isinstance(output, str) or not SAFE_TOKEN.fullmatch(output) or output in outputs:
            _fail("TOPOLOGY_READINESS_OUTPUT_INVALID", process_id)
        outputs.add(output)
        if not isinstance(process["shutdownOrder"], int) or process["shutdownOrder"] < 1:
            _fail("TOPOLOGY_SHUTDOWN_ORDER_INVALID", process_id)
        if kind == "nfd":
            if process["identityRef"] is not None or process["identityReadOnly"] is not True:
                _fail("TOPOLOGY_NFD_IDENTITY_INVALID", process_id)
            _validate_nfd_config_argv(command, process_id)
            _validate_runtime_command_paths(command, process_id, None, kind)
            if node_rank in nfd_nodes:
                _fail("TOPOLOGY_DUPLICATE_NFD", node_rank)
            nfd_nodes.add(node_rank)
            if process["gpuRank"] is not None or process["gpuUuid"] is not None:
                _fail("TOPOLOGY_NFD_GPU_INVALID", process_id)
        else:
            identity = process["identityRef"]
            if (not isinstance(identity, str) or not identity.startswith("/project/") or
                    ".." in Path(identity).parts or
                    any(char in identity for char in "\x00\n\r") or
                    process["identityReadOnly"] is not True):
                _fail("TOPOLOGY_IDENTITY_BINDING_INVALID", process_id)
            _validate_runtime_command_paths(command, process_id, identity, kind)
            if identity in identities:
                _fail("TOPOLOGY_DUPLICATE_IDENTITY", identity)
            identities.add(identity)
            if kind == "provider":
                if not isinstance(process["gpuRank"], int) or not isinstance(process["gpuUuid"], str):
                    _fail("TOPOLOGY_PROVIDER_GPU_INVALID", process_id)
                if process["gpuUuid"] in provider_gpus:
                    _fail("TOPOLOGY_DUPLICATE_GPU", process["gpuUuid"])
                slot = (node_rank, process["gpuRank"])
                if slot in provider_slots:
                    _fail("TOPOLOGY_DUPLICATE_GPU_SLOT", slot)
                provider_gpus.add(process["gpuUuid"])
                provider_slots.add(slot)
            elif process["gpuRank"] is not None or process["gpuUuid"] is not None:
                _fail("TOPOLOGY_CONTROL_GPU_INVALID", process_id)
    if nfd_nodes != node_ranks:
        _fail("TOPOLOGY_ONE_NFD_PER_NODE_REQUIRED")
    if kind_counts != {"nfd": len(nodes), "controller": 1, "user": 1, "provider": 3}:
        _fail("TOPOLOGY_ROLE_GRAPH_INVALID", kind_counts)
    if len(task_ranks) != len(processes) or task_ranks != set(range(len(processes))):
        _fail("TOPOLOGY_TASK_RANK_NOT_DENSE")
    if len({process["shutdownOrder"] for process in processes}) != len(processes):
        _fail("TOPOLOGY_SHUTDOWN_ORDER_DUPLICATE")

    routes = value["routes"]
    if not isinstance(routes, list):
        _fail("TOPOLOGY_ROUTES_INVALID")
    for route in routes:
        fields = {"fromNodeRank", "toNodeRank", "prefix", "transport", "remoteAddress", "port"}
        if not isinstance(route, Mapping) or set(route) != fields:
            _fail("TOPOLOGY_ROUTE_FIELDS_INVALID")
        if route["fromNodeRank"] not in node_ranks or route["toNodeRank"] not in node_ranks or route["fromNodeRank"] == route["toNodeRank"]:
            _fail("TOPOLOGY_ROUTE_NODE_INVALID")
        target = nodes[route["toNodeRank"]]
        if route["transport"] != selected or route["remoteAddress"] != target["address"] or route["port"] != target[selected + "Port"]:
            _fail("TOPOLOGY_ROUTE_BINDING_INVALID")
        if not isinstance(route["prefix"], str) or not route["prefix"].startswith("/"):
            _fail("TOPOLOGY_ROUTE_PREFIX_INVALID")
    if placement == "single-node-multi-gpu" and routes:
        _fail("TOPOLOGY_SINGLE_NODE_ROUTE_FORBIDDEN")
    if placement == "multi-node" and not routes:
        _fail("TOPOLOGY_CROSS_NODE_ROUTE_REQUIRED")
    return dict(value)


def render_nfd_config(template: str, node: Mapping[str, Any], state_dir: str) -> str:
    values = {
        "NODE_RANK": node["nodeRank"], "NFD_SOCKET": node["nfdSocket"],
        "TCP_PORT": node["tcpPort"], "UDP_PORT": node["udpPort"], "STATE_DIR": state_dir,
    }
    rendered = template
    for key, value in values.items():
        rendered = rendered.replace("@@" + key + "@@", str(value))
    if ("@@" in rendered or
            not str(state_dir).startswith("/tmp/ndnsf-di-") or
            ".." in Path(state_dir).parts or
            any(char in str(state_dir) for char in "\x00\n\r")):
        _fail("TOPOLOGY_NFD_TEMPLATE_INVALID")
    return rendered


def render_multiprog(value: Mapping[str, Any]) -> str:
    process_map = validate_process_map(value)
    return "\n".join(
        f"{process['taskRank']} {shlex.join(process['command'])}"
        for process in sorted(process_map["processes"], key=lambda item: item["taskRank"])
    ) + "\n"


def render_process_launcher(process: Mapping[str, Any], scratch: Path | str,
                            workdir: Path | str) -> str:
    """Render a deterministic per-process launcher for an allocation node.

    ``workdir`` is an explicit shared bundle/config directory.  ``identityRef``
    is a read-only source bundle, not a writable NDN keychain.
    Copying it into a process-specific scratch HOME avoids shared PIB/TPM
    locks and prevents a Slurm/login environment from silently selecting a
    different role.  NFD has no identity in the v1 map, so its inherited NDN
    locators are cleared as well.
    """
    process_id = process.get("processId")
    kind = process.get("kind")
    socket_path = process.get("nfdSocket")
    command = process.get("command")
    if not isinstance(process_id, str) or not PROCESS_ID.fullmatch(process_id):
        _fail("TOPOLOGY_PROCESS_ID_INVALID", process_id)
    if kind not in {"nfd", "controller", "user", "provider"}:
        _fail("TOPOLOGY_PROCESS_KIND_INVALID", process_id)
    if (not isinstance(socket_path, str) or not socket_path.startswith("/tmp/ndnsf-di-") or
            ".." in Path(socket_path).parts):
        _fail("TOPOLOGY_NFD_SOCKET_INVALID", process_id)
    command = _safe_command(command, process_id)
    if kind == "nfd":
        if process.get("identityRef") is not None or process.get("identityReadOnly") is not True:
            _fail("TOPOLOGY_NFD_IDENTITY_INVALID", process_id)
        _validate_nfd_config_argv(command, process_id)
    elif process.get("identityReadOnly") is not True:
        _fail("TOPOLOGY_IDENTITY_BINDING_INVALID", process_id)
    identity: str | None = None
    if kind != "nfd":
        identity = process.get("identityRef")
        if (not isinstance(identity, str) or not identity.startswith("/project/") or
                ".." in Path(identity).parts or
                any(char in identity for char in "\x00\n\r")):
            _fail("TOPOLOGY_IDENTITY_BINDING_INVALID", process_id)
    _validate_runtime_command_paths(command, process_id, identity, kind)
    if kind == "provider":
        gpu_uuid = process.get("gpuUuid")
        if not isinstance(gpu_uuid, str) or not gpu_uuid:
            _fail("TOPOLOGY_PROVIDER_GPU_INVALID", process_id)
    scratch_path = Path(scratch)
    if (not scratch_path.is_absolute() or not str(scratch_path).startswith("/tmp/ndnsf-di-") or
            ".." in scratch_path.parts):
        _fail("TOPOLOGY_SCRATCH_INVALID", scratch)
    workdir_path = Path(workdir)
    if (not workdir_path.is_absolute() or ".." in workdir_path.parts or
            any(char in str(workdir_path) for char in "\x00\n\r")):
        _fail("TOPOLOGY_WORKDIR_INVALID", workdir)
    if not str(socket_path).startswith(str(scratch_path).rstrip("/") + "/"):
        _fail("TOPOLOGY_SOCKET_SCOPE_INVALID", process_id)

    home = scratch_path / "homes" / process_id
    tmp = scratch_path / "tmp" / process_id
    runtime_config = (scratch_path / "generated" / (process_id + ".conf")) if kind == "nfd" else None
    lines = ["#!/bin/bash", "set -euo pipefail", "umask 077"]
    lines += [
        f"runtime_home={shlex.quote(str(home))}",
        f"runtime_tmp={shlex.quote(str(tmp))}",
        'rm -rf "$runtime_home" "$runtime_tmp"',
        'mkdir -p "$runtime_home" "$runtime_tmp"',
        'chmod 700 "$runtime_home" "$runtime_tmp"',
        'unset NDN_CLIENT_PIB NDN_CLIENT_TPM',
        'export HOME="$runtime_home" TMPDIR="$runtime_tmp"',
        f"runtime_workdir={shlex.quote(str(workdir_path))}",
        'test -d "$runtime_workdir" || { echo SPEC110_WORKDIR_MISSING >&2; exit 8; }',
        'cd "$runtime_workdir"',
    ]
    if kind == "nfd":
        lines.append(f"runtime_config={shlex.quote(str(runtime_config))}")
        lines.append('printf \'SPEC110_PROCESS_HOME_READY process=%s kind=nfd home=%s\\n\' ' +
                     f"{shlex.quote(process_id)} \"$HOME\" >&2")
    else:
        lines += [
            f"identity_source={shlex.quote(identity)}",
            'if [[ ${NDNSF_SPEC110_TEST_MODE:-0} == 1 && ! -d "$identity_source/.ndn" ]]; then',
            '  printf \'SPEC110_TEST_IDENTITY_SOURCE_BYPASS process=%s source=%s\\n\' ' +
            f"{shlex.quote(process_id)} \"$identity_source\" >&2",
            'else',
            '  test -d "$identity_source/.ndn" || { echo SPEC110_IDENTITY_SOURCE_MISSING >&2; exit 8; }',
            '  identity_link=$(find "$identity_source/.ndn" -type l -print -quit)',
            '  [[ -z "$identity_link" ]] || { echo SPEC110_IDENTITY_SYMLINK_FORBIDDEN >&2; exit 8; }',
            '  cp -a "$identity_source/." "$runtime_home/"',
            '  test -f "$runtime_home/.ndn/pib.db" || { echo SPEC110_IDENTITY_PIB_MISSING >&2; exit 8; }',
            '  test -e "$runtime_home/.ndn/ndnsec-key-file" || { echo SPEC110_IDENTITY_TPM_MISSING >&2; exit 8; }',
            '  export NDN_CLIENT_PIB="pib-sqlite3:$runtime_home/.ndn/pib.db"',
            '  export NDN_CLIENT_TPM="tpm-file:$runtime_home/.ndn/ndnsec-key-file"',
            'fi',
            'printf \'SPEC110_PROCESS_HOME_READY process=%s kind=%s source=%s home=%s\\n\' ' +
            f"{shlex.quote(process_id)} {shlex.quote(kind)} \"$identity_source\" \"$HOME\" >&2",
        ]
    if kind == "provider":
        gpu_uuid = process["gpuUuid"]
        lines += [
            f"expected_gpu_uuid={shlex.quote(gpu_uuid)}",
            'if [[ ${NDNSF_SPEC110_TEST_MODE:-0} != 1 ]]; then',
            '  command -v nvidia-smi >/dev/null 2>&1 || { echo SPEC110_GPU_PROBE_MISSING >&2; exit 8; }',
            '  visible_gpu=${CUDA_VISIBLE_DEVICES:-}',
            '  [[ -n "$visible_gpu" && "$visible_gpu" != *,* && "$visible_gpu" != *[[:space:]]* ]] || { echo SPEC110_GPU_BINDING_MISSING >&2; exit 8; }',
            '  if ! observed_gpu_uuids=$(nvidia-smi -i "$visible_gpu" --query-gpu=uuid --format=csv,noheader,nounits | tr -d "\\r" | sed "/^[[:space:]]*$/d"); then',
            '    echo SPEC110_GPU_QUERY_FAILED >&2; exit 8;',
            '  fi',
            '  [[ "$observed_gpu_uuids" == "$expected_gpu_uuid" ]] || { echo SPEC110_GPU_UUID_MISMATCH >&2; exit 8; }',
            '  printf \'SPEC110_GPU_UUID_READY process=%s uuid=%s\\n\' ' +
            f"{shlex.quote(process_id)} \"$expected_gpu_uuid\" >&2",
            'fi',
        ]
    lines += [
        f"export NDN_CLIENT_TRANSPORT={shlex.quote('unix://' + socket_path)}",
        _render_launcher_command(command, identity, runtime_config),
        "",
    ]
    return "\n".join(lines)


def evaluate_transport_probe(process_map: Mapping[str, Any], observations: Mapping[str, Any]) -> dict[str, Any]:
    validated = validate_process_map(process_map)
    selected = validated["selectedTransport"]
    required = {"allocationAddresses", "tcp", "udp"}
    if not isinstance(observations, Mapping) or set(observations) != required:
        _fail("TOPOLOGY_PROBE_FIELDS_INVALID")
    expected = {node["address"] for node in validated["nodes"]}
    if set(observations["allocationAddresses"]) != expected:
        _fail("TOPOLOGY_PROBE_ADDRESS_MISMATCH")
    for transport in ("tcp", "udp"):
        row = observations[transport]
        if not isinstance(row, Mapping) or set(row) != {"status", "closedPorts", "reachableRoutes"}:
            _fail("TOPOLOGY_PROBE_RESULT_INVALID", transport)
    selected_row = observations[selected]
    if selected_row["status"] != "PASS" or selected_row["closedPorts"]:
        _fail("TOPOLOGY_SELECTED_TRANSPORT_FAILED", selected)
    return {
        "status": "PASS", "selectedTransport": selected,
        "selectedStatus": "PASS", "diagnosticTransport": "udp" if selected == "tcp" else "tcp",
        "diagnosticStatus": observations["udp" if selected == "tcp" else "tcp"]["status"],
    }


def load_process_map(path: Path | str) -> dict[str, Any]:
    source = Path(path)
    try:
        if source.suffix in {".yaml", ".yml"}:
            import yaml
            value = yaml.safe_load(source.read_text(encoding="utf-8"))
        else:
            value = json.loads(source.read_text(encoding="utf-8"))
    except Exception as exc:
        raise TopologyError("TOPOLOGY_READ_FAILED:" + str(exc)) from exc
    return validate_process_map(value)


__all__ = [
    "TopologyError", "command_digest", "evaluate_transport_probe", "load_process_map",
    "render_multiprog", "render_nfd_config", "render_process_launcher",
    "validate_process_map",
]
