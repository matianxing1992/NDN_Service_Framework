#!/usr/bin/env python3
"""Run deterministic NDNSF authorization evidence cases."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import shutil
import subprocess
import sys
import time
from types import SimpleNamespace
from typing import Any, Iterable

import jsonschema
import yaml


ROOT = Path(__file__).resolve().parents[1]
FEATURE = ROOT / "specs/172-data-centric-authorization-evaluation"
MARKER = re.compile(
    r"NDNSF_AUTH_CASE\s+case_id=(?P<case_id>\S+)\s+"
    r"terminal=(?P<terminal>allow|deny)\s+"
    r"observed_executions=(?P<executions>\d+)\s+"
    r"gate=(?P<gate>[A-Za-z0-9_]+)"
)
ONBOARDING_MARKER = re.compile(
    r"NDNSF_ONBOARDING_CASE\s+"
    r"stale_terminal=(?P<stale_terminal>deny)\s+"
    r"refreshed_terminal=(?P<refreshed_terminal>allow)\s+"
    r"stale_executions=(?P<stale_executions>\d+)\s+"
    r"refreshed_executions=(?P<refreshed_executions>\d+)\s+"
    r"old_epoch=(?P<old_epoch>\d+)\s+new_epoch=(?P<new_epoch>\d+)\s+"
    r"provider_manual_changes=(?P<provider_manual_changes>\d+)\s+"
    r"refresh_operations=(?P<refresh_operations>\d+)\s+"
    r"control_bytes=(?P<control_bytes>\d+)\s+"
    r"time_to_first_success_us=(?P<time_to_first_success_us>\d+)"
)
CRYPTO_COUNTER_MARKER = re.compile(
    r"NDNSF_AUTH_OVERHEAD_COUNTERS\s+role=(?P<role>\S+)\s+"
    r"hybrid_key_epochs=(?P<hybrid_key_epochs>\d+)\s+"
    r"abe_wraps=(?P<abe_wraps>\d+)\s+abe_unwraps=(?P<abe_unwraps>\d+)\s+"
    r"symmetric_encrypts=(?P<symmetric_encrypts>\d+)\s+"
    r"symmetric_decrypts=(?P<symmetric_decrypts>\d+)\s+"
    r"key_cache_hits=(?P<key_cache_hits>\d+)\s+"
    r"key_cache_misses=(?P<key_cache_misses>\d+)\s+"
    r"decrypt_failures=(?P<decrypt_failures>\d+)"
)
SCALE_MARKER = re.compile(
    r"NDNSF_AUTH_SCALE\s+users=(?P<users>\d+)\s+"
    r"providers=(?P<providers>\d+)\s+policy_terms=(?P<policy_terms>\d+)\s+"
    r"encryptions=(?P<encryptions>\d+)\s+decryptions=(?P<decryptions>\d+)\s+"
    r"response_bytes=(?P<response_bytes>\d+)\s+"
    r"encrypted_bytes=(?P<encrypted_bytes>\d+)\s+total_us=(?P<total_us>\d+)"
)
PUBLICATION_AUDIT_MARKER = re.compile(
    r"NDNSF_PUBLICATION_AUDIT\s+role=(?P<role>user|provider)\s+"
    r"type=(?P<message_type>REQUEST|ACK|SELECTION|RESPONSE)\s+"
    r"validated=(?P<validated>true|false)\s+"
    r"packetPresent=(?P<packet_present>true|false)\s+"
    r"packetName=(?P<packet_name>\S+)\s+"
    r"producerPrefix=(?P<producer_prefix>\S+)\s+"
    r"seqNo=(?P<seq_no>\d+)\s+"
    r"signerKeyLocator=(?P<signer_key_locator>\S+)\s+"
    r"wireDigest=(?P<wire_digest>\S+)\s+"
    r"requestId=(?P<request_id>\S+)\s+"
    r"serviceName=(?P<service_name>\S+)\s+"
    r"requesterName=(?P<requester_name>\S+)\s+"
    r"providerName=(?P<provider_name>\S+)"
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_auth_markers(output: str) -> dict[str, dict[str, Any]]:
    observed: dict[str, dict[str, Any]] = {}
    for match in MARKER.finditer(output):
        item = {
            "case_id": match.group("case_id"),
            "terminal": match.group("terminal"),
            "observed_executions": int(match.group("executions")),
            "gate": match.group("gate"),
        }
        previous = observed.get(item["case_id"])
        if previous is not None and previous != item:
            raise ValueError(f"conflicting markers for {item['case_id']}")
        observed[item["case_id"]] = item
    return observed


def parse_onboarding_marker(output: str) -> dict[str, Any]:
    matches = list(ONBOARDING_MARKER.finditer(output))
    if len(matches) != 1:
        raise ValueError(f"expected one onboarding marker, observed {len(matches)}")
    match = matches[0]
    result: dict[str, Any] = {
        "stale_terminal": match.group("stale_terminal"),
        "refreshed_terminal": match.group("refreshed_terminal"),
    }
    for key in (
        "stale_executions",
        "refreshed_executions",
        "old_epoch",
        "new_epoch",
        "provider_manual_changes",
        "refresh_operations",
        "control_bytes",
        "time_to_first_success_us",
    ):
        result[key] = int(match.group(key))
    return result


def parse_crypto_counter_marker(output: str) -> dict[str, Any]:
    matches = list(CRYPTO_COUNTER_MARKER.finditer(output))
    if len(matches) != 1:
        raise ValueError(f"expected one crypto counter marker, observed {len(matches)}")
    match = matches[0]
    result: dict[str, Any] = {"role": match.group("role")}
    for key in (
        "hybrid_key_epochs",
        "abe_wraps",
        "abe_unwraps",
        "symmetric_encrypts",
        "symmetric_decrypts",
        "key_cache_hits",
        "key_cache_misses",
        "decrypt_failures",
    ):
        result[key] = int(match.group(key))
    return result


def parse_scale_markers(output: str) -> list[dict[str, int]]:
    keys = (
        "users",
        "providers",
        "policy_terms",
        "encryptions",
        "decryptions",
        "response_bytes",
        "encrypted_bytes",
        "total_us",
    )
    return [
        {key: int(match.group(key)) for key in keys}
        for match in SCALE_MARKER.finditer(output)
    ]


def parse_publication_audit_markers(output: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for match in PUBLICATION_AUDIT_MARKER.finditer(output):
        rows.append(
            {
                "role": match.group("role"),
                "message_type": match.group("message_type"),
                "validated": match.group("validated") == "true",
                "packet_present": match.group("packet_present") == "true",
                "packet_name": match.group("packet_name"),
                "producer_prefix": match.group("producer_prefix"),
                "seq_no": int(match.group("seq_no")),
                "signer_key_locator": match.group("signer_key_locator"),
                "wire_digest": match.group("wire_digest"),
                "request_id": match.group("request_id"),
                "service_name": match.group("service_name"),
                "requester_name": match.group("requester_name"),
                "provider_name": match.group("provider_name"),
            }
        )
    return rows


def validate_case_results(
    cases: Iterable[dict[str, Any]],
    observed: dict[str, dict[str, Any]],
) -> list[str]:
    errors: list[str] = []
    expected_ids = {case["case_id"] for case in cases}
    for case in cases:
        case_id = case["case_id"]
        result = observed.get(case_id)
        if result is None:
            errors.append(f"missing marker for {case_id}")
            continue
        comparisons = (
            ("terminal", "expected_terminal"),
            ("observed_executions", "expected_executions"),
            ("gate", "expected_gate"),
        )
        for observed_key, expected_key in comparisons:
            if result[observed_key] != case[expected_key]:
                errors.append(
                    f"{case_id} {observed_key}: observed={result[observed_key]!r} "
                    f"expected={case[expected_key]!r}"
                )
    for extra in sorted(set(observed) - expected_ids):
        errors.append(f"unregistered marker for {extra}")
    return errors


def resolve_unit_tests_binary(root: Path, explicit: Path | None = None) -> Path:
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    env_path = os.environ.get("NDNSF_UNIT_TESTS_BIN")
    if env_path:
        candidates.append(Path(env_path))
    candidates.extend(
        [
            root / "build-new-svs-20260808/unit-tests",
            root / "build/unit-tests",
        ]
    )
    for candidate in candidates:
        resolved = candidate if candidate.is_absolute() else root / candidate
        if resolved.is_file():
            return resolved.resolve()
    raise FileNotFoundError("no NDNSF unit-tests binary found")


def artifact_record(output_dir: Path, path: Path) -> dict[str, Any]:
    return {
        "path": str(path.relative_to(output_dir)),
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def capture_ndn_svs_subject(output_dir: Path, build_dir: Path) -> dict[str, Any]:
    """Retain the exact dirty NDN-SVS source and runtime linkage under test."""
    source_root = (ROOT.parent / "ndn-svs").resolve()
    library = (
        ROOT
        / "build/deps/ndn-svs-experimental/lib/libndn-svs.so.0.1.0"
    ).resolve()

    def git_text(*arguments: str) -> str:
        completed = subprocess.run(
            ["git", *arguments],
            cwd=source_root,
            capture_output=True,
            text=True,
            check=True,
        )
        return completed.stdout.strip()

    diff = subprocess.run(
        ["git", "diff", "--binary", "HEAD"],
        cwd=source_root,
        capture_output=True,
        check=True,
    ).stdout
    patch_path = output_dir / "ndn-svs-subject.patch"
    patch_path.write_bytes(diff)

    linkage_env = os.environ.copy()
    runtime_paths = [
        str(build_dir),
        str(ROOT / "build/deps/ndn-svs-experimental/lib"),
        "/usr/lib/x86_64-linux-gnu",
    ]
    if linkage_env.get("LD_LIBRARY_PATH"):
        runtime_paths.append(linkage_env["LD_LIBRARY_PATH"])
    linkage_env["LD_LIBRARY_PATH"] = ":".join(runtime_paths)
    linkage = subprocess.run(
        ["ldd", str(build_dir / "unit-tests")],
        capture_output=True,
        text=True,
        check=True,
        env=linkage_env,
    ).stdout
    linkage_path = output_dir / "runtime-linkage.txt"
    linkage_path.write_text(linkage, encoding="utf-8")

    return {
        "source_root": str(source_root),
        "branch": git_text("branch", "--show-current"),
        "head": git_text("rev-parse", "HEAD"),
        "head_tree": git_text("rev-parse", "HEAD^{tree}"),
        "dirty": bool(diff),
        "patch_path": patch_path.name,
        "patch_sha256": sha256_file(patch_path),
        "runtime_library": str(library),
        "runtime_library_sha256": sha256_file(library),
        "runtime_linkage_path": linkage_path.name,
        "runtime_linkage_sha256": sha256_file(linkage_path),
        "boost_version": "1.71.0",
    }


def _wait_for_log(
    path: Path, needle: str, timeout_s: float, process: subprocess.Popen[Any] | None = None
) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if path.is_file() and needle in path.read_text(errors="replace"):
            return True
        if process is not None and process.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def _stop_minindn_processes(
    processes: list[tuple[subprocess.Popen[Any], Any]],
) -> None:
    for process, log_handle in reversed(processes):
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        log_handle.close()


def _minindn_perf_args() -> SimpleNamespace:
    return SimpleNamespace(
        controller_node="memphis",
        user_node="memphis",
        providers=1,
        provider_nodes="ucla",
        serve_provider_certs=False,
        debug_ack=False,
        timeline_trace=True,
        timeline_trace_sample_rate=1,
        svs_piggyback_trace=False,
        dk_bootstrap_check=False,
        crypto_diagnostics=False,
        diag_plaintext_ack=False,
        diag_plaintext_response=False,
        svs_parallel_sync_processing=False,
        svs_parallel_workers=1,
        svs_parallel_queue=1,
        svs_sync_publish=False,
        svs_disable_parallel_production=False,
        svs_parallel_production_workers=None,
        svs_disable_parallel_production_signing=False,
        svs_parallel_production_signing=False,
        svs_disable_parallel_production_extra_block=False,
        svs_parallel_production_extra_block=False,
        svs_sync_batching=False,
        svs_sync_batch_ms=0,
        ack_threads=-1,
        performance_mode=False,
        workload_mode="single",
        rate_rps=None,
    )


def _minindn_provider_hashes(build_dir: Path) -> dict[str, str]:
    return {
        "executable": sha256_file(build_dir / "examples/App_Provider"),
        "service_config": hashlib.sha256(
            b"service=/HELLO;invocation=NormalAndTargeted"
        ).hexdigest(),
        "identity": hashlib.sha256(b"/example/hello/provider").hexdigest(),
        "trust_config": sha256_file(ROOT / "examples/trust-schema.conf"),
    }


def _run_minindn_case(
    args: argparse.Namespace,
    case_id: str,
    policy_file: Path,
    policy_epoch: int,
    build_dir: Path,
    case_dir: Path,
) -> dict[str, Any]:
    # Imports stay lazy so local artifact analysis does not require MiniNDN.
    sys.path.insert(0, str(ROOT / "Experiments"))
    import NDNSF_NewAPI_Minindn_Perf as perf  # type: ignore
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.helpers.nfdc import Nfdc
    from minindn.minindn import Minindn
    from minindn.util import getPopen

    def app_command(binary: str, argv: list[str]) -> str:
        words = [str(build_dir / "examples" / binary), *argv]
        return "cd {} && exec {}".format(
            shlex.quote(str(ROOT)), " ".join(shlex.quote(word) for word in words)
        )

    def start_app(node: Any, name: str, command: str, env: dict[str, str]):
        log_path = case_dir / f"{name}.log"
        log_handle = log_path.open("wb")
        process = getPopen(
            node,
            command,
            envDict=env,
            shell=True,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
        )
        processes.append((process, log_handle))
        return process, log_path

    case_dir.mkdir(parents=True, exist_ok=True)
    topology = args.topology.resolve()
    processes: list[tuple[subprocess.Popen[Any], Any]] = []
    ndn = None
    Minindn.cleanUp()
    Minindn.verifyDependencies()
    try:
        ndn = Minindn(topoFile=str(topology), workDir=str(case_dir / "minindn"))
        ndn.start()
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="ERROR")
        perf.wait_for_nfd_sockets(ndn, case_dir, timeout_s=20)
        if args.quick_smoke:
            return {
                "case_id": case_id,
                "terminal": "smoke",
                "provider_executions": 0,
                "policy_epoch": policy_epoch,
                "provider_local_hashes": _minindn_provider_hashes(build_dir),
            }

        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        routing.addOrigin(
            [ndn.net["memphis"]],
            ["/example/hello/controller", "/example/hello/user", "/example/hello/group"],
        )
        routing.addOrigin(
            [ndn.net["ucla"]],
            [
                "/example/hello/provider",
                "/example/hello/provider/KEY",
                "/example/hello/group",
            ],
        )
        routing.calculateRoutes()
        for node in ndn.net.hosts:
            for prefix in ("/example/hello", "/example/hello/group"):
                Nfdc.setStrategy(node, prefix, Nfdc.STRATEGY_MULTICAST)

        perf_args = _minindn_perf_args()
        perf.initialize_example_keychains(ndn, perf_args, case_dir)
        env = perf.app_env(case_dir, int(time.time()) + os.getpid(), perf_args)
        runtime_paths = [
            str(build_dir),
            str(ROOT / "build/deps/ndn-svs-experimental/lib"),
        ]
        if os.environ.get("LD_LIBRARY_PATH"):
            runtime_paths.append(os.environ["LD_LIBRARY_PATH"])
        env["LD_LIBRARY_PATH"] = ":".join(runtime_paths)
        env["NDNSF_POLICY_EPOCH"] = str(policy_epoch)
        env["NDN_LOG"] = "ndn_service_framework.*=TRACE"

        controller, controller_log = start_app(
            ndn.net["memphis"],
            "controller",
            app_command("App_ServiceController", ["--policy-file", str(policy_file)]),
            env,
        )
        if not _wait_for_log(controller_log, "ServiceController started", 8, controller):
            raise RuntimeError(f"controller did not become ready: {controller_log}")

        provider, provider_log = start_app(
            ndn.net["ucla"],
            "provider",
            app_command("App_Provider", ["--timeline-trace"]),
            env,
        )
        if not _wait_for_log(provider_log, "registered service /HELLO", 12, provider):
            raise RuntimeError(f"provider did not become ready: {provider_log}")

        user, user_log = start_app(
            ndn.net["memphis"],
            "user",
            app_command(
                "App_User",
                [
                    "--ack-timeout-ms",
                    "1000",
                    "--timeout-ms",
                    "5000",
                    "--timeline-trace",
                ],
            ),
            env,
        )
        expected_needle = (
            "Waiting for decryption key"
            if case_id == "pre_onboarding_no_user"
            else "Received response: HELLO"
        )
        observed_terminal = _wait_for_log(user_log, expected_needle, 12, user)
        if not observed_terminal:
            raise RuntimeError(f"user terminal event missing for {case_id}: {user_log}")
        time.sleep(1)

        user_text = user_log.read_text(errors="replace")
        provider_text = provider_log.read_text(errors="replace")
        executions = provider_text.count("Received HELLO request")
        epoch_matches = re.findall(r"policyEpoch=(\d+)", provider_text)
        observed_epoch = int(epoch_matches[-1]) if epoch_matches else 0
        terminal = "allow" if "Received response: HELLO" in user_text else "deny"
        publication_audit = parse_publication_audit_markers(
            provider_text + "\n" + user_text
        )
        return {
            "case_id": case_id,
            "terminal": terminal,
            "terminal_gate": (
                "response_acceptance" if terminal == "allow" else "abe_key_readiness"
            ),
            "provider_executions": executions,
            "request_send_attempted": "Sending HELLO request" in user_text,
            "policy_epoch": observed_epoch,
            "provider_local_hashes": _minindn_provider_hashes(build_dir),
            "publication_audit": publication_audit,
            "logs": {
                "controller": str(controller_log.relative_to(args.output.resolve())),
                "provider": str(provider_log.relative_to(args.output.resolve())),
                "user": str(user_log.relative_to(args.output.resolve())),
            },
        }
    finally:
        _stop_minindn_processes(processes)
        if ndn is not None:
            ndn.stop()
        Minindn.cleanUp()
        # Node homes and exported key bundles are setup material, not evidence.
        # Never retain private keys in a canonical result directory.
        shutil.rmtree(case_dir / "minindn", ignore_errors=True)
        for private_key in (case_dir / "security").glob("*.ndnkey"):
            private_key.unlink(missing_ok=True)


def run_minindn(args: argparse.Namespace) -> int:
    started_at = utc_now()
    invocation = [sys.executable, *sys.argv]
    # MiniNDN's legacy module parses process-wide argv during construction.
    sys.argv = [sys.argv[0]]
    contract = json.loads(
        (FEATURE / "contracts/runtime-gates.json").read_text(encoding="utf-8")
    )
    binary = resolve_unit_tests_binary(ROOT, args.unit_tests)
    build_dir = binary.parent
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=False)
    ndn_svs_subject = capture_ndn_svs_subject(output_dir, build_dir)

    if args.quick_smoke:
        _run_minindn_case(
            args,
            "startup_smoke",
            ROOT / "examples/hello.policies",
            1,
            build_dir,
            output_dir,
        )
        smoke_path = output_dir / "smoke.json"
        smoke_path.write_text(
            json.dumps({"status": "SPEC172_MININDN_SMOKE_OK"}, indent=2) + "\n",
            encoding="utf-8",
        )
        print("SPEC172_MININDN_SMOKE_OK")
        return 0

    old_policy = output_dir / "pre-onboarding.policies"
    old_policy.write_text(
        """name /example/hello/controller/NDNSF/ControllerPolicy/v1

provider-policies
{
    provider-policy
    {
        for /example/hello/provider
        allow { /HELLO }
    }
}

user-policies
{
    user-policy
    {
        for /example/hello/not-the-user
        allow { /HELLO }
    }
}
""",
        encoding="utf-8",
    )
    observations = [
        _run_minindn_case(
            args,
            "pre_onboarding_no_user",
            old_policy,
            1,
            build_dir,
            output_dir / "pre_onboarding_no_user",
        ),
        _run_minindn_case(
            args,
            "post_onboarding_authorized",
            ROOT / "examples/hello.policies",
            2,
            build_dir,
            output_dir / "post_onboarding_authorized",
        ),
    ]
    from Experiments.analyze_authorization_evaluation import (
        summarize_minindn,
        summarize_publication_audit,
    )

    summary = summarize_minindn(observations)
    publication_audit = summarize_publication_audit(
        observations[1].get("publication_audit", [])
    )
    summary["publication_audit"] = publication_audit
    summary["failures"].extend(
        f"publication audit: {failure}"
        for failure in publication_audit["failures"]
    )
    summary["supported"] = not summary["failures"]
    results_path = output_dir / "minindn-results.json"
    results_path.write_text(
        json.dumps({"observations": observations}, indent=2) + "\n", encoding="utf-8"
    )
    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    source_hashes = {
        relative: sha256_file(ROOT / relative)
        for relative in contract["baseline_source_hashes"]
    }
    artifacts = [
        artifact_record(output_dir, path)
        for path in sorted(output_dir.rglob("*"))
        if path.is_file()
    ]
    manifest = {
        "schema_version": 1,
        "manifest_id": f"spec172-minindn-{started_at}",
        "subject": "ndnsf-minindn-authorization-onboarding-confirmation",
        "command": invocation,
        "configuration": {
            "topology": str(args.topology.resolve()),
            "topology_sha256": sha256_file(args.topology.resolve()),
            "controller_node": "memphis",
            "user_node": "memphis",
            "provider_node": "ucla",
            "network": "wired MiniNDN",
            "executables": {
                name: sha256_file(build_dir / "examples" / name)
                for name in ("App_ServiceController", "App_Provider", "App_User")
            },
            "authorized_policy_sha256": sha256_file(ROOT / "examples/hello.policies"),
            "trust_schema_sha256": sha256_file(ROOT / "examples/trust-schema.conf"),
            "provider_manual_changes": 0,
            "online_hot_refresh_claimed": False,
            "ndn_svs_subject": ndn_svs_subject,
        },
        "source_hashes": source_hashes,
        "started_at": started_at,
        "completed_at": utc_now(),
        "terminal_status": "success" if summary["supported"] else "failure",
        "artifacts": artifacts,
    }
    schema = json.loads(
        (FEATURE / "contracts/experiment-manifest.schema.json").read_text(
            encoding="utf-8"
        )
    )
    jsonschema.validate(manifest, schema)
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))
    return 0 if summary["supported"] else 1


def run_correctness_smoke(args: argparse.Namespace) -> int:
    started_at = utc_now()
    cases_path = args.cases.resolve()
    contract_path = FEATURE / "contracts/runtime-gates.json"
    cases = yaml.safe_load(cases_path.read_text(encoding="utf-8"))["cases"]
    contract = json.loads(contract_path.read_text(encoding="utf-8"))
    gates = contract["gates"]
    binary = resolve_unit_tests_binary(ROOT, args.unit_tests)

    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=False)
    logs_dir = output_dir / "logs"
    logs_dir.mkdir()

    selectors: dict[str, list[str]] = {}
    for case in cases:
        selector = gates[case["expected_gate"]]["boost_test_selector"]
        selectors.setdefault(selector, []).append(case["case_id"])

    observed: dict[str, dict[str, Any]] = {}
    selector_runs: list[dict[str, Any]] = []
    for index, selector in enumerate(sorted(selectors), start=1):
        command = [
            str(binary),
            f"--run_test={selector}",
            "--log_level=message",
            "--report_level=no",
        ]
        completed = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            env=os.environ.copy(),
        )
        combined = completed.stdout + completed.stderr
        log_path = logs_dir / f"{index:02d}.log"
        log_path.write_text(combined, encoding="utf-8")
        markers = parse_auth_markers(combined)
        for case_id, marker in markers.items():
            previous = observed.get(case_id)
            if previous is not None and previous != marker:
                raise RuntimeError(f"conflicting cross-selector marker for {case_id}")
            observed[case_id] = marker
        selector_runs.append(
            {
                "selector": selector,
                "registered_cases": selectors[selector],
                "return_code": completed.returncode,
                "log": str(log_path.relative_to(output_dir)),
            }
        )

    errors = validate_case_results(cases, observed)
    for run in selector_runs:
        if run["return_code"] != 0:
            errors.append(
                f"selector failed rc={run['return_code']}: {run['selector']}"
            )

    security_regression: dict[str, Any] | None = None
    if args.security_regressions:
        regression_log = output_dir / "security-regressions.log"
        regression_env = os.environ.copy()
        regression_env["NDNSF_BUILD_DIR"] = str(binary.parent)
        runtime_paths = [
            str(binary.parent),
            str(ROOT / "build/deps/ndn-svs-experimental/lib"),
        ]
        if regression_env.get("LD_LIBRARY_PATH"):
            runtime_paths.append(regression_env["LD_LIBRARY_PATH"])
        regression_env["LD_LIBRARY_PATH"] = ":".join(runtime_paths)
        try:
            completed = subprocess.run(
                [str(ROOT / "examples/run_security_regressions.sh")],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
                timeout=args.security_timeout_s,
                env=regression_env,
            )
            combined = completed.stdout + completed.stderr
            regression_log.write_text(combined, encoding="utf-8")
            passed = (
                completed.returncode == 0
                and "NDNSF_SECURITY_REGRESSIONS=PASS" in combined
            )
            security_regression = {
                "return_code": completed.returncode,
                "terminal": "pass" if passed else "fail",
                "log": str(regression_log.relative_to(output_dir)),
            }
            if not passed:
                errors.append(
                    "security regression suite did not emit a successful terminal marker"
                )
        except subprocess.TimeoutExpired as error:
            combined = (error.stdout or "") + (error.stderr or "")
            regression_log.write_text(combined, encoding="utf-8")
            security_regression = {
                "return_code": None,
                "terminal": "timeout",
                "log": str(regression_log.relative_to(output_dir)),
            }
            errors.append(
                f"security regression suite timed out after {args.security_timeout_s}s"
            )

    results = []
    for case in cases:
        marker = observed.get(case["case_id"], {})
        results.append(
            {
                **case,
                "terminal": marker.get("terminal", "missing"),
                "observed_executions": marker.get("observed_executions"),
                "observed_gate": marker.get("gate", "missing"),
                "selector": gates[case["expected_gate"]]["boost_test_selector"],
            }
        )

    results_path = output_dir / "case-results.json"
    results_path.write_text(
        json.dumps({"schema_version": 1, "cases": results}, indent=2) + "\n",
        encoding="utf-8",
    )
    runs_path = output_dir / "selector-runs.json"
    runs_path.write_text(
        json.dumps({"schema_version": 1, "runs": selector_runs}, indent=2) + "\n",
        encoding="utf-8",
    )
    summary_path = output_dir / "summary.json"
    summary_path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "registered_cases": len(cases),
                "matched_cases": len(cases) - sum(error.startswith("missing marker") for error in errors),
                "expected_observed_agreement": len(errors) == 0,
                "denied_handler_executions": sum(
                    int(item.get("observed_executions") or 0)
                    for item in results
                    if item["expected_terminal"] == "deny"
                ),
                "security_regressions": security_regression,
                "errors": errors,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    source_hashes = {
        relative: sha256_file(ROOT / relative)
        for relative in contract["baseline_source_hashes"]
    }
    artifacts = [
        artifact_record(output_dir, path)
        for path in sorted(output_dir.rglob("*"))
        if path.is_file()
    ]
    manifest = {
        "schema_version": 1,
        "manifest_id": f"spec172-correctness-{started_at}",
        "subject": "ndnsf-authorization-correctness",
        "command": [sys.executable, *sys.argv],
        "configuration": {
            "mode": "correctness-smoke",
            "cases": str(cases_path.relative_to(ROOT)),
            "unit_tests_binary": str(binary),
            "unit_tests_sha256": sha256_file(binary),
            "runtime_gates": str(contract_path.relative_to(ROOT)),
            "security_regressions": args.security_regressions,
            "security_timeout_s": args.security_timeout_s,
        },
        "source_hashes": source_hashes,
        "started_at": started_at,
        "completed_at": utc_now(),
        "terminal_status": "success" if not errors else "failure",
        "artifacts": artifacts,
    }
    schema = json.loads(
        (FEATURE / "contracts/experiment-manifest.schema.json").read_text(
            encoding="utf-8"
        )
    )
    jsonschema.validate(manifest, schema)
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    print(json.dumps(json.loads(summary_path.read_text(encoding="utf-8")), indent=2))
    return 0 if not errors else 1


def run_onboarding(args: argparse.Namespace) -> int:
    started_at = utc_now()
    contract_path = FEATURE / "contracts/runtime-gates.json"
    contract = json.loads(contract_path.read_text(encoding="utf-8"))
    binary = resolve_unit_tests_binary(ROOT, args.unit_tests)
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=False)
    logs_dir = output_dir / "logs"
    logs_dir.mkdir()

    def capture_provider_local_hashes() -> dict[str, str]:
        return {
            "executable": sha256_file(binary),
            "service_config": sha256_file(ROOT / "examples/hello.policies"),
            "identity": hashlib.sha256(
                b"/test/provider/existing"
            ).hexdigest(),
            "trust_config": sha256_file(ROOT / "examples/trust-any.conf"),
        }
    selector = (
        "GenericDynamicApi/CryptoAndAuthorization/"
        "NewUserUsesUnchangedProviderAfterControllerMaterialRefresh"
    )
    observations: list[dict[str, Any]] = []
    errors: list[str] = []
    for repetition in range(1, args.repetitions + 1):
        command = [
            str(binary),
            f"--run_test={selector}",
            "--log_level=message",
            "--report_level=no",
        ]
        completed = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            env=os.environ.copy(),
        )
        combined = completed.stdout + completed.stderr
        log_path = logs_dir / f"repetition-{repetition:02d}.log"
        log_path.write_text(combined, encoding="utf-8")
        if completed.returncode != 0:
            errors.append(
                f"repetition {repetition} failed rc={completed.returncode}"
            )
            continue
        try:
            marker = parse_onboarding_marker(combined)
        except ValueError as error:
            errors.append(f"repetition {repetition}: {error}")
            continue
        observations.append(
            {
                "repetition": repetition,
                **marker,
                "provider_local_hashes": capture_provider_local_hashes(),
                "log": str(log_path.relative_to(output_dir)),
            }
        )

    if len(observations) != args.repetitions:
        errors.append(
            f"observed {len(observations)} repetitions, expected {args.repetitions}"
        )
    reference_hashes = observations[0]["provider_local_hashes"] if observations else None
    for row in observations:
        for key, expected in {
            "stale_terminal": "deny",
            "refreshed_terminal": "allow",
            "stale_executions": 0,
            "refreshed_executions": 1,
            "provider_manual_changes": 0,
            "refresh_operations": 1,
        }.items():
            if row[key] != expected:
                errors.append(
                    f"repetition {row['repetition']} {key}: "
                    f"observed={row[key]!r} expected={expected!r}"
                )
        if row["provider_local_hashes"] != reference_hashes:
            errors.append(
                f"repetition {row['repetition']} changed Provider-local hashes"
            )

    results_path = output_dir / "onboarding-results.json"
    results_path.write_text(
        json.dumps({"schema_version": 1, "observations": observations}, indent=2)
        + "\n",
        encoding="utf-8",
    )
    summary = {
        "schema_version": 1,
        "evidence_scope": "local-framework-transition",
        "repetitions": len(observations),
        "provider_hashes_unchanged": (
            len(observations) == args.repetitions
            and all(
                item["provider_local_hashes"] == reference_hashes
                for item in observations
            )
        ),
        "manual_provider_changes": sum(
            item["provider_manual_changes"] for item in observations
        ),
        "refresh_operations": sum(
            item["refresh_operations"] for item in observations
        ),
        "control_bytes": [item["control_bytes"] for item in observations],
        "time_to_first_success_us": [
            item["time_to_first_success_us"] for item in observations
        ],
        "errors": errors,
    }
    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    source_hashes = {
        relative: sha256_file(ROOT / relative)
        for relative in contract["baseline_source_hashes"]
    }
    artifacts = [
        artifact_record(output_dir, path)
        for path in sorted(output_dir.rglob("*"))
        if path.is_file()
    ]
    manifest = {
        "schema_version": 1,
        "manifest_id": f"spec172-onboarding-{started_at}",
        "subject": "ndnsf-offline-provider-new-user-onboarding",
        "command": [sys.executable, *sys.argv],
        "configuration": {
            "mode": "onboarding",
            "repetitions": args.repetitions,
            "unit_tests_binary": str(binary),
            "unit_tests_sha256": sha256_file(binary),
            "selector": selector,
            "provider_rejoin_behavior": (
                "unchanged Provider installs current controller material before retry"
            ),
            "network_confirmation": "separate results/spec172_authorization_minindn campaign",
        },
        "source_hashes": source_hashes,
        "started_at": started_at,
        "completed_at": utc_now(),
        "terminal_status": "success" if not errors else "failure",
        "artifacts": artifacts,
    }
    schema = json.loads(
        (FEATURE / "contracts/experiment-manifest.schema.json").read_text(
            encoding="utf-8"
        )
    )
    jsonschema.validate(manifest, schema)
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))
    return 0 if not errors else 1


def _terminate_process(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def _parse_timeline_durations(output: str) -> dict[str, list[int]]:
    result: dict[str, list[int]] = {}
    for line in output.splitlines():
        event = re.search(r"\bevent=([A-Za-z0-9_]+)", line)
        duration = re.search(r"\bduration_us=(\d+)", line)
        if event and duration:
            result.setdefault(event.group(1), []).append(int(duration.group(1)))
    return result


def run_overhead(args: argparse.Namespace) -> int:
    started_at = utc_now()
    contract_path = FEATURE / "contracts/runtime-gates.json"
    contract = json.loads(contract_path.read_text(encoding="utf-8"))
    binary = resolve_unit_tests_binary(ROOT, args.unit_tests)
    build_dir = binary.parent
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=False)
    nfd_started = False
    observations: list[dict[str, Any]] = []
    errors: list[str] = []

    if not Path("/run/nfd/nfd.sock").exists():
        completed = subprocess.run(
            ["nfd-start"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
            timeout=10,
        )
        if completed.returncode != 0:
            raise RuntimeError(f"nfd-start failed with rc={completed.returncode}")
        nfd_started = True
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not Path("/run/nfd/nfd.sock").exists():
            time.sleep(0.1)
    if not Path("/run/nfd/nfd.sock").exists():
        raise RuntimeError("NFD socket did not become ready")

    try:
        for repetition in range(1, args.repetitions + 1):
            repetition_dir = output_dir / f"repetition-{repetition:02d}"
            repetition_dir.mkdir()
            env = os.environ.copy()
            env["NDNSF_CONFIG"] = str(repetition_dir / "ndnsf.conf")
            env["NDNSF_SESSION_BASE"] = str(int(time.time()) + os.getpid() + repetition)
            env["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = "1"
            env["NDN_LOG"] = "ndn_service_framework.*=TRACE"
            runtime_paths = [
                str(build_dir),
                str(ROOT / "build/deps/ndn-svs-experimental/lib"),
            ]
            if env.get("LD_LIBRARY_PATH"):
                runtime_paths.append(env["LD_LIBRARY_PATH"])
            env["LD_LIBRARY_PATH"] = ":".join(runtime_paths)

            controller_log = repetition_dir / "controller.log"
            provider_log = repetition_dir / "provider.log"
            user_log = repetition_dir / "user.log"
            csv_path = repetition_dir / "requests.csv"
            controller_handle = controller_log.open("w", encoding="utf-8")
            provider_handle = provider_log.open("w", encoding="utf-8")
            controller = subprocess.Popen(
                [str(build_dir / "examples/App_ServiceController")],
                cwd=ROOT,
                stdout=controller_handle,
                stderr=subprocess.STDOUT,
                text=True,
                env=env,
            )
            provider: subprocess.Popen[Any] | None = None
            try:
                time.sleep(2)
                provider = subprocess.Popen(
                    [str(build_dir / "examples/App_Provider"), "--timeline-trace"],
                    cwd=ROOT,
                    stdout=provider_handle,
                    stderr=subprocess.STDOUT,
                    text=True,
                    env=env,
                )
                time.sleep(4)
                command = [
                    str(build_dir / "examples/App_User"),
                    "--benchmark",
                    "--workload-mode", "open-loop",
                    "--rate-rps", str(args.rate_rps),
                    "--duration", str(args.duration_s),
                    "--warmup", "0",
                    "--strategy", "first-responding",
                    "--ack-timeout-ms", "1000",
                    "--request-timeout-ms", "5000",
                    "--timeout-ms", "5000",
                    "--max-inflight", "16",
                    "--drain-seconds", "6",
                    "--disable-adaptive-admission-control",
                    "--timeline-trace",
                    "--output-csv", str(csv_path),
                ]
                with user_log.open("w", encoding="utf-8") as user_handle:
                    completed = subprocess.run(
                        command,
                        cwd=ROOT,
                        stdout=user_handle,
                        stderr=subprocess.STDOUT,
                        text=True,
                        check=False,
                        timeout=args.duration_s + 40,
                        env=env,
                    )
                time.sleep(1)
            except subprocess.TimeoutExpired:
                errors.append(f"repetition {repetition} user timed out")
                completed = subprocess.CompletedProcess([], 124)
            finally:
                if provider is not None:
                    _terminate_process(provider)
                _terminate_process(controller)
                provider_handle.close()
                controller_handle.close()

            if completed.returncode != 0:
                errors.append(
                    f"repetition {repetition} user rc={completed.returncode}"
                )
            if not csv_path.is_file():
                errors.append(f"repetition {repetition} missing requests.csv")
                continue
            rows = list(csv.DictReader(csv_path.open(encoding="utf-8")))
            successes = [
                float(row["latency_ms"])
                for row in rows
                if row.get("success", "").lower() in {"1", "true"}
                and row.get("latency_ms")
            ]
            request_failures = len(rows) - len(successes)
            user_text = user_log.read_text(encoding="utf-8")
            provider_text = provider_log.read_text(encoding="utf-8")
            try:
                counters = parse_crypto_counter_marker(user_text)
            except ValueError as error:
                errors.append(f"repetition {repetition}: {error}")
                continue
            content_bytes = sum(
                int(match.group(1))
                for text_value in (user_text, provider_text)
                for match in re.finditer(
                    r"\bevent=SVS_PUBLISH_BEGIN\b[^\n]*\bcontentBytes=(\d+)",
                    text_value,
                )
            )
            durations = _parse_timeline_durations(user_text + "\n" + provider_text)
            scale_selector = (
                "EncryptedPermissionResponse/"
                "PermissionProvisioningScaleEmitsRegisteredCostEvidence"
            )
            scale_completed = subprocess.run(
                [
                    str(binary),
                    f"--run_test={scale_selector}",
                    "--log_level=message",
                    "--report_level=no",
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
                env=env,
            )
            scale_text = scale_completed.stdout + scale_completed.stderr
            (repetition_dir / "provisioning-scale.log").write_text(
                scale_text, encoding="utf-8"
            )
            scale_rows = parse_scale_markers(scale_text)
            expected_scale_points = {
                (users, providers)
                for users in (1, 10, 100)
                for providers in (1, 4, 16)
            }
            observed_scale_points = {
                (item["users"], item["providers"]) for item in scale_rows
            }
            if scale_completed.returncode != 0:
                errors.append(
                    f"repetition {repetition} provisioning scale rc="
                    f"{scale_completed.returncode}"
                )
            if observed_scale_points != expected_scale_points:
                errors.append(
                    f"repetition {repetition} provisioning scale points mismatch"
                )
            observations.append(
                {
                    "repetition": repetition,
                    "cold_latency_ms": successes[0] if successes else None,
                    "warm_latencies_ms": successes[1:],
                    "requests": len(rows),
                    "successes": len(successes),
                    "failures": request_failures,
                    "crypto_counters": counters,
                    "protected_content_bytes": content_bytes,
                    "timeline_duration_us": durations,
                    "provisioning_scale": scale_rows,
                    "command": command,
                }
            )

        if len(observations) != args.repetitions:
            errors.append(
                f"observed {len(observations)} repetitions, expected {args.repetitions}"
            )
        for row in observations:
            if row["cold_latency_ms"] is None or not row["warm_latencies_ms"]:
                errors.append(
                    f"repetition {row['repetition']} lacks cold or warm successes"
                )
            if row["failures"] != 0:
                errors.append(
                    f"repetition {row['repetition']} request failures={row['failures']}"
                )
            if row["crypto_counters"]["decrypt_failures"] != 0:
                errors.append(
                    f"repetition {row['repetition']} crypto decrypt failures"
                )
            if row["protected_content_bytes"] <= 0:
                errors.append(
                    f"repetition {row['repetition']} lacks protected content bytes"
                )
    finally:
        if nfd_started:
            subprocess.run(
                ["nfd-stop"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=10,
            )

    results_path = output_dir / "overhead-results.json"
    results_path.write_text(
        json.dumps({"schema_version": 1, "observations": observations}, indent=2)
        + "\n",
        encoding="utf-8",
    )
    summary = {
        "schema_version": 1,
        "evidence_scope": "local-nfd-secured-cold-warm",
        "repetitions": len(observations),
        "duration_s": args.duration_s,
        "rate_rps": args.rate_rps,
        "errors": errors,
    }
    summary_path = output_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    source_hashes = {
        relative: sha256_file(ROOT / relative)
        for relative in contract["baseline_source_hashes"]
    }
    artifacts = [
        artifact_record(output_dir, path)
        for path in sorted(output_dir.rglob("*"))
        if path.is_file()
    ]
    manifest = {
        "schema_version": 1,
        "manifest_id": f"spec172-overhead-{started_at}",
        "subject": "ndnsf-local-nfd-cold-warm-authorization-cost",
        "command": [sys.executable, *sys.argv],
        "configuration": {
            "mode": "overhead",
            "repetitions": args.repetitions,
            "duration_s": args.duration_s,
            "rate_rps": args.rate_rps,
            "admission_control": False,
            "tokens": True,
            "authorization": True,
            "executables": {
                name: sha256_file(build_dir / "examples" / name)
                for name in ("App_ServiceController", "App_Provider", "App_User")
            },
            "comparison": "first secured transaction versus later secured transactions",
            "network_scope": "local NFD only; no network-cost claim",
        },
        "source_hashes": source_hashes,
        "started_at": started_at,
        "completed_at": utc_now(),
        "terminal_status": "success" if not errors else "failure",
        "artifacts": artifacts,
    }
    schema = json.loads(
        (FEATURE / "contracts/experiment-manifest.schema.json").read_text(
            encoding="utf-8"
        )
    )
    jsonschema.validate(manifest, schema)
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, indent=2))
    return 0 if not errors else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mode",
        choices=["correctness-smoke", "onboarding", "overhead", "minindn"],
        required=True,
    )
    parser.add_argument(
        "--cases",
        type=Path,
        default=FEATURE / "contracts/authorization-cases.yaml",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--unit-tests", type=Path)
    parser.add_argument(
        "--security-regressions",
        action="store_true",
        help="run and retain the composed six-script security regression suite",
    )
    parser.add_argument("--security-timeout-s", type=int, default=300)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--duration-s", type=int, default=60)
    parser.add_argument("--rate-rps", type=float, default=1.0)
    parser.add_argument(
        "--topology",
        type=Path,
        default=ROOT / "Experiments/Topology/AI_Lab.conf",
    )
    parser.add_argument("--quick-smoke", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.mode == "correctness-smoke":
        return run_correctness_smoke(args)
    if args.mode == "onboarding":
        return run_onboarding(args)
    if args.mode == "overhead":
        return run_overhead(args)
    if args.mode == "minindn":
        return run_minindn(args)
    raise AssertionError(f"unhandled mode: {args.mode}")


if __name__ == "__main__":
    raise SystemExit(main())
