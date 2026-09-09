from __future__ import annotations

from pathlib import Path


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
    assert "py::function" not in source
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
    # The native branch is selected before the automatic planner branch and
    # remains explicit in the maintained caller source.
    assert user.index("if args.native_requester_config:") < user.index(
        "elif args.automatic_planning_manifest:")
