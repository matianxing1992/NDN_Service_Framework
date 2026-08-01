#!/usr/bin/env python3
"""Describe the current DistributedRepo exact-packet subject deterministically.

This script is intentionally observational. It does not import NDNSF, start
NFD, write repository state, or benchmark anything. Its output identifies the
legacy subject that Spec 164 must preserve while implementing a new artifact
transport.
"""

from __future__ import annotations

import ast
import hashlib
import json
import re
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
ORCHESTRATION = (
    ROOT
    / "NDNSF-DistributedRepo"
    / "pythonWrapper"
    / "py_repoclient"
    / "orchestration.py"
)
NATIVE_BINDING = ROOT / "pythonWrapper" / "src" / "ndnsf" / "_ndnsf.cpp"


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_text(value: str) -> str:
    return sha256_bytes(value.encode())


def python_method_source(
    source: str,
    tree: ast.Module,
    class_name: str,
    method_name: str,
) -> str:
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            for child in node.body:
                if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    if child.name == method_name:
                        segment = ast.get_source_segment(source, child)
                        if segment is None:
                            raise RuntimeError(
                                f"cannot extract {class_name}.{method_name}"
                            )
                        return segment
    raise RuntimeError(f"missing method {class_name}.{method_name}")


def cxx_function_source(source: str, function_name: str) -> str:
    match = re.search(rf"\b{re.escape(function_name)}\s*\([^;]*?\)\s*\{{", source, re.S)
    if match is None:
        raise RuntimeError(f"missing C++ function {function_name}")
    start = source.rfind("\n", 0, match.start()) + 1
    brace = source.find("{", match.start())
    depth = 0
    in_string = False
    quote = ""
    escaped = False
    index = brace
    while index < len(source):
        character = source[index]
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                in_string = False
        elif character in {'"', "'"}:
            in_string = True
            quote = character
        elif character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
        index += 1
    raise RuntimeError(f"unterminated C++ function {function_name}")


def call_names(method: ast.AST, called_name: str) -> list[ast.Call]:
    calls: list[ast.Call] = []
    for node in ast.walk(method):
        if not isinstance(node, ast.Call):
            continue
        function = node.func
        if isinstance(function, ast.Name) and function.id == called_name:
            calls.append(node)
        elif isinstance(function, ast.Attribute) and function.attr == called_name:
            calls.append(node)
    return calls


def class_method(
    tree: ast.Module,
    class_name: str,
    method_name: str,
) -> ast.FunctionDef:
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            for child in node.body:
                if isinstance(child, ast.FunctionDef) and child.name == method_name:
                    return child
    raise RuntimeError(f"missing method {class_name}.{method_name}")


def literal_int(node: ast.AST) -> int:
    if isinstance(node, ast.Constant) and isinstance(node.value, int):
        return node.value
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
        return -literal_int(node.operand)
    if isinstance(node, ast.BinOp):
        left = literal_int(node.left)
        right = literal_int(node.right)
        if isinstance(node.op, ast.Add):
            return left + right
        if isinstance(node.op, ast.Sub):
            return left - right
        if isinstance(node.op, ast.Mult):
            return left * right
        if isinstance(node.op, ast.FloorDiv):
            return left // right
    raise RuntimeError("expected integer literal expression")


def default_argument(method: ast.FunctionDef, name: str) -> int:
    positional = list(method.args.args)
    defaults = [None] * (len(positional) - len(method.args.defaults))
    defaults.extend(method.args.defaults)
    for argument, default in zip(positional, defaults):
        if argument.arg == name and default is not None:
            return literal_int(default)
    keyword_defaults = dict(
        zip((arg.arg for arg in method.args.kwonlyargs), method.args.kw_defaults)
    )
    default = keyword_defaults.get(name)
    if default is None:
        raise RuntimeError(f"missing default for {name}")
    return literal_int(default)


def keyword_int(call: ast.Call, name: str) -> int:
    for keyword in call.keywords:
        if keyword.arg == name:
            return literal_int(keyword.value)
    raise RuntimeError(f"missing keyword {name}")


def string_operations(method: ast.FunctionDef) -> list[str]:
    operations: set[str] = set()
    for call in call_names(method, "encode_repo_request"):
        if call.args and isinstance(call.args[0], ast.Constant):
            value = call.args[0].value
            if isinstance(value, str):
                operations.add(value)
    return sorted(operations)


def deterministic_fixture(size: int) -> bytes:
    prefix = b"ndnsf-spec164-exact-packet-v1:"
    output = bytearray()
    counter = 0
    while len(output) < size:
        output.extend(
            hashlib.sha256(prefix + counter.to_bytes(8, "big")).digest()
        )
        counter += 1
    return bytes(output[:size])


def build_subject() -> dict[str, Any]:
    orchestration_bytes = ORCHESTRATION.read_bytes()
    orchestration_source = orchestration_bytes.decode()
    native_bytes = NATIVE_BINDING.read_bytes()
    native_source = native_bytes.decode()
    tree = ast.parse(orchestration_source, filename=str(ORCHESTRATION))

    python_methods = {
        "NetworkDistributedRepoClient._packet_to_request": python_method_source(
            orchestration_source,
            tree,
            "NetworkDistributedRepoClient",
            "_packet_to_request",
        ),
        "NetworkDistributedRepoClient.store_object": python_method_source(
            orchestration_source,
            tree,
            "NetworkDistributedRepoClient",
            "store_object",
        ),
        "DistributedRepo.put": python_method_source(
            orchestration_source, tree, "DistributedRepo", "put"
        ),
        "DistributedRepo.put_file": python_method_source(
            orchestration_source, tree, "DistributedRepo", "put_file"
        ),
        "DistributedRepo.get_file": python_method_source(
            orchestration_source, tree, "DistributedRepo", "get_file"
        ),
        "RepoNodeApp._init_sqlite": python_method_source(
            orchestration_source, tree, "RepoNodeApp", "_init_sqlite"
        ),
        "RepoNodeApp._persist_packets": python_method_source(
            orchestration_source, tree, "RepoNodeApp", "_persist_packets"
        ),
        "RepoNodeApp._persist_packet": python_method_source(
            orchestration_source, tree, "RepoNodeApp", "_persist_packet"
        ),
    }
    cxx_functions = {
        "makeSegmentedDataPackets": cxx_function_source(
            native_source, "makeSegmentedDataPackets"
        ),
        "fetchSegmentedDataPackets": cxx_function_source(
            native_source, "fetchSegmentedDataPackets"
        ),
    }

    put_file = class_method(tree, "DistributedRepo", "put_file")
    store_object = class_method(tree, "NetworkDistributedRepoClient", "store_object")
    segmented_calls = call_names(store_object, "make_segmented_data_packets")
    if len(segmented_calls) != 1:
        raise RuntimeError(
            "legacy store_object must have exactly one segmented-packet call"
        )
    range_calls = call_names(store_object, "range")
    retry_attempts = [
        literal_int(call.args[0])
        for call in range_calls
        if len(call.args) == 1
        and isinstance(call.args[0], (ast.Constant, ast.BinOp))
    ]
    if 3 not in retry_attempts:
        raise RuntimeError("legacy store_object retry range(3) was not found")

    sqlite_method = python_methods["RepoNodeApp._init_sqlite"]
    sqlite_tables = sorted(
        set(
            re.findall(
                r"CREATE TABLE IF NOT EXISTS\s+([a-z_]+)",
                sqlite_method,
                flags=re.I,
            )
        )
    )
    fixture_sizes = (0, 1, 4000, 4001, 1024 * 1024 + 17)

    return {
        "schemaVersion": "spec164-exact-packet-v1-subject-v1",
        "subjectLabel": "legacy-exact-packet-path-before-artifact-manifest-v2",
        "claimLevel": "captured-not-benchmarked",
        "sourceFiles": [
            {
                "path": str(ORCHESTRATION.relative_to(ROOT)),
                "sha256": sha256_bytes(orchestration_bytes),
            },
            {
                "path": str(NATIVE_BINDING.relative_to(ROOT)),
                "sha256": sha256_bytes(native_bytes),
            },
        ],
        "symbolSha256": {
            **{
                name: sha256_text(source)
                for name, source in sorted(python_methods.items())
            },
            **{
                name: sha256_text(source)
                for name, source in sorted(cxx_functions.items())
            },
        },
        "observedBehavior": {
            "putFileDefaultChunkBytes": default_argument(put_file, "chunk_size"),
            "dataPacketMaxSegmentBytes": keyword_int(
                segmented_calls[0], "max_segment_size"
            ),
            "storeObjectAttempts": 3,
            "storeObjectControlOperations": string_operations(store_object),
            "packetRequestCarriesExactWire": all(
                token
                in python_methods[
                    "NetworkDistributedRepoClient._packet_to_request"
                ]
                for token in ("wireB64", "wireSha256", "packet.wire")
            ),
            "sqliteJournalMode": "WAL",
            "sqliteTables": sqlite_tables,
            "sqliteStoresPacketWireBlob": all(
                token in sqlite_method
                for token in (
                    "CREATE TABLE IF NOT EXISTS data_packets",
                    "wire BLOB NOT NULL",
                    "CREATE TABLE IF NOT EXISTS object_packet_refs",
                )
            ),
            "fileBundleSchema": "ndnsf-distributed-repo-file-bundle-v1",
        },
        "operationCountModel": {
            "putFileStoreObjectCalls": (
                "ceil(fileBytes / chunkBytes) file chunks + 1 root manifest; "
                "an empty file publishes only the root manifest"
            ),
            "capabilitySelectionCallsPerStoreAttempt": (
                "0 with explicit replicas or valid placement cache; otherwise 1"
            ),
            "reserveCallsPerStoreAttempt": (
                "replicationFactor when capacity reservations are enabled"
            ),
            "pushStoreCallsPerStoreAttempt": (
                "replicationFactor * signedDataPacketCount STORE_PACKET calls"
            ),
            "pullStoreCallsPerStoreAttempt": (
                "replicationFactor STORE_PACKET_PULL calls"
            ),
            "commitCallsPerSuccessfulStoreAttempt": (
                "replicationFactor COMMIT_PACKET_SET calls"
            ),
            "maximumStoreAttempts": 3,
        },
        "deterministicFixtures": [
            {
                "generator": "sha256-counter-v1",
                "size": size,
                "sha256": sha256_bytes(deterministic_fixture(size)),
            }
            for size in fixture_sizes
        ],
    }


def main() -> None:
    print(json.dumps(build_subject(), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
