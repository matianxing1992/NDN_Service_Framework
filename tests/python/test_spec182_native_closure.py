from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
RUNNER_PATH = ROOT / "tests/standalone/run-spec182-native-closure.py"
SPEC = importlib.util.spec_from_file_location("spec182_native_closure", RUNNER_PATH)
assert SPEC and SPEC.loader
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

MININDN_PATH = ROOT / "Experiments/NDNSF_DI_NativeClosure_Minindn.py"
MININDN_SPEC = importlib.util.spec_from_file_location("spec182_minindn", MININDN_PATH)
assert MININDN_SPEC and MININDN_SPEC.loader
minindn = importlib.util.module_from_spec(MININDN_SPEC)
MININDN_SPEC.loader.exec_module(minindn)


def _manifest(tmp_path: Path, *, source: Path | None = None,
              business_marker: str | None = None) -> Path:
    source = source or Path("/bin/true")
    digest = "sha256:" + hashlib.sha256(source.read_bytes()).hexdigest()
    case = {
        "id": "positive",
        "expectedExit": 0,
        "isolation": {
            "schema": runner.ISOLATION_SCHEMA,
            "artifacts": [{
                "source": str(source), "target": "/bin/true",
                "sha256": digest, "kind": "executable", "mode": "0555",
            }],
            "processes": [{
                "id": "requester", "role": "requester",
                "executable": "/bin/true",
                "argv": ["/probe-root/bin/true"], "env": {},
            }],
            "childProcesses": [], "endpoints": [], "tools": {},
            "limits": {"runSeconds": 10, "cleanupSeconds": 5,
                        "traceBytes": 1024 * 1024},
            "requiredEvidence": sorted(runner.REQUIRED_EVIDENCE),
        },
    }
    if business_marker is not None:
        case["businessOracle"] = {"stdoutMarker": business_marker}
    document = {
        "schema": runner.MANIFEST_SCHEMA,
        "cases": [case],
    }
    path = tmp_path / "case-manifest.json"
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


def test_native_positive(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    staged = runner.stage_root(case, tmp_path / "run")
    command = runner.make_launch(case, staged, {"id": ""}, tmp_path / "run/trace.txt")
    assert "--unshare-all" in command
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False,
               "evidence": sorted(runner.REQUIRED_EVIDENCE)},
        {"complete": True, "violations": []})
    assert result["status"] == "PASS"


def test_helper_exec_rejected(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path, source=Path("/usr/bin/python3")), "positive")
    try:
        runner.stage_root(case, tmp_path / "run")
    except runner.PreflightError as exc:
        assert "Python runtime" in str(exc)
    else:
        raise AssertionError("Python executable was accepted")


def test_transient_python_mapping_rejected(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False,
               "evidence": sorted(runner.REQUIRED_EVIDENCE)},
        {"complete": True, "violations": ["PYTHON_MAPPING"]})
    assert result["status"] == "FAIL"
    assert "PYTHON_MAPPING" in result["failures"]


def test_undeclared_endpoint_rejected(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False,
               "evidence": sorted(runner.REQUIRED_EVIDENCE)},
        {"complete": True, "violations": ["UNDECLARED_ENDPOINT"]})
    assert result["status"] == "FAIL"


def test_incomplete_observation_unqualified(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False,
               "evidence": sorted(runner.REQUIRED_EVIDENCE)},
        {"complete": False, "violations": []})
    assert result["status"] == "UNQUALIFIED"
    assert "OBSERVATION_UNQUALIFIED" in result["failures"]


def test_cold_path_and_role_coverage_required(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    case["cold"] = True
    case["requiredRoles"] = ["requester", "provider"]
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False,
               "evidence": sorted(runner.REQUIRED_EVIDENCE)},
        {"complete": True, "violations": []})
    assert result["status"] == "UNQUALIFIED"
    assert "ROLE_OBSERVATION_MISSING" in result["failures"]
    assert "COLD_PATH_OBSERVATION_MISSING" in result["failures"]


def test_cold_path_and_role_coverage_passes_with_verified_observation(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    case["cold"] = True
    case["requiredRoles"] = ["requester", "provider"]
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False,
               "evidence": sorted(runner.REQUIRED_EVIDENCE)},
        {"complete": True, "violations": [],
         "roles": ["requester", "provider"], "coldVerified": True})
    assert result["status"] == "PASS"


def test_role_or_cold_mismatch_is_a_complete_failure(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    case["cold"] = True
    case["requiredRoles"] = ["requester", "provider"]
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False,
               "evidence": sorted(runner.REQUIRED_EVIDENCE)},
        {"complete": True, "violations": [],
         "roles": ["requester"], "coldVerified": False})
    assert result["status"] == "FAIL"
    assert "ROLE_COVERAGE_MISMATCH" in result["failures"]
    assert "COLD_PATH_MISMATCH" in result["failures"]


def test_duplicate_role_observation_is_unqualified(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    case["requiredRoles"] = ["requester", "provider"]
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False,
               "evidence": sorted(runner.REQUIRED_EVIDENCE)},
        {"complete": True, "violations": [],
         "roles": ["requester", "requester"], "coldVerified": False})
    assert result["status"] == "UNQUALIFIED"
    assert "ROLE_OBSERVATION_INVALID" in result["failures"]


def test_external_harness_excluded(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    assert all(process["role"] != "harness" for process in case["isolation"]["processes"])


def _node_context(tmp_path: Path, *, start_ticks: int | None = None) -> tuple[dict, socket.socket]:
    socket_path = tmp_path / "nfd.sock"
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(str(socket_path))
    netns_path = Path("/proc/self/ns/net")
    return ({
        "id": "requester",
        "netnsPath": str(netns_path),
        "netnsInode": netns_path.stat().st_ino,
        "ownerPid": os.getpid(),
        "ownerStartTicks": (runner._read_proc_start_ticks(os.getpid())
                             if start_ticks is None else start_ticks),
        "nfdSocket": str(socket_path),
        "peerNodeIds": ["provider"],
        "_namespaceFdPath": "/proc/self/fd/9",
    }, sock)


def test_declared_node_requires_valid_context(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    case["isolation"]["processes"][0]["node"] = "requester"
    staged = runner.stage_root(case, tmp_path / "run")
    try:
        runner.run_case(case, staged, tmp_path / "run", nodes={})
    except runner.PreflightError as exc:
        assert "node context is missing" in str(exc)
    else:
        raise AssertionError("missing node context was accepted")


def test_node_context_binds_namespace_and_nfd_identity(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    case["isolation"]["processes"][0]["node"] = "requester"
    node, sock = _node_context(tmp_path)
    try:
        validated = runner._validate_node_context(case, node)
        assert validated["netnsInode"] == node["netnsInode"]
        command = runner.make_launch(case, runner.stage_root(case, tmp_path / "run"),
                                     node, tmp_path / "run/trace.txt")
        assert command[0] == "nsenter"
        assert command[1] == "--net=/proc/self/fd/9"
        assert "--unshare-all" in command
    finally:
        sock.close()


def test_dynamic_shared_libraries_are_bound_at_absolute_elf_paths(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    library = Path("/lib/x86_64-linux-gnu/libc.so.6")
    case["isolation"]["artifacts"].append({
        "source": str(library), "target": str(library),
        "sha256": "sha256:" + hashlib.sha256(library.read_bytes()).hexdigest(),
        "kind": "shared-library", "mode": "0555",
    })
    staged = runner.stage_root(case, tmp_path / "run")
    command = runner.make_launch(case, staged, {"id": ""}, tmp_path / "run/trace.txt")
    mount_index = command.index("--ro-bind", command.index("--ro-bind") + 1)
    assert str(library) in command[mount_index:mount_index + 4]


def test_node_context_rejects_stale_owner_starttime(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    case["isolation"]["processes"][0]["node"] = "requester"
    node, sock = _node_context(tmp_path, start_ticks=1)
    try:
        try:
            runner._validate_node_context(case, node)
        except runner.PreflightError as exc:
            assert "starttime changed" in str(exc)
        else:
            raise AssertionError("stale owner identity was accepted")
    finally:
        sock.close()


def test_node_context_rejects_unheld_namespace_path(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    case["isolation"]["processes"][0]["node"] = "requester"
    node, sock = _node_context(tmp_path)
    node["_namespaceFdPath"] = "/proc/1/ns/net"
    try:
        try:
            runner.make_launch(case, runner.stage_root(case, tmp_path / "run"),
                               node, tmp_path / "run/trace.txt")
        except runner.PreflightError as exc:
            assert "namespace FD" in str(exc)
        else:
            raise AssertionError("unheld namespace path was accepted")
    finally:
        sock.close()


def test_unbound_process_does_not_adopt_incidental_namespace_fd(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    node, sock = _node_context(tmp_path)
    try:
        command = runner.make_launch(case, runner.stage_root(case, tmp_path / "run"),
                                     node, tmp_path / "run/trace.txt")
        assert command[0] != "nsenter"
    finally:
        sock.close()


def test_descendant_cleanup_required(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False},
        {"complete": False, "violations": ["OWNED_PROCESS_ALIVE"]})
    assert result["status"] == "UNQUALIFIED"
    assert "OBSERVATION_UNQUALIFIED" in result["failures"]


def test_missing_evidence_is_unqualified(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": False, "evidence": []},
        {"complete": True, "violations": []})
    assert result["status"] == "UNQUALIFIED"
    assert "MISSING_EVIDENCE:identity" in result["failures"]


def test_timeout_is_unqualified(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path), "positive")
    result = runner.evaluate_case(
        case, {"returncode": 0, "timedOut": True,
               "evidence": sorted(runner.REQUIRED_EVIDENCE)},
        {"complete": True, "violations": []})
    assert result["status"] == "UNQUALIFIED"
    assert "RUN_TIMEOUT" in result["failures"]


def test_trace_integrity_is_separate_from_policy_violation(tmp_path: Path) -> None:
    trace = tmp_path / "trace.txt"
    trace.write_text(
        '123 execve("/probe-root/bin/true", ["true"], 0x0) = 0\n'
        '123 connect(3, {sa_family=AF_INET, sin_port=80}, 0) = 0\n'
        '123 exit_group(0) = ?\n', encoding="utf-8")
    observation = runner.collect_trace({}, {"trace": str(trace)})
    assert observation["complete"] is True
    assert observation["integrityViolations"] == []
    assert observation["policyViolations"] == ["UNDECLARED_ENDPOINT"]


def test_trace_integrity_accepts_paired_unfinished_syscalls(tmp_path: Path) -> None:
    trace = tmp_path / "trace.txt"
    trace.write_text(
        '123 openat(AT_FDCWD, "x", O_RDONLY <unfinished ...>\n'
        '123 <... openat resumed>)          = 3\n'
        '123 execve("/probe-root/bin/true", ["true"], 0x0) = 0\n'
        '123 exit_group(0)                   = ?\n', encoding="utf-8")
    observation = runner.collect_trace({}, {"trace": str(trace)})
    assert observation["complete"] is True
    assert observation["integrityViolations"] == []


def test_trace_integrity_rejects_unpaired_unfinished_syscall(tmp_path: Path) -> None:
    trace = tmp_path / "trace.txt"
    trace.write_text('123 openat(AT_FDCWD, "x", O_RDONLY <unfinished ...>\n', encoding="utf-8")
    observation = runner.collect_trace({}, {"trace": str(trace)})
    assert observation["complete"] is False
    assert observation["integrityViolations"] == ["TRACE_UNPAIRED"]


def test_trace_derives_runtime_evidence_and_accepts_declared_marker(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path, business_marker="NATIVE_OK"), "positive")
    trace = tmp_path / "trace.txt"
    trace.write_text(
        '123 execve("/probe-root/bin/true", ["true"], 0x0) = 0\n'
        '123 exit_group(0)                   = ?\n', encoding="utf-8")
    stdout = tmp_path / "stdout.log"
    stdout.write_text("NATIVE_OK\n", encoding="utf-8")
    run = {"trace": str(trace), "stdout": str(stdout), "returncode": 0,
           "timedOut": False, "supervisorPid": 123,
           "command": ["strace", "bwrap", "--unshare-all"]}
    observation = runner.collect_trace(case, run)
    assert observation["complete"] is True
    assert set(runner.REQUIRED_EVIDENCE) <= set(observation["evidence"])
    assert runner.evaluate_case(case, run, observation)["status"] == "PASS"


def test_declared_business_marker_missing_keeps_case_unqualified(tmp_path: Path) -> None:
    case = runner.load_case(_manifest(tmp_path, business_marker="NATIVE_OK"), "positive")
    trace = tmp_path / "trace.txt"
    trace.write_text(
        '123 execve("/probe-root/bin/true", ["true"], 0x0) = 0\n'
        '123 exit_group(0)                   = ?\n', encoding="utf-8")
    run = {"trace": str(trace), "stdout": str(tmp_path / "stdout.log"),
           "returncode": 0, "timedOut": False, "supervisorPid": 123,
           "command": ["strace", "bwrap", "--unshare-all"]}
    (tmp_path / "stdout.log").write_text("", encoding="utf-8")
    observation = runner.collect_trace(case, run)
    result = runner.evaluate_case(case, run, observation)
    assert result["status"] == "UNQUALIFIED"
    assert "MISSING_EVIDENCE:business-oracle" in result["failures"]


def test_minindn_owner_does_not_fake_native_qualification(tmp_path: Path) -> None:
    manifest = _manifest(tmp_path)
    document = json.loads(manifest.read_text(encoding="utf-8"))
    document["campaignCase"] = "positive"
    manifest.write_text(json.dumps(document), encoding="utf-8")
    output = tmp_path / "campaign"
    assert minindn.run_campaign(manifest, output) == 2
    assert json.loads((output / "result.json").read_text())["status"] == "UNQUALIFIED"


def test_minindn_registration_covers_counterexamples_and_proof_cases() -> None:
    manifest = ROOT / "tests/fixtures/spec182/case-manifest.json"
    registration = minindn.load_registration(manifest)
    ids = {case["id"] for case in registration["cases"]}
    assert set(minindn.COUNTEREXAMPLES) <= ids
    assert set(minindn.PROOF_CASES) <= ids
    assert registration["runner"] == "tests/standalone/run-spec182-native-closure.py"
    assert registration["limits"] == {
        "runSeconds": 180, "cleanupSeconds": 15, "traceBytes": 268435456,
    }


def test_minindn_registration_writes_fresh_unqualified_record(tmp_path: Path) -> None:
    manifest = ROOT / "tests/fixtures/spec182/case-manifest.json"
    document = json.loads(manifest.read_text(encoding="utf-8"))
    document["campaignCase"] = "I01"
    selected = tmp_path / "manifest.json"
    selected.write_text(json.dumps(document), encoding="utf-8")
    output = tmp_path / "campaign"
    assert minindn.run_campaign(selected, output) == 2
    result = json.loads((output / "result.json").read_text(encoding="utf-8"))
    assert result["status"] == "UNQUALIFIED"
    assert result["reason"] == "MININDN_NODE_CONTEXT_NOT_PROVIDED"
    assert result["campaignCase"] == "I01"
    assert len(result["registeredCases"]) == 22


def test_minindn_registration_refuses_existing_output(tmp_path: Path) -> None:
    manifest = ROOT / "tests/fixtures/spec182/case-manifest.json"
    document = json.loads(manifest.read_text(encoding="utf-8"))
    document["campaignCase"] = "I01"
    selected = tmp_path / "manifest.json"
    selected.write_text(json.dumps(document), encoding="utf-8")
    output = tmp_path / "campaign"
    output.mkdir()
    marker = output / "existing.txt"
    marker.write_text("keep", encoding="utf-8")
    assert minindn.run_campaign(selected, output) == 2
    assert marker.read_text(encoding="utf-8") == "keep"
    assert not (output / "result.json").exists()


def test_minindn_owner_exports_identity_bound_node_context(tmp_path: Path) -> None:
    socket_paths = {}
    sockets = []
    for name in ("requester", "provider"):
        path = tmp_path / f"{name}.sock"
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.bind(str(path))
        socket_paths[name] = path
        sockets.append(sock)
    try:
        nodes = [SimpleNamespace(name=name, inNamespace=True, pid=os.getpid())
                 for name in ("requester", "provider")]
        contexts = minindn.collect_node_context(
            nodes, socket_paths,
            {"requester": ["provider"], "provider": ["requester"]})
        assert set(contexts) == {"requester", "provider"}
        assert contexts["requester"]["netnsInode"] == Path("/proc/self/ns/net").stat().st_ino
        assert contexts["requester"]["ownerStartTicks"] > 0
    finally:
        for sock in sockets:
            sock.close()


def test_minindn_owner_rejects_host_namespace_node(tmp_path: Path) -> None:
    path = tmp_path / "requester.sock"
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(str(path))
    try:
        node = SimpleNamespace(name="requester", inNamespace=False, pid=os.getpid())
        try:
            minindn.collect_node_context([node], {"requester": path},
                                         {"requester": ["provider"]})
        except ValueError as exc:
            assert "namespace-isolated" in str(exc)
        else:
            raise AssertionError("host namespace node was accepted")
    finally:
        sock.close()


def test_minindn_owner_rejects_peer_outside_topology(tmp_path: Path) -> None:
    path = tmp_path / "requester.sock"
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(str(path))
    try:
        node = SimpleNamespace(name="requester", inNamespace=True, pid=os.getpid())
        try:
            minindn.collect_node_context([node], {"requester": path},
                                         {"requester": ["ghost"]})
        except ValueError as exc:
            assert "peer metadata" in str(exc)
        else:
            raise AssertionError("peer outside topology was accepted")
    finally:
        sock.close()
