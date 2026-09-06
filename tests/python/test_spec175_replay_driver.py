from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
DRIVER_PATH = ROOT / "packaging/ndnsf-di-container/jobs/spec175/replay-exact-sif.py"


def _driver_module():
    spec = importlib.util.spec_from_file_location("spec175_replay_driver", DRIVER_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def test_replay_driver_has_bounded_matrix_contract():
    driver = DRIVER_PATH.read_text(encoding="utf-8")
    assert "CASE_WATCHDOG_SECONDS = 900" in driver
    assert "CAMPAIGN_WATCHDOG_SECONDS = 14400" in driver
    assert "start_new_session=True" in driver
    assert '"notRunEntries"' in driver
    assert '"firstIncomplete"' in driver
    assert "ALL_FOUR_PROVIDER_READINESS_MISSING" in driver


def test_provider_readiness_requires_all_four_logs(tmp_path):
    module = _driver_module()
    (tmp_path / "stage0-provider.log").write_text(
        "LLM_PIPELINE_PROVIDER_READY\n", encoding="utf-8")
    (tmp_path / "stage1-provider.log").write_text(
        "LLM_PIPELINE_PROVIDER_READY\n", encoding="utf-8")
    (tmp_path / "stage2-provider.log").write_text(
        "LLM_PIPELINE_PROVIDER_READY\n", encoding="utf-8")
    snapshot = module.provider_readiness(tmp_path)
    assert snapshot["readyCount"] == 3
    assert snapshot["allFourReady"] is False

    (tmp_path / "stage3-provider.log").write_text(
        "LLM_PIPELINE_PROVIDER_READY\n", encoding="utf-8")
    snapshot = module.provider_readiness(tmp_path)
    assert snapshot["readyCount"] == 4
    assert snapshot["allFourReady"] is True


def test_case_watchdog_reaps_process_group(tmp_path, monkeypatch):
    module = _driver_module()
    monkeypatch.setattr(module, "CASE_WATCHDOG_SECONDS", 0.05)
    monkeypatch.setattr(module, "PROCESS_TERM_GRACE_SECONDS", 1)
    command = [sys.executable, "-c", "import time; time.sleep(2)"]
    report = module.run_case(
        command,
        tmp_path / "M01-r1",
        "M01-r1",
        dict(module.os.environ),
        time.monotonic() + 5,
        "M01",
    )
    assert report["status"] == "FAIL"
    assert report["returncode"] == 124
    assert report["failureReason"] == "CASE_WATCHDOG_TIMEOUT"
    assert (tmp_path / "M01-r1" / "g4.stdout").is_file()
    assert (tmp_path / "M01-r1" / "g4.stderr").is_file()


def test_case_pass_requires_four_provider_readiness_records(tmp_path):
    module = _driver_module()
    terminal = {
        "schema": "ndnsf-di-spec175-terminal-evidence-v1",
        "status": "PASS",
        "resultWrittenAfterProcessExit": True,
        "abortObserved": False,
        "unexpectedSignalExits": {},
        "survivingOwnedProcesses": [],
        "childExitCodes": {"controller": 0},
    }
    case_result = {
        "schema": "ndnsf-di-spec175-minindn-case-result-v1",
        "status": "PASS",
        "case": "M01",
        "terminalEvidence": terminal,
    }
    output = tmp_path / "M01-r1"
    script = (
        "import json,pathlib,sys;"
        "p=pathlib.Path(sys.argv[1]);"
        "(p/'spec175-case-result.json').write_text(json.dumps(" +
        repr(case_result) + "));"
        "[(p/f'stage{i}-provider.log').write_text('LLM_PIPELINE_PROVIDER_READY\\n') "
        "for i in range(3)]"
    )
    report = module.run_case(
        [sys.executable, "-c", script, str(output)], output, "M01-r1",
        dict(module.os.environ), time.monotonic() + 5, "M01")
    assert report["status"] == "FAIL"
    assert report["failureReason"] == "ALL_FOUR_PROVIDER_READINESS_MISSING"
    assert report["providerReadiness"]["readyCount"] == 3
