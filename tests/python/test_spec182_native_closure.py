from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path


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


def _manifest(tmp_path: Path, *, source: Path | None = None) -> Path:
    source = source or Path("/bin/true")
    digest = "sha256:" + hashlib.sha256(source.read_bytes()).hexdigest()
    document = {
        "schema": runner.MANIFEST_SCHEMA,
        "cases": [{
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
        }],
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
