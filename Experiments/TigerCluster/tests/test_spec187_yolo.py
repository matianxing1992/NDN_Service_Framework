"""Static orchestration checks for the Spec187 YOLO native selector."""

import importlib.util
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT.parent / "NDNSF_DI_YoloAckDriven_Minindn.py"
spec = importlib.util.spec_from_file_location("spec187_yolo_runner", SCRIPT)
runner = importlib.util.module_from_spec(spec)
assert spec.loader is not None
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)


def test_spec187_selector_has_independent_cpp_suite_and_waf_target():
    source = (ROOT.parent.parent / "tests/integration-tests/di-prepared-request.t.cpp").read_text()
    assert "SPEC187_YOLO_SELECTOR" in source
    assert "BOOST_AUTO_TEST_SUITE(Spec187YoloMiniNdn)" in source
    assert "NativeRequesterThroughMiniNdn" in source
    assert "SPEC187_NATIVE_REQUEST_PASS" in source
    assert "16 * 1024 * 1024" in source
    assert "NDNSF_DI_NATIVE_ACK_CLOSED" in source
    assert "NDNSF_DI_NATIVE_SELECTION_COMMITTED" in source
    assert "NDNSF_DI_NATIVE_SELECTION_ACCEPTED" in source
    assert "NDNSF_DI_NATIVE_PROVIDER_EXECUTION_COMPLETED" in source
    assert "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED" in source
    assert "Spec187 native stage evidence is out of order" in source
    assert "spec187FieldPosition" in source
    wscript = (ROOT.parent.parent / "tests/wscript").read_text()
    assert "name='spec187-yolo-minindn'" in wscript
    assert "'SPEC187_YOLO_SELECTOR'" in wscript
    runner_source = SCRIPT.read_text()
    assert 'SPEC187_NATIVE_MODE' in runner_source
    assert 'SPEC187_NATIVE_SELECTOR_REQUIRED' in runner_source
    assert 'SPEC187_NATIVE_AUTHORITY_CONFIG_REQUIRED' in runner_source
    assert 'NATIVE_GRANT_AUTHORITY_READY' in runner_source
    assert 'DI_NativeArtifactAuthority' in runner_source
    assert 'SPEC181_GRANT_AUTHORITY_PUBLIC_KEY' in runner_source
    assert 'SPEC181_PROVIDER_RECIPIENT_KEY_MAP' in runner_source
    assert 'SPEC187_NATIVE_SELECTOR_FILTER' not in runner_source
    assert 'SPEC187_NATIVE_REQUEST_PASS' in runner_source
