from __future__ import annotations

from pathlib import Path
import json


ROOT = Path(__file__).resolve().parents[2]


def test_legacy_default_routes_are_explicitly_inventory_owned() -> None:
    design = (ROOT / "specs/182-native-di-python-bindings/contracts/code-design.md").read_text(
        encoding="utf-8")
    for path in (
            "NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py",
            "NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1.py",
            "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/facades.py",
            "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py"):
        assert path in design
    assert "RETIRE default execution" in design
    assert "defaultReachable=false" in (
        ROOT / "specs/182-native-di-python-bindings/contracts/runtime-boundaries.md"
    ).read_text(encoding="utf-8")


def test_native_binding_has_no_python_strategy_or_legacy_import() -> None:
    source = (ROOT / "pythonWrapper/src/ndnsf/di_bindings.cpp").read_text(
        encoding="utf-8")
    # A Python callback is allowed only at the terminal observer facade; model
    # selection, planning, and execution strategy remain native-owned.
    assert 'py::function observer' in source
    assert 'NativeInferenceHandle::observe' not in source
    assert "placement.py" not in source
    assert "runtime_v1" not in source
    assert "subprocess" not in source


def test_setup_compiles_binding_and_links_the_shared_di_library() -> None:
    setup = (ROOT / "pythonWrapper/setup.py").read_text(encoding="utf-8")
    assert '"src/ndnsf/di_bindings.cpp"' in setup
    assert '"ndnsf-distributed-inference"' in setup
    assert "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.cpp" not in setup
    assert "NativeGrantVerifier.cpp" not in setup


def test_maintained_qwen_routes_use_explicit_native_config_without_fallback() -> None:
    user = (ROOT / "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py").read_text(
        encoding="utf-8")
    client = (ROOT / "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py").read_text(
        encoding="utf-8")
    qwen_harness = (ROOT / "Experiments/NDNSF_DI_QwenAckDriven_Minindn.py").read_text(
        encoding="utf-8")
    stream_harness = (ROOT / "Experiments/NDNSF_DI_StreamedGeneration_Minindn.py").read_text(
        encoding="utf-8")
    assert "--native-requester-config" in user
    assert "configure_native_requester_from_config" in user
    assert "request_native_payload" in user
    assert "refusing Python planner fallback" in client
    assert "native-requester-config" in qwen_harness
    assert "native-requester-config" in stream_harness
    assert "native requester route currently supports only Qwen runtimes" in user
    assert "NativeServiceUser currently has no configured NativeConversationConfig" in user
    assert "on_event=on_native_event" in user
    assert "native observer terminal notification did not arrive" in user
    assert 'response["streamEventCount"]' in user
    assert "on_event" in client
    # The native branch is selected before the automatic planner branch and
    # remains explicit in the maintained caller source.
    assert user.index("if args.native_requester_config:") < user.index(
        "elif args.automatic_planning_manifest:")


def test_maintained_yolo_native_route_is_explicit_and_fail_closed() -> None:
    user = (ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2/user.py").read_text(
        encoding="utf-8")
    assert "--native-requester-config" in user
    assert "_load_yolo_native_payload" in user
    assert "configure_native_requester_from_config" in user
    assert "request_native_reference" in user
    assert "native YOLO requester model identity does not match package" in user
    assert "does not yet support Spec180 lifecycle journaling" in user
    assert "requires --native-tensor-input" in user
    assert "configure_automatic_planning" in user
    assert "request_task" in user
    # The native branch is selected immediately after APPClient construction;
    # its body cannot fall through into the ACK-driven Python planner.
    assert user.index("if args.native_requester_config:") < user.index(
        "if not args.offline_oracle:")
    native_branch = user[user.index("def _load_yolo_native_payload"):user.index(
        "def main()")]
    assert "configure_automatic_planning" not in native_branch
    assert "request_task" not in native_branch


def test_maintained_yolo_native_route_uses_repository_reference() -> None:
    user = (ROOT / "examples/python/NDNSF-DistributedInference/yolo_2x2/user.py").read_text(
        encoding="utf-8")
    native_branch = user[user.index("def _load_yolo_native_payload"):user.index(
        "def main()")]
    assert "publish_application_input_reference" in native_branch
    assert "request_native_reference" in native_branch
    assert "request_native_payload" not in native_branch
    assert native_branch.index("publish_application_input_reference") < native_branch.index(
        "request_native_reference")
    assert "fetch/decrypt" in native_branch


def test_legacy_runtime_removal_stays_blocked_by_manifest_consumers() -> None:
    manifest = json.loads(
        (ROOT / "specs/182-native-di-python-bindings/contracts/compatibility-manifest.json")
        .read_text(encoding="utf-8"))
    legacy_paths = {
        "NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py",
        "NDNSF-DistributedInference/ndnsf_distributed_inference/runtime_v1.py",
        "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/facades.py",
        "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py",
    }
    entries = [
        entry for entry in manifest["entries"]
        if entry.get("source", {}).get("path") in legacy_paths
    ]
    assert entries
    assert all(entry.get("removalEligible") is False for entry in entries)
    assert all(entry.get("status") in {
        "RETAINED_UNTIL_MIGRATION", "PLANNED_NATIVE"
    } for entry in entries)
    assert all(entry.get("externalUseStatus") in {
        "repository_callers_found", "external_use_unknown"
    } for entry in entries)
