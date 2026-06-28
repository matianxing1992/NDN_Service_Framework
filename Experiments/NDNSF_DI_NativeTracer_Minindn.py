#!/usr/bin/env python3
"""Real MiniNDN evidence launcher for the NDNSF-DI native tracer."""

from __future__ import annotations

import argparse
import csv
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "Experiments"))

import NDNSF_NewAPI_Minindn_Perf as perf  # noqa: E402
from mininet.log import info, setLogLevel  # noqa: E402
from minindn.apps.app_manager import AppManager  # noqa: E402
from minindn.apps.nfd import Nfd  # noqa: E402
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper  # noqa: E402
from minindn.helpers.nfdc import Nfdc  # noqa: E402
from minindn.minindn import Minindn  # noqa: E402
from minindn.util import getPopen  # noqa: E402

TOPO = REPO / "Experiments/Topology/AI_Lab.conf"
TRACER_DIR = REPO / "examples/python/NDNSF-DistributedInference/native_di_tracer"
PLAN_TRACER = TRACER_DIR / "plan_tracer.py"
USER_DRIVER = TRACER_DIR / "user_driver.py"
PROVIDER_EXE = REPO / "build/examples/di-native-provider"
PLAN_SCHEMA_EXE = REPO / "build/examples/di-native-plan-schema-smoke"
PLAN_MANIFEST_EXE = REPO / "build/examples/di-native-plan-manifest-smoke"
PROVIDER_SESSION_EXE = REPO / "build/examples/di-native-provider-session-smoke"
DEFAULT_OUT = REPO / "results/native_di_real_minindn/latest"
SERVICE = "/Inference/NativeTracer"
GROUP = "/NDNSF-DI/Tracer/group"
CONTROLLER = "/NDNSF-DI/Tracer/controller"
USER = "/NDNSF-DI/Tracer/user"
IDENTITY_SAFEBAG_PASSPHRASE = "ndnsf-di-native-tracer"
REQUIRED_ROLES = ["/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"]

DEFAULT_ASSIGNMENT = {
    "/Backbone": ("ucla", "/NDNSF-DI/Tracer/provider/backbone"),
    "/Head/Shard/0": ("arizona", "/NDNSF-DI/Tracer/provider/head0"),
    "/Head/Shard/1": ("wustl", "/NDNSF-DI/Tracer/provider/head1"),
    "/Merge": ("neu", "/NDNSF-DI/Tracer/provider/merge"),
}

ALTERNATE_ASSIGNMENT = {
    "/Backbone": ("neu", "/NDNSF-DI/Tracer/alt-provider/backbone"),
    "/Head/Shard/0": ("ucla", "/NDNSF-DI/Tracer/alt-provider/head0"),
    "/Head/Shard/1": ("arizona", "/NDNSF-DI/Tracer/alt-provider/head1"),
    "/Merge": ("wustl", "/NDNSF-DI/Tracer/alt-provider/merge"),
}


def log(message: str) -> None:
    info(message + "\n")


def mini_status() -> str:
    try:
        import minindn  # noqa: F401
    except Exception as exc:  # pragma: no cover - diagnostic path
        return f"unavailable:{exc}"
    return "available-root" if os.geteuid() == 0 else "available-non-root"


def topology_nodes(path: Path) -> set[str]:
    nodes: set[str] = set()
    in_nodes = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line == "[nodes]":
            in_nodes = True
            continue
        if line.startswith("[") and line != "[nodes]":
            in_nodes = False
        if in_nodes and line and not line.startswith("#"):
            nodes.add(line.rstrip(":"))
    return nodes


def assignment_for(name: str) -> dict[str, tuple[str, str]]:
    if name == "default":
        return DEFAULT_ASSIGNMENT
    if name == "alternate":
        return ALTERNATE_ASSIGNMENT
    raise ValueError(f"unknown assignment: {name}")


def shell_join(items: list[str]) -> str:
    return " ".join(perf.shell_quote(str(item)) for item in items)


def run_logged(name: str, command: list[str], logs_dir: Path, env: dict[str, str]) -> None:
    log_path = logs_dir / f"{name}.log"
    with log_path.open("wb") as output:
        output.write(("RUN " + shell_join(command) + "\n").encode("utf-8"))
        output.flush()
        subprocess.run(command, cwd=str(REPO), env=env, stdout=output,
                       stderr=subprocess.STDOUT, check=True)


def write_assignment_csv(path: Path,
                         assignment_name: str,
                         roles: list[str],
                         assignment: dict[str, tuple[str, str]]) -> list[dict[str, str]]:
    rows = []
    for role in REQUIRED_ROLES:
        if role not in roles:
            raise RuntimeError(f"generated plan is missing required role: {role}")
        node, provider = assignment[role]
        rows.append({
            "assignment": assignment_name,
            "role": role,
            "provider": provider,
            "node": node,
            "service": SERVICE,
        })
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=[
            "assignment", "role", "provider", "node", "service",
        ])
        writer.writeheader()
        writer.writerows(rows)
    return rows


def load_plan_roles(plan_path: Path) -> list[str]:
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    service = next(item for item in plan["services"] if item["service"] == SERVICE)
    return list(service["roles"])


def validate_prerequisites() -> None:
    missing = [
        str(path) for path in (
            TOPO,
            PLAN_TRACER,
            USER_DRIVER,
            PROVIDER_EXE,
            PLAN_SCHEMA_EXE,
            PLAN_MANIFEST_EXE,
            PROVIDER_SESSION_EXE,
        )
        if not path.exists()
    ]
    nodes = topology_nodes(TOPO) if TOPO.exists() else set()
    required_nodes = {"memphis", "ucla", "arizona", "wustl", "neu"}
    missing_nodes = sorted(required_nodes - nodes)
    if missing or missing_nodes:
        raise RuntimeError(
            f"native tracer MiniNDN prerequisites failed missingFiles={missing} "
            f"missingNodes={missing_nodes}")


def read_log_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def wait_for_log_patterns(paths: list[Path],
                          patterns: list[str],
                          timeout_s: int,
                          label: str) -> None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if all(
            any(pattern in read_log_text(path) for path in paths)
            for pattern in patterns
        ):
            return
        time.sleep(0.2)
    observed = {str(path): read_log_text(path)[-2000:] for path in paths}
    raise RuntimeError(f"timed out waiting for {label}: {observed}")


def validate_local_timing_csv(path: Path,
                              assignment_rows: list[dict[str, str]]) -> dict[str, object]:
    expected = {row["role"]: row["provider"] for row in assignment_rows}
    with path.open(newline="", encoding="utf-8") as input_file:
        reader = csv.DictReader(input_file)
        required_columns = [
            "sessionId", "provider", "role", "inputBytes", "outputBytes",
            "prefetchMs", "executeMs", "publishMs", "endToEndMs", "status",
        ]
        if reader.fieldnames != required_columns:
            raise RuntimeError(
                f"unexpected local execution timing columns: {reader.fieldnames}")
        rows = list(reader)
    observed_roles = {row["role"] for row in rows}
    if observed_roles != set(expected):
        raise RuntimeError(
            f"local execution roles {sorted(observed_roles)} do not match "
            f"assignment roles {sorted(expected)}")
    for row in rows:
        if row["provider"] != expected[row["role"]]:
            raise RuntimeError(
                f"local execution provider mismatch for {row['role']}: "
                f"{row['provider']} != {expected[row['role']]}")
        if row["status"] != "ok":
            raise RuntimeError(f"local execution row did not finish ok: {row}")
        int(row["inputBytes"])
        int(row["outputBytes"])
        for column in ("prefetchMs", "executeMs", "publishMs", "endToEndMs"):
            float(row[column])
    return {
        "rows": len(rows),
        "roles": sorted(observed_roles),
    }


def run_local_execution_baseline(policy_dir: Path,
                                 out_dir: Path,
                                 logs_dir: Path,
                                 env: dict[str, str],
                                 assignment_name: str,
                                 assignment_rows: list[dict[str, str]]) -> dict[str, object]:
    timing_csv = out_dir / "local-execution-timing.csv"
    run_logged("local-plan-schema-smoke", [
        str(PLAN_SCHEMA_EXE),
        str(policy_dir / "native-execution-plan.json"),
        SERVICE,
        "yolo-onnx",
        "onnx",
        "yolo-detect-auto",
    ], logs_dir, env)
    run_logged("local-plan-manifest-smoke", [
        str(PLAN_MANIFEST_EXE),
        str(policy_dir / "native-execution-plan.json"),
        str(policy_dir / "service-manifest.json"),
        SERVICE,
        "--timing-csv",
        str(timing_csv),
        "--assignment",
        assignment_name,
    ], logs_dir, env)
    timing = validate_local_timing_csv(timing_csv, assignment_rows)
    run_logged("local-provider-session-smoke", [
        str(PROVIDER_SESSION_EXE),
    ], logs_dir, env)
    return {
        "status": "executed",
        "mode": "in-memory-dependency-io",
        "timingCsv": str(timing_csv),
        "timingRows": timing["rows"],
        "roles": timing["roles"],
        "reason": (
            "C++ native plan/manifest execution baseline completed before "
            "full NDNSF network user request execution"
        ),
    }


def provider_check_command(row: dict[str, str], policy_dir: Path) -> str:
    args = [
        str(PROVIDER_EXE),
        "--plan", str(policy_dir / "native-execution-plan.json"),
        "--manifest", str(policy_dir / "service-manifest.json"),
        "--service", SERVICE,
        "--provider", row["provider"],
        "--group", GROUP,
        "--controller", CONTROLLER,
        "--trust-schema", "examples/trust-schema.conf",
        "--roles", row["role"],
        "--workers", "1",
        "--check-only",
        "--wiring-check-only",
        "--no-serve-certificates",
    ]
    return f"cd {perf.shell_quote(str(TRACER_DIR))} && exec {shell_join(args)}"


def provider_serve_command(row: dict[str, str], policy_dir: Path) -> str:
    args = [
        str(PROVIDER_EXE),
        "--plan", str(policy_dir / "native-execution-plan.json"),
        "--manifest", str(policy_dir / "service-manifest.json"),
        "--service", SERVICE,
        "--provider", row["provider"],
        "--group", GROUP,
        "--controller", CONTROLLER,
        "--trust-schema", str(policy_dir / "trust-schema.conf"),
        "--roles", row["role"],
        "--workers", "1",
        "--handler-threads", "2",
        "--ack-threads", "2",
        "--serve",
    ]
    return f"cd {perf.shell_quote(str(TRACER_DIR))} && exec {shell_join(args)}"


def controller_command(policy_dir: Path, assignment_rows: list[dict[str, str]]) -> str:
    code = (
        "from ndnsf import ServiceController; "
        "ServiceController("
        f"controller_prefix={CONTROLLER!r}, "
        f"policy_file={str(policy_dir / 'controller.policies')!r}, "
        f"trust_schema={str(policy_dir / 'trust-schema.conf')!r}, "
        "bootstrap_identities=[], "
        "serve_certificates=True"
        ").run()"
    )
    return f"cd {perf.shell_quote(str(REPO))} && exec python3 -c {perf.shell_quote(code)}"


def user_driver_command(policy_dir: Path) -> str:
    args = [
        "python3", str(USER_DRIVER),
        "--plan", str(policy_dir / "native-execution-plan.json"),
        "--service", SERVICE,
        "--group", GROUP,
        "--controller", CONTROLLER,
        "--user", USER,
        "--trust-schema", str(policy_dir / "trust-schema.conf"),
        "--ack-timeout-ms", "8000",
        "--timeout-ms", "60000",
        "--permission-wait-ms", "8000",
    ]
    return f"cd {perf.shell_quote(str(REPO))} && exec {shell_join(args)}"


def parse_user_execution(log_path: Path) -> dict[str, object]:
    prefix = "NDNSF_DI_NATIVE_TRACER_USER_EXECUTION "
    for line in read_log_text(log_path).splitlines():
        if line.startswith(prefix):
            return json.loads(line[len(prefix):])
    raise RuntimeError(f"user driver log did not contain execution JSON: {log_path}")


def observed_role_timings(log_paths: list[Path]) -> set[str]:
    roles: set[str] = set()
    for path in log_paths:
        for line in read_log_text(path).splitlines():
            if "NDNSF_DI_PROVIDER_HANDLER_TIMING" not in line or " event=end " not in line:
                continue
            for part in line.split():
                if part.startswith("role="):
                    roles.add(part[len("role="):])
    return roles


def node_client_conf(home: Path, node_name: str) -> Path:
    ndn_dir = home / ".ndn"
    ndn_dir.mkdir(parents=True, exist_ok=True)
    client_conf = ndn_dir / "client.conf"
    client_conf.write_text(
        f"transport=unix:///run/nfd/{node_name}.sock\n",
        encoding="utf-8")
    return client_conf


def run_identity_command(command: str, log_path: Path) -> None:
    with log_path.open("ab") as output:
        output.write(("RUN " + command + "\n").encode("utf-8"))
        output.flush()
        subprocess.run(command,
                       shell=True,
                       cwd=str(REPO),
                       stdout=output,
                       stderr=subprocess.STDOUT,
                       check=True)


def setup_full_network_identities(ndn,
                                  assignment_rows: list[dict[str, str]],
                                  out_dir: Path,
                                  logs_dir: Path) -> None:
    node_identities = {
        "memphis": [CONTROLLER, USER],
    }
    for row in assignment_rows:
        node_identities.setdefault(row["node"], []).append(row["provider"])

    homes = {}
    certs: list[Path] = []
    safebags: dict[str, Path] = {}
    log_path = logs_dir / "identity-bootstrap.log"
    for node_name in sorted(node_identities):
        home = Path(ndn.net[node_name].params["params"]["homeDir"])
        subprocess.run(["rm", "-rf", str(home / ".ndn")], check=False)
        client_conf = node_client_conf(home, node_name)
        homes[node_name] = (home, client_conf)

    authority_home, authority_client_conf = homes["memphis"]
    all_identities = [CONTROLLER, USER] + [
        row["provider"] for row in assignment_rows
    ]
    for identity in all_identities:
        label = identity.strip("/").replace("/", "_")
        cert_path = out_dir / f"identity-authority-{label}.cert"
        safebag_path = out_dir / f"identity-authority-{label}.safebag"
        command = (
            f"HOME={perf.shell_quote(str(authority_home))} "
            f"NDN_CLIENT_CONF={perf.shell_quote(str(authority_client_conf))} "
            f"ndnsec key-gen -t r {perf.shell_quote(identity)} > "
            f"{perf.shell_quote(str(cert_path))}"
        )
        run_identity_command(command, log_path)
        command = (
            f"HOME={perf.shell_quote(str(authority_home))} "
            f"NDN_CLIENT_CONF={perf.shell_quote(str(authority_client_conf))} "
            f"ndnsec export -i -P {perf.shell_quote(IDENTITY_SAFEBAG_PASSPHRASE)} "
            f"-o {perf.shell_quote(str(safebag_path))} "
            f"{perf.shell_quote(identity)}"
        )
        run_identity_command(command, log_path)
        certs.append(cert_path)
        safebags[identity] = safebag_path

    for node_name, identities in node_identities.items():
        if node_name == "memphis":
            continue
        home, client_conf = homes[node_name]
        for identity in identities:
            command = (
                f"HOME={perf.shell_quote(str(home))} "
                f"NDN_CLIENT_CONF={perf.shell_quote(str(client_conf))} "
                f"ndnsec import -P {perf.shell_quote(IDENTITY_SAFEBAG_PASSPHRASE)} "
                f"{perf.shell_quote(str(safebags[identity]))}"
            )
            run_identity_command(command, log_path)

    for node_name, (home, client_conf) in homes.items():
        for cert_path in certs:
            command = (
                f"HOME={perf.shell_quote(str(home))} "
                f"NDN_CLIENT_CONF={perf.shell_quote(str(client_conf))} "
                f"ndnsec cert-install -f {perf.shell_quote(str(cert_path))} "
                ">/dev/null 2>&1 || true"
            )
            run_identity_command(command, log_path)


def start_node_command(node, name: str, command: str, logs_dir: Path, env: dict[str, str], procs):
    path = logs_dir / f"{name}.log"
    output = path.open("wb")
    output.write(("RUN " + command + "\n").encode("utf-8"))
    output.flush()
    node_env = dict(env)
    home = Path(node_env.get("NDNSF_NATIVE_TRACER_SHARED_HOME") or
                node.params["params"]["homeDir"])
    ndn_dir = home / ".ndn"
    ndn_dir.mkdir(parents=True, exist_ok=True)
    client_conf = ndn_dir / "client.conf"
    client_conf.write_text(
        f"transport=unix:///run/nfd/{node.name}.sock\n",
        encoding="utf-8")
    node_env["HOME"] = str(home)
    node_env["NDN_CLIENT_CONF"] = str(client_conf)
    node_env["NDN_CLIENT_TRANSPORT"] = f"unix:///run/nfd/{node.name}.sock"
    proc = getPopen(node, command, envDict=node_env, shell=True,
                    stdout=output, stderr=subprocess.STDOUT)
    procs.append((proc, output, path))
    return proc, path


def stop_processes(procs) -> None:
    for proc, output, _ in reversed(procs):
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
        try:
            output.close()
        except Exception:
            pass


def wait_provider_checks(procs, timeout_s: int = 45) -> list[dict[str, object]]:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if all(proc.poll() is not None for proc, _, _ in procs):
            break
        time.sleep(0.2)
    results = []
    for proc, _, path in procs:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=3)
        results.append({
            "log": str(path),
            "returncode": proc.returncode,
            "status": "passed" if proc.returncode == 0 else "failed",
        })
    return results


def write_summary(out_dir: Path, summary: dict[str, object]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    lines = [
        "NDNSF Native DI Real MiniNDN Evidence",
        f"status={summary['status']}",
        f"gitCommit={summary['gitCommit']}",
        f"command={summary['command']}",
        f"resultDir={summary['resultDir']}",
        f"policyBundle={summary['policyBundle']}",
        f"assignmentCsv={summary['assignmentCsv']}",
        f"logs={summary['logs']}",
        f"miniNDNStatus={summary['miniNDNStatus']}",
        f"miniNDNRun={summary['miniNDNRun']}",
        f"runnerMode={summary['runnerMode']}",
        f"localExecution={summary['localExecution']['status']}",
        f"securityBootstrap={summary['securityBootstrap']['status']}",
        f"userExecution={summary['userExecution']['status']}",
        f"dependencyExecution={summary['dependencyExecution']['status']}",
    ]
    if summary.get("failureReason"):
        lines.append(f"failureReason={summary['failureReason']}")
    (out_dir / "summary.txt").write_text("\n".join(lines) + "\n",
                                         encoding="utf-8")
    marker = "SUCCESS" if summary["status"] == "SUCCESS" else "FAILURE"
    (out_dir / marker).touch()


def build_base_summary(args, out_dir: Path, policy_dir: Path, logs_dir: Path) -> dict[str, object]:
    git_commit = subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=str(REPO),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    ).stdout.strip() or "unknown"
    return {
        "status": "FAILURE",
        "gitCommit": git_commit,
        "command": " ".join(sys.argv),
        "resultDir": str(out_dir),
        "policyBundle": str(policy_dir),
        "nativePlan": str(policy_dir / "native-execution-plan.json"),
        "serviceManifest": str(policy_dir / "service-manifest.json"),
        "assignmentCsv": str(out_dir / "assignment.csv"),
        "logs": str(logs_dir),
        "miniNDNStatus": mini_status(),
        "miniNDNRun": "not-started",
        "securityBootstrap": {
            "status": "not-required-for-check-only",
            "reason": "provider checks validate MiniNDN placement and native provider wiring only",
        },
        "localExecution": {
            "status": "not-started",
            "reason": "local execution baseline has not run yet",
        },
        "providerChecks": [],
        "runnerMode": "none",
        "userExecution": {
            "status": "gated",
            "reason": "native tracer user request driver is not yet available",
        },
        "dependencyExecution": {
            "status": "gated",
            "reason": (
                "network dependency exchange through NdnsfCollaborationDependencyIo "
                "is gated until the native tracer user request driver is available"
            ),
        },
        "assignment": args.assignment,
        "failureReason": "",
    }


class MiniNdnArgvGuard:
    def __enter__(self):
        self._argv = sys.argv[:]
        sys.argv = [sys.argv[0]]
        return self

    def __exit__(self, exc_type, exc, tb):
        sys.argv = self._argv
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick-smoke", action="store_true")
    parser.add_argument("--out", default=str(DEFAULT_OUT))
    parser.add_argument("--assignment", choices=["default", "alternate"],
                        default="default")
    parser.add_argument("--provider-check-timeout", type=int, default=45)
    parser.add_argument("--local-execution-only", action="store_true",
                        help="Generate policy and run local native execution evidence without MiniNDN")
    parser.add_argument("--full-network", action="store_true",
                        help="Run controller, providers in --serve mode, and the NativeTracer user driver")
    parser.add_argument("--core-trace", action="store_true",
                        help="Enable NDNSF ServiceUser/ServiceProvider trace logs only for launched app processes")
    args = parser.parse_args()

    if args.quick_smoke:
        validate_prerequisites()
        print(
            "NDNSF_DI_NATIVE_TRACER_MININDN_QUICK_SMOKE_OK "
            f"topology={TOPO} provider={PROVIDER_EXE}")
        return 0

    setLogLevel("info")
    out_dir = Path(args.out)
    policy_dir = out_dir / "policy-bundle"
    logs_dir = out_dir / "logs"
    if out_dir.exists():
        subprocess.run(["rm", "-rf", str(out_dir)], check=True)
    policy_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["PYTHONPATH"] = ":".join([
        str(REPO / "NDNSF-DistributedInference"),
        str(REPO / "pythonWrapper"),
        env.get("PYTHONPATH", ""),
    ])
    env["LD_LIBRARY_PATH"] = ":".join([
        str(REPO / "build"),
        env.get("LD_LIBRARY_PATH", ""),
    ])

    summary = build_base_summary(args, out_dir, policy_dir, logs_dir)
    ndn = None
    procs = []
    try:
        if args.full_network and args.assignment != "default":
            raise RuntimeError("full-network NativeTracer currently requires --assignment default")
        validate_prerequisites()
        run_logged("plan-tracer", [
            "python3", str(PLAN_TRACER),
            "--out", str(policy_dir),
            "--summary-json", str(out_dir / "policy-summary.json"),
        ], logs_dir, env)

        roles = load_plan_roles(policy_dir / "native-execution-plan.json")
        assignment_rows = write_assignment_csv(
            out_dir / "assignment.csv",
            args.assignment,
            roles,
            assignment_for(args.assignment))
        summary["localExecution"] = run_local_execution_baseline(
            policy_dir,
            out_dir,
            logs_dir,
            env,
            args.assignment,
            assignment_rows)
        summary["dependencyExecution"] = {
            "status": "local-baseline-executed",
            "reason": (
                "in-memory native DependencyIo baseline executed; full network "
                "dependency exchange remains gated"
            ),
        }

        if args.local_execution_only:
            summary["miniNDNRun"] = "skipped-local-execution-only"
            summary["status"] = "SUCCESS"
            return_code = 0
            return return_code

        if mini_status() != "available-root":
            raise RuntimeError(f"MiniNDN normal mode requires root; status is {mini_status()}")

        with MiniNdnArgvGuard():
            Minindn.cleanUp()
            Minindn.verifyDependencies()
            ndn = Minindn(topoFile=str(TOPO))
            ndn.start()
        summary["miniNDNRun"] = "started"
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="ERROR")
        perf.wait_for_nfd_sockets(ndn, out_dir)
        if args.full_network:
            env["NDNSF_DI_RUNTIME_TIMING"] = "1"
        if args.core_trace:
            env["NDN_LOG"] = (
                "ndn_service_framework.ServiceUser=TRACE:"
                "ndn_service_framework.ServiceProvider=TRACE:"
                "ndn_service_framework.ServiceController=TRACE"
            )
        if args.full_network:
            setup_full_network_identities(ndn, assignment_rows, out_dir, logs_dir)

        routing = NdnRoutingHelper(ndn.net, "udp", "link-state")
        routing.addOrigin([ndn.net["memphis"]], [CONTROLLER, GROUP, USER])
        for row in assignment_rows:
            routing.addOrigin([ndn.net[row["node"]]], [row["provider"], GROUP])
        routing.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, "/NDNSF-DI/Tracer", Nfdc.STRATEGY_MULTICAST)

        if args.full_network:
            controller_proc, controller_log = start_node_command(
                ndn.net["memphis"],
                "controller",
                controller_command(policy_dir, assignment_rows),
                logs_dir,
                env,
                procs)
            time.sleep(2.0)
            if controller_proc.poll() is not None:
                raise RuntimeError(
                    f"controller exited before user execution; see {controller_log}")

            provider_logs = []
            for row in assignment_rows:
                node = ndn.net[row["node"]]
                command = provider_serve_command(row, policy_dir)
                proc, path = start_node_command(
                    node,
                    "provider-serve-" + row["role"].strip("/").replace("/", "-"),
                    command,
                    logs_dir,
                    env,
                    procs)
                provider_logs.append(path)
            wait_for_log_patterns(provider_logs,
                                  ["NDNSF_DI_NATIVE_PROVIDER_PROVISION_READY"],
                                  args.provider_check_timeout,
                                  "native provider provisioning")
            time.sleep(8.0)
            user_proc, user_log = start_node_command(
                ndn.net["memphis"],
                "user-driver",
                user_driver_command(policy_dir),
                logs_dir,
                env,
                procs)
            try:
                user_proc.wait(timeout=75)
            except Exception:
                user_proc.kill()
                user_proc.wait(timeout=3)
            user_result = parse_user_execution(user_log)
            if user_proc.returncode != 0 or user_result.get("status") != "executed":
                raise RuntimeError(
                    f"NativeTracer user execution failed rc={user_proc.returncode} "
                    f"result={user_result}; see {user_log}")
            roles = observed_role_timings(provider_logs)
            missing_roles = sorted(set(REQUIRED_ROLES) - roles)
            if missing_roles:
                raise RuntimeError(
                    f"missing provider role timing logs for {missing_roles}; logs={provider_logs}")
            summary["runnerMode"] = "qwen-onnx-native"
            summary["providerChecks"] = [
                {
                    "log": str(path),
                    "status": "served",
                }
                for path in provider_logs
            ]
            summary["securityBootstrap"] = {
                "status": "executed",
                "reason": "ServiceController and user/provider permission fetch path ran during full-network execution",
            }
            summary["userExecution"] = {
                "status": "executed",
                "reason": "NativeTracer user driver returned a successful NDNSF collaboration response",
                "log": str(user_log),
                "payloadBytes": user_result.get("payloadBytes", 0),
                "elapsedMs": user_result.get("elapsedMs", 0.0),
            }
            summary["dependencyExecution"] = {
                "status": "executed",
                "reason": "all NativeTracer roles emitted provider handler timing in serve mode",
                "roles": sorted(roles),
            }
            summary["status"] = "SUCCESS"
            return 0

        for row in assignment_rows:
            node = ndn.net[row["node"]]
            command = provider_check_command(row, policy_dir)
            start_node_command(node,
                               "provider-check-" + row["role"].strip("/").replace("/", "-"),
                               command,
                               logs_dir,
                               env,
                               procs)
        provider_results = wait_provider_checks(procs, args.provider_check_timeout)
        summary["providerChecks"] = provider_results
        failed = [item for item in provider_results if item["returncode"] != 0]
        if failed:
            raise RuntimeError(f"provider checks failed: {failed}")

        summary["status"] = "SUCCESS"
    except Exception as exc:
        summary["status"] = "FAILURE"
        summary["failureReason"] = str(exc)
    finally:
        stop_processes(procs)
        if ndn is not None:
            try:
                ndn.stop()
            except Exception:
                pass
        try:
            with MiniNdnArgvGuard():
                Minindn.cleanUp()
        except Exception:
            pass
        write_summary(out_dir, summary)
        print((out_dir / "summary.txt").read_text(encoding="utf-8"))
    return 0 if summary["status"] == "SUCCESS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
