#!/usr/bin/env python3
"""Qualify the Spec 261 component or hybrid split across two MiniNDN Providers.

The maintained process driver owns the catalog, credentials, protocol and
oracle. This runner only replaces the transport: each actor executes inside its
MiniNDN node's network namespace and talks to that node's own NFD. The
dependency handoff crosses the emulated links.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
from urllib.parse import unquote
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
TOPOLOGY = ROOT / "Experiments/Topology/spec261-hybrid.conf"
WIRED_TOPOLOGY = ROOT / "Experiments/Topology/spec270-wired-star.conf"
DRIVER = ROOT / "tests/standalone/run-spec261-yolo-twoprovider-process.py"
ROLE_NODE = {
    "requester": "requester",
    "provider": "provider",
    "provider2": "provider2",
    "controller": "control",
    "authority": "control",
}
WIRED_ROLE_NODE = {
    "requester": "requester",
    "provider": "provider0",
    "provider2": "provider1",
    "controller": "requester",
    "authority": "requester",
}
ACTOR_PREFIX = {
    "requester": "/example/hello/user",
    "provider": "/example/hello/provider",
    "provider2": "/example/hello/provider2",
    "controller": "/example/hello/controller",
    "authority": "/example/hello/authority",
}


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def check_ffn_handoffs(run_root: Path, requester_log: str, warm: bool,
                       block_edges=None) -> dict:
    selection = "NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId="
    sessions = [line.split(selection, 1)[1].split()[0]
                for line in requester_log.splitlines() if selection in line]
    if len(sessions) != 1:
        raise RuntimeError("hybrid run did not seal exactly one request")
    session = sessions[0]
    owners = {"Prefix": "provider", "Rank0": "provider",
              "Rank1": "provider2", "Suffix": "provider2"}
    edges = [
        ("Prefix", "Rank0", "Prefix/RUN", "hidden", 66),
        ("Prefix", "Rank1", "Prefix/RUN", "hidden", 66),
        ("Rank1", "Rank0", "Rank1/RUN/0", "mid", 67),
        ("Rank0", "Rank1", "Rank0/RUN/0", "mid", 67),
        ("Rank1", "Rank0", "Rank1/RUN/1", "last", 64),
        ("Rank0", "Suffix", "Rank0/SUM/1", "last", 64),
        ("Prefix", "Suffix", "Prefix/RUN", "hidden", 66),
    ]
    if block_edges is not None:
        edges = block_edges
    rows = []
    fetch_events = {"Rank0": [], "Rank1": []}
    middle_cache = {}
    terminals = []
    marker = "NDNSF_DI_DEPENDENCY_OBJECT "
    for file in ("provider.log", "provider2.log", "provider2-fault.log"):
        path = run_root / file
        if not path.exists():
            continue
        node = path.stem.split("-", 1)[0]
        with path.open(errors="replace") as log:
            for line in log:
                if marker in line and f" session={session} " in line:
                    fields = dict(field.split("=", 1)
                                  for field in line.split(marker, 1)[1].split())
                    if fields.get("status") == "ok":
                        rows.append((node, fields))
                        role = fields.get("consumer")
                        if (role in fetch_events and node == owners[role] and
                                fields.get("direction") == "fetch-exact-ndn"):
                            fetch_events[role].append(("validated", fields["planned_name"]))
                elif ("stage=DEPENDENCY_FETCH status=begin " in line and
                      f" sessionId={session} " in line):
                    fields = dict(field.split("=", 1) for field in
                                  line.split("status=begin ", 1)[1].split())
                    role = fields["role"]
                    if role in fetch_events and node == owners[role]:
                        fetch_events[role].append(("begin", fields["plannedName"]))
                elif f"stage=TERMINAL status=observed requestId={session} " in line:
                    fields = dict(field.split("=", 1) for field in
                                  line.split("status=observed ", 1)[1].split())
                    terminals.append((node, fields["provider"], fields["role"]))
                elif ("NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_ACQUIRE_DONE " in line and
                      f" requestId={session} " in line):
                    role = "Rank0" if node == "provider" else "Rank1"
                    if f" role={role} " in line:
                        middle_cache[role] = line.split(" detail=", 1)[1].split()[0]
    verified = []
    for producer, consumer, step, port, size in edges:
        pair = [row for node, row in rows if row.get("producer") == producer and
                row.get("consumer") == consumer and
                f"/8=step/8={step.replace('/', '%2F')}/" in row.get("planned_name", "") and
                f"/8=port/8={port.replace('/', '%2F')}/" in row["planned_name"] and
                row.get("encoded_payload_bytes") == str(size) and
                ((node == owners[producer] and row["direction"] == "publish-exact-ndn") or
                 (node == owners[consumer] and row["direction"] == "fetch-exact-ndn"))]
        if block_edges is not None and (producer, consumer) in (
                ("Prefix", "Rank0"), ("Prefix", "Rank1")):
            # The same latents can feed the first LayerNorm and its residual.
            names = {row["planned_name"] for row in pair}
            if not names or any({row["direction"] for row in pair
                                  if row["planned_name"] == name} !=
                                 {"publish-exact-ndn", "fetch-exact-ndn"} or
                                not name.startswith(f"/8=example/8=hello/8={owners[producer]}/")
                                for name in names):
                raise RuntimeError(f"missing signed prefix handoff to {consumer}")
            for name in sorted(names):
                verified.append({"producer": producer, "consumer": consumer, "step": step,
                                 "port": port, "bytes": size, "name": name})
            continue
        if ({row["direction"] for row in pair} != {"publish-exact-ndn", "fetch-exact-ndn"} or
                len({row["planned_name"] for row in pair}) != 1 or
                not pair[0]["planned_name"].startswith(
                    f"/8=example/8=hello/8={owners[producer]}/")):
            raise RuntimeError(f"missing signed handoff from {producer} to {consumer} for {step}")
        verified.append({"producer": producer, "consumer": consumer, "step": step, "port": port,
                         "bytes": size, "name": pair[0]["planned_name"]})
    expected_cache = {"template-hit", "assembled-disk-hit"} if warm else {"assembled-built"}
    if set(middle_cache) != {"Rank0", "Rank1"} or not set(middle_cache.values()) <= expected_cache:
        raise RuntimeError(f"unexpected middle-rank artifact cache path: {middle_cache}")
    for role, actual in fetch_events.items():
        expected = [(event, edge["name"]) for edge in verified if edge["consumer"] == role
                    for event in ("begin", "validated")]
        if block_edges is not None:
            # A bypass can be consumed by multiple distinct CALLs. Across all
            # steps the worker still admits one remote operand at a time.
            started = [name for operation, name in actual if operation == "begin"]
            paired = [(event, name) for name in started for event in ("begin", "validated")]
            if actual != paired or sorted(actual) != sorted(expected):
                raise RuntimeError(f"{role} has an unmatched or out-of-order remote read")
        elif actual != expected:
            raise RuntimeError(f"{role} did not fetch only the active step's remote operand")
    count = 128 if block_edges is not None else 3
    if (terminals != [("provider2", "/example/hello/provider2", "Suffix")] or
            f"NATIVE_NUMERICAL_ORACLE_PASS tensor=y values={count}" not in requester_log.splitlines()):
        raise RuntimeError("hybrid run did not return the verified y output from Suffix")
    return {"request": session, "handoffs": verified, "middleCache": middle_cache,
            "middleFetchOrder": fetch_events, "terminalRole": "Suffix", "outputTensor": "y"}


def check_block_handoffs(run_root: Path, requester_log: str, warm: bool) -> dict:
    def tensor_bytes(name, elements):
        return 56 + len(name) + 4 * elements

    edges = [("Prefix", rank, "Prefix/RUN", "prefix", tensor_bytes("prefix", 128))
             for rank in ("Rank0", "Rank1")]
    for block in range(2):
        base = 16 * block
        for branch, offset in (("self", 0), ("cross", 7)):
            name = f"block-{block}/{branch}"
            phases = ((2, name + "/norm-q/mean", 2),
                      (4, name + "/norm-k/mean", 2 if branch == "self" else 3),
                      (6, name + "/o/projection", 128))
            for phase, port, elements in phases:
                for source, target in (("Rank0", "Rank1"), ("Rank1", "Rank0")):
                    edges.append((source, target, f"{source}/RUN/{base + offset + phase}",
                                  port, tensor_bytes(port, elements)))
        port = f"block-{block}/down/projection"
        edges.append(("Rank1", "Rank0", f"Rank1/RUN/{base + 15}",
                      port, tensor_bytes(port, 128)))
        if block == 0:
            edges.append(("Rank0", "Rank1", "Rank0/RUN/15", port, tensor_bytes(port, 128)))
    port = "block-1/residual"
    edges.append(("Rank0", "Suffix", "Rank0/RUN/32", port, tensor_bytes(port, 128)))
    return check_ffn_handoffs(run_root, requester_log, warm, edges)


def check_invocation_wire(requester_log: str, session: str, roles) -> dict:
    marker = f"NDNSF_DI_INVOCATION_WIRE requestId={session} "
    sizes = {}
    for line in requester_log.splitlines():
        if marker not in line:
            continue
        fields = dict(field.split("=", 1) for field in line.split(marker, 1)[1].split())
        role = fields.pop("role")
        if role in sizes or fields.pop("version") != "2":
            raise RuntimeError(f"{role} lacks a unique v2 invocation measurement")
        sizes[role] = {name: int(value) for name, value in fields.items()}
        compact = sizes[role]["wire_bytes"]
        expanded = sizes[role]["expanded_endpoint_wire_bytes"]
        if not 0 < min(compact, expanded) or max(compact, expanded) > (4 << 20):
            raise RuntimeError(f"{role} invocation exceeds the native reader bound")
    if set(sizes) != set(roles):
        raise RuntimeError(f"invocation measurements missing for {set(roles) - sizes.keys()}")
    return sizes


def check_wan_fixed_role_templates(run_root: Path) -> dict:
    root = run_root / "requester" / "encrypted-repo" / "manifests"
    prefix = "/8=example/8=hello/8=user/8=NDNSF-DI/8=assignment/"
    roles = {"Prefix": [], "Rank0": [], "Rank1": [], "Suffix": []}
    for path in root.glob("*.json"):
        manifest = json.loads(path.read_text())
        name = manifest.get("dataPrefix", "")
        if not name.startswith(prefix):
            continue
        role = unquote(name.split("/8=role/8=name%3D", 1)[1].split("/", 1)[0])
        plaintext = manifest["plaintextSize"]
        if (role not in roles or manifest["lifecycleState"] != "COMMITTED" or
                manifest["objectType"] != "original-packet-set" or
                not 0 < plaintext <= (4 << 20)):
            raise RuntimeError(f"invalid stored fixed-role source for {role}")
        roles[role].append({
            "digest": manifest["contentDigest"], "plaintextBytes": plaintext,
            "originalPacketSetBytes": manifest["size"], "segments": manifest["segmentCount"],
        })
    if {role: len(items) for role, items in roles.items()} != {
            "Prefix": 2, "Rank0": 1, "Rank1": 1, "Suffix": 1}:
        raise RuntimeError("fixed-role Repo inventory differs from one full and one hybrid publication")
    return {role: sorted(rows, key=lambda item: item["digest"]) for role, rows in roles.items()}


def check_wan_artifacts(run_root: Path, owners: dict, selection: str) -> dict:
    selected = json.loads(selection.split(" manifestJson=", 1)[1])
    manifest_digest = selection.split(" manifestDigest=", 1)[1].split()[0]
    source_digest = selected["metadata"]["canonicalSourceDigest"]
    source_sidecar_digest = selected["metadata"]["canonicalInitializerObjectDigest"]
    prefix_cache = run_root / owners["Prefix"] / "cache" / "assembled" / "Prefix"
    full_manifests = []
    for path in prefix_cache.glob("*/manifest.json"):
        manifest = json.loads(path.read_text())
        if manifest["artifactCoordinate"] == "/graph/full/entry/region/profile/whole":
            full_manifests.append((path.parent, manifest))
    if len(full_manifests) != 1:
        raise RuntimeError("Wan lacks a unique uncut reference artifact")
    full_dir, full = full_manifests[0]
    if (full["nodeCount"] != 3006 or full["canonicalSourceDigest"] != source_digest or
            full["assembledModelDigest"] != "sha256:" + digest(full_dir / "model.onnx") or
            full["externalInitializerDigest"] != source_sidecar_digest):
        raise RuntimeError("Wan uncut artifact differs from the hybrid source")
    # The assembly contract identifies the initializer inventory. Its digest
    # differs from the serialized sidecar's file digest.
    initializer_digest = full["canonicalInitializerDigest"]
    artifacts = {}
    for role, node in owners.items():
        paths = (run_root / node / "cache" / "assembled" / role).glob("*/manifest.json")
        matches = []
        for path in paths:
            manifest = json.loads(path.read_text())
            if manifest["modelManifestDigest"] == manifest_digest:
                matches.append((path, manifest))
        if len(matches) != 1:
            raise RuntimeError(f"{role} lacks a unique artifact for the cold Selection")
        path, manifest = matches[0]
        if (manifest["role"] != role or manifest["signer"] != f"/example/hello/{node}" or
                manifest["canonicalSourceDigest"] != source_digest or
                manifest["canonicalInitializerDigest"] != initializer_digest or
                not (path.parent / "manifest.signature").stat().st_size):
            raise RuntimeError(f"{role} artifact differs from its Selection")
        artifacts[role] = {"recipeDigest": manifest["recipeDigest"],
                           "artifactManifestSha256": digest(path)}
        if role in ("Prefix", "Suffix"):
            expected_nodes = {"Prefix": 2697, "Suffix": 111}
            graph = path.parent / "model.onnx"
            initializer = path.parent / "model.onnx.data"
            if (manifest["roleKind"] != "COMPONENT_SET" or
                    int(manifest["nodeCount"]) != expected_nodes[role] or
                    graph.stat().st_size != manifest["assembledModelBytes"] or
                    "sha256:" + digest(graph) != manifest["assembledModelDigest"] or
                    initializer.stat().st_size != manifest["externalInitializerBytes"] or
                    "sha256:" + digest(initializer) != manifest["externalInitializerDigest"]):
                raise RuntimeError(f"{role} assembled graph differs from the frozen source cut")
            artifacts[role]["nodes"] = expected_nodes[role]
            artifacts[role]["graphBytes"] = manifest["assembledModelBytes"]
            artifacts[role]["initializerBytes"] = manifest["externalInitializerBytes"]
            continue
        begin = 0 if role == "Rank0" else 6
        shard = json.loads(manifest["tensorShardV1"])
        expected_axis = {"axis": "attention-head", "begin": begin, "end": begin + 6, "extent": 12}
        entries = json.loads(manifest["sourceEntryFiles"])
        count = 33 if role == "Rank0" else 32
        if (manifest["roleKind"] != "TENSOR_RANK" or shard["axes"] != [expected_axis] or
                [entry["entryId"] for entry in entries] !=
                [f"{role}/artifact/{index}" for index in range(count)]):
            raise RuntimeError(f"{role} entry inventory or attention head range differs")
        for index, entry in enumerate(entries):
            directory = path.parent / f"entry-{index}" if index else path.parent
            graph = directory / "model.onnx"
            initializer = directory / "model.onnx.data"
            if (graph.stat().st_size != entry["graphBytes"] or
                    "sha256:" + digest(graph) != entry["graphDigest"]):
                raise RuntimeError(f"{role} artifact entry {index} differs from its manifest")
            if entry["initializerBytes"]:
                if (initializer.stat().st_size != entry["initializerBytes"] or
                        "sha256:" + digest(initializer) != entry["initializerDigest"]):
                    raise RuntimeError(f"{role} artifact entry {index} initializer differs")
            elif initializer.exists():
                raise RuntimeError(f"{role} artifact entry {index} has an undeclared initializer")
        artifacts[role].update(headRange=expected_axis, entryCount=count,
                               graphBytes=sum(entry["graphBytes"] for entry in entries),
                               initializerBytes=sum(entry["initializerBytes"] for entry in entries),
                               stageLayoutDigest=shard["stage_layout_digest"])
    if artifacts["Rank0"]["stageLayoutDigest"] != artifacts["Rank1"]["stageLayoutDigest"]:
        raise RuntimeError("Wan rank artifacts disagree on the stage layout")
    return artifacts


def check_whole_model_tp(run_root: Path, requester_log: str, warm: bool) -> dict:
    selection = "NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId="
    committed = [line for line in requester_log.splitlines() if selection in line]
    if len(committed) != 1 or "NATIVE_NUMERICAL_ORACLE_PASS tensor=predictions values=1800" \
            not in requester_log:
        raise RuntimeError("whole-model TP requester lacks a unique verified result")
    session = committed[0].split(selection, 1)[1].split()[0]
    plan = committed[0].split(" planDigest=", 1)[1].split()[0]
    owners = {"Rank0": "provider", "Rank1": "provider2"}
    matched = {}
    cache = {}
    completed = []
    terminal = []
    onnx_ms = {role: [] for role in owners}
    timing = f"NDNSF_DI_ONNX_TIMING session={session} "
    marker = "NDNSF_DI_DEPENDENCY_OBJECT "
    for node in owners.values():
        with (run_root / f"{node}.log").open(errors="replace") as stream:
            for line in stream:
                if timing in line:
                    fields = dict(item.split("=", 1) for item in line.split(timing, 1)[1].split())
                    role = fields["role"]
                    elapsed = float(fields["run_ms"])
                    if owners.get(role) != node or fields["state_io_binding"] != "host" or \
                            not 0 <= elapsed < float("inf"):
                        raise RuntimeError("whole-model TP CPU timing does not match its role")
                    onnx_ms[role].append(elapsed)
                if f" requestId={session} " in line and " role=" in line:
                    role = line.split(" role=", 1)[1].split()[0]
                    if "stage=EXECUTION_COMPLETED status=observed" in line:
                        if f" planDigest={plan} " not in line:
                            raise RuntimeError("whole-model TP execution used a different plan")
                        completed.append((node, role))
                    if "stage=TERMINAL status=observed" in line:
                        terminal.append((node, role))
                    if "phase=CACHE_ACQUIRE_DONE" in line or "phase=RUNNER_REUSE_HIT" in line:
                        if owners.get(role) != node or role in cache:
                            raise RuntimeError("whole-model TP role cache ownership differs")
                        cache[role] = line.split(" detail=", 1)[1].split()[0]
                if marker not in line or f" session={session} " not in line:
                    continue
                fields = dict(item.split("=", 1) for item in line.split(marker, 1)[1].split())
                source, target = fields["producer"], fields["consumer"]
                if fields["status"] != "ok" or {source, target} != set(owners):
                    continue
                direction, name = fields["direction"], fields["planned_name"]
                endpoint = {"publish-exact-ndn": source, "fetch-exact-ndn": target}.get(direction)
                if owners.get(endpoint) != node or not name.startswith(
                        f"/8=example/8=hello/8={owners[source]}/"):
                    raise RuntimeError("whole-model TP handoff used a foreign producer or receiver")
                scope = fields["scope"].rsplit("/", 1)[-1]
                identity = (source, target, name, int(fields["encoded_payload_bytes"]))
                previous, directions = matched.setdefault(scope, (identity, set()))
                if previous != identity or direction in directions:
                    raise RuntimeError(f"whole-model TP handoff differs across endpoints: {scope}")
                directions.add(direction)
    expected = {f"source-tp-{index}" for index in range(1, 240)}
    if set(matched) != expected or any(
            kinds != {"publish-exact-ndn", "fetch-exact-ndn"} for _, kinds in matched.values()):
        raise RuntimeError(f"whole-model TP protected handoffs incomplete: {len(matched)}")
    if len({identity[2] for identity, _ in matched.values()}) != len(expected):
        raise RuntimeError("whole-model TP reused a protected object name across dependencies")
    cache_modes = {"template-hit", "assembled-disk-hit", "live-runner"} if warm else {"assembled-built"}
    if (sorted(completed) != sorted((node, role) for role, node in owners.items()) or
            terminal != [("provider", "Rank0")] or set(cache) != set(owners) or
            not set(cache.values()) <= cache_modes or
            {role: len(times) for role, times in onnx_ms.items()} != {"Rank0": 162, "Rank1": 119}):
        raise RuntimeError(f"whole-model TP completion, call counts or cache path differ: {cache}")
    return {"request": session, "planDigest": plan, "handoffs": len(matched),
            "tpEncodedBytes": sum(identity[3] for identity, _ in matched.values()),
            "roleCache": cache, "terminalRole": "Rank0",
            "onnxExecution": {role: {"calls": len(times), "runMs": round(sum(times), 3)}
                              for role, times in onnx_ms.items()},
            "invocationWire": check_invocation_wire(requester_log, session, owners)}


def check_wan_handoffs(run_root: Path, requester_log: str, warm: bool) -> dict:
    selection = "NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId="
    selections = [line for line in requester_log.splitlines() if selection in line]
    if len(selections) != 1 or "NATIVE_NUMERICAL_ORACLE_PASS tensor=flow values=4096" not in requester_log:
        raise RuntimeError("Wan requester lacks a unique sealed result")
    session = selections[0].split(selection, 1)[1].split()[0]
    plan = selections[0].split(" planDigest=", 1)[1].split()[0]
    owners = {"Prefix": "provider", "Rank0": "provider",
              "Rank1": "provider2", "Suffix": "provider2"}
    matched = {}
    cache = {}
    terminal = []
    completed = set()
    onnx_ms = {role: [] for role in owners}
    assembly_markers = {role: {} for role in owners}
    timing = f"NDNSF_DI_ONNX_TIMING session={session} "
    for node in ("provider", "provider2"):
        with (run_root / f"{node}.log").open(errors="replace") as stream:
            for line in stream:
                if timing in line:
                    fields = dict(item.split("=", 1) for item in line.split(timing, 1)[1].split())
                    role = fields["role"]
                    if owners.get(role) != node or fields["state_io_binding"] != "host":
                        raise RuntimeError(f"{role} ORT call differs from the selected CPU role")
                    elapsed = float(fields["run_ms"])
                    if not 0 <= elapsed < float("inf"):
                        raise RuntimeError(f"{role} ORT call has invalid timing")
                    onnx_ms[role].append(elapsed)
                if f" requestId={session} " in line:
                    for phase in ("ASSEMBLY_CALL_BEGIN", "ASSEMBLY_CALL_DONE"):
                        if f"phase={phase} " in line:
                            role = line.split(" role=", 1)[1].split()[0]
                            if owners.get(role) != node or phase in assembly_markers[role]:
                                raise RuntimeError(f"{role} has an ambiguous assembly receipt")
                            assembly_markers[role][phase] = float(line.split()[0])
                    if "stage=EXECUTION_COMPLETED status=observed" in line:
                        role = line.split(" role=", 1)[1].split()[0]
                        if owners.get(role) != node or role in completed or f" planDigest={plan} " not in line:
                            raise RuntimeError(f"{role} execution differs from its sealed plan")
                        completed.add(role)
                    if "stage=TERMINAL status=observed" in line:
                        terminal.append((node, line.split(" role=", 1)[1].split()[0]))
                    if "phase=CACHE_ACQUIRE_DONE" in line or "phase=RUNNER_REUSE_HIT" in line:
                        role = line.split(" role=", 1)[1].split()[0]
                        if owners.get(role) != node or role in cache:
                            raise RuntimeError(f"{role} lacks a unique cache receipt from its Provider")
                        cache[role] = line.split(" detail=", 1)[1].split()[0]
                marker = "NDNSF_DI_DEPENDENCY_OBJECT "
                if marker not in line or f" session={session} " not in line:
                    continue
                fields = dict(field.split("=", 1) for field in line.split(marker, 1)[1].split())
                if fields.get("status") != "ok":
                    continue
                source, target = fields["producer"], fields["consumer"]
                if source not in owners or target not in owners:
                    continue
                name = fields["planned_name"]
                expected_owner = owners[source if fields["direction"] == "publish-exact-ndn" else target]
                if node != expected_owner or not name.startswith(f"/8=example/8=hello/8={owners[source]}/"):
                    raise RuntimeError("Wan tensor producer or consumer identity differs")
                port = unquote(name.split("/8=port/8=", 1)[1].split("/", 1)[0])
                key = (source, target, port, int(fields["encoded_payload_bytes"]))
                value = matched.setdefault(name, (key, set()))
                if value[0] != key:
                    raise RuntimeError("Wan publication and decoded copy differ")
                value[1].add(fields["direction"])
    expected_calls = {"Prefix": 1, "Rank0": 33, "Rank1": 32, "Suffix": 1}
    if completed != owners.keys() or {role: len(times) for role, times in onnx_ms.items()} != expected_calls:
        raise RuntimeError("Wan Provider completions or ORT call counts differ from the sealed profile")
    assembly_ms = {}
    for role, markers in assembly_markers.items():
        if not markers and warm:
            continue
        if set(markers) != {"ASSEMBLY_CALL_BEGIN", "ASSEMBLY_CALL_DONE"}:
            raise RuntimeError(f"{role} lacks a complete assembly interval")
        duration = markers["ASSEMBLY_CALL_DONE"] - markers["ASSEMBLY_CALL_BEGIN"]
        if not 0 <= duration < float("inf"):
            raise RuntimeError(f"{role} assembly interval has invalid timing")
        assembly_ms[role] = round(1000 * duration, 3)
    if len(matched) != 43 or any(directions != {"publish-exact-ndn", "fetch-exact-ndn"}
                                   for _, directions in matched.values()):
        raise RuntimeError(f"Wan protected handoffs incomplete: {len(matched)}")
    expected_tp = set()
    for block in (27, 28):
        for attention in ("self", "cross"):
            prefix = f"block-{block}/{attention}"
            for name in ("norm-q/mean", "norm-k/mean", "o/projection"):
                for pair in (("Rank0", "Rank1"), ("Rank1", "Rank0")):
                    expected_tp.add((*pair, f"{prefix}/{name}"))
        for pair in ((("Rank0", "Rank1"), ("Rank1", "Rank0")) if block == 27 else (("Rank1", "Rank0"),)):
            expected_tp.add((*pair, f"block-{block}/down/projection"))
    observed_tp = []
    pp = 0
    lp = 0
    pp_ports = set()
    for (source, target, port, size), _ in matched.values():
        if source.startswith("Rank") and target.startswith("Rank"):
            observed_tp.append((source, target, port, size))
        elif owners[source] != owners[target]:
            pp_ports.add((source, target, port))
            pp += size
        else:
            lp += size
    if (len(observed_tp) != len(expected_tp) or
            {item[:3] for item in observed_tp} != expected_tp or
            ("Prefix", "Rank1", "block-26/ffn/residual") not in pp_ports or
            ("Rank0", "Suffix", "block-28/ffn/residual") not in pp_ports):
        raise RuntimeError("Wan head, FFN or pipeline handoff inventory differs")
    expected_cache = {"template-hit", "assembled-disk-hit", "live-runner"} if warm else {
        "assembled-built"}
    if terminal != [("provider2", "Suffix")] or set(cache) != set(owners) or not set(cache.values()) <= expected_cache:
        raise RuntimeError(f"Wan terminal or prepared artifact mismatch: {terminal}, {cache}")
    result = {"request": session, "middleCache": cache, "handoffs": len(matched),
              "tpEncodedBytes": sum(item[3] for item in observed_tp),
              "networkPpEncodedBytes": pp, "localPpEncodedBytes": lp,
              "pipelinePorts": sorted(pp_ports), "reductions": sorted(observed_tp),
              "assemblyWallMs": assembly_ms,
              "onnxExecution": {role: {"calls": len(times), "runMs": round(sum(times), 3)}
                                for role, times in onnx_ms.items()},
              "invocationWire": check_invocation_wire(requester_log, session, owners)}
    if not warm:
        result["assembledArtifacts"] = check_wan_artifacts(run_root, owners, selections[0])
    return result


def check_ffn_cancellation(run_root: Path, blocks: bool) -> dict:
    session = (run_root / "cancelled-request.txt").read_text().strip()
    requester = (run_root / "requester-hybrid-cancel.log").read_text(errors="replace")
    marker = f"NDNSF_DI_NATIVE_CANCELLATION_RECEIPT requestId={session} "
    receipt = [line.split(marker, 1)[1] for line in requester.splitlines() if marker in line]
    if len(receipt) != 1:
        raise RuntimeError("cancelled request has no unique native cancellation receipt")
    counts = {key: int(value) for key, value in
              (field.split("=", 1) for field in receipt[0].split())}
    if (counts.get("expected") != 4 or counts.get("drained") != 4 or
            counts.get("fenced", 0) < 2 or
            f"NATIVE_REQUEST_CANCEL_DRAINED request={session}" not in requester or
            "NATIVE_REQUEST_INTERRUPTED" not in requester or
            (run_root / "requester/output-hybrid-cancel.bin").exists()):
        raise RuntimeError(f"incomplete cancellation or drain receipt: {counts}")
    left = (run_root / "provider.log").read_text(errors="replace").splitlines()
    right = (run_root / "provider2-fault.log").read_text(errors="replace").splitlines()
    first, subsequent = (2, 4) if blocks else (0, 1)
    withheld = f"/8=step/8=Rank1%2FRUN%2F{first}/"
    next_partial = f"/8=step/8=Rank1%2FRUN%2F{subsequent}/"
    downstream = "/8=step/8=Rank0%2FRUN%2F32/" if blocks else "/8=step/8=Rank0%2FSUM%2F1/"
    if not any(session in line and "status=begin" in line and " role=Rank0 " in line and
               withheld in line for line in left):
        raise RuntimeError("Rank0 did not wait on the withheld Rank1 partial")
    for line in left + right:
        if session not in line:
            continue
        if ("stage=DEPENDENCY_FETCH status=begin " in line and " role=Rank0 " in line and
                next_partial in line):
            raise RuntimeError("Rank0 started the next window with its first operand missing")
        if ("direction=publish-exact-ndn" in line and
                any(step in line for step in (withheld, downstream))):
            raise RuntimeError("cancelled rank published the withheld or dependent tensor")
        if "stage=EXECUTION_COMPLETED" in line and " role=Suffix " in line:
            raise RuntimeError("suffix completed with a missing rank operand")
    return {"request": session, **counts}


def check_tp_cancellation(run_root: Path, case: str) -> dict:
    session = (run_root / "cancelled-request.txt").read_text().strip()
    phase = f"{case}-tp-cancel"
    requester = (run_root / f"requester-{phase}.log").read_text(errors="replace")
    marker = f"NDNSF_DI_NATIVE_CANCELLATION_RECEIPT requestId={session} "
    receipts = [line.split(marker, 1)[1] for line in requester.splitlines() if marker in line]
    if len(receipts) != 1:
        raise RuntimeError("cancelled YOLO request has no unique native cancellation receipt")
    counts = {key: int(value) for key, value in
              (field.split("=", 1) for field in receipts[0].split())}
    if (counts.get("expected") != 2 or counts.get("drained") != 2 or
            f"NATIVE_REQUEST_CANCEL_DRAINED request={session}" not in requester or
            "NATIVE_REQUEST_INTERRUPTED" not in requester or
            (run_root / "requester" / f"output-{phase}.bin").exists()):
        raise RuntimeError(f"incomplete YOLO cancellation or drain receipt: {counts}")

    withheld = "/8=step/8=Rank1%2Fcall%2F0/"
    left = (run_root / "provider.log").read_text(errors="replace").splitlines()
    right = (run_root / "provider2-fault.log").read_text(errors="replace").splitlines()
    if not any(f"requestId={session} " in line and "stage=DEPENDENCY_FETCH status=begin" in line
               and "role=Rank0 " in line and withheld in line for line in left):
        raise RuntimeError("YOLO Rank0 did not wait on its required Rank1 contribution")
    for line in left + right:
        if session not in line:
            continue
        if (("direction=publish-exact-ndn" in line and withheld in line) or
                "stage=EXECUTION_COMPLETED status=observed" in line or
                "stage=TERMINAL status=observed" in line):
            raise RuntimeError("cancelled YOLO execution crossed its publication fence")
    return {"request": session, **counts}


def check_wired_execution(run_root: Path, mode: str) -> dict:
    expected = ({"provider": {"Prefix"}, "provider2": {"Suffix"}} if mode == "component"
                else {"provider": {"Prefix", "Rank0"}, "provider2": {"Rank1", "Suffix"}})
    placements = {}
    requests = set()
    for actor, roles in expected.items():
        path = run_root / (actor + "-statistics.jsonl")
        actor_requests = set()
        for line in path.read_text().splitlines():
            row = json.loads(line)
            if row.get("type") != "request" or not row.get("completionAdmitted"):
                continue
            if row.get("provider") != ACTOR_PREFIX[actor] or row.get("failureReason"):
                raise ValueError(f"invalid execution receipt from {actor}")
            actor_requests.add((row["requestId"], row["attemptEpoch"], row["planDigest"]))
        if len(actor_requests) != 1:
            raise ValueError(f"{actor} lacks a unique admitted request receipt")
        requests.update(actor_requests)
        request_id, attempt, _ = next(iter(actor_requests))
        executed = set()
        with (run_root / (actor + ".log")).open() as log:
            for line in log:
                if "NDNSF_PHASE_TIMING " not in line or " phase=roleExecutionEnd " not in line:
                    continue
                row = dict(field.split("=", 1) for field in line.split() if "=" in field)
                if row.get("requestId") == request_id and row.get("attempt") == str(attempt):
                    executed.add(row["executionRole"].lstrip("/"))
        if executed != roles:
            raise ValueError(f"{actor} executed {sorted(executed)}, expected {sorted(roles)}")
        placements[actor] = sorted(executed)
    if len(requests) != 1:
        raise ValueError("Provider receipts do not identify the same sealed request and attempt")
    timing = json.loads((run_root / "request-timing.json").read_text())
    return {"providerRoles": placements, "request": timing}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, default=Path("/usr/local"))
    parser.add_argument("--variant", choices=("26n", "11n", "ffn", "attention-ffn", "wan"), default="26n")
    parser.add_argument("--mode", choices=("component", "hybrid"), default="hybrid")
    parser.add_argument("--cache", type=Path, help="prepared assets for the selected YOLO variant")
    parser.add_argument("--initializer-sha256", help="explicit sha256: digest of prepared weights")
    parser.add_argument("--whole-model-tp", action="store_true",
                        help="run the two-rank whole-model source plan for YOLO26n")
    parser.add_argument("--matched-tp", action="store_true",
                        help="compare full, cold TP and warm TP on one fixed case")
    parser.add_argument("--tp-cancel", action="store_true",
                        help="cancel with a withheld YOLO TP contribution, then recover")
    parser.add_argument("--face-transport", choices=("udp", "tcp"), default="udp",
                        help="transport for inter-node NFD faces")
    parser.add_argument("--ffn-fixture", type=Path, help="prepared FFN, block or Wan fixture")
    parser.add_argument("--wan-reference", type=Path, help="frozen input.bundle and mixed oracle.json")
    parser.add_argument("--ffn-cancel", action="store_true")
    parser.add_argument("--wired-star", action="store_true", help="use the Spec 270 star topology")
    parser.add_argument("--topology", type=Path, help="override the selected layout's topology file")
    parser.add_argument("--case", action="append", default=[],
                        help="case directory under the model assets (repeatable)")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--nfd-log-level", default="NONE", choices=("NONE", "WARN", "DEBUG"))
    args = parser.parse_args()
    if args.whole_model_tp and args.variant != "26n":
        parser.error("--whole-model-tp requires --variant 26n")
    if args.mode == "component" and (args.whole_model_tp or args.variant not in ("26n", "11n")):
        parser.error("--mode component requires YOLO without --whole-model-tp")
    if args.matched_tp and (not args.whole_model_tp or len(args.case) != 1):
        parser.error("--matched-tp requires --whole-model-tp and one --case")
    if args.tp_cancel and (not args.whole_model_tp or len(args.case) != 1 or args.matched_tp):
        parser.error("--tp-cancel requires --whole-model-tp and one --case without --matched-tp")
    wan = args.variant == "wan"
    blocks = args.variant == "attention-ffn"
    if wan != (args.wan_reference is not None) or (wan and args.ffn_cancel):
        parser.error("--wan-reference selects the Wan inputs and excludes FFN fault injection")
    if (args.variant in ("ffn", "attention-ffn", "wan")) != (args.ffn_fixture is not None):
        parser.error("the selected variant and --ffn-fixture disagree")
    if args.ffn_fixture and (args.case or args.cache or args.initializer_sha256):
        parser.error("--case, --cache and --initializer-sha256 select prepared YOLO assets")
    if args.ffn_cancel and not args.ffn_fixture:
        parser.error("--ffn-cancel requires the FFN fixture")
    if args.wired_star and args.case and not args.whole_model_tp:
        parser.error("--wired-star selects the tracked fixed input and oracle for component and hybrid")
    topology = args.topology or (WIRED_TOPOLOGY if args.wired_star else TOPOLOGY)
    role_node = WIRED_ROLE_NODE if args.wired_star else ROLE_NODE
    frozen_yolo = args.wired_star and not args.case and not args.ffn_fixture
    if os.geteuid() != 0:
        parser.error("MiniNDN qualification requires root")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    scratch = args.scratch_root.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    receipt = output / "result.json"

    prefix = args.prefix.resolve()
    fixture_cache = args.ffn_fixture or args.cache
    installed = {
        name: prefix / path for name, path in {
            "requester": "bin/DI_NativeRequester",
            "provider": "bin/di-native-provider",
            "controller": "bin/App_ServiceController",
            "authority": "bin/DI_NativeArtifactAuthority",
            "worker": "libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker",
        }.items()
    }
    if args.ffn_cancel or args.tp_cancel:
        installed["fault-provider"] = prefix / "bin/di-native-fault-provider"
    if args.ffn_fixture:
        installed["source-inspect"] = prefix / "libexec/ndnsf-tests/native-source-inspect"
    missing = [str(path) for path in installed.values() if not path.is_file()]
    if missing:
        raise RuntimeError("installed native entry is missing: " + ",".join(missing))

    result = {
        "schema": "spec263-minindn-hybrid-v1" if args.ffn_fixture else "spec261-minindn-hybrid-v1",
        "status": "IN_PROGRESS",
        "mode": args.mode,
        "variant": args.variant,
        "expectedInitializerSha256": args.initializer_sha256,
        "wholeModelTensorParallel": args.whole_model_tp,
        "matchedTensorParallel": args.matched_tp,
        "faceTransport": args.face_transport,
        "actorNodes": role_node,
        "topologySha256": digest(topology),
        "harnessSha256": digest(Path(__file__)),
        "installed": {name: {"path": str(path), "sha256": digest(path)}
                      for name, path in installed.items()},
        "phases": [],
        "forcedCleanup": False,
    }
    if wan:
        result["fixture"] = {name: digest(args.ffn_fixture / name)
                             for name in ("model.onnx", "model.onnx.data", "source-anchors.json")}
        result["inputs"] = {name: digest(args.wan_reference / name)
                            for name in ("input.bundle", "oracle.json")}
    elif args.ffn_fixture:
        files = ("model.onnx", "oracle.json") + (
            ("input.bundle", "source-anchors.json") if blocks else ("input.f32",))
        result["fixture"] = {name: digest(args.ffn_fixture / name) for name in files}
    elif args.whole_model_tp:
        asset = Path(os.environ.get("SPEC261_ASSET_ROOT",
                                    "/usr/local/lib/ndnsf/spec261/models")) / "yolo26n"
        model_cache = args.cache or asset
        graph = model_cache / "model.onnx"
        weights = model_cache / "model.weights"
        if not graph.is_file():
            graph = model_cache / "yolo26n.onnx"
        if not weights.exists():
            weights = model_cache / "yolo26n.weights"
        result["fixture"] = {path.name: digest(path) for path in (graph, weights)}
        result["inputs"] = {
            case: {name: digest(asset / "cases" / case / name)
                   for name in ("input.f32", "oracle.json")}
            for case in args.case
        }
        if frozen_yolo:
            fixture = ROOT / "tests/fixtures/spec261/yolo26n"
            result["inputs"]["tracked"] = {
                name: digest(fixture / name)
                for name in ("fixed-input.npy", "oracle-predictions.npy")
            }

    def persist() -> None:
        receipt.write_text(json.dumps(result, indent=1, sort_keys=True), encoding="utf-8")

    persist()

    # Mini-NDN installs its Python package outside the default site-packages on
    # this host (a versioned prefix). Resolve it explicitly before importing.
    candidates = [os.environ.get("NDNSF_MININDN_PYTHONPATH", "")]
    for pattern in ("/usr/local/lib/ndnsf/mini-ndn/*/python*/site-packages",
                    "/home/*/NDN/mini-ndn"):
        candidates.extend(str(Path(item).parent if Path(item).name == "minindn"
                               else item)
                          for item in glob.glob(pattern))
    for candidate in candidates:
        if candidate and (Path(candidate) / "minindn" / "__init__.py").is_file():
            sys.path.insert(0, candidate)
            result["minindnPath"] = candidate
            break
    try:
        from minindn.minindn import Minindn
        from minindn.apps.app_manager import AppManager
        from minindn.apps.nfd import Nfd
        from minindn.helpers.nfdc import Nfdc
    except ModuleNotFoundError as error:
        # The MiniNDN Python package is a host prerequisite, like Mininet
        # itself. Report the unmet prerequisite instead of a traceback.
        result["status"] = "UNQUALIFIED"
        result["unmetPrerequisite"] = "python-module:" + error.name
        persist()
        return 2

    ndn = None
    manager = None
    driver_process = None
    streams = []
    started = time.monotonic()

    def interrupted(signum, frame):
        raise KeyboardInterrupt("signal " + str(signum))

    previous = {number: signal.signal(number, interrupted)
                for number in (signal.SIGINT, signal.SIGTERM)}

    def stop(process) -> None:
        if process is None or process.poll() is not None:
            return
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            result["forcedCleanup"] = True
            process.kill()
            process.wait(timeout=10)

    try:
        if shutil.which("infoconv") is None:
            raise RuntimeError("MiniNDN NFD configuration requires infoconv on PATH")
        ndn = Minindn(parser=parser, topoFile=str(topology),
                      workDir=str(scratch / "nodes"), controller=None)
        if Minindn.ndnSecurityDisabled:
            raise RuntimeError("dummy keychain cannot qualify signature behavior")
        ndn.start()
        manager = AppManager(ndn, [], Nfd)
        transports: dict[str, str] = {}
        pids: dict[str, int] = {}
        for node in ndn.net.hosts:
            manager.startOnNode(node, logLevel=args.nfd_log_level, csSize=0)
            app = manager[node.name]
            if app.process.poll() is not None:
                raise RuntimeError(f"NFD exited on {node.name}; see {app.logFile}")
            transports[node.name] = "unix://" + app.sockFile
            pids[node.name] = app.process.pid
            result["phases"].append({"phase": "nfd-ready", "node": node.name,
                                     "pid": app.process.pid})
        persist()

        # Static reachability for the shared /example/hello namespace: every
        # node gets a route through each of its links. The small topology has no
        # NLSR, and a single missing route would look like a protocol failure.
        def peer_ip(node, other):
            for intf in node.intfList():
                link = intf.link
                if link is None:
                    continue
                a, b = link.intf1, link.intf2
                peer = b if a is intf else a
                if peer.node is other:
                    return str(peer.ip)
            return None

        def nfdc(node, *args):
            env = dict(os.environ, HOME=str(manager[node.name].homeDir),
                       NDN_CLIENT_TRANSPORT=transports[node.name])
            stdout, stderr, code = node.pexec(["timeout", "5", "nfdc", *args], env=env)
            if code:
                raise RuntimeError(f"nfdc on {node.name}: {stdout}{stderr}")
            return stdout

        hosts = list(ndn.net.hosts)
        required_nodes = set(role_node.values())
        if args.wired_star:
            required_nodes.add("router")
        missing_nodes = required_nodes - {node.name for node in hosts}
        if missing_nodes:
            raise RuntimeError(f"selected topology lacks nodes {sorted(missing_nodes)}")
        routing = {}
        actor_routes = {}
        for node in hosts:
            nfdc(node, "strategy", "set", "/example/hello", "/localhost/nfd/strategy/multicast")
            faces = {}
            for other in hosts:
                if other is node:
                    continue
                address = peer_ip(node, other)
                if address is None:
                    # Not a direct neighbor: reachability comes from the
                    # neighbors' own faces and the multicast strategy.
                    continue
                face = Nfdc.createFace(node, address, protocol=args.face_transport)
                if str(face) == "-1":
                    raise RuntimeError("cannot create a face from " + node.name +
                                       " to " + other.name)
                faces[other.name] = str(face)
                nfdc(node, "route", "add", "/example/hello", str(face))
            # Route Producer and certificate names towards the actor, using
            # the router as next hop for indirect leaves of the wired star.
            actor_routes[node.name] = {}
            for role, destination in role_node.items():
                if destination == node.name:
                    continue
                face = faces.get(destination)
                if face is None and args.wired_star:
                    face = faces.get("router")
                if face is None:
                    continue
                prefix_name = ACTOR_PREFIX[role]
                for namespace in (prefix_name, prefix_name + "/KEY"):
                    if args.whole_model_tp:
                        Nfdc.registerRoute(node, namespace, face, captureFlag=True)
                    else:
                        nfdc(node, "route", "add", namespace, face)
                    actor_routes[node.name][namespace] = face
            if args.wired_star or args.whole_model_tp:
                fib_output = nfdc(node, "fib", "list")
                if args.wired_star:
                    routing[node.name] = {"faces": nfdc(node, "face", "list"), "fib": fib_output}
                if args.whole_model_tp:
                    fib = dict(line.strip().split(" nexthops=", 1)
                               for line in fib_output.splitlines() if " nexthops=" in line)
                    for namespace, face in actor_routes[node.name].items():
                        if fib.get(namespace) != f"{{faceid={face} (cost=0)}}":
                            raise RuntimeError(f"{node.name} lacks the captured route to {namespace}")
        if args.wired_star:
            (output / "routing.json").write_text(json.dumps(routing, indent=1))
        if args.whole_model_tp:
            result["actorDirectRoutes" if not args.wired_star else "actorRoutes"] = actor_routes
        result["phases"].append({"phase": "routes-ready", "nodes": len(hosts)})
        persist()

        environment = os.environ.copy()
        environment["NDNSF_TEST_RUNTIME_PREFIX"] = str(prefix)
        driver_log = "ndnsf.di.NativeRequester=WARN"
        if args.ffn_fixture:
            driver_log += ":ndnsf.di.Spec220PreparationTrace=DEBUG"
        if args.whole_model_tp:
            driver_log += (":ndn_service_framework.ServiceUser=INFO"
                           ":ndn_service_framework.ServiceProvider=INFO")
        environment.setdefault("NDN_LOG", driver_log)
        if args.wired_star:
            environment["NDNSF_PHASE_TIMING"] = "1"
            environment["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = "1"
            environment["NDN_LOG"] += ":ndn_service_framework.TimelineTrace=WARN"
        for role, node in role_node.items():
            environment["SPEC261_TRANSPORT_" + role] = transports[node]
            environment["SPEC261_NETNS_PID_" + role] = str(pids[node])
        environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin"
        run_root = output / "driver"
        log_path = output / "driver.log"
        with log_path.open("wb") as stream:
            streams.append(stream)
            driver_process = subprocess.Popen(
                [sys.executable, str(DRIVER), "--no-local-nfd", "--variant", args.variant,
                 "--mode", args.mode, "--run-root", str(run_root)] +
                (["--frozen-fixtures"] if frozen_yolo else []) +
                (["--cache", str(fixture_cache.resolve())] if fixture_cache else []) +
                (["--initializer-sha256", args.initializer_sha256] if args.initializer_sha256 else []) +
                (["--wan-reference", str(args.wan_reference.resolve())] if wan else []) +
                (["--ffn-cancel"] if args.ffn_cancel else []) +
                (["--whole-model-tp"] if args.whole_model_tp else []) +
                (["--matched-tp"] if args.matched_tp else []) +
                (["--tp-cancel"] if args.tp_cancel else []) +
                [item for case in args.case for item in ("--case", case)],
                cwd=scratch, env=environment, stdout=stream,
                stderr=subprocess.STDOUT)
            driver_process.wait(timeout=args.timeout)
        result["driverExit"] = driver_process.returncode
        requester_log = run_root / "requester.log"
        phases = ("full", "hybrid-cold", "hybrid-warm") if args.ffn_fixture else args.case
        if args.matched_tp:
            phases = [f"{args.case[0]}-{suffix}" for suffix in ("full", "tp-cold", "tp-warm")]
        if args.tp_cancel:
            phases = [f"{args.case[0]}-tp-recovery"]
        if args.ffn_cancel:
            phases += ("hybrid-recovery",)
        case_logs = [run_root / f"requester-{case}.log" for case in phases]
        texts = [requester_log.read_text(errors="replace") if requester_log.exists() else ""]
        texts += [log.read_text(errors="replace") if log.exists() else "" for log in case_logs]
        result["phases"].append({"phase": "driver-finished",
                                 "exit": driver_process.returncode,
                                 "requesterLogs": [str(requester_log)] + [str(log) for log in case_logs]})
        passed = driver_process.returncode == 0 and bool(phases or texts[0]) and all(
            "NATIVE_NUMERICAL_ORACLE_PASS" in text for text in
            (texts[1:] if phases else texts[:1]))
        if frozen_yolo and (run_root / "actor-cleanup.json").is_file():
            cleanup = json.loads((run_root / "actor-cleanup.json").read_text())
            result["actorCleanup"] = cleanup
            result["forcedCleanup"] |= any(row["forced"] or not row["reaped"] for row in cleanup)
        if passed and frozen_yolo and not args.whole_model_tp:
            if len(result.get("actorCleanup", [])) != len(role_node):
                raise ValueError("incomplete native actor cleanup receipt")
            result.update(check_wired_execution(run_root, args.mode))
        result["status"] = "PASS" if passed else "FAIL"
        if not passed:
            first = next((source.splitlines() for source in texts
                          if "NATIVE_REQUEST_STAGE_FAILED" in source), [])
            failure = next((line for line in first if " domain=" in line and
                            line.startswith("NATIVE_") and " boundary=" in line), None)
            if failure is None:
                failure = next((line for line in first
                                if "NATIVE_REQUEST_STAGE_FAILED" in line), None)
            result["firstFailure"] = failure or "native driver did not report the oracle pass"
        if args.ffn_fixture or args.whole_model_tp:
            trials = run_root / "requester-trials.jsonl"
            if trials.exists():
                result["requesterTrials"] = [json.loads(line) for line in trials.read_text().splitlines()]
            cleanup = run_root / "process-cleanup.json"
            if cleanup.exists():
                result["actorCleanup"] = json.loads(cleanup.read_text())
                result["forcedCleanup"] |= any(actor["exit"] in (None, -signal.SIGKILL)
                                               for actor in result["actorCleanup"])
        if args.matched_tp and passed:
            records = result["requesterTrials"]
            if ([row["phase"] for row in records] != phases or
                    len({row["inputSha256"] for row in records}) != 1 or
                    any(row["exit"] != 0 for row in records)):
                raise RuntimeError("matched TP trial inventory differs from the frozen input")
            result["exactOutputMatch"] = len({row["outputSha256"] for row in records}) == 1
            sessions = []
            for log in texts[1:]:
                marker = "NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId="
                ids = [line.split(marker, 1)[1].split()[0]
                       for line in log.splitlines() if marker in line]
                if len(ids) != 1:
                    raise RuntimeError("matched TP trial did not commit exactly one Selection")
                sessions.extend(ids)
            if len(set(sessions)) != len(phases):
                raise RuntimeError("matched TP trials reused a request identity")
            result["matchedRequestIds"] = sessions
            result["tensorParallelExecutions"] = [
                check_whole_model_tp(run_root, log, warm)
                for log, warm in zip(texts[2:], (False, True))
            ]
        if args.tp_cancel and passed:
            case = args.case[0]
            result["cancellation"] = check_tp_cancellation(run_root, case)
            result["recovery"] = check_whole_model_tp(run_root, texts[-1], warm=True)
            if result["cancellation"]["request"] == result["recovery"]["request"]:
                raise RuntimeError("YOLO recovery reused a cancelled request identity")
            exits = {actor["pid"]: actor["exit"] for actor in result.get("actorCleanup", [])}
            cancelled = int((run_root / f"requester-{case}-tp-cancel.pid").read_text())
            if exits.get(cancelled) != 1 or any(
                    code != 0 for pid, code in exits.items() if pid != cancelled):
                raise RuntimeError(f"unexpected YOLO cancellation actor exits: {exits}")
        if passed and args.whole_model_tp and not (args.matched_tp or args.tp_cancel):
            trial_logs = texts[1:] if phases else texts[:1]
            result["tensorParallelExecutions"] = [
                check_whole_model_tp(run_root, log, warm=index > 0)
                for index, log in enumerate(trial_logs)
            ]
        if args.ffn_fixture:
            if args.ffn_cancel and any(trial["phase"] == "hybrid-cancel"
                                       for trial in result.get("requesterTrials", [])):
                result["cancellation"] = check_ffn_cancellation(run_root, blocks)
            if passed:
                actor_exits = {actor["pid"]: actor["exit"] for actor in result.get("actorCleanup", [])}
                actors = ["controller", "authority", "provider", "provider2"]
                actors += [f"requester-{phase}" for phase in phases]
                expected_exits = {int((run_root / f"{actor}.pid").read_text()): 0
                                  for actor in actors}
                if args.ffn_cancel:
                    expected_exits[int((run_root / "provider2-fault.pid").read_text())] = 0
                    expected_exits[int((run_root / "requester-hybrid-cancel.pid").read_text())] = 1
                if actor_exits != expected_exits:
                    raise RuntimeError(f"unexpected FFN actor exit status: {actor_exits}")
                check_handoffs = check_wan_handoffs if wan else (
                    check_block_handoffs if blocks else check_ffn_handoffs)
                result["hybridHandoffs"] = [check_handoffs(run_root, log, warm)
                                            for log, warm in zip(texts[2:], (False, True))]
                if wan:
                    result["fixedRoleTemplates"] = check_wan_fixed_role_templates(run_root)
                if len({trial["request"] for trial in result["hybridHandoffs"]}) != 2:
                    raise RuntimeError("cold and warm trials reused a request identity")
                if args.ffn_cancel:
                    cancelled = result["cancellation"]
                    recovery = check_handoffs(run_root, texts[-1], warm=True)
                    earlier = {trial["request"] for trial in result["hybridHandoffs"]}
                    if recovery["request"] in earlier | {cancelled["request"]}:
                        raise RuntimeError("recovery reused a request identity")
                    result["recovery"] = recovery
    except (Exception, KeyboardInterrupt) as error:
        result["status"] = "FAIL"
        result.setdefault("firstFailure", f"{type(error).__name__}: {error}")
        raise
    finally:
        stop(driver_process)
        if manager is not None:
            try:
                manager.cleanup()
            except Exception:
                result["forcedCleanup"] = True
        if ndn is not None:
            try:
                ndn.stop()
            except Exception:
                result["forcedCleanup"] = True
        for stream in streams:
            try:
                stream.close()
            except Exception:
                pass
        for number, handler in previous.items():
            signal.signal(number, handler)
        if result["forcedCleanup"]:
            result["status"] = "FAIL"
            result.setdefault("firstFailure", "MiniNDN cleanup did not complete normally")
        result["elapsedSeconds"] = round(time.monotonic() - started, 3)
        persist()
    return 0 if result.get("status") == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
