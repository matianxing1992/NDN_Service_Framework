#!/usr/bin/env python3
"""Bounded MiniNDN campaign using installed native Qwen/Repo applications."""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import sqlite3
import stat
import subprocess
import sys
import time

import run_spec192_real_cpp_repo as repo
from spec212_qwen_profile import prepare, qwen, write_json
from native_probe_validation_links import render_anchored_probe_validation_schema

SERVICE = "/Spec212/DI/Infer"
PROVIDERS = [repo.REPO_B_PROVIDER, repo.CONSUMER]
PROVIDER_HOSTS = [repo.REPO_B_HOST, repo.CONSUMER_HOST]
AUTHORITY = repo.DI_PROVIDER
LARGE_DATA_FETCH_TIMEOUT_MS = 600_000
# Attempt50 exhausted 1400s before source assembly finished. Allow the existing
# 2400s assembly limit plus control margin; the prepared destination is separate.
MODEL_ASSEMBLY_TIMEOUT_MS = 2_400_000
EVENT_REQUEST_TIMEOUT_MS = MODEL_ASSEMBLY_TIMEOUT_MS + 120_000
CONTINUATION_REQUEST_TIMEOUT_MS = 300_000
BINARY_NAMES = ("App_ServiceController", "DI_NativeArtifactAuthority", "di-native-provider",
                "DI_NativeRequester", "DI_NativeOnnxAssemblyWorker", "DistributedRepoServer",
                "spec212-continuation-oracle", "spec192-repo-client")


def native_turn_budgets(text):
    return [tuple(map(int, values)) for values in re.findall(
        r"^NATIVE_CONVERSATION_TURN_BUDGET index=(\d+) timeoutMs=(\d+) ackTimeoutMs=(\d+)$",
        text, flags=re.MULTILINE)]


def seal_binaries(prefix: Path):
    result = {}
    library_hashes = {}
    for name in BINARY_NAMES:
        relative = (Path("libexec/ndnsf-di") / name if name == "DI_NativeOnnxAssemblyWorker"
                    else Path("bin") / name)
        path = (prefix / relative).resolve(strict=True)
        resolved = subprocess.check_output(["ldd", str(path)], text=True, stderr=subprocess.STDOUT)
        resolved = re.sub(r"\s+\(0x[0-9a-fA-F]+\)", "", resolved)
        if "not found" in resolved or str(repo.REPO) in resolved or "libpython" in resolved.lower():
            raise RuntimeError(f"installed native runtime closure failed: {path}")
        libraries = {}
        for line in resolved.splitlines():
            for token in line.split():
                if token.startswith("/") and Path(token).is_file():
                    library = Path(token).resolve(strict=True)
                    if str(library) not in library_hashes:
                        library_hashes[str(library)] = repo.digest(library)
                    libraries[str(library)] = library_hashes[str(library)]
        result[name] = {"path": str(path), "sha256": repo.digest(path), "ldd": resolved,
                        "libraries": libraries}
    return result


def policy(providers, role):
    provider_grants = {AUTHORITY: ["/HELLO"],
                       repo.REPO_A_PROVIDER: [repo.SERVICE_PREFIX + "/" + operation
                                              for operation in (*repo.REPO_OPERATIONS, *repo.OWNER_REPO_OPERATIONS)]}
    for provider in providers:
        provider_grants[provider] = [SERVICE, SERVICE + "/ROLE" + role, "/NDNSF/Network/Probe/v1"]
    user_grants = {repo.USER: [SERVICE, "/HELLO", "/NDNSF/Network/Probe/v1",
                              *[repo.SERVICE_PREFIX + "/" + operation for operation in
                                (*repo.REPO_OPERATIONS, *repo.OWNER_REPO_OPERATIONS)]]}
    for provider in providers:
        user_grants[provider] = [repo.SERVICE_PREFIX + "/" + operation
                                 for operation in repo.REPO_OPERATIONS]
    def blocks(kind, grants):
        return kind + "-policies\n{\n" + "".join(
            f" {kind}-policy\n {{\n  for {identity}\n  allow {{ " + "\n   ".join(allowed) + " }\n }\n"
            for identity, allowed in grants.items()) + "}\n"
    return (f"name {repo.CONTROLLER}/NDNSF/ControllerPolicy/v1\n" +
            blocks("provider", provider_grants) + blocks("user", user_grants))


def wait(record, marker, seconds):
    repo.wait_log(record, marker, seconds)


class ProviderTerminalFailureMonitor:
    def __init__(self, provider_log, log_offset, provider_label):
        self.provider_label = provider_label
        self.log = provider_log.open("rb")
        self.log.seek(log_offset)
        self.assembly_failures = {}

    def check(self):
        for raw_line in self.log:
            line = raw_line.decode("utf-8", errors="replace")
            fields = dict(re.findall(r"([A-Za-z][A-Za-z0-9]*)=([^\s]+)", line))
            request_id = fields.get("requestId")
            if "NDNSF_DI_ASSEMBLY_FAILURE" in line and request_id:
                self.assembly_failures[request_id] = fields.get(
                    "error", "assembly failed")
            if ("NDNSF_DI_PROVIDER_STAGE" in line and
                    fields.get("stage") == "TERMINAL" and
                    fields.get("status") == "failed"):
                failure = self.assembly_failures.get(request_id)
                reason = fields.get("reason")
                details = [value for value in (failure, reason) if value]
                if not details:
                    details = ["unknown Provider failure"]
                raise RuntimeError(
                    f"{self.provider_label} Provider terminal failure: " +
                    "; ".join(details))

    def close(self):
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.close()




def wait_for_log_marker_or_provider_terminal_failure(
        process, process_log, marker, provider_monitor, seconds):
    deadline = time.monotonic() + seconds
    marker_bytes = marker.encode("utf-8")
    with process_log.open("rb") as log:
        while True:
            for raw_line in log:
                if marker_bytes in raw_line:
                    return
            provider_monitor.check()
            returncode = process.poll()
            if returncode is not None:
                raise RuntimeError(f"requester exited before {marker}: exit={returncode}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(process.args, seconds)
            time.sleep(min(0.25, remaining))


def observe_libraries(record, sealed_libraries):
    libraries = {}
    for line in Path(f"/proc/{record['pid']}/maps").read_text().splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) != 6 or not fields[5].startswith("/") or ".so" not in fields[5]:
            continue
        path = Path(fields[5]).resolve(strict=True)
        if repo.REPO == path or repo.REPO in path.parents or "libpython" in path.name.lower():
            raise RuntimeError("native process loaded a checkout/Python library: " + str(path))
        if str(path) not in libraries:
            libraries[str(path)] = repo.digest(path)
        if sealed_libraries.get(str(path)) != libraries[str(path)]:
            raise RuntimeError("runtime library is outside the sealed closure: " + str(path))
    if not libraries:
        raise RuntimeError("native loaded-library evidence missing: " + record["label"])
    record["loaded_libraries"] = libraries


def owned_state(directory):
    snapshot = {}
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or path.suffix not in (".sqlite", ".sqlite3"):
            continue
        with sqlite3.connect(path.as_uri() + "?mode=ro", uri=True) as connection:
            connection.row_factory = sqlite3.Row
            tables = {row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            if "repo_user_storage_claims" not in tables:
                continue
            table_snapshot = {}
            for table in ("repo_user_storage_claims", "repo_user_storage_writes"):
                rows = [dict(row) for row in connection.execute(f'SELECT * FROM "{table}"')]
                rows.sort(key=lambda row: json.dumps(row, sort_keys=True))
                table_snapshot[table] = {"rows": len(rows), "values": rows}
            snapshot[str(path.relative_to(directory))] = table_snapshot
    return snapshot




def repo_payload_snapshot(path: Path):
    if not path.is_file():
        return {"exists": False}
    info = path.stat()
    return {"exists": True, "size_bytes": info.st_size, "sha256": repo.digest(path),
            "inode": info.st_ino, "mtime_ns": info.st_mtime_ns,
            "ctime_ns": info.st_ctime_ns}


def wait_for_source_exit_before_append(source, requester, timeout):
    deadline = time.monotonic() + timeout
    while source.poll() is None:
        requester_exit = requester.poll()
        if requester_exit is not None:
            raise RuntimeError(
                f"requester exited while waiting for source close: exit={requester_exit}")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise subprocess.TimeoutExpired(source.args, timeout)
        try:
            return source.wait(timeout=min(0.25, remaining))
        except subprocess.TimeoutExpired:
            pass
    return source.returncode


def verify_producer_packet_storage(root: Path, deadline: float):
    """Read committed producer packets before releasing the main conversation."""
    report = {"status": "FAIL_STOPPED", "objects": 0, "bytes": 0}
    identities = hashlib.sha256()
    try:
        manifests = sorted((root / "manifests").glob("*.json"))
        if not manifests or len(manifests) > 65536:
            raise RuntimeError("producer manifest inventory is empty or exceeds its bound")
        for path in manifests:
            if time.monotonic() >= deadline:
                raise RuntimeError("producer readback exceeded the prewarm barrier budget")
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode) or info.st_size > 16 * 1024 * 1024:
                raise RuntimeError("producer manifest is not a bounded regular file")
            manifest = json.loads(path.read_text())
            if not isinstance(manifest, dict):
                raise RuntimeError("producer manifest is not an object")
            if manifest.get("objectType") == "encrypted-large-data-envelope":
                continue
            if (manifest.get("objectType") != "original-packet-set" or
                    manifest.get("lifecycleState") != "COMMITTED"):
                raise RuntimeError("producer manifest is not a committed original packet set")
            expected = manifest.get("sha256", "")
            size = manifest.get("size")
            if (not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected) or
                    type(size) is not int or size <= 0 or
                    report["bytes"] + size > 4 * 1024 ** 3):
                raise RuntimeError("producer payload identity or size is invalid")
            payload = root / "payloads" / "sha256" / expected[:2] / expected
            identity = lambda value: (value.st_dev, value.st_ino, value.st_size,
                                      value.st_mtime_ns, value.st_ctime_ns)
            before = payload.lstat()
            if not stat.S_ISREG(before.st_mode) or before.st_size != size:
                raise RuntimeError("producer payload is missing or has an invalid size/type")
            digest = hashlib.sha256()
            with os.fdopen(os.open(str(payload), os.O_RDONLY | os.O_NOFOLLOW), "rb") as stream:
                if identity(os.fstat(stream.fileno())) != identity(before):
                    raise RuntimeError("producer payload changed before readback")
                for block in iter(lambda: stream.read(1048576), b""):
                    if time.monotonic() >= deadline:
                        raise RuntimeError("producer readback exceeded the prewarm barrier budget")
                    digest.update(block)
                after = os.fstat(stream.fileno())
            if (identity(before) != identity(after) or
                    identity(after) != identity(payload.lstat()) or
                    identity(info) != identity(path.lstat())):
                raise RuntimeError("producer payload changed during readback")
            actual = digest.hexdigest()
            if actual != expected:
                report.update(manifest=path.name, expected_sha256=expected, actual_sha256=actual)
                raise RuntimeError("producer committed payload digest mismatch")
            identities.update((path.name + ":" + expected + "\n").encode("ascii"))
            report["objects"] += 1
            report["bytes"] += size
        if not report["objects"]:
            raise RuntimeError("producer original packet inventory is empty")
        report.update(status="PASS", inventory_sha256=identities.hexdigest())
    except (OSError, ValueError, TypeError, RuntimeError) as error:
        report["error"] = str(error)
    return report




def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    work_root = args.work_root.resolve(strict=True)
    if output == work_root or output in work_root.parents or work_root in output.parents:
        raise ValueError("durable evidence and owned scratch must be separate")
    cache_candidate = re.sub(r"[^A-Za-z0-9._-]", "_", args.model_dir.name)
    artifact_cache_root = work_root.parent / "provider-artifact-cache" / cache_candidate
    artifact_cache_root.mkdir(parents=True, exist_ok=True, mode=0o700)
    artifact_cache_dirs = {
        provider: artifact_cache_root / re.sub(r"[^A-Za-z0-9._-]", "_", provider)
        for provider in PROVIDERS
    }
    for path in artifact_cache_dirs.values():
        path.mkdir(parents=True, exist_ok=True, mode=0o700)
    for path in [work_root.parent, artifact_cache_root, *artifact_cache_dirs.values()]:
        info = path.lstat()
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or
                info.st_mode & 0o077):
            raise RuntimeError(f"provider artifact cache path is not private and process-owned: {path}")
    artifact_cache_access = {
        str(path): {"uid": path.lstat().st_uid,
                    "mode": oct(stat.S_IMODE(path.lstat().st_mode))}
        for path in [work_root.parent, artifact_cache_root, *artifact_cache_dirs.values()]
    }
    binaries = seal_binaries(args.prefix)
    sealed_libraries = {path: digest for binary in binaries.values()
                        for path, digest in binary["libraries"].items()}
    seal = "sha256:" + hashlib.sha256(json.dumps(binaries, sort_keys=True).encode()).hexdigest()
    receipt = {"status": "RUNNING", "event": args.event, "withdrawal_scope": args.withdrawal_scope,
               "binaries": binaries,
               "candidate_digest": seal,
               "native_large_data_fetch_timeout_ms": LARGE_DATA_FETCH_TIMEOUT_MS,
               "native_request_large_data_retention_ms": int(os.environ.get(
                   "NDNSF_REQUEST_LARGE_DATA_RETENTION_MS", "300000")),
               "provider_artifact_cache_dirs": {
                   provider: str(path) for provider, path in artifact_cache_dirs.items()},
                "provider_artifact_cache_access": artifact_cache_access,
               "started_unix": time.time()}
    write_json(output / "result.json", receipt)
    processes = []
    ndn = None
    source_payload_path = None
    try:
        from minindn.apps.app_manager import AppManager
        from minindn.apps.nfd import Nfd
        from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
        from minindn.helpers.nfdc import Nfdc
        from minindn.minindn import Minindn
        saved_argv = sys.argv
        sys.argv = [sys.argv[0]]
        try:
            ndn = Minindn(topoFile=str(args.topology.resolve()))
        finally:
            sys.argv = saved_argv
        ndn.start()
        nfd_instances = []

        def configured_nfd(node, **options):
            app = Nfd(node, **options)
            nfd_instances.append(app)
            return app

        try:
            AppManager(ndn, ndn.net.hosts, configured_nfd, csSize=256, logLevel="INFO")
        except BaseException:
            # AppManager registers cleanup only after constructing every Nfd.
            for app in reversed(nfd_instances):
                try:
                    app.stop()
                except Exception as cleanup_error:
                    receipt.setdefault("nfd_startup_cleanup_errors", []).append({
                        "host": app.node.name, "error_type": type(cleanup_error).__name__})
            raise
        homes = {host: repo.set_host_ndn_config(ndn.net[host]) for host in repo.HOST_ROLES}
        repo.perf.wait_for_nfd_sockets(ndn, output)
        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        for host, identity in [(repo.CONTROLLER_HOST, repo.CONTROLLER), (repo.APP_HOST, repo.USER),
                               (repo.APP_HOST, AUTHORITY), (repo.REPO_A_HOST, repo.REPO_A_PROVIDER),
                               *zip(PROVIDER_HOSTS, PROVIDERS)]:
            routing.addOrigin([ndn.net[host]], [identity, identity + "/KEY", repo.GROUP])
        routing.addOrigin([ndn.net[repo.REPO_A_HOST]], [repo.SERVICE_PREFIX])
        routing.calculateRoutes()
        time.sleep(3)
        for host in ndn.net.hosts:
            Nfdc.setStrategy(host, "/example/repo", Nfdc.STRATEGY_MULTICAST)
            Nfdc.setStrategy(host, repo.SERVICE_PREFIX, Nfdc.STRATEGY_MULTICAST)
        identities = repo.generate_identities(ndn, output)
        certificates = {identity: output / "identity" / (host + ".cert")
                        for host, identity in identities["identities"].items()}
        certificates.update({identity: Path(path) for identity, path in
                             identities["target_public_certificates"].items()})
        def node_env(host):
            env = qwen.env_for(homes[host], host, artifact_cache_root=artifact_cache_root)
            env.update({"NDN_CLIENT_PIB": "pib-sqlite3:", "NDN_CLIENT_TPM": "tpm-file:",
                        "NDNSF_CONTROLLER_CERT_FILE": str(certificates[repo.CONTROLLER]),
                        "NDNSF_PHASE_TIMING": "1"})
            retention = os.environ.get("NDNSF_REQUEST_LARGE_DATA_RETENTION_MS", "")
            if retention:
                env["NDNSF_REQUEST_LARGE_DATA_RETENTION_MS"] = retention
            return env
        imports = [(host, certificate) for host in repo.HOST_ROLES
                   for certificate in dict.fromkeys(certificates.values())]
        for index, (host, certificate) in enumerate(imports):
            repo.run_child(ndn, output, processes, host, f"public-certificate-{index}",
                           [binaries["spec192-repo-client"]["path"], "--mode",
                            "import-public-certificate", "--certificate", str(certificate)],
                           node_env(host), 15, require_no_python=True)
        for host, identity in identities["identities"].items():
            repo.set_default_identity(ndn.net[host], identity,
                                      repo.perf.certificate_name_from_file(certificates[identity]))
        schema = output / "trust-schema.conf"
        schema.write_text(render_anchored_probe_validation_schema("/example/repo", certificates.values()))
        target = {"repo_node": repo.REPO_A_ID, "provider_identity": repo.REPO_A_PROVIDER,
                  "service_prefix": repo.SERVICE_PREFIX}
        profile = prepare(output, args.model_dir.resolve(), work_root=work_root,
                          service=SERVICE, group=repo.GROUP,
                          controller=repo.CONTROLLER, authority=AUTHORITY, owner=repo.USER,
                          providers=PROVIDERS, certificates=certificates, schema=schema,
                          repo_target=target, event=args.event, candidate_digest=seal,
                          withdrawal_scope=args.withdrawal_scope)
        receipt.update({"source_digest": profile["source_digest"], "tokenizer_digest": profile["tokenizer_digest"],
                        "topology_sha256": repo.digest(args.topology), "identities": identities})
        source_repository_root = Path(json.loads(
            (profile["requester"] / "config.json").read_text())["repository"]["path"])
        source_payload_digest = profile["source_digest"].split(":", 1)[-1]
        source_payload_path = (source_repository_root / "native-artifact-api" / "payloads" /
                               "sha256" / source_payload_digest[:2] / source_payload_digest)
        write_json(output / "result.json", receipt)
        policy_path = output / "controller-policy.info"
        policy_path.write_text(policy(PROVIDERS, profile["role"]))
        def launch(host, label, binary, argv, extra=None):
            env = node_env(host)
            env.update(extra or {})
            return repo.spawn(ndn, output, processes, host, label,
                              [binaries[binary]["path"], *map(str, argv)], env)
        controller = launch(repo.CONTROLLER_HOST, "controller", "App_ServiceController", [
            "--controller-prefix", repo.CONTROLLER, "--policy-file", policy_path,
            "--trust-schema", schema,
            "--ensure-identities", ",".join([AUTHORITY, repo.USER, repo.REPO_A_PROVIDER, *PROVIDERS]),
            "--run-for-ms", "12300000"],
            {"NDNSF_CONTROLLER_GENERATION_STATE": str(output / "controller-generation.state")})
        wait(controller, "ServiceController started", 60)
        observe_libraries(controller, sealed_libraries)
        authority = launch(repo.APP_HOST, "authority", "DI_NativeArtifactAuthority", [
            "--config", profile["authority"] / "config.json"])
        wait(authority, "NATIVE_GRANT_AUTHORITY_READY", 60)
        observe_libraries(authority, sealed_libraries)
        repo_dir = output / "repo"
        repo_dir.mkdir(mode=0o700)
        server = launch(repo.REPO_A_HOST, "repo", "DistributedRepoServer", [
            "--repo-node", repo.REPO_A_ID, "--database", repo_dir / "repo.sqlite",
            "--group-prefix", repo.GROUP, "--provider-identity", repo.REPO_A_PROVIDER,
            "--authority-identity", repo.CONTROLLER, "--trust-schema", schema,
            "--service-prefix", repo.SERVICE_PREFIX, "--failure-domain", repo.REPO_A_HOST,
            "--ack-collection-ms", "250", "--request-timeout-ms", "120000"])
        wait(server, "Persistent Repo serving", 60)
        observe_libraries(server, sealed_libraries)
        preflight_owned_before = owned_state(repo_dir) if args.configuration_preflight_only else None
        provider_records = []
        for i, (host, provider) in enumerate(zip(PROVIDER_HOSTS, PROVIDERS)):
            directory = profile["provider_dirs"][i]
            record = launch(host, f"provider-{i}", "di-native-provider", [
                "--plan", profile["plan"], "--manifest", profile["manifest"], "--service", SERVICE,
                "--provider", provider, "--group", repo.GROUP, "--controller", repo.CONTROLLER,
                "--trust-schema", schema, "--roles", profile["role"], "--serve", "--run-for-ms", "12300000",
                "--artifact-cache-dir", artifact_cache_dirs[provider], "--repo-fetch-timeout-ms",
                str(LARGE_DATA_FETCH_TIMEOUT_MS),
                "--assembly-timeout-ms", str(MODEL_ASSEMBLY_TIMEOUT_MS), "--conversation-retention-ms", "3600000",
                "--tokenizer-json", args.model_dir.resolve() / "tokenizer.json",
                "--selection-offer-key-file", directory / "offer-private.pem",
                "--offer-backend", "onnxruntime-cpu", "--offer-can-provision", "--offer-has-model",
                "--kv-recovery-config", directory / "recovery.json", "--kv-handoff-config", directory / "handoff.json",
                *(["--check-kv-handoff-config"] if args.configuration_preflight_only else [])],
                # KV recovery uses its explicit Repo route from recovery.json.
                {"SPEC181_GRANT_AUTHORITY_PUBLIC_KEY": str(directory / "authority-public.pem"),
                 "SPEC181_PROVIDER_RECIPIENT_KEY_MAP": str(directory / "recipient-key-map.json"),
                 "NDNSF_DI_WORKER_BINARY": binaries["DI_NativeOnnxAssemblyWorker"]["path"],
                 # ServiceProvider's protected large-data fetch has a separate
                 # 30s default. Align it with the existing Repo fetch bound;
                 # the fetch bound remains below the fixed event and warm-up
                 # request budgets; Provider assembly has its own unchanged cap.
                 "NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS": str(LARGE_DATA_FETCH_TIMEOUT_MS),
                 # The source-side Qwen artifact is about 754 MB. Keep the
                 # existing 600 s deadline and widen the constant NDN window
                 # after Attempt23 hit that bound during source retrieval.
                 "NDNSF_REQUEST_LARGE_FETCH_INIT_CWND": "32",
                 })
            provider_records.append(record)
            if args.configuration_preflight_only:
                exit_code = record["proc"].wait(timeout=60)
                if exit_code != 0 or "NDNSF_DI_HANDOFF_CONFIG_CHECK_PASS" not in record["log"].read_text():
                    raise RuntimeError(f"Provider configuration preflight failed: exit={exit_code}; {record['log']}")
                continue
            wait(record, "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY", 90)
            observe_libraries(record, sealed_libraries)

        if args.configuration_preflight_only:
            preflight_owned_after = owned_state(repo_dir)
            if preflight_owned_after != preflight_owned_before:
                raise RuntimeError("Provider configuration checks changed Repo user-storage claims/writes")
            if seal_binaries(args.prefix) != binaries:
                raise RuntimeError("installed closure changed during Provider configuration checks")
            receipt.update({"status": "CONFIGURATION_PREFLIGHT_PASS_NOT_EVENT_ACCEPTANCE",
                            "event_acceptance": False,
                            "configuration_preflight_owned_before": preflight_owned_before,
                            "configuration_preflight_owned_after": preflight_owned_after,
                            "configuration_preflight_provider_count": len(provider_records)})
            return

        # Keep the requester Runtime and Producer face alive between the
        # destination warm-up and the event conversation. The two conversation
        # IDs remain distinct, so warm-up KV cannot seed the migrated session.
        main_config_path = profile["requester"] / "config.json"
        main_config = json.loads(main_config_path.read_text())
        if len(main_config.get("turns", [])) != 2:
            raise RuntimeError("event profile requires exactly two turns")
        main_config["request"]["timeout_ms"] = EVENT_REQUEST_TIMEOUT_MS
        main_config["turns"][1]["request_timeout_ms"] = CONTINUATION_REQUEST_TIMEOUT_MS
        write_json(main_config_path, main_config)
        receipt["event_request_timeout_ms"] = EVENT_REQUEST_TIMEOUT_MS
        receipt["continuation_request_timeout_ms"] = CONTINUATION_REQUEST_TIMEOUT_MS
        write_json(output / "result.json", receipt)
        main_config = json.loads(main_config_path.read_text())
        expected_handoff_barrier_ms = (
            650000 if args.event == "departure" and args.withdrawal_scope == "provider"
            else 60000)
        if main_config["test_handoff_barrier"]["timeout_ms"] != expected_handoff_barrier_ms:
            raise RuntimeError("handoff test barrier does not cover the source exit profile")
        receipt["configured_handoff_barrier_timeout_ms"] = expected_handoff_barrier_ms
        main_config["limits"]["bootstrap_ms"] = 100000
        main_config["limits"]["preparation_timeout_ms"] = 1200000
        main_config["request"]["timeout_ms"] = EVENT_REQUEST_TIMEOUT_MS
        main_conversation_id = main_config["conversation"]["turn"]["conversation_id"]
        prewarm_conversation_id = main_conversation_id + "-destination-prewarm"
        main_config["conversation"]["turn"]["retention_deadline_ms"] = int(
            time.time() * 1000) + 3600000
        # Prewarm the model in a separate conversation before the event;
        # event turns retain their own request and handoff deadlines.
        main_config["test_destination_prewarm"] = {
            "conversation_id": prewarm_conversation_id,
            "request": {
                "provider_names": [PROVIDERS[1]],
                "timeout_ms": 2520000,
            },
            "turns": [{
                "input_file": "input.bin",
                "options_file": "options.json",
                "output_file": "destination-prewarm-output.json",
                "checkpoint_output_file": "destination-prewarm-checkpoint.json",
            }],
            "barrier": {
                "ready_file": "destination-prewarm.ready",
                "release_file": "destination-prewarm.release",
                "timeout_ms": 60000,
            },
        }
        source_timeout = main_config["request"]["timeout_ms"]
        continuation_timeout = main_config["turns"][1]["request_timeout_ms"]
        ack_timeout = main_config["request"]["ack_timeout_ms"]
        if (source_timeout != EVENT_REQUEST_TIMEOUT_MS or
                continuation_timeout != CONTINUATION_REQUEST_TIMEOUT_MS or
                not 0 < ack_timeout < continuation_timeout <= source_timeout or
                source_timeout + continuation_timeout +
                main_config["kv_handoff"]["timeout_ms"] + 50000 >= 3600000):
            raise RuntimeError("event turn budgets do not fit the frozen profile")
        expected_event_budgets = [(0, 2520000, ack_timeout),
                                  (0, source_timeout, ack_timeout),
                                  (1, continuation_timeout, ack_timeout)]
        receipt["configured_turn_timeouts_ms"] = [source_timeout, continuation_timeout]
        write_json(main_config_path, main_config)
        destination_provider_log = provider_records[1]["log"]
        destination_provider_log_offset = destination_provider_log.stat().st_size
        source_provider_log = provider_records[0]["log"]
        source_provider_log_offset = source_provider_log.stat().st_size
        requester = launch(
            repo.APP_HOST, "requester", "DI_NativeRequester",
            ["--config", main_config_path],
            {"NDNSF_REQUEST_LARGE_IMS_LIMIT": str(qwen.large_data_ims_limit(
                (args.model_dir / "model_quantized.onnx").stat().st_size, 0))})
        with ProviderTerminalFailureMonitor(
                destination_provider_log, destination_provider_log_offset,
                "destination") as destination_monitor:
            wait_for_log_marker_or_provider_terminal_failure(
                requester["proc"], requester["log"],
                "NATIVE_TEST_EXECUTION_BARRIER_READY name=test_destination_prewarm",
                destination_monitor, 3920)
        if requester["proc"].poll() is not None:
            raise RuntimeError("requester exited at the destination-prewarm barrier")
        destination_prewarm_log = requester["log"].read_text(errors="replace")
        prewarm_turns = re.findall(
            r"^NATIVE_CONVERSATION_TURN_COMPLETE index=(\d+) request=",
            destination_prewarm_log, flags=re.MULTILINE)
        prewarm_successes = re.findall(
            r"^NATIVE_DESTINATION_PREWARM_SUCCEEDED warmupConversation=(\S+) "
            r"mainConversation=(\S+) request=(\S+)$",
            destination_prewarm_log, flags=re.MULTILINE)
        if (prewarm_turns != ["0"] or len(prewarm_successes) != 1 or
                prewarm_successes[0][0] != prewarm_conversation_id or
                prewarm_successes[0][1] != main_conversation_id or
                prewarm_successes[0][0] == prewarm_successes[0][1]):
            raise RuntimeError(
                "same-Runtime destination prewarm failed its distinct-conversation "
                f"check: {requester['log']}")
        qwen.validate_native_output(
            profile["requester"] / "destination-prewarm-output.json")
        prewarm_payload_snapshot = repo_payload_snapshot(source_payload_path)
        receipt["source_repo_payload_after_destination_prewarm"] = prewarm_payload_snapshot
        write_json(output / "result.json", receipt)
        if (not prewarm_payload_snapshot.get("exists") or
                prewarm_payload_snapshot.get("size_bytes") !=
                (args.model_dir / "model_quantized.onnx").stat().st_size or
                prewarm_payload_snapshot.get("sha256") != source_payload_digest):
            raise RuntimeError(
                "source Repo payload failed full verification after destination prewarm")
        readback = verify_producer_packet_storage(
            work_root / "encrypted-repository", time.monotonic() + 60)
        receipt["producer_packet_storage_after_destination_prewarm"] = readback
        write_json(output / "result.json", receipt)
        if readback["status"] != "PASS":
            raise RuntimeError("producer packet storage readback failed: " + readback["error"])
        destination_prewarm_log_size = destination_provider_log.stat().st_size
        receipt["destination_prewarm_request_succeeded"] = True
        receipt["destination_prewarm_output"] = str(
            profile["requester"] / "destination-prewarm-output.json")
        write_json(output / "result.json", receipt)

        pre_event_events = [
            line for line in server["log"].read_text(errors="replace").splitlines()
            if "repo-original-data-served name=" in line]
        if pre_event_events:
            raise RuntimeError("Repo served original KV Data before the handoff event")
        receipt["repo_original_data_served_before_event"] = 0
        recovery_hints = {
            provider: json.loads((directory / "recovery.json").read_text())["repoForwardingHints"]
            for provider, directory in zip(PROVIDERS, profile["provider_dirs"])
        }
        if any(hints != [repo.REPO_A_PROVIDER] for hints in recovery_hints.values()):
            raise RuntimeError("KV recovery must use the configured Repo provider route")
        receipt["kv_recovery_forwarding_hints"] = recovery_hints
        write_json(output / "result.json", receipt)
        if (profile["requester"] / "destination-prewarm.ready").read_text() != \
                "test_destination_prewarm\n":
            raise RuntimeError("requester destination-prewarm barrier marker is malformed")
        (profile["requester"] / "destination-prewarm.release").write_text(
            "destination-output-validated\n", encoding="ascii")
        with ProviderTerminalFailureMonitor(
                source_provider_log, source_provider_log_offset, "source") as source_monitor:
            if args.event == "departure":
                # The conversation starts after Runtime/User.prepare and the
                # destination prewarm. Allow the source turn plus 400s for
                # watcher marker delivery and process scheduling.
                wait_for_log_marker_or_provider_terminal_failure(
                    requester["proc"], requester["log"],
                    "NATIVE_KV_DEPARTURE_WATCH_READY", source_monitor,
                    EVENT_REQUEST_TIMEOUT_MS // 1000 + 400)
                withdrawal_started = time.monotonic()
                withdrawal_deadline = withdrawal_started + 650
                receipt["withdrawal_signal_unix"] = time.time()
                provider_records[0]["proc"].send_signal(signal.SIGUSR1)
            barrier = profile["requester"] / "handoff.ready"
            # The event conversation starts after preparation and destination
            # prewarm. Switch allows the source turn, a 600s handoff, and
            # 400s of marker/scheduling margin before this barrier.
            barrier_wait_s = (650 if args.event == "departure" else
                              EVENT_REQUEST_TIMEOUT_MS // 1000 + 1000)
            barrier_deadline = (withdrawal_deadline if args.event == "departure" else
                                time.monotonic() + barrier_wait_s)
            while not barrier.exists():
                source_monitor.check()
                if requester["proc"].poll() is not None:
                    raise RuntimeError(
                        "requester terminated before the post-handoff barrier")
                if time.monotonic() >= barrier_deadline:
                    raise RuntimeError("post-handoff barrier timed out")
                time.sleep(0.05)
        source = provider_records[0]
        observe_libraries(requester, sealed_libraries)
        observe_libraries(provider_records[1], sealed_libraries)
        if args.event == "departure" and args.withdrawal_scope == "service":
            wait(source, "scope=service closed=1 forced=0", 20)
        if args.event == "switch" or args.withdrawal_scope == "service":
            source["proc"].terminate()
        source_exit_wait_s = 50
        if args.event == "departure" and args.withdrawal_scope == "provider":
            source_exit_wait_s = max(0, withdrawal_deadline - time.monotonic())
            receipt["withdrawal_exit_wait_remaining_s"] = source_exit_wait_s
        source_exit = wait_for_source_exit_before_append(
            source["proc"], requester["proc"], source_exit_wait_s)
        if args.event == "departure":
            receipt["withdrawal_signal_to_exit_s"] = time.monotonic() - withdrawal_started
        if source_exit != 0:
            raise RuntimeError(f"source did not close cleanly before APPEND: exit={source_exit}")
        campaign_state = {
            "source_stopped_before_append": True, "source_exit": source_exit,
            "source_pid": source["pid"], "delta_tokens": len(profile["delta_ids"]),
            "closed_unix": time.time(), "withdrawal_scope": args.withdrawal_scope,
            "destination_runner_prepared_before_event": True,
            "repo_original_data_served_before_event":
                receipt["repo_original_data_served_before_event"],
            "kv_recovery_forwarding_hints": receipt["kv_recovery_forwarding_hints"]}
        write_json(output / "campaign-state.json", campaign_state)
        (profile["requester"] / "handoff.release").write_text("source-closed\n")
        # The prepared destination APPEND gets its turn budget plus 100s exit
        # grace; both requests and the handoff must fit the conversation's
        # independently refreshed one-hour Owner deadline.
        post_append_wait_s = CONTINUATION_REQUEST_TIMEOUT_MS // 1000 + 100
        rc = requester["proc"].wait(timeout=post_append_wait_s)
        text = requester["log"].read_text(errors="replace")
        if rc != 0 or "NATIVE_CONVERSATION_TURNS_SUCCEEDED count=2" not in text:
            raise RuntimeError(f"native requester failed: exit={rc}; {requester['log']}")
        actual_event_budgets = native_turn_budgets(text)
        if actual_event_budgets != expected_event_budgets:
            raise RuntimeError("native event turn budgets differ from the frozen profile")
        receipt["native_turn_budgets_ms"] = actual_event_budgets
        repo_served_events = [
            dict(re.findall(r"([A-Za-z][A-Za-z0-9]*)=([^\s]+)", line))
            for line in server["log"].read_text(errors="replace").splitlines()
            if "repo-original-data-served name=" in line]
        if not repo_served_events or any(
                not event.get("name") or
                not re.fullmatch(r"[0-9a-f]{64}", event.get("sha256", ""))
                for event in repo_served_events):
            raise RuntimeError(
                "switch/departure event did not record valid Repo original-Data service")
        campaign_state["repo_original_data_served_count"] = len(repo_served_events)
        campaign_state["repo_original_data_served"] = repo_served_events
        write_json(output / "campaign-state.json", campaign_state)
        destination_provider_text = destination_provider_log.read_text(errors="replace")
        destination_event_log = destination_provider_text[destination_prewarm_log_size:]
        destination_event_fields = [
            (line, dict(re.findall(r"([A-Za-z][A-Za-z0-9]*)=([^\s]+)", line)))
            for line in destination_event_log.splitlines()]
        event_attempts = {
            (fields["requestId"], fields["attemptEpoch"])
            for _, fields in destination_event_fields
            if fields.get("stage") == "ASSEMBLY_STARTED" and
            fields.get("provider") == PROVIDERS[1] and
            fields.get("role") == profile["role"] and
            fields.get("requestId") and fields.get("attemptEpoch")
        }
        reuse_hits = [
            line for line, fields in destination_event_fields
            if fields.get("phase") == "RUNNER_REUSE_HIT" and
            fields.get("provider") == PROVIDERS[1] and
            fields.get("role") == profile["role"] and
            (fields.get("requestId"), fields.get("attemptEpoch")) in event_attempts
        ]
        if not reuse_hits:
            raise RuntimeError(
                "destination event did not reuse its prewarmed model runner "
                "for the same requestId/attemptEpoch")
        campaign_state.update({
            "destination_runner_reuse_hit": True,
            "destination_runner_reuse_marker_count": len(reuse_hits),
            "destination_runner_reuse_request_attempts": sorted(
                [list(item) for item in event_attempts])})
        write_json(output / "campaign-state.json", campaign_state)
        marker = "NATIVE_KV_HANDOFF_APPEND_SUCCEEDED" if args.event == "switch" else "NATIVE_KV_DEPARTURE_SAVED"
        if marker not in text:
            raise RuntimeError("requester handoff completion evidence missing")
        outputs = [qwen.validate_native_output(profile["requester"] / f"output-{i}.json") for i in range(2)]
        # Replay the complete history independently with identical request and
        # decode boundaries; this graph is not invariant to prefill batching.
        campaign_state["owned_before_baseline"] = owned_state(repo_dir)
        baseline_config = json.loads((profile["requester"] / "config.json").read_text())
        for key in ("kv_handoff", "test_handoff_barrier", "test_only",
                    "test_destination_prewarm"):
            baseline_config.pop(key, None)
        # Both baseline providers were independently warmed before the event;
        # keep the fresh history oracle at its original preparation and
        # per-request bounds, independent of the event budgets.
        baseline_config["limits"]["preparation_timeout_ms"] = 1200000
        baseline_config["request"]["timeout_ms"] = 1200000
        baseline_config["request"]["provider_names"] = [PROVIDERS[1]]
        baseline_config["request"]["options_file"] = "options.json"
        # B must generate its own parent history without an imported checkpoint.
        # The C++ oracle compares both generations and their checkpoint prefixes.
        full_ids = profile["first_ids"] + outputs[0]["tokenIds"] + profile["delta_ids"]
        campaign_state["reference_initial_tokens"] = len(profile["first_ids"])
        campaign_state["reference_context_tokens_before_append"] = len(full_ids) - len(profile["delta_ids"])
        campaign_state["reference_context_tokens_after_delta"] = len(full_ids)
        campaign_state["reference_execution"] = "independent-same-request-and-decode-boundaries"
        baseline_config["conversation"]["journal"]["state_root"] = "baseline-state"
        baseline_config["conversation"]["turn"]["conversation_id"] += "-baseline"
        baseline_config["conversation"]["turn"]["canonical_token_ids"] = profile["first_ids"]
        options = json.loads((profile["requester"] / "options.json").read_text())
        baseline_config["conversation"]["turn"]["generation_id"] = options["generationId"]
        baseline_config["turns"] = [
            {"input_file": "input.bin", "options_file": "options.json",
             "output_file": "baseline-0.json", "checkpoint_output_file": "baseline-checkpoint-0.json"},
            {"input_file": "delta.bin", "options_file": "options-1.json",
             "output_file": "baseline-1.json", "checkpoint_output_file": "baseline-checkpoint-1.json"}]
        baseline_config["conversation"]["turn"]["retention_deadline_ms"] = int(
            time.time() * 1000) + 3600000
        baseline_path = profile["requester"] / "baseline-config.json"
        write_json(baseline_path, baseline_config)
        baseline = launch(repo.APP_HOST, "baseline-requester", "DI_NativeRequester", ["--config", baseline_path],
                          {"NDNSF_REQUEST_LARGE_IMS_LIMIT": str(qwen.large_data_ims_limit(
                              (args.model_dir / "model_quantized.onnx").stat().st_size, 0))})
        # Cover Runtime bootstrap, preparation, both request deadlines and
        # exit grace without changing any native business deadline.
        baseline_wait_ms = (baseline_config["limits"]["bootstrap_ms"] +
                            baseline_config["limits"]["preparation_timeout_ms"] +
                            sum(turn.get("request_timeout_ms", baseline_config["request"]["timeout_ms"])
                                for turn in baseline_config["turns"]) + 100000)
        baseline_wait_s = (baseline_wait_ms + 999) // 1000
        receipt["baseline_process_wait_s"] = baseline_wait_s
        write_json(output / "result.json", receipt)
        baseline_rc = baseline["proc"].wait(timeout=baseline_wait_s)
        if baseline_rc != 0:
            raise RuntimeError(f"independent no-event history reference failed: exit={baseline_rc}")
        baseline_budgets = native_turn_budgets(baseline["log"].read_text())
        baseline_ack = baseline_config["request"]["ack_timeout_ms"]
        if baseline_budgets != [(0, 1200000, baseline_ack), (1, 1200000, baseline_ack)]:
            raise RuntimeError("native baseline turn budgets differ from the frozen profile")
        receipt["baseline_turn_budgets_ms"] = baseline_budgets
        campaign_state["owned_after_baseline"] = owned_state(repo_dir)
        observe_libraries(provider_records[1], sealed_libraries)
        provider_records[1]["proc"].terminate()
        destination_exit = provider_records[1]["proc"].wait(timeout=50)
        if destination_exit != 0:
            raise RuntimeError(f"destination cleanup failed: exit={destination_exit}")
        campaign_state["destination_exit"] = destination_exit
        campaign_state["owned_after_close"] = owned_state(repo_dir)
        campaign_state["checkpoint_scratch_files"] = [
            str(path.relative_to(work_root)) for index in range(2)
            for path in sorted((work_root / f"provider-{index}-scratch").rglob("*")) if path.is_file()]
        write_json(output / "campaign-state.json", campaign_state)
        oracle = launch(repo.APP_HOST, "native-oracle", "spec212-continuation-oracle", [output])
        oracle_rc = oracle["proc"].wait(timeout=30)
        if oracle_rc != 0 or "SPEC212_NATIVE_CONTINUATION_PASS" not in oracle["log"].read_text():
            raise RuntimeError(f"native continuation assertions failed: {oracle['log']}")
        if seal_binaries(args.prefix) != binaries:
            raise RuntimeError("installed binary/library content changed during the campaign")
        receipt.update({"status": "PROCESS_CHAIN_PASS_PENDING_FINAL_AUDIT", "requester_exit": rc,
                        "outputs": outputs, "source_exit": source_exit, "baseline_exit": baseline_rc,
                        "native_oracle_exit": oracle_rc, "native_oracle_log": str(oracle["log"])})
    except BaseException as error:
        receipt.update({"status": "FAIL_STOPPED", "first_failure": f"{type(error).__name__}: {error}"})
        raise
    finally:
        receipt["loaded_libraries"] = {record["label"]: record["loaded_libraries"]
                                       for record in processes if "loaded_libraries" in record}
        receipt["processes"] = [repo.stop_daemon(record, 15) for record in reversed(processes)]
        if ndn is not None:
            ndn.stop()
        receipt["finished_unix"] = time.time()
        if source_payload_path is not None:
            try:
                receipt["source_repo_payload_after_cleanup"] = repo_payload_snapshot(
                    source_payload_path)
            except Exception as snapshot_error:
                receipt["source_repo_payload_after_cleanup_error"] = (
                    f"{type(snapshot_error).__name__}: {snapshot_error}")
        write_json(output / "result.json", receipt)


def main(*, _supervised=False):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", choices=("switch", "departure"))
    parser.add_argument("--withdrawal-scope", default="provider", choices=("provider", "service"))
    parser.add_argument("--model-dir", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--work-root", type=Path,
                        help="exclusive owned disk scratch from ndnsf-di-workspace.py")
    parser.add_argument("--configuration-preflight-only", action="store_true",
                        help="check both production Provider configurations without model requests or KV events")
    parser.add_argument("--prefix", type=Path, default=Path("/usr/local"))
    parser.add_argument("--topology", type=Path, default=repo.REPO / "Experiments/Topology/AI_Lab.conf")
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("MiniNDN requires root; use the maintained sudo environment")
    if args.event is None or args.model_dir is None or args.work_root is None:
        parser.error("full campaigns require --event, --model-dir, and --work-root")
    if not _supervised:
        from native_resource_guard import run_guarded
        with Path("/run/lock/ndnsf-spec212-minindn.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            output = args.output.resolve()
            output.mkdir(parents=True, exist_ok=False, mode=0o700)
            worker = [sys.executable, "-c",
                      "import pathlib,runpy,sys; p=sys.argv[1]; "
                      "sys.path.insert(0,str(pathlib.Path(p).parent)); sys.argv=sys.argv[1:]; "
                      "m=runpy.run_path(p); raise SystemExit(m['main'](_supervised=True))",
                      str(Path(__file__).resolve()), *sys.argv[1:]]
            with (output / "supervisor.json").open("x") as receipt:
                # This is the campaign resource cap, including setup/cleanup;
                # individual request and handoff deadlines remain independent.
                result = run_guarded(worker, cwd=repo.REPO, stdout=None,
                                     sample_path=output / "resource-samples.jsonl",
                                     limits={"timeoutSeconds": (300 if args.configuration_preflight_only else 12000),
                                             "stopGraceSeconds": 15})
                json.dump(result, receipt, indent=2, sort_keys=True)
                receipt.write("\n")
            if result["boundary"] or result["cleanup"] != "PASS":
                return 1
            return result["returncode"] if result["returncode"] is not None else 1
    run(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
