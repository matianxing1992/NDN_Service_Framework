#!/usr/bin/env python3
"""Five-host MiniNDN qualification for the Spec192 production C++ Repo path.

The Python process only supervises MiniNDN. Repo service handlers and the
independent native client are the built C++ executables; the Python peer uses
the thin pybind API against those same services and receipt rows.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
from importlib import metadata
import json
import os
import re
import shlex
import shutil
import signal
import sqlite3
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple, Union

import spec193_preflight

REPO = Path(__file__).resolve().parents[2]
NO_PYTHON_PATH = REPO / "build/spec192-no-python-path"
NO_PYTHON_CPP_BINARIES = {"spec192-repo-client", "DistributedRepoServer",
                          "spec193-object-repo-server", "ndnsf-repo-import"}
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_DistributedRepo_Generic_Minindn as generic  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402

perf = generic.perf
CleanNlsr = generic.CleanNlsr
CONTROLLER_HOST = "memphis"
APP_HOST = "neu"
REPO_A_HOST = "ucla"
REPO_B_HOST = "wustl"
CONSUMER_HOST = "arizona"
REPO_C_HOST = CONSUMER_HOST
REPO_D_HOST = None
HOST_ROLES = {
    CONTROLLER_HOST: "controller",
    APP_HOST: "app",
    REPO_A_HOST: "repoA",
    REPO_B_HOST: "repoB",
    CONSUMER_HOST: "consumer",
}
GROUP = "/example/repo/group"
CONTROLLER = "/example/repo/controller"
USER = "/example/repo/user"
CONSUMER = "/example/repo/consumer"
REPO_A_PROVIDER = "/example/repo/provider/repoA"
REPO_B_PROVIDER = "/example/repo/provider/repoB"
REPO_C_PROVIDER = "/example/repo/provider/repoC"
REPO_D_PROVIDER = "/example/repo/provider/repoD"
DI_PROVIDER_ID = "spec192-di"
DI_PROVIDER = "/example/repo/provider/" + DI_PROVIDER_ID
DI_UNAUTHORIZED_ID = "spec192-di-unauthorized"
DI_UNAUTHORIZED_PROVIDER = "/example/repo/provider/" + DI_UNAUTHORIZED_ID
DI_REPO_CLIENT_IDENTITY = "/example/repo/di-authorized-reader"
DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY = "/example/repo/di-unauthorized-reader"
REPO_A_ID = "/example/repo/node/repoA"
REPO_B_ID = "/example/repo/node/repoB"
REPO_C_ID = "/example/repo/node/repoC"
REPO_D_ID = "/example/repo/node/repoD"
SERVICE_PREFIX = "/NDNSF/DistributedRepo/Object/v1"
DI_SERVICE = "/Spec192/DI/Infer"
DI_ROLE = "Inspect"
REPO_OPERATIONS = (
    "STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES", "INSERT",
    "STORE_MANIFEST", "FETCH", "FETCH_RANGE", "MANIFEST", "DATA_RECEIPT",
    "DATA_LOCATION", "INVENTORY",
    "CAPABILITY", "CACHE_STATUS", "STATUS", "CATALOG_STATUS",
    "CATALOG_SNAPSHOT", "CATALOG_DELTA", "CATALOG_LOOKUP", "DELETE",
)


def repo_acl_principals() -> Tuple[List[str], List[str]]:
    users = [USER, CONSUMER, REPO_A_PROVIDER, REPO_B_PROVIDER,
             REPO_C_PROVIDER]
    providers = [REPO_A_PROVIDER, REPO_B_PROVIDER, REPO_C_PROVIDER]
    if REPO_D_HOST is not None:
        users.append(REPO_D_PROVIDER)
        providers.append(REPO_D_PROVIDER)
    users.extend([DI_PROVIDER, DI_REPO_CLIENT_IDENTITY])
    return users, providers
REPO_OPERATION_TRACE_ENV = "NDNSF_SPEC192_REPO_OPERATION_TRACE"
SELECTED_REQUEST_TRACE_ENV = "NDNSF_SPEC192_SELECTED_REQUEST_TRACE"
SELECTED_REQUEST_TRACE_CHILD_ENV = "NDNSF_SPEC192_SELECTED_REQUEST_TRACE_CHILD"
NDN_LOG_TRACE_CHILD_ENV = "NDNSF_SPEC192_NDN_LOG_TRACE_CHILD"
NDN_LOG_TRACE_PROVIDER_ENV = "NDNSF_SPEC192_NDN_LOG_TRACE_PROVIDER"
SELECTED_REQUEST_TRACE_CHILDREN = {
    "cpp-publish-di-model",
    "cpp-publish-di-input",
    "cpp-publish-di-output",
    "spec192-di-request-authorized",
    "python-binding-fetch-cpp-artifact",
    "python-binding-query-cpp-native-receipt",
    "python-binding-publish-status-fetch",
    "cpp-client-fetch-python-receipt-no-python",
    "vm20-operation-begin-before-owner-restart",
}
NDN_LOG_TRACE_CHILDREN = {
    "cpp-publish-di-model",
    "cpp-publish-di-input",
    "cpp-publish-di-output",
    "python-binding-fetch-cpp-artifact",
    "python-binding-query-cpp-native-receipt",
    "python-binding-publish-status-fetch",
    "cpp-client-fetch-python-receipt-no-python",
    "spec192-di-request-unauthorized",
    "spec192-di-request-authorized",
    "vm20-operation-begin-before-owner-restart",
}
NDN_LOG_TRACE_PROVIDER_LABELS = {
    "cpp-repo-ucla",
    "cpp-repo-wustl",
    "cpp-repo-ucla-o17-original-seed",
    "cpp-repo-wustl-o17-original-seed",
    "spec192-di-provider-unauthorized",
    "spec192-di-provider-authorized",
}


def traced_provider_labels() -> set:
    return {label.strip() for label in
            os.environ.get(NDN_LOG_TRACE_PROVIDER_ENV, "").split(",")
            if label.strip()}


DI_PUBLISH_CHILD_TIMEOUT_S = 4200
DI_PUBLISH_CHILD_TIMEOUT_MARGIN_MS = 60000
REPO_SERVER_ACK_COLLECTION_MS = 1000
REPO_SERVER_REQUEST_TIMEOUT_MS = 30000
REPO_PREFIXES = ("NDNSF-", "ndn-service-framework/", "pythonWrapper/",
                 "tests/", "examples/", "Experiments/", "docs/", "specs/")
CANDIDATE_EVIDENCE_EXCLUDES = {
    "docs/failure-log.md",
    "specs/192-distributedrepo-lifecycle/audit.md",
    "specs/192-distributedrepo-lifecycle/traceability.md",
    "specs/192-distributedrepo-lifecycle/tasks.md",
    "specs/192-distributedrepo-lifecycle/validation-matrix.md",
}
CANDIDATE_GENERATED_PATH_COMPONENTS = {
    "build", "dist", "__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache",
}
CANDIDATE_GENERATED_SUFFIXES = {
    ".a", ".dylib", ".dll", ".o", ".pyc", ".pyo", ".sif", ".so", ".whl",
}


def hide_python_path(environment: Dict[str, str]) -> Dict[str, str]:
    NO_PYTHON_PATH.mkdir(parents=True, exist_ok=True)
    if any(NO_PYTHON_PATH.iterdir()):
        raise RuntimeError("Spec192 no-Python PATH must remain empty")
    isolated = dict(environment)
    isolated["PATH"] = str(NO_PYTHON_PATH)
    return isolated


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def runtime_libraries(ldd_output: str) -> List[Dict[str, Any]]:
    libraries = {}
    for line in ldd_output.splitlines():
        entry = line.strip()
        if "=>" in entry:
            target = entry.split("=>", 1)[1].strip().split()[0]
            if target == "not":
                continue
        elif entry.startswith("/"):
            target = entry.split()[0]
        else:
            continue
        path = Path(target).resolve()
        if not path.is_file():
            raise RuntimeError("ldd runtime library is not a regular file: " + target)
        libraries[str(path)] = {
            "sha256": digest(path),
            "size_bytes": path.stat().st_size,
        }
    return [{"path": path, **libraries[path]} for path in sorted(libraries)]


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def read_log(path: Path) -> str:
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        return stream.read()


def source_candidate() -> Tuple[str, List[str]]:
    tracked = subprocess.check_output(
        ["git", "diff", "--name-only", "-z", "HEAD"], cwd=str(REPO)
    ).decode().split("\0")
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=str(REPO)
    ).decode().split("\0")
    tracked_names = {name for name in tracked if name}
    candidates = sorted({name for name in tracked + untracked if name and
                         name.startswith(REPO_PREFIXES) and
                         name not in CANDIDATE_EVIDENCE_EXCLUDES and
                         not (set(Path(name).parts) & CANDIDATE_GENERATED_PATH_COMPONENTS) and
                         Path(name).suffix.lower() not in CANDIDATE_GENERATED_SUFFIXES and
                         ((REPO / name).is_file() or name in tracked_names)})
    state = hashlib.sha256()
    for name in candidates:
        state.update(name.encode("utf-8"))
        state.update(b"\0")
        path = REPO / name
        if path.is_file():
            state.update(b"file\0")
            state.update(hashlib.sha256(path.read_bytes()).digest())
        else:
            state.update(b"deleted\0")
        state.update(b"\n")
    return state.hexdigest(), candidates


def validate_di_publish_child_timeout() -> None:
    runner_source = Path(__file__).read_text(encoding="utf-8")
    client_source = (REPO / "tests/standalone/spec192-repo-client.cpp").read_text(
        encoding="utf-8")
    api_source = (REPO / "NDNSF-DistributedRepo/src/ArtifactRepositoryApi.cpp").read_text(
        encoding="utf-8")
    limits_source = (REPO / "NDNSF-DistributedRepo/include/ndnsf-distributed-repo/ArtifactTypes.hpp").read_text(
        encoding="utf-8")
    publish_mode = re.search(
        r'if\s*\(\s*mode\s*==\s*"publish"\s*\)\s*\{(?P<body>[^}]+)\}',
        client_source, re.S)
    publish_function = re.search(
        r'repo::ArtifactPublishResult\s+publish\s*\([^)]*\)\s*\{(?P<body>.*?)\n\}',
        client_source, re.S)
    api_function = re.search(
        r'ArtifactPublishResult\s+ArtifactRepositoryApi::publishFile\s*\('
        r'\s*const std::string&\s+sourcePath,\s*'
        r'const ArtifactReference&\s+reference,\s*'
        r'const SelectedRepoSet&\s+selected,\s*int timeoutMs\s*\)\s*'
        r'\{(?P<body>.*?)\n\}',
        api_source, re.S)
    begin_api_function = re.search(
        r'PublishOperationHandle\s+ArtifactRepositoryApi::beginPublishFileImpl\s*'
        r'\([^)]*\)\s*\{(?P<body>.*?)\n\}',
        api_source, re.S)
    reserve_function = re.search(
        r'int64_t\s+statusRecoveryReserveTimeoutMs\s*\([^)]*\)\s*'
        r'\{(?P<body>.*?)\n\}', api_source, re.S)
    bounded_request_function = re.search(
        r'int\s+boundedRequestTimeoutMs\s*\([^)]*\)\s*'
        r'\{(?P<body>.*?)\n\}', api_source, re.S)
    status_until_function = re.search(
        r'PublishOperationStatus\s+ArtifactRepositoryApi::getPublishStatusUntil\s*'
        r'\([^)]*\)\s*const\s*\{(?P<body>.*?)\n\}', api_source, re.S)
    begin_archive_function = re.search(
        r'PublishOperationHandle\s+ArtifactRepositoryApi::beginArchive\s*'
        r'\([^)]*\)\s*\{(?P<body>.*?)\n\}', api_source, re.S)
    if (publish_mode is None or publish_function is None or api_function is None or
            begin_api_function is None or reserve_function is None or
            bounded_request_function is None or status_until_function is None or
            begin_archive_function is None):
        raise RuntimeError("cannot derive the native Spec192 publish timeout bounds")

    reserve_body = reserve_function.group("body")
    normalized_reserve_body = " ".join(reserve_body.split())
    if ("config.requestTimeoutMs" not in reserve_body or
            "config.ackCollectionTimeMs" not in reserve_body or
            "requestAndAckWindowMs" not in reserve_body or
            "2LL * static_cast<int64_t>(config.ackCollectionTimeMs)" not in reserve_body or
            re.search(
                r"std::max<int64_t>\(\s*\{\s*60'000,\s*requestAndAckWindowMs,\s*"
                r"twoAckWindowsMs\s*\}\s*\)", normalized_reserve_body) is None):
        raise RuntimeError("shared STATUS recovery reserve formula changed; re-review begin budgets")

    recovery_query = re.search(
        r'const auto seedWriteFailure\s*=.*?'
        r'queryPublishOperationStatus\(\s*\*m_impl->serviceUser,\s*'
        r'config,\s*handle,\s*(?P<timeout>.*?)\s*\);',
        begin_api_function.group("body"), re.S)
    if (recovery_query is None or
            "remainingRequestTimeout()" not in recovery_query.group("timeout") or
            "timeoutMs" in recovery_query.group("timeout")):
        raise RuntimeError(
            "ambiguous seed STORE status recovery must use the original begin deadline")
    begin_body = begin_api_function.group("body")
    if ("statusRecoveryReserveTimeoutMs(config)" not in begin_body or
            "boundedRequestTimeoutMs(deadline, config.requestTimeoutMs)" not in begin_body or
            "std::min(configuredTimeoutMs, remainingTimeoutMs(deadline))" not in
            " ".join(bounded_request_function.group("body").split())):
        raise RuntimeError(
            "begin STATUS/discovery/intent requests must be capped by requestTimeoutMs and the same deadline")
    seed_write_budget = re.search(
        r"const auto remainingForSeedWrite\s*=\s*\[&\]\s*\{\s*"
        r"const int timeout\s*=\s*remaining\(\);\s*"
        r"if\s*\(\s*timeout\s*<=\s*statusRecoveryReserveMs\s*\+\s*"
        r"config\.ackCollectionTimeMs\s*\)\s*\{\s*"
        r"throw std::runtime_error\(\s*"
        r"\"repo-operation-timeout: insufficient begin budget for seed write and status recovery\"\s*\);\s*"
        r"\}\s*"
        r"return static_cast<int>\(\s*std::min<int64_t>\(\s*"
        r"config\.requestTimeoutMs,\s*static_cast<int64_t>\(timeout\)\s*-\s*"
        r"statusRecoveryReserveMs\s*\)\s*\);\s*"
        r"\};",
        begin_body)
    expected_seed_write_calls = (
        ("STORE", "write"),
        ("STORE_RANGE", "write"),
        ("COMMIT_RANGES", "commit"),
    )
    seed_write_calls_bound = all(
        re.search(
            r"runSelectedRepoRequest\s*\(\s*\*m_impl->serviceUser,\s*"
            r"\*seedTarget,\s*makeRepoServiceName\s*\(\s*"
            r"seedTarget->servicePrefix\s*,\s*\"{}\"\s*\)\s*,\s*"
            r"{}\.toWire\(config\.limits\)\s*,\s*config\s*,\s*"
            r"remainingForSeedWrite\(\)\s*\)".format(operation, payload),
            begin_body, re.S) is not None
        for operation, payload in expected_seed_write_calls)
    if (seed_write_budget is None or
            not seed_write_calls_bound or
            begin_body.count("remainingForSeedWrite()") != 3 or
            begin_body.count("remainingRequestTimeout()") < 5):
        raise RuntimeError(
            "seed write timeout arithmetic and STATUS recovery call binding changed; review the begin deadline")

    sync_status_reserve = re.search(
        r'const int64_t statusRecoveryReserveMs\s*=\s*'
        r'statusRecoveryReserveTimeoutMs\(config\)\s*;', api_function.group("body"))
    status_until_body = status_until_function.group("body")
    if (sync_status_reserve is None or
            "try_lock()" not in status_until_body or
            "boundedRequestTimeoutMs(deadline, requestTimeoutMs)" not in status_until_body or
            "statusTimeoutMs <= config.ackCollectionTimeMs" not in status_until_body or
            "repo-operation-timeout: status deadline leaves insufficient ACK window" not in
            status_until_body or
            "getPublishStatusUntil(handle, deadline, config.requestTimeoutMs)" not in
            api_function.group("body")):
        raise RuntimeError(
            "post-accept STATUS lock wait, ACK floor and request must share the caller wait deadline")

    archive_body = begin_archive_function.group("body")
    archive_start = archive_body.find("std::chrono::steady_clock::now()")
    archive_lookup = archive_body.find("inAppSource.lookupLocalCommitUntil(")
    archive_fetch = archive_body.find("inAppSource.fetchFileUntil(")
    archive_lookup_deadline_bound = re.search(
        r"inAppSource\.lookupLocalCommitUntil\(\s*handle\.reference\s*,\s*"
        r"deadline\s*\)", archive_body, re.S)
    archive_fetch_deadline_bound = re.search(
        r"inAppSource\.fetchFileUntil\(\s*handle\.reference\s*,\s*"
        r"temporaryPayload\.string\(\)\s*,\s*deadline\s*\)",
        archive_body, re.S)
    archive_pre_fetch_deadline_check = archive_body.find(
        "std::chrono::steady_clock::now() >= deadline", archive_lookup)
    archive_deadline_check = archive_body.find(
        "std::chrono::steady_clock::now() >= deadline", archive_fetch)
    archive_begin = archive_body.find("beginPublishFileImpl(")
    archive_begin_call = re.search(
        r'beginPublishFileImpl\(\s*temporaryPayload\.string\(\)\s*,\s*'
        r'handle\.reference\s*,\s*selected\s*,\s*idempotencyKey\s*,\s*'
        r'deadline\s*,\s*nullptr\s*\)', archive_body, re.S)
    if (not (0 <= archive_start < archive_lookup <
             archive_pre_fetch_deadline_check < archive_fetch <
             archive_deadline_check < archive_begin) or
            archive_lookup_deadline_bound is None or
            archive_fetch_deadline_bound is None or
            archive_begin_call is None or
            "inAppSource.lookupLocalCommit(" in archive_body or
            "inAppSource.fetchFile(" in archive_body):
        raise RuntimeError(
            "beginArchive must pass its original deadline to In-App lookup/fetch and Persistent begin")

    request_match = re.search(
        r'config\.requestTimeoutMs\s*=\s*(\d+)\s*;', publish_mode.group("body"))
    call_match = re.search(
        r'api\.publishFile\s*\(\s*required\(options,\s*"source"\)\s*,\s*'
        r'reference\s*,\s*selected\s*,\s*(\d+)\s*\)',
        publish_function.group("body"), re.S)
    begin_match = re.search(
        r'const int beginTimeout\s*=\s*(.*?);', api_function.group("body"), re.S)
    wait_match = re.search(
        r'const int waitBudget\s*=\s*(.*?);', api_function.group("body"), re.S)
    if request_match is None or call_match is None or begin_match is None or wait_match is None:
        raise RuntimeError("cannot derive the native Spec192 publish timeout bounds")

    request_timeout_ms = int(request_match.group(1))
    publish_timeout_ms = int(call_match.group(1))
    begin_expression = " ".join(begin_match.group(1).split())
    wait_expression = " ".join(wait_match.group(1).split())
    if ("requestWindows" not in begin_expression or
            "requestWindowMs" not in begin_expression or
            "statusRecoveryReserveMs" not in begin_expression or
            "timeoutMs == 0" not in wait_expression or
            "config.operationWaitTimeoutMs" not in wait_expression or
            not wait_expression.endswith(": timeoutMs")):
        raise RuntimeError("publish timeout structure changed; review the MiniNDN child budget")

    begin_budget_ms = max(publish_timeout_ms, request_timeout_ms)
    profile_ack = re.search(
        r'config\.ackCollectionTimeMs\s*=\s*(\d+)\s*;', publish_mode.group("body"))
    profile_wait = re.search(
        r'config\.operationWaitTimeoutMs\s*=\s*(\d+)\s*;', publish_mode.group("body"))
    pre_seed = re.search(r'PRE_SEED_REQUESTS\s*=\s*(\d+)', api_function.group("body"))
    local_margin = re.search(
        r'LOCAL_WORK_MARGIN_WINDOWS\s*=\s*(\d+)', api_function.group("body"))
    request_windows_formula = re.search(
        r'requestWindows\s*=\s*PRE_SEED_REQUESTS\s*\+\s*'
        r'seedWriteRequests\s*\+\s*LOCAL_WORK_MARGIN_WINDOWS\s*;',
        api_function.group("body"))
    if (profile_ack is None or profile_wait is None or pre_seed is None or
            local_margin is None or request_windows_formula is None or
            "statusRecoveryReserveTimeoutMs(config)" not in api_function.group("body") or
            "statusRecoveryReserveTimeoutMs(config)" not in begin_body):
        raise RuntimeError("cannot prove shared, source-derived begin and caller wait budgets")

    try:
        runner_tree = ast.parse(runner_source)
        payload_assignment = next(
            node for node in ast.walk(runner_tree)
            if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == "di_payloads"
                for target in node.targets))
        di_payloads = ast.literal_eval(payload_assignment.value)
    except (StopIteration, SyntaxError, ValueError, TypeError) as error:
        raise RuntimeError("cannot derive the exact inline Spec192 DI payload sizes") from error
    if not isinstance(di_payloads, dict) or not di_payloads or any(
            not isinstance(payload, bytes) for payload in di_payloads.values()):
        raise RuntimeError("Spec192 DI publish source set is no longer a literal byte map")

    def cpp_product(name: str, source: str) -> int:
        match = re.search(
            r'constexpr\s+(?:size_t|uint64_t)\s+' + re.escape(name) +
            r'\s*=\s*([^;]+);', source)
        if match is None:
            raise RuntimeError("cannot derive native chunk limit " + name)
        expression = re.sub(r'(?:ULL|UL|LL|U|L)\b', '', match.group(1)).strip()
        if re.fullmatch(r'[0-9\s*]+', expression) is None:
            raise RuntimeError("native chunk limit expression changed: " + name)
        result = 1
        for factor in expression.split("*"):
            result *= int(factor.strip())
        return result

    def cpp_limit(name: str, source: str) -> int:
        match = re.search(
            r'\b(?:uint64_t|uint32_t)\s+' + re.escape(name) +
            r'\s*=\s*([^;]+);', source)
        if match is None:
            raise RuntimeError("cannot derive artifact limit " + name)
        expression = re.sub(r'(?:ULL|UL|LL|U|L)\b', '', match.group(1)).strip()
        if re.fullmatch(r'[0-9\s*]+', expression) is None:
            raise RuntimeError("artifact limit expression changed: " + name)
        result = 1
        for factor in expression.split("*"):
            result *= int(factor.strip())
        return result

    chunk_bytes = min(
        cpp_product("COPY_CHUNK_BYTES", api_source),
        cpp_product("MAX_STORE_RANGE_BYTES", api_source),
        cpp_limit("maxChunkBytes", limits_source),
        max(1, cpp_limit("maxPacketPayloadBytes", limits_source) // 2))
    ack_window_ms = int(profile_ack.group(1))
    request_window_ms = request_timeout_ms + ack_window_ms
    status_reserve_ms = max(
        60_000, request_window_ms, 2 * ack_window_ms)
    seed_write_requests = 0
    for payload in di_payloads.values():
        if len(payload) <= chunk_bytes:
            requests = 1
        else:
            range_chunks = (len(payload) + chunk_bytes - 1) // chunk_bytes
            requests = range_chunks + 1
        seed_write_requests = max(seed_write_requests, requests)
    request_window_count = (
        int(pre_seed.group(1)) + seed_write_requests +
        int(local_margin.group(1)))
    begin_budget_ms = request_window_count * request_window_ms + status_reserve_ms
    wait_budget_ms = int(profile_wait.group(1))
    if publish_timeout_ms not in (0, wait_budget_ms):
        raise RuntimeError("standalone publish wait no longer matches its operation-wait profile")
    if "requestWindows) * requestWindowMs + statusRecoveryReserveMs" not in begin_expression:
        raise RuntimeError("native begin budget no longer uses request windows plus shared recovery reserve")

    required_timeout_ms = (begin_budget_ms + wait_budget_ms +
                           DI_PUBLISH_CHILD_TIMEOUT_MARGIN_MS)

    try:
        timeout_tree = ast.parse(runner_source)
    except SyntaxError as error:
        raise RuntimeError("cannot parse MiniNDN child launch arguments") from error
    di_publish_calls = []
    for node in ast.walk(timeout_tree):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Name) or \
                node.func.id != "run_child" or len(node.args) < 8:
            continue
        child_name = node.args[4]
        if (isinstance(child_name, ast.BinOp) and
                isinstance(child_name.left, ast.Constant) and
                isinstance(child_name.left.value, str) and
                child_name.left.value.startswith("cpp-publish-di-")):
            di_publish_calls.append(node)
    if (len(di_publish_calls) != 1 or
            not isinstance(di_publish_calls[0].args[7], ast.Name) or
            di_publish_calls[0].args[7].id != "DI_PUBLISH_CHILD_TIMEOUT_S"):
        raise RuntimeError(
            "each DI publish child must pass DI_PUBLISH_CHILD_TIMEOUT_S as its actual run_child timeout")

    child_timeout_ms = DI_PUBLISH_CHILD_TIMEOUT_S * 1000
    if child_timeout_ms < required_timeout_ms:
        raise RuntimeError(
            "DI publish child timeout {}ms is below native publish budget {}ms "
            "plus margin {}ms".format(
                child_timeout_ms, begin_budget_ms + wait_budget_ms,
                DI_PUBLISH_CHILD_TIMEOUT_MARGIN_MS))


def checked_node_cmd(node, command: str) -> str:
    output = perf.node_cmd(node, command + "; __spec192_rc=$?; echo __SPEC192_RC__$__spec192_rc")
    marker = "__SPEC192_RC__"
    if marker not in output:
        raise RuntimeError("node command returned no exit marker: " + command)
    body, rc_text = output.rsplit(marker, 1)
    try:
        rc = int(rc_text.strip().splitlines()[0])
    except (ValueError, IndexError):
        raise RuntimeError("node command returned malformed exit marker: " + output[-400:])
    if rc != 0:
        raise RuntimeError("node command failed rc={} command={} output={}".format(
            rc, command, body[-1000:]))
    return body


def set_host_ndn_config(host) -> Path:
    home = Path(host.params["params"]["homeDir"])
    ndn_dir = home / ".ndn"
    shutil.rmtree(ndn_dir, ignore_errors=True)
    ndn_dir.mkdir(parents=True, exist_ok=True)
    (ndn_dir / "client.conf").write_text(
        "transport=unix:///run/nfd/{}.sock\n".format(host.name), encoding="utf-8")
    return home


def generate_identities(ndn, output: Path) -> Dict[str, Any]:
    identities = {
        CONTROLLER_HOST: CONTROLLER,
        APP_HOST: USER,
        REPO_A_HOST: REPO_A_PROVIDER,
        REPO_B_HOST: REPO_B_PROVIDER,
        CONSUMER_HOST: CONSUMER,
    }
    root_identity = "/example/repo"
    root_cert = output / "identity" / "root.cert"
    root_cert.parent.mkdir(parents=True, exist_ok=True)
    controller = ndn.net[CONTROLLER_HOST]
    checked_node_cmd(
        controller,
        "ndnsec key-gen -t r {} > {}".format(
            perf.shell_quote(root_identity), perf.shell_quote(root_cert)))
    checked_node_cmd(controller, "ndnsec cert-install -f {}".format(perf.shell_quote(root_cert)))

    certs: Dict[str, Path] = {}
    for index, (host_name, identity) in enumerate(identities.items()):
        request = output / "identity" / (host_name + ".req")
        cert = output / "identity" / (host_name + ".cert")
        checked_node_cmd(
            ndn.net[host_name],
            "ndnsec key-gen -t r {} > {}".format(
                perf.shell_quote(identity), perf.shell_quote(request)))
        checked_node_cmd(
            controller,
            "ndnsec cert-gen -s {} -i T010-{} {} > {}".format(
                perf.shell_quote(root_identity), index,
                perf.shell_quote(request), perf.shell_quote(cert)))
        certs[host_name] = cert

    identity_cert_names: Dict[str, str] = {}
    for host_name, identity in identities.items():
        host = ndn.net[host_name]
        # Peers load the public root certificate from the trust-schema anchor;
        # their PIB only needs the leaf certificate matching their own key.
        checked_node_cmd(host, "ndnsec cert-install -f {}".format(perf.shell_quote(certs[host_name])))
        cert_name = perf.certificate_name_from_file(certs[host_name])
        identity_cert_names[host_name] = cert_name
        checked_node_cmd(host, "ndnsec set-default -n {}".format(perf.shell_quote(identity)))
        checked_node_cmd(host, "ndnsec set-default -c -n {}".format(perf.shell_quote(cert_name)))

    extra_identity_hosts = {
        DI_PROVIDER: APP_HOST,
        DI_UNAUTHORIZED_PROVIDER: APP_HOST,
        REPO_C_PROVIDER: REPO_C_HOST,
        DI_REPO_CLIENT_IDENTITY: APP_HOST,
        DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY: APP_HOST,
    }
    if REPO_D_HOST is not None:
        extra_identity_hosts[REPO_D_PROVIDER] = REPO_D_HOST
    extra_certificates: Dict[str, str] = {}
    extra_certificate_files: Dict[str, str] = {}
    for index, (identity, host_name) in enumerate(
            extra_identity_hosts.items(), start=len(identities)):
        request = output / "identity" / ("extra-{}.req".format(index))
        cert = output / "identity" / ("extra-{}.cert".format(index))
        checked_node_cmd(
            ndn.net[host_name],
            "ndnsec key-gen -t r {} > {}".format(
                perf.shell_quote(identity), perf.shell_quote(request)))
        checked_node_cmd(
            controller,
            "ndnsec cert-gen -s {} -i T010-{} {} > {}".format(
                perf.shell_quote(root_identity), index,
                perf.shell_quote(request), perf.shell_quote(cert)))
        checked_node_cmd(ndn.net[host_name],
                         "ndnsec cert-install -f {}".format(perf.shell_quote(cert)))
        extra_certificates[identity] = perf.certificate_name_from_file(cert)
        extra_certificate_files[identity] = str(cert)

    # The Controller encrypts permission responses to each target identity.
    # Keep private keys on their owner nodes, but import the signed public
    # leaves into the Controller's PIB so its permission handlers can resolve
    # the target RSA certificate locally.
    target_public_certificates = {
        identity: str(certs[host_name])
        for host_name, identity in identities.items()
        if host_name != CONTROLLER_HOST
    }
    target_public_certificates.update(extra_certificate_files)

    # Leaf private keys remain in their owner host's PIB; only the public root
    # and leaf certificates are shared by the NDN trust and certificate paths.
    return {
        "root_identity": root_identity,
        "root_certificate": str(root_cert),
        "controller_certificate": identity_cert_names[CONTROLLER_HOST],
        "controller_certificate_file": str(certs[CONTROLLER_HOST]),
        "identities": identities,
        "additional_identities": {
            "hosts": extra_identity_hosts,
            "certificate_names": extra_certificates,
        },
        "target_public_certificates": target_public_certificates,
        "private_key_owners": sorted(set(identities) | set(extra_identity_hosts.values())),
    }


def make_runtime_policy(source: Path, destination: Path, root_cert: Path,
                        metadata_denied_user: Optional[str] = None) -> None:
    import yaml

    config = yaml.safe_load(source.read_text(encoding="utf-8"))
    repo_users, provider_identities = repo_acl_principals()
    repo_providers = [{"identity": identity, "roles": []}
                      for identity in provider_identities]
    # The authorized and unauthorized DI Repo clients are distinct from both
    # the requester and service Provider. Keep the unauthorized principal out
    # of this ACL so the negative VM-14/15 case exercises Repo authorization.
    config["trust"]["anchor_file"] = str(root_cert)
    config["runtime"]["user_identity"] = CONSUMER
    config["identities"]["user"] = CONSUMER
    config["services"] = [
        {
            "name": SERVICE_PREFIX + "/" + operation,
            "model": SERVICE_PREFIX + "/" + operation,
            "roles": [],
            "dependencies": [],
            "users": ([user for user in repo_users if user != metadata_denied_user]
                      if operation in ("DATA_RECEIPT", "DATA_LOCATION")
                      else repo_users),
            "providers": repo_providers,
        }
        for operation in REPO_OPERATIONS
    ]
    config["services"].append({
        "name": DI_SERVICE,
        "model": "/Spec192/DI/Model",
        "roles": [DI_ROLE],
        "dependencies": [],
        "users": [CONSUMER],
        "providers": [
            {"identity": DI_PROVIDER, "roles": [DI_ROLE]},
            {"identity": DI_UNAUTHORIZED_PROVIDER, "roles": [DI_ROLE]},
        ],
        "artifacts": [{
            "role": DI_ROLE,
            "path": "",
            "artifact": "/Spec192/DI/model",
            "filename": "model.bin",
            "kind": "onnx-model",
            "backend": "onnxruntime",
        }],
        "input": {"codec": "bytes"},
        "output": {"codec": "bytes"},
    })
    write_json(destination.with_suffix(".json"), config)
    destination.write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")


def node_environment(host, repo_root: Path, *, no_python: bool = False) -> Dict[str, str]:
    home = str(Path(host.params["params"]["homeDir"]))
    env = dict(os.environ)
    for name in ("NDN_LOG", SELECTED_REQUEST_TRACE_ENV,
                 SELECTED_REQUEST_TRACE_CHILD_ENV, NDN_LOG_TRACE_CHILD_ENV,
                 NDN_LOG_TRACE_PROVIDER_ENV):
        env.pop(name, None)
    env.update({
        "HOME": home,
        "NDN_CLIENT_CONF": str(Path(home) / ".ndn/client.conf"),
        "NDN_CLIENT_TRANSPORT": "unix:///run/nfd/{}.sock".format(host.name),
        "LD_LIBRARY_PATH": ":".join(filter(None, [str(repo_root / "build"),
                                                        str(repo_root / "NDNSF-DistributedRepo/pythonWrapper/build/lib.linux-x86_64-3.8"),
                                                        env.get("LD_LIBRARY_PATH", "")])),
        "PYTHONPATH": ":".join(filter(None, [str(repo_root / "NDNSF-DistributedInference"),
                                                  str(repo_root / "NDNSF-DistributedRepo/pythonWrapper"),
                                                  str(repo_root / "pythonWrapper"),
                                                  env.get("PYTHONPATH", "")])),
        "PYTHONUNBUFFERED": "1",
    })
    return hide_python_path(env) if no_python else env


def repo_server_environment(environment: Dict[str, str], label: str) -> Dict[str, str]:
    traced = dict(environment)
    traced[REPO_OPERATION_TRACE_ENV] = "1"
    if label in traced_provider_labels():
        traced["NDN_LOG"] = (
            "ndn_service_framework.ServiceProvider=TRACE:ndn_svs.SVSPubSub=TRACE")
    return traced


def repo_request_events(processes: List[Dict[str, Any]],
                        repo_nodes: Optional[set] = None) -> List[Dict[str, str]]:
    marker = "SPEC192_REPO_REQUEST "
    events = []
    for process in processes:
        repo_node = process.get("repo_node")
        if repo_node is None or (repo_nodes is not None and repo_node not in repo_nodes):
            continue
        for line in read_log(process["log"]).splitlines():
            offset = line.find(marker)
            if offset < 0:
                continue
            fields = line[offset + len(marker):].split()
            if len(fields) != 2 or not fields[0].startswith("service=") or \
                    not fields[1].startswith("requester="):
                raise RuntimeError("malformed authenticated Repo request trace: " + line)
            service = fields[0].split("=", 1)[1]
            events.append({
                "repo_node": repo_node,
                "service": service,
                "operation": service.rsplit("/", 1)[-1],
                "requester": fields[1].split("=", 1)[1],
                "process": process["label"],
            })
    return events


def repo_store_events(processes: List[Dict[str, Any]],
                      repo_nodes: Optional[set] = None) -> List[Dict[str, str]]:
    return [event for event in repo_request_events(processes, repo_nodes)
            if event["operation"] in {"STORE", "STORE_RANGE", "COMMIT_RANGES"}]


def spawn(ndn, output: Path, processes: List[Dict[str, Any]], host_name: str,
          label: str, argv: List[str], env: Dict[str, str]):
    requires_no_python = bool(argv) and (
        Path(argv[0]).name in NO_PYTHON_CPP_BINARIES
        or (Path(argv[0]).name == "strace" and any(
            Path(argument).name in NO_PYTHON_CPP_BINARIES
            for argument in argv[1:])))
    child_env = env
    launch_env = env
    command = "exec " + shlex.join(argv)
    if requires_no_python:
        child_env = hide_python_path(env)
        # Mininet resolves mnexec with the launch PATH; clear PATH after entry.
        launch_env = dict(child_env)
        launch_env["PATH"] = os.environ.get("PATH") or os.defpath
        command = "exec /usr/bin/env PATH={} {}".format(
            shlex.quote(child_env["PATH"]), shlex.join(argv))
    log_path = output / "logs" / (label + ".log")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    stream = log_path.open("wb")
    proc = generic.getPopen(ndn.net[host_name], command, envDict=launch_env,
                            shell=True, stdout=stream, stderr=subprocess.STDOUT)
    record = {"host": host_name, "label": label, "proc": proc,
              "stream": stream, "log": log_path, "argv": argv,
              "started_at_unix": time.time(), "pid": proc.pid,
              "requires_no_python": requires_no_python,
              "path_hides_python": child_env.get("PATH") == str(NO_PYTHON_PATH),
              "python_mapped": False, "python_maps_observed": False}
    processes.append(record)
    return record


def assert_no_python_mapped(record: Dict[str, Any], timeout_s: float = 2.0) -> None:
    maps = Path("/proc/{}/maps".format(record["proc"].pid))
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if maps.is_file():
            record["python_maps_observed"] = True
            record["python_mapped"] = "libpython" in maps.read_text(errors="replace").lower()
            if record["python_mapped"]:
                raise RuntimeError("standalone C++ Repo process loaded libpython: " + record["label"])
            return
        if record["proc"].poll() is not None:
            break
        time.sleep(0.01)
    raise RuntimeError("could not observe standalone C++ Repo process mappings: " + record["label"])


def run_child(ndn, output: Path, processes: List[Dict[str, Any]], host_name: str,
              label: str, argv: List[str], env: Dict[str, str], timeout_s: float,
              *, require_no_python: bool = False) -> Dict[str, Any]:
    trace_children = {
        child.strip()
        for child in os.environ.get(SELECTED_REQUEST_TRACE_CHILD_ENV, "").split(",")
        if child.strip()
    }
    log_children = {
        child.strip()
        for child in os.environ.get(NDN_LOG_TRACE_CHILD_ENV, "").split(",")
        if child.strip()
    }
    env = dict(env)
    for name in ("NDN_LOG", SELECTED_REQUEST_TRACE_ENV,
                 SELECTED_REQUEST_TRACE_CHILD_ENV, NDN_LOG_TRACE_CHILD_ENV,
                 NDN_LOG_TRACE_PROVIDER_ENV):
        env.pop(name, None)
    if label in trace_children:
        env[SELECTED_REQUEST_TRACE_ENV] = "1"
    if label in log_children:
        trace_levels = {
            "cpp-publish-di-model": "ndn_service_framework.ServiceUser=TRACE",
            "cpp-publish-di-input": "ndn_service_framework.ServiceUser=TRACE",
            "cpp-publish-di-output": "ndn_service_framework.ServiceUser=TRACE",
            "python-binding-fetch-cpp-artifact": "ndn_service_framework.ServiceUser=TRACE",
            "python-binding-query-cpp-native-receipt": "ndn_service_framework.*=TRACE",
            "python-binding-publish-status-fetch": "ndn_service_framework.ServiceUser=TRACE",
            "cpp-client-fetch-python-receipt-no-python": (
                "ndn_service_framework.ServiceUser=TRACE:ndn_svs.SVSPubSub=TRACE"),
            "spec192-di-request-unauthorized": "*=TRACE",
            "spec192-di-request-authorized": (
                "ndn_service_framework.ServiceUser=TRACE:ndn_svs.SVSPubSub=TRACE"),
            "vm20-operation-begin-before-owner-restart": "ndn_service_framework.ServiceUser=TRACE",
        }
        env["NDN_LOG"] = trace_levels[label]
        if "ndn_svs.SVSPubSub" not in env["NDN_LOG"]:
            env["NDN_LOG"] += ":ndn_svs.SVSPubSub=TRACE"
    record = spawn(ndn, output, processes, host_name, label, argv, env)
    require_no_python = require_no_python or record["requires_no_python"]
    if require_no_python and not record["path_hides_python"]:
        raise RuntimeError("standalone C++ Repo client PATH exposes Python: " + label)
    proc = record["proc"]
    python_mapped = False
    python_maps_observed = False
    if require_no_python:
        maps = Path("/proc/{}/maps".format(proc.pid))
        map_deadline = time.monotonic() + min(timeout_s, 2.0)
        while time.monotonic() < map_deadline:
            if maps.is_file():
                python_maps_observed = True
                python_mapped = "libpython" in maps.read_text(errors="replace").lower()
                break
            if proc.poll() is not None:
                break
            time.sleep(0.01)
    record["python_mapped"] = python_mapped
    record["python_maps_observed"] = python_maps_observed
    try:
        rc = proc.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        record["stream"].close()
        raise RuntimeError("child timed out: {} log={}".format(label, record["log"]))
    record["stream"].close()
    result = {
        "host": host_name,
        "label": label,
        "pid": record["pid"],
        "argv": argv,
        "exit_code": rc,
        "log": str(record["log"]),
        "python_mapped": python_mapped,
        "python_maps_observed": python_maps_observed,
        "path_hides_python": record["path_hides_python"],
    }
    if rc != 0:
        tail = read_log(record["log"])[-3000:]
        raise RuntimeError("{} exited {}; log tail:\n{}".format(label, rc, tail))
    if require_no_python and python_mapped:
        raise RuntimeError("C++ Repo client loaded libpython")
    return result


def stop_daemon(record: Dict[str, Any], timeout_s: float = 8.0) -> Dict[str, Any]:
    proc = record["proc"]
    if proc.poll() is None:
        proc.terminate()
        try:
            rc = proc.wait(timeout=timeout_s)
            method = "SIGTERM"
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = proc.wait(timeout=5)
            method = "SIGKILL-fallback"
    else:
        rc = proc.returncode
        method = "already-exited"
    if not record["stream"].closed:
        record["stream"].close()
    return {"host": record["host"], "label": record["label"],
            "pid": record["pid"], "exit_code": rc, "stop_method": method,
            "log": str(record["log"])}


def stop_repo_server(record: Dict[str, Any], timeout_s: float = 8.0) -> Dict[str, Any]:
    stopped = stop_daemon(record, timeout_s)
    if (stopped["stop_method"] != "SIGTERM" or
            stopped["exit_code"] not in (0, -signal.SIGTERM)):
        raise RuntimeError("Repo service did not stop cleanly: {}".format(stopped))
    return stopped


def stop_capture(record: Dict[str, Any]) -> Dict[str, Any]:
    proc = record["proc"]
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            rc = proc.wait(timeout=10)
            method = "SIGINT"
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = proc.wait(timeout=5)
            method = "SIGKILL-fallback"
    else:
        rc = proc.returncode
        method = "already-exited"
    if not record["stream"].closed:
        record["stream"].close()
    capture_path = record["capture_path"]
    summary = ""
    if capture_path.is_file():
        traced = subprocess.run(
            ["tcpdump", "-n", "-r", str(capture_path), "udp"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
            check=False,
        )
        summary = traced.stderr.strip().splitlines()[-1] if traced.stderr.strip() else ""
    if rc != 0 or method == "SIGKILL-fallback" or not capture_path.is_file():
        raise RuntimeError("per-node NDN trace did not close cleanly: {} rc={} {}".format(
            record["host"], rc, summary))
    return {"host": record["host"], "path": str(capture_path),
            "bytes": capture_path.stat().st_size, "sha256": digest(capture_path),
            "tcpdump_summary": summary, "stop_method": method, "exit_code": rc}


def wait_log(record: Dict[str, Any], marker: str, timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if record["proc"].poll() is not None:
            raise RuntimeError("{} exited before readiness: {}".format(record["label"],
                                                                         read_log(record["log"])[-2000:]))
        if marker in read_log(record["log"]):
            return
        time.sleep(0.2)
    raise RuntimeError("{} readiness marker timeout {}".format(record["label"], marker))


def repo_database_state(repo_dir: Path) -> Dict[str, Any]:
    database_paths = sorted(
        path for path in repo_dir.rglob("*")
        if path.is_file() and path.suffix in {".sqlite", ".sqlite3"}
    )
    result: Dict[str, Any] = {
        "databases": {}, "payload_files": [],
        "repo_manifests": {"rows": 0, "rowsSha256": hashlib.sha256().hexdigest(),
                           "entries": []},
    }
    database_and_sidecar_paths = set()
    for path in database_paths:
        uri = "file:{}?mode=ro".format(path.resolve().as_posix())
        connection = sqlite3.connect(uri, uri=True, timeout=10)
        try:
            connection.execute("PRAGMA query_only=ON")
            tables = [row[0] for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
            table_state = {}
            for table in tables:
                escaped = table.replace('"', '""')
                columns = [row[1] for row in connection.execute(
                    'PRAGMA table_info("{}")'.format(escaped))]
                rows = connection.execute(
                    'SELECT * FROM "{}"'.format(escaped)).fetchall()
                encoded_rows = []
                for row in rows:
                    values = []
                    for value in row:
                        if isinstance(value, (bytes, bytearray, memoryview)):
                            raw = bytes(value)
                            values.append({"blobSha256": hashlib.sha256(raw).hexdigest(),
                                           "size": len(raw)})
                        else:
                            values.append(value)
                    encoded_rows.append(json.dumps(
                        values, sort_keys=True, separators=(",", ":"),
                        ensure_ascii=False))
                encoded_rows.sort()
                row_digest = hashlib.sha256()
                for encoded in encoded_rows:
                    row_digest.update(encoded.encode("utf-8"))
                    row_digest.update(b"\n")
                table_state[table] = {
                    "columns": columns,
                    "rows": len(rows),
                    "rowsSha256": row_digest.hexdigest(),
                }
            database_state = {"sha256": digest(path), "tables": table_state}
            database_and_sidecar_paths.add(path)
            for suffix in ("-wal", "-shm"):
                sidecar = Path(str(path) + suffix)
                if sidecar.is_file():
                    database_and_sidecar_paths.add(sidecar)
                    database_state[suffix[1:]] = {
                        "size": sidecar.stat().st_size,
                        "sha256": digest(sidecar),
                    }
            result["databases"][str(path.relative_to(repo_dir))] = database_state
        finally:
            connection.close()
    for path in sorted(repo_dir.rglob("*")):
        if not path.is_file() or path in database_and_sidecar_paths:
            continue
        result["payload_files"].append({
            "path": str(path.relative_to(repo_dir)),
            "size": path.stat().st_size,
            "sha256": digest(path),
        })
    manifest_entries = []
    for path in sorted(path for path in repo_dir.rglob("*.json")
                       if path.is_file() and path.parent.name == "manifests"):
        try:
            manifest = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise RuntimeError("invalid durable Repo manifest {}: {}".format(
                path.relative_to(repo_dir), error)) from error
        if not isinstance(manifest, dict):
            raise RuntimeError("durable Repo manifest is not an object: {}".format(
                path.relative_to(repo_dir)))
        manifest_entries.append({
            "path": str(path.relative_to(repo_dir)),
            "objectName": manifest.get("objectName"),
            "objectType": manifest.get("objectType"),
            "sha256": manifest.get("sha256"),
            "size": manifest.get("size"),
            "generation": manifest.get("generation"),
            "manifestFileSha256": digest(path),
        })
    manifest_rows = sorted(json.dumps(
        entry, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
        for entry in manifest_entries)
    manifest_digest = hashlib.sha256()
    for row in manifest_rows:
        manifest_digest.update(row.encode("utf-8"))
        manifest_digest.update(b"\n")
    result["repo_manifests"] = {
        "rows": len(manifest_entries),
        "rowsSha256": manifest_digest.hexdigest(),
        "entries": manifest_entries,
    }
    return result


def ensure_two_repo_commits(receipt_path: Path) -> None:
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    commits = receipt.get("replicaCommits") or receipt.get("replica_commits") or []
    states = {item.get("state") for item in commits}
    repos = {item.get("repoNode", item.get("repo_node")) for item in commits}
    if len(commits) != 2 or states != {"COMMITTED"} or repos != {REPO_A_ID, REPO_B_ID}:
        raise RuntimeError("receipt does not independently record both selected C++ Repos")


def assert_no_remote_artifacts(state: Dict[str, Any],
                               allow_missing_tables: bool = False) -> None:
    manifest_rows = state.get("repo_manifests", {}).get("rows", 0)
    if manifest_rows != 0:
        raise RuntimeError("pre-archive Repo manifest inventory is non-empty")
    checked = False
    for database in state["databases"].values():
        table_map = database["tables"]
        for table in ("objects", "artifact_persistent_receipts", "artifact_locations",
                      "artifact_publish_operations", "artifact_publish_receipts"):
            if table in table_map:
                checked = True
                if table_map[table]["rows"] != 0:
                    raise RuntimeError("pre-archive Repo table {} is non-empty".format(table))
    if not checked and not allow_missing_tables:
        raise RuntimeError("pre-archive oracle did not find Repo object/receipt tables")


def assert_repo_durable_state(state: Dict[str, Any], expected_digest: str) -> None:
    table_maps = [database["tables"] for database in state["databases"].values()]
    object_rows = sum(table_map.get("objects", {}).get("rows", 0)
                      for table_map in table_maps)
    object_rows += state.get("repo_manifests", {}).get("rows", 0)
    receipt_rows = sum(table_map.get("artifact_persistent_receipts", {}).get("rows", 0)
                       for table_map in table_maps)
    location_rows = sum(table_map.get("artifact_locations", {}).get("rows", 0)
                        for table_map in table_maps)
    if object_rows == 0 or receipt_rows == 0 or location_rows == 0:
        raise RuntimeError("Repo DB oracle missed committed object/receipt/location rows")
    artifact_manifests = [entry for entry in
                          state.get("repo_manifests", {}).get("entries", [])
                          if entry.get("objectType") == "artifact-v2-bytes" and
                          entry.get("sha256") == expected_digest]
    if not artifact_manifests:
        raise RuntimeError("Repo filesystem manifest does not bind the expected artifact digest")
    payloads = [item for item in state["payload_files"]
                if item["path"].endswith("/" + expected_digest)]
    if len(payloads) != 1 or payloads[0]["sha256"] != expected_digest:
        raise RuntimeError("Repo durable payload file does not match the artifact digest")


REPO_ARTIFACT_TABLES = {
    "objects",
    "artifact_persistent_receipts",
    "artifact_locations",
    "artifact_publish_operations",
    "artifact_publish_receipts",
}


def repo_artifact_inventory(state: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "databases": {
            path: {
                table: values
                for table, values in database["tables"].items()
                if table in REPO_ARTIFACT_TABLES
            }
            for path, database in sorted(state["databases"].items())
        },
        "repo_manifests": state.get("repo_manifests", {}),
        "payload_files": state["payload_files"],
    }


def set_default_identity(node, identity: str, certificate_name: str) -> None:
    checked_node_cmd(node, "ndnsec set-default -n {}".format(
        perf.shell_quote(identity)))
    checked_node_cmd(node, "ndnsec set-default -c -n {}".format(
        perf.shell_quote(certificate_name)))


def json_event(path: Union[Path, str], marker: str) -> Dict[str, Any]:
    path = Path(path)
    for line in read_log(path).splitlines():
        if marker in line:
            return json.loads(line.split(marker, 1)[1].strip())
    raise RuntimeError("{} missing from {}".format(marker, path))


def start_di_provider(ndn, output: Path, processes: List[Dict[str, Any]],
                      environment: Dict[str, str], runtime_config: Path,
                      trust_schema: Path, identity_certificates: Dict[str, str],
                      provider_id: str,
                      provider_identity: str, repo_identity: str, label: str,
                      cache_dir: Optional[Path] = None):
    # APPProvider and its native RepoClient own separate Faces. Keep the
    # authorized Repo read principal distinct from the service Provider
    # identity so the two Faces do not contend for one NFD identity prefix.
    certificate_name = identity_certificates.get(provider_identity)
    if certificate_name is None:
        raise RuntimeError("unknown Spec192 DI provider identity " + provider_identity)
    set_default_identity(ndn.net[APP_HOST], provider_identity, certificate_name)
    peer = str(REPO / "tests/minindn/spec192_di_peer.py")
    argv = [
        sys.executable, peer, "--mode", "provider",
        "--config", str(runtime_config),
        "--generated-policy-dir", str(output / "generated-policy"),
        "--group", GROUP, "--controller", CONTROLLER,
        "--trust-schema", str(trust_schema),
        "--repo-service", SERVICE_PREFIX,
        "--provider-id", provider_id,
        "--repo-identity", repo_identity,
        "--temp-dir", str(output / "di-provider-state" / provider_id),
    ]
    provider_environment = dict(environment)
    if cache_dir is not None:
        provider_environment["NDNSF_ARTIFACT_CACHE_DIR"] = str(cache_dir)
    for name in ("NDN_LOG", SELECTED_REQUEST_TRACE_ENV,
                 SELECTED_REQUEST_TRACE_CHILD_ENV, NDN_LOG_TRACE_CHILD_ENV,
                 NDN_LOG_TRACE_PROVIDER_ENV):
        provider_environment.pop(name, None)
    if label in traced_provider_labels():
        provider_environment["NDN_LOG"] = (
            "ndn_service_framework.ServiceProvider=TRACE:ndn_svs.SVSPubSub=TRACE"
            if label == "spec192-di-provider-authorized" else "*=TRACE")
    record = spawn(ndn, output, processes, APP_HOST, label, argv,
                   provider_environment)
    wait_log(record, "SPEC192_DI_PROVIDER_SERVING", 45)
    time.sleep(3)
    if record["proc"].poll() is not None:
        raise RuntimeError("DI Provider exited after startup: " + read_log(record["log"])[-2000:])
    ready = json_event(record["log"], "SPEC192_DI_PROVIDER_SERVING")
    return record, ready


def start_repo_server(ndn, output: Path, processes: List[Dict[str, Any]],
                      environments: Dict[str, Dict[str, str]], repo_server: Path,
                      trust_schema: Path, identity_certificates: Dict[str, str],
                      host_name: str, repo_identity: str, repo_node: str,
                      label: str, directory_name: str):
    certificate_name = identity_certificates.get(repo_identity)
    if certificate_name is None:
        raise RuntimeError("unknown Spec192 Repo identity " + repo_identity)
    host = ndn.net[host_name]
    set_default_identity(host, repo_identity, certificate_name)
    repo_dir = Path(host.params["params"]["homeDir"]) / directory_name
    shutil.rmtree(repo_dir, ignore_errors=True)
    database = repo_dir / "repo.sqlite"
    argv = [
        str(repo_server),
        "--repo-node", repo_node,
        "--database", str(database),
        "--group-prefix", GROUP,
        "--provider-identity", repo_identity,
        "--authority-identity", CONTROLLER,
        "--trust-schema", str(trust_schema),
        "--service-prefix", SERVICE_PREFIX,
        "--failure-domain", host_name,
        "--ack-collection-ms", str(REPO_SERVER_ACK_COLLECTION_MS),
        "--request-timeout-ms", str(REPO_SERVER_REQUEST_TIMEOUT_MS),
    ]
    environment = node_environment(host, REPO, no_python=True)
    record = spawn(ndn, output, processes, host_name, label, argv,
                   repo_server_environment(environment, label))
    record["repo_dir"] = repo_dir
    record["database"] = database
    record["repo_node"] = repo_node
    record["provider_identity"] = repo_identity
    wait_log(record, "Persistent Repo serving", 35)
    assert_no_python_mapped(record)
    return record


def restart_repo_servers(ndn, output: Path, processes: List[Dict[str, Any]],
                         environments: Dict[str, Dict[str, str]],
                         servers: List[Dict[str, Any]], label_suffix: str,
                         ready_text: str = "Persistent Repo serving"
                         ) -> Dict[str, Any]:
    stopped = []
    for server in servers:
        if server["proc"].poll() is None:
            stopped.append(stop_repo_server(server))
    restarted = []
    for old in servers:
        replacement_label = old["label"] + "-" + label_suffix
        replacement = spawn(ndn, output, processes, old["host"],
                            replacement_label, old["argv"],
                            repo_server_environment(node_environment(
                                ndn.net[old["host"]], REPO, no_python=True),
                                replacement_label))
        replacement["repo_dir"] = old["repo_dir"]
        replacement["database"] = old["database"]
        replacement["repo_node"] = old["repo_node"]
        replacement["provider_identity"] = old["provider_identity"]
        wait_log(replacement, ready_text, 35)
        assert_no_python_mapped(replacement)
        restarted.append(replacement)
    servers[:] = restarted
    return {
        "stopped": stopped,
        "restarted_pids": [item["pid"] for item in restarted],
    }


def wait_for_json_file(path: Path, process: Dict[str, Any], timeout_s: float,
                       label: str) -> Dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if path.is_file():
            try:
                value = json.loads(path.read_text(encoding="utf-8"))
                if isinstance(value, dict):
                    return value
            except (OSError, json.JSONDecodeError):
                pass
        if process["proc"].poll() is not None:
            raise RuntimeError("{} exited before publishing its handle: {}".format(
                label, read_log(process["log"])[-2000:]))
        time.sleep(0.02)
    raise RuntimeError("{} did not publish its operation handle".format(label))


def finish_spawned_child(record: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    proc = record["proc"]
    try:
        rc = proc.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        record["stream"].close()
        raise RuntimeError("child timed out: {} log={}".format(record["label"], record["log"]))
    record["stream"].close()
    result = {
        "host": record["host"],
        "label": record["label"],
        "pid": record["pid"],
        "argv": record["argv"],
        "exit_code": rc,
        "log": str(record["log"]),
        "python_mapped": record["python_mapped"],
        "python_maps_observed": record["python_maps_observed"],
        "path_hides_python": record["path_hides_python"],
    }
    if rc != 0:
        raise RuntimeError("{} exited {}; log tail:\n{}".format(
            record["label"], rc, read_log(record["log"])[-3000:]))
    return result


def assert_digest_absent(state: Dict[str, Any], expected_digest: str) -> None:
    if any(item["sha256"] == expected_digest for item in state["payload_files"]):
        raise RuntimeError("unexpected durable Repo payload for digest " + expected_digest)


def wait_for_repo_payload(repo_dir: Path, expected_digest: str, expected_size: int,
                          process: Dict[str, Any], timeout_s: float) -> Path:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for path in repo_dir.rglob(expected_digest):
            if path.is_file() and path.stat().st_size == expected_size:
                if digest(path) != expected_digest:
                    raise RuntimeError("seed Repo published bytes with the wrong digest")
                return path
        if process["proc"].poll() is not None:
            raise RuntimeError("VM-20 publisher exited before seed commit: exit_code={} log_tail={}".format(
                process["proc"].returncode, read_log(process["log"])[-2000:]))
        time.sleep(0.005)
    raise RuntimeError("VM-20 selected seed did not durably publish its payload in time")


def cpp_json_marker(log_path: Path, marker: str) -> Dict[str, Any]:
    for line in read_log(log_path).splitlines():
        if line.startswith(marker + " "):
            value = json.loads(line[len(marker):].strip())
            if not isinstance(value, dict):
                break
            return value
    raise RuntimeError("C++ client log omitted JSON marker " + marker)


def assert_three_repo_commits(receipt_path: Path) -> None:
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    commits = receipt.get("replicaCommits") or receipt.get("replica_commits") or []
    states = {item.get("state") for item in commits}
    repos = {item.get("repoNode", item.get("repo_node")) for item in commits}
    if len(commits) != 3 or states != {"COMMITTED"} or \
            repos != {REPO_A_ID, REPO_B_ID, REPO_C_ID}:
        raise RuntimeError("receipt does not independently record all three selected C++ Repos")


def repo_table_row_counts(state: Dict[str, Any]) -> Dict[str, int]:
    table_names = ("objects", "artifact_publish_operations", "artifact_publish_receipts",
                   "artifact_persistent_receipts", "artifact_locations")
    return {
        name: sum(database["tables"].get(name, {}).get("rows", 0)
                  for database in state["databases"].values())
             + (state.get("repo_manifests", {}).get("rows", 0)
                if name == "objects" else 0)
        for name in table_names
    }


def build_services(ndn, output: Path, runtime_config: Path,
                   trust_schema: Path, repo_server: Path,
                   processes: List[Dict[str, Any]], repo_root: Path,
                   server_ready_text: str = "Persistent Repo serving",
                   durable_repo_root: Optional[Path] = None,
                   durable_mode: Optional[str] = None):
    environments = {name: node_environment(ndn.net[name], repo_root)
                    for name in HOST_ROLES}
    environments[CONTROLLER_HOST]["NDNSF_CONTROLLER_GENERATION_STATE"] = str(
        output / "controller-generation.state")
    controller_argv = [
        sys.executable,
        str(repo_root / "examples/python/NDNSF-DistributedRepo/generic_object_store/controller.py"),
        "--config", str(runtime_config),
        "--generated-policy-dir", str(output / "generated-policy"),
    ]
    controller = spawn(ndn, output, processes, CONTROLLER_HOST,
                       "controller", controller_argv, environments[CONTROLLER_HOST])
    deadline = time.monotonic() + 45
    while not trust_schema.exists() and time.monotonic() < deadline:
        if controller["proc"].poll() is not None:
            raise RuntimeError("Controller exited before policy generation")
        time.sleep(0.2)
    if not trust_schema.exists():
        raise RuntimeError("Controller did not generate trust schema")
    time.sleep(5)

    servers = []
    for host_name, repo_node, provider in [
        (REPO_A_HOST, REPO_A_ID, REPO_A_PROVIDER),
        (REPO_B_HOST, REPO_B_ID, REPO_B_PROVIDER),
    ]:
        repo_dir = ((durable_repo_root / host_name) if durable_repo_root else
                    Path(ndn.net[host_name].params["params"]["homeDir"]) / "spec192-repo")
        if durable_mode == "seed":
            if repo_dir.exists():
                raise RuntimeError("durable Repo seed directory already exists: " + str(repo_dir))
            repo_dir.mkdir(parents=True)
        elif durable_mode == "read":
            if not repo_dir.is_dir():
                raise RuntimeError("durable Repo read directory is missing: " + str(repo_dir))
        else:
            shutil.rmtree(repo_dir, ignore_errors=True)
        database = repo_dir / "repo.sqlite"
        argv = [
            str(repo_server),
            "--repo-node", repo_node,
            "--database", str(database),
            "--group-prefix", GROUP,
            "--provider-identity", provider,
            "--authority-identity", CONTROLLER,
            "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--failure-domain", host_name,
            "--ack-collection-ms", str(REPO_SERVER_ACK_COLLECTION_MS),
            "--request-timeout-ms", str(REPO_SERVER_REQUEST_TIMEOUT_MS),
        ]
        repo_label = "cpp-repo-" + host_name
        record = spawn(ndn, output, processes, host_name,
                       repo_label, argv,
                       repo_server_environment(node_environment(
                           ndn.net[host_name], repo_root, no_python=True),
                           repo_label))
        record["repo_dir"] = repo_dir
        record["database"] = database
        record["repo_node"] = repo_node
        record["provider_identity"] = provider
        wait_log(record, server_ready_text, 35)
        assert_no_python_mapped(record)
        servers.append(record)
    return controller, servers, environments


def run_scenario(ndn, output: Path, processes: List[Dict[str, Any]],
                 environments: Dict[str, Dict[str, str]], repo_client: Path,
                 repo_server: Path, servers: List[Dict[str, Any]],
                 trust_schema: Path, runtime_config: Path,
                 identity_certificates: Dict[str, str]) -> Dict[str, Any]:
    trace_children = {
        child.strip()
        for child in os.environ.get(SELECTED_REQUEST_TRACE_CHILD_ENV, "").split(",")
        if child.strip()
    }
    unsupported_trace_children = trace_children - SELECTED_REQUEST_TRACE_CHILDREN
    if unsupported_trace_children:
        raise RuntimeError(
            "unsupported selected-request trace children: " +
            ",".join(sorted(unsupported_trace_children)))
    log_children = {
        child.strip()
        for child in os.environ.get(NDN_LOG_TRACE_CHILD_ENV, "").split(",")
        if child.strip()
    }
    unsupported_log_children = log_children - NDN_LOG_TRACE_CHILDREN
    if unsupported_log_children:
        raise RuntimeError(
            "unsupported NDN logger trace children: " +
            ",".join(sorted(unsupported_log_children)))
    unsupported_log_providers = traced_provider_labels() - NDN_LOG_TRACE_PROVIDER_LABELS
    if unsupported_log_providers:
        raise RuntimeError("unsupported NDN logger trace providers: " +
                           ",".join(sorted(unsupported_log_providers)))
    result: Dict[str, Any] = {
        "scenario": "VM-11/12/14/15/20/21/22/23",
        "children": [],
    }
    app_home = Path(ndn.net[APP_HOST].params["params"]["homeDir"])
    consumer_home = Path(ndn.net[CONSUMER_HOST].params["params"]["homeDir"])
    repo_a_dir = Path(ndn.net[REPO_A_HOST].params["params"]["homeDir"]) / "spec192-repo"
    repo_b_dir = Path(ndn.net[REPO_B_HOST].params["params"]["homeDir"]) / "spec192-repo"
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "cpp-inapp-payload.bin"
    payload = bytes((index * 31 + 7) % 256 for index in range(9001))
    source.write_bytes(payload)
    local_root = app_home / "spec192-inapp-root"
    local_handle = output / "artifacts/inapp-handle.json"
    local_receipt = output / "artifacts/inapp-receipt.json"
    reopened = output / "artifacts/inapp-reopened.bin"
    repo_pair = {REPO_A_ID, REPO_B_ID}

    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--user", USER,
            "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    cpp_env = node_environment(ndn.net[APP_HOST], REPO, no_python=True)
    store_trace_before_inapp = repo_store_events(processes, repo_pair)
    if store_trace_before_inapp:
        raise RuntimeError("C++ Repo received a write request before the In-App local publish")
    before_a = repo_database_state(repo_a_dir)
    before_b = repo_database_state(repo_b_dir)
    assert_no_remote_artifacts(before_a)
    assert_no_remote_artifacts(before_b)
    child = run_child(
        ndn, output, processes, APP_HOST, "inapp-publish-process",
        [str(repo_client), "--mode", "inapp-publish", "--source", str(source),
         "--store-dir", str(local_root), "--handle-out", str(local_handle),
         "--receipt-out", str(local_receipt), "--user", USER],
        cpp_env, 20, require_no_python=True)
    result["children"].append(child)
    child = run_child(
        ndn, output, processes, APP_HOST, "inapp-reopen-new-process",
        [str(repo_client), "--mode", "inapp-reopen", "--store-dir", str(local_root),
         "--handle", str(local_handle), "--destination", str(reopened)],
        cpp_env, 20, require_no_python=True)
    result["children"].append(child)
    if digest(reopened) != digest(source) or before_a != repo_database_state(repo_a_dir) or before_b != repo_database_state(repo_b_dir):
        raise RuntimeError("In-App restart changed remote Repo state before explicit archive")
    store_trace_after_inapp = repo_store_events(processes, repo_pair)
    if store_trace_after_inapp != store_trace_before_inapp:
        raise RuntimeError("In-App publish/reopen reached a Persistent Repo write handler")
    result["pre_archive"] = {"repoA": before_a, "repoB": before_b,
                              "unchanged_after_inapp_reopen": True,
                              "source_sha256": digest(source),
                              "authenticated_store_trace": store_trace_after_inapp}

    archived_handle = output / "artifacts/cpp-archived-handle.json"
    archived_receipt = output / "artifacts/cpp-archived-receipt.json"
    child = run_child(
        ndn, output, processes, APP_HOST, "explicit-archive-process",
        [str(repo_client), "--mode", "archive", "--store-dir", str(local_root),
         "--handle", str(local_handle), "--handle-out", str(archived_handle),
         "--receipt-out", str(archived_receipt), "--group", GROUP,
         "--controller", CONTROLLER, "--authority", CONTROLLER, "--user", USER,
         "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        cpp_env, 120, require_no_python=True)
    result["children"].append(child)
    ensure_two_repo_commits(archived_receipt)
    archive_store_trace = repo_store_events(processes, repo_pair)
    store_by_repo = {
        repo_node: [event for event in archive_store_trace
                    if event["repo_node"] == repo_node]
        for repo_node in repo_pair
    }
    seed_events = store_by_repo[REPO_A_ID]
    replication_events = store_by_repo[REPO_B_ID]
    if (any(event["requester"] != USER for event in seed_events) or
            not any(event["operation"] == "STORE" for event in seed_events) or
            not any(event["operation"] == "STORE_RANGE" for event in seed_events) or
            sum(event["operation"] == "COMMIT_RANGES" for event in seed_events) != 1 or
            any(event["requester"] != REPO_A_PROVIDER for event in replication_events) or
            not any(event["operation"] == "STORE_RANGE" for event in replication_events) or
            sum(event["operation"] == "COMMIT_RANGES" for event in replication_events) != 1):
        raise RuntimeError(
            "explicit archive STORE/STORE_RANGE/COMMIT_RANGES trace did not bind "
            "the user seed and selected Repo A replication")
    post_archive_a = repo_database_state(repo_a_dir)
    post_archive_b = repo_database_state(repo_b_dir)
    artifact_digest = digest(source)
    assert_repo_durable_state(post_archive_a, artifact_digest)
    assert_repo_durable_state(post_archive_b, artifact_digest)
    result["post_archive"] = {"repoA": post_archive_a, "repoB": post_archive_b,
                              "receipt_sha256": digest(archived_receipt),
                              "artifact_sha256": artifact_digest,
                              "authenticated_store_trace": archive_store_trace}

    binding = str(REPO / "tests/minindn/spec192_binding_client.py")
    python_args = [
        sys.executable, binding, "--mode", "fetch",
        "--group", GROUP, "--controller", CONTROLLER, "--user", CONSUMER,
        "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
        "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
        "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER,
        "--handle", str(archived_handle), "--destination",
        str(output / "artifacts/python-fetched-cpp.bin"),
    ]
    child = run_child(ndn, output, processes, CONSUMER_HOST,
                      "python-binding-fetch-cpp-artifact", python_args,
                      environments[CONSUMER_HOST], 360)
    result["children"].append(child)
    result["cpp_receipt_json"] = json.loads(archived_receipt.read_text(encoding="utf-8"))
    result["cpp_fetch_identity"] = CONSUMER

    receipt_args = [
        sys.executable, binding, "--mode", "receipt",
        "--group", GROUP, "--controller", CONTROLLER, "--user", USER,
        "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
        "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
        "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER,
        "--handle", str(archived_handle), "--receipt", str(archived_receipt),
    ]
    child = run_child(ndn, output, processes, APP_HOST,
                      "python-binding-query-cpp-native-receipt", receipt_args,
                      environments[APP_HOST], 360)
    result["children"].append(child)
    result["cpp_receipt_query_identity"] = USER

    python_source = output / "artifacts/python-published-payload.bin"
    python_source.write_bytes(bytes(reversed(payload)) + b"python-binding-origin")
    python_handle = output / "artifacts/python-published-handle.json"
    python_receipt = output / "artifacts/python-published-receipt.json"
    python_args = [
        sys.executable, binding, "--mode", "publish",
        "--group", GROUP, "--controller", CONTROLLER, "--user", CONSUMER,
        "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
        "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
        "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER,
        "--source", str(python_source), "--handle-out", str(python_handle),
        "--receipt-out", str(python_receipt), "--destination",
        str(output / "artifacts/python-publish-roundtrip.bin"),
    ]
    child = run_child(ndn, output, processes, CONSUMER_HOST,
                      "python-binding-publish-status-fetch", python_args,
                      environments[CONSUMER_HOST], 360)
    result["children"].append(child)
    child = run_child(
        ndn, output, processes, CONSUMER_HOST, "cpp-client-fetch-python-receipt-no-python",
        [str(repo_client), "--mode", "fetch", "--handle", str(python_handle),
         "--receipt", str(python_receipt), "--destination",
         str(output / "artifacts/cpp-fetched-python.bin"),
         "--group", GROUP, "--controller", CONTROLLER, "--authority", CONTROLLER,
         "--user", CONSUMER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True), 180,
        require_no_python=True)
    result["children"].append(child)
    if digest(output / "artifacts/cpp-fetched-python.bin") != digest(python_source):
        raise RuntimeError("independent C++ client could not consume Python binding publication")

    di_work = output / "artifacts" / "di"
    handle_dir = di_work / "handles"
    handle_dir.mkdir(parents=True, exist_ok=True)
    di_payloads = {
        "model": b"Spec192 immutable DI model reference\x00v1",
        "input": b"Spec192 explicitly retained DI input dataset\x00v1",
        "output": b"Spec192 explicitly retained DI output artifact\x00v1",
    }
    di_receipts = {}
    for slot, payload_bytes in di_payloads.items():
        source_path = di_work / (slot + ".bin")
        source_path.write_bytes(payload_bytes)
        handle_path = handle_dir / (slot + "-handle.json")
        receipt_path = handle_dir / (slot + "-receipt.json")
        child = run_child(
            ndn, output, processes, APP_HOST, "cpp-publish-di-" + slot,
            [str(repo_client), "--mode", "publish", "--source", str(source_path),
             "--handle-out", str(handle_path), "--receipt-out", str(receipt_path),
             "--group", GROUP, "--controller", CONTROLLER, "--authority", CONTROLLER,
             "--user", USER, "--trust-schema", str(trust_schema),
             "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
             "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
            environments[APP_HOST], DI_PUBLISH_CHILD_TIMEOUT_S)
        result["children"].append(child)
        ensure_two_repo_commits(receipt_path)
        di_receipts[slot] = json.loads(receipt_path.read_text(encoding="utf-8"))

    artifact_references = di_work / "artifact-references.json"
    binding_script = str(REPO / "tests/minindn/spec192_di_peer.py")
    child = run_child(
        ndn, output, processes, CONSUMER_HOST, "python-build-di-references",
        [sys.executable, binding_script, "--mode", "bindings",
         "--handle-dir", str(handle_dir), "--output", str(artifact_references)],
        environments[CONSUMER_HOST], 30)
    result["children"].append(child)

    di_before = {
        "repoA": repo_database_state(repo_a_dir),
        "repoB": repo_database_state(repo_b_dir),
    }
    for source_path in (di_work / "model.bin", di_work / "input.bin", di_work / "output.bin"):
        expected_digest = digest(source_path)
        assert_repo_durable_state(di_before["repoA"], expected_digest)
        assert_repo_durable_state(di_before["repoB"], expected_digest)
    di_inventory_before = {
        node: repo_artifact_inventory(state) for node, state in di_before.items()
    }

    di_request_args = [
        sys.executable, binding_script, "--mode", "request",
        "--config", str(runtime_config),
        "--generated-policy-dir", str(output / "generated-policy"),
        "--group", GROUP,
        "--artifact-references", str(artifact_references),
    ]
    authorized_provider, authorized_epoch = start_di_provider(
        ndn, output, processes, environments[APP_HOST], runtime_config,
        trust_schema, identity_certificates, DI_PROVIDER_ID, DI_PROVIDER,
        DI_REPO_CLIENT_IDENTITY,
        "spec192-di-provider-authorized")
    child = run_child(ndn, output, processes, CONSUMER_HOST,
                      "spec192-di-request-authorized", di_request_args,
                      environments[CONSUMER_HOST], 100)
    result["children"].append(child)
    authorized_result = json_event(
        child["log"], "SPEC192_DI_REQUEST_PASS")
    provider_materialization = json_event(
        authorized_provider["log"], "SPEC192_DI_PROVIDER_ARTIFACTS")
    if authorized_result.get("provider") != DI_PROVIDER:
        raise RuntimeError("DI request was not served by the authorized fresh Provider")
    if provider_materialization.get("provider") != DI_PROVIDER:
        raise RuntimeError("DI Provider artifact record has the wrong identity")
    authorized_stop = stop_daemon(authorized_provider)

    after_authorized = {
        "repoA": repo_database_state(repo_a_dir),
        "repoB": repo_database_state(repo_b_dir),
    }
    if any(repo_artifact_inventory(after_authorized[node]) != di_inventory_before[node]
           for node in ("repoA", "repoB")):
        raise RuntimeError("authorized DI request published transient state into Repo")

    unauthorized_provider, unauthorized_epoch = start_di_provider(
        ndn, output, processes, environments[APP_HOST], runtime_config,
        trust_schema, identity_certificates, DI_UNAUTHORIZED_ID,
        DI_UNAUTHORIZED_PROVIDER, DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY,
        "spec192-di-provider-unauthorized")
    child = run_child(ndn, output, processes, CONSUMER_HOST,
                      "spec192-di-request-unauthorized", di_request_args + ["--expect-denied"],
                      environments[CONSUMER_HOST], 100)
    result["children"].append(child)
    denied_request = json_event(child["log"], "SPEC192_DI_REQUEST_FAILED")
    if denied_request.get("status") is not False:
        raise RuntimeError("unauthorized Repo read unexpectedly returned an inference result")
    repo_denial = json_event(
        unauthorized_provider["log"], "SPEC192_DI_REPO_FETCH_REJECTED")
    if repo_denial.get("identity") != DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY:
        raise RuntimeError("DI Repo fetch used an unexpected authorization identity")
    denial_error = str(repo_denial.get("error", ""))
    if "repo-request-start-failed" not in denial_error or \
            "Core rejected Provider request" not in denial_error:
        raise RuntimeError("unauthorized DI Repo read did not fail at Repo request admission")
    if "SPEC192_DI_PROVIDER_ARTIFACTS" in read_log(unauthorized_provider["log"]):
        raise RuntimeError("unauthorized DI Provider reached the inference handler")
    unauthorized_stop = stop_daemon(unauthorized_provider)

    after_denied = {
        "repoA": repo_database_state(repo_a_dir),
        "repoB": repo_database_state(repo_b_dir),
    }
    if any(repo_artifact_inventory(after_denied[node]) != di_inventory_before[node]
           for node in ("repoA", "repoB")):
        raise RuntimeError("denied DI request changed Repo artifact inventory")
    result["di_vm14_vm15"] = {
        "service": DI_SERVICE,
        "role": DI_ROLE,
        "artifact_references_sha256": digest(artifact_references),
        "explicit_artifacts": {
            slot: {
                "sha256": digest(di_work / (slot + ".bin")),
                "size": (di_work / (slot + ".bin")).stat().st_size,
                "receipt": di_receipts[slot],
            }
            for slot in di_payloads
        },
        "authorized_provider_boot_epoch": authorized_epoch["boot_epoch"],
        "authorized_repo_client_identity": DI_REPO_CLIENT_IDENTITY,
        "authorized_provider_response": authorized_result,
        "authorized_provider_materialization": provider_materialization,
        "authorized_provider_stop": authorized_stop,
        "unauthorized_provider_boot_epoch": unauthorized_epoch["boot_epoch"],
        "unauthorized_repo_client_identity": DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY,
        "unauthorized_client_result": denied_request,
        "unauthorized_repo_denial": repo_denial,
        "unauthorized_provider_stop": unauthorized_stop,
        "repo_artifact_inventory_before": di_inventory_before,
        "repo_artifact_inventory_after_authorized": {
            node: repo_artifact_inventory(state)
            for node, state in after_authorized.items()
        },
        "repo_artifact_inventory_after_denied": {
            node: repo_artifact_inventory(state)
            for node, state in after_denied.items()
        },
        "no_transient_repo_publication": True,
    }

    # VM-20 stops the operation owner after the seed receipt is durable while
    # the second selected Repo is offline.  The restarted owner must reconcile
    # the same operation, while replay and key-conflict requests remain bound.
    vm20_source = work / "vm20-multichunk-payload.bin"
    # The 4,400-byte wire-safe range size still exercises multiple chunks.
    # Keep this recovery fixture bounded under a five-host network campaign.
    vm20_source.write_bytes(bytes((index * 17 + 23) % 256
                                  for index in range(64 * 1024 + 17)))
    vm20_digest = digest(vm20_source)
    vm20_handle = work / "vm20-operation-handle.json"
    vm20_status_path = work / "vm20-operation-status.json"
    vm20_key = "spec192-t010-vm20-owner-restart"
    vm20_label = "vm20-operation-begin-before-owner-restart"
    vm20_environment = node_environment(ndn.net[APP_HOST], REPO, no_python=True)
    trace_children = {
        child.strip()
        for child in os.environ.get(SELECTED_REQUEST_TRACE_CHILD_ENV, "").split(",")
        if child.strip()
    }
    log_children = {
        child.strip()
        for child in os.environ.get(NDN_LOG_TRACE_CHILD_ENV, "").split(",")
        if child.strip()
    }
    for name in ("NDN_LOG", SELECTED_REQUEST_TRACE_ENV,
                 SELECTED_REQUEST_TRACE_CHILD_ENV, NDN_LOG_TRACE_CHILD_ENV,
                 NDN_LOG_TRACE_PROVIDER_ENV):
        vm20_environment.pop(name, None)
    if vm20_label in trace_children:
        vm20_environment[SELECTED_REQUEST_TRACE_ENV] = "1"
    if vm20_label in log_children:
        vm20_environment["NDN_LOG"] = (
            "ndn_service_framework.ServiceUser=TRACE:ndn_svs.SVSPubSub=TRACE")
    begin_record = spawn(
        ndn, output, processes, APP_HOST, vm20_label,
        [str(repo_client), "--mode", "operation-begin", "--source", str(vm20_source),
         "--handle-out", str(vm20_handle), "--key", vm20_key] + base,
        vm20_environment)
    assert_no_python_mapped(begin_record)
    wait_for_repo_payload(servers[0]["repo_dir"], vm20_digest, vm20_source.stat().st_size,
                          begin_record, 390)
    seed_state = repo_database_state(servers[0]["repo_dir"])
    assert_repo_durable_state(seed_state, vm20_digest)
    stopped_b = stop_repo_server(servers[1])
    replica_state_before_restart = repo_database_state(servers[1]["repo_dir"])
    assert_digest_absent(replica_state_before_restart, vm20_digest)
    vm20_handle_value = wait_for_json_file(vm20_handle, begin_record, 30,
                                           "VM-20 operation acceptance")
    if vm20_handle_value.get("seed", {}).get("repoNode") != REPO_A_ID:
        raise RuntimeError("VM-20 operation did not accept through its explicitly selected seed")
    begin_result = finish_spawned_child(begin_record, 90)
    result["children"].append(begin_result)
    stopped_a = stop_repo_server(servers[0], timeout_s=25)
    vm20_restart = restart_repo_servers(
        ndn, output, processes, environments, servers, "vm20-owner-restart")
    time.sleep(3)

    status_args = [str(repo_client), "--mode", "operation-status",
                   "--handle", str(vm20_handle), "--status-out", str(vm20_status_path),
                   "--wait-ms", "120000"] + base
    child = run_child(ndn, output, processes, APP_HOST,
                      "vm20-operation-status-after-owner-restart", status_args,
                      node_environment(ndn.net[APP_HOST], REPO, no_python=True), 140,
                      require_no_python=True)
    result["children"].append(child)
    vm20_status = json.loads(vm20_status_path.read_text(encoding="utf-8"))
    if vm20_status.get("operationId") != vm20_handle_value.get("operationId") or \
            vm20_status.get("state") != "COMMITTED" or \
            len(vm20_status.get("targets", [])) != 2 or \
            any(target.get("state") != "COMMITTED" for target in vm20_status["targets"]) or \
            {target.get("target", {}).get("repoNode") for target in vm20_status["targets"]} != \
            {REPO_A_ID, REPO_B_ID}:
        raise RuntimeError("VM-20 resumed operation status does not bind both selected commits")
    assert_repo_durable_state(repo_database_state(servers[1]["repo_dir"]), vm20_digest)

    before_idempotent_replay = {
        REPO_A_ID: repo_table_row_counts(repo_database_state(servers[0]["repo_dir"])),
        REPO_B_ID: repo_table_row_counts(repo_database_state(servers[1]["repo_dir"])),
    }
    child = run_child(
        ndn, output, processes, APP_HOST, "vm20-idempotent-replay",
        [str(repo_client), "--mode", "operation-replay", "--source", str(vm20_source),
         "--handle", str(vm20_handle), "--key", vm20_key] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 45,
        require_no_python=True)
    result["children"].append(child)
    conflict_source = work / "vm20-conflicting-payload.bin"
    conflict_source.write_bytes(b"different bytes under the original idempotency key")
    child = run_child(
        ndn, output, processes, APP_HOST, "vm20-same-key-changed-request-rejected",
        [str(repo_client), "--mode", "operation-conflict", "--source", str(conflict_source),
         "--key", vm20_key] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 45,
        require_no_python=True)
    result["children"].append(child)
    after_idempotent_replay = {
        REPO_A_ID: repo_table_row_counts(repo_database_state(servers[0]["repo_dir"])),
        REPO_B_ID: repo_table_row_counts(repo_database_state(servers[1]["repo_dir"])),
    }
    if after_idempotent_replay != before_idempotent_replay:
        raise RuntimeError("VM-20 replay or conflict created duplicate durable operation effects")

    old_receipt_digest = digest(archived_receipt)
    old_handle_value = json.loads(archived_handle.read_text(encoding="utf-8"))
    old_versions = {
        item["repoNode"]: int(item["locationVersion"])
        for item in old_handle_value["replicaCommits"]
    }
    repair_handle = work / "vm20-new-key-operation-handle.json"
    repair_status_path = work / "vm20-new-key-operation-status.json"
    child = run_child(
        ndn, output, processes, APP_HOST, "vm20-same-reference-new-key",
        [str(repo_client), "--mode", "operation-begin", "--source", str(source),
         "--handle-out", str(repair_handle), "--key", "spec192-t010-vm20-repair"] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 390,
        require_no_python=True)
    result["children"].append(child)
    child = run_child(
        ndn, output, processes, APP_HOST, "vm20-new-key-operation-status",
        [str(repo_client), "--mode", "operation-status", "--handle", str(repair_handle),
         "--status-out", str(repair_status_path), "--wait-ms", "120000"] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 140,
        require_no_python=True)
    result["children"].append(child)
    repair_status = json.loads(repair_status_path.read_text(encoding="utf-8"))
    if repair_status.get("state") != "COMMITTED" or \
            len(repair_status.get("targets", [])) != 2 or \
            any(target.get("state") != "COMMITTED" for target in repair_status["targets"]) or \
            {target.get("target", {}).get("repoNode") for target in repair_status["targets"]} != \
            {REPO_A_ID, REPO_B_ID}:
        raise RuntimeError("VM-20 new-key publication did not commit all selected targets")
    repair_receipt_versions = {
        item["target"]["repoNode"]: int(item["receipt"]["locationVersion"])
        for item in repair_status["targets"]
    }
    if any(repair_receipt_versions.get(repo_name, 0) <= old_version
           for repo_name, old_version in old_versions.items()):
        raise RuntimeError("VM-20 new-key receipts did not advance current LOCATION versions")
    fetched_old_handle = work / "vm20-fetch-old-handle-after-location-advance.bin"
    child = run_child(
        ndn, output, processes, APP_HOST, "vm20-fetch-old-handle-after-location-advance",
        [str(repo_client), "--mode", "fetch", "--handle", str(archived_handle),
         "--receipt", str(archived_receipt), "--destination", str(fetched_old_handle)] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 120,
        require_no_python=True)
    result["children"].append(child)
    if digest(fetched_old_handle) != digest(source) or digest(archived_receipt) != old_receipt_digest:
        raise RuntimeError("VM-20 changed the immutable old receipt or failed to fetch the old handle")
    child = run_child(
        ndn, output, processes, APP_HOST, "vm20-old-handle-current-location",
        [str(repo_client), "--mode", "availability", "--handle", str(archived_handle),
         "--expect", "healthy", "--target-repo", REPO_A_ID, "--timeout-ms", "10000"] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 120,
        require_no_python=True)
    result["children"].append(child)
    current_report = cpp_json_marker(Path(child["log"]), "AVAILABILITY_PASS")
    current_versions = {item["repoNode"]: int(item["locationVersion"])
                        for item in current_report["locations"]}
    if any(current_versions.get(repo_name, 0) <= old_version
           for repo_name, old_version in old_versions.items()):
        raise RuntimeError("VM-20 current LOCATION versions did not advance beyond the saved handle")
    if digest(archived_receipt) != old_receipt_digest:
        raise RuntimeError("VM-20 rewrote the old aggregate receipt")
    result["vm20_operation_recovery"] = {
        "seed_repo": REPO_A_ID,
        "operation_id": vm20_status["operationId"],
        "seed_commit_before_owner_restart": seed_state,
        "replica_absent_before_owner_restart": True,
        "replica_commit_after_owner_restart": repo_database_state(servers[1]["repo_dir"]),
        "repo_process_restart": vm20_restart,
        "pre_restart_stops": [stopped_b, stopped_a],
        "same_key_replay": "PASS",
        "same_key_changed_request": "REJECTED",
        "new_key_status": repair_status,
        "new_key_location_versions": repair_receipt_versions,
        "old_handle_fetch_sha256": digest(fetched_old_handle),
        "old_receipt_sha256_unchanged": old_receipt_digest,
        "old_location_versions": old_versions,
        "current_location_versions": current_versions,
    }

    # VM-22's restart/reuse proof uses only the standalone C++ client. Its
    # child PATH is empty; any live mapping snapshot is checked for libpython.
    restart_result = restart_repo_servers(
        ndn, output, processes, environments, servers, "vm22-restart")
    time.sleep(3)
    child = run_child(
        ndn, output, processes, APP_HOST, "cpp-client-fetch-after-service-restart-no-python",
        [str(repo_client), "--mode", "fetch", "--handle", str(archived_handle),
         "--receipt", str(archived_receipt), "--destination",
         str(output / "artifacts/cpp-fetched-after-restart.bin"),
         "--group", GROUP, "--controller", CONTROLLER, "--authority", CONTROLLER,
         "--user", USER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 120,
        require_no_python=True)
    result["children"].append(child)
    if digest(output / "artifacts/cpp-fetched-after-restart.bin") != digest(source):
        raise RuntimeError("standalone C++ client could not reuse handle after Repo restart")
    result["service_restart"] = {
        "stopped": restart_result["stopped"],
        "restarted_pids": restart_result["restarted_pids"],
        "client_fetch_sha256": digest(output / "artifacts/cpp-fetched-after-restart.bin"),
    }
    result["final_repo_state"] = {"repoA": repo_database_state(servers[0]["repo_dir"]),
                                 "repoB": repo_database_state(servers[1]["repo_dir"])}

    # VM-11/12 exercise three distinct authenticated Repo principals, the
    # availability state oracle, failover, and a global timeout across an
    # all-offline set.  Repo C uses the idle consumer host and its own PIB key.
    repo_c = start_repo_server(
        ndn, output, processes, environments, repo_server, trust_schema,
        identity_certificates, REPO_C_HOST, REPO_C_PROVIDER, REPO_C_ID,
        "cpp-repo-C", "spec192-repo-C")
    time.sleep(3)
    three_repo_servers = servers + [repo_c]
    three_repo_before = {
        server["repo_node"]: repo_database_state(server["repo_dir"])
        for server in three_repo_servers
    }
    three_repo_args = base + ["--repo-c", REPO_C_ID, "--provider-c", REPO_C_PROVIDER]
    three_source = work / "vm11-three-replica-payload.bin"
    three_source.write_bytes(b"Spec192 three-replica failover and availability probe payload\x00v1")
    three_handle = work / "vm11-three-replica-handle.json"
    three_receipt = work / "vm11-three-replica-receipt.json"
    child = run_child(
        ndn, output, processes, APP_HOST, "vm11-three-replica-publish",
        [str(repo_client), "--mode", "publish", "--source", str(three_source),
         "--handle-out", str(three_handle), "--receipt-out", str(three_receipt)] + three_repo_args,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 600,
        require_no_python=True)
    result["children"].append(child)
    assert_three_repo_commits(three_receipt)
    three_digest = digest(three_source)
    three_repo_after = {
        server["repo_node"]: repo_database_state(server["repo_dir"])
        for server in three_repo_servers
    }
    for repo_node in (REPO_A_ID, REPO_B_ID, REPO_C_ID):
        before_state = three_repo_before[repo_node]
        after_state = three_repo_after[repo_node]
        before_receipts = repo_table_row_counts(before_state)["artifact_persistent_receipts"]
        after_receipts = repo_table_row_counts(after_state)["artifact_persistent_receipts"]
        if after_receipts != before_receipts + 1:
            raise RuntimeError("three-Repo publish lacks one new durable receipt on " + repo_node)
        before_paths = {item["path"] for item in before_state["payload_files"]}
        fresh_payloads = [item for item in after_state["payload_files"]
                          if item["path"] not in before_paths and
                          item["sha256"] == three_digest]
        if len(fresh_payloads) != 1:
            raise RuntimeError("three-Repo publish lacks one new complete payload on " + repo_node)

    child = run_child(
        ndn, output, processes, APP_HOST, "vm12-duplicate-provider-alias-rejected",
        [str(repo_client), "--mode", "reject-provider-alias", "--handle", str(three_handle),
         "--destination", str(work / "vm12-alias-must-not-fetch.bin")] + three_repo_args,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 45,
        require_no_python=True)
    result["children"].append(child)

    availability_logs = {}

    def run_availability(label: str, expectation: str, target_repo: str) -> Dict[str, Any]:
        child_result = run_child(
            ndn, output, processes, APP_HOST, label,
            [str(repo_client), "--mode", "availability", "--handle", str(three_handle),
             "--expect", expectation, "--target-repo", target_repo, "--timeout-ms", "10000"] + three_repo_args,
            node_environment(ndn.net[APP_HOST], REPO, no_python=True), 90,
            require_no_python=True)
        result["children"].append(child_result)
        report = cpp_json_marker(Path(child_result["log"]), "AVAILABILITY_PASS")
        availability_logs[label] = report
        return report

    healthy = run_availability("vm11-all-replicas-healthy", "healthy", REPO_A_ID)
    vm11_stopped_a = stop_repo_server(servers[0])
    unreachable = run_availability("vm11-repo-a-unreachable", "unreachable", REPO_A_ID)
    failover_destination = work / "vm11-failover-fetch.bin"
    child = run_child(
        ndn, output, processes, APP_HOST, "vm11-fetch-failover-a-to-b-or-c",
        [str(repo_client), "--mode", "failover-fetch", "--handle", str(three_handle),
         "--destination", str(failover_destination), "--failed-provider", REPO_A_PROVIDER,
         "--timeout-ms", "15000"] + three_repo_args,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 90,
        require_no_python=True)
    result["children"].append(child)
    failover_child = child
    if digest(failover_destination) != digest(three_source):
        raise RuntimeError("VM-11 failover fetch did not preserve the complete artifact")
    server_restart = restart_repo_servers(
        ndn, output, processes, environments, servers, "vm11-repo-a-return")
    time.sleep(3)

    repo_a_payload = next(
        Path(servers[0]["repo_dir"]) / item["path"]
        for item in repo_database_state(servers[0]["repo_dir"])["payload_files"]
        if item["sha256"] == three_digest)
    parked_payload = repo_a_payload.with_name(repo_a_payload.name + ".vm11-parked")
    os.replace(repo_a_payload, parked_payload)
    try:
        missing = run_availability("vm11-repo-a-object-missing", "missing", REPO_A_ID)
    finally:
        os.replace(parked_payload, repo_a_payload)
    if digest(repo_a_payload) != three_digest:
        raise RuntimeError("VM-11 missing-object availability setup did not restore exact bytes")

    original_payload = repo_a_payload.read_bytes()
    try:
        with repo_a_payload.open("r+b") as stream:
            stream.write(bytes([original_payload[0] ^ 0xff]) + original_payload[1:])
            stream.flush()
            os.fsync(stream.fileno())
        corrupt = run_availability("vm11-repo-a-integrity-failed", "corrupt", REPO_A_ID)
    finally:
        with repo_a_payload.open("r+b") as stream:
            stream.write(original_payload)
            stream.truncate(len(original_payload))
            stream.flush()
            os.fsync(stream.fileno())
    if digest(repo_a_payload) != three_digest:
        raise RuntimeError("VM-11 corruption availability setup did not restore exact bytes")

    three_servers = servers + [repo_c]
    all_down_stops = []
    for server in three_servers:
        all_down_stops.append(stop_repo_server(server))
    all_down_destination = work / "vm12-all-down-must-not-publish.bin"
    child = run_child(
        ndn, output, processes, APP_HOST, "vm12-all-distinct-providers-offline-bounded",
        [str(repo_client), "--mode", "all-down-fetch", "--handle", str(three_handle),
         "--destination", str(all_down_destination), "--timeout-ms", "7000"] + three_repo_args,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 45,
        require_no_python=True)
    result["children"].append(child)
    all_down_restart = restart_repo_servers(
        ndn, output, processes, environments, three_servers, "vm12-all-down-return")
    servers[:] = three_servers[:2]
    repo_c = three_servers[2]
    time.sleep(3)
    result["vm11_vm12_availability_failover"] = {
        "repo_c": {"host": repo_c["host"], "repo_node": REPO_C_ID,
                   "provider_identity": REPO_C_PROVIDER, "pid": repo_c["pid"]},
        "three_repo_receipt_sha256": digest(three_receipt),
        "three_repo_source_sha256": three_digest,
        "three_repo_storage_before": three_repo_before,
        "three_repo_storage_after": three_repo_after,
        "availability": availability_logs,
        "unreachable_repo_stop": vm11_stopped_a,
        "failover_client": failover_child,
        "repo_a_return": server_restart,
        "all_offline_stops": all_down_stops,
        "all_offline_restart": all_down_restart,
        "all_offline_client": child,
    }

    result["python_binding_interop"] = {
        "cpp_to_python_digest": digest(output / "artifacts/python-fetched-cpp.bin"),
        "python_to_cpp_digest": digest(output / "artifacts/cpp-fetched-python.bin"),
        "python_receipt": json.loads(python_receipt.read_text(encoding="utf-8")),
        "cpp_receipt": json.loads(archived_receipt.read_text(encoding="utf-8")),
    }
    return result


def run_spec193_discovery(ndn, output: Path, processes: List[Dict[str, Any]],
                          repo_client: Path, servers: List[Dict[str, Any]],
                          trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A05 requires two independent C++ Repo services")
    source = output / "discovery-probe.bin"
    source.write_bytes(b"Spec193 capability discovery probe\x00v1")
    before = {server["repo_node"]: repo_database_state(server["repo_dir"])
              for server in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    write_operations = {"STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES",
                        "INSERT", "STORE_MANIFEST", "DELETE"}
    authenticated_requests_before = repo_request_events(processes,
                                                          set(before))
    capture_hosts = (APP_HOST, REPO_A_HOST, REPO_B_HOST)
    capture_offsets = {
        host: (output / "pcap" / (host + ".pcap")).stat().st_size
        for host in capture_hosts
    }

    argv = [str(repo_client), "--mode", "discover", "--source", str(source),
            "--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--user", USER,
            "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    child = run_child(ndn, output, processes, APP_HOST, "spec193-a05-discover",
                      argv, node_environment(ndn.net[APP_HOST], REPO,
                                             no_python=True), 180,
                      require_no_python=True)
    time.sleep(1)
    capture_deltas = {}
    for host in capture_hosts:
        path = output / "pcap" / (host + ".pcap")
        with path.open("rb") as stream:
            stream.seek(capture_offsets[host])
            captured = stream.read()
        if not captured:
            raise RuntimeError("A05 captured no new NDN packets on host " + host)
        capture_deltas[host] = {"bytes": len(captured),
                                "sha256": hashlib.sha256(captured).hexdigest()}
    lines = read_log(Path(child["log"])).splitlines()
    request_lines = [line for line in lines if line.startswith("DISCOVERY_REQUEST id=")]
    if len(request_lines) != 1:
        raise RuntimeError("A05 discovery did not return exactly one request ID")
    request_id = request_lines[0][len("DISCOVERY_REQUEST id="):].lstrip("/")
    if not request_id or "/" in request_id:
        raise RuntimeError("A05 discovery returned a malformed request ID")
    candidates = []
    for line in lines:
        match = re.fullmatch(
            r"DISCOVERY_CANDIDATE repo=(\S+) provider=(\S+) prefix=(\S+) "
            r"wire_digest=(sha256:[0-9a-f]{64}) locator=(\S+)",
            line.strip())
        if match:
            candidates.append({"repo": match.group(1),
                               "provider": match.group(2),
                               "prefix": match.group(3),
                               "ack_wire_digest": match.group(4),
                               "signer_key_locator": match.group(5)})
    expected = {(REPO_A_ID, REPO_A_PROVIDER, SERVICE_PREFIX),
                (REPO_B_ID, REPO_B_PROVIDER, SERVICE_PREFIX)}
    actual = {(item["repo"], item["provider"], item["prefix"])
              for item in candidates}
    if (len(candidates) != 2 or actual != expected or
            "DISCOVERY_COMPLETE count=2" not in lines):
        raise RuntimeError("A05 capability candidates differ from the two authenticated Repo services")
    after = {server["repo_node"]: repo_database_state(server["repo_dir"])
             for server in servers}
    for repo_node in before:
        if repo_table_row_counts(before[repo_node]) != repo_table_row_counts(after[repo_node]) or \
                before[repo_node]["payload_files"] != after[repo_node]["payload_files"] or \
                before[repo_node]["repo_manifests"] != after[repo_node]["repo_manifests"]:
            raise RuntimeError("A05 discovery mutated Repo storage: " + repo_node)
    authenticated_requests_after = repo_request_events(processes, set(before))
    if authenticated_requests_after[:len(authenticated_requests_before)] != \
            authenticated_requests_before:
        raise RuntimeError("A05 authenticated request trace changed before discovery")
    new_requests = authenticated_requests_after[len(authenticated_requests_before):]
    if any(event["operation"] in write_operations for event in new_requests):
        raise RuntimeError("A05 discovery reached a Repo write service")
    return {"case_id": "A05", "status": "PENDING_PACKET_ORACLE", "children": [child],
            "source_sha256": digest(source), "request_id": request_id,
            "candidates": candidates, "capture_offsets": capture_offsets,
            "repo_state_before": before, "repo_state_after": after,
            "authenticated_requests": new_requests,
            "capability_capture_deltas": capture_deltas}


def run_spec193_object_baseline(ndn, output: Path,
                                processes: List[Dict[str, Any]],
                                repo_client: Path,
                                servers: List[Dict[str, Any]],
                                trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("Spec193 object baseline requires two independent Repos")
    source = output / "artifacts/spec193-object-source.bin"
    destination = output / "artifacts/spec193-object-fetched.bin"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(b"Spec193 object protocol across MiniNDN\x00store-fetch-v1")
    object_name = "/spec193/object/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    capture_offsets = {
        host: (output / "pcap" / (host + ".pcap")).stat().st_size
        for host in (APP_HOST, REPO_A_HOST, REPO_B_HOST)
    }
    argv = [str(repo_client), "--mode", "object-store-fetch",
            "--source", str(source), "--destination", str(destination),
            "--object-name", object_name,
            "--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--user", USER,
            "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    child = run_child(ndn, output, processes, APP_HOST,
                      "spec193-object-store-fetch", argv,
                      node_environment(ndn.net[APP_HOST], REPO, no_python=True),
                      420, require_no_python=True)
    if digest(destination) != digest(source):
        raise RuntimeError("object network fetch differs from source bytes")
    child_log = read_log(Path(child["log"]))
    if "SPEC193_OBJECT_NETWORK_PASS" not in child_log:
        raise RuntimeError("object client did not report complete network sequence")
    capability = re.search(
        r"SPEC193_OBJECT_CAPABILITY request_id=(\S+) "
        r"ack_digest=(sha256:[0-9a-f]{64}) locator=(\S+) receipts=0",
        child_log)
    if capability is None:
        raise RuntimeError("object client lacked authenticated no-receipt capability")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    before_a, after_a = before[REPO_A_ID], after[REPO_A_ID]
    before_b, after_b = before[REPO_B_ID], after[REPO_B_ID]
    selected_entries = [item for item in after_a["repo_manifests"]["entries"]
                        if item["objectName"] == object_name and
                        item["sha256"] == digest(source)]
    if len(selected_entries) != 1 or \
            after_a["repo_manifests"]["rows"] != \
            before_a["repo_manifests"]["rows"] + 1:
        raise RuntimeError("selected object Repo lacks one new durable manifest")
    if not any(item["sha256"] == digest(source)
               for item in after_a["payload_files"]):
        raise RuntimeError("selected object Repo lacks durable source bytes")
    if before_b != after_b:
        raise RuntimeError("unselected Repo storage changed during object request")
    if any(".artifact-v2.sqlite" in path for path in after_a["databases"]):
        raise RuntimeError("object protocol Repo unexpectedly created artifact metadata")
    return {"case_id": "OBJECT_BASELINE", "status": "PENDING_PACKET_ORACLE",
            "children": [child], "object_name": object_name,
            "source_sha256": digest(source), "fetched_sha256": digest(destination),
            "capability_request_id": capability.group(1),
            "capability_ack_wire_digest": capability.group(2),
            "capability_signer_key_locator": capability.group(3),
            "capture_offsets": capture_offsets,
            "repo_state_before": before, "repo_state_after": after}


def run_spec193_persistence_seed(ndn, output: Path,
                                 processes: List[Dict[str, Any]],
                                 repo_client: Path,
                                 servers: List[Dict[str, Any]],
                                 trust_schema: Path,
                                 durable_repo_root: Path) -> Dict[str, Any]:
    result = run_spec193_object_baseline(
        ndn, output, processes, repo_client, servers, trust_schema)
    result["case_id"] = "PERSIST_SEED"
    result["durable_repo_root"] = str(durable_repo_root)
    result["source_size"] = (output / "artifacts/spec193-object-source.bin").stat().st_size
    return result


def run_spec193_persistence_read(ndn, output: Path,
                                 processes: List[Dict[str, Any]],
                                 repo_client: Path,
                                 servers: List[Dict[str, Any]],
                                 trust_schema: Path,
                                 durable_repo_root: Path,
                                 seed_manifest_path: Path,
                                 candidate_lock_sha256: str) -> Dict[str, Any]:
    seed_manifest = json.loads(seed_manifest_path.read_text(encoding="utf-8"))
    seed = seed_manifest.get("scenario_result", {})
    if (seed_manifest.get("status") != "PASS" or
            seed_manifest.get("minindn_cleanup") != "PASS" or
            seed_manifest.get("preflight_candidate_lock_sha256") !=
            candidate_lock_sha256 or
            seed.get("case_id") != "PERSIST_SEED" or
            seed.get("status") != "PASS" or
            seed.get("durable_repo_root") != str(durable_repo_root)):
        raise RuntimeError("persistence read does not match a clean seed run and candidate")
    object_name = seed["object_name"]
    source_sha256 = seed["source_sha256"]
    source_size = seed["source_size"]
    if not re.fullmatch(r"[0-9a-f]{64}", source_sha256) or source_size <= 0:
        raise RuntimeError("seed object digest or size is invalid")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    entries = [item for item in before[REPO_A_ID]["repo_manifests"]["entries"]
               if item["objectName"] == object_name and
               item["sha256"] == source_sha256 and
               item["size"] == source_size]
    if (len(entries) != 1 or
            not any(item["sha256"] == source_sha256
                    for item in before[REPO_A_ID]["payload_files"]) or
            before[REPO_B_ID]["repo_manifests"]["rows"] != 0):
        raise RuntimeError("reopened Repo disk lacks the seed object before fetch")
    destination = output / "artifacts/persist-reopened.bin"
    requests_before = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    child = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-persist-reopen-fetch",
        [str(repo_client), "--mode", "object-reopen-fetch",
         "--object-name", object_name, "--expected-sha256", source_sha256,
         "--expected-size", str(source_size), "--destination", str(destination),
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--user", CONSUMER,
         "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if (digest(destination) != source_sha256 or
            destination.stat().st_size != source_size or
            "SPEC193_OBJECT_REOPEN_PASS" not in read_log(Path(child["log"]))):
        raise RuntimeError("new MiniNDN consumer did not recover exact seed bytes")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    requests_after = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    new_requests = requests_after[len(requests_before):]
    if (before != after or len(new_requests) < 3 or
            {"MANIFEST", "INVENTORY", "FETCH"} -
            {item["operation"] for item in new_requests} or
            any(item["operation"] in {"STORE", "STORE_RANGE", "COMMIT_RANGES"}
                for item in new_requests)):
        raise RuntimeError("reopened read changed storage or lacked network Repo reads")
    return {"case_id": "PERSIST_READ", "status": "PASS",
            "children": [child], "seed_manifest": str(seed_manifest_path),
            "seed_manifest_sha256": digest(seed_manifest_path),
            "durable_repo_root": str(durable_repo_root),
            "object_name": object_name, "source_sha256": source_sha256,
            "source_size": source_size, "fetched_sha256": digest(destination),
            "repo_state_before": before, "repo_state_after": after,
            "authenticated_requests": new_requests}


def run_spec193_artifact_persistence_seed(ndn, output: Path,
                                          processes: List[Dict[str, Any]],
                                          repo_client: Path,
                                          servers: List[Dict[str, Any]],
                                          trust_schema: Path,
                                          durable_repo_root: Path) -> Dict[str, Any]:
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "persist-artifact-source.bin"
    source.write_bytes(bytes((index * 61 + 23) % 256 for index in range(4401)))
    source_sha256 = digest(source)
    handle = work / "persist-artifact-handle.json"
    receipt_path = work / "persist-artifact-receipt.json"
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    publisher = run_child(
        ndn, output, processes, APP_HOST, "spec193-persist-artifact-publish",
        [str(repo_client), "--mode", "publish", "--source", str(source),
         "--handle-out", str(handle), "--receipt-out", str(receipt_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        1200, require_no_python=True)
    ensure_two_repo_commits(receipt_path)
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    for state in after.values():
        assert_repo_durable_state(state, source_sha256)
    source.unlink()
    return {"case_id": "PERSIST_ARTIFACT_SEED", "status": "PASS",
            "children": [publisher], "durable_repo_root": str(durable_repo_root),
            "source_sha256": source_sha256, "source_size": 4401,
            "source_removed": not source.exists(), "handle": str(handle),
            "handle_sha256": digest(handle), "receipt_sha256": digest(receipt_path),
            "repo_state_before": before, "repo_state_after": after}


def run_spec193_artifact_persistence_read(ndn, output: Path,
                                          processes: List[Dict[str, Any]],
                                          repo_client: Path,
                                          servers: List[Dict[str, Any]],
                                          trust_schema: Path,
                                          durable_repo_root: Path,
                                          seed_manifest_path: Path,
                                          candidate_lock_sha256: str) -> Dict[str, Any]:
    seed_manifest = json.loads(seed_manifest_path.read_text(encoding="utf-8"))
    seed = seed_manifest.get("scenario_result", {})
    if (seed_manifest.get("status") != "PASS" or
            seed_manifest.get("minindn_cleanup") != "PASS" or
            seed_manifest.get("preflight_candidate_lock_sha256") !=
            candidate_lock_sha256 or
            seed.get("case_id") != "PERSIST_ARTIFACT_SEED" or
            seed.get("status") != "PASS" or
            seed.get("durable_repo_root") != str(durable_repo_root) or
            not seed.get("source_removed")):
        raise RuntimeError("artifact read does not match a clean seed run and candidate")
    handle = Path(seed["handle"])
    if digest(handle) != seed["handle_sha256"]:
        raise RuntimeError("artifact handle changed between MiniNDN runs")
    source_sha256 = seed["source_sha256"]
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_repo_durable_state(state, source_sha256)
    destination = output / "artifacts/persist-artifact-reopened.bin"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    requests_before = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    consumer = run_child(
        ndn, output, processes, CONSUMER_HOST,
        "spec193-persist-artifact-reopen-fetch",
        [str(repo_client), "--mode", "fetch", "--handle", str(handle),
         "--destination", str(destination), "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if (digest(destination) != source_sha256 or
            destination.stat().st_size != seed["source_size"]):
        raise RuntimeError("new MiniNDN consumer did not recover seed artifact bytes")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    new_requests = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})[
        len(requests_before):]
    if before != after or not new_requests or any(
            item["operation"] in {"STORE", "STORE_RANGE", "COMMIT_RANGES"}
            for item in new_requests):
        raise RuntimeError("artifact reopen changed storage or lacked network reads")
    return {"case_id": "PERSIST_ARTIFACT_READ", "status": "PASS",
            "children": [consumer], "seed_manifest": str(seed_manifest_path),
            "seed_manifest_sha256": digest(seed_manifest_path),
            "durable_repo_root": str(durable_repo_root),
            "source_sha256": source_sha256, "source_size": seed["source_size"],
            "fetched_sha256": digest(destination),
            "repo_state_before": before, "repo_state_after": after,
            "authenticated_requests": new_requests}


def run_spec193_local_registry_object(ndn, output: Path,
                                      processes: List[Dict[str, Any]],
                                      servers: List[Dict[str, Any]]) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("L01 requires two independent network Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "l01-local-source.bin"
    report_path = work / "l01-local-report.json"
    local_root = work / "l01-local-repo"
    source.write_bytes(bytes((index * 47 + 13) % 256 for index in range(521)))
    object_name = "/spec193/local-registry/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    requests_before = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    child = run_child(
        ndn, output, processes, APP_HOST, "spec193-l01-local-registry",
        servers[0]["argv"] + [
            "--local-command", "registry-object", "--local-source", str(source),
            "--local-object", object_name, "--local-manifest", str(report_path),
            "--local-store-root", str(local_root / "repo.sqlite.store")],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_L01_LOCAL_PASS" not in read_log(Path(child["log"])):
        raise RuntimeError("L01 local registry child did not report completion")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    local_state = repo_database_state(local_root)
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    requests_after = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    if (report["stored"]["objectName"] != object_name or
            report["stored"]["sha256"] != digest(source) or
            report["witness"]["objectName"] != object_name + "/witness" or
            report["witness"]["sha256"] != digest(source) or
            report["afterPutCount"] != 1 or
            report["afterRemoveCount"] != 0 or
            not report["missingGetRejected"] or
            not report["missingManifestRejected"]):
        raise RuntimeError("L01 local registry report contradicts source or lifecycle")
    entries = local_state["repo_manifests"]["entries"]
    if (len(entries) != 1 or entries[0]["objectName"] != object_name + "/witness" or
            entries[0]["sha256"] != digest(source) or
            not any(item["sha256"] == digest(source)
                    for item in local_state["payload_files"])):
        raise RuntimeError("L01 isolated local store lacks durable witness")
    if before != after or requests_before != requests_after:
        raise RuntimeError("L01 local registry changed a network Repo or reached its handler")
    return {"case_id": "L01", "status": "PASS", "children": [child],
            "object_name": object_name, "source_sha256": digest(source),
            "local_report": report, "local_state": local_state,
            "remote_state_before": before, "remote_state_after": after,
            "remote_request_count_before": len(requests_before),
            "remote_request_count_after": len(requests_after)}


def run_spec193_network_artifact_wrappers(ndn, output: Path,
                                        processes: List[Dict[str, Any]],
                                        environments: Dict[str, Dict[str, str]],
                                        repo_client: Path, servers: List[Dict[str, Any]],
                                        trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("artifact wrappers require two independent Persistent Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "nw-artifact-source.bin"
    source.write_bytes(bytes((index * 71 + 31) % 256 for index in range(521)))
    object_name = "/spec193/artifact-wrappers/" + output.name
    report_path = work / "nw-artifact-report.json"
    before = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    requests_before = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    stopped_b = stop_repo_server(servers[1])
    child = run_child(
        ndn, output, processes, APP_HOST, "spec193-nw-artifact-wrappers",
        [str(repo_client), "--mode", "network-object-wrappers", "--phase", "artifact",
         "--source", str(source), "--object-name", object_name, "--report-out", str(report_path),
         "--user", USER, "--group", GROUP, "--controller", CONTROLLER, "--authority", CONTROLLER,
         "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 650, require_no_python=True)
    if "SPEC193_NETWORK_WRAPPERS_PASS phase=artifact" not in read_log(Path(child["log"])):
        raise RuntimeError("artifact wrappers did not complete")
    report = json.loads(report_path.read_text())
    calls = report["calls"]
    responses = {}
    for call in calls:
        path = work / call["payloadFile"]
        if digest(path) != call["payloadSha256"]:
            raise RuntimeError("artifact wrapper saved response digest mismatch")
        responses.setdefault(call["operation"], []).append(path.read_bytes())
    counts = {key: len(value) for key, value in responses.items()}
    expected_counts = {"PUBLISH_INTENT": 2, "ARTIFACT_STORE": 1, "ARTIFACT_STORE_RANGE": 2,
                       "ARTIFACT_COMMIT_RANGES": 1, "LOCATION": 2, "RECEIPT": 2,
                       "AVAILABILITY": 2, "ARTIFACT_FETCH": 2}
    if any(counts.get(key) != value for key, value in expected_counts.items()) or counts.get("PUBLISH_STATUS", 0) < 2:
        raise RuntimeError("artifact wrapper invocation coverage incomplete")
    if any(payload != source.read_bytes() for payload in responses["ARTIFACT_FETCH"]):
        raise RuntimeError("artifact wrapper FETCH bytes differ from source")
    commits = [json.loads(responses[key][0]) for key in ("ARTIFACT_STORE", "ARTIFACT_COMMIT_RANGES")]
    receipts = [json.loads(value) for value in responses["RECEIPT"]]
    locations = [json.loads(value) for value in responses["LOCATION"]]
    availability = [json.loads(value) for value in responses["AVAILABILITY"]]
    expected_names = {}
    for index, commit in enumerate(commits):
        reference = commit["artifact"]
        if (reference["logicalName"] != object_name + ("/whole" if index == 0 else "/range") or
                reference["contentDigest"] != digest(source) or reference["sizeBytes"] != source.stat().st_size or
                reference["publisherIdentity"] != USER or commit["repoNode"] != REPO_A_ID or
                commit["providerIdentity"] != REPO_A_PROVIDER or commit["persistedBytes"] != source.stat().st_size):
            raise RuntimeError("independent artifact receipt reference/owner binding failed")
        receipt, location, observation = receipts[index], locations[index], availability[index]
        if (receipt["reference"] != reference or receipt["state"] != "COMMITTED" or
                receipt["operationId"] != commit["operationId"] or receipt["achievedReplicas"] != 1 or
                location["artifact"] != reference or location["receiptId"] != commit["receiptId"] or
                location["operationId"] != commit["operationId"] or observation["artifact"] != reference or
                observation["report"]["objectPresence"] != "PRESENT" or
                observation["report"]["integrity"] != "VERIFIED"):
            raise RuntimeError("historical/current artifact wrapper responses contradict commit")
        reference_wire = json.dumps(reference, separators=(",", ":"), ensure_ascii=False).encode()
        expected_names["/_ndnsf/artifact-v2/" + hashlib.sha256(reference_wire).hexdigest()] = digest(source)
    after = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    assert_repo_durable_state(after[REPO_A_ID], digest(source))
    manifests = after[REPO_A_ID]["repo_manifests"]["entries"]
    if ({row["objectName"]: row["sha256"] for row in manifests} != expected_names or
            any(row["objectType"] != "artifact-v2-bytes" for row in manifests) or
            after[REPO_B_ID] != before[REPO_B_ID] or
            any("/staging/" in "/" + row["path"] for row in after[REPO_A_ID]["payload_files"])):
        raise RuntimeError("artifact wrapper exact physical inventory or unselected B oracle failed")
    after_write = after
    target = [servers[1]]
    restarted_b = restart_repo_servers(ndn, output, processes, environments, target, "nw-metadata")
    servers[1] = target[0]
    reports = {"artifact": report}
    children = [child]
    def observe(phase: str) -> dict:
        path = work / ("nw-" + phase + "-report.json")
        receipt_file = next(work / call["payloadFile"] for call in calls if call["operation"] == "RECEIPT")
        location_file = next(work / call["payloadFile"] for call in calls if call["operation"] == "LOCATION")
        result = run_child(
            ndn, output, processes, APP_HOST, "spec193-nw-artifact-" + phase,
            [str(repo_client), "--mode", "network-object-wrappers", "--phase", phase,
             "--source", str(source), "--object-name", object_name, "--report-out", str(path),
             "--receipt", str(receipt_file), "--location", str(location_file),
             "--user", USER, "--group", GROUP, "--controller", CONTROLLER, "--authority", CONTROLLER,
             "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
             "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER, "--provider-b", REPO_B_PROVIDER],
            node_environment(ndn.net[APP_HOST], REPO, no_python=True), 180, require_no_python=True)
        children.append(result)
        if "SPEC193_NETWORK_WRAPPERS_PASS phase=" + phase not in read_log(Path(result["log"])):
            raise RuntimeError("physical-loss wrapper phase did not finish: " + phase)
        saved = json.loads(path.read_text())
        expected_methods = ["RECEIPT", "LOCATION", "AVAILABILITY"] + (["ARTIFACT_FETCH"] if phase == "restored" else [])
        if [call["operation"] for call in saved["calls"]] != expected_methods:
            raise RuntimeError("physical-loss wrapper invocation list differs")
        values = {}
        for call in saved["calls"]:
            file = work / call["payloadFile"]
            if digest(file) != call["payloadSha256"]:
                raise RuntimeError("physical-loss wrapper saved response hash mismatch")
            values[call["operation"]] = file.read_bytes()
        current = json.loads(values["AVAILABILITY"])
        expected_fence = {key: locations[0][key] for key in
                          ("repoNode", "providerIdentity", "operationId", "receiptId", "generation", "locationVersion")}
        if (values["RECEIPT"] != receipt_file.read_bytes() or values["LOCATION"] != location_file.read_bytes() or
                current["artifact"] != commits[0]["artifact"] or current["expectedLocation"] != expected_fence or
                current["report"]["repoNode"] != REPO_A_ID or current["report"]["providerIdentity"] != REPO_A_PROVIDER or
                current["report"]["reachability"] != "REACHABLE" or
                current["report"]["objectPresence"] != ("PARTIAL" if phase == "missing" else "PRESENT") or
                current["report"]["integrity"] != ("NOT_CHECKED" if phase == "missing" else "VERIFIED") or
                (phase == "restored" and values["ARTIFACT_FETCH"] != source.read_bytes())):
            raise RuntimeError("independent oracle failed historical/current physical-loss distinction")
        reports[phase] = saved
        return current
    payload_rows = [row for row in after_write[REPO_A_ID]["payload_files"]
                    if "/payloads/sha256/" in "/" + row["path"] and row["sha256"] == digest(source)]
    if len(payload_rows) != 1:
        raise RuntimeError("physical-loss oracle needs exactly one deduplicated payload")
    owner = next(server for server in servers if server["repo_node"] == REPO_A_ID)
    payload = owner["repo_dir"] / payload_rows[0]["path"]
    displaced = work / "nw-displaced-payload.bin"
    if displaced.exists() or digest(payload) != digest(source):
        raise RuntimeError("physical-loss fault input is not the exact source payload")
    payload.replace(displaced)
    try:
        missing_observation = observe("missing")
        after_missing = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
        missing_a = after_missing[REPO_A_ID]
        if (missing_a["databases"] != after_write[REPO_A_ID]["databases"] or
                missing_a["repo_manifests"] != after_write[REPO_A_ID]["repo_manifests"] or
                missing_a["payload_files"] != [row for row in after_write[REPO_A_ID]["payload_files"] if row != payload_rows[0]] or
                after_missing[REPO_B_ID] != before[REPO_B_ID] or digest(displaced) != digest(source)):
            raise RuntimeError("physical-loss oracle found metadata mutation or unintended file loss")
    finally:
        displaced.replace(payload)
    restored_observation = observe("restored")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    if after != after_write or digest(payload) != digest(source):
        raise RuntimeError("restored wrapper read changed inventory or payload bytes")
    events = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})[len(requests_before):]
    if not events or any(event["repo_node"] != REPO_A_ID or event["requester"] != USER for event in events):
        raise RuntimeError("artifact wrapper handler principal mismatch")
    return {"case_id": "NW_ARTIFACT", "status": "PASS", "children": children,
            "reports": reports, "source_sha256": digest(source),
            "commits": commits, "receipts": receipts, "locations": locations,
            "availability": availability, "repo_state_before": before,
            "repo_state_after_write": after_write, "repo_state_after_missing": after_missing,
            "repo_state_after": after, "missing_observation": missing_observation,
            "restored_observation": restored_observation, "authenticated_requests": events,
            "stopped_repo_b": stopped_b, "restarted_repo_b": restarted_b}


def run_spec193_network_object_wrappers(ndn, output: Path,
                                      processes: List[Dict[str, Any]],
                                      environments: Dict[str, Dict[str, str]],
                                      repo_client: Path, servers: List[Dict[str, Any]],
                                      trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("network wrappers require two independent object Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "nw-source.bin"
    source.write_bytes(bytes((index * 67 + 29) % 256 for index in range(521)))
    wire, certificate = work / "nw-producer.wire", work / "nw-producer-certificate.wire"
    object_name = "/spec193/network-wrappers/" + output.name
    data_prefix = CONSUMER + "/spec193/network-wrappers/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    requests_before = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    publisher = spawn(
        ndn, output, processes, CONSUMER_HOST, "spec193-nw-producer",
        [str(repo_client), "--mode", "object-data-publisher", "--source", str(source),
         "--wire-source", str(wire), "--certificate", str(certificate),
         "--data-prefix", data_prefix, "--user", CONSUMER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True))
    assert_no_python_mapped(publisher)
    wait_log(publisher, "SPEC193_DATA_PUBLISHER_READY", 30)
    stopped_b = stop_repo_server(servers[1])
    report_paths = {phase: work / ("nw-" + phase + "-report.json") for phase in ("object", "catalog")}
    base = ["--source", str(source), "--wire-source", str(wire), "--object-name", object_name,
            "--data-prefix", data_prefix, "--user", USER, "--group", GROUP,
            "--controller", CONTROLLER, "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX, "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER]
    objects = run_child(
        ndn, output, processes, APP_HOST, "spec193-nw-object-wrappers",
        [str(repo_client), "--mode", "network-object-wrappers", "--phase", "object",
         "--report-out", str(report_paths["object"])] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 650, require_no_python=True)
    if "SPEC193_NETWORK_WRAPPERS_PASS phase=object" not in read_log(Path(objects["log"])):
        raise RuntimeError("object wrappers did not complete")
    after_object = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    if after_object[REPO_B_ID] != before[REPO_B_ID]:
        raise RuntimeError("offline unselected Repo B changed during wrappers")
    target = [servers[1]]
    restarted_b = restart_repo_servers(ndn, output, processes, environments, target,
                                       "nw-catalog", ready_text="Spec193 object Repo serving")
    servers[1] = target[0]
    catalogs = run_child(
        ndn, output, processes, APP_HOST, "spec193-nw-selected-catalog",
        [str(repo_client), "--mode", "network-object-wrappers", "--phase", "catalog",
         "--report-out", str(report_paths["catalog"])] + base + ["--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 180, require_no_python=True)
    if "SPEC193_NETWORK_WRAPPERS_PASS phase=catalog" not in read_log(Path(catalogs["log"])):
        raise RuntimeError("selected catalog wrappers did not complete")
    stopped_publisher = stop_daemon(publisher)
    reports = {phase: json.loads(path.read_text()) for phase, path in report_paths.items()}
    calls = reports["object"]["calls"]
    expected_operations = ["CAPABILITY", "CACHE_STATUS", "STORE", "STORE_RANGE", "STORE_RANGE",
                           "COMMIT_RANGES", "STORE_RANGE", "ABORT_RANGES", "FETCH", "FETCH_RANGE",
                           "FETCH_RANGE_DIRECT", "MANIFEST", "INSERT", "STATUS", "FETCH_DATA",
                           "INVENTORY", "DELETE"]
    if [call["operation"] for call in calls] != expected_operations:
        raise RuntimeError("wrapper invocation list is incomplete")
    for report in reports.values():
        for call in report["calls"]:
            if digest(work / call["payloadFile"]) != call["payloadSha256"]:
                raise RuntimeError("saved wrapper response digest differs from native callback")
    def response(operation):
        call = next(item for item in calls if item["operation"] == operation)
        return (work / call["payloadFile"]).read_bytes()
    capability = json.loads(response("CAPABILITY"))
    if capability["repoNode"] != REPO_A_ID or capability["failureDomain"] != "ucla":
        raise RuntimeError("object wrapper capability identity/domain mismatch")
    if (response("FETCH") != source.read_bytes() or response("FETCH_DATA") != wire.read_bytes() or
            response("FETCH_RANGE") != source.read_bytes()[37:150] or
            response("FETCH_RANGE_DIRECT") != source.read_bytes()[37:150] or
            json.loads(response("STATUS"))["state"] != "DONE"):
        raise RuntimeError("independent wrapper byte/status oracle failed")
    from spec193_packet_oracle import _certificate, _verify_signature
    from ndn.encoding import Name, parse_data
    root_name, root_key, root_signature, _ = _certificate(output / "identity/root.cert")
    _verify_signature(root_signature, root_key,
                      Name.to_str(root_signature.signature_info.key_locator.name), "root certificate")
    producer_name, producer_key, producer_signature, producer_digest = _certificate(output / "identity/arizona.cert")
    _verify_signature(producer_signature, root_key, root_name, "producer certificate")
    packet_name, _, content, packet_signature = parse_data(wire.read_bytes())
    _verify_signature(packet_signature, producer_key, producer_name, "original producer Data")
    if (not producer_name.startswith(CONSUMER + "/KEY/") or
            digest(certificate) != producer_digest or bytes(content) != source.read_bytes()):
        raise RuntimeError("producer identity/content differs from pinned source")
    expected = {object_name + "/range": digest(source), object_name + "/insert": digest(wire), Name.to_str(packet_name): digest(wire)}
    actual = {row["objectName"]: row["sha256"] for row in after_object[REPO_A_ID]["repo_manifests"]["entries"]}
    physical = {row["sha256"] for row in after_object[REPO_A_ID]["payload_files"]
                if "/payloads/sha256/" in "/" + row["path"]}
    if (actual != expected or physical != {digest(source), digest(wire)} or
            any("/staging/" in "/" + row["path"] for row in after_object[REPO_A_ID]["payload_files"])):
        raise RuntimeError("wrapper final manifest/payload/staging oracle failed")
    catalog_calls = reports["catalog"]["calls"]
    if [call["operation"] for call in catalog_calls] != ["CATALOG_STATUS", "CATALOG_SNAPSHOT", "CATALOG_DELTA", "CATALOG_LOOKUP"]:
        raise RuntimeError("selected catalog invocation list incomplete")
    catalog = {call["operation"]: json.loads((work / call["payloadFile"]).read_text()) for call in catalog_calls}
    if (catalog["CATALOG_STATUS"]["objectCount"] != 3 or
            catalog["CATALOG_LOOKUP"]["manifest"]["sha256"] != digest(source) or
            {row["manifest"]["objectName"]: row["manifest"]["sha256"] for row in catalog["CATALOG_SNAPSHOT"]["entries"]} != expected or
            not any(row["state"] == "DELETED" and row["manifest"]["objectName"] == object_name + "/primary"
                    for row in catalog["CATALOG_DELTA"]["entries"])):
        raise RuntimeError("catalog wrapper responses contradict physical store")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    if after != after_object or after[REPO_B_ID] != before[REPO_B_ID]:
        raise RuntimeError("catalog reads or unselected B changed physical storage")
    events = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})[len(requests_before):]
    if not events or any(event["repo_node"] != REPO_A_ID or event["requester"] != USER for event in events):
        raise RuntimeError("wrapper handler execution identity mismatch")
    return {"case_id": "NW_OBJECT", "status": "PASS", "object_name": object_name,
            "children": [objects, catalogs], "reports": reports, "source_sha256": digest(source),
            "original_wire_sha256": digest(wire), "repo_state_before": before,
            "repo_state_after_object": after_object, "repo_state_after": after,
            "authenticated_requests": events, "stopped_repo_b": stopped_b,
            "restarted_repo_b": restarted_b, "producer_stop": stopped_publisher}


def run_spec193_local_registry_insert_segmented(ndn, output: Path,
                                               processes: List[Dict[str, Any]],
                                               servers: List[Dict[str, Any]]) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("L03/L04 requires two independent network Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "l03-l04-source.bin"
    source.write_bytes(bytes((index * 59 + 19) % 256 for index in range(521)))
    report_path = work / "l03-l04-report.json"
    local_root = work / "l03-l04-local-repo"
    object_name = "/spec193/local-insert-segmented/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    requests_before = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    argv = list(servers[0]["argv"])
    argv[argv.index("--provider-identity") + 1] = USER
    argv[argv.index("--repo-node") + 1] = object_name + "/repo"
    child = run_child(
        ndn, output, processes, APP_HOST, "spec193-l03-l04-local-registry",
        argv + ["--local-command", "registry-insert-segmented",
                "--local-source", str(source), "--local-object", object_name,
                "--local-manifest", str(report_path), "--local-segment-bytes", "128",
                "--local-store-root", str(local_root / "repo.sqlite.store")],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_L03_L04_LOCAL_PASS" not in read_log(Path(child["log"])):
        raise RuntimeError("L03/L04 native local registry did not complete")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    from spec193_packet_oracle import _certificate, _verify_signature
    from ndn.encoding import Name, parse_data
    root_name, root_key, root_signature, _ = _certificate(output / "identity/root.cert")
    _verify_signature(root_signature, root_key,
                      Name.to_str(root_signature.signature_info.key_locator.name), "root certificate")
    certificate_name, caller_key, caller_signature, caller_digest = _certificate(
        output / "identity/neu.cert")
    _verify_signature(caller_signature, root_key, root_name, "application certificate")
    if not certificate_name.startswith(USER + "/KEY/") or \
            digest(work / "l03-certificate.wire") != caller_digest:
        raise RuntimeError("L03 saved signer certificate differs from the application principal")
    wire = (work / "l03-original.wire").read_bytes()
    packet_name, _, content, signature = parse_data(wire)
    _verify_signature(signature, caller_key, certificate_name, "local original Data")
    if Name.to_str(packet_name) != object_name + "/packet" or bytes(content) != source.read_bytes():
        raise RuntimeError("L03 original Data name/content differs from source")
    wire_digest = digest(work / "l03-original.wire")
    if (report["insertStatus"]["state"] != "DONE" or
            report["insertStatus"]["completedSegments"] != 1 or
            report["failedStatus"]["state"] != "FAILED" or
            report["failedStatus"]["completedSegments"] != 0 or
            report["catalog"]["objectCount"] != 2 or
            report["lookup"]["manifest"]["objectName"] != object_name + "/insert" or
            report["lookup"]["sourceRepo"] != object_name + "/repo" or
            report["cacheAfter"]["hits"] < report["cacheBefore"]["hits"] + 2 or
            not all(report[k] for k in ("missingLookupRejected", "missingSegmentRejected",
                                       "corruptSegmentRejected", "restored"))):
        raise RuntimeError("L03/L04 native report contradicts expected API outcomes")
    expected_insert = {object_name + "/packet", object_name + "/insert"}
    for label in ("snapshot", "delta"):
        rows = report[label]["entries"]
        if ({row["manifest"]["objectName"] for row in rows} != expected_insert or
                any(row["manifest"]["sha256"] != wire_digest or row["state"] != "AVAILABLE" or
                    row["sourceRepo"] != object_name + "/repo" or
                    row["manifest"]["replicaNodes"] != [object_name + "/repo"]
                    for row in rows)):
            raise RuntimeError("L03 local catalog does not describe the exact committed wire")
    payload = source.read_bytes()
    expected = {name: wire_digest for name in expected_insert}
    expected[object_name + "/segmented"] = digest(source)
    expected[object_name + "/plain"] = hashlib.sha256(payload[:23]).hexdigest()
    for index, offset in enumerate(range(0, len(payload), 128)):
        expected[object_name + "/segmented/seg/" + str(index)] = hashlib.sha256(
            payload[offset:offset + 128]).hexdigest()
    local_state = repo_database_state(local_root)
    actual = {row["objectName"]: row["sha256"]
              for row in local_state["repo_manifests"]["entries"]}
    physical = {row["sha256"] for row in local_state["payload_files"]
                if "/payloads/sha256/" in "/" + row["path"]}
    expected_physical = {wire_digest, expected[object_name + "/plain"]}
    expected_physical.update(value for name, value in expected.items()
                             if name.startswith(object_name + "/segmented/seg/"))
    if (actual != expected or physical != expected_physical or
            digest(work / "l04-rebuilt.bin") != digest(source) or
            report["segmented"]["segmentCount"] != 5 or report["plain"]["segmentCount"] != 1 or
            any("/staging/" in "/" + row["path"] for row in local_state["payload_files"])):
        raise RuntimeError("L03/L04 physical manifests/payloads differ from independent source oracle")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    requests_after = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    if before != after or requests_before != requests_after:
        raise RuntimeError("L03/L04 local registry reached or changed a remote Repo")
    return {"case_id": "L03_L04", "status": "PASS", "children": [child],
            "object_name": object_name, "source_sha256": digest(source),
            "original_wire_sha256": wire_digest, "signer_certificate_sha256": caller_digest,
            "local_report": report, "local_state": local_state,
            "remote_state_before": before, "remote_state_after": after,
            "remote_request_count_before": len(requests_before),
            "remote_request_count_after": len(requests_after)}


def run_spec193_local_registry_ranges(ndn, output: Path,
                                      processes: List[Dict[str, Any]],
                                      servers: List[Dict[str, Any]]) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("L02 requires two independent network Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "l02-local-source.bin"
    report_path = work / "l02-local-report.json"
    local_root = work / "l02-local-repo"
    source.write_bytes(bytes((index * 53 + 17) % 256 for index in range(467)))
    object_name = "/spec193/local-ranges/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    requests_before = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    child = run_child(
        ndn, output, processes, APP_HOST, "spec193-l02-local-ranges",
        servers[0]["argv"] + [
            "--local-command", "registry-ranges", "--local-source", str(source),
            "--local-object", object_name, "--local-manifest", str(report_path),
            "--local-store-root", str(local_root / "repo.sqlite.store")],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_L02_LOCAL_PASS" not in read_log(Path(child["log"])):
        raise RuntimeError("L02 local registry child did not report completion")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    local_state = repo_database_state(local_root)
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    requests_after = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
    entries = local_state["repo_manifests"]["entries"]
    if (report["committed"]["objectName"] != object_name + "/committed" or
            report["committed"]["sha256"] != digest(source) or
            report["stagedFiles"] < 1 or
            not all(report[key] for key in ("incompleteRejected", "abortClean",
                                            "boundsRejected", "lengthRejected")) or
            report["finalStagingFiles"] != 0 or
            len(entries) != 1 or
            entries[0]["objectName"] != object_name + "/committed" or
            entries[0]["sha256"] != digest(source) or
            not any(item["sha256"] == digest(source)
                    for item in local_state["payload_files"]) or
            any("/staging/" in "/" + item["path"]
                for item in local_state["payload_files"])):
        raise RuntimeError("L02 local range state contradicts source or abort")
    if before != after or requests_before != requests_after:
        raise RuntimeError("L02 local registry changed a network Repo or reached its handler")
    return {"case_id": "L02", "status": "PASS", "children": [child],
            "object_name": object_name, "source_sha256": digest(source),
            "local_report": report, "local_state": local_state,
            "remote_state_before": before, "remote_state_after": after,
            "remote_request_count_before": len(requests_before),
            "remote_request_count_after": len(requests_after)}


def run_spec193_object_put_replace(ndn, output: Path,
                                   processes: List[Dict[str, Any]],
                                   environments: Dict[str, Dict[str, str]],
                                   repo_client: Path,
                                   servers: List[Dict[str, Any]],
                                   trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O01 requires two independent object Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    first = work / "o01-first.bin"
    replacement = work / "o01-replacement.bin"
    fetched = work / "o01-fetched.bin"
    local_path = work / "o01-local.json"
    network_path = work / "o01-network.json"
    first.write_bytes(bytes((index * 31 + 7) % 256 for index in range(263)))
    replacement.write_bytes(bytes((index * 47 + 19) % 256 for index in range(389)))
    object_name = "/spec193/atomic-replace/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    if any(item["objectName"] == object_name for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O01 target object already exists")
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    local = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o01-local-put-replace",
        servers[0]["argv"] + [
            "--local-command", "object-put-replace",
            "--local-source", str(first),
            "--local-replacement", str(replacement),
            "--local-object", object_name,
            "--local-manifest", str(local_path)],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_O01_LOCAL_PASS generation=3" not in read_log(Path(local["log"])):
        raise RuntimeError("O01 local RepoClient sequence failed")
    local_report = json.loads(local_path.read_text(encoding="utf-8"))
    versions = [local_report[key] for key in ("first", "duplicate", "final")]
    if ([item["generation"] for item in versions] != [1, 2, 3] or
            [item["sha256"] for item in versions] !=
            [digest(first), digest(first), digest(replacement)] or
            local_report["listCount"] != 1):
        raise RuntimeError("O01 local manifest versions contradict source bytes")
    after_local = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    entries = [item for item in after_local[REPO_A_ID]["repo_manifests"]["entries"]
               if item["objectName"] == object_name]
    if (len(entries) != 1 or entries[0]["generation"] != 3 or
            entries[0]["sha256"] != digest(replacement) or
            after_local[REPO_A_ID]["repo_manifests"]["rows"] !=
            before[REPO_A_ID]["repo_manifests"]["rows"] + 1 or
            before[REPO_B_ID] != after_local[REPO_B_ID] or
            not any(item["sha256"] == digest(replacement)
                    for item in after_local[REPO_A_ID]["payload_files"])):
        raise RuntimeError("O01 durable Repo state differs from final version")
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o01-replaced", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    network = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o01-network-final-fetch",
        [str(repo_client), "--mode", "object-version-fetch",
         "--source", str(replacement), "--destination", str(fetched),
         "--report-out", str(network_path), "--object-name", object_name,
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--user", CONSUMER,
         "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if (digest(fetched) != digest(replacement) or
            "SPEC193_O01_NETWORK_PASS generation=3" not in
            read_log(Path(network["log"]))):
        raise RuntimeError("O01 independent consumer did not fetch final version")
    network_report = json.loads(network_path.read_text(encoding="utf-8"))
    if network_report["manifest"] != local_report["final"]:
        raise RuntimeError("O01 network manifest differs from local final version")
    after_network = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    if after_network != after_local:
        raise RuntimeError("O01 restart or network reads changed durable Repo state")
    return {"case_id": "O01", "status": "PASS",
            "children": [local, network], "object_name": object_name,
            "source_sha256": digest(first),
            "replacement_sha256": digest(replacement),
            "local_report": local_report, "network_report": network_report,
            "repo_state_before": before, "repo_state_after": after_network,
            "restart_epochs": restart}


def run_spec193_object_range_commit(ndn, output: Path,
                                    processes: List[Dict[str, Any]],
                                    environments: Dict[str, Dict[str, str]],
                                    repo_client: Path,
                                    servers: List[Dict[str, Any]],
                                    trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O02 requires two independent object Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o02-range-source.bin"
    fetched = work / "o02-range-fetched.bin"
    local_path = work / "o02-local.json"
    network_path = work / "o02-network.json"
    source.write_bytes(bytes((index * 37 + 23) % 256 for index in range(521)))
    object_name = "/spec193/range-commit/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    if any(item["objectName"] == object_name for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O02 target object already exists")
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    local = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o02-local-range-commit",
        servers[0]["argv"] + [
            "--local-command", "range-commit", "--local-source", str(source),
            "--local-object", object_name, "--local-manifest", str(local_path)],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_O02_LOCAL_PASS size=521" not in read_log(Path(local["log"])):
        raise RuntimeError("O02 local noncontiguous range sequence failed")
    local_report = json.loads(local_path.read_text(encoding="utf-8"))
    if (local_report["stagedInvisible"] is not True or
            local_report["writeOrder"] != [[318, 203], [0, 101], [101, 217]] or
            local_report["manifest"]["generation"] != 1 or
            local_report["manifest"]["size"] != 521 or
            local_report["manifest"]["sha256"] != digest(source)):
        raise RuntimeError("O02 local range report contradicts source or offsets")
    after_local = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    entries = [item for item in after_local[REPO_A_ID]["repo_manifests"]["entries"]
               if item["objectName"] == object_name]
    if (len(entries) != 1 or entries[0]["size"] != 521 or
            entries[0]["sha256"] != digest(source) or
            before[REPO_B_ID] != after_local[REPO_B_ID] or
            not any(item["sha256"] == digest(source)
                    for item in after_local[REPO_A_ID]["payload_files"])):
        raise RuntimeError("O02 committed range absent from durable Repo A")
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o02-range", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    network = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o02-network-range-read",
        [str(repo_client), "--mode", "object-range-read",
         "--source", str(source), "--destination", str(fetched),
         "--report-out", str(network_path), "--object-name", object_name,
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--user", CONSUMER,
         "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if (digest(fetched) != digest(source) or
            "SPEC193_O02_NETWORK_PASS size=521" not in read_log(Path(network["log"]))):
        raise RuntimeError("O02 independent consumer did not read all ranges")
    network_report = json.loads(network_path.read_text(encoding="utf-8"))
    if network_report["manifest"] != local_report["manifest"]:
        raise RuntimeError("O02 network manifest differs from committed ranges")
    after_network = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    if after_network != after_local:
        raise RuntimeError("O02 restart or range reads changed durable state")
    return {"case_id": "O02", "status": "PASS",
            "children": [local, network], "object_name": object_name,
            "source_sha256": digest(source), "local_report": local_report,
            "network_report": network_report, "repo_state_before": before,
            "repo_state_after": after_network, "restart_epochs": restart}


def run_spec193_object_range_rejections(ndn, output: Path,
                                        processes: List[Dict[str, Any]],
                                        environments: Dict[str, Dict[str, str]],
                                        repo_client: Path,
                                        servers: List[Dict[str, Any]],
                                        trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O03 requires two independent object Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o03-range-source.bin"
    fetched = work / "o03-owned-fetched.bin"
    local_path = work / "o03-local.json"
    network_path = work / "o03-network.json"
    source.write_bytes(bytes((index * 53 + 13) % 256 for index in range(509)))
    base_name = "/spec193/range-rejections/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    local = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o03-local-range-rejections",
        servers[0]["argv"] + [
            "--local-command", "range-rejections", "--local-source", str(source),
            "--local-object", base_name, "--local-manifest", str(local_path)],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_O03_LOCAL_PASS size=509" not in read_log(Path(local["log"])):
        raise RuntimeError("O03 local range rejection matrix failed")
    local_report = json.loads(local_path.read_text(encoding="utf-8"))
    if (local_report["committed"]["objectName"] != base_name + "/owned" or
            local_report["committed"]["sha256"] != digest(source) or
            not all(local_report[key] for key in (
                "wrongOwnerRejected", "missingCommitRejected",
                "quotaRejectedWhileReserved", "abortReadmitted",
                "boundsRejected"))):
        raise RuntimeError("O03 local report contradicts rejection matrix")
    after_local = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    added = [item for item in after_local[REPO_A_ID]["repo_manifests"]["entries"]
             if item["objectName"].startswith(base_name)]
    staging = [item for item in after_local[REPO_A_ID]["payload_files"]
               if "/staging/" in item["path"]]
    if (len(added) != 1 or added[0]["objectName"] != base_name + "/owned" or
            added[0]["sha256"] != digest(source) or staging or
            before[REPO_B_ID] != after_local[REPO_B_ID]):
        raise RuntimeError("O03 rejected range left manifest, staging, or Repo B mutation")
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o03-rejections", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    network = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o03-network-audit",
        [str(repo_client), "--mode", "object-range-rejection-audit",
         "--source", str(source), "--destination", str(fetched),
         "--report-out", str(network_path), "--object-name", base_name,
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--user", CONSUMER,
         "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if (digest(fetched) != digest(source) or
            "SPEC193_O03_NETWORK_PASS rejected=3" not in
            read_log(Path(network["log"]))):
        raise RuntimeError("O03 independent consumer did not confirm rejection")
    network_report = json.loads(network_path.read_text(encoding="utf-8"))
    if network_report["committed"] != local_report["committed"]:
        raise RuntimeError("O03 network committed manifest differs from local")
    after_network = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    if after_network != after_local:
        raise RuntimeError("O03 restart or rejection queries changed durable state")
    return {"case_id": "O03", "status": "PASS",
            "children": [local, network], "base_name": base_name,
            "source_sha256": digest(source), "local_report": local_report,
            "network_report": network_report, "repo_state_before": before,
            "repo_state_after": after_network, "restart_epochs": restart}


def run_spec193_object_remove(ndn, output: Path,
                              processes: List[Dict[str, Any]],
                              environments: Dict[str, Dict[str, str]],
                              repo_client: Path,
                              servers: List[Dict[str, Any]],
                              trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O04 requires two independent object Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o04-remove-source.bin"
    local_path = work / "o04-local.json"
    network_path = work / "o04-network.json"
    source.write_bytes(bytes((index * 59 + 5) % 256 for index in range(419)))
    object_name = "/spec193/remove/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    if any(item["objectName"] == object_name for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O04 target object already exists")
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    local = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o04-local-remove",
        servers[0]["argv"] + [
            "--local-command", "object-remove", "--local-source", str(source),
            "--local-object", object_name, "--local-manifest", str(local_path)],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_O04_LOCAL_PASS" not in read_log(Path(local["log"])):
        raise RuntimeError("O04 local removal sequence failed")
    local_report = json.loads(local_path.read_text(encoding="utf-8"))
    if (local_report["stored"]["objectName"] != object_name or
            local_report["stored"]["sha256"] != digest(source) or
            local_report["afterPut"]["catalogEpoch"] !=
            local_report["before"]["catalogEpoch"] + 1 or
            local_report["afterDelete"]["catalogEpoch"] !=
            local_report["afterPut"]["catalogEpoch"] + 1 or
            local_report["delta"]["entries"][0]["state"] != "DELETED" or
            local_report["missingRemove"] is not False or
            local_report["firstRemove"] is not True or
            local_report["repeatRemove"] is not False):
        raise RuntimeError("O04 local catalog or idempotency report invalid")
    after_local = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    if (after_local[REPO_A_ID]["repo_manifests"] !=
            before[REPO_A_ID]["repo_manifests"] or
            any(item["sha256"] == digest(source) for item in
                after_local[REPO_A_ID]["payload_files"]) or
            before[REPO_B_ID] != after_local[REPO_B_ID]):
        raise RuntimeError("O04 removal left manifest or payload or changed Repo B")
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o04-removed", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    network = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o04-network-removed",
        [str(repo_client), "--mode", "object-remove-audit",
         "--source", str(source), "--report-out", str(network_path),
         "--since-epoch", str(local_report["afterPut"]["catalogEpoch"]),
         "--object-name", object_name,
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--user", CONSUMER,
         "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if "SPEC193_O04_NETWORK_PASS rejected=3" not in read_log(Path(network["log"])):
        raise RuntimeError("O04 independent consumer could read removed object")
    network_report = json.loads(network_path.read_text(encoding="utf-8"))
    if (network_report["sourceSha256"] != digest(source) or
            not network_report["status"]["reconciliationRequired"] or
            network_report["rejectedOperations"] != 3):
        raise RuntimeError("O04 network report contradicts source or reconciliation")
    after_network = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    if after_network != after_local:
        raise RuntimeError("O04 restart or rejection queries changed durable state")
    return {"case_id": "O04", "status": "PASS",
            "children": [local, network], "object_name": object_name,
            "source_sha256": digest(source), "local_report": local_report,
            "network_report": network_report, "repo_state_before": before,
            "repo_state_after": after_network, "restart_epochs": restart}


def run_spec193_object_corruption(ndn, output: Path,
                                  processes: List[Dict[str, Any]],
                                  environments: Dict[str, Dict[str, str]],
                                  repo_client: Path,
                                  servers: List[Dict[str, Any]],
                                  trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O05 requires two independent object Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o05-source.bin"
    destination = work / "o05-destination.bin"
    local_path = work / "o05-local.json"
    source.write_bytes(bytes((index * 61 + 9) % 256 for index in range(487)))
    destination.write_bytes(b"O05 existing destination must survive failed reads")
    sentinel_sha = digest(destination)
    object_name = "/spec193/corruption/" + output.name
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    local = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o05-local-seed",
        servers[0]["argv"] + [
            "--local-command", "corruption-seed", "--local-source", str(source),
            "--local-object", object_name, "--local-manifest", str(local_path)],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_O05_LOCAL_SEED_PASS size=487" not in read_log(Path(local["log"])):
        raise RuntimeError("O05 local missing-object and seed sequence failed")
    local_report = json.loads(local_path.read_text(encoding="utf-8"))
    if (local_report["manifest"]["objectName"] != object_name or
            local_report["manifest"]["sha256"] != digest(source) or
            not local_report["missingGetRejected"] or
            not local_report["missingManifestRejected"]):
        raise RuntimeError("O05 local seed report differs from source")
    after_seed = {item["repo_node"]: repo_database_state(item["repo_dir"])
                  for item in servers}
    payloads = [item for item in after_seed[REPO_A_ID]["payload_files"]
                if "/payloads/" in item["path"] and
                item["sha256"] == digest(source)]
    if (len(payloads) != 1 or
            len([item for item in after_seed[REPO_A_ID]["repo_manifests"]["entries"]
                 if item["objectName"] == object_name and
                 item["sha256"] == digest(source)]) != 1 or
            before[REPO_B_ID] != after_seed[REPO_B_ID]):
        raise RuntimeError("O05 cannot isolate one committed payload file")
    payload_path = servers[0]["repo_dir"] / payloads[0]["path"]
    original_bytes = payload_path.read_bytes()
    if hashlib.sha256(original_bytes).hexdigest() != digest(source):
        raise RuntimeError("O05 physical payload differs before fault")
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o05-corrupt", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    base = ["--source", str(source), "--destination", str(destination),
            "--object-name", object_name, "--group", GROUP,
            "--controller", CONTROLLER, "--authority", CONTROLLER,
            "--user", CONSUMER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    altered = bytearray(original_bytes)
    altered[0] ^= 0x01
    payload_path.write_bytes(altered)
    try:
        corrupted_state = {item["repo_node"]: repo_database_state(item["repo_dir"])
                           for item in servers}
        if (corrupted_state[REPO_A_ID]["repo_manifests"] !=
                after_seed[REPO_A_ID]["repo_manifests"] or
                before[REPO_B_ID] != corrupted_state[REPO_B_ID] or
                not any(item["path"] == payloads[0]["path"] and
                        item["sha256"] != digest(source)
                        for item in corrupted_state[REPO_A_ID]["payload_files"])):
            raise RuntimeError("O05 physical corruption was not isolated")
        corrupt_path = work / "o05-network-corrupt.json"
        corrupt = run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o05-network-corrupt",
            [str(repo_client), "--mode", "object-corruption-audit",
             "--expect", "corrupt", "--report-out", str(corrupt_path)] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            300, require_no_python=True)
        if ("SPEC193_O05_NETWORK_PASS phase=corrupt" not in
                read_log(Path(corrupt["log"])) or digest(destination) != sentinel_sha):
            raise RuntimeError("O05 corrupt read changed existing destination")
        corrupt_report = json.loads(corrupt_path.read_text(encoding="utf-8"))
    finally:
        payload_path.write_bytes(original_bytes)
    if digest(payload_path) != digest(source):
        raise RuntimeError("O05 physical payload restoration failed")
    restored_path = work / "o05-network-restored.json"
    restored = run_child(
        ndn, output, processes, CONSUMER_HOST,
        "spec193-o05-network-restored",
        [str(repo_client), "--mode", "object-corruption-audit",
         "--expect", "restored", "--report-out", str(restored_path)] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if ("SPEC193_O05_NETWORK_PASS phase=restored" not in
            read_log(Path(restored["log"])) or digest(destination) != digest(source)):
        raise RuntimeError("O05 restored object did not fetch original bytes")
    restored_report = json.loads(restored_path.read_text(encoding="utf-8"))
    after_restored = {item["repo_node"]: repo_database_state(item["repo_dir"])
                      for item in servers}
    if after_restored != after_seed:
        raise RuntimeError("O05 restored Repo state differs from original")
    return {"case_id": "O05", "status": "PASS",
            "children": [local, corrupt, restored], "object_name": object_name,
            "source_sha256": digest(source), "sentinel_sha256": sentinel_sha,
            "local_report": local_report, "corrupt_report": corrupt_report,
            "restored_report": restored_report, "repo_state_before": before,
            "repo_state_corrupt": corrupted_state,
            "repo_state_after": after_restored, "restart_epochs": restart}


def run_spec193_segmented_object(ndn, output: Path,
                                 processes: List[Dict[str, Any]],
                                 environments: Dict[str, Dict[str, str]],
                                 repo_client: Path,
                                 servers: List[Dict[str, Any]],
                                 trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O06/O07 requires two independent object Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o06-segmented-source.bin"
    source.write_bytes(bytes((index * 37 + 11) % 256 for index in range(2048)))
    source_sha = digest(source)
    object_name = "/spec193/segmented/" + output.name
    if any(item["objectName"].startswith(object_name) for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O06/O07 target object already exists before segmentation")
    manifest_path = work / "o06-parent-manifest.json"
    segment_bytes = 128
    children = []
    restarts = []
    ready_text = "Spec193 object Repo serving"

    def run_local(command: str) -> None:
        if servers[0]["proc"].poll() is None:
            stop_repo_server(servers[0])
        argv = servers[0]["argv"] + [
            "--local-command", command, "--local-source", str(source),
            "--local-object", object_name, "--local-manifest", str(manifest_path),
            "--local-segment-bytes", str(segment_bytes)]
        label = "spec193-o06-o07-local-" + command
        child = run_child(ndn, output, processes, REPO_A_HOST, label, argv,
                          node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
                          90, require_no_python=True)
        children.append(child)
        target = [servers[0]]
        restarts.append(restart_repo_servers(
            ndn, output, processes, environments, target,
            "o06-o07-" + command, ready_text=ready_text))
        servers[0] = target[0]
        time.sleep(2)

    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    destination = work / "o06-o07-consumer-result.bin"

    def fetch(label: str, expectation: str) -> None:
        if expectation != "present" and not destination.exists():
            destination.write_bytes(b"O07 failed fetch must preserve this destination\x00v1")
        original = destination.read_bytes() if destination.exists() else None
        child = run_child(
            ndn, output, processes, CONSUMER_HOST, label,
            [str(repo_client), "--mode", "object-segmented-fetch",
             "--source", str(source), "--destination", str(destination),
             "--object-name", object_name, "--expect", expectation,
             "--user", CONSUMER] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            600, require_no_python=True)
        children.append(child)
        if expectation == "present" and digest(destination) != source_sha:
            raise RuntimeError("O06/O07 consumer reassembly differs from source")
        if expectation != "present" and destination.read_bytes() != original:
            raise RuntimeError("O07 rejected fetch changed existing destination")

    run_local("seed")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected_count = (source.stat().st_size + segment_bytes - 1) // segment_bytes
    if (manifest.get("segmentCount") != expected_count or
            manifest.get("sha256") != source_sha or
            manifest.get("size") != source.stat().st_size):
        raise RuntimeError("O06 seed lacks actual segmented parent manifest")
    after_seed = {item["repo_node"]: repo_database_state(item["repo_dir"])
                  for item in servers}
    segment_names = {object_name + "/seg/" + str(index)
                     for index in range(expected_count)}
    stored_names = {item["objectName"] for item in
                    after_seed[REPO_A_ID]["repo_manifests"]["entries"]}
    if not segment_names.issubset(stored_names) or object_name not in stored_names or \
            before[REPO_B_ID] != after_seed[REPO_B_ID]:
        raise RuntimeError("O06 durable segment inventory or unselected Repo changed")
    fetch("spec193-o06-consumer-reassemble", "present")
    run_local("duplicate")
    fetch("spec193-o07-identical-duplicate-still-readable", "present")
    run_local("missing")
    after_missing = repo_database_state(servers[0]["repo_dir"])
    missing_names = {item["objectName"] for item in
                     after_missing["repo_manifests"]["entries"]}
    if object_name + "/seg/1" in missing_names or \
            not (segment_names - {object_name + "/seg/1"}).issubset(missing_names):
        raise RuntimeError("O07 fault did not remove exactly one stored segment")
    fetch("spec193-o07-missing-segment-rejected", "missing")
    run_local("corrupt")
    fetch("spec193-o07-corrupt-segment-rejected", "corrupt")
    run_local("restore")
    fetch("spec193-o07-restored-consumer-reassemble", "present")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if before[REPO_B_ID] != after[REPO_B_ID]:
        raise RuntimeError("O06/O07 changed the unselected Repo")
    return {"case_id": "O06_O07", "status": "PASS", "children": children,
            "object_name": object_name, "source_sha256": source_sha,
            "segment_bytes": segment_bytes, "segment_count": expected_count,
            "manifest": manifest, "repo_state_before": before,
            "repo_state_after_seed": after_seed,
            "repo_state_after_missing": after_missing,
            "repo_state_after_restore": after,
            "restart_epochs": restarts,
            "fetched_sha256": digest(destination)}


def run_spec193_data_packet(ndn, output: Path,
                           processes: List[Dict[str, Any]],
                           environments: Dict[str, Dict[str, str]],
                           repo_client: Path,
                           servers: List[Dict[str, Any]],
                           trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O08 requires two independent object Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o08-data-content.bin"
    source.write_bytes(bytes((index * 53 + 19) % 256 for index in range(513)))
    wire = work / "o08-original-signed-data.wire"
    certificate = work / "o08-provider-signing-certificate.wire"
    destination = work / "o08-consumer-signed-data.wire"
    data_name = "/spec193/signed-data/" + output.name
    if any(item["objectName"] == data_name for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O08 target Data name already exists")
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    seed = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o08-local-data-seed",
        servers[0]["argv"] + [
            "--local-command", "data-seed", "--local-source", str(source),
            "--local-object", data_name, "--local-manifest", str(wire),
            "--local-certificate", str(certificate)],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o08-data-seed", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    time.sleep(2)
    after_seed = {item["repo_node"]: repo_database_state(item["repo_dir"])
                  for item in servers}
    stored = [item for item in after_seed[REPO_A_ID]["repo_manifests"]["entries"]
              if item["objectName"] == data_name]
    if (len(stored) != 1 or stored[0]["objectType"] != "ndn-data-wire" or
            stored[0]["sha256"] != digest(wire) or
            stored[0]["size"] != wire.stat().st_size or
            before[REPO_B_ID] != after_seed[REPO_B_ID]):
        raise RuntimeError("O08 durable Data wire or unselected Repo state invalid")
    child = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o08-consumer-data-fetch",
        [str(repo_client), "--mode", "object-data-fetch",
         "--source", str(source), "--wire-source", str(wire),
         "--certificate", str(certificate), "--destination", str(destination),
         "--object-name", data_name, "--user", CONSUMER,
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        180, require_no_python=True)
    if digest(destination) != digest(wire) or \
            "SPEC193_DATA_PACKET_NETWORK_PASS" not in read_log(Path(child["log"])):
        raise RuntimeError("O08 consumer did not preserve verified signed Data wire")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if after != after_seed:
        raise RuntimeError("O08 read changed durable Repo storage")
    return {"case_id": "O08", "status": "PASS", "children": [seed, child],
            "data_name": data_name, "source_sha256": digest(source),
            "wire_sha256": digest(wire), "fetched_sha256": digest(destination),
            "repo_state_before": before, "repo_state_after_seed": after_seed,
            "repo_state_after_fetch": after, "restart_epochs": restart}


def run_spec193_original_data_route(ndn, output: Path,
                                    processes: List[Dict[str, Any]],
                                    environments: Dict[str, Dict[str, str]],
                                    repo_client: Path,
                                    servers: List[Dict[str, Any]],
                                    trust_schema: Path,
                                    failover: bool = False,
                                    corrupt_location: bool = False,
                                    drop_original_data: bool = False,
                                    import_legacy: bool = False,
                                    cache_hit: bool = False,
                                    server_auth: bool = False) -> Dict[str, Any]:
    if sum((failover, corrupt_location, drop_original_data, import_legacy,
            cache_hit)) > 1:
        raise ValueError("O17 fault modes are mutually exclusive")
    if len(servers) != 2:
        raise RuntimeError("O17 route probe requires two independent object Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o17-data-content.bin"
    source.write_bytes(bytes((index * 59 + 23) % 256 for index in range(513)))
    wire = work / "o17-original-signed-data.wire"
    certificate = work / "o17-producer-certificate.wire"
    destination = work / "o17-consumer-original-data.wire"
    data_name = "/spec193/original-data/" + output.name
    for server in servers:
        if server["proc"].poll() is None:
            stop_repo_server(server)
    seed_a = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o17-original-seed-a",
        servers[0]["argv"] + [
            "--local-command", ("data-legacy-seed" if import_legacy else
                                "data-cache-seed" if cache_hit else "data-seed"),
            "--local-source", str(source),
            "--local-object", data_name, "--local-manifest", str(wire),
            "--local-certificate", str(certificate)],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    seed_b = run_child(
        ndn, output, processes, REPO_B_HOST, "spec193-o17-original-seed-b",
        servers[1]["argv"] + [
            "--local-command", "data-wire-seed", "--local-source", str(wire),
            "--local-object", data_name],
        node_environment(ndn.net[REPO_B_HOST], REPO, no_python=True),
        90, require_no_python=True)
    target = list(servers)
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o17-original-seed", ready_text="Spec193 object Repo serving")
    servers[:] = target
    time.sleep(2)
    preimport_audit = None
    import_child = None
    state_before_import = None
    import_restart = None
    if import_legacy:
        state_before_import = {item["repo_node"]: repo_database_state(item["repo_dir"])
                               for item in servers}
        entries = [entry for entry in state_before_import[REPO_A_ID][
            "repo_manifests"]["entries"]
                   if entry["objectName"] == data_name]
        legacy_manifest = (json.loads((servers[0]["repo_dir"] /
                                       entries[0]["path"]).read_text())
                           if len(entries) == 1 else {})
        if len(entries) != 1 or legacy_manifest.get("dataReceiptId"):
            raise RuntimeError("O17 legacy seed unexpectedly has a receipt")
        preimport_audit = run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o17-preimport-audit",
            [str(repo_client), "--mode", "object-original-data-preimport-audit",
             "--wire-source", str(wire), "--object-name", data_name,
             "--user", CONSUMER, "--group", GROUP,
             "--controller", CONTROLLER, "--authority", CONTROLLER,
             "--trust-schema", str(trust_schema),
             "--service-prefix", SERVICE_PREFIX,
             "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER],
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            90, require_no_python=True)
        if read_log(Path(preimport_audit["log"])).count(
                "SPEC193_LEGACY_DATA_HIDDEN") != 2:
            raise RuntimeError("O17 legacy Data was not isolated before import")
        stop_repo_server(servers[0])
        import_child = run_child(
            ndn, output, processes, REPO_A_HOST,
            "spec193-o17-legacy-import",
            servers[0]["argv"] + [
                "--local-command", "data-legacy-import",
                "--local-source", str(wire), "--local-object", data_name],
            node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
            90, require_no_python=True)
        if "SPEC193_LEGACY_DATA_IMPORTED" not in read_log(Path(import_child["log"])):
            raise RuntimeError("O17 legacy Data import did not complete")
        target = list(servers)
        import_restart = restart_repo_servers(
            ndn, output, processes, environments, target,
            "o17-legacy-import", ready_text="Spec193 object Repo serving")
        servers[:] = target
        time.sleep(2)
    face_reports = [run_child(
        ndn, output, processes, host,
        "spec193-o17-faces-" + host,
        ["/usr/local/bin/nfdc", "face", "list"],
        node_environment(ndn.net[host], REPO),
        30) for host in (CONSUMER_HOST, REPO_A_HOST)]
    route_reports = [run_child(
        ndn, output, processes, host,
        "spec193-o17-routes-" + host,
        ["/usr/local/bin/nfdc", "route", "list"],
        node_environment(ndn.net[host], REPO),
        30) for host in (CONSUMER_HOST, REPO_A_HOST)]
    after_seed = {item["repo_node"]: repo_database_state(item["repo_dir"])
                  for item in servers}
    def original_payloads(state: Dict[str, Any]) -> List[Dict[str, Any]]:
        return [item for item in state["payload_files"]
                if "/payloads/sha256/" in "/" + item["path"]]
    if import_legacy and (
            not original_payloads(after_seed[REPO_A_ID]) or
            original_payloads(after_seed[REPO_A_ID]) !=
            original_payloads(state_before_import[REPO_A_ID]) or
            after_seed[REPO_B_ID] != state_before_import[REPO_B_ID]):
        raise RuntimeError("O17 import changed original payload bytes")
    for repo_node in (REPO_A_ID, REPO_B_ID):
        stored = [item for item in after_seed[repo_node]["repo_manifests"]["entries"]
                  if item["objectName"] == data_name]
        if (len(stored) != 1 or stored[0]["objectType"] != "ndn-data-wire" or
                stored[0]["sha256"] != digest(wire) or
                stored[0]["size"] != wire.stat().st_size):
            raise RuntimeError("O17 Repo replica lacks original signed Data wire")
    cleared_cache = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o17-consumer-cache-erase",
        ["/usr/local/bin/nfdc", "cs", "erase", data_name],
        node_environment(ndn.net[CONSUMER_HOST], REPO), 30)
    after_corruption = None
    corrupt_audit = None
    if corrupt_location:
        candidate = [item for item in after_seed[REPO_A_ID]["payload_files"]
                     if item["path"].endswith("/payloads/sha256/" +
                                              digest(wire)[:2] + "/" + digest(wire))]
        if len(candidate) != 1:
            raise RuntimeError("O17 cannot isolate Repo A original payload")
        payload_path = servers[0]["repo_dir"] / candidate[0]["path"]
        if not payload_path.is_file():
            raise RuntimeError("O17 Repo A original payload disappeared before fault")
        payload_path.unlink()
        after_corruption = {item["repo_node"]: repo_database_state(item["repo_dir"])
                            for item in servers}
        if after_corruption[REPO_B_ID] != after_seed[REPO_B_ID] or \
                after_corruption[REPO_A_ID] == after_seed[REPO_A_ID]:
            raise RuntimeError("O17 corruption fault did not isolate Repo A")
        corrupt_audit = run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o17-corrupt-location-audit",
            [str(repo_client), "--mode", "object-original-data-location-audit",
             "--wire-source", str(wire), "--object-name", data_name,
             "--user", CONSUMER, "--group", GROUP,
             "--controller", CONTROLLER, "--authority", CONTROLLER,
             "--trust-schema", str(trust_schema),
             "--service-prefix", SERVICE_PREFIX,
             "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
             "--expected-state", "UNKNOWN"],
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            90, require_no_python=True)
        if "SPEC193_ORIGINAL_DATA_UNKNOWN_LOCATION_PASS" not in \
                read_log(Path(corrupt_audit["log"])):
            raise RuntimeError("O17 corrupt Repo A did not report UNKNOWN")
    denied_audit = None
    if not failover:
        denied_before = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
        denied_audit = run_child(
            ndn, output, processes, APP_HOST,
            "spec193-o17-unauthorized-query-audit",
            [str(repo_client), "--mode", "object-original-data-denied-audit",
             "--wire-source", str(wire), "--object-name", data_name,
             "--user", USER,
             "--group", GROUP, "--controller", CONTROLLER,
             "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
             "--service-prefix", SERVICE_PREFIX,
             "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER],
            node_environment(ndn.net[APP_HOST], REPO, no_python=True),
            90, require_no_python=True)
        denial_log = read_log(Path(denied_audit["log"]))
        if ("SPEC193_ORIGINAL_DATA_DENIAL_BOOTSTRAP_READY" not in denial_log or
                denial_log.count("SPEC193_ORIGINAL_DATA_QUERY_DENIED") != 2 or
                repo_request_events(processes, {REPO_A_ID, REPO_B_ID}) != denied_before):
            raise RuntimeError("O17 unauthorized receipt/location reached a Repo handler")
    server_auth_audit = None
    server_auth_evidence = []
    if server_auth:
        auth_logs_before = {item["repo_node"]: read_log(item["log"])
                            for item in servers}
        auth_requests_before = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
        auth_state_before = {item["repo_node"]: repo_database_state(item["repo_dir"])
                             for item in servers}
        server_auth_audit = run_child(
            ndn, output, processes, APP_HOST, "spec193-o17-server-auth-audit",
            [str(repo_client), "--mode", "object-original-data-server-auth-audit",
             "--wire-source", str(wire), "--object-name", data_name,
             "--user", USER, "--group", GROUP, "--controller", CONTROLLER,
             "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
             "--service-prefix", SERVICE_PREFIX,
             "--provider-a", REPO_A_PROVIDER],
            node_environment(ndn.net[APP_HOST], REPO, no_python=True),
            90, require_no_python=True)
        auth_client_log = read_log(Path(server_auth_audit["log"]))
        if auth_client_log.count("SPEC193_SERVER_AUTH_REQUEST_PUBLISHED") != 2:
            raise RuntimeError("O17 server authorization probe did not publish both queries")
        for server in servers:
            new_log = read_log(server["log"])[len(auth_logs_before[server["repo_node"]]):]
            for operation in ("DATA_RECEIPT", "DATA_LOCATION"):
                request_id = "/spec193-server-auth-" + hashlib.sha256(
                    (data_name + operation).encode("utf-8")).hexdigest()[:32]
                matched = [line for line in new_log.splitlines()
                           if "NDNSF_PUBLICATION_AUDIT role=provider type=REQUEST" in line
                           and "validated=true" in line and "packetPresent=true" in line
                           and " requestId=" + request_id + " " in line
                           and " serviceName=" + SERVICE_PREFIX + "/" + operation + " " in line
                           and " requesterName=" + USER + " " in line]
                if not matched:
                    raise RuntimeError("O17 server did not validate unauthorized " + operation)
                server_auth_evidence.append({"repo_node": server["repo_node"],
                                             "operation": operation, "request_id": request_id,
                                             "validated_publications": matched})
            if new_log.count("OnRequestDecryptionSuccessCallbackV2: Missing UserToken") != 2:
                raise RuntimeError("O17 server did not reject both decoded tokenless queries")
            if repo_database_state(server["repo_dir"]) != auth_state_before[server["repo_node"]]:
                raise RuntimeError("O17 denied raw queries changed Repo storage")
        if repo_request_events(processes, {REPO_A_ID, REPO_B_ID}) != auth_requests_before:
            raise RuntimeError("O17 denied raw queries reached a Repo handler")
    stopped_a = stop_repo_server(servers[0]) if failover else None
    dropped_a = None
    if drop_original_data:
        dropped_a = run_child(
            ndn, output, processes, REPO_A_HOST,
            "spec193-o17-drop-a-original-data",
            ["/usr/sbin/iptables", "-I", "OUTPUT", "-m", "string",
             "--string", "original-data", "--algo", "bm", "-j", "DROP"],
            node_environment(ndn.net[REPO_A_HOST], REPO), 30)
    requests_before = len(repo_request_events(processes, {REPO_A_ID, REPO_B_ID}))
    child = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o17-original-fetch",
        [str(repo_client), "--mode", "object-original-data-fetch",
         "--source", str(source), "--wire-source", str(wire),
         "--certificate", str(certificate), "--destination", str(destination),
         "--object-name", data_name, "--user", CONSUMER,
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER,
         "--duplicate-provider", "yes"],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        180, require_no_python=True)
    if digest(destination) != digest(wire) or \
            "SPEC193_ORIGINAL_DATA_PASS" not in read_log(Path(child["log"])):
        raise RuntimeError("O17 route probe did not preserve original signed Data")
    authenticated = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})[
        requests_before:]
    selected_node = REPO_B_ID if failover or corrupt_location or drop_original_data else REPO_A_ID
    actual_queries = [(item["repo_node"], item["operation"]) for item in authenticated
                      if item["operation"] in ("DATA_RECEIPT", "DATA_LOCATION")]
    expected_queries = [(selected_node, "DATA_LOCATION"),
                        (selected_node, "DATA_RECEIPT")]
    if corrupt_location:
        expected_queries.extend([(REPO_A_ID, "DATA_LOCATION"),
                                 (REPO_A_ID, "DATA_RECEIPT")])
    if drop_original_data:
        expected_queries.extend([(REPO_A_ID, "DATA_RECEIPT"),
                                 (REPO_A_ID, "DATA_LOCATION"),
                                 (REPO_A_ID, "DATA_LOCATION")])
    if sorted(actual_queries) != sorted(expected_queries):
        raise RuntimeError("O17 duplicate Provider caused extra or missing receipt/location query")
    cache_children = []
    cache_queries = None
    if cache_hit:
        cache_children.append(run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o17-cache-info-before",
            ["/usr/local/bin/nfdc", "cs", "info"],
            node_environment(ndn.net[CONSUMER_HOST], REPO), 30))
        second_destination = work / "o17-consumer-cache-hit.wire"
        second_requests_before = len(repo_request_events(
            processes, {REPO_A_ID, REPO_B_ID}))
        cache_children.append(run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o17-cache-fetch",
            [str(repo_client), "--mode", "object-original-data-fetch",
             "--source", str(source), "--wire-source", str(wire),
             "--certificate", str(certificate),
             "--destination", str(second_destination),
             "--object-name", data_name, "--user", CONSUMER,
             "--group", GROUP, "--controller", CONTROLLER,
             "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
             "--service-prefix", SERVICE_PREFIX,
             "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER],
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            180, require_no_python=True))
        cache_children.append(run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o17-cache-info-after",
            ["/usr/local/bin/nfdc", "cs", "info"],
            node_environment(ndn.net[CONSUMER_HOST], REPO), 30))
        cache_queries = repo_request_events(
            processes, {REPO_A_ID, REPO_B_ID})[second_requests_before:]
        expected_cache_queries = [(REPO_A_ID, "DATA_RECEIPT"),
                                  (REPO_A_ID, "DATA_LOCATION")]
        actual_cache_queries = [(item["repo_node"], item["operation"])
                                for item in cache_queries
                                if item["operation"] in
                                ("DATA_RECEIPT", "DATA_LOCATION")]
        if (digest(second_destination) != digest(wire) or
                "SPEC193_ORIGINAL_DATA_PASS" not in read_log(
                    Path(cache_children[1]["log"])) or
                sorted(actual_cache_queries) != sorted(expected_cache_queries)):
            raise RuntimeError("O17 cached fetch changed wire or skipped metadata authority")
    negative_destination = work / "o17-no-candidate-output.wire"
    negative = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o17-no-candidate",
        [str(repo_client), "--mode", "object-original-data-fetch",
         "--source", str(source), "--wire-source", str(wire),
         "--certificate", str(certificate), "--destination", str(negative_destination),
         "--object-name", data_name, "--user", CONSUMER,
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER,
         "--no-candidates", "yes"],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        60, require_no_python=True)
    if negative_destination.exists() or \
            "SPEC193_ORIGINAL_DATA_NO_CANDIDATE_REJECTED" not in read_log(Path(negative["log"])) or \
            repo_request_events(processes, {REPO_A_ID, REPO_B_ID})[requests_before:] != \
            authenticated + (cache_queries or []):
        raise RuntimeError("O17 empty candidate set emitted a request or destination")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if after != (after_corruption if corrupt_location else after_seed):
        raise RuntimeError("O17 route probe changed durable Repo storage")
    location_audit = None
    after_delete = None
    if not failover and not corrupt_location and not drop_original_data:
        location_audit = run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o17-location-delete-audit",
            [str(repo_client), "--mode", "object-original-data-location-audit",
             "--wire-source", str(wire), "--object-name", data_name,
             "--user", CONSUMER, "--group", GROUP,
             "--controller", CONTROLLER, "--authority", CONTROLLER,
             "--trust-schema", str(trust_schema),
             "--service-prefix", SERVICE_PREFIX,
             "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER],
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            120, require_no_python=True)
        after_delete = repo_database_state(servers[0]["repo_dir"])
        if any(item["objectName"] == data_name for item in
               after_delete["repo_manifests"]["entries"]):
            raise RuntimeError("O17 deleted Data remained in current manifest")
        if repo_database_state(servers[1]["repo_dir"]) != after_seed[REPO_B_ID]:
            raise RuntimeError("O17 deletion changed the untouched Repo B")
    # Keep captures alive until the NFD and tcpdump consumers drain the final
    # original Data exchange; the oracle reads the closed pcap after this.
    time.sleep(2)
    case_id = "O17_CACHE" if cache_hit else (
        "O17_IMPORT" if import_legacy else (
        "O17_DROP" if drop_original_data else (
        "O17_CORRUPT" if corrupt_location else (
            "O17_FAILOVER" if failover else "O17_ROUTE"))))
    return {"case_id": case_id, "status": "PASS",
            "children": [seed_a, seed_b, *face_reports, *route_reports,
                         cleared_cache, child, negative, *cache_children] +
                        ([dropped_a] if dropped_a else []) +
                        ([preimport_audit, import_child] if import_legacy else []) +
                        ([corrupt_audit] if corrupt_audit else []) +
                        ([denied_audit] if denied_audit else []) +
                        ([server_auth_audit] if server_auth_audit else []) +
                        ([location_audit] if location_audit else []),
            "server_auth_diagnostic": server_auth_evidence,
            "authenticated_original_queries": authenticated,
            "cache_authenticated_queries": cache_queries,
            "repo_state_after_corruption": after_corruption,
            "repo_a_state_after_delete": after_delete,
            "stopped_repo_a": stopped_a,
            "data_name": data_name,
            "wire_sha256": digest(wire), "fetched_sha256": digest(destination),
            "repo_state_before": before, "repo_state_after_seed": after_seed,
            "repo_state_after_fetch": after, "restart_epochs": restart,
            "repo_state_before_import": state_before_import,
            "import_restart_epochs": import_restart,
            "active_repo_logs": {item["host"]: str(item["log"]) for item in servers}}


def run_spec193_packet_set(ndn, output: Path,
                          processes: List[Dict[str, Any]],
                          environments: Dict[str, Dict[str, str]],
                          repo_client: Path,
                          servers: List[Dict[str, Any]],
                          trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O09 requires two independent object Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o09-packet-set-content.bin"
    source.write_bytes(bytes((index * 67 + 23) % 256 for index in range(513)))
    certificate = work / "o09-provider-signing-certificate.wire"
    manifest_path = work / "o09-parent-manifest.json"
    ordered_wire = work / "o09-expected-ordered-wires.bin"
    destination = work / "o09-consumer-ordered-wires.bin"
    object_name = "/spec193/packet-set/" + output.name
    if any(item["objectName"].startswith(object_name) for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O09 target packet namespace already exists")
    children = []
    restarts = []

    def run_local(command: str) -> None:
        if servers[0]["proc"].poll() is None:
            stop_repo_server(servers[0])
        child = run_child(
            ndn, output, processes, REPO_A_HOST,
            "spec193-o09-local-" + command,
            servers[0]["argv"] + [
                "--local-command", "packet-set-" + command,
                "--local-source", str(source), "--local-object", object_name,
                "--local-manifest", str(manifest_path),
                "--local-certificate", str(certificate)],
            node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
            90, require_no_python=True)
        children.append(child)
        target = [servers[0]]
        restarts.append(restart_repo_servers(
            ndn, output, processes, environments, target,
            "o09-" + command, ready_text="Spec193 object Repo serving"))
        servers[0] = target[0]
        time.sleep(2)

    base = ["--source", str(source), "--wire-dir", str(work),
            "--certificate", str(certificate), "--destination", str(destination),
            "--object-name", object_name, "--user", CONSUMER,
            "--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]

    def fetch(label: str, expectation: str) -> None:
        previous = destination.read_bytes() if destination.exists() else None
        child = run_child(
            ndn, output, processes, CONSUMER_HOST, label,
            [str(repo_client), "--mode", "object-packet-set-fetch",
             "--expect", expectation] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            240, require_no_python=True)
        children.append(child)
        if expectation == "present" and digest(destination) != digest(ordered_wire):
            raise RuntimeError("O09 consumer changed manifest-ordered packet wire")
        if expectation == "missing" and \
                (not destination.exists() or destination.read_bytes() != previous):
            raise RuntimeError("O09 missing packet returned or wrote partial result")

    run_local("seed")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    order = (2, 0, 1)
    names = [object_name + "/pkt/" + str(index) for index in order]
    ordered_wire.write_bytes(b"".join(
        (work / ("o09-packet-" + str(index) + ".wire")).read_bytes()
        for index in order))
    if (manifest.get("objectName") != object_name or
            manifest.get("packetNames") != names or
            manifest.get("segmentCount") != 3 or
            manifest.get("size") != ordered_wire.stat().st_size or
            manifest.get("sha256") != digest(ordered_wire)):
        raise RuntimeError("O09 parent manifest lost application packet order")
    after_seed = {item["repo_node"]: repo_database_state(item["repo_dir"])
                  for item in servers}
    stored = {item["objectName"] for item in
              after_seed[REPO_A_ID]["repo_manifests"]["entries"]}
    if not set(names + [object_name]).issubset(stored) or \
            before[REPO_B_ID] != after_seed[REPO_B_ID]:
        raise RuntimeError("O09 durable packet inventory or unselected Repo changed")
    fetch("spec193-o09-consumer-ordered-fetch", "present")
    run_local("missing")
    after_missing = repo_database_state(servers[0]["repo_dir"])
    missing_names = {item["objectName"] for item in
                     after_missing["repo_manifests"]["entries"]}
    if object_name + "/pkt/1" in missing_names or \
            not set(names[:2] + [object_name]).issubset(missing_names):
        raise RuntimeError("O09 fault did not remove only final ordered packet")
    fetch("spec193-o09-missing-index-rejected", "missing")
    run_local("restore")
    fetch("spec193-o09-restored-ordered-fetch", "present")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if before[REPO_B_ID] != after[REPO_B_ID]:
        raise RuntimeError("O09 changed the unselected Repo")
    return {"case_id": "O09", "status": "PASS", "children": children,
            "object_name": object_name, "manifest": manifest,
            "ordered_wire_sha256": digest(ordered_wire),
            "fetched_sha256": digest(destination),
            "repo_state_before": before, "repo_state_after_seed": after_seed,
            "repo_state_after_missing": after_missing,
            "repo_state_after_restore": after, "restart_epochs": restarts}


def run_spec193_insert_published_data(ndn, output: Path,
                                     processes: List[Dict[str, Any]],
                                     repo_client: Path,
                                     servers: List[Dict[str, Any]],
                                     trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O10 requires two independent object Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o10-published-content.bin"
    source.write_bytes(bytes((index * 71 + 29) % 256 for index in range(301)))
    wire = work / "o10-original-signed-data.wire"
    certificate = work / "o10-publisher-certificate.wire"
    report = work / "o10-insert-operation.json"
    destination = work / "o10-consumer-signed-data.wire"
    object_name = "/spec193/inserted/" + output.name
    data_prefix = USER + "/spec193/upload/" + output.name
    data_name = data_prefix + "/seg=0"
    if any(item["objectName"] in (object_name, data_name) for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O10 target parent or Data name already exists")
    publisher = spawn(
        ndn, output, processes, APP_HOST, "spec193-o10-data-publisher",
        [str(repo_client), "--mode", "object-data-publisher",
         "--source", str(source), "--wire-source", str(wire),
         "--certificate", str(certificate), "--data-prefix", data_prefix,
         "--user", USER],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True))
    assert_no_python_mapped(publisher)
    wait_log(publisher, "SPEC193_DATA_PUBLISHER_READY", 30)
    if publisher["proc"].poll() is not None or not wire.is_file() or \
            not certificate.is_file():
        raise RuntimeError("O10 signed Data producer did not stay ready")
    base = ["--source", str(source), "--wire-source", str(wire),
            "--certificate", str(certificate), "--object-name", object_name,
            "--data-prefix", data_prefix, "--group", GROUP,
            "--controller", CONTROLLER, "--authority", CONTROLLER,
            "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    inserted = run_child(
        ndn, output, processes, APP_HOST, "spec193-o10-insert-request",
        [str(repo_client), "--mode", "object-insert-request",
         "--report-out", str(report), "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        210, require_no_python=True)
    if "SPEC193_DATA_PUBLISHER_SENT" not in read_log(publisher["log"]):
        raise RuntimeError("O10 Repo INSERT did not fetch published Data over NDN")
    operation = json.loads(report.read_text(encoding="utf-8"))
    if operation.get("state") != "DONE" or operation.get("completedSegments") != 1:
        raise RuntimeError("O10 INSERT operation report was not DONE")
    after_insert = {item["repo_node"]: repo_database_state(item["repo_dir"])
                    for item in servers}
    stored = [item for item in after_insert[REPO_A_ID]["repo_manifests"]["entries"]
              if item["objectName"] in (object_name, data_name)]
    if len(stored) != 2 or \
            not all(item["sha256"] == digest(wire) for item in stored) or \
            before[REPO_B_ID] != after_insert[REPO_B_ID]:
        raise RuntimeError("O10 Repo A lacks inserted parent/Data durable wire")
    consumed = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o10-consumer-status-fetch",
        [str(repo_client), "--mode", "object-insert-fetch",
         "--report", str(report), "--destination", str(destination),
         "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        210, require_no_python=True)
    if digest(destination) != digest(wire):
        raise RuntimeError("O10 consumer Data wire differs from published wire")
    after_fetch = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    if after_fetch != after_insert:
        raise RuntimeError("O10 consumer read changed durable Repo storage")
    stopped = stop_daemon(publisher)
    if stopped["stop_method"] != "SIGTERM" or \
            stopped["exit_code"] not in (0, -signal.SIGTERM):
        raise RuntimeError("O10 Data publisher did not stop cleanly")
    return {"case_id": "O10", "status": "PASS",
            "children": [inserted, consumed], "publisher_stop": stopped,
            "operation": operation, "object_name": object_name,
            "data_name": data_name, "wire_sha256": digest(wire),
            "fetched_sha256": digest(destination),
            "repo_state_before": before, "repo_state_after_insert": after_insert,
            "repo_state_after_fetch": after_fetch}


def run_spec193_insert_payload(ndn, output: Path,
                              processes: List[Dict[str, Any]],
                              environments: Dict[str, Dict[str, str]],
                              repo_client: Path,
                              servers: List[Dict[str, Any]],
                              trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O11 requires two independent object Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o11-payload-source.bin"
    source.write_bytes(bytes((index * 79 + 31) % 256 for index in range(385)))
    manifest_path = work / "o11-parent-manifest.json"
    certificate = work / "o11-provider-signing-certificate.wire"
    destination = work / "o11-consumer-reassembled-payload.bin"
    object_name = "/spec193/auto-segmented/" + output.name
    segment_bytes = 128
    count = (source.stat().st_size + segment_bytes - 1) // segment_bytes
    if any(item["objectName"].startswith(object_name) for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O11 target payload namespace already exists")
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    seed = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o11-local-insert-payload",
        servers[0]["argv"] + [
            "--local-command", "payload-insert", "--local-source", str(source),
            "--local-object", object_name, "--local-manifest", str(manifest_path),
            "--local-certificate", str(certificate),
            "--local-segment-bytes", str(segment_bytes)],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_INSERT_PAYLOAD_DONE" not in read_log(Path(seed["log"])):
        raise RuntimeError("O11 local RepoClient did not complete insertPayload")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    names = [object_name + "/seg=" + str(index) for index in range(count)]
    wire_paths = [work / ("o11-segment-" + str(index) + ".wire")
                  for index in range(count)]
    ordered_wire = work / "o11-expected-ordered-wires.bin"
    ordered_wire.write_bytes(b"".join(path.read_bytes() for path in wire_paths))
    if (manifest.get("objectName") != object_name or
            manifest.get("objectType") != "ndn-auto-segmented-payload" or
            manifest.get("packetNames") != names or
            manifest.get("segmentCount") != count or
            manifest.get("size") != ordered_wire.stat().st_size or
            manifest.get("sha256") != digest(ordered_wire)):
        raise RuntimeError("O11 insertPayload parent manifest or wire digest invalid")
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o11-insert-payload", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    time.sleep(2)
    after_seed = {item["repo_node"]: repo_database_state(item["repo_dir"])
                  for item in servers}
    stored = {item["objectName"]: item for item in
              after_seed[REPO_A_ID]["repo_manifests"]["entries"]}
    if not set(names + [object_name]).issubset(stored) or \
            any(stored[name]["sha256"] != digest(wire_paths[index])
                for index, name in enumerate(names)) or \
            before[REPO_B_ID] != after_seed[REPO_B_ID]:
        raise RuntimeError("O11 durable Data segment inventory or unselected Repo changed")
    consumed = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o11-consumer-reassemble",
        [str(repo_client), "--mode", "object-payload-fetch",
         "--source", str(source), "--wire-dir", str(work),
         "--certificate", str(certificate), "--destination", str(destination),
         "--object-name", object_name, "--user", CONSUMER,
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if digest(destination) != digest(source) or \
            "SPEC193_INSERT_PAYLOAD_NETWORK_PASS" not in read_log(Path(consumed["log"])):
        raise RuntimeError("O11 consumer could not verify full application payload")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if after != after_seed:
        raise RuntimeError("O11 consumer read changed durable Repo storage")
    return {"case_id": "O11", "status": "PASS", "children": [seed, consumed],
            "object_name": object_name, "segment_bytes": segment_bytes,
            "segment_count": count, "manifest": manifest,
            "source_sha256": digest(source), "fetched_sha256": digest(destination),
            "ordered_wire_sha256": digest(ordered_wire),
            "repo_state_before": before, "repo_state_after_insert": after_seed,
            "repo_state_after_fetch": after, "restart_epochs": restart}


def run_spec193_insert_rejections(ndn, output: Path,
                                 processes: List[Dict[str, Any]],
                                 environments: Dict[str, Dict[str, str]],
                                 repo_client: Path,
                                 servers: List[Dict[str, Any]],
                                 trust_schema: Path) -> Dict[str, Any]:
    baseline = run_spec193_insert_payload(
        ndn, output, processes, environments, repo_client, servers, trust_schema)
    object_name = baseline["object_name"]
    before_reject = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    work = output / "artifacts"
    rejected = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o12-local-insert-rejections",
        servers[0]["argv"] + [
            "--local-command", "payload-reject",
            "--local-source", str(work / "o11-payload-source.bin"),
            "--local-object", object_name,
            "--local-segment-bytes", "128"],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    rejection_log = read_log(Path(rejected["log"]))
    if (rejection_log.count("SPEC193_O12_REJECT ") != 3 or
            "SPEC193_O12_REJECTIONS_DONE" not in rejection_log or
            not all(label in rejection_log for label in
                    ("same-name-new-content", "declared-name-mismatch",
                     "same-name-bad-signature"))):
        raise RuntimeError("O12 did not observe three stable product rejections")
    after_reject = {item["repo_node"]: repo_database_state(item["repo_dir"])
                    for item in servers}
    if after_reject != before_reject:
        raise RuntimeError("O12 rejection committed or changed durable Repo data")
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o12-insert-rejections", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    time.sleep(2)
    after_restart = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    if after_restart != before_reject:
        raise RuntimeError("O12 restart changed rejected object inventory")
    destination = work / "o12-consumer-reassembled-payload.bin"
    consumed = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o12-consumer-original-fetch",
        [str(repo_client), "--mode", "object-payload-fetch",
         "--source", str(work / "o11-payload-source.bin"),
         "--wire-dir", str(work),
         "--certificate", str(work / "o11-provider-signing-certificate.wire"),
         "--destination", str(destination), "--object-name", object_name,
         "--user", CONSUMER, "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if (digest(destination) != baseline["source_sha256"] or
            "SPEC193_INSERT_PAYLOAD_NETWORK_PASS" not in
            read_log(Path(consumed["log"]))):
        raise RuntimeError("O12 independent consumer could not verify original Data")
    after_fetch = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    if after_fetch != before_reject:
        raise RuntimeError("O12 original fetch changed durable Repo data")
    return {"case_id": "O12", "status": "PASS",
            "children": baseline["children"] + [rejected, consumed],
            "object_name": object_name,
            "source_sha256": baseline["source_sha256"],
            "fetched_sha256": digest(destination),
            "rejection_labels": ["same-name-new-content", "declared-name-mismatch",
                                 "same-name-bad-signature"],
            "repo_state_before_reject": before_reject,
            "repo_state_after_reject": after_reject,
            "repo_state_after_restart": after_restart,
            "repo_state_after_fetch": after_fetch,
            "restart_epochs": {"baseline": baseline["restart_epochs"],
                               "after_rejections": restart}}


def run_spec193_cache_status(ndn, output: Path,
                            processes: List[Dict[str, Any]],
                            environments: Dict[str, Dict[str, str]],
                            repo_client: Path,
                            servers: List[Dict[str, Any]],
                            trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O13 requires two independent object Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o13-cache-source.bin"
    source.write_bytes(bytes((index * 29 + 17) % 256 for index in range(257)))
    local_report_path = work / "o13-local-cache-status.json"
    network_report_path = work / "o13-network-cache-status.json"
    object_name = "/spec193/cache-status/" + output.name
    if any(item["objectName"] == object_name for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O13 target object already exists")
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    seeded = run_child(
        ndn, output, processes, REPO_A_HOST, "spec193-o13-local-cache-seed",
        servers[0]["argv"] + [
            "--local-command", "cache-seed", "--local-source", str(source),
            "--local-object", object_name,
            "--local-manifest", str(local_report_path)],
        node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
        90, require_no_python=True)
    if "SPEC193_O13_LOCAL_CACHE_PASS" not in read_log(Path(seeded["log"])):
        raise RuntimeError("O13 local cache-status observations failed")
    local_report = json.loads(local_report_path.read_text(encoding="utf-8"))
    if (local_report["manifest"]["objectName"] != object_name or
            local_report["expectedChargeBytes"] !=
            local_report["afterPut"]["usedBytes"] or
            local_report["afterReads"]["hits"] !=
            local_report["afterPut"]["hits"] + 2):
        raise RuntimeError("O13 local report contradicts cache operations")
    after_seed = {item["repo_node"]: repo_database_state(item["repo_dir"])
                  for item in servers}
    added = [item for item in after_seed[REPO_A_ID]["repo_manifests"]["entries"]
             if item["objectName"] == object_name]
    if (len(added) != 1 or added[0]["sha256"] != digest(source) or
            before[REPO_B_ID] != after_seed[REPO_B_ID]):
        raise RuntimeError("O13 source object absent or Repo B changed")
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o13-cache-status", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    time.sleep(2)
    fetched = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-o13-network-cache-probe",
        [str(repo_client), "--mode", "object-cache-status",
         "--source", str(source), "--report-out", str(network_report_path),
         "--object-name", object_name, "--user", CONSUMER,
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if "SPEC193_O13_NETWORK_CACHE_PASS" not in read_log(Path(fetched["log"])):
        raise RuntimeError("O13 independent network cache-status probe failed")
    network_report = json.loads(network_report_path.read_text(encoding="utf-8"))
    if (network_report["manifest"] != local_report["manifest"] or
            network_report["before"]["entryCount"] != 0 or
            network_report["afterFirst"]["entryCount"] != 1 or
            network_report["afterSecond"]["entryCount"] != 1 or
            network_report["expectedChargeBytes"] !=
            local_report["expectedChargeBytes"]):
        raise RuntimeError("O13 network cache report disagrees with stored object")
    after_fetch = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    if after_fetch != after_seed:
        raise RuntimeError("O13 cache-only reads changed durable Repo storage")
    return {"case_id": "O13", "status": "PASS", "children": [seeded, fetched],
            "object_name": object_name, "source_sha256": digest(source),
            "local_report": local_report, "network_report": network_report,
            "repo_state_before": before, "repo_state_after_seed": after_seed,
            "repo_state_after_fetch": after_fetch, "restart_epochs": restart}


def run_spec193_catalog_create(ndn, output: Path,
                              processes: List[Dict[str, Any]],
                              environments: Dict[str, Dict[str, str]],
                              repo_client: Path,
                              servers: List[Dict[str, Any]],
                              trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O14 requires two independent object Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o14-catalog-source.bin"
    source.write_bytes(bytes((index * 43 + 11) % 256 for index in range(317)))
    object_name = "/spec193/catalog-create/" + output.name
    if any(item["objectName"] == object_name for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O14 target object already exists")
    base = ["--object-name", object_name, "--source", str(source),
            "--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    published = run_child(
        ndn, output, processes, APP_HOST, "spec193-o14-network-store",
        [str(repo_client), "--mode", "object-store-fetch",
         "--destination", str(work / "o14-app-fetched.bin"),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if (digest(work / "o14-app-fetched.bin") != digest(source) or
            "SPEC193_OBJECT_NETWORK_PASS" not in read_log(Path(published["log"]))):
        raise RuntimeError("O14 APP did not publish and fetch catalog object")
    after_store = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    durable_entries = after_store[REPO_A_ID]["repo_manifests"]["entries"]
    matches = [entry for entry in durable_entries
               if entry["objectName"] == object_name]
    if (len(matches) != 1 or matches[0]["sha256"] != digest(source) or
            before[REPO_B_ID] != after_store[REPO_B_ID]):
        raise RuntimeError("O14 committed object absent from durable Repo A")

    def query(phase: str):
        report_path = work / ("o14-catalog-" + phase + ".json")
        child = run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o14-consumer-catalog-" + phase,
            [str(repo_client), "--mode", "object-catalog-query",
             "--expect", phase, "--report-out", str(report_path),
             "--user", CONSUMER] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            300, require_no_python=True)
        if "SPEC193_O14_CATALOG_PASS phase=" + phase not in read_log(Path(child["log"])):
            raise RuntimeError("O14 catalog query lacks pass marker: " + phase)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        snapshot = {entry["objectName"]: entry["manifestSha256"]
                    for entry in report["snapshot"]["entries"]}
        durable = {entry["objectName"]: entry["sha256"]
                   for entry in durable_entries}
        if snapshot != durable or report["status"]["objectCount"] != len(durable):
            raise RuntimeError("O14 catalog snapshot differs from durable manifest rows")
        return child, report

    live_child, live_report = query("live")
    after_live = {item["repo_node"]: repo_database_state(item["repo_dir"])
                  for item in servers}
    if after_live != after_store:
        raise RuntimeError("O14 live catalog query changed durable Repo data")
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o14-catalog", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    time.sleep(2)
    restarted_child, restarted_report = query("restarted")
    after_restart = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    if after_restart != after_store:
        raise RuntimeError("O14 catalog restart changed durable Repo data")
    return {"case_id": "O14", "status": "PASS",
            "children": [published, live_child, restarted_child],
            "object_name": object_name, "source_sha256": digest(source),
            "live_report": live_report, "restarted_report": restarted_report,
            "repo_state_before": before, "repo_state_after_store": after_store,
            "repo_state_after_restart": after_restart,
            "restart_epochs": restart}


def run_spec193_catalog_delete(ndn, output: Path,
                              processes: List[Dict[str, Any]],
                              environments: Dict[str, Dict[str, str]],
                              repo_client: Path,
                              servers: List[Dict[str, Any]],
                              trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("O15 requires two independent object Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "o15-catalog-source.bin"
    source.write_bytes(bytes((index * 37 + 19) % 256 for index in range(241)))
    object_name = "/spec193/catalog-delete/" + output.name
    base = ["--object-name", object_name, "--source", str(source),
            "--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    if any(item["objectName"] == object_name for state in before.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O15 target object already exists")
    published = run_child(
        ndn, output, processes, APP_HOST, "spec193-o15-network-store",
        [str(repo_client), "--mode", "object-store-fetch",
         "--destination", str(work / "o15-app-fetched.bin"),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if digest(work / "o15-app-fetched.bin") != digest(source):
        raise RuntimeError("O15 APP STORE/FETCH payload differs")
    after_store = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    if (len([item for item in after_store[REPO_A_ID]["repo_manifests"]["entries"]
             if item["objectName"] == object_name and
             item["sha256"] == digest(source)]) != 1 or
            before[REPO_B_ID] != after_store[REPO_B_ID]):
        raise RuntimeError("O15 STORE did not create exactly one target in Repo A")
    baseline_path = work / "o15-catalog-before-delete.json"
    baseline_child = run_child(
        ndn, output, processes, CONSUMER_HOST,
        "spec193-o15-consumer-before-delete",
        [str(repo_client), "--mode", "object-catalog-query",
         "--expect", "live", "--report-out", str(baseline_path),
         "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    old_epoch = baseline["status"]["catalogEpoch"]
    if old_epoch == 0 or baseline["status"]["objectCount"] != 1:
        raise RuntimeError("O15 cannot establish a pre-delete catalog epoch")
    deleted = run_child(
        ndn, output, processes, APP_HOST, "spec193-o15-network-delete",
        [str(repo_client), "--mode", "object-catalog-delete",
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if "SPEC193_O15_DELETE_PASS" not in read_log(Path(deleted["log"])):
        raise RuntimeError("O15 network DELETE did not complete")
    after_delete = {item["repo_node"]: repo_database_state(item["repo_dir"])
                    for item in servers}
    if (any(item["objectName"] == object_name for item in
            after_delete[REPO_A_ID]["repo_manifests"]["entries"]) or
            before[REPO_B_ID] != after_delete[REPO_B_ID]):
        raise RuntimeError("O15 deleted target remains durable or Repo B changed")

    def query(phase: str):
        report_path = work / ("o15-catalog-deleted-" + phase + ".json")
        child = run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o15-consumer-deleted-" + phase,
            [str(repo_client), "--mode", "object-catalog-deleted",
             "--expect", phase, "--since-epoch", str(old_epoch),
             "--report-out", str(report_path), "--user", CONSUMER] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            300, require_no_python=True)
        if "SPEC193_O15_DELETED_PASS phase=" + phase not in read_log(Path(child["log"])):
            raise RuntimeError("O15 deleted catalog query lacks pass marker")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        durable = {item["objectName"]: item["sha256"] for item in
                   after_delete[REPO_A_ID]["repo_manifests"]["entries"]}
        snapshot = {item["objectName"]: item["manifestSha256"] for item in
                    report["snapshot"]["entries"]}
        if snapshot != durable:
            raise RuntimeError("O15 deleted catalog snapshot differs from durable rows")
        return child, report

    live_child, live_report = query("live")
    if live_report["status"]["catalogEpoch"] <= old_epoch:
        raise RuntimeError("O15 delete did not advance catalog epoch")
    if servers[0]["proc"].poll() is None:
        stop_repo_server(servers[0])
    target = [servers[0]]
    restart = restart_repo_servers(
        ndn, output, processes, environments, target,
        "o15-catalog-delete", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    time.sleep(2)
    restarted_child, restarted_report = query("restarted")
    after_restart = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    if after_restart != after_delete:
        raise RuntimeError("O15 restart changed deleted Repo inventory")
    return {"case_id": "O15", "status": "PASS",
            "children": [published, baseline_child, deleted,
                         live_child, restarted_child],
            "object_name": object_name, "source_sha256": digest(source),
            "old_epoch": old_epoch, "baseline_report": baseline,
            "live_report": live_report, "restarted_report": restarted_report,
            "repo_state_before": before, "repo_state_after_store": after_store,
            "repo_state_after_delete": after_delete,
            "repo_state_after_restart": after_restart,
            "restart_epochs": restart}


def run_spec193_inventory_visibility(ndn, output: Path,
                                    processes: List[Dict[str, Any]],
                                    repo_client: Path,
                                    servers: List[Dict[str, Any]],
                                    trust_schema: Path) -> Dict[str, Any]:
    baseline = run_spec193_insert_published_data(
        ndn, output, processes, repo_client, servers, trust_schema)
    if len(servers) != 2:
        raise RuntimeError("O16 requires two independent object Repos")
    work = output / "artifacts"
    source = work / "o16-range-source.bin"
    source.write_bytes(bytes((index * 67 + 23) % 256 for index in range(517)))
    range_name = "/spec193/inventory-range/" + output.name
    before_stage = {item["repo_node"]: repo_database_state(item["repo_dir"])
                    for item in servers}
    if any(item["objectName"] == range_name for state in before_stage.values()
           for item in state["repo_manifests"]["entries"]):
        raise RuntimeError("O16 range target already exists")
    base = ["--source", str(source), "--object-name", range_name,
            "--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    children = list(baseline["children"])

    def phase(name: str):
        child = run_child(
            ndn, output, processes, APP_HOST, "spec193-o16-range-" + name,
            [str(repo_client), "--mode", "object-range-phase",
             "--expect", name, "--user", USER] + base,
            node_environment(ndn.net[APP_HOST], REPO, no_python=True),
            300, require_no_python=True)
        if "SPEC193_O16_RANGE_PASS phase=" + name not in read_log(Path(child["log"])):
            raise RuntimeError("O16 network range phase lacks pass marker: " + name)
        children.append(child)

    def audit(name: str):
        report_path = work / ("o16-inventory-" + name + ".json")
        child = run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-o16-inventory-" + name,
            [str(repo_client), "--mode", "object-inventory-audit",
             "--expect", name, "--user", CONSUMER,
             "--parent-name", baseline["object_name"],
             "--data-name", baseline["data_name"],
             "--wire", str(work / "o10-original-signed-data.wire"),
             "--report", str(work / "o10-insert-operation.json"),
             "--report-out", str(report_path)] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            300, require_no_python=True)
        if "SPEC193_O16_INVENTORY_PASS phase=" + name not in read_log(Path(child["log"])):
            raise RuntimeError("O16 independent inventory audit lacks pass marker")
        children.append(child)
        return json.loads(report_path.read_text(encoding="utf-8"))

    phase("stage")
    after_stage = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    if (after_stage[REPO_A_ID]["repo_manifests"] !=
            before_stage[REPO_A_ID]["repo_manifests"] or
            before_stage[REPO_B_ID] != after_stage[REPO_B_ID]):
        raise RuntimeError("O16 staged range created a committed manifest")
    staged_parts = [item for item in after_stage[REPO_A_ID]["payload_files"]
                    if "/staging/" in item["path"] and
                    item["path"].endswith(".part") and
                    digest(source) in item["path"]]
    if (len(staged_parts) != 1 or
            staged_parts[0]["sha256"] != digest(source) or
            any("/payloads/" in item["path"] and
                item["sha256"] == digest(source)
                for item in after_stage[REPO_A_ID]["payload_files"])):
        raise RuntimeError("O16 staged bytes were not isolated from committed payloads")
    staged_report = audit("staged")
    phase("commit")
    after_commit = {item["repo_node"]: repo_database_state(item["repo_dir"])
                    for item in servers}
    committed = [item for item in
                 after_commit[REPO_A_ID]["repo_manifests"]["entries"]
                 if item["objectName"] == range_name]
    if (len(committed) != 1 or committed[0]["sha256"] != digest(source) or
            before_stage[REPO_B_ID] != after_commit[REPO_B_ID]):
        raise RuntimeError("O16 committed range lacks durable payload or changed Repo B")
    payload_digests = [item["sha256"] for item in
                       after_commit[REPO_A_ID]["payload_files"]
                       if "/payloads/" in item["path"]]
    if (payload_digests.count(digest(source)) != 1 or
            baseline["wire_sha256"] not in payload_digests or
            any("/staging/" in item["path"] and
                digest(source) in item["path"] for item in
                after_commit[REPO_A_ID]["payload_files"])):
        raise RuntimeError("O16 committed payload or exact Data wire lacks durable bytes")
    committed_report = audit("committed")
    durable = {item["objectName"]: item["sha256"] for item in
               after_commit[REPO_A_ID]["repo_manifests"]["entries"]}
    inventory = {item["objectName"]: item["sha256"] for item in
                 committed_report["inventory"]}
    if (inventory != durable or
            staged_report["status"]["operationId"] !=
            committed_report["status"]["operationId"]):
        raise RuntimeError("O16 inventory or INSERT STATUS differs from durable rows")
    after_audit = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    if after_audit != after_commit:
        raise RuntimeError("O16 independent reads changed durable Repo storage")
    return {"case_id": "O16", "status": "PASS", "children": children,
            "insert_operation": baseline["operation"],
            "parent_name": baseline["object_name"],
            "data_name": baseline["data_name"],
            "range_name": range_name, "range_sha256": digest(source),
            "wire_sha256": baseline["wire_sha256"],
            "staged_report": staged_report,
            "committed_report": committed_report,
            "repo_state_before_stage": before_stage,
            "repo_state_after_stage": after_stage,
            "repo_state_after_commit": after_commit,
            "repo_state_after_audit": after_audit}


def run_spec193_inapp_matrix(ndn, output: Path,
                             processes: List[Dict[str, Any]],
                             repo_client: Path,
                             servers: List[Dict[str, Any]]) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("In-App matrix requires two remote Repo sentinels")
    app_home = Path(ndn.net[APP_HOST].params["params"]["homeDir"])
    local_root = app_home / ("spec193-inapp-" + output.name)
    if local_root.exists():
        raise RuntimeError("refusing to reuse In-App storage root")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    client_env = node_environment(ndn.net[APP_HOST], REPO, no_python=True)
    repo_nodes = {item["repo_node"] for item in servers}
    remote_before = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for state in remote_before.values():
        assert_no_remote_artifacts(state)
    remote_writes_before = repo_store_events(processes, repo_nodes)
    children = []

    small_source = work / "a01-small-source.bin"
    small_source.write_bytes(bytes((index * 31 + 7) % 256 for index in range(9001)))
    small_handle = work / "a01-handle.json"
    small_receipt = work / "a01-receipt.json"
    small_fetch = work / "a01-fetched.bin"
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a01-inapp-publish",
        [str(repo_client), "--mode", "inapp-publish", "--source", str(small_source),
         "--store-dir", str(local_root), "--handle-out", str(small_handle),
         "--receipt-out", str(small_receipt), "--user", USER],
        client_env, 30, require_no_python=True))
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a01-inapp-reopen",
        [str(repo_client), "--mode", "inapp-reopen", "--store-dir", str(local_root),
         "--handle", str(small_handle), "--destination", str(small_fetch)],
        client_env, 30, require_no_python=True))
    if digest(small_fetch) != digest(small_source):
        raise RuntimeError("A01 new-process In-App bytes differ from source")

    large_source = work / "a02-large-source.bin"
    large_source.write_bytes(bytes(range(256)) * 4097)
    large_handle = work / "a02-handle.json"
    large_receipt = work / "a02-receipt.json"
    large_fetch = work / "a02-fetched.bin"
    trace_prefix = output / "traces/a02-inapp-pwrite"
    trace_prefix.parent.mkdir(parents=True, exist_ok=True)
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a02-inapp-publish-traced",
        ["/usr/bin/strace", "-ff", "-yy", "-e", "trace=pwrite64",
         "-o", str(trace_prefix), str(repo_client), "--mode", "inapp-publish",
         "--source", str(large_source), "--store-dir", str(local_root),
         "--handle-out", str(large_handle), "--receipt-out", str(large_receipt),
         "--user", USER], client_env, 60))
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a02-inapp-reopen",
        [str(repo_client), "--mode", "inapp-reopen", "--store-dir", str(local_root),
         "--handle", str(large_handle), "--destination", str(large_fetch)],
        client_env, 30, require_no_python=True))
    if digest(large_fetch) != digest(large_source):
        raise RuntimeError("A02 new-process large In-App bytes differ from source")
    range_writes = set()
    for trace in trace_prefix.parent.glob(trace_prefix.name + ".*"):
        for line in read_log(trace).splitlines():
            if "pwrite64(" not in line or "/staging/" not in line or ".part>" not in line:
                continue
            match = re.search(r", (\d+), (\d+)\) = (\d+)$", line)
            if match:
                length, offset, written = map(int, match.groups())
                if length == written:
                    range_writes.add((offset, length))
    if not {(0, 1048576), (1048576, 256)}.issubset(range_writes):
        raise RuntimeError("A02 syscall trace lacks both complete staging chunk writes")

    local_state = repo_database_state(local_root)
    local_commits = sum(database["tables"].get("artifact_local_commits", {}).get("rows", 0)
                        for database in local_state["databases"].values())
    if local_commits != 2:
        raise RuntimeError("In-App store lacks two independent local commit rows")
    payloads = [item for item in local_state["payload_files"]
                if item["sha256"] == digest(large_source)]
    if len(payloads) != 1:
        raise RuntimeError("A02 store lacks one complete committed large payload")
    committed_path = local_root / payloads[0]["path"]
    original_payload = committed_path.read_bytes()
    original_receipt_sha = digest(large_receipt)
    negative_destination = work / "a03-preserved-destination.bin"
    sentinel = b"Spec193 existing destination must survive failed fetch\x00v1"
    negative_destination.write_bytes(sentinel)

    try:
        with committed_path.open("r+b") as stream:
            stream.write(bytes([original_payload[0] ^ 0xff]))
            stream.flush()
            os.fsync(stream.fileno())
        children.append(run_child(
            ndn, output, processes, APP_HOST, "spec193-a03-corrupt-fetch-rejected",
            [str(repo_client), "--mode", "inapp-reopen-expect-error",
             "--store-dir", str(local_root), "--handle", str(large_handle),
             "--destination", str(negative_destination), "--expected-error",
             "repo-artifact-digest-mismatch"],
            client_env, 30, require_no_python=True))
    finally:
        committed_path.write_bytes(original_payload)
    if negative_destination.read_bytes() != sentinel or \
            digest(large_receipt) != original_receipt_sha:
        raise RuntimeError("A03 corruption changed destination or historical receipt")

    parked_path = committed_path.with_name(committed_path.name + ".parked")
    os.replace(committed_path, parked_path)
    try:
        children.append(run_child(
            ndn, output, processes, APP_HOST, "spec193-a03-missing-fetch-rejected",
            [str(repo_client), "--mode", "inapp-reopen-expect-error",
             "--store-dir", str(local_root), "--handle", str(large_handle),
             "--destination", str(negative_destination), "--expected-error",
             "repo-artifact-payload-missing"],
            client_env, 30, require_no_python=True))
    finally:
        os.replace(parked_path, committed_path)
    if negative_destination.read_bytes() != sentinel or \
            digest(committed_path) != digest(large_source) or \
            digest(large_receipt) != original_receipt_sha:
        raise RuntimeError("A03 missing-object probe failed to restore exact state")
    final_local = repo_database_state(local_root)
    final_commits = sum(database["tables"].get("artifact_local_commits", {}).get("rows", 0)
                        for database in final_local["databases"].values())
    if final_commits != 2:
        raise RuntimeError("A03 failed fetch changed local commit history")
    remote_after = {item["repo_node"]: repo_database_state(item["repo_dir"])
                    for item in servers}
    if remote_before != remote_after or \
            repo_store_events(processes, repo_nodes) != remote_writes_before:
        raise RuntimeError("In-App matrix reached or changed a remote Repo")
    return {"case_id": "INAPP_MATRIX", "status": "PASS", "children": children,
            "a01": {"source_sha256": digest(small_source),
                    "fetched_sha256": digest(small_fetch)},
            "a02": {"source_sha256": digest(large_source),
                    "fetched_sha256": digest(large_fetch),
                    "staging_writes": sorted(range_writes)},
            "a03": {"destination_sha256": digest(negative_destination),
                    "receipt_sha256_unchanged": digest(large_receipt) == original_receipt_sha,
                    "restored_payload_sha256": digest(committed_path)},
            "local_store": final_local, "remote_state_before": remote_before,
            "remote_state_after": remote_after,
            "remote_authenticated_writes": repo_store_events(processes, repo_nodes)}


def run_spec193_invalid_inputs(ndn, output: Path,
                               processes: List[Dict[str, Any]],
                               repo_client: Path,
                               servers: List[Dict[str, Any]],
                               trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("invalid-input matrix requires two remote Repo sentinels")
    app_home = Path(ndn.net[APP_HOST].params["params"]["homeDir"])
    local_root = app_home / ("spec193-invalid-" + output.name)
    if local_root.exists():
        raise RuntimeError("refusing to reuse invalid-input storage root")
    source = output / "artifacts/a04-valid-seed.bin"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(bytes(range(256)) * 16)
    client_env = node_environment(ndn.net[APP_HOST], REPO, no_python=True)
    repo_nodes = {item["repo_node"] for item in servers}
    remote_before = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for state in remote_before.values():
        assert_no_remote_artifacts(state)
    remote_writes_before = repo_store_events(processes, repo_nodes)
    children = [run_child(
        ndn, output, processes, APP_HOST, "spec193-a04-valid-local-seed",
        [str(repo_client), "--mode", "inapp-publish", "--source", str(source),
         "--store-dir", str(local_root), "--handle-out",
         str(output / "artifacts/a04-seed-handle.json"), "--receipt-out",
         str(output / "artifacts/a04-seed-receipt.json"), "--user", USER],
        client_env, 30, require_no_python=True)]
    local_before = repo_database_state(local_root)
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a04-invalid-inapp",
        [str(repo_client), "--mode", "inapp-invalid-inputs", "--source", str(source),
         "--store-dir", str(local_root), "--user", USER],
        client_env, 30, require_no_python=True))
    if repo_database_state(local_root) != local_before:
        raise RuntimeError("A04 invalid In-App input changed local durable state")
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a04-invalid-persistent-config",
        [str(repo_client), "--mode", "persistent-invalid-config",
         "--group", GROUP, "--controller", CONTROLLER,
         "--authority", CONTROLLER, "--user", USER,
         "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        client_env, 90, require_no_python=True))
    if repo_database_state(local_root) != local_before:
        raise RuntimeError("A04 invalid Persistent configuration changed In-App state")
    remote_after = {item["repo_node"]: repo_database_state(item["repo_dir"])
                    for item in servers}
    if remote_before != remote_after or \
            repo_store_events(processes, repo_nodes) != remote_writes_before:
        raise RuntimeError("A04 invalid input changed a remote Repo")
    for child, markers in zip(children[1:],
                              [("reference", "missing-source", "symlink-source",
                                "symlink-root"), ("persistent-deadline",)]):
        log = read_log(Path(child["log"]))
        if any("SPEC193_INVALID_INPUT_REJECTED case=" + marker not in log
               for marker in markers):
            raise RuntimeError("A04 client lacks a required rejection marker")
    return {"case_id": "A04", "status": "PASS", "children": children,
            "local_state_before": local_before,
            "local_state_after": repo_database_state(local_root),
            "remote_state_before": remote_before,
            "remote_state_after": remote_after,
            "remote_authenticated_writes": repo_store_events(processes, repo_nodes)}


def run_spec193_single_target(ndn, output: Path,
                              processes: List[Dict[str, Any]],
                              repo_client: Path,
                              servers: List[Dict[str, Any]],
                              trust_schema: Path,
                              range_case: bool = False,
                              large_case: bool = False) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("single-target campaign requires two Repo sentinels")
    repo_a, repo_b = servers
    before_a = repo_database_state(repo_a["repo_dir"])
    before_b = repo_database_state(repo_b["repo_dir"])
    assert_no_remote_artifacts(before_a)
    assert_no_remote_artifacts(before_b)
    source = output / "artifacts/a06-single-target-source.bin"
    source.parent.mkdir(parents=True, exist_ok=True)
    source_bytes = 131123 if large_case else (4401 if range_case else 2048)
    source.write_bytes(bytes((index * 17 + 3) % 256 for index in range(source_bytes)))
    handle = output / "artifacts/a06-single-target-handle.json"
    receipt_path = output / "artifacts/a06-single-target-receipt.json"
    destination = output / "artifacts/a06-consumer-fetched.bin"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    writes_before = repo_store_events(processes, {REPO_A_ID, REPO_B_ID})
    publish_child = run_child(
        ndn, output, processes, APP_HOST, "spec193-a06-single-target-publish",
        [str(repo_client), "--mode", "single-publish", "--source", str(source),
         "--handle-out", str(handle), "--receipt-out", str(receipt_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        3600 if large_case else (900 if range_case else 600), require_no_python=True)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    commits = receipt.get("replicaCommits", [])
    if (receipt.get("lifecycleMode") != "PERSISTENT" or
            receipt.get("writeConsistency") != "ONE" or
            receipt.get("requestedReplicas") != 1 or
            receipt.get("requiredAcks") != 1 or
            receipt.get("achievedReplicas") != 1 or
            len(commits) != 1 or
            commits[0].get("repoNode") != REPO_A_ID or
            commits[0].get("state") != "COMMITTED"):
        raise RuntimeError("A06 receipt does not bind exactly one committed Repo A")
    after_a = repo_database_state(repo_a["repo_dir"])
    after_b = repo_database_state(repo_b["repo_dir"])
    before_receipts = repo_table_row_counts(before_a)["artifact_persistent_receipts"]
    after_receipts = repo_table_row_counts(after_a)["artifact_persistent_receipts"]
    fresh_paths = {item["path"] for item in after_a["payload_files"]} - \
                  {item["path"] for item in before_a["payload_files"]}
    fresh_payloads = [item for item in after_a["payload_files"]
                      if item["path"] in fresh_paths and
                      item["sha256"] == digest(source)]
    if after_receipts != before_receipts + 1 or len(fresh_payloads) != 1:
        raise RuntimeError("A06 Repo A lacks one durable receipt and complete payload")
    if after_b != before_b:
        raise RuntimeError("A06 unselected Repo B changed durable state")
    new_writes = repo_store_events(processes, {REPO_A_ID, REPO_B_ID})[
        len(writes_before):]
    if not new_writes or any(item["repo_node"] != REPO_A_ID for item in new_writes):
        raise RuntimeError("A06 authenticated STORE trace reached wrong Repo")
    if range_case:
        operations = [item["operation"] for item in new_writes]
        if operations.count("STORE_RANGE") < 2 or "COMMIT_RANGES" not in operations:
            raise RuntimeError("A06 range case lacks two stages and final commit")
    fetch_child = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-a06-independent-consumer-fetch",
        [str(repo_client), "--mode", "fetch", "--handle", str(handle),
         "--destination", str(destination), "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if digest(destination) != digest(source) or repo_database_state(repo_b["repo_dir"]) != before_b:
        raise RuntimeError("A06 independent consumer bytes or Repo B state mismatch")
    return {"case_id": "A06_LARGE" if large_case else ("A06_RANGE" if range_case else "A06"), "status": "PASS",
            "source_bytes": source_bytes, "children": [publish_child, fetch_child],
            "publish_request_budget_ms": 600000,
            "publish_child_timeout_s": 3600 if large_case else (900 if range_case else 600),
            "source_sha256": digest(source), "fetched_sha256": digest(destination),
            "receipt": receipt, "repo_a_before": before_a, "repo_a_after": after_a,
            "repo_b_before": before_b, "repo_b_after": repo_database_state(repo_b["repo_dir"]),
            "authenticated_writes": new_writes}


def run_spec193_source_removed_fetch(ndn, output: Path,
                                     processes: List[Dict[str, Any]],
                                     repo_client: Path,
                                     servers: List[Dict[str, Any]],
                                     trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A15 requires two Persistent Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "a15-original-application-source.bin"
    source.write_bytes(bytes((index * 47 + 19) % 256 for index in range(4401)))
    source_sha = digest(source)
    handle = work / "a15-persistent-handle.json"
    receipt_path = work / "a15-publish-receipt.json"
    destination = work / "a15-independent-consumer-fetch.bin"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    publisher = run_child(
        ndn, output, processes, APP_HOST, "spec193-a15-publish",
        [str(repo_client), "--mode", "publish", "--source", str(source),
         "--handle-out", str(handle), "--receipt-out", str(receipt_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        1200, require_no_python=True)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    commits = receipt.get("replicaCommits", [])
    if (receipt.get("lifecycleMode") != "PERSISTENT" or
            receipt.get("writeConsistency") != "ALL" or
            receipt.get("requiredAcks") != 2 or
            receipt.get("achievedReplicas") != 2 or
            len(commits) != 2 or
            {item.get("repoNode"): item.get("state") for item in commits} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"}):
        raise RuntimeError("A15 publish receipt did not bind two committed Repos")
    after_publish = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for state in after_publish.values():
        assert_repo_durable_state(state, source_sha)
    source.unlink()
    if source.exists() or destination.exists():
        raise RuntimeError("A15 original source remained or destination preexisted")
    consumer = run_child(
        ndn, output, processes, CONSUMER_HOST,
        "spec193-a15-independent-consumer-after-source-removal",
        [str(repo_client), "--mode", "fetch", "--handle", str(handle),
         "--destination", str(destination), "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if (not destination.is_file() or digest(destination) != source_sha or
            source.exists() or consumer["pid"] == publisher["pid"] or
            consumer["host"] == publisher["host"]):
        raise RuntimeError("A15 independent fetch did not survive source removal")
    after_fetch = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    for node in before:
        if repo_artifact_inventory(after_fetch[node]) != \
                repo_artifact_inventory(after_publish[node]):
            raise RuntimeError("A15 read changed durable Repo artifact state: " + node)
    fetch_requests = [item for item in repo_request_events(processes, set(before))
                      if item["requester"] == CONSUMER]
    if not any(item["operation"] in ("FETCH", "FETCH_RANGE") and
               item["requester"] == CONSUMER
               for item in fetch_requests):
        raise RuntimeError("A15 lacks an authenticated remote FETCH/FETCH_RANGE")
    return {"case_id": "A15", "status": "PASS", "children": [publisher, consumer],
            "source_sha256": source_sha, "fetched_sha256": digest(destination),
            "source_path": str(source), "source_exists_after_fetch": source.exists(),
            "receipt": receipt,
            "repo_state_before": before, "repo_state_after_publish": after_publish,
            "repo_state_after_fetch": after_fetch,
            "authenticated_fetch_requests": fetch_requests}


def run_spec193_receipt_lookup(ndn, output: Path,
                               processes: List[Dict[str, Any]],
                               repo_client: Path,
                               servers: List[Dict[str, Any]],
                               trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A16 requires two Persistent Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    sources = [work / "a16-source-a.bin", work / "a16-source-b.bin"]
    sources[0].write_bytes(bytes((index * 29 + 7) % 256 for index in range(2048)))
    sources[1].write_bytes(bytes((index * 31 + 11) % 256 for index in range(2049)))
    handles = [work / "a16-handle-a.json", work / "a16-handle-b.json"]
    receipts = [work / "a16-receipt-a.json", work / "a16-receipt-b.json"]
    queried = [work / "a16-query-a-before.json", work / "a16-query-b.json",
               work / "a16-query-a-after.json"]
    common = ["--group", GROUP, "--controller", CONTROLLER,
              "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
              "--service-prefix", SERVICE_PREFIX]
    target_args = [
        ["--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        ["--repo-a", REPO_B_ID, "--provider-a", REPO_B_PROVIDER,
         "--repo-b", REPO_A_ID, "--provider-b", REPO_A_PROVIDER],
    ]
    children = []
    query_traces = []

    def status_counts() -> Dict[str, int]:
        events = repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
        return {node: sum(item["operation"] == "STATUS" and
                          item["requester"] == USER and item["repo_node"] == node
                          for item in events)
                for node in (REPO_A_ID, REPO_B_ID)}

    def publish(index: int) -> None:
        child = run_child(
            ndn, output, processes, APP_HOST,
            "spec193-a16-publish-" + ("a" if index == 0 else "b"),
            [str(repo_client), "--mode", "single-publish", "--source",
             str(sources[index]), "--handle-out", str(handles[index]),
             "--receipt-out", str(receipts[index]), "--user", USER] +
            common + target_args[index],
            node_environment(ndn.net[APP_HOST], REPO, no_python=True),
            600, require_no_python=True)
        children.append(child)
        receipt = json.loads(receipts[index].read_text(encoding="utf-8"))
        expected_repo = REPO_A_ID if index == 0 else REPO_B_ID
        if (receipt.get("lifecycleMode") != "PERSISTENT" or
                receipt.get("achievedReplicas") != 1 or
                [(item.get("repoNode"), item.get("state")) for item in
                 receipt.get("replicaCommits", [])] !=
                [(expected_repo, "COMMITTED")]):
            raise RuntimeError("A16 receipt did not bind its selected seed Repo")
        assert_repo_durable_state(
            repo_database_state(servers[index]["repo_dir"]), digest(sources[index]))

    def query(label: str, handle: Path, receipt: Path,
              extra: List[str], target_repo: Optional[str],
              output_path: Optional[Path] = None) -> None:
        argv = [str(repo_client), "--mode", "receipt-query", "--handle",
                str(handle), "--receipt", str(receipt), "--user", USER]
        if output_path is not None:
            argv += ["--receipt-out", str(output_path)]
        counts_before = status_counts()
        child = run_child(
            ndn, output, processes, APP_HOST, "spec193-a16-" + label,
            argv + extra + common + target_args[0],
            node_environment(ndn.net[APP_HOST], REPO, no_python=True),
            150, require_no_python=True)
        children.append(child)
        counts_after = status_counts()
        deltas = {node: counts_after[node] - counts_before[node]
                  for node in counts_before}
        if (target_repo is None and any(deltas.values())) or \
                (target_repo is not None and
                 (deltas[target_repo] < 1 or
                  any(count for node, count in deltas.items()
                      if node != target_repo))):
            raise RuntimeError("A16 authenticated STATUS routed to wrong Repo: " + label)
        query_traces.append({"label": label, "target_repo": target_repo,
                             "authenticated_status_delta": deltas})

    publish(0)
    receipt_a_bytes = receipts[0].read_bytes()
    query("query-a-before", handles[0], receipts[0], [], REPO_A_ID, queried[0])
    if queried[0].read_bytes() != receipt_a_bytes:
        raise RuntimeError("A16 first seed query changed immutable receipt A")
    after_a = repo_database_state(servers[0]["repo_dir"])
    publish(1)
    receipt_b_bytes = receipts[1].read_bytes()
    if repo_artifact_inventory(repo_database_state(servers[0]["repo_dir"])) != \
            repo_artifact_inventory(after_a):
        raise RuntimeError("A16 second seed changed first Repo artifact inventory")
    query("query-b", handles[1], receipts[1], [], REPO_B_ID, queried[1])
    query("query-a-after", handles[0], receipts[0], [], REPO_A_ID, queried[2])
    if (queried[1].read_bytes() != receipt_b_bytes or
            queried[2].read_bytes() != receipt_a_bytes or
            receipts[0].read_bytes() != receipt_a_bytes):
        raise RuntimeError("A16 historical receipt bytes changed after second publish")
    query("reject-wrong-handle", handles[0], receipts[1],
          ["--expected-error", "Persistent receipt ID or seed does not match"],
          None)
    query("reject-forged-handle", handles[0], receipts[1],
          ["--forge-handle-receipt", "1", "--expected-error",
           "repo-receipt-not-found"], REPO_A_ID)
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    for index, node in enumerate((REPO_A_ID, REPO_B_ID)):
        assert_repo_durable_state(after[node], digest(sources[index]))
    if (repo_artifact_inventory(after[REPO_A_ID]) !=
            repo_artifact_inventory(after_a) or
            receipts[0].read_bytes() != receipt_a_bytes or
            receipts[1].read_bytes() != receipt_b_bytes):
        raise RuntimeError("A16 negative queries changed historical receipt state")
    return {"case_id": "A16", "status": "PASS", "children": children,
            "source_sha256": [digest(path) for path in sources],
            "receipt_a_sha256": hashlib.sha256(receipt_a_bytes).hexdigest(),
            "receipt_b_sha256": hashlib.sha256(receipt_b_bytes).hexdigest(),
            "repo_state_before": before, "repo_a_after_first": after_a,
            "repo_state_after": after, "authenticated_query_traces": query_traces}


def run_spec193_metadata_availability(ndn, output: Path,
                                      processes: List[Dict[str, Any]],
                                      repo_client: Path,
                                      servers: List[Dict[str, Any]],
                                      trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A17 requires two Persistent Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "a17-metadata-source.bin"
    source.write_bytes(bytes((index * 37 + 13) % 256 for index in range(2048)))
    source_sha = digest(source)
    handle_path = work / "a17-handle.json"
    receipt_path = work / "a17-receipt.json"
    report_path = work / "a17-metadata-report.json"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    publisher = run_child(
        ndn, output, processes, APP_HOST, "spec193-a17-publish-all",
        [str(repo_client), "--mode", "publish", "--source", str(source),
         "--handle-out", str(handle_path), "--receipt-out", str(receipt_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        900, require_no_python=True)
    handle = json.loads(handle_path.read_text(encoding="utf-8"))
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    commits = {item["repoNode"]: item for item in receipt["replicaCommits"]}
    replicas = {item["repoNode"]: item for item in handle["replicaCommits"]}
    if (receipt.get("lifecycleMode") != "PERSISTENT" or
            receipt.get("writeConsistency") != "ALL" or
            receipt.get("achievedReplicas") != 2 or
            {node: item["state"] for node, item in commits.items()} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"} or
            set(replicas) != {REPO_A_ID, REPO_B_ID}):
        raise RuntimeError("A17 setup did not commit both selected Repo locations")
    after_publish = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for state in after_publish.values():
        assert_repo_durable_state(state, source_sha)
    status_before = [item for item in
                     repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
                     if item["operation"] == "STATUS" and
                     item["requester"] == CONSUMER]
    probe = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-a17-metadata-probe",
        [str(repo_client), "--mode", "availability", "--handle", str(handle_path),
         "--probe-kind", "metadata", "--expect", "metadata-healthy",
         "--target-repo", REPO_A_ID, "--timeout-ms", "180000",
         "--report-out", str(report_path), "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        210, require_no_python=True)
    report = json.loads(report_path.read_text(encoding="utf-8"))
    locations = {item["repoNode"]: item for item in report["locations"]}
    if (report.get("probeKind") != "metadata" or
            report.get("artifact") != handle["reference"] or
            set(locations) != {REPO_A_ID, REPO_B_ID}):
        raise RuntimeError("A17 metadata report omitted current selected locations")
    for node, location in locations.items():
        historic = replicas[node]
        if (location.get("locationKind") != "PERSISTENT" or
                location.get("providerIdentity") != historic["providerIdentity"] or
                location.get("generation") != historic["generation"] or
                location.get("operationId") != historic["operationId"] or
                location.get("receiptId") != historic["receiptId"] or
                location.get("locationVersion", 0) < historic["locationVersion"] or
                location.get("reachability") != "REACHABLE" or
                location.get("objectPresence") != "PRESENT" or
                location.get("integrity") != "NOT_CHECKED" or
                location.get("validUntilMs", 0) <= location.get("observedAtMs", 0)):
            raise RuntimeError("A17 current metadata mismatches receipt/location: " + node)
    after_probe = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    for node in before:
        if repo_artifact_inventory(after_probe[node]) != \
                repo_artifact_inventory(after_publish[node]):
            raise RuntimeError("A17 metadata probe changed durable artifact state: " + node)
    status_after = [item for item in
                    repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
                    if item["operation"] == "STATUS" and
                    item["requester"] == CONSUMER]
    deltas = {node: sum(item["repo_node"] == node for item in status_after) -
                    sum(item["repo_node"] == node for item in status_before)
              for node in (REPO_A_ID, REPO_B_ID)}
    if any(count < 2 for count in deltas.values()):
        raise RuntimeError("A17 lacks fresh authenticated LOCATION/AVAILABILITY probes")
    return {"case_id": "A17", "status": "PASS", "children": [publisher, probe],
            "source_sha256": source_sha, "handle": handle, "receipt": receipt,
            "metadata_report": report, "authenticated_status_delta": deltas,
            "repo_state_before": before, "repo_state_after_publish": after_publish,
            "repo_state_after_probe": after_probe}


def run_spec193_verify_content_availability(ndn, output: Path,
                                            processes: List[Dict[str, Any]],
                                            repo_client: Path,
                                            servers: List[Dict[str, Any]],
                                            trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A18 requires two Persistent Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "a18-verify-source.bin"
    source.write_bytes(bytes((index * 41 + 5) % 256 for index in range(2048)))
    source_sha = digest(source)
    handle = work / "a18-handle.json"
    receipt_path = work / "a18-receipt.json"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    children = [run_child(
        ndn, output, processes, APP_HOST, "spec193-a18-publish-all",
        [str(repo_client), "--mode", "publish", "--source", str(source),
         "--handle-out", str(handle), "--receipt-out", str(receipt_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        900, require_no_python=True)]
    receipt_bytes = receipt_path.read_bytes()
    receipt = json.loads(receipt_bytes)
    if (receipt.get("writeConsistency") != "ALL" or
            receipt.get("achievedReplicas") != 2 or
            {item["repoNode"]: item["state"] for item in
             receipt.get("replicaCommits", [])} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"}):
        raise RuntimeError("A18 setup did not commit both selected Repos")
    after_publish = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for state in after_publish.values():
        assert_repo_durable_state(state, source_sha)

    def receipt_rows(state: Dict[str, Any]) -> Dict[str, Any]:
        return {path: {table: values for table, values in db["tables"].items()
                       if table in ("artifact_persistent_receipts",
                                    "artifact_publish_receipts")}
                for path, db in state["databases"].items()}

    baseline_receipts = {node: receipt_rows(state)
                         for node, state in after_publish.items()}
    report_paths = []
    reports = []
    status_traces = []

    def probe(label: str, kind: str, expectation: str) -> Dict[str, Any]:
        path = work / (label + ".json")
        before_events = [item for item in
                         repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
                         if item["operation"] == "STATUS" and
                         item["requester"] == CONSUMER]
        child = run_child(
            ndn, output, processes, CONSUMER_HOST, label,
            [str(repo_client), "--mode", "availability", "--handle", str(handle),
             "--probe-kind", kind, "--expect", expectation,
             "--target-repo", REPO_A_ID, "--timeout-ms", "180000",
             "--report-out", str(path), "--user", CONSUMER] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            210, require_no_python=True)
        children.append(child)
        report = json.loads(path.read_text(encoding="utf-8"))
        locations = {item["repoNode"]: item for item in report["locations"]}
        if (report.get("probeKind") != kind or
                set(locations) != {REPO_A_ID, REPO_B_ID} or
                any(item["reachability"] != "REACHABLE" or
                    item["objectPresence"] != "PRESENT" for item in
                    locations.values())):
            raise RuntimeError("A18 probe lacked current complete locations: " + label)
        after_events = [item for item in
                        repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
                        if item["operation"] == "STATUS" and
                        item["requester"] == CONSUMER]
        deltas = {node: sum(item["repo_node"] == node for item in after_events) -
                        sum(item["repo_node"] == node for item in before_events)
                  for node in (REPO_A_ID, REPO_B_ID)}
        if any(value < 2 for value in deltas.values()):
            raise RuntimeError("A18 probe lacked authenticated LOCATION/AVAILABILITY: " + label)
        state = {item["repo_node"]: repo_database_state(item["repo_dir"])
                 for item in servers}
        if ({node: receipt_rows(value) for node, value in state.items()} !=
                baseline_receipts or receipt_path.read_bytes() != receipt_bytes):
            raise RuntimeError("A18 availability probe changed historical receipts")
        report_paths.append(str(path))
        reports.append(report)
        status_traces.append({"label": label, "authenticated_status_delta": deltas})
        return locations

    healthy_before = probe("spec193-a18-verify-healthy-before",
                           "verify_content", "healthy")
    if any(item["integrity"] != "VERIFIED" for item in healthy_before.values()):
        raise RuntimeError("A18 complete bytes did not verify before fault")
    repo_a_payload = next(
        Path(servers[0]["repo_dir"]) / item["path"]
        for item in after_publish[REPO_A_ID]["payload_files"]
        if item["sha256"] == source_sha)
    original_bytes = repo_a_payload.read_bytes()
    try:
        with repo_a_payload.open("r+b") as stream:
            stream.write(bytes([original_bytes[0] ^ 0xff]) + original_bytes[1:])
            stream.flush()
            os.fsync(stream.fileno())
        if (repo_a_payload.stat().st_size != len(original_bytes) or
                digest(repo_a_payload) == source_sha):
            raise RuntimeError("A18 fault did not alter bytes at the same size")
        metadata = probe("spec193-a18-corrupt-metadata",
                         "metadata", "metadata-healthy")
        if any(item["integrity"] != "NOT_CHECKED" for item in metadata.values()):
            raise RuntimeError("A18 metadata probe claimed full verification")
        corrupt = probe("spec193-a18-verify-corrupt-a",
                        "verify_content", "corrupt")
        if (corrupt[REPO_A_ID]["integrity"] != "FAILED" or
                corrupt[REPO_B_ID]["integrity"] != "VERIFIED"):
            raise RuntimeError("A18 corrupt A or intact B integrity classification was wrong")
    finally:
        with repo_a_payload.open("r+b") as stream:
            stream.write(original_bytes)
            stream.truncate(len(original_bytes))
            stream.flush()
            os.fsync(stream.fileno())
    if digest(repo_a_payload) != source_sha:
        raise RuntimeError("A18 fault restoration did not recover exact bytes")
    healthy_after = probe("spec193-a18-verify-healthy-after",
                          "verify_content", "healthy")
    if any(item["integrity"] != "VERIFIED" for item in healthy_after.values()):
        raise RuntimeError("A18 restored bytes did not verify")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if any(repo_artifact_inventory(after[node]) !=
           repo_artifact_inventory(after_publish[node]) for node in after):
        raise RuntimeError("A18 restored Repo artifact inventory differs from original")
    return {"case_id": "A18", "status": "PASS", "children": children,
            "source_sha256": source_sha, "receipt_sha256":
            hashlib.sha256(receipt_bytes).hexdigest(), "reports": reports,
            "report_paths": report_paths, "authenticated_status_traces": status_traces,
            "repo_state_before": before, "repo_state_after_publish": after_publish,
            "repo_state_after_restore": after}


def run_spec193_missing_partial_availability(ndn, output: Path,
                                             processes: List[Dict[str, Any]],
                                             repo_client: Path,
                                             servers: List[Dict[str, Any]],
                                             trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A19 requires two Persistent Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "a19-range-source.bin"
    source.write_bytes(bytes((index * 43 + 17) % 256 for index in range(4401)))
    source_sha = digest(source)
    handle = work / "a19-handle.json"
    receipt_path = work / "a19-receipt.json"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    children = [run_child(
        ndn, output, processes, APP_HOST, "spec193-a19-publish-all",
        [str(repo_client), "--mode", "publish", "--source", str(source),
         "--handle-out", str(handle), "--receipt-out", str(receipt_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        1200, require_no_python=True)]
    receipt_bytes = receipt_path.read_bytes()
    receipt = json.loads(receipt_bytes)
    if (receipt.get("writeConsistency") != "ALL" or
            receipt.get("achievedReplicas") != 2 or
            {item["repoNode"]: item["state"] for item in
             receipt.get("replicaCommits", [])} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"}):
        raise RuntimeError("A19 setup did not commit two selected locations")
    after_publish = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for state in after_publish.values():
        assert_repo_durable_state(state, source_sha)
    repo_a_root = Path(servers[0]["repo_dir"])
    payload_entry = next(item for item in
                         after_publish[REPO_A_ID]["payload_files"]
                         if item["sha256"] == source_sha and
                         item["path"].endswith("/" + source_sha))
    manifest_entry = next(item for item in
                          after_publish[REPO_A_ID]["repo_manifests"]["entries"]
                          if item["sha256"] == source_sha and
                          item["objectType"] == "artifact-v2-bytes")
    payload = repo_a_root / payload_entry["path"]
    manifest = repo_a_root / manifest_entry["path"]
    original_payload = payload.read_bytes()
    original_manifest = manifest.read_bytes()
    if (len(original_payload) != 4401 or
            hashlib.sha256(original_payload).hexdigest() != source_sha):
        raise RuntimeError("A19 did not create the intended two-range payload")
    reports = []
    traces = []

    def write_synced(path: Path, data: bytes) -> None:
        with path.open("wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())

    def probe(label: str, expected_a: str) -> Dict[str, Any]:
        if (servers[0]["proc"].poll() is not None or
                servers[1]["proc"].poll() is not None):
            raise RuntimeError("A19 Repo went offline during a storage-only fault")
        status_before = [item for item in
                         repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
                         if item["operation"] == "STATUS" and
                         item["requester"] == CONSUMER]
        report_path = work / (label + ".json")
        child = run_child(
            ndn, output, processes, CONSUMER_HOST, label,
            [str(repo_client), "--mode", "availability", "--handle", str(handle),
             "--probe-kind", "metadata", "--expect",
             "metadata-healthy" if expected_a == "PRESENT" else "missing",
             "--target-repo", REPO_A_ID, "--timeout-ms", "180000",
             "--report-out", str(report_path), "--user", CONSUMER] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            210, require_no_python=True)
        children.append(child)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        locations = {item["repoNode"]: item for item in report["locations"]}
        if (report.get("probeKind") != "metadata" or
                set(locations) != {REPO_A_ID, REPO_B_ID} or
                locations[REPO_A_ID]["reachability"] != "REACHABLE" or
                locations[REPO_A_ID]["objectPresence"] != expected_a or
                locations[REPO_B_ID]["reachability"] != "REACHABLE" or
                locations[REPO_B_ID]["objectPresence"] != "PRESENT" or
                any(item["integrity"] != "NOT_CHECKED" for item in
                    locations.values())):
            raise RuntimeError("A19 availability did not distinguish storage gap: " + label)
        status_after = [item for item in
                        repo_request_events(processes, {REPO_A_ID, REPO_B_ID})
                        if item["operation"] == "STATUS" and
                        item["requester"] == CONSUMER]
        deltas = {node: sum(item["repo_node"] == node for item in status_after) -
                        sum(item["repo_node"] == node for item in status_before)
                  for node in (REPO_A_ID, REPO_B_ID)}
        if any(count < 2 for count in deltas.values()):
            raise RuntimeError("A19 lacks authenticated current probes: " + label)
        if receipt_path.read_bytes() != receipt_bytes:
            raise RuntimeError("A19 storage fault changed immutable receipt")
        reports.append(report)
        traces.append({"label": label, "authenticated_status_delta": deltas})
        return report

    healthy_before = probe("spec193-a19-healthy-before", "PRESENT")
    try:
        manifest.unlink()
        payload.unlink()
        if manifest.exists() or payload.exists():
            raise RuntimeError("A19 object removal did not create a storage gap")
        absent = probe("spec193-a19-object-absent", "ABSENT")
    finally:
        write_synced(manifest, original_manifest)
        write_synced(payload, original_payload)
    if (digest(manifest) != hashlib.sha256(original_manifest).hexdigest() or
            digest(payload) != source_sha):
        raise RuntimeError("A19 object restoration changed durable bytes")
    try:
        with payload.open("r+b") as stream:
            stream.truncate(4400)
            stream.flush()
            os.fsync(stream.fileno())
        if (not manifest.exists() or payload.stat().st_size != 4400 or
                payload.read_bytes() != original_payload[:4400]):
            raise RuntimeError("A19 missing tail range fault was not installed")
        partial = probe("spec193-a19-tail-range-partial", "PARTIAL")
    finally:
        write_synced(payload, original_payload)
    healthy_after = probe("spec193-a19-healthy-after", "PRESENT")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if (digest(payload) != source_sha or
            digest(manifest) != hashlib.sha256(original_manifest).hexdigest() or
            any(repo_artifact_inventory(after[node]) !=
                repo_artifact_inventory(after_publish[node]) for node in after)):
        raise RuntimeError("A19 recovered artifact inventory differs from original")
    return {"case_id": "A19", "status": "PASS", "children": children,
            "source_sha256": source_sha,
            "receipt_sha256": hashlib.sha256(receipt_bytes).hexdigest(),
            "original_payload_bytes": len(original_payload),
            "truncated_payload_bytes": 4400,
            "reports": [healthy_before, absent, partial, healthy_after],
            "authenticated_status_traces": traces,
            "repo_state_before": before, "repo_state_after_publish": after_publish,
            "repo_state_after_restore": after}


def run_spec193_repo_offline_availability(ndn, output: Path,
                                         processes: List[Dict[str, Any]],
                                         environments: Dict[str, Dict[str, str]],
                                         repo_client: Path,
                                         servers: List[Dict[str, Any]],
                                         trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A20 requires two Persistent Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "a20-offline-source.bin"
    source.write_bytes(bytes((index * 47 + 23) % 256 for index in range(2048)))
    source_sha = digest(source)
    handle = work / "a20-handle.json"
    receipt_path = work / "a20-receipt.json"
    destination = work / "a20-existing-destination.bin"
    destination_original = b"A20 destination must survive every offline fetch\x00v1"
    destination.write_bytes(destination_original)
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    children = [run_child(
        ndn, output, processes, APP_HOST, "spec193-a20-publish-all",
        [str(repo_client), "--mode", "publish", "--source", str(source),
         "--handle-out", str(handle), "--receipt-out", str(receipt_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        900, require_no_python=True)]
    receipt_bytes = receipt_path.read_bytes()
    receipt = json.loads(receipt_bytes)
    if (receipt.get("writeConsistency") != "ALL" or
            receipt.get("achievedReplicas") != 2 or
            {item["repoNode"]: item["state"] for item in
             receipt.get("replicaCommits", [])} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"}):
        raise RuntimeError("A20 setup did not commit two selected Repos")
    after_publish = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for state in after_publish.values():
        assert_repo_durable_state(state, source_sha)

    def nfd_status(server: Dict[str, Any]) -> Dict[str, str]:
        host = server["host"]
        command = "NDN_CLIENT_TRANSPORT=unix:///run/nfd/{}.sock nfdc status".format(host)
        status = checked_node_cmd(ndn.net[host], command)
        start = re.search(r"\bstart(?:Time| time)\s*[=:]\s*(\S+)",
                          status, re.IGNORECASE)
        if start is None:
            raise RuntimeError("A20 NFD status lacked a start-time epoch: " + host)
        return {"host": host, "start_time": start.group(1),
                "status_sha256": hashlib.sha256(status.encode()).hexdigest()}

    nfd_before = {item["repo_node"]: nfd_status(item) for item in servers}
    reports = []

    def probe(label: str, expectation: str, timeout_ms: int,
              target_repo: str = REPO_A_ID,
              extra: Tuple[str, ...] = ()) -> Dict[str, Any]:
        path = work / (label + ".json")
        child = run_child(
            ndn, output, processes, CONSUMER_HOST, label,
            [str(repo_client), "--mode", "availability", "--handle", str(handle),
             "--probe-kind", "metadata", "--expect", expectation,
             "--target-repo", target_repo, "--timeout-ms", str(timeout_ms),
             "--report-out", str(path), "--user", CONSUMER] + list(extra) + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            max(90, timeout_ms // 1000 + 30), require_no_python=True)
        children.append(child)
        report = json.loads(path.read_text(encoding="utf-8"))
        locations = {item["repoNode"]: item for item in report["locations"]}
        if report.get("probeKind") != "metadata" or \
                set(locations) != {REPO_A_ID, REPO_B_ID}:
            raise RuntimeError("A20 availability report lost selected locations")
        reports.append(report)
        return locations

    healthy = probe("spec193-a20-healthy-before", "metadata-healthy", 180000)
    stopped_a = stop_repo_server(servers[0])
    if servers[0]["proc"].poll() is None:
        raise RuntimeError("A20 Repo A remained online after stop")
    nfd_single = {item["repo_node"]: nfd_status(item) for item in servers}
    single = probe("spec193-a20-one-repo-offline", "unreachable", 30000)
    if (single[REPO_A_ID]["reachability"] != "UNREACHABLE" or
            single[REPO_A_ID]["objectPresence"] != "UNKNOWN" or
            single[REPO_A_ID]["integrity"] != "NOT_CHECKED" or
            single[REPO_B_ID]["objectPresence"] == "ABSENT"):
        raise RuntimeError("A20 one-offline report confused absence with reachability")
    single_b_first = probe(
        "spec193-a20-one-offline-b-first", "reachable-target", 30000,
        target_repo=REPO_B_ID,
        extra=("--probe-first-repo", REPO_B_ID))
    if (single_b_first[REPO_B_ID]["reachability"] != "REACHABLE" or
            single_b_first[REPO_B_ID]["objectPresence"] != "PRESENT" or
            single_b_first[REPO_A_ID]["objectPresence"] != "UNKNOWN"):
        raise RuntimeError("A20 live Repo B was not independently reachable")
    stopped_b = stop_repo_server(servers[1])
    if servers[1]["proc"].poll() is None:
        raise RuntimeError("A20 Repo B remained online after stop")
    nfd_all = {item["repo_node"]: nfd_status(item) for item in servers}
    all_down = probe("spec193-a20-all-repos-offline", "unreachable", 30000)
    if any(item["reachability"] == "REACHABLE" or
           item["objectPresence"] != "UNKNOWN" or
           item["integrity"] != "NOT_CHECKED" for item in all_down.values()):
        raise RuntimeError("A20 all-offline report claimed current object availability")
    child = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-a20-all-down-fetch",
        [str(repo_client), "--mode", "all-down-fetch", "--handle", str(handle),
         "--destination", str(destination), "--timeout-ms", "7000",
         "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        60, require_no_python=True)
    children.append(child)
    attempts = [line for line in read_log(Path(child["log"])).splitlines()
                if line.startswith("SPEC193_FETCH_ATTEMPT ")]
    if (len(attempts) != 2 or
            any("outcome=FAILED" not in line or
                "failureClass=timeout" not in line for line in attempts) or
            destination.read_bytes() != destination_original):
        raise RuntimeError("A20 all-offline fetch changed destination or failure class")
    if {item["repo_node"]: repo_artifact_inventory(
            repo_database_state(item["repo_dir"])) for item in servers} != \
            {node: repo_artifact_inventory(state)
             for node, state in after_publish.items()} or \
            receipt_path.read_bytes() != receipt_bytes:
        raise RuntimeError("A20 offline probes changed durable receipt or payload")
    restart = restart_repo_servers(
        ndn, output, processes, environments, servers, "a20-return")
    nfd_after = {item["repo_node"]: nfd_status(item) for item in servers}
    if any(nfd_before[node]["start_time"] != snapshot[node]["start_time"]
           for node in nfd_before for snapshot in
           (nfd_single, nfd_all, nfd_after)):
        raise RuntimeError("A20 NFD restarted during Repo-only outage")
    if (any(new == old for new, old in zip(
            restart["restarted_pids"], (stopped_a["pid"], stopped_b["pid"]))) or
            any(item["proc"].poll() is not None for item in servers)):
        raise RuntimeError("A20 Repo process epoch did not advance on recovery")
    time.sleep(3)
    recovered = probe("spec193-a20-recovered", "metadata-healthy", 180000)
    if any(item["reachability"] != "REACHABLE" or
           item["objectPresence"] != "PRESENT" for item in recovered.values()):
        raise RuntimeError("A20 restored Repo did not recover current availability")
    return {"case_id": "A20", "status": "PASS", "children": children,
            "source_sha256": source_sha,
            "receipt_sha256": hashlib.sha256(receipt_bytes).hexdigest(),
            "stopped_repos": [stopped_a, stopped_b], "restart": restart,
            "nfd_status_before": nfd_before,
            "nfd_status_one_offline": nfd_single,
            "nfd_status_all_offline": nfd_all,
            "nfd_status_after": nfd_after,
            "availability_reports": reports,
            "all_down_fetch_attempts": attempts,
            "destination_sha256": digest(destination),
            "repo_state_before": before,
            "repo_state_after_publish": after_publish,
            "repo_state_after_recovery": {item["repo_node"]:
                repo_database_state(item["repo_dir"]) for item in servers}}


def run_spec193_three_repo_failover(ndn, output: Path,
                                    processes: List[Dict[str, Any]],
                                    environments: Dict[str, Dict[str, str]],
                                    repo_client: Path, repo_server: Path,
                                    servers: List[Dict[str, Any]],
                                    trust_schema: Path,
                                    identity_certificates: Dict[str, str]) -> Dict[str, Any]:
    if len(servers) != 2 or REPO_C_HOST != "seattle" or \
            len({APP_HOST, CONSUMER_HOST, servers[0]["host"],
                 servers[1]["host"], REPO_C_HOST, CONTROLLER_HOST}) != 6:
        raise RuntimeError("A21 requires six independent hosts and Repo C")
    repo_c = start_repo_server(
        ndn, output, processes, environments, repo_server, trust_schema,
        identity_certificates, REPO_C_HOST, REPO_C_PROVIDER, REPO_C_ID,
        "cpp-repo-C", "spec193-a21-repo-C")
    time.sleep(3)
    all_servers = servers + [repo_c]
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in all_servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "a21-three-repo-source.bin"
    source.write_bytes(bytes((index * 61 + 17) % 256 for index in range(4096)))
    source_sha = digest(source)
    handle = work / "a21-handle.json"
    receipt_path = work / "a21-receipt.json"
    destination = work / "a21-consumer-fetched.bin"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER,
            "--repo-c", REPO_C_ID, "--provider-c", REPO_C_PROVIDER]
    children = [run_child(
        ndn, output, processes, APP_HOST, "spec193-a21-publish-all",
        [str(repo_client), "--mode", "publish", "--source", str(source),
         "--handle-out", str(handle), "--receipt-out", str(receipt_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        1200, require_no_python=True)]
    assert_three_repo_commits(receipt_path)
    receipt_bytes = receipt_path.read_bytes()
    after_publish = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in all_servers}
    for state in after_publish.values():
        assert_repo_durable_state(state, source_sha)
    repo_nodes = set(after_publish)
    before_alias_requests = repo_request_events(processes, repo_nodes)
    children.append(run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-a21-reject-provider-alias",
        [str(repo_client), "--mode", "reject-provider-alias",
         "--handle", str(handle), "--destination",
         str(work / "a21-alias-must-not-fetch.bin"), "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        45, require_no_python=True))
    if repo_request_events(processes, repo_nodes) != before_alias_requests:
        raise RuntimeError("A21 duplicate Provider alias emitted a Repo request")
    stopped = [stop_repo_server(servers[0])]
    ready = work / "a21-first-fetch-complete.json"
    release = work / "a21-all-offline-release"
    label = "spec193-a21-failover-cooldown"
    child_record = spawn(
        ndn, output, processes, CONSUMER_HOST, label,
        [str(repo_client), "--mode", "failover-cooldown", "--handle", str(handle),
         "--destination", str(destination), "--failed-provider", REPO_A_PROVIDER,
         "--phase-ready", str(ready), "--phase-continue", str(release),
         "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True))
    assert_no_python_mapped(child_record)
    first_phase = wait_for_json_file(ready, child_record, 90, label)
    if first_phase != {"phase": "first-fetch-complete"} or \
            digest(destination) != source_sha:
        raise RuntimeError("A21 first failover did not deliver the committed bytes")
    stopped.extend([stop_repo_server(servers[1]), stop_repo_server(repo_c)])
    offline_capture_offsets = {
        host: (output / "pcap" / (host + ".pcap")).stat().st_size
        for host in (CONSUMER_HOST, CONTROLLER_HOST)}
    release.write_text("all selected Repo processes stopped\n", encoding="utf-8")
    child = finish_spawned_child(child_record, 60)
    children.append(child)
    attempts = [line for line in read_log(Path(child["log"])).splitlines()
                if line.startswith("SPEC193_A21_")]
    if (sum(line.startswith("SPEC193_A21_FIRST_ATTEMPT ") for line in attempts) < 2 or
            sum(line.startswith("SPEC193_A21_SECOND_ATTEMPT ") for line in attempts) != 3 or
            not any("provider=" + REPO_A_PROVIDER in line and "cooled=1" in line
                    for line in attempts if line.startswith("SPEC193_A21_SECOND_ATTEMPT ")) or
            digest(destination) != source_sha or
            receipt_path.read_bytes() != receipt_bytes):
        raise RuntimeError("A21 fetch attempt, cooldown, or immutable receipt oracle failed")
    after_outage = {item["repo_node"]: repo_database_state(item["repo_dir"])
                    for item in all_servers}
    if {node: repo_artifact_inventory(state) for node, state in after_outage.items()} != \
            {node: repo_artifact_inventory(state)
             for node, state in after_publish.items()}:
        raise RuntimeError("A21 failover changed committed Repo state")
    restart = restart_repo_servers(
        ndn, output, processes, environments, all_servers, "a21-return")
    time.sleep(3)
    return {"case_id": "A21", "status": "PASS", "children": children,
            "source_sha256": source_sha, "destination_sha256": digest(destination),
            "receipt_sha256": hashlib.sha256(receipt_bytes).hexdigest(),
            "repo_state_before": before,
            "repo_state_after_publish": after_publish,
            "repo_state_after_outage": after_outage,
            "repo_state_after_recovery": {item["repo_node"]:
                repo_database_state(item["repo_dir"]) for item in all_servers},
            "stopped_repos": stopped, "restart": restart,
            "first_phase": first_phase, "fetch_attempts": attempts,
            "alias_authenticated_requests": 0,
            "offline_capture_offsets": offline_capture_offsets}


def run_spec193_old_handle_new_location(ndn, output: Path,
                                        processes: List[Dict[str, Any]],
                                        repo_client: Path,
                                        servers: List[Dict[str, Any]],
                                        trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A22 requires two Persistent Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "a22-same-reference-source.bin"
    source.write_bytes(bytes((index * 41 + 29) % 256 for index in range(3072)))
    source_sha = digest(source)
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    children = []
    handles = []
    receipts = []
    for ordinal in (1, 2):
        handle = work / "a22-handle-{}.json".format(ordinal)
        receipt = work / "a22-receipt-{}.json".format(ordinal)
        children.append(run_child(
            ndn, output, processes, APP_HOST,
            "spec193-a22-publish-{}".format(ordinal),
            [str(repo_client), "--mode", "publish", "--source", str(source),
             "--handle-out", str(handle), "--receipt-out", str(receipt),
             "--user", USER] + base,
            node_environment(ndn.net[APP_HOST], REPO, no_python=True),
            900, require_no_python=True))
        ensure_two_repo_commits(receipt)
        handles.append(handle)
        receipts.append(receipt)
    old_handle_bytes = handles[0].read_bytes()
    old_receipt_bytes = receipts[0].read_bytes()
    old_handle = json.loads(old_handle_bytes)
    new_handle = json.loads(handles[1].read_text(encoding="utf-8"))
    old_commits = {item["repoNode"]: item for item in old_handle["replicaCommits"]}
    new_commits = {item["repoNode"]: item for item in new_handle["replicaCommits"]}
    if (old_handle["reference"] != new_handle["reference"] or
            set(old_commits) != {REPO_A_ID, REPO_B_ID} or
            set(new_commits) != set(old_commits) or
            any(new_commits[node]["locationVersion"] <=
                old_commits[node]["locationVersion"] or
                new_commits[node]["receiptId"] == old_commits[node]["receiptId"] or
                new_commits[node]["operationId"] == old_commits[node]["operationId"]
                for node in old_commits)):
        raise RuntimeError("A22 second commit did not advance current locations")
    after_publish = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for node, state in after_publish.items():
        assert_repo_durable_state(state, source_sha)
        if (repo_table_row_counts(state)["artifact_persistent_receipts"] !=
                repo_table_row_counts(before[node])["artifact_persistent_receipts"] + 2):
            raise RuntimeError("A22 Repo lacks two immutable persistent receipts: " + node)

    reports = []

    def probe(label: str, expectation: str) -> Dict[str, Any]:
        path = work / (label + ".json")
        children.append(run_child(
            ndn, output, processes, CONSUMER_HOST, label,
            [str(repo_client), "--mode", "availability", "--handle", str(handles[0]),
             "--probe-kind", "metadata", "--expect", expectation,
             "--target-repo", REPO_A_ID, "--timeout-ms", "180000",
             "--report-out", str(path), "--user", CONSUMER] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            240, require_no_python=True))
        report = json.loads(path.read_text(encoding="utf-8"))
        locations = {item["repoNode"]: item for item in report["locations"]}
        if set(locations) != set(old_commits):
            raise RuntimeError("A22 old-handle report lost a selected Repo")
        reports.append(report)
        return locations

    current = probe("spec193-a22-old-handle-new-location", "metadata-healthy")
    for node, location in current.items():
        if (location["locationVersion"] != new_commits[node]["locationVersion"] or
                location["operationId"] != new_commits[node]["operationId"] or
                location["receiptId"] != new_commits[node]["receiptId"]):
            raise RuntimeError("A22 old handle did not see the new current LOCATION: " + node)
    receipt_query = work / "a22-old-receipt-queried.json"
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a22-old-receipt-query",
        [str(repo_client), "--mode", "receipt-query", "--handle", str(handles[0]),
         "--receipt", str(receipts[0]), "--receipt-out", str(receipt_query),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        180, require_no_python=True))
    if receipt_query.read_bytes() != old_receipt_bytes:
        raise RuntimeError("A22 historical receipt query changed old receipt bytes")

    location_db = Path(servers[0]["database"]).with_name("repo.sqlite.artifact-v2.sqlite")
    with sqlite3.connect(str(location_db)) as connection:
        rows = connection.execute(
            "SELECT rowid, committed_at_ms, expires_at_ms, location_version "
            "FROM artifact_locations WHERE repo_node=? AND provider_identity=?",
            (REPO_A_ID, REPO_A_PROVIDER)).fetchall()
    if len(rows) != 1 or rows[0][3] != new_commits[REPO_A_ID]["locationVersion"]:
        raise RuntimeError("A22 expected one current Repo A location row")
    rowid, committed_at_ms, original_expiry_ms, _ = rows[0]
    expired_at_ms = int(time.time() * 1000) - 1
    if expired_at_ms <= committed_at_ms or original_expiry_ms <= expired_at_ms:
        raise RuntimeError("A22 cannot make a valid but expired current hint")
    try:
        with sqlite3.connect(str(location_db)) as connection:
            connection.execute("UPDATE artifact_locations SET expires_at_ms=? WHERE rowid=?",
                               (expired_at_ms, rowid))
        expired = probe("spec193-a22-expired-hint", "unknown-target")
        if (expired[REPO_A_ID]["objectPresence"] != "UNKNOWN" or
                expired[REPO_B_ID]["reachability"] != "REACHABLE" or
                expired[REPO_B_ID]["objectPresence"] != "PRESENT"):
            raise RuntimeError("A22 expired hint confused UNKNOWN with absence")
        destination = work / "a22-old-handle-fallback.bin"
        children.append(run_child(
            ndn, output, processes, CONSUMER_HOST,
            "spec193-a22-expired-hint-fetch-from-b",
            [str(repo_client), "--mode", "failover-fetch", "--handle", str(handles[0]),
             "--destination", str(destination), "--failed-provider", REPO_A_PROVIDER,
             "--timeout-ms", "15000", "--user", CONSUMER] + base,
            node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
            90, require_no_python=True))
        if digest(destination) != source_sha:
            raise RuntimeError("A22 old-handle fallback did not preserve content bytes")
    finally:
        with sqlite3.connect(str(location_db)) as connection:
            connection.execute("UPDATE artifact_locations SET expires_at_ms=? WHERE rowid=?",
                               (original_expiry_ms, rowid))
    restored = probe("spec193-a22-restored-hint", "metadata-healthy")
    if (handles[0].read_bytes() != old_handle_bytes or
            receipts[0].read_bytes() != old_receipt_bytes or
            receipt_query.read_bytes() != old_receipt_bytes or
            any(restored[node]["locationVersion"] !=
                new_commits[node]["locationVersion"] for node in restored)):
        raise RuntimeError("A22 restoration changed old facts or current version")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if {node: repo_artifact_inventory(state) for node, state in after.items()} != \
            {node: repo_artifact_inventory(state)
             for node, state in after_publish.items()}:
        raise RuntimeError("A22 probes changed durable artifact inventory")
    return {"case_id": "A22", "status": "PASS", "children": children,
            "source_sha256": source_sha,
            "old_receipt_sha256": hashlib.sha256(old_receipt_bytes).hexdigest(),
            "old_handle_sha256": hashlib.sha256(old_handle_bytes).hexdigest(),
            "old_location_versions": {node: item["locationVersion"]
                                      for node, item in old_commits.items()},
            "new_location_versions": {node: item["locationVersion"]
                                      for node, item in new_commits.items()},
            "expired_repo": REPO_A_ID, "expired_at_ms": expired_at_ms,
            "availability_reports": reports,
            "repo_state_before": before,
            "repo_state_after_publish": after_publish,
            "repo_state_after_recovery": after}


def run_spec193_invalid_selected_set(ndn, output: Path,
                                     processes: List[Dict[str, Any]],
                                     repo_client: Path,
                                     servers: List[Dict[str, Any]],
                                     trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("selected-set rejection requires two Repo sentinels")
    repo_nodes = {item["repo_node"] for item in servers}
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    writes_before = repo_store_events(processes, repo_nodes)
    source = output / "artifacts/a09-rejected-source.bin"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(bytes((index * 19 + 5) % 256 for index in range(512)))
    child = run_child(
        ndn, output, processes, APP_HOST, "spec193-a09-invalid-selected-set",
        [str(repo_client), "--mode", "invalid-selected-set",
         "--source", str(source), "--group", GROUP,
         "--controller", CONTROLLER, "--authority", CONTROLLER,
         "--user", USER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
         "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        90, require_no_python=True)
    log = read_log(Path(child["log"]))
    labels = ("outside-seed", "duplicate-repo", "duplicate-provider",
              "wrong-prefix")
    for label in labels:
        if log.count("SPEC193_SELECTED_SET_REJECTED case=" + label) != 1:
            raise RuntimeError("A09 lacks exactly one rejection for " + label)
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if before != after or repo_store_events(processes, repo_nodes) != writes_before:
        raise RuntimeError("A09 invalid selected set changed a remote Repo")
    return {"case_id": "A09", "status": "PASS", "children": [child],
            "rejected_variants": list(labels), "repo_state_before": before,
            "repo_state_after": after,
            "authenticated_writes": repo_store_events(processes, repo_nodes)}


def run_spec193_three_selected(ndn, output: Path,
                               processes: List[Dict[str, Any]],
                               environments: Dict[str, Dict[str, str]],
                               repo_client: Path, repo_server: Path,
                               servers: List[Dict[str, Any]],
                               trust_schema: Path,
                               identity_certificates: Dict[str, str]) -> Dict[str, Any]:
    if len(servers) != 2 or REPO_C_HOST != "seattle" or REPO_D_HOST != "stanford":
        raise RuntimeError("A07 requires independent Repo C and D hosts")
    repo_c = start_repo_server(
        ndn, output, processes, environments, repo_server, trust_schema,
        identity_certificates, REPO_C_HOST, REPO_C_PROVIDER, REPO_C_ID,
        "cpp-repo-C", "spec193-repo-C")
    repo_d = start_repo_server(
        ndn, output, processes, environments, repo_server, trust_schema,
        identity_certificates, REPO_D_HOST, REPO_D_PROVIDER, REPO_D_ID,
        "cpp-repo-D", "spec193-repo-D")
    time.sleep(3)
    all_servers = servers + [repo_c, repo_d]
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in all_servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    repo_nodes = set(before)
    writes_before = repo_store_events(processes, repo_nodes)
    source = output / "artifacts/a07-three-selected-source.bin"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(bytes((index * 23 + 11) % 256 for index in range(2048)))
    handle = output / "artifacts/a07-three-selected-handle.json"
    receipt_path = output / "artifacts/a07-three-selected-receipt.json"
    destination = output / "artifacts/a07-consumer-fetched.bin"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER,
            "--repo-c", REPO_C_ID, "--provider-c", REPO_C_PROVIDER]
    publisher = run_child(
        ndn, output, processes, APP_HOST, "spec193-a07-three-selected-publish",
        [str(repo_client), "--mode", "publish", "--source", str(source),
         "--handle-out", str(handle), "--receipt-out", str(receipt_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        1200, require_no_python=True)
    assert_three_repo_commits(receipt_path)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if (receipt.get("writeConsistency") != "ALL" or
            receipt.get("requestedReplicas") != 3 or
            receipt.get("requiredAcks") != 3 or
            receipt.get("achievedReplicas") != 3 or
            {item.get("providerIdentity") for item in
             receipt.get("replicaCommits", [])} !=
            {REPO_A_PROVIDER, REPO_B_PROVIDER, REPO_C_PROVIDER}):
        raise RuntimeError("A07 receipt does not bind three selected Provider principals")
    after_publish = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in all_servers}
    source_sha = digest(source)
    for repo_node in (REPO_A_ID, REPO_B_ID, REPO_C_ID):
        old = before[repo_node]
        new = after_publish[repo_node]
        before_receipts = repo_table_row_counts(old)["artifact_persistent_receipts"]
        after_receipts = repo_table_row_counts(new)["artifact_persistent_receipts"]
        old_paths = {item["path"] for item in old["payload_files"]}
        new_payloads = [item for item in new["payload_files"]
                        if item["path"] not in old_paths and
                        item["sha256"] == source_sha]
        if after_receipts != before_receipts + 1 or len(new_payloads) != 1:
            raise RuntimeError("A07 selected Repo lacks durable receipt/payload: " + repo_node)
    if before[REPO_D_ID] != after_publish[REPO_D_ID] or repo_d["proc"].poll() is not None:
        raise RuntimeError("A07 unselected live Repo D changed state or exited")
    new_writes = repo_store_events(processes, repo_nodes)[len(writes_before):]
    if {item["repo_node"] for item in new_writes} != \
            {REPO_A_ID, REPO_B_ID, REPO_C_ID}:
        raise RuntimeError("A07 authenticated write set differs from selected set")
    consumer = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-a07-consumer-fetch",
        [str(repo_client), "--mode", "fetch", "--handle", str(handle),
         "--destination", str(destination), "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    after_fetch = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in all_servers}
    if digest(destination) != source_sha or after_fetch[REPO_D_ID] != before[REPO_D_ID]:
        raise RuntimeError("A07 consumer bytes or unselected Repo D state mismatch")
    return {"case_id": "A07", "status": "PASS", "children": [publisher, consumer],
            "source_sha256": source_sha, "fetched_sha256": digest(destination),
            "receipt": receipt, "repo_state_before": before,
            "repo_state_after_publish": after_publish,
            "repo_state_after_fetch": after_fetch,
            "authenticated_writes": new_writes,
            "unselected_repo_host": REPO_D_HOST,
            "unselected_repo_pid": repo_d["pid"]}


def run_spec193_partial_consistency(ndn, output: Path,
                                   processes: List[Dict[str, Any]],
                                   environments: Dict[str, Dict[str, str]],
                                   repo_client: Path, repo_server: Path,
                                   servers: List[Dict[str, Any]],
                                   trust_schema: Path,
                                   identity_certificates: Dict[str, str],
                                   consistency: str,
                                   reject_target: bool = False) -> Dict[str, Any]:
    if len(servers) != 2 or REPO_C_HOST != "seattle" or consistency not in {"ONE", "QUORUM"}:
        raise RuntimeError("A08 requires three independent Repos and ONE or QUORUM")
    if reject_target and consistency != "QUORUM":
        raise RuntimeError("A08 reject-target probe requires QUORUM")
    repo_c = start_repo_server(
        ndn, output, processes, environments, repo_server, trust_schema,
        identity_certificates, REPO_C_HOST, REPO_C_PROVIDER, REPO_C_ID,
        "cpp-repo-C", "spec193-repo-C")
    all_servers = servers + [repo_c]
    time.sleep(3)
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in all_servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    source = output / ("artifacts/a08-" + consistency.lower() + "-source.bin")
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(bytes((index * 29 + 13) % 256 for index in range(2048)))
    source_sha = digest(source)
    handle = source.with_name("a08-" + consistency.lower() + "-handle.json")
    receipt_path = source.with_name("a08-" + consistency.lower() + "-receipt.json")
    destination = source.with_name("a08-" + consistency.lower() + "-consumer.bin")
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER,
            "--repo-c", REPO_C_ID, "--provider-c", REPO_C_PROVIDER]
    writes_before = repo_store_events(processes, set(before))
    publisher = spawn(
        ndn, output, processes, APP_HOST,
        "spec193-a08-" + consistency.lower() + "-publish",
        [str(repo_client), "--mode", "publish", "--source", str(source),
         "--handle-out", str(handle), "--receipt-out", str(receipt_path),
         "--consistency", consistency, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True))
    assert_no_python_mapped(publisher)
    wait_log(servers[0],
             "service=" + SERVICE_PREFIX + "/STORE requester=" + USER,
             360)
    object_replacement = None
    if reject_target:
        object_server = REPO / "build/spec193-object-repo-server"
        object_dir = Path(ndn.net[REPO_B_HOST].params["params"]["homeDir"]) / \
                     "spec193-a08-object-reject"
        shutil.rmtree(object_dir, ignore_errors=True)
        object_database = object_dir / "repo.sqlite"
        object_argv = list(servers[1]["argv"])
        object_argv[0] = str(object_server)
        object_argv[object_argv.index("--database") + 1] = str(object_database)
        os.kill(servers[0]["pid"], signal.SIGSTOP)
        try:
            stopped = [stop_repo_server(servers[1])]
            object_replacement = spawn(
                ndn, output, processes, REPO_B_HOST,
                "cpp-repo-B-object-reject", object_argv,
                repo_server_environment(node_environment(
                    ndn.net[REPO_B_HOST], REPO, no_python=True),
                    "cpp-repo-B-object-reject"))
            object_replacement["repo_node"] = REPO_B_ID
            object_replacement["provider_identity"] = REPO_B_PROVIDER
            object_replacement["repo_dir"] = object_dir
            object_replacement["database"] = object_database
            wait_log(object_replacement, "Spec193 object Repo serving", 60)
            assert_no_python_mapped(object_replacement)
        finally:
            if servers[0]["proc"].poll() is None:
                os.kill(servers[0]["pid"], signal.SIGCONT)
    else:
        stopped = [stop_repo_server(servers[1])]
        if consistency == "ONE":
            stopped.append(stop_repo_server(repo_c))
    publisher_result = finish_spawned_child(publisher, 1200)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    replicas = {item.get("repoNode"): item for item in receipt.get("replicaCommits", [])}
    required = 1 if consistency == "ONE" else 2
    expected_committed = {REPO_A_ID} if consistency == "ONE" else {REPO_A_ID, REPO_C_ID}
    actual_committed = {repo_node for repo_node, item in replicas.items()
                        if item.get("state") == "COMMITTED"}
    if (receipt.get("writeConsistency") != consistency or
            receipt.get("requestedReplicas") != 3 or
            receipt.get("requiredAcks") != required or
            receipt.get("achievedReplicas") != required or
            receipt.get("placementState") != "SATISFIED" or
            set(replicas) != {REPO_A_ID, REPO_B_ID, REPO_C_ID} or
            actual_committed != expected_committed):
        raise RuntimeError("A08 receipt threshold or per-Repo outcome mismatch")
    if reject_target and (replicas[REPO_B_ID].get("state") != "FAILED" or
                          not replicas[REPO_B_ID].get("failureReason")):
        raise RuntimeError("A08 replacement was not recorded as a failed target")
    after_publish = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in all_servers}
    for repo_node in expected_committed:
        old, new = before[repo_node], after_publish[repo_node]
        receipt_delta = (repo_table_row_counts(new)["artifact_persistent_receipts"] -
                         repo_table_row_counts(old)["artifact_persistent_receipts"])
        old_paths = {item["path"] for item in old["payload_files"]}
        payloads = [item for item in new["payload_files"]
                    if item["path"] not in old_paths and item["sha256"] == source_sha]
        if receipt_delta != 1 or len(payloads) != 1:
            raise RuntimeError("A08 committed Repo lacks receipt/payload: " + repo_node)
    for repo_node in {REPO_A_ID, REPO_B_ID, REPO_C_ID} - expected_committed:
        if (repo_artifact_inventory(before[repo_node]) !=
                repo_artifact_inventory(after_publish[repo_node]) or
                any(item["sha256"] == source_sha for item in
                    after_publish[repo_node]["payload_files"])):
            raise RuntimeError("A08 failed/skipped Repo gained artifact state: " + repo_node)
    new_writes = repo_store_events(processes, set(before))[len(writes_before):]
    if not any(item["repo_node"] == REPO_A_ID for item in new_writes):
        raise RuntimeError("A08 seed lacks authenticated STORE")
    if reject_target and (object_replacement is None or
                          object_replacement["proc"].poll() is not None or
                          not any(item["repo_node"] == REPO_B_ID and
                                  item["requester"] == REPO_A_PROVIDER
                                  for item in new_writes)):
        raise RuntimeError("A08 rejected target did not receive the owner STORE")
    object_state = None
    if object_replacement is not None:
        object_state = repo_database_state(object_replacement["repo_dir"])
        assert_no_remote_artifacts(object_state, allow_missing_tables=True)
        if any(item["path"] != "repo.sqlite.store.authority.lock"
               for item in object_state["payload_files"]):
            raise RuntimeError("A08 rejected Repo retained payload bytes")
    consumer = run_child(
        ndn, output, processes, CONSUMER_HOST,
        "spec193-a08-" + consistency.lower() + "-consumer-fetch",
        [str(repo_client), "--mode", "fetch", "--handle", str(handle),
         "--destination", str(destination), "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True)
    if digest(destination) != source_sha:
        raise RuntimeError("A08 independent consumer bytes differ from source")
    return {"case_id": "A08_REJECT" if reject_target else "A08_" + consistency,
            "status": "PASS",
            "children": [publisher_result, consumer], "stopped_repos": stopped,
            "source_sha256": source_sha, "fetched_sha256": digest(destination),
            "receipt": receipt, "repo_state_before": before,
            "repo_state_after_publish": after_publish,
            "replacement_repo_state": object_state,
            "authenticated_writes": new_writes}


def run_spec193_operation_lifecycle(ndn, output: Path,
                                    processes: List[Dict[str, Any]],
                                    repo_client: Path,
                                    servers: List[Dict[str, Any]],
                                    trust_schema: Path,
                                    restart_owner: bool = False) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A10/A11 requires two independent Persistent Repos")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    writes_before = len(repo_store_events(processes, set(before)))
    source = output / "artifacts/a10-seed-source.bin"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(bytes((index * 29 + 13) % 256 for index in range(4401)))
    source_sha = digest(source)
    handle_path = output / "artifacts/a10-operation-handle.json"
    initial_status_path = output / "artifacts/a10-initial-status.json"
    final_status_path = output / "artifacts/a10-final-status.json"
    key = "spec193-a10-a11-seed-barrier-key"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    begin = spawn(
        ndn, output, processes, APP_HOST, "spec193-a10-operation-begin",
        [str(repo_client), "--mode", "operation-begin", "--source", str(source),
         "--handle-out", str(handle_path), "--key", key, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True))
    assert_no_python_mapped(begin)
    wait_log(servers[0], "service=" + SERVICE_PREFIX + "/STORE requester=" + USER,
             360)
    stopped_b = stop_repo_server(servers[1])
    wait_for_repo_payload(servers[0]["repo_dir"], source_sha,
                          source.stat().st_size, begin, 390)
    begin_result = finish_spawned_child(begin, 90)
    handle = json.loads(handle_path.read_text(encoding="utf-8"))
    if handle.get("seed", {}).get("repoNode") != REPO_A_ID:
        raise RuntimeError("A10 handle does not bind the selected seed")
    after_seed = {item["repo_node"]: repo_database_state(item["repo_dir"])
                  for item in servers}
    assert_repo_durable_state(after_seed[REPO_A_ID], source_sha)
    assert_digest_absent(after_seed[REPO_B_ID], source_sha)
    counts = repo_table_row_counts(after_seed[REPO_A_ID])
    if (counts["artifact_publish_operations"] != 1 or
            counts["artifact_persistent_receipts"] != 1 or
            counts["artifact_publish_receipts"] != 0):
        raise RuntimeError("A10 seed barrier did not leave one accepted journal and seed receipt")

    initial_child = run_child(
        ndn, output, processes, APP_HOST, "spec193-a10-status-once",
        [str(repo_client), "--mode", "operation-status-once", "--handle",
         str(handle_path), "--status-out", str(initial_status_path),
         "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        90, require_no_python=True)
    initial = json.loads(initial_status_path.read_text(encoding="utf-8"))
    target_states = {item["target"]["repoNode"]: item["state"]
                     for item in initial["targets"]}
    if (initial.get("operationId") != handle.get("operationId") or
            initial.get("state") not in ("ACCEPTED", "IN_PROGRESS") or
            target_states.get(REPO_A_ID) != "COMMITTED" or
            target_states.get(REPO_B_ID) == "COMMITTED" or
            initial.get("receiptId")):
        raise RuntimeError("A10 initial status did not expose seed-only asynchronous progress")

    stopped_a = None
    old_a = servers[0]
    if restart_owner:
        stopped_a = stop_repo_server(old_a)
    old_b = servers[1]
    replacement = spawn(
        ndn, output, processes, old_b["host"], "cpp-repo-B-a10-resumed",
        old_b["argv"], repo_server_environment(node_environment(
            ndn.net[old_b["host"]], REPO, no_python=True),
            "cpp-repo-B-a10-resumed"))
    for field in ("repo_dir", "database", "repo_node", "provider_identity"):
        replacement[field] = old_b[field]
    wait_log(replacement, "Persistent Repo serving", 35)
    assert_no_python_mapped(replacement)
    servers[1] = replacement
    if restart_owner:
        resumed_owner = spawn(
            ndn, output, processes, old_a["host"], "cpp-repo-A-a12-resumed",
            old_a["argv"], repo_server_environment(node_environment(
                ndn.net[old_a["host"]], REPO, no_python=True),
                "cpp-repo-A-a12-resumed"))
        for field in ("repo_dir", "database", "repo_node", "provider_identity"):
            resumed_owner[field] = old_a[field]
        wait_log(resumed_owner, "Persistent Repo serving", 35)
        assert_no_python_mapped(resumed_owner)
        if resumed_owner["pid"] == old_a["pid"]:
            raise RuntimeError("A12 owner restart reused the old process epoch")
        servers[0] = resumed_owner
    time.sleep(3)
    final_child = run_child(
        ndn, output, processes, APP_HOST, "spec193-a10-status-terminal",
        [str(repo_client), "--mode", "operation-status", "--handle",
         str(handle_path), "--status-out", str(final_status_path),
         "--wait-ms", "300000", "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        330, require_no_python=True)
    final = json.loads(final_status_path.read_text(encoding="utf-8"))
    final_targets = {item["target"]["repoNode"]: item for item in final["targets"]}
    if (final.get("operationId") != handle.get("operationId") or
            final.get("state") != "COMMITTED" or
            {node: item["state"] for node, item in final_targets.items()} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"} or
            not final.get("receiptId")):
        raise RuntimeError("A10 terminal status lacks two bound commits")
    after_final = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    assert_repo_durable_state(after_final[REPO_B_ID], source_sha)
    final_counts = repo_table_row_counts(after_final[REPO_A_ID])
    replica_counts = repo_table_row_counts(after_final[REPO_B_ID])
    if (final_counts["artifact_publish_operations"] != 1 or
            final_counts["artifact_publish_receipts"] != 1 or
            final_counts["artifact_persistent_receipts"] != 1 or
            replica_counts["artifact_persistent_receipts"] != 1):
        raise RuntimeError("A10 terminal journal or aggregate receipt duplicated")
    replay = run_child(
        ndn, output, processes, APP_HOST, "spec193-a11-idempotent-replay",
        [str(repo_client), "--mode", "operation-replay", "--source", str(source),
         "--handle", str(handle_path), "--key", key, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        90, require_no_python=True)
    conflicting_source = output / "artifacts/a11-conflicting-source.bin"
    conflicting_source.write_bytes(b"same idempotency key with different content")
    conflict = run_child(
        ndn, output, processes, APP_HOST, "spec193-a11-conflict-rejected",
        [str(repo_client), "--mode", "operation-conflict", "--source",
         str(conflicting_source), "--key", key, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        90, require_no_python=True)
    after_replay = {item["repo_node"]: repo_database_state(item["repo_dir"])
                    for item in servers}
    if (any(repo_artifact_inventory(after_replay[node]) !=
            repo_artifact_inventory(after_final[node]) for node in after_final) or
            any(item["sha256"] == digest(conflicting_source)
                for item in after_replay[REPO_A_ID]["payload_files"])):
        raise RuntimeError("A11 replay/conflict mutated terminal receipt or stored changed bytes")
    new_writes = repo_store_events(processes, set(before))[writes_before:]
    if not any(item["repo_node"] == REPO_A_ID and item["requester"] == USER
               for item in new_writes) or not any(
                   item["repo_node"] == REPO_B_ID and
                   item["requester"] == REPO_A_PROVIDER for item in new_writes):
        raise RuntimeError("A10 seed or replica lacks authenticated STORE")
    return {"case_id": "A12" if restart_owner else "A10_A11", "status": "PASS",
            "children": [begin_result, initial_child, replay, conflict, final_child],
            "stopped_repo": stopped_b, "stopped_owner": stopped_a,
            "owner_pids": [old_a["pid"], servers[0]["pid"]],
            "source_sha256": source_sha,
            "handle": handle, "initial_status": initial, "final_status": final,
            "repo_state_before": before, "repo_state_after_seed": after_seed,
            "repo_state_after_replay": after_replay,
            "repo_state_after_final": after_final,
            "authenticated_writes": new_writes}


def run_spec193_archive_lifecycle(ndn, output: Path,
                                 processes: List[Dict[str, Any]],
                                 repo_client: Path,
                                 servers: List[Dict[str, Any]],
                                 trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A13 requires two independent Persistent Repos")
    repo_nodes = {item["repo_node"] for item in servers}
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    writes_before = len(repo_store_events(processes, repo_nodes))
    app_home = Path(ndn.net[APP_HOST].params["params"]["homeDir"])
    local_root = app_home / ("spec193-a13-inapp-" + output.name)
    if local_root.exists():
        raise RuntimeError("A13 refuses to reuse an In-App storage root")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    app_env = node_environment(ndn.net[APP_HOST], REPO, no_python=True)
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    children = []
    local_handles = []
    local_receipts = []
    sources = []
    for index, size in ((1, 2048), (2, 2053)):
        source = work / ("a13-source-{}.bin".format(index))
        source.write_bytes(bytes((offset * (17 + index) + index) % 256
                                 for offset in range(size)))
        handle_path = work / ("a13-inapp-handle-{}.json".format(index))
        receipt_path = work / ("a13-inapp-receipt-{}.json".format(index))
        children.append(run_child(
            ndn, output, processes, APP_HOST,
            "spec193-a13-inapp-publish-{}".format(index),
            [str(repo_client), "--mode", "inapp-publish", "--source", str(source),
             "--store-dir", str(local_root), "--handle-out", str(handle_path),
             "--receipt-out", str(receipt_path), "--user", USER],
            app_env, 30, require_no_python=True))
        local_handle = json.loads(handle_path.read_text(encoding="utf-8"))
        local_receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        if (local_handle.get("lifecycleMode") != "IN_APP" or
                local_receipt.get("lifecycleMode") != "IN_APP" or
                local_handle.get("reference") != local_receipt.get("reference") or
                local_handle["reference"].get("contentDigest") != digest(source)):
            raise RuntimeError("A13 local handle or receipt lacks In-App binding")
        sources.append(source)
        local_handles.append((handle_path, local_handle))
        local_receipts.append((receipt_path, local_receipt))
    local_receipt_hashes = [digest(item[0]) for item in local_receipts]
    after_inapp = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    if (after_inapp != before or
            len(repo_store_events(processes, repo_nodes)) != writes_before):
        raise RuntimeError("A13 In-App publication wrote to a remote Repo before archive")
    local_state = repo_database_state(local_root)
    local_commits = sum(db["tables"].get("artifact_local_commits", {}).get("rows", 0)
                        for db in local_state["databases"].values())
    if local_commits != 2:
        raise RuntimeError("A13 In-App store lacks two distinct local commits")

    operation_path = work / "a13-begin-archive-operation.json"
    status_path = work / "a13-begin-archive-status.json"
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a13-begin-archive",
        [str(repo_client), "--mode", "archive-begin", "--store-dir", str(local_root),
         "--handle", str(local_handles[0][0]), "--handle-out", str(operation_path),
         "--key", "spec193-a13-begin-archive-key", "--user", USER] + base,
        app_env, 420, require_no_python=True))
    operation = json.loads(operation_path.read_text(encoding="utf-8"))
    if operation.get("seed", {}).get("repoNode") != REPO_A_ID:
        raise RuntimeError("A13 beginArchive did not bind the selected seed")
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a13-begin-status",
        [str(repo_client), "--mode", "operation-status", "--handle",
         str(operation_path), "--status-out", str(status_path),
         "--wait-ms", "180000", "--user", USER] + base,
        app_env, 210, require_no_python=True))
    first_status = json.loads(status_path.read_text(encoding="utf-8"))
    if (first_status.get("operationId") != operation.get("operationId") or
            first_status.get("state") != "COMMITTED" or
            {item["target"]["repoNode"]: item["state"]
             for item in first_status["targets"]} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"}):
        raise RuntimeError("A13 beginArchive status lacks two durable commits")
    after_begin = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    for node in repo_nodes:
        assert_repo_durable_state(after_begin[node], digest(sources[0]))
        if repo_table_row_counts(after_begin[node])["artifact_persistent_receipts"] != 1:
            raise RuntimeError("A13 beginArchive duplicated a Repo receipt")

    persistent_handle_path = work / "a13-persistent-handle.json"
    persistent_receipt_path = work / "a13-persistent-receipt.json"
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a13-archive",
        [str(repo_client), "--mode", "archive", "--store-dir", str(local_root),
         "--handle", str(local_handles[1][0]), "--handle-out",
         str(persistent_handle_path), "--receipt-out", str(persistent_receipt_path),
         "--user", USER] + base,
        app_env, 420, require_no_python=True))
    persistent_handle = json.loads(persistent_handle_path.read_text(encoding="utf-8"))
    persistent_receipt = json.loads(persistent_receipt_path.read_text(encoding="utf-8"))
    if (persistent_handle.get("lifecycleMode") != "PERSISTENT" or
            persistent_receipt.get("lifecycleMode") != "PERSISTENT" or
            persistent_handle.get("reference") != local_handles[1][1]["reference"] or
            persistent_receipt.get("reference") != local_handles[1][1]["reference"] or
            set(persistent_handle.get("commitReceiptIds", [])) &
            set(local_handles[1][1].get("commitReceiptIds", [])) or
            persistent_receipt.get("achievedReplicas") != 2 or
            {item["repoNode"] for item in persistent_receipt.get("replicaCommits", [])
             if item.get("state") == "COMMITTED"} != repo_nodes):
        raise RuntimeError("A13 archive merged lifecycle handles or lacks two receipts")
    after_archive = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for node in repo_nodes:
        state = after_archive[node]
        counts = repo_table_row_counts(state)
        if (counts["artifact_persistent_receipts"] != 2 or
                not any(item["sha256"] == digest(sources[0])
                        for item in state["payload_files"]) or
                not any(item["sha256"] == digest(sources[1])
                        for item in state["payload_files"])):
            raise RuntimeError("A13 archived Repo lacks two distinct durable payloads")
    destination = work / "a13-consumer-fetched.bin"
    children.append(run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-a13-consumer-fetch",
        [str(repo_client), "--mode", "fetch", "--handle",
         str(persistent_handle_path), "--destination", str(destination),
         "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True),
        300, require_no_python=True))
    owner_destination = work / "a13-owner-verified.bin"
    children.append(run_child(
        ndn, output, processes, APP_HOST, "spec193-a13-owner-receipt-check",
        [str(repo_client), "--mode", "fetch", "--handle",
         str(persistent_handle_path), "--receipt", str(persistent_receipt_path),
         "--destination", str(owner_destination), "--user", USER] + base,
        app_env, 300, require_no_python=True))
    if (digest(destination) != digest(sources[1]) or
            digest(owner_destination) != digest(sources[1]) or
            [digest(item[0]) for item in local_receipts] != local_receipt_hashes or
            len(repo_store_events(processes, repo_nodes)) <= writes_before):
        raise RuntimeError("A13 consumer bytes, local receipts or network writes differ")
    return {"case_id": "A13", "status": "PASS", "children": children,
            "source_sha256": [digest(source) for source in sources],
            "consumer_sha256": digest(destination),
            "inapp_handles": [item[1] for item in local_handles],
            "inapp_receipts": [item[1] for item in local_receipts],
            "begin_operation": operation, "begin_status": first_status,
            "persistent_handle": persistent_handle,
            "persistent_receipt": persistent_receipt,
            "repo_state_before": before, "repo_state_after_inapp": after_inapp,
            "repo_state_after_begin": after_begin,
            "repo_state_after_archive": after_archive,
            "authenticated_writes": repo_store_events(processes, repo_nodes)[writes_before:]}


def run_spec193_source_failure(ndn, output: Path,
                              processes: List[Dict[str, Any]],
                              repo_client: Path,
                              servers: List[Dict[str, Any]],
                              trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A14 source failure requires two Persistent Repos")
    repo_nodes = {item["repo_node"] for item in servers}
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    requests_before = len(repo_request_events(processes, repo_nodes))
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "a14-source-before-truncation.bin"
    source_bytes = bytes((index * 31 + 19) % 256 for index in range(8801))
    source.write_bytes(source_bytes)
    source_sha = digest(source)
    failed_handle = work / "a14-failed-handle.json"
    retry_handle = work / "a14-retry-handle.json"
    final_status_path = work / "a14-retry-status.json"
    key = "spec193-a14-source-truncation-retry"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    first = spawn(
        ndn, output, processes, APP_HOST, "spec193-a14-source-truncated",
        [str(repo_client), "--mode", "operation-begin", "--source", str(source),
         "--handle-out", str(failed_handle), "--key", key, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True))
    assert_no_python_mapped(first)
    wait_log(servers[0], "service=" + SERVICE_PREFIX + "/STORE_RANGE requester=" + USER,
             360)
    if first["proc"].poll() is not None:
        raise RuntimeError("A14 publisher exited before source fault injection")
    os.kill(first["pid"], signal.SIGSTOP)
    staged = None
    try:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            staged = repo_database_state(servers[0]["repo_dir"])
            if any("/staging/" in item["path"]
                   for item in staged["payload_files"]):
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("A14 fault did not observe durable first-range staging")
        source.write_bytes(b"")
    finally:
        if first["proc"].poll() is None:
            os.kill(first["pid"], signal.SIGCONT)
    try:
        first_exit = first["proc"].wait(timeout=150)
    except subprocess.TimeoutExpired:
        first["proc"].kill()
        first["proc"].wait(timeout=5)
        raise RuntimeError("A14 truncated source did not fail within the bound")
    first["stream"].close()
    first_log = read_log(first["log"])
    if (first_exit == 0 or
            "repo-artifact-source-truncated:" not in first_log or
            failed_handle.exists()):
        raise RuntimeError("A14 truncated source returned a false accepted handle")
    requests_after_failure = repo_request_events(processes, repo_nodes)[requests_before:]
    if not any(item["repo_node"] == REPO_A_ID and
               item["operation"] == "ABORT_RANGES" and
               item["requester"] == USER for item in requests_after_failure):
        raise RuntimeError("A14 failed seed did not send authenticated ABORT_RANGES")
    after_failure = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for node, state in after_failure.items():
        counts = repo_table_row_counts(state)
        if (counts["objects"] != 0 or
                counts["artifact_persistent_receipts"] != 0 or
                counts["artifact_publish_receipts"] != 0 or
                any("/staging/" in item["path"] for item in state["payload_files"]) or
                any(item["sha256"] == source_sha for item in state["payload_files"])):
            raise RuntimeError("A14 failed source left a committed or staged artifact: " + node)
    if repo_artifact_inventory(after_failure[REPO_B_ID]) != \
            repo_artifact_inventory(before[REPO_B_ID]):
        raise RuntimeError("A14 target Repo changed before seed acceptance")

    source.write_bytes(source_bytes)
    retry = run_child(
        ndn, output, processes, APP_HOST, "spec193-a14-same-key-retry",
        [str(repo_client), "--mode", "operation-begin", "--source", str(source),
         "--handle-out", str(retry_handle), "--key", key, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        420, require_no_python=True)
    final_child = run_child(
        ndn, output, processes, APP_HOST, "spec193-a14-retry-status",
        [str(repo_client), "--mode", "operation-status", "--handle",
         str(retry_handle), "--status-out", str(final_status_path),
         "--wait-ms", "180000", "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        210, require_no_python=True)
    final = json.loads(final_status_path.read_text(encoding="utf-8"))
    if (final.get("state") != "COMMITTED" or
            {item["target"]["repoNode"]: item["state"]
             for item in final["targets"]} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"}):
        raise RuntimeError("A14 same-key retry lacks a two-Repo terminal commit")
    after_retry = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    for node, state in after_retry.items():
        assert_repo_durable_state(state, source_sha)
        counts = repo_table_row_counts(state)
        if (counts["artifact_persistent_receipts"] != 1 or
                any("/staging/" in item["path"] for item in state["payload_files"])):
            raise RuntimeError("A14 retry duplicated receipt or retained staged bytes")
    if (repo_table_row_counts(after_retry[REPO_A_ID])["artifact_publish_operations"] != 1 or
            repo_table_row_counts(after_retry[REPO_A_ID])["artifact_publish_receipts"] != 1):
        raise RuntimeError("A14 retry created another owner operation or aggregate receipt")
    return {"case_id": "A14_SOURCE", "status": "PASS",
            "children": [{"host": first["host"], "label": first["label"],
                          "pid": first["pid"], "exit_code": first_exit,
                          "expected_failure": True, "log": str(first["log"])},
                         retry, final_child],
            "source_sha256": source_sha, "staged_before_fault": staged,
            "repo_state_before": before, "repo_state_after_failure": after_failure,
            "repo_state_after_retry": after_retry, "final_status": final,
            "authenticated_requests": repo_request_events(processes, repo_nodes)[requests_before:]}


def run_spec193_target_timeout(ndn, output: Path,
                              processes: List[Dict[str, Any]],
                              repo_client: Path,
                              servers: List[Dict[str, Any]],
                              trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A14 target timeout requires two Persistent Repos")
    repo_nodes = {item["repo_node"] for item in servers}
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    first_source = work / "a14-timeout-first-source.bin"
    first_source.write_bytes(bytes((index * 37 + 5) % 256 for index in range(4401)))
    first_sha = digest(first_source)
    first_handle = work / "a14-timeout-first-handle.json"
    partial_path = work / "a14-timeout-partial-status.json"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    first = spawn(
        ndn, output, processes, APP_HOST, "spec193-a14-target-begin",
        [str(repo_client), "--mode", "operation-begin", "--source",
         str(first_source), "--handle-out", str(first_handle),
         "--key", "spec193-a14-target-timeout", "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True))
    assert_no_python_mapped(first)
    wait_log(servers[0], "service=" + SERVICE_PREFIX + "/STORE requester=" + USER,
             360)
    stopped_b = stop_repo_server(servers[1])
    begin_result = finish_spawned_child(first, 390)
    handle = json.loads(first_handle.read_text(encoding="utf-8"))
    terminal = spawn(
        ndn, output, processes, APP_HOST, "spec193-a14-target-partial",
        [str(repo_client), "--mode", "operation-status", "--handle",
         str(first_handle), "--status-out", str(partial_path),
         "--wait-ms", "180000", "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True))
    assert_no_python_mapped(terminal)
    try:
        terminal_exit = terminal["proc"].wait(timeout=210)
    except subprocess.TimeoutExpired:
        terminal["proc"].kill()
        terminal["proc"].wait(timeout=5)
        raise RuntimeError("A14 target outage did not reach a bounded terminal state")
    terminal["stream"].close()
    if terminal_exit != 2 or not partial_path.is_file():
        raise RuntimeError("A14 target outage did not expose terminal PARTIAL status")
    partial = json.loads(partial_path.read_text(encoding="utf-8"))
    target_states = {item["target"]["repoNode"]: item for item in partial["targets"]}
    if (partial.get("operationId") != handle.get("operationId") or
            partial.get("state") != "PARTIAL" or
            target_states[REPO_A_ID]["state"] != "COMMITTED" or
            target_states[REPO_B_ID]["state"] == "COMMITTED" or
            target_states[REPO_B_ID]["attemptCount"] == 0):
        raise RuntimeError("A14 ALL target timeout was reported as a false commit")
    after_partial = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    assert_repo_durable_state(after_partial[REPO_A_ID], first_sha)
    if (repo_artifact_inventory(after_partial[REPO_B_ID]) !=
            repo_artifact_inventory(before[REPO_B_ID])):
        raise RuntimeError("A14 offline target gained old operation artifact state")

    def receipt_wire(operation_id: str) -> str:
        state = repo_database_state(servers[0]["repo_dir"])
        databases = [servers[0]["repo_dir"] / relative
                     for relative, record in state["databases"].items()
                     if "artifact_publish_receipts" in record["tables"]]
        if len(databases) != 1:
            raise RuntimeError("A14 cannot identify the owner's artifact receipt database")
        database = databases[0]
        connection = sqlite3.connect(
            "file:{}?mode=ro".format(database.resolve().as_posix()), uri=True)
        try:
            row = connection.execute(
                "SELECT receipt_json FROM artifact_publish_receipts WHERE operation_id=?",
                (operation_id,)).fetchone()
        finally:
            connection.close()
        if row is None:
            raise RuntimeError("A14 terminal operation lacks aggregate receipt")
        return row[0]

    partial_wire = receipt_wire(handle["operationId"])
    partial_receipt = json.loads(partial_wire)
    replica_rows = {item["repoNode"]: item for item in partial_receipt["replicaCommits"]}
    if (partial_receipt.get("state") != "PARTIAL" or
            partial_receipt.get("writeConsistency") != "ALL" or
            partial_receipt.get("requiredAcks") != 2 or
            partial_receipt.get("achievedReplicas") != 1 or
            replica_rows[REPO_A_ID]["state"] != "COMMITTED" or
            replica_rows[REPO_B_ID]["state"] != "FAILED"):
        raise RuntimeError("A14 ALL aggregate receipt misstates target failure")

    old_b = servers[1]
    replacement = spawn(
        ndn, output, processes, old_b["host"], "cpp-repo-B-a14-restored",
        old_b["argv"], repo_server_environment(node_environment(
            ndn.net[old_b["host"]], REPO, no_python=True),
            "cpp-repo-B-a14-restored"))
    for field in ("repo_dir", "database", "repo_node", "provider_identity"):
        replacement[field] = old_b[field]
    wait_log(replacement, "Persistent Repo serving", 35)
    assert_no_python_mapped(replacement)
    servers[1] = replacement
    time.sleep(3)
    second_source = work / "a14-timeout-retry-source.bin"
    second_source.write_bytes(bytes((index * 41 + 9) % 256 for index in range(2049)))
    second_sha = digest(second_source)
    second_handle = work / "a14-timeout-retry-handle.json"
    final_path = work / "a14-timeout-retry-status.json"
    retry = run_child(
        ndn, output, processes, APP_HOST, "spec193-a14-target-recovery-begin",
        [str(repo_client), "--mode", "operation-begin", "--source",
         str(second_source), "--handle-out", str(second_handle),
         "--key", "spec193-a14-target-recovery-new-operation", "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        420, require_no_python=True)
    final_child = run_child(
        ndn, output, processes, APP_HOST, "spec193-a14-target-recovery-status",
        [str(repo_client), "--mode", "operation-status", "--handle",
         str(second_handle), "--status-out", str(final_path),
         "--wait-ms", "180000", "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        210, require_no_python=True)
    final = json.loads(final_path.read_text(encoding="utf-8"))
    if (final.get("state") != "COMMITTED" or
            {item["target"]["repoNode"]: item["state"]
             for item in final["targets"]} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"} or
            receipt_wire(handle["operationId"]) != partial_wire):
        raise RuntimeError("A14 recovery changed old receipt or failed new operation")
    after_recovery = {item["repo_node"]: repo_database_state(item["repo_dir"])
                      for item in servers}
    assert_repo_durable_state(after_recovery[REPO_B_ID], second_sha)
    if (any(item["sha256"] == first_sha
            for item in after_recovery[REPO_B_ID]["payload_files"]) or
            repo_table_row_counts(after_recovery[REPO_B_ID])["artifact_persistent_receipts"] != 1 or
            repo_table_row_counts(after_recovery[REPO_A_ID])["artifact_publish_receipts"] != 2):
        raise RuntimeError("A14 restored target acquired old data or duplicate receipts")
    return {"case_id": "A14_TARGET", "status": "PASS",
            "children": [begin_result,
                         {"host": terminal["host"], "label": terminal["label"],
                          "pid": terminal["pid"], "exit_code": terminal_exit,
                          "expected_failure": True, "log": str(terminal["log"])},
                         retry, final_child],
            "stopped_repo": stopped_b, "first_source_sha256": first_sha,
            "second_source_sha256": second_sha, "partial_status": partial,
            "partial_receipt": partial_receipt, "final_status": final,
            "repo_state_before": before, "repo_state_after_partial": after_partial,
            "repo_state_after_recovery": after_recovery}


def run_spec193_seed_outage(ndn, output: Path,
                           processes: List[Dict[str, Any]],
                           repo_client: Path,
                           servers: List[Dict[str, Any]],
                           trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("A14 seed outage requires two Persistent Repos")
    repo_nodes = {item["repo_node"] for item in servers}
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    requests_before = len(repo_request_events(processes, repo_nodes))
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "a14-seed-outage-source.bin"
    source.write_bytes(bytes((index * 43 + 7) % 256 for index in range(4401)))
    source_sha = digest(source)
    failed_handle = work / "a14-seed-outage-failed-handle.json"
    retry_handle = work / "a14-seed-outage-retry-handle.json"
    final_path = work / "a14-seed-outage-final-status.json"
    key = "spec193-a14-seed-outage-same-key"
    base = ["--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    first = spawn(
        ndn, output, processes, APP_HOST, "spec193-a14-seed-outage-begin",
        [str(repo_client), "--mode", "operation-begin", "--source", str(source),
         "--handle-out", str(failed_handle), "--key", key, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True))
    assert_no_python_mapped(first)
    wait_log(servers[0], "service=" + SERVICE_PREFIX + "/STORE requester=" + USER,
             360)
    stopped_a = stop_repo_server(servers[0])
    try:
        first_exit = first["proc"].wait(timeout=180)
    except subprocess.TimeoutExpired:
        first["proc"].kill()
        first["proc"].wait(timeout=5)
        raise RuntimeError("A14 unavailable seed did not fail within the bound")
    first["stream"].close()
    first_log = read_log(first["log"])
    if first_exit == 0 or failed_handle.exists():
        raise RuntimeError("A14 unavailable seed returned a false accepted handle")
    requests_after_failure = repo_request_events(processes, repo_nodes)[requests_before:]
    if not any(item["repo_node"] == REPO_A_ID and
               item["operation"] == "STORE" and
               item["requester"] == USER for item in requests_after_failure):
        raise RuntimeError("A14 seed outage did not follow authenticated intent")
    after_failure = {item["repo_node"]: repo_database_state(item["repo_dir"])
                     for item in servers}
    for node, state in after_failure.items():
        counts = repo_table_row_counts(state)
        if (counts["objects"] != 0 or
                counts["artifact_persistent_receipts"] != 0 or
                counts["artifact_publish_receipts"] != 0 or
                any(item["sha256"] == source_sha for item in state["payload_files"])):
            raise RuntimeError("A14 unavailable seed produced a false durable commit: " + node)
    if repo_artifact_inventory(after_failure[REPO_B_ID]) != \
            repo_artifact_inventory(before[REPO_B_ID]):
        raise RuntimeError("A14 target changed while seed was unavailable")

    old_a = servers[0]
    replacement = spawn(
        ndn, output, processes, old_a["host"], "cpp-repo-A-a14-restored",
        old_a["argv"], repo_server_environment(node_environment(
            ndn.net[old_a["host"]], REPO, no_python=True),
            "cpp-repo-A-a14-restored"))
    for field in ("repo_dir", "database", "repo_node", "provider_identity"):
        replacement[field] = old_a[field]
    wait_log(replacement, "Persistent Repo serving", 35)
    assert_no_python_mapped(replacement)
    servers[0] = replacement
    time.sleep(3)
    retry = run_child(
        ndn, output, processes, APP_HOST, "spec193-a14-seed-same-key-retry",
        [str(repo_client), "--mode", "operation-begin", "--source", str(source),
         "--handle-out", str(retry_handle), "--key", key, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        420, require_no_python=True)
    final_child = run_child(
        ndn, output, processes, APP_HOST, "spec193-a14-seed-retry-status",
        [str(repo_client), "--mode", "operation-status", "--handle",
         str(retry_handle), "--status-out", str(final_path),
         "--wait-ms", "180000", "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True),
        210, require_no_python=True)
    final = json.loads(final_path.read_text(encoding="utf-8"))
    if (final.get("state") != "COMMITTED" or
            {item["target"]["repoNode"]: item["state"]
             for item in final["targets"]} !=
            {REPO_A_ID: "COMMITTED", REPO_B_ID: "COMMITTED"}):
        raise RuntimeError("A14 restored seed did not complete same-key operation")
    after_retry = {item["repo_node"]: repo_database_state(item["repo_dir"])
                   for item in servers}
    for node, state in after_retry.items():
        assert_repo_durable_state(state, source_sha)
        if repo_table_row_counts(state)["artifact_persistent_receipts"] != 1:
            raise RuntimeError("A14 seed retry duplicated a Repo receipt")
    owner_counts = repo_table_row_counts(after_retry[REPO_A_ID])
    if (owner_counts["artifact_publish_operations"] != 1 or
            owner_counts["artifact_publish_receipts"] != 1):
        raise RuntimeError("A14 seed retry duplicated owner operation")
    return {"case_id": "A14_SEED", "status": "PASS",
            "children": [{"host": first["host"], "label": first["label"],
                          "pid": first["pid"], "exit_code": first_exit,
                          "expected_failure": True, "log": str(first["log"])},
                         retry, final_child],
            "stopped_seed": stopped_a, "old_seed_pid": old_a["pid"],
            "restored_seed_pid": replacement["pid"],
            "source_sha256": source_sha, "failure_log_tail": first_log[-1200:],
            "repo_state_before": before, "repo_state_after_failure": after_failure,
            "repo_state_after_retry": after_retry, "final_status": final,
            "authenticated_requests": repo_request_events(processes, repo_nodes)[requests_before:]}


def run_spec193_di_reuse(ndn, output: Path, processes: List[Dict[str, Any]],
                        environments: Dict[str, Dict[str, str]], repo_client: Path,
                        servers: List[Dict[str, Any]], trust_schema: Path,
                        runtime_config: Path, identities: Dict[str, Any]) -> Dict[str, Any]:
    work = output / "artifacts/p03"
    handles = work / "handles"
    handles.mkdir(parents=True, exist_ok=True)
    children = []
    expected = {}
    base = ["--group", GROUP, "--controller", CONTROLLER, "--authority", CONTROLLER,
            "--user", USER, "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    for slot in ("model", "input", "output"):
        source = work / (slot + ".bin")
        source.write_bytes(("Spec193 DI " + slot + " immutable reference").encode() + b"\x00")
        receipt = handles / (slot + "-receipt.json")
        children.append(run_child(ndn, output, processes, APP_HOST, "spec193-p03-publish-" + slot,
            [str(repo_client), "--mode", "publish", "--source", str(source),
             "--handle-out", str(handles / (slot + "-handle.json")), "--receipt-out", str(receipt)] + base,
            node_environment(ndn.net[APP_HOST], REPO, no_python=True),
            DI_PUBLISH_CHILD_TIMEOUT_S, require_no_python=True))
        ensure_two_repo_commits(receipt)
        expected[slot] = {"sha256": digest(source), "size": source.stat().st_size}
    peer = str(REPO / "tests/minindn/spec192_di_peer.py")
    references = work / "references.json"
    children.append(run_child(ndn, output, processes, CONSUMER_HOST, "spec193-p03-bindings",
        [sys.executable, peer, "--mode", "bindings", "--handle-dir", str(handles),
         "--output", str(references), "--cache-artifacts"], environments[CONSUMER_HOST], 30))
    def inventories():
        return {item["repo_node"]: repo_artifact_inventory(repo_database_state(item["repo_dir"])) for item in servers}
    baseline = inventories()
    cache = output / "di-shared-cache"
    def files(root):
        return {str(path.relative_to(root)): {"sha256": digest(path), "size": path.stat().st_size}
                for path in sorted(root.rglob("*")) if path.is_file()} if root.exists() else {}
    request = [sys.executable, peer, "--mode", "request", "--config", str(runtime_config),
               "--generated-policy-dir", str(output / "generated-policy"), "--group", GROUP,
               "--artifact-references", str(references)]
    cert_names = identities["additional_identities"]["certificate_names"]
    denied = []
    def deny(label):
        before_cache = files(cache)
        provider, ready = start_di_provider(ndn, output, processes, environments[APP_HOST],
            runtime_config, trust_schema, cert_names, DI_UNAUTHORIZED_ID,
            DI_UNAUTHORIZED_PROVIDER, DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY,
            "spec193-p04-provider-" + label, cache_dir=cache)
        child = run_child(ndn, output, processes, CONSUMER_HOST, "spec193-p04-request-" + label,
                          request + ["--expect-denied"], environments[CONSUMER_HOST], 100)
        children.append(child)
        result = json_event(child["log"], "SPEC192_DI_REQUEST_FAILED")
        rejection = json_event(provider["log"], "SPEC192_DI_REPO_FETCH_REJECTED")
        text = read_log(provider["log"])
        if (result.get("status") is not False or rejection.get("identity") != DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY or
                rejection.get("destination_exists") is not False or
                "repo-request-start-failed" not in rejection.get("error", "") or
                "Core rejected Provider request" not in rejection.get("error", "") or
                "SPEC192_DI_PROVIDER_ARTIFACTS" in text or "SPEC193_DI_REPO_FETCH_OK" in text or
                "SPEC193_DI_REPO_FETCH_BEGIN" not in text):
            raise RuntimeError("P04 lacks actual native admission refusal without completion/content")
        stopped = stop_daemon(provider)
        workspace = output / "di-provider-state" / DI_UNAUTHORIZED_ID
        if files(workspace) or files(cache) != before_cache or inventories() != baseline:
            raise RuntimeError("P04 denial changed workspace/cache/Repo content")
        denied.append({"phase": label, "provider_epoch": ready, "request": result,
                       "repo_rejection": rejection, "stop": stopped, "cache": before_cache})
    deny("cold")
    provider, ready = start_di_provider(ndn, output, processes, environments[APP_HOST],
        runtime_config, trust_schema, cert_names, DI_PROVIDER_ID, DI_PROVIDER,
        DI_REPO_CLIENT_IDENTITY, "spec193-p03-authorized-provider", cache_dir=cache)
    child = run_child(ndn, output, processes, CONSUMER_HOST, "spec193-p03-authorized-request",
                      request, environments[CONSUMER_HOST], 100)
    children.append(child)
    result = json_event(child["log"], "SPEC192_DI_REQUEST_PASS")
    observed = json_event(provider["log"], "SPEC192_DI_PROVIDER_ARTIFACTS")
    if result.get("provider") != DI_PROVIDER or result.get("artifacts") != expected or observed.get("artifacts") != expected:
        raise RuntimeError("P03 actual Provider output/reference bytes mismatch")
    successes = [json.loads(line.split("SPEC193_DI_REPO_FETCH_OK ", 1)[1])
                 for line in read_log(provider["log"]).splitlines() if "SPEC193_DI_REPO_FETCH_OK " in line]
    if ({(x["digest"], x["size"]) for x in successes} != {(x["sha256"], x["size"]) for x in expected.values()} or
            len(successes) != 3 or any(x["identity"] != DI_REPO_CLIENT_IDENTITY for x in successes)):
        raise RuntimeError("P03 lacks all three actual native Repo fetches")
    stopped = stop_daemon(provider)
    warm = files(cache)
    expected_cache = {("Spec192-DI-model" if slot == "model" else "spec193-" + slot) +
                      "-" + value["sha256"][:16] + "/" + slot + ".bin": value
                      for slot, value in expected.items()}
    if warm != expected_cache:
        raise RuntimeError("P03 cache files mismatch")
    if inventories() != baseline:
        raise RuntimeError("P03 read-only Repo inventory mismatch")
    deny("warm")
    if denied[0]["provider_epoch"]["boot_epoch"] == denied[1]["provider_epoch"]["boot_epoch"]:
        raise RuntimeError("P04 did not use fresh unauthorized Provider processes")
    return {"case_id": "P03_P04", "status": "PASS", "children": children, "expected": expected,
            "authorized_epoch": ready, "authorized_result": result, "authorized_stop": stopped,
            "authorized_fetches": successes, "denied": denied, "cache_inventory": warm,
            "repo_inventory": baseline, "references_sha256": digest(references),
            "reader_certificate": identities["target_public_certificates"][DI_REPO_CLIENT_IDENTITY]}


def run_spec193_legacy_import(ndn, output: Path, processes: List[Dict[str, Any]],
                             environments: Dict[str, Dict[str, str]], repo_client: Path,
                             servers: List[Dict[str, Any]], trust_schema: Path) -> Dict[str, Any]:
    import sqlite3
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    generator, source_root, destination = work / "p05-generator", work / "p05-source", work / "p05-imported"
    generator.mkdir()
    source_root.mkdir()
    database = generator / "legacy.sqlite"
    connection = sqlite3.connect(str(database))
    object_name = "/legacy/p05/" + output.name
    object_payload = bytes((index * 37 + 19) % 256 for index in range(521))
    cas_payload = bytes((index * 7 + 3) % 256 for index in range(263))
    object_sha, cas_sha = hashlib.sha256(object_payload).hexdigest(), hashlib.sha256(cas_payload).hexdigest()
    reference = {"logicalName": "/legacy/cas/p05", "digestAlgorithm": "sha256", "contentDigest": cas_sha,
                 "sizeBytes": len(cas_payload), "formatVersion": "artifact-manifest-v2",
                 "rootManifestName": "/legacy/manifests/p05", "publisherIdentity": "/legacy/publisher",
                 "policyEpoch": "legacy-epoch"}
    old_manifest = {"objectName": object_name, "objectType": "object", "sha256": object_sha,
        "size": len(object_payload), "segmentCount": 1, "replicationFactor": 3, "generation": 9,
        "parentGeneration": 4, "replicaNodes": ["old-a", "old-b"], "confirmedReplicaNodes": ["old-a"],
        "policyEpoch": "unverified-old-policy", "publicationIdentity": "old-operation",
        "protectionEpoch": "old-protection", "operationId": "old-operation",
        "writeConsistency": "QUORUM", "requiredWriteAcks": 2, "lifecycleState": "ACTIVE", "packetNames": []}
    compact = lambda value: json.dumps(value, separators=(",", ":"), sort_keys=True)
    try:
        connection.executescript((REPO / "tests/fixtures/spec192/legacy-python-repo-v12.sql").read_text())
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA wal_autocheckpoint=0")
        connection.execute("INSERT INTO objects(object_name,manifest_json,payload,payload_size,sha256,object_type,updated_at,hit_count) VALUES(?,?,?,?,?,?,1,0)",
                           (object_name, compact(old_manifest), object_payload, len(object_payload), object_sha, "object"))
        connection.execute("INSERT INTO artifact_active_catalog(logical_name,policy_epoch,format_version,digest_algorithm,artifact_digest,generation,operation_id,artifact_json,activated_at_ms) VALUES(?,?,'artifact-manifest-v2','sha256',?,5,?,?,100)",
                           (reference["logicalName"], reference["policyEpoch"], cas_sha, "p05-old-operation", compact(reference)))
        for index, (start, end) in enumerate((("FINALIZING", "COMMITTED"), ("COMMITTED", "ACTIVE"))):
            connection.execute("INSERT INTO artifact_lifecycle_journal(event_id,operation_id,artifact_digest,generation,from_state,to_state,event_time_ms,accepted,detail_json,error) VALUES(?,?,?,5,?,?,?,1,'{}','')",
                               ("p05-event-" + str(index), "p05-old-operation", cas_sha, start, end, 101 + index))
        connection.commit()
        for suffix in ("", "-wal", "-shm"):
            shutil.copy2(Path(str(database) + suffix), source_root / ("legacy.sqlite" + suffix))
    finally:
        connection.close()
    payload_path = source_root / "payloads/sha256" / cas_sha[:2] / cas_sha
    payload_path.parent.mkdir(parents=True)
    payload_path.write_bytes(cas_payload)
    reference_path = work / "p05-reference.json"
    reference_path.write_text(compact(reference))
    def inventory(root: Path, destination_data=False) -> dict:
        return {str(path.relative_to(root)): {"sha256": digest(path), "size": path.stat().st_size}
                for path in sorted(root.rglob("*")) if path.is_file() and
                not (destination_data and path.name.endswith(".authority.lock"))}
    before = inventory(source_root)
    if before["legacy.sqlite-wal"]["size"] == 0 or before["legacy.sqlite-shm"]["size"] == 0:
        raise RuntimeError("P05 frozen legacy bundle has no real WAL/SHM")
    import_id = "p05-" + output.name
    importer = REPO / "build/NDNSF-DistributedRepo/ndnsf-repo-import"
    argv = [str(importer), "--source-db", str(source_root / "legacy.sqlite"),
            "--source-payload-root", str(source_root), "--destination", str(destination), "--import-id", import_id,
            "--max-range-bytes", "256"]
    children = [run_child(ndn, output, processes, REPO_A_HOST, "spec193-p05-import", argv,
                         node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True), 90, require_no_python=True)]
    manifest_path = destination / "LegacyImportManifest.json"
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    if (manifest["state"] != "COMMITTED" or manifest["itemCount"] != 2 or
            manifest["byteCount"] != len(object_payload) + len(cas_payload) or
            {item["sourcePayloadSha256"] for item in manifest["items"]} != {object_sha, cas_sha} or
            {item["sourceKind"] for item in manifest["items"]} != {"python-repo-object", "python-artifact-catalog"} or
            inventory(source_root) != before):
        raise RuntimeError("P05 complete native import/source preservation oracle failed")
    imported_before = inventory(destination, True)
    children.append(run_child(ndn, output, processes, REPO_A_HOST, "spec193-p05-import-replay", argv,
                             node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True), 90, require_no_python=True))
    if manifest_path.read_bytes() != manifest_bytes or inventory(destination, True) != imported_before or inventory(source_root) != before:
        raise RuntimeError("P05 idempotent import changed source/destination")
    target = [{**servers[0], "argv": servers[0]["argv"] + ["--local-store-root", str(destination / "repo-core")]}]
    restarted = restart_repo_servers(ndn, output, processes, environments, target,
                                    "p05-import", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    untouched_b = repo_database_state(servers[1]["repo_dir"])
    fetched, returned_manifest = work / "p05-object-fetched.bin", work / "p05-network-manifest.json"
    children.append(run_child(ndn, output, processes, CONSUMER_HOST, "spec193-p05-network-reader",
        [str(repo_client), "--mode", "object-reopen-fetch", "--object-name", object_name,
         "--expected-sha256", object_sha, "--expected-size", str(len(object_payload)),
         "--destination", str(fetched), "--manifest-out", str(returned_manifest), "--user", CONSUMER,
         "--group", GROUP, "--controller", CONTROLLER, "--authority", CONTROLLER,
         "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
         "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True), 180, require_no_python=True))
    observed = json.loads(returned_manifest.read_text())
    if (digest(fetched) != object_sha or observed["generation"] != 9 or observed["replicationFactor"] != 1 or
            observed["replicaNodes"] or observed["confirmedReplicaNodes"] or observed["operationId"] or
            observed["policyEpoch"] or observed["publicationIdentity"] or observed["protectionEpoch"] or
            observed["writeConsistency"] != "ALL" or observed["lifecycleState"] != "COMMITTED"):
        raise RuntimeError("P05 imported object promoted old claims or changed bytes")
    cas_destination = work / "p05-cas-fetched.bin"
    children.append(run_child(ndn, output, processes, CONSUMER_HOST, "spec193-p05-cas-reader",
        [str(repo_client), "--mode", "legacy-inapp-read", "--store-dir", str(destination / "artifacts"),
         "--import-id", import_id, "--reference", str(reference_path), "--destination", str(cas_destination)],
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True), 60, require_no_python=True))
    after = inventory(source_root)
    if (digest(cas_destination) != cas_sha or after != before or inventory(destination, True) != imported_before or
            repo_database_state(servers[1]["repo_dir"]) != untouched_b):
        raise RuntimeError("P05 source/destination data or unselected B changed during reads")
    reports = {"legacy": {"calls": [{"operation": operation, "requestId": "/"}
                                    for operation in ("MANIFEST", "INVENTORY", "FETCH")]}}
    return {"case_id": "P05", "status": "PASS", "children": children, "reports": reports,
            "source_before": before, "source_after": after, "import_manifest": manifest,
            "importer_sha256": digest(importer), "imported_inventory": imported_before,
            "network_manifest": observed, "object_sha256": object_sha, "cas_sha256": cas_sha,
            "restarted_repo_a": restarted, "repo_b_unchanged": True}


def run_spec193_retired_python(ndn, output: Path, processes: List[Dict[str, Any]],
                              environments: Dict[str, Dict[str, str]],
                              servers: List[Dict[str, Any]], trust_schema: Path) -> Dict[str, Any]:
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "p06-source.bin"
    source.write_bytes(bytes(range(256)) + b"p06-source")
    source_hash = digest(source)
    forbidden = work / "p06-forbidden"
    report_path = work / "p06-report.json"
    before = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    events = repo_request_events(processes, set(before))
    child = run_child(ndn, output, processes, APP_HOST, "spec193-p06-retired-python",
        [sys.executable, str(REPO / "tests/minindn/spec193_retired_python_peer.py"),
         "--group", GROUP, "--controller", CONTROLLER, "--user", USER,
         "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
         "--source", str(source), "--forbidden-root", str(forbidden), "--report-out", str(report_path)],
        environments[APP_HOST], 60)
    report = json.loads(report_path.read_text())
    cases = report.get("cases", [])
    if (report.get("status") != "PASS" or len(cases) != 141 or len({x["case"] for x in cases}) != 141 or
            any(x["code"] != "SPEC192_COMPATIBILITY_V1" or not x["operation"] or not x["replacement"] for x in cases) or
            not report.get("native_type", "").endswith("NativeArtifactRepositoryApi") or
            report.get("source_sha256") != source_hash or digest(source) != source_hash or forbidden.exists()):
        raise RuntimeError("P06 independent compatibility/local-side-effect oracle failed")
    for module in report["native_modules"].values():
        if digest(Path(module["path"])) != module["sha256"]:
            raise RuntimeError("P06 loaded native module hash changed")
    for path, sha in report["loaded_libraries"].items():
        if digest(Path(path)) != sha:
            raise RuntimeError("P06 loaded native library hash changed")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    if after != before or repo_request_events(processes, set(before)) != events:
        raise RuntimeError("P06 retired calls changed Repo storage or invoked handler")
    return {"case_id": "P06", "status": "PASS", "children": [child], "report": report,
            "repo_state_before": before, "repo_state_after": after,
            "repo_handler_events": events, "source_sha256": source_hash, "forbidden_root_absent": True}


def run_spec193_cross_language(ndn, output: Path, processes: List[Dict[str, Any]],
                             environments: Dict[str, Dict[str, str]], repo_client: Path,
                             servers: List[Dict[str, Any]], trust_schema: Path) -> Dict[str, Any]:
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    cpp_source, python_source = work / "p01-source.bin", work / "p02-source.bin"
    payload = bytes((index * 19 + 11) % 256 for index in range(521))
    cpp_source.write_bytes(payload)
    python_source.write_bytes(payload[::-1] + b"python-origin")
    cpp_handle, cpp_receipt = work / "p01-handle.json", work / "p01-receipt.json"
    py_handle, py_receipt = work / "p02-handle.json", work / "p02-receipt.json"
    base = ["--group", GROUP, "--controller", CONTROLLER, "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX, "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    binding = str(REPO / "tests/minindn/spec192_binding_client.py")
    children = []
    def state() -> dict:
        return {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    def inventory(states: dict) -> dict:
        return {node: repo_artifact_inventory(value) for node, value in states.items()}
    def python_report(child: dict, mode: str) -> dict:
        reports = [json.loads(line) for line in read_log(Path(child["log"])).splitlines()
                   if line.startswith('{"')]
        matches = [report for report in reports if report.get("mode") == mode]
        if len(matches) != 1:
            raise RuntimeError("binding lacks one final structured result: " + mode)
        return matches[0]
    def reference_pair(handle_path: Path, receipt_path: Path, source: Path, publisher: str) -> dict:
        handle, receipt = json.loads(handle_path.read_text()), json.loads(receipt_path.read_text())
        ensure_two_repo_commits(receipt_path)
        reference = handle["reference"]
        if (reference != receipt["reference"] or reference["contentDigest"] != digest(source) or
                reference["sizeBytes"] != source.stat().st_size or reference["publisherIdentity"] != publisher or
                receipt["receiptId"] not in handle["commitReceiptIds"] or
                handle["replicaCommits"] != receipt["replicaCommits"] or
                handle["lifecycleMode"] != "PERSISTENT" or receipt["state"] != "COMMITTED"):
            raise RuntimeError("cross-language handle/receipt/reference mismatch")
        return {"handle": handle, "receipt": receipt}
    before = state()
    for value in before.values():
        assert_no_remote_artifacts(value)
    child = run_child(ndn, output, processes, APP_HOST, "spec193-p01-cpp-publish",
        [str(repo_client), "--mode", "publish", "--source", str(cpp_source),
         "--handle-out", str(cpp_handle), "--receipt-out", str(cpp_receipt),
         "--authority", CONTROLLER, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 1200, require_no_python=True)
    children.append(child)
    p01 = reference_pair(cpp_handle, cpp_receipt, cpp_source, USER)
    p01_state = state()
    for value in p01_state.values():
        assert_repo_durable_state(value, digest(cpp_source))
    destination = work / "p01-python-fetched.bin"
    child = run_child(ndn, output, processes, CONSUMER_HOST, "spec193-p01-python-fetch",
        [sys.executable, binding, "--mode", "fetch", "--user", CONSUMER,
         "--handle", str(cpp_handle), "--destination", str(destination)] + base,
        environments[CONSUMER_HOST], 360)
    children.append(child)
    fetched = python_report(child, "fetch")
    if digest(destination) != digest(cpp_source) or fetched["digest"] != digest(cpp_source):
        raise RuntimeError("P01 independent Python source/destination digest mismatch")
    child = run_child(ndn, output, processes, APP_HOST, "spec193-p01-python-receipt",
        [sys.executable, binding, "--mode", "receipt", "--user", USER,
         "--handle", str(cpp_handle), "--receipt", str(cpp_receipt)] + base,
        environments[APP_HOST], 360)
    children.append(child)
    queried = python_report(child, "receipt")
    if json.loads(queried["receipt_json"]) != p01["receipt"] or inventory(state()) != inventory(p01_state):
        raise RuntimeError("P01 receipt identity or read-only inventory mismatch")
    p01.update({"python_fetch": fetched, "python_receipt": queried, "repo_state": p01_state})
    destination = work / "p02-python-roundtrip.bin"
    child = run_child(ndn, output, processes, CONSUMER_HOST, "spec193-p02-python-publish",
        [sys.executable, binding, "--mode", "publish", "--user", CONSUMER,
         "--source", str(python_source), "--handle-out", str(py_handle),
         "--receipt-out", str(py_receipt), "--destination", str(destination)] + base,
        environments[CONSUMER_HOST], 360)
    children.append(child)
    published = python_report(child, "publish")
    p02 = reference_pair(py_handle, py_receipt, python_source, CONSUMER)
    if (digest(destination) != digest(python_source) or published["digest"] != digest(python_source) or
            json.loads(published["receipt_json"]) != p02["receipt"]):
        raise RuntimeError("P02 native binding publication mismatch")
    p02_state = state()
    for value in p02_state.values():
        assert_repo_durable_state(value, digest(cpp_source))
        assert_repo_durable_state(value, digest(python_source))
    destination = work / "p02-cpp-fetched.bin"
    child = run_child(ndn, output, processes, APP_HOST, "spec193-p02-cpp-fetch",
        [str(repo_client), "--mode", "fetch", "--handle", str(py_handle),
         "--destination", str(destination), "--authority", CONTROLLER, "--user", USER] + base,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 180, require_no_python=True)
    children.append(child)
    if digest(destination) != digest(python_source) or "PERSISTENT_FETCH_PASS" not in read_log(Path(child["log"])):
        raise RuntimeError("P02 independent cross-node C++ fetch mismatch")
    queried_receipt = work / "p02-cpp-receipt.json"
    child = run_child(ndn, output, processes, CONSUMER_HOST, "spec193-p02-cpp-receipt",
        [str(repo_client), "--mode", "receipt-query", "--handle", str(py_handle),
         "--receipt", str(py_receipt), "--receipt-out", str(queried_receipt),
         "--authority", CONTROLLER, "--user", CONSUMER] + base,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True), 180, require_no_python=True)
    children.append(child)
    after = state()
    if json.loads(queried_receipt.read_text()) != p02["receipt"] or inventory(after) != inventory(p02_state):
        raise RuntimeError("P02 receipt identity or read-only inventory mismatch")
    p02.update({"python_publish": published, "repo_state": p02_state})
    return {"case_id": "P01_P02", "status": "PASS", "children": children, "p01": p01, "p02": p02,
            "repo_state_before": before, "repo_state_after": after,
            "source_sha256": {"p01": digest(cpp_source), "p02": digest(python_source)}}


def run_spec193_network_wrong_signer(ndn, output: Path,
                                    processes: List[Dict[str, Any]],
                                    environments: Dict[str, Dict[str, str]],
                                    repo_client: Path, servers: List[Dict[str, Any]],
                                    trust_schema: Path) -> Dict[str, Any]:
    scenario = run_spec193_network_object_wrappers(
        ndn, output, processes, environments, repo_client, servers, trust_schema)
    before = scenario["repo_state_after"]
    work = output / "artifacts"
    report_path = work / "nw-wrong-signer-report.json"
    gate = work / "nw-wrong-signer-response-name"
    target = [{**servers[0], "argv": servers[0]["argv"] + ["--response-fault", "hold"]},
              {**servers[1], "argv": servers[1]["argv"] + ["--response-fault", "wrong-signer",
                                                       "--fault-response-file", str(gate)]}]
    restarted = restart_repo_servers(ndn, output, processes, environments, target,
                                    "wrong-signer", ready_text="Spec193 object Repo serving")
    servers[:] = target
    waiting = spawn(
        ndn, output, processes, APP_HOST, "spec193-nw-wrong-signer-client",
        [str(repo_client), "--mode", "network-object-wrappers", "--phase", "wrong-signer",
         "--source", str(work / "nw-source.bin"), "--object-name", "/spec193/wrong-signer/" + output.name,
         "--report-out", str(report_path), "--user", USER, "--group", GROUP,
         "--controller", CONTROLLER, "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX, "--repo-a", REPO_A_ID,
         "--provider-a", REPO_A_PROVIDER, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True))
    assert_no_python_mapped(waiting)
    wait_log(servers[0], "SPEC193_FAULT_SELECTED requestId=", 35)
    matches = re.findall(r"SPEC193_FAULT_SELECTED requestId=(\S+) requester=(\S+) response=(\S+)",
                         read_log(servers[0]["log"]))
    if len(matches) != 1 or matches[0][1] != USER:
        raise RuntimeError("wrong-signer lacks one genuine selected A request")
    temporary = gate.with_suffix(".pending")
    temporary.write_text(matches[0][2] + "\n")
    temporary.replace(gate)
    try:
        rc = waiting["proc"].wait(timeout=15)
    except subprocess.TimeoutExpired:
        waiting["proc"].kill()
        waiting["proc"].wait(timeout=5)
        raise RuntimeError("wrong-signer observer exceeded deadline")
    waiting["stream"].close()
    stopped = [stop_repo_server(server) for server in servers]
    client_log = read_log(waiting["log"])
    refusal = "Reject RESPONSE whose KeyLocator identity does not match the Provider in its name: " + matches[0][2]
    if (rc != 0 or "SPEC193_NETWORK_WRAPPERS_PASS phase=wrong-signer count=1" not in client_log or
            refusal not in client_log or "ResponseMessage decode failed" in client_log):
        raise RuntimeError("wrong-signer lacks identity refusal and timeout-only result: " + client_log[-2000:])
    report = json.loads(report_path.read_text())
    call = report["calls"][0] if len(report["calls"]) == 1 else {}
    if (call.get("operation") != "RECEIPT_WRONG_SIGNER" or call.get("requestId") != matches[0][0] or
            call.get("status") is not False or call.get("errorCode") != "network-wrapper-timeout" or
            (work / call["payloadFile"]).read_bytes() != b"" or
            digest(work / call["payloadFile"]) != call["payloadSha256"]):
        raise RuntimeError("wrong-signer application callback oracle failed")
    publication = "SPEC193_WRONG_SIGNER_PUBLISHED requestId=" + call["requestId"] + " signer=" + REPO_B_PROVIDER
    if read_log(servers[1]["log"]).count(publication) != 1:
        raise RuntimeError("wrong-signer lacks one B-owned fault publication")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    if after != before or repo_request_events(processes, set(before)) != scenario["authenticated_requests"]:
        raise RuntimeError("wrong-signer changed storage or ordinary handler events")
    child = {key: waiting[key] for key in ("host", "label", "pid", "python_mapped", "python_maps_observed", "path_hides_python")}
    child.update({"exit_code": rc, "log": str(waiting["log"])})
    scenario.update({"case_id": "NW_WRONG_SIGNER", "wrong_signer_report": report,
                     "wrong_signer_response_name": matches[0][2], "wrong_signer_client_log": child["log"],
                     "repo_state_after_wrong_signer": after, "fault_server_restart": restarted,
                     "fault_server_stop": stopped})
    scenario["children"].append(child)
    return scenario


def run_spec193_network_malformed_response(ndn, output: Path,
                                         processes: List[Dict[str, Any]],
                                         environments: Dict[str, Dict[str, str]],
                                         repo_client: Path, servers: List[Dict[str, Any]],
                                         trust_schema: Path) -> Dict[str, Any]:
    scenario = run_spec193_network_object_wrappers(
        ndn, output, processes, environments, repo_client, servers, trust_schema)
    before = scenario["repo_state_after"]
    work = output / "artifacts"
    source = work / "nw-source.bin"
    report_path = work / "nw-malformed-report.json"
    fault_configuration = {**servers[0], "argv": servers[0]["argv"] + ["--response-fault", "malformed"]}
    target = [fault_configuration]
    restarted = restart_repo_servers(ndn, output, processes, environments, target,
                                    "malformed", ready_text="Spec193 object Repo serving")
    servers[0] = target[0]
    child = run_child(
        ndn, output, processes, APP_HOST, "spec193-nw-malformed-client",
        [str(repo_client), "--mode", "network-object-wrappers", "--phase", "malformed",
         "--source", str(source), "--object-name", "/spec193/malformed/" + output.name,
         "--report-out", str(report_path), "--user", USER, "--group", GROUP,
         "--controller", CONTROLLER, "--authority", CONTROLLER, "--trust-schema", str(trust_schema),
         "--service-prefix", SERVICE_PREFIX, "--repo-a", REPO_A_ID,
         "--provider-a", REPO_A_PROVIDER, "--provider-b", REPO_B_PROVIDER],
        node_environment(ndn.net[APP_HOST], REPO, no_python=True), 45, require_no_python=True)
    stopped = stop_repo_server(servers[0])
    client_log = read_log(Path(child["log"]))
    if ("SPEC193_NETWORK_WRAPPERS_PASS phase=malformed count=1" not in client_log or
            "ResponseMessage decode failed" not in client_log):
        raise RuntimeError("malformed response lacks actual decrypted decode refusal and timeout-only result")
    report = json.loads(report_path.read_text())
    call = report["calls"][0] if len(report["calls"]) == 1 else {}
    if (call.get("operation") != "RECEIPT_MALFORMED" or call.get("status") is not False or
            call.get("errorCode") != "network-wrapper-timeout" or
            (work / call["payloadFile"]).read_bytes() != b"" or
            digest(work / call["payloadFile"]) != call["payloadSha256"]):
        raise RuntimeError("malformed response completed application callback")
    fault_log = read_log(servers[0]["log"])
    request_id = call["requestId"]
    if (fault_log.count("SPEC193_FAULT_SELECTED requestId=" + request_id + " ") != 1 or
            fault_log.count("SPEC193_FAULT_PUBLISHED requestId=" + request_id + " plaintextHex=ff") != 1):
        raise RuntimeError("malformed producer selection/publication is not bound to one real request")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    if after != before:
        raise RuntimeError("malformed response fixture changed Repo storage")
    if repo_request_events(processes, {REPO_A_ID, REPO_B_ID}) != scenario["authenticated_requests"]:
        raise RuntimeError("malformed query unexpectedly invoked a normal Repo handler")
    scenario.update({"case_id": "NW_MALFORMED", "malformed_report": report,
                     "malformed_client_log": child["log"], "malformed_provider_log": str(servers[0]["log"]),
                     "repo_state_after_malformed": after, "fault_server_restart": restarted,
                     "fault_server_stop": stopped})
    scenario["children"].append(child)
    return scenario


def run_spec193_network_negative_callbacks(ndn, output: Path,
                                          processes: List[Dict[str, Any]],
                                          environments: Dict[str, Dict[str, str]],
                                          repo_client: Path, servers: List[Dict[str, Any]],
                                          trust_schema: Path) -> Dict[str, Any]:
    scenario = run_spec193_network_artifact_wrappers(
        ndn, output, processes, environments, repo_client, servers, trust_schema)
    work = output / "artifacts"
    source = work / "nw-artifact-source.bin"
    before = scenario["repo_state_after"]
    receipt = next(work / call["payloadFile"] for call in scenario["reports"]["artifact"]["calls"]
                   if call["operation"] == "RECEIPT")
    common = ["--source", str(source), "--object-name", "/spec193/negative/" + output.name,
              "--group", GROUP, "--controller", CONTROLLER, "--authority", CONTROLLER,
              "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
              "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER, "--provider-b", REPO_B_PROVIDER]
    unauthorized_path = work / "nw-unauthorized-report.json"
    denied = run_child(
        ndn, output, processes, CONSUMER_HOST, "spec193-nw-unauthorized-receipt",
        [str(repo_client), "--mode", "network-object-wrappers", "--phase", "unauthorized",
         "--user", CONSUMER, "--receipt", str(receipt), "--report-out", str(unauthorized_path)] + common,
        node_environment(ndn.net[CONSUMER_HOST], REPO, no_python=True), 90, require_no_python=True)
    if "SPEC193_NETWORK_WRAPPERS_PASS phase=unauthorized count=1" not in read_log(Path(denied["log"])):
        raise RuntimeError("unauthorized wrapper phase incomplete")
    unauthorized = json.loads(unauthorized_path.read_text())
    call = unauthorized["calls"][0] if len(unauthorized["calls"]) == 1 else {}
    expected = b"repo-publish-operation-unauthorized: requester is not the submitting principal"
    if (call.get("operation") != "RECEIPT" or call.get("status") is not False or
            call.get("errorCode") != "repo-publish-operation-unauthorized" or
            (work / call["payloadFile"]).read_bytes() != expected or
            digest(work / call["payloadFile"]) != call["payloadSha256"]):
        raise RuntimeError("independent unauthorized callback oracle failed")
    after_denied = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    if after_denied != before:
        raise RuntimeError("unauthorized receipt request mutated storage")
    timeout_path = work / "nw-timeout-report.json"
    gate = work / "nw-timeout-request-go"
    waiting = spawn(
        ndn, output, processes, APP_HOST, "spec193-nw-timeout",
        [str(repo_client), "--mode", "network-object-wrappers", "--phase", "timeout",
         "--user", USER, "--receipt", str(receipt), "--request-gate", str(gate), "--report-out", str(timeout_path)] + common,
        node_environment(ndn.net[APP_HOST], REPO, no_python=True))
    assert_no_python_mapped(waiting)
    wait_log(waiting, "SPEC193_NEGATIVE_BOOTSTRAP_READY", 35)
    (work / "nw-negative-state-before-stop.json").write_text(json.dumps(before, sort_keys=True, indent=2))
    stopped = [stop_repo_server(server) for server in servers if server["repo_node"] == REPO_A_ID]
    after_stop = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    (work / "nw-negative-state-after-stop.json").write_text(json.dumps(after_stop, sort_keys=True, indent=2))
    gate.write_text("go\n")
    try:
        rc = waiting["proc"].wait(timeout=20)
    except subprocess.TimeoutExpired:
        waiting["proc"].kill()
        waiting["proc"].wait(timeout=5)
        raise RuntimeError("wrapper timeout observer did not terminate within outer bound")
    waiting["stream"].close()
    if rc != 0 or "SPEC193_NETWORK_WRAPPERS_PASS phase=timeout count=1" not in read_log(waiting["log"]):
        raise RuntimeError("wrapper timeout phase failed: " + read_log(waiting["log"])[-1500:])
    timeout = json.loads(timeout_path.read_text())
    call = timeout["calls"][0] if len(timeout["calls"]) == 1 else {}
    if (call.get("operation") != "RECEIPT_TIMEOUT" or call.get("status") is not False or
            call.get("errorCode") != "network-wrapper-timeout" or call.get("requestId") in (None, "", "/") or
            (work / call["payloadFile"]).read_bytes() != b"" or
            digest(work / call["payloadFile"]) != call["payloadSha256"]):
        raise RuntimeError("independent timeout callback oracle failed")
    after_timeout = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    (work / "nw-negative-state-after-timeout.json").write_text(json.dumps(after_timeout, sort_keys=True, indent=2))
    def without_sqlite_shm(states: dict) -> dict:
        return {node: {**state, "databases": {
            path: {field: value for field, value in database.items() if field != "shm"}
            for path, database in state["databases"].items()}}
            for node, state in states.items()}
    if (without_sqlite_shm(after_stop) != without_sqlite_shm(before) or
            without_sqlite_shm(after_timeout) != without_sqlite_shm(after_stop)):
        raise RuntimeError("timeout request mutated storage")
    requests = repo_request_events(processes, set(before))
    denied_events = [event for event in requests if event["requester"] == CONSUMER]
    if (len(denied_events) != 1 or denied_events[0]["repo_node"] != REPO_A_ID or
            denied_events[0]["service"] != SERVICE_PREFIX + "/STATUS" or
            any(event["repo_node"] != REPO_A_ID or event["requester"] not in (USER, CONSUMER) for event in requests)):
        raise RuntimeError("negative wrapper authenticated principal/handler oracle failed")
    timeout_child = {key: waiting[key] for key in ("host", "label", "pid", "python_mapped", "python_maps_observed", "path_hides_python")}
    timeout_child.update({"exit_code": rc, "log": str(waiting["log"])})
    scenario.update({"case_id": "NW_NEGATIVE", "negative_reports": {"unauthorized": unauthorized, "timeout": timeout},
                     "repo_state_after_denied": after_denied, "repo_state_after_stop": after_stop,
                     "repo_state_after_timeout": after_timeout,
                     "negative_authenticated_requests": denied_events, "negative_stopped_servers": stopped})
    scenario["children"].extend([denied, timeout_child])
    return scenario


def run_spec193_network_legacy_rejections(ndn, output: Path,
                                        processes: List[Dict[str, Any]],
                                        repo_client: Path, servers: List[Dict[str, Any]],
                                        trust_schema: Path) -> Dict[str, Any]:
    if len(servers) != 2:
        raise RuntimeError("N10 requires two live Persistent Repos")
    work = output / "artifacts"
    work.mkdir(parents=True, exist_ok=True)
    source = work / "nw-rejected-source.bin"
    source.write_bytes(bytes((index * 73 + 37) % 256 for index in range(521)))
    before = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
    for state in before.values():
        assert_no_remote_artifacts(state)
    requests_before = repo_request_events(processes, set(before))
    operations = ["STORE", "STORE_RANGE", "COMMIT_RANGES", "ABORT_RANGES", "INSERT", "STORE_MANIFEST", "DELETE"]
    children, reports, snapshots = [], {}, {}
    for operation in operations:
        path = work / ("nw-reject-" + operation + "-report.json")
        child = run_child(
            ndn, output, processes, APP_HOST, "spec193-nw-rejected-" + operation.lower(),
            [str(repo_client), "--mode", "network-object-wrappers", "--phase", "rejected",
             "--operation", operation, "--source", str(source), "--report-out", str(path),
             "--object-name", "/spec193/legacy-rejected/" + output.name,
             "--user", USER, "--group", GROUP, "--controller", CONTROLLER, "--authority", CONTROLLER,
             "--trust-schema", str(trust_schema), "--service-prefix", SERVICE_PREFIX,
             "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER, "--provider-b", REPO_B_PROVIDER],
            node_environment(ndn.net[APP_HOST], REPO, no_python=True), 90, require_no_python=True)
        if "SPEC193_NETWORK_WRAPPERS_PASS phase=rejected count=1" not in read_log(Path(child["log"])):
            raise RuntimeError("N10 native rejection phase incomplete: " + operation)
        report = json.loads(path.read_text())
        calls = report["calls"]
        expected_error = "repo-persistent-operation-required: " + operation + (
            " is disabled for persistent artifact repos" if operation in ("INSERT", "STORE_MANIFEST", "DELETE")
            else " requires a versioned artifact request")
        if (len(calls) != 1 or calls[0]["operation"] != operation or calls[0]["status"] is not False or
                calls[0]["errorCode"] != "repo-persistent-operation-required" or
                (work / calls[0]["payloadFile"]).read_bytes() != expected_error.encode() or
                digest(work / calls[0]["payloadFile"]) != calls[0]["payloadSha256"]):
            raise RuntimeError("independent N10 rejection result oracle failed: " + operation)
        snapshot = {item["repo_node"]: repo_database_state(item["repo_dir"]) for item in servers}
        if snapshot != before:
            raise RuntimeError("N10 rejected request changed persistent storage: " + operation)
        children.append(child)
        reports[operation] = report
        snapshots[operation] = snapshot
    events = repo_request_events(processes, set(before))[len(requests_before):]
    if (len(events) != len(operations) or
            [event["service"] for event in events] != [SERVICE_PREFIX + "/" + operation for operation in operations] or
            any(event["repo_node"] != REPO_A_ID or event["requester"] != USER for event in events)):
        raise RuntimeError("N10 authenticated executing principal/operation oracle failed")
    return {"case_id": "NW_REJECT", "status": "PASS", "children": children,
            "reports": reports, "operations": operations, "repo_state_before": before,
            "repo_state_after_each": snapshots, "authenticated_requests": events}


def run_spec193_persistent_mode_boundary(ndn, output: Path,
                                         processes: List[Dict[str, Any]],
                                         repo_client: Path,
                                         servers: List[Dict[str, Any]],
                                         trust_schema: Path) -> Dict[str, Any]:
    source = output / "artifacts/spec193-legacy-store-reject.bin"
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(b"Spec193 persistent mode must reject unversioned STORE\x00v1")
    before = {item["repo_node"]: repo_database_state(item["repo_dir"])
              for item in servers}
    requests_before = repo_request_events(processes, set(before))
    argv = [str(repo_client), "--mode", "object-write-reject",
            "--source", str(source), "--object-name", "/spec193/rejected/" + output.name,
            "--group", GROUP, "--controller", CONTROLLER,
            "--authority", CONTROLLER, "--user", USER,
            "--trust-schema", str(trust_schema),
            "--service-prefix", SERVICE_PREFIX,
            "--repo-a", REPO_A_ID, "--provider-a", REPO_A_PROVIDER,
            "--repo-b", REPO_B_ID, "--provider-b", REPO_B_PROVIDER]
    child = run_child(ndn, output, processes, APP_HOST,
                      "spec193-persistent-object-store-rejected", argv,
                      node_environment(ndn.net[APP_HOST], REPO, no_python=True),
                      180, require_no_python=True)
    if "SPEC193_PERSISTENT_OBJECT_REJECT_PASS" not in read_log(Path(child["log"])):
        raise RuntimeError("Persistent client did not prove mode-boundary rejection")
    after = {item["repo_node"]: repo_database_state(item["repo_dir"])
             for item in servers}
    if before != after:
        raise RuntimeError("rejected unversioned STORE changed Persistent Repo storage")
    requests_after = repo_request_events(processes, set(before))
    new_requests = requests_after[len(requests_before):]
    if not any(item["repo_node"] == REPO_A_ID and
               item["service"] == SERVICE_PREFIX + "/STORE" and
               item["requester"] == USER for item in new_requests):
        raise RuntimeError("selected Persistent Repo did not receive signed STORE")
    return {"case_id": "PERSISTENT_MODE_BOUNDARY", "status": "PASS",
            "children": [child], "repo_state_before": before,
            "repo_state_after": after, "authenticated_requests": new_requests}


def main() -> int:
    global REPO_C_HOST, REPO_D_HOST, HOST_ROLES
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology-file", type=Path,
                        default=REPO / "Experiments/Topology/AI_Lab.conf")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--campaign-case", choices=("A05", "ARTIFACT_BASELINE",
                                                   "ARTIFACT_SIX_HOST",
                                                   "OBJECT_BASELINE", "PERSIST_SEED", "PERSIST_READ", "PERSIST_ARTIFACT_SEED", "PERSIST_ARTIFACT_READ", "L01", "L02", "L03_L04", "NW_OBJECT", "NW_MALFORMED", "O01", "O02", "O03", "O04", "O05", "O06_O07", "O08", "O09", "O10", "O11", "O12", "O13", "O14", "O15", "O16", "O17_ROUTE",
                                                   "PERSISTENT_MODE_BOUNDARY", "NW_REJECT", "NW_NEGATIVE", "NW_WRONG_SIGNER", "P01_P02", "P06", "P05", "P03_P04",
                                                   "INAPP_MATRIX", "A04", "A06",
                                                   "A06_RANGE", "A06_LARGE", "A09", "A07",
                                                   "A08_ONE", "A08_QUORUM",
                                                   "A08_REJECT", "A10_A11", "A12",
                                                   "NW_ARTIFACT", "A13", "A14_SOURCE",
                                                   "A14_TARGET", "A14_SEED",
                                                   "A15", "A16", "A17", "A18",
                                                   "A19", "A20", "A21", "A22"))
    parser.add_argument("--candidate-lock", type=Path)
    parser.add_argument("--o17-failover", action="store_true")
    parser.add_argument("--o17-corrupt-location", action="store_true")
    parser.add_argument("--o17-drop-original-data", action="store_true")
    parser.add_argument("--o17-import-legacy", action="store_true")
    parser.add_argument("--o17-cache-hit", action="store_true")
    parser.add_argument("--o17-server-auth", action="store_true")
    parser.add_argument("--durable-repo-root", type=Path)
    parser.add_argument("--seed-run-manifest", type=Path)
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--preflight-acl-services", type=Path)
    args, minindn_args = parser.parse_known_args()
    if (args.o17_failover or args.o17_corrupt_location or
            args.o17_drop_original_data or args.o17_import_legacy or
            args.o17_cache_hit or args.o17_server_auth) and \
            args.campaign_case != "O17_ROUTE":
        parser.error("O17 fault options require --campaign-case O17_ROUTE")
    if args.o17_server_auth:
        required_labels = {"cpp-repo-ucla-o17-original-seed", "cpp-repo-wustl-o17-original-seed"}
        if not required_labels.issubset(traced_provider_labels()):
            parser.error("O17 server authorization diagnostic requires explicit provider trace labels")
        if (args.o17_failover or args.o17_corrupt_location or args.o17_drop_original_data or
                args.o17_import_legacy or args.o17_cache_hit):
            parser.error("O17 server authorization diagnostic requires the unfaulted route case")
    if sum((args.o17_failover, args.o17_corrupt_location,
            args.o17_drop_original_data, args.o17_import_legacy,
            args.o17_cache_hit)) > 1:
        parser.error("O17 fault options are mutually exclusive")
    if args.campaign_case in ("PERSIST_SEED", "PERSIST_READ",
                             "PERSIST_ARTIFACT_SEED", "PERSIST_ARTIFACT_READ"):
        if args.durable_repo_root is None:
            raise RuntimeError("persistence campaign requires --durable-repo-root")
        args.durable_repo_root = args.durable_repo_root.resolve()
        evidence_root = (REPO / "specs/193-distributedrepo-multinode-api-campaign/evidence").resolve()
        try:
            relative_root = args.durable_repo_root.relative_to(evidence_root)
        except ValueError as error:
            raise RuntimeError("durable Repo root must be inside Spec193 evidence") from error
        if len(relative_root.parts) < 2 or args.durable_repo_root == args.output_dir.resolve():
            raise RuntimeError("durable Repo root must be a separate evidence cohort directory")
        if args.campaign_case in ("PERSIST_SEED", "PERSIST_ARTIFACT_SEED") and args.durable_repo_root.exists():
            raise RuntimeError("durable Repo seed root already exists")
        if args.campaign_case in ("PERSIST_READ", "PERSIST_ARTIFACT_READ"):
            if not args.durable_repo_root.is_dir() or args.seed_run_manifest is None:
                raise RuntimeError("persistence read requires seed root and --seed-run-manifest")
            args.seed_run_manifest = args.seed_run_manifest.resolve()
    if args.campaign_case in ("ARTIFACT_SIX_HOST", "A08_ONE", "A08_QUORUM",
                              "A08_REJECT", "A21"):
        REPO_C_HOST = "seattle"
        HOST_ROLES = dict(HOST_ROLES, seattle="repoC")
        if args.topology_file == REPO / "Experiments/Topology/AI_Lab.conf":
            args.topology_file = REPO / "tests/minindn/spec193_six_host.conf"
    if args.campaign_case == "A07":
        REPO_C_HOST = "seattle"
        REPO_D_HOST = "stanford"
        HOST_ROLES = dict(HOST_ROLES, seattle="repoC", stanford="repoD")
        if args.topology_file == REPO / "Experiments/Topology/AI_Lab.conf":
            args.topology_file = REPO / "tests/minindn/spec193_seven_host.conf"
    if args.campaign_case == "A05" and shutil.which("tshark") is None:
        raise RuntimeError("Spec193 A05 requires tshark for signed NDN packet verification")
    validate_di_publish_child_timeout()
    # Minindn parses process-wide sys.argv again in its constructor.
    sys.argv[:] = [sys.argv[0]] + minindn_args
    if not args.preflight_only and os.geteuid() != 0:
        raise RuntimeError("MiniNDN requires root; run this harness with sudo -E")
    log_children = {
        child.strip()
        for child in os.environ.get(NDN_LOG_TRACE_CHILD_ENV, "").split(",")
        if child.strip()
    }
    unsupported_log_children = log_children - NDN_LOG_TRACE_CHILDREN
    if unsupported_log_children:
        raise RuntimeError(
            "unsupported NDN logger trace children: " +
            ",".join(sorted(unsupported_log_children)))
    unsupported_log_providers = traced_provider_labels() - NDN_LOG_TRACE_PROVIDER_LABELS
    if unsupported_log_providers:
        raise RuntimeError("unsupported NDN logger trace providers: " +
                           ",".join(sorted(unsupported_log_providers)))
    # Mininet starts NFD and application processes from the host environment.
    # Clear ambient logging there; validated opt-ins set TRACE only on targets.
    os.environ.pop("NDN_LOG", None)
    args.output_dir = args.output_dir.resolve()
    candidate_sha, candidate_files = source_candidate()
    preflight = None
    if args.campaign_case:
        acl_services = None
        if args.preflight_acl_services:
            acl_services = json.loads(args.preflight_acl_services.read_text(encoding="utf-8"))
        try:
            preflight = spec193_preflight.check(
                REPO, args.output_dir, args.topology_file, candidate_sha,
                candidate_files, HOST_ROLES, SERVICE_PREFIX, REPO_OPERATIONS,
                *repo_acl_principals(),
                require_six=args.campaign_case in
                    ("ARTIFACT_SIX_HOST", "A08_ONE", "A08_QUORUM", "A08_REJECT", "A21"),
                require_seven=args.campaign_case == "A07",
                object_mode=args.campaign_case in ("OBJECT_BASELINE", "P05", "PERSIST_SEED", "PERSIST_READ", "L01", "L02", "L03_L04", "NW_OBJECT", "NW_MALFORMED", "NW_WRONG_SIGNER", "O01", "O02", "O03", "O04", "O05", "O06_O07", "O08", "O09", "O10", "O11", "O12", "O13", "O14", "O15", "O16", "O17_ROUTE"),
                lock_path=args.candidate_lock, acl_services=acl_services,
                include_object_server=args.campaign_case == "A08_REJECT",
                include_importer=args.campaign_case == "P05")
        except spec193_preflight.PreflightError as error:
            if args.preflight_only:
                print(json.dumps({"status": "REJECT", "failure_class": error.failure_class,
                                  "detail": str(error)}, sort_keys=True))
                return 2
            raise
    elif args.output_dir.exists():
        raise RuntimeError("refusing to reuse existing evidence directory: {}".format(args.output_dir))
    if args.preflight_only:
        print(json.dumps({"status": "PASS", "candidate_lock_sha256":
                          preflight["candidate_lock_sha256"]}, sort_keys=True))
        return 0
    args.output_dir.mkdir(parents=True)
    if preflight is not None:
        write_json(args.output_dir / "preflight-candidate-lock.json", preflight)
    (REPO / "build/spec192-no-python-path").mkdir(parents=True, exist_ok=True)
    processes: List[Dict[str, Any]] = []
    daemon_stops: List[Dict[str, Any]] = []
    captures: List[Dict[str, Any]] = []
    capture_results: List[Dict[str, Any]] = []
    manifest: Dict[str, Any] = {
        "spec": "193" if args.campaign_case else "192",
        "scenario": (
            "Spec193 A05 authenticated discovery without storage mutation"
            if args.campaign_case == "A05" else
            "Spec193 artifact lifecycle, failover, and DI baseline"
            if args.campaign_case in ("ARTIFACT_BASELINE", "ARTIFACT_SIX_HOST") else
            "Spec193 object protocol selected Repo store and fetch baseline"
            if args.campaign_case == "OBJECT_BASELINE" else
            "Spec193 L03/L04 trusted local INSERT/catalog/cache and segmented reads"
            if args.campaign_case == "L03_L04" else
            "Spec193 public network object wrappers and selected catalog matrix"
            if args.campaign_case == "NW_OBJECT" else
            "Spec193 public artifact write, receipt, location and availability wrappers"
            if args.campaign_case == "NW_ARTIFACT" else
            "Spec193 Persistent rejects all legacy writes without storage mutation"
            if args.campaign_case == "NW_REJECT" else
            "Spec193 public wrapper unauthorized receipt and admitted request timeout"
            if args.campaign_case == "NW_NEGATIVE" else
            "Spec193 P03/P04 actual DI Provider native Repo authorization and warm cache"
            if args.campaign_case == "P03_P04" else
            "Spec193 P05 explicit immutable legacy DB/CAS import and independent reads"
            if args.campaign_case == "P05" else
            "Spec193 P06 retired Python/native facade zero Repo side effects"
            if args.campaign_case == "P06" else
            "Spec193 P01/P02 native Python cross-language cross-node reuse"
            if args.campaign_case == "P01_P02" else
            "Spec193 selected A response name with trusted B signer fail-closed"
            if args.campaign_case == "NW_WRONG_SIGNER" else
            "Spec193 selected Provider encrypted malformed Response fail-closed"
            if args.campaign_case == "NW_MALFORMED" else
            "Spec193 O01 atomic put/get and same-name version replacement"
            if args.campaign_case == "O01" else
            "Spec193 O02 noncontiguous range commit and remote range reads"
            if args.campaign_case == "O02" else
            "Spec193 O03 range abort, wrong owner and incomplete commit rejection"
            if args.campaign_case == "O03" else
            "Spec193 O04 local object removal and independent deleted-object audit"
            if args.campaign_case == "O04" else
            "Spec193 O05 missing and physically corrupt object read rejection"
            if args.campaign_case == "O05" else
            "Spec193 O06/O07 segmented object and missing segment matrix"
            if args.campaign_case == "O06_O07" else
            "Spec193 O08 signed Data wire storage and independent network fetch"
            if args.campaign_case == "O08" else
            "Spec193 O09 manifest-ordered signed packet set and missing index"
            if args.campaign_case == "O09" else
            "Spec193 O10 pull published signed Data through Repo INSERT"
            if args.campaign_case == "O10" else
            "Spec193 O11 insertPayload auto segmentation and APP verification"
            if args.campaign_case == "O11" else
            "Spec193 O12 insert conflicts and invalid Data rejection"
            if args.campaign_case == "O12" else
            "Spec193 O13 cache status after object write and independent reads"
            if args.campaign_case == "O13" else
            "Spec193 O14 catalog status, snapshot, delta and lookup after create"
            if args.campaign_case == "O14" else
            "Spec193 O15 catalog tombstone and stale epoch after delete"
            if args.campaign_case == "O15" else
            "Spec193 O16 inventory, manifest and status versus durable visibility"
            if args.campaign_case == "O16" else
            "Spec193 O17 original-name Data Interest with replica forwarding hint"
            if args.campaign_case == "O17_ROUTE" else
            "Spec193 Persistent Repo rejects legacy unversioned object STORE"
            if args.campaign_case == "PERSISTENT_MODE_BOUNDARY" else
            "Spec193 In-App small, multi-chunk, corrupt and missing payload matrix"
            if args.campaign_case == "INAPP_MATRIX" else
            "Spec193 A04 invalid In-App references/paths and Persistent configuration"
            if args.campaign_case == "A04" else
            "Spec193 A06 single-target publish and independent consumer fetch"
            if args.campaign_case == "A06" else
            "Spec193 A06 two-range publish, commit and independent fetch"
            if args.campaign_case == "A06_RANGE" else
            "Spec193 A06 131123B foreground range publication and independent fetch"
            if args.campaign_case == "A06_LARGE" else
            "Spec193 A09 invalid selected Repo set rejected before storage"
            if args.campaign_case == "A09" else
            "Spec193 A07 three selected Repos and one independent unselected Repo"
            if args.campaign_case == "A07" else
            "Spec193 A08 ONE/QUORUM partial-availability receipt and storage"
            if args.campaign_case in ("A08_ONE", "A08_QUORUM") else
            "Spec193 A08 QUORUM with an emitted request rejected by a replaced Repo"
            if args.campaign_case == "A08_REJECT" else
            "Spec193 A10/A11 seed barrier, asynchronous status and idempotency"
            if args.campaign_case == "A10_A11" else
            "Spec193 A12 owner restart during durable in-flight placement"
            if args.campaign_case == "A12" else
            "Spec193 A13 explicit beginArchive/archive lifecycle separation"
            if args.campaign_case == "A13" else
            "Spec193 A14 source truncation after range stage and same-key retry"
            if args.campaign_case == "A14_SOURCE" else
            "Spec193 A14 ALL target timeout, partial receipt and later recovery"
            if args.campaign_case == "A14_TARGET" else
            "Spec193 A14 seed outage before acceptance and same-key retry"
            if args.campaign_case == "A14_SEED" else
            "Spec193 A15 independent fetch after original source removal"
            if args.campaign_case == "A15" else
            "Spec193 A16 immutable receipt lookup, target identity and rejection"
            if args.campaign_case == "A16" else
            "Spec193 A17 fresh metadata availability at both committed Repos"
            if args.campaign_case == "A17" else
            "Spec193 A18 full-content availability before, during and after corruption"
            if args.campaign_case == "A18" else
            "Spec193 A19 reachable Repo with absent object and missing tail range"
            if args.campaign_case == "A19" else
            "Spec193 A20 one/all Repo outage with NFD epoch and fetch safety"
            if args.campaign_case == "A20" else
            "spec193-a21-three-repo-failover" if args.campaign_case == "A21" else
            "Spec193 A22 old handle, new LOCATION and expired hint" if args.campaign_case == "A22" else
            "VM-11/12/14/15/20/21/22/23 C++ Repo lifecycle, failover, and DI interop"),
        "campaign_case": args.campaign_case,
        "topology": str(args.topology_file.resolve()),
        "hosts": HOST_ROLES,
        "service_prefix": SERVICE_PREFIX,
        "source_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(REPO), text=True).strip(),
        "source_candidate_sha256": candidate_sha,
        "preflight_candidate_lock_sha256": (
            preflight["candidate_lock_sha256"] if preflight else None),
        "source_candidate_files": candidate_files,
        "source_candidate_excluded_evidence_paths": sorted(CANDIDATE_EVIDENCE_EXCLUDES),
        "source_candidate_excluded_generated_path_components": sorted(
            CANDIDATE_GENERATED_PATH_COMPONENTS),
        "source_candidate_excluded_generated_suffixes": sorted(
            CANDIDATE_GENERATED_SUFFIXES),
        "controller_generation_state": str(
            (args.output_dir / "controller-generation.state").resolve()),
        "host_toolchain": {
            "platform": subprocess.check_output(["uname", "-a"], text=True).strip(),
            "python": sys.version,
            "compiler": subprocess.check_output(
                ["c++", "--version"], text=True).splitlines()[0],
            "ndn_cxx": subprocess.check_output(
                ["pkg-config", "--modversion", "libndn-cxx"], text=True).strip(),
            "ndn_svs": subprocess.check_output(
                ["pkg-config", "--modversion", "libndn-svs"], text=True).strip(),
            "nfd": subprocess.check_output(
                ["nfd", "--version"], text=True,
                stderr=subprocess.STDOUT).splitlines()[0],
            "mininet": metadata.version("mininet"),
        },
        "repo_operations_authorized": list(REPO_OPERATIONS),
        "private_keys_shared": False,
    }
    ndn = None
    try:
        Minindn.cleanUp()
        Minindn.verifyDependencies()
        ndn = Minindn(topoFile=str(args.topology_file))
        ndn.start()
        generic.normalize_nlsr_link_costs(ndn)
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")
        AppManager(ndn, ndn.net.hosts, CleanNlsr, sync="psync", security=False,
                   faceType="udp", nFaces=3, routingType="link-state",
                   logLevel="INFO")
        homes = {name: set_host_ndn_config(ndn.net[name]) for name in HOST_ROLES}
        perf.wait_for_nfd_sockets(ndn, args.output_dir)
        route_helper = NdnRoutingHelper(ndn.net, "udp", "link-state")
        route_helper.addOrigin(
            [ndn.net[CONTROLLER_HOST]],
            [CONTROLLER, CONTROLLER + "/KEY", "/example/repo",
             "/example/repo/KEY", GROUP, DI_SERVICE],
        )
        for host, identity in [(APP_HOST, USER), (CONSUMER_HOST, CONSUMER)]:
            route_helper.addOrigin([ndn.net[host]], [identity, identity + "/KEY", GROUP])
        route_helper.addOrigin(
            [ndn.net[APP_HOST]],
            [DI_PROVIDER, DI_PROVIDER + "/KEY", DI_UNAUTHORIZED_PROVIDER,
             DI_UNAUTHORIZED_PROVIDER + "/KEY",
             DI_REPO_CLIENT_IDENTITY, DI_REPO_CLIENT_IDENTITY + "/KEY",
             DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY,
             DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY + "/KEY", DI_SERVICE],
        )
        repo_routes = [(REPO_A_HOST, REPO_A_PROVIDER),
                       (REPO_B_HOST, REPO_B_PROVIDER),
                       (REPO_C_HOST, REPO_C_PROVIDER)]
        if REPO_D_HOST is not None:
            repo_routes.append((REPO_D_HOST, REPO_D_PROVIDER))
        for host, identity in repo_routes:
            route_helper.addOrigin([ndn.net[host]], [identity, identity + "/KEY", GROUP])
        route_helper.addOrigin([ndn.net[host] for host, _ in repo_routes],
                               [SERVICE_PREFIX])
        route_helper.calculateRoutes()
        time.sleep(15)
        for host in ndn.net.hosts:
            Nfdc.setStrategy(host, "/example/repo", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(host, SERVICE_PREFIX, Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(host, DI_SERVICE, Nfdc.STRATEGY_MULTICAST)

        for host_name in HOST_ROLES:
            capture_path = args.output_dir / "pcap" / (host_name + ".pcap")
            capture_path.parent.mkdir(parents=True, exist_ok=True)
            capture = spawn(
                ndn, args.output_dir, processes, host_name,
                "nfd-udp-trace-" + host_name,
                ["tcpdump", "--immediate-mode", "-i", "any", "-U", "-n", "-w",
                 str(capture_path),
                 "udp or tcp" if args.campaign_case == "O17_ROUTE" else "udp"],
                node_environment(ndn.net[host_name], REPO),
            )
            capture["capture_path"] = capture_path
            captures.append(capture)
        time.sleep(1)
        if any(record["proc"].poll() is not None for record in captures):
            raise RuntimeError("per-node tcpdump trace process failed to start")

        identities = generate_identities(ndn, args.output_dir)
        manifest["identities"] = identities
        runtime_config = args.output_dir / "repo_policy.yaml"
        template = REPO / "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_policy.yaml"
        metadata_denied_user = USER if args.campaign_case == "O17_ROUTE" else None
        make_runtime_policy(template, runtime_config,
                            Path(identities["root_certificate"]),
                            metadata_denied_user=metadata_denied_user)
        if args.campaign_case:
            manifest["verified_runtime_policy_sha256"] = \
                spec193_preflight.verify_runtime_policy(
                    runtime_config, SERVICE_PREFIX, REPO_OPERATIONS,
                    *repo_acl_principals(),
                    metadata_denied_user=metadata_denied_user)
        trust_schema = args.output_dir / "generated-policy/trust-schema.conf"
        server_candidates = (
            [REPO / "build/spec193-object-repo-server"]
            if args.campaign_case in ("OBJECT_BASELINE", "P05", "PERSIST_SEED", "PERSIST_READ", "L01", "L02", "L03_L04", "NW_OBJECT", "NW_MALFORMED", "NW_WRONG_SIGNER", "O01", "O02", "O03", "O04", "O05", "O06_O07", "O08", "O09", "O10", "O11", "O12", "O13", "O14", "O15", "O16", "O17_ROUTE") else
            [REPO / "build/NDNSF-DistributedRepo/DistributedRepoServer",
             REPO / "build/DistributedRepoServer"])
        repo_server = next((item for item in server_candidates if item.is_file()), None)
        repo_client = REPO / "build/spec192-repo-client"
        if repo_server is None or not repo_client.is_file():
            raise RuntimeError("build the exact C++ Repo server and spec192-repo-client before MiniNDN")
        manifest["controller_target_certificate_imports"] = []
        for index, (identity, certificate_file) in enumerate(
                sorted(identities["target_public_certificates"].items())):
            imported = run_child(
                ndn, args.output_dir, processes, CONTROLLER_HOST,
                "import-controller-target-public-certificate-{:02d}".format(index),
                [str(repo_client), "--mode", "import-public-certificate",
                 "--certificate", certificate_file],
                node_environment(ndn.net[CONTROLLER_HOST], REPO), 15,
                require_no_python=True)
            imported["target_identity"] = identity
            manifest["controller_target_certificate_imports"].append(imported)
        manifest["authority_certificate_imports"] = []
        for host_name in HOST_ROLES:
            if host_name == CONTROLLER_HOST:
                continue
            manifest["authority_certificate_imports"].append(run_child(
                ndn, args.output_dir, processes, host_name,
                "import-controller-public-certificate-" + host_name,
                [str(repo_client), "--mode", "import-public-certificate",
                 "--certificate", identities["controller_certificate_file"]],
                node_environment(ndn.net[host_name], REPO), 15,
                require_no_python=True))
        ldd_executable = shutil.which("ldd")
        if ldd_executable is None:
            raise RuntimeError("ldd is required to verify standalone C++ runtime closure")
        controller, servers, environments = build_services(
            ndn, args.output_dir, runtime_config, trust_schema,
            repo_server, processes, REPO,
            server_ready_text=("Spec193 object Repo serving"
                               if args.campaign_case in ("OBJECT_BASELINE", "P05", "PERSIST_SEED", "PERSIST_READ", "L01", "L02", "L03_L04", "NW_OBJECT", "NW_MALFORMED", "NW_WRONG_SIGNER", "O01", "O02", "O03", "O04", "O05", "O06_O07", "O08", "O09", "O10", "O11", "O12", "O13", "O14", "O15", "O16", "O17_ROUTE") else
                               "Persistent Repo serving"),
            durable_repo_root=args.durable_repo_root,
            durable_mode=("seed" if args.campaign_case in
                          ("PERSIST_SEED", "PERSIST_ARTIFACT_SEED") else
                          "read" if args.campaign_case in
                          ("PERSIST_READ", "PERSIST_ARTIFACT_READ") else None))
        manifest["server_binary"] = str(repo_server)
        manifest["server_sha256"] = digest(repo_server)
        manifest["repo_server_configuration"] = {
            "ack_collection_ms": REPO_SERVER_ACK_COLLECTION_MS,
            "request_timeout_ms": REPO_SERVER_REQUEST_TIMEOUT_MS,
        }
        manifest["client_binary"] = str(repo_client)
        manifest["client_sha256"] = digest(repo_client)
        server_ldd = subprocess.check_output(
            [ldd_executable, str(repo_server)],
            env=node_environment(ndn.net[REPO_A_HOST], REPO, no_python=True),
            text=True, stderr=subprocess.STDOUT)
        client_ldd = subprocess.check_output(
            [ldd_executable, str(repo_client)],
            env=node_environment(ndn.net[APP_HOST], REPO, no_python=True),
            text=True, stderr=subprocess.STDOUT)
        if "libpython" in server_ldd.lower() or "libpython" in client_ldd.lower():
            raise RuntimeError("standalone C++ Repo service/client links a Python runtime")
        if "not found" in server_ldd.lower() or "not found" in client_ldd.lower():
            raise RuntimeError("standalone C++ Repo service/client has an unresolved shared library")
        manifest["server_ldd"] = server_ldd.splitlines()
        manifest["client_ldd"] = client_ldd.splitlines()
        manifest["server_runtime_libraries"] = runtime_libraries(server_ldd)
        manifest["client_runtime_libraries"] = runtime_libraries(client_ldd)
        manifest["trust_schema_sha256"] = digest(trust_schema)
        manifest["runtime_config_sha256"] = digest(runtime_config)
        manifest["process_epochs"] = [
            {"label": item["label"], "host": item["host"], "pid": item["pid"],
             "path_hides_python": item["path_hides_python"],
             "python_mapped": item["python_mapped"],
             "python_maps_observed": item["python_maps_observed"]}
            for item in [controller] + servers]
        if args.campaign_case == "A05":
            scenario = run_spec193_discovery(
                ndn, args.output_dir, processes, repo_client, servers, trust_schema)
        elif args.campaign_case == "OBJECT_BASELINE":
            scenario = run_spec193_object_baseline(
                ndn, args.output_dir, processes, repo_client, servers, trust_schema)
        elif args.campaign_case == "PERSIST_SEED":
            scenario = run_spec193_persistence_seed(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema, args.durable_repo_root)
        elif args.campaign_case == "PERSIST_READ":
            scenario = run_spec193_persistence_read(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema, args.durable_repo_root,
                args.seed_run_manifest, preflight["candidate_lock_sha256"])
        elif args.campaign_case == "PERSIST_ARTIFACT_SEED":
            scenario = run_spec193_artifact_persistence_seed(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema, args.durable_repo_root)
        elif args.campaign_case == "PERSIST_ARTIFACT_READ":
            scenario = run_spec193_artifact_persistence_read(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema, args.durable_repo_root,
                args.seed_run_manifest, preflight["candidate_lock_sha256"])
        elif args.campaign_case == "L01":
            scenario = run_spec193_local_registry_object(
                ndn, args.output_dir, processes, servers)
        elif args.campaign_case == "L02":
            scenario = run_spec193_local_registry_ranges(
                ndn, args.output_dir, processes, servers)
        elif args.campaign_case == "L03_L04":
            scenario = run_spec193_local_registry_insert_segmented(
                ndn, args.output_dir, processes, servers)
        elif args.campaign_case == "NW_ARTIFACT":
            scenario = run_spec193_network_artifact_wrappers(
                ndn, args.output_dir, processes, environments, repo_client, servers, trust_schema)
        elif args.campaign_case == "NW_OBJECT":
            scenario = run_spec193_network_object_wrappers(
                ndn, args.output_dir, processes, environments, repo_client, servers, trust_schema)
        elif args.campaign_case == "O01":
            scenario = run_spec193_object_put_replace(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O02":
            scenario = run_spec193_object_range_commit(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O03":
            scenario = run_spec193_object_range_rejections(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O04":
            scenario = run_spec193_object_remove(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O05":
            scenario = run_spec193_object_corruption(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O06_O07":
            scenario = run_spec193_segmented_object(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O08":
            scenario = run_spec193_data_packet(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O17_ROUTE":
            scenario = run_spec193_original_data_route(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema, failover=args.o17_failover,
                corrupt_location=args.o17_corrupt_location,
                drop_original_data=args.o17_drop_original_data,
                import_legacy=args.o17_import_legacy,
                cache_hit=args.o17_cache_hit,
                server_auth=args.o17_server_auth)
        elif args.campaign_case == "O09":
            scenario = run_spec193_packet_set(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O10":
            scenario = run_spec193_insert_published_data(
                ndn, args.output_dir, processes, repo_client, servers, trust_schema)
        elif args.campaign_case == "O11":
            scenario = run_spec193_insert_payload(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O12":
            scenario = run_spec193_insert_rejections(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O13":
            scenario = run_spec193_cache_status(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O14":
            scenario = run_spec193_catalog_create(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O15":
            scenario = run_spec193_catalog_delete(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "O16":
            scenario = run_spec193_inventory_visibility(
                ndn, args.output_dir, processes, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "P03_P04":
            scenario = run_spec193_di_reuse(ndn, args.output_dir, processes, environments,
                repo_client, servers, trust_schema, runtime_config, identities)
        elif args.campaign_case == "P05":
            scenario = run_spec193_legacy_import(
                ndn, args.output_dir, processes, environments, repo_client, servers, trust_schema)
        elif args.campaign_case == "P06":
            scenario = run_spec193_retired_python(
                ndn, args.output_dir, processes, environments, servers, trust_schema)
        elif args.campaign_case == "P01_P02":
            scenario = run_spec193_cross_language(
                ndn, args.output_dir, processes, environments, repo_client, servers, trust_schema)
        elif args.campaign_case == "NW_WRONG_SIGNER":
            scenario = run_spec193_network_wrong_signer(
                ndn, args.output_dir, processes, environments, repo_client, servers, trust_schema)
        elif args.campaign_case == "NW_MALFORMED":
            scenario = run_spec193_network_malformed_response(
                ndn, args.output_dir, processes, environments, repo_client, servers, trust_schema)
        elif args.campaign_case == "NW_NEGATIVE":
            scenario = run_spec193_network_negative_callbacks(
                ndn, args.output_dir, processes, environments, repo_client, servers, trust_schema)
        elif args.campaign_case == "NW_REJECT":
            scenario = run_spec193_network_legacy_rejections(
                ndn, args.output_dir, processes, repo_client, servers, trust_schema)
        elif args.campaign_case == "PERSISTENT_MODE_BOUNDARY":
            scenario = run_spec193_persistent_mode_boundary(
                ndn, args.output_dir, processes, repo_client, servers, trust_schema)
        elif args.campaign_case == "INAPP_MATRIX":
            scenario = run_spec193_inapp_matrix(
                ndn, args.output_dir, processes, repo_client, servers)
        elif args.campaign_case == "A04":
            scenario = run_spec193_invalid_inputs(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case in ("A06", "A06_RANGE", "A06_LARGE"):
            scenario = run_spec193_single_target(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema, range_case=args.campaign_case in ("A06_RANGE", "A06_LARGE"),
                large_case=args.campaign_case == "A06_LARGE")
        elif args.campaign_case == "A09":
            scenario = run_spec193_invalid_selected_set(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A07":
            scenario = run_spec193_three_selected(
                ndn, args.output_dir, processes, environments, repo_client,
                repo_server, servers, trust_schema,
                identities["additional_identities"]["certificate_names"])
        elif args.campaign_case in ("A08_ONE", "A08_QUORUM", "A08_REJECT"):
            scenario = run_spec193_partial_consistency(
                ndn, args.output_dir, processes, environments, repo_client,
                repo_server, servers, trust_schema,
                identities["additional_identities"]["certificate_names"],
                "ONE" if args.campaign_case == "A08_ONE" else "QUORUM",
                reject_target=args.campaign_case == "A08_REJECT")
        elif args.campaign_case in ("A10_A11", "A12"):
            scenario = run_spec193_operation_lifecycle(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema, restart_owner=args.campaign_case == "A12")
        elif args.campaign_case == "A13":
            scenario = run_spec193_archive_lifecycle(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A14_SOURCE":
            scenario = run_spec193_source_failure(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A14_TARGET":
            scenario = run_spec193_target_timeout(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A14_SEED":
            scenario = run_spec193_seed_outage(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A15":
            scenario = run_spec193_source_removed_fetch(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A16":
            scenario = run_spec193_receipt_lookup(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A17":
            scenario = run_spec193_metadata_availability(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A18":
            scenario = run_spec193_verify_content_availability(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A19":
            scenario = run_spec193_missing_partial_availability(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        elif args.campaign_case == "A20":
            scenario = run_spec193_repo_offline_availability(
                ndn, args.output_dir, processes, environments, repo_client,
                servers, trust_schema)
        elif args.campaign_case == "A21":
            scenario = run_spec193_three_repo_failover(
                ndn, args.output_dir, processes, environments, repo_client,
                repo_server, servers, trust_schema,
                identities["additional_identities"]["certificate_names"])
        elif args.campaign_case == "A22":
            scenario = run_spec193_old_handle_new_location(
                ndn, args.output_dir, processes, repo_client, servers,
                trust_schema)
        else:
            scenario = run_scenario(ndn, args.output_dir, processes, environments,
                                    repo_client, repo_server, servers,
                                    trust_schema, runtime_config,
                                    identities["additional_identities"]["certificate_names"])
        manifest["scenario_result"] = scenario
        manifest["repo_server_process_epochs"] = [
            {"label": item["label"], "host": item["host"], "pid": item["pid"],
             "repo_node": item["repo_node"],
             "provider_identity": item["provider_identity"],
             "path_hides_python": item["path_hides_python"],
             "python_mapped": item["python_mapped"],
             "python_maps_observed": item["python_maps_observed"]}
            for item in processes if item.get("repo_node") is not None
        ]
        for record in processes:
            if record["proc"].poll() is None and record not in captures:
                daemon_stops.append(stop_daemon(record))
        capture_results.extend(stop_capture(record) for record in captures)
        manifest["pcap_traces"] = capture_results
        manifest["service_stop_results"] = daemon_stops
        if any(item["stop_method"] == "SIGKILL-fallback" for item in daemon_stops):
            raise RuntimeError("a C++ Repo or controller process required SIGKILL cleanup")
        if any(item["bytes"] == 0 or "0 packets captured" in item["tcpdump_summary"]
               for item in capture_results):
            raise RuntimeError("at least one MiniNDN host captured no NDN UDP packets")
        if args.campaign_case == "A05":
            from spec193_packet_oracle import verify_a05
            scenario["signed_packet_oracle"] = verify_a05(
                args.output_dir, SERVICE_PREFIX, USER,
                {REPO_A_HOST: REPO_A_PROVIDER, REPO_B_HOST: REPO_B_PROVIDER},
                scenario["capture_offsets"], scenario["request_id"],
                scenario["candidates"])
            scenario["status"] = "PASS"
        elif args.campaign_case == "P03_P04":
            from spec193_packet_oracle import verify_cross_language_calls, verify_local_namespace_absent
            scenario["packet_oracle"] = {
                "authorized": verify_cross_language_calls(args.output_dir, SERVICE_PREFIX,
                    {USER: APP_HOST, DI_REPO_CLIENT_IDENTITY: APP_HOST},
                    {REPO_A_PROVIDER: REPO_A_HOST, REPO_B_PROVIDER: REPO_B_HOST},
                    certificate_files={DI_REPO_CLIENT_IDENTITY: scenario["reader_certificate"]},
                    required_operations={USER: {"STORE", "STATUS"}, DI_REPO_CLIENT_IDENTITY: {"STATUS", "FETCH_RANGE"}}),
                "denied": verify_local_namespace_absent(args.output_dir,
                    DI_UNAUTHORIZED_REPO_CLIENT_IDENTITY + "/NDNSF/REQUEST", list(HOST_ROLES))}
        elif args.campaign_case == "P05":
            from spec193_packet_oracle import verify_network_wrapper_calls
            scenario["packet_oracle"] = verify_network_wrapper_calls(
                args.output_dir, scenario["reports"], SERVICE_PREFIX, CONSUMER,
                REPO_A_PROVIDER, REPO_B_PROVIDER, caller_host=CONSUMER_HOST)
        elif args.campaign_case == "P06":
            from spec193_packet_oracle import verify_local_namespace_absent
            scenario["packet_oracle"] = verify_local_namespace_absent(
                args.output_dir, USER + "/NDNSF/REQUEST", list(HOST_ROLES))
        elif args.campaign_case == "P01_P02":
            from spec193_packet_oracle import verify_cross_language_calls
            scenario["packet_oracle"] = verify_cross_language_calls(
                args.output_dir, SERVICE_PREFIX, {USER: APP_HOST, CONSUMER: CONSUMER_HOST},
                {REPO_A_PROVIDER: REPO_A_HOST, REPO_B_PROVIDER: REPO_B_HOST})
        elif args.campaign_case == "NW_WRONG_SIGNER":
            from spec193_packet_oracle import verify_network_wrapper_calls
            fault_id = scenario["wrong_signer_report"]["calls"][0]["requestId"]
            scenario["packet_oracle"] = {
                "positive": verify_network_wrapper_calls(args.output_dir, scenario["reports"], SERVICE_PREFIX,
                    USER, REPO_A_PROVIDER, REPO_B_PROVIDER, wrong_signer_request_id=fault_id),
                "wrong_signer": verify_network_wrapper_calls(args.output_dir,
                    {"wrong-signer": scenario["wrong_signer_report"]}, SERVICE_PREFIX,
                    USER, REPO_A_PROVIDER, REPO_B_PROVIDER, wrong_signer_request_id=fault_id)}
            calls = scenario["packet_oracle"]["wrong_signer"]["signed_calls"]
            if len(calls) != 1 or len(calls[0]["wire"]["neu:response"]) != 1:
                raise RuntimeError("wrong-signer fault is not one independently verified Response")
        elif args.campaign_case == "NW_MALFORMED":
            from spec193_packet_oracle import verify_network_wrapper_calls
            scenario["packet_oracle"] = {
                "positive": verify_network_wrapper_calls(args.output_dir, scenario["reports"], SERVICE_PREFIX,
                    USER, REPO_A_PROVIDER, REPO_B_PROVIDER),
                "malformed": verify_network_wrapper_calls(args.output_dir,
                    {"malformed": scenario["malformed_report"]}, SERVICE_PREFIX,
                    USER, REPO_A_PROVIDER, REPO_B_PROVIDER)}
            calls = scenario["packet_oracle"]["malformed"]["signed_calls"]
            if len(calls) != 1 or len(calls[0]["wire"]["neu:response"]) != 1:
                raise RuntimeError("malformed fault is not one independently verified Response publication")
        elif args.campaign_case == "NW_NEGATIVE":
            from spec193_packet_oracle import verify_network_wrapper_calls, verify_timeout_request
            scenario["packet_oracle"] = {
                "positive": verify_network_wrapper_calls(args.output_dir, scenario["reports"], SERVICE_PREFIX,
                    USER, REPO_A_PROVIDER, REPO_B_PROVIDER),
                "unauthorized": verify_network_wrapper_calls(args.output_dir,
                    {"unauthorized": scenario["negative_reports"]["unauthorized"]}, SERVICE_PREFIX,
                    CONSUMER, REPO_A_PROVIDER, REPO_B_PROVIDER, caller_host=CONSUMER_HOST),
                "timeout": verify_timeout_request(args.output_dir, scenario["negative_reports"]["timeout"],
                    SERVICE_PREFIX, USER, [REPO_A_PROVIDER, REPO_B_PROVIDER], list(HOST_ROLES))}
        elif args.campaign_case in ("NW_OBJECT", "NW_ARTIFACT", "NW_REJECT"):
            from spec193_packet_oracle import verify_network_wrapper_calls
            scenario["packet_oracle"] = verify_network_wrapper_calls(
                args.output_dir, scenario["reports"], SERVICE_PREFIX, USER,
                REPO_A_PROVIDER, REPO_B_PROVIDER)
        elif args.campaign_case == "L03_L04":
            from spec193_packet_oracle import verify_local_namespace_absent
            scenario["packet_oracle"] = verify_local_namespace_absent(
                args.output_dir, scenario["object_name"], list(HOST_ROLES))
        elif args.campaign_case == "A21":
            from spec193_packet_oracle import verify_a21_sync_attempts
            scenario["packet_oracle"] = verify_a21_sync_attempts(
                args.output_dir, SERVICE_PREFIX, CONSUMER, GROUP,
                scenario["offline_capture_offsets"], scenario["fetch_attempts"],
                [REPO_C_PROVIDER, REPO_B_PROVIDER, REPO_A_PROVIDER])
        elif args.campaign_case == "O17_ROUTE":
            from spec193_packet_oracle import verify_original_data_route
            scenario["packet_oracle"] = verify_original_data_route(
                args.output_dir, scenario["data_name"],
                scenario["wire_sha256"],
                REPO_B_PROVIDER if (args.o17_failover or args.o17_corrupt_location or
                                    args.o17_drop_original_data)
                else REPO_A_PROVIDER,
                "wustl" if (args.o17_failover or args.o17_corrupt_location or
                            args.o17_drop_original_data) else "ucla",
                failed_provider=REPO_A_PROVIDER if args.o17_drop_original_data else None,
                active_logs=scenario["active_repo_logs"],
                expect_single_serve=args.o17_cache_hit)
        elif args.campaign_case in ("OBJECT_BASELINE", "PERSIST_SEED"):
            from spec193_packet_oracle import verify_object_capability
            scenario["signed_packet_oracle"] = verify_object_capability(
                args.output_dir, SERVICE_PREFIX, USER,
                REPO_A_PROVIDER, REPO_B_PROVIDER,
                scenario["capture_offsets"], scenario["capability_request_id"],
                scenario["capability_ack_wire_digest"],
                scenario["capability_signer_key_locator"])
            scenario["status"] = "PASS"
        manifest["status"] = "PASS"
        manifest["finished_unix"] = time.time()
        write_json(args.output_dir / "run-manifest.json", manifest)
        return 0
    except BaseException as exc:
        manifest["status"] = "FAIL"
        manifest["error"] = repr(exc)
        manifest["failed_unix"] = time.time()
        raise
    finally:
        cleanup_failure = None
        for record in reversed(processes):
            if record["proc"].poll() is None:
                try:
                    if "capture_path" in record:
                        capture_results.append(stop_capture(record))
                    else:
                        daemon_stops.append(stop_daemon(record))
                except Exception as cleanup_exc:
                    cleanup_failure = repr(cleanup_exc)
        try:
            if ndn is not None:
                Minindn.cleanUp()
            manifest["minindn_cleanup"] = "PASS"
        except Exception as cleanup_exc:
            cleanup_failure = repr(cleanup_exc)
            manifest["minindn_cleanup"] = "FAIL"
        if cleanup_failure is not None:
            manifest["status"] = "FAIL"
            manifest["cleanup_error"] = cleanup_failure
        manifest["pcap_traces"] = capture_results
        manifest["cleanup_results"] = daemon_stops
        write_json(args.output_dir / "run-manifest.json", manifest)
        if cleanup_failure is not None:
            raise RuntimeError("MiniNDN cleanup failed: " + cleanup_failure)


if __name__ == "__main__":
    raise SystemExit(main())
