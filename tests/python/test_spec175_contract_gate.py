from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts/spec175_contract_gate.py"
FEATURE = REPO / "specs/175-ndnsf-di-streamed-invocation"


def _load_gate():
    spec = importlib.util.spec_from_file_location("spec175_contract_gate", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_tlv_scan_reports_every_owner_in_reserved_range(tmp_path: Path) -> None:
    gate = _load_gate()
    source = tmp_path / "protocol.hpp"
    source.write_text(
        "enum Types { ExistingA = 0xF636, ExistingB = 0xF657, Outside = 0xF700 };\n",
        encoding="utf-8",
    )

    collisions = gate.scan_tlv_collisions(tmp_path, 0xF636, 0xF657)

    assert [(item["name"], item["value"]) for item in collisions] == [
        ("ExistingA", "0xF636"),
        ("ExistingB", "0xF657"),
    ]


def test_tlv_scan_ignores_hidden_local_scratch_roots(tmp_path: Path) -> None:
    gate = _load_gate()
    scratch = tmp_path / ".worker-tmp"
    scratch.mkdir()
    (scratch / "stale.hpp").write_text(
        "enum Types { StaleCopy = 0xF636 };\n", encoding="utf-8")

    assert gate.scan_tlv_collisions(tmp_path, 0xF636, 0xF657) == []


def test_source_census_accepts_canonical_streaming_fixture(tmp_path: Path) -> None:
    gate = _load_gate()
    files = {
        "ndn-service-framework/InvocationStream.hpp": """
          struct StreamRequestOptions {};
          struct InvocationEventMessage {};
          struct StreamCompletion {};
        """,
        "ndn-service-framework/ServiceUser.hpp": """
          RequestServiceStreaming(const ndn::Name& serviceName, const Request& request);
        """,
        "ndn-service-framework/ServiceProvider.hpp": """
          void addStreamingHandler(); struct StreamedResponseWriter {};
        """,
        "NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp": """
          require(maxGeneratedTokens <= 64, "bound");
          const char* role = "/LLM/Pipeline/Stage/0";
        """,
        "NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/placement.py": """
          QWEN36_27B_PRECISION = "float16"
          QWEN36_27B_DECODE_MODE = "single-token-autoregressive"
          QWEN36_27B_MODALITY = "text-only"
          QWEN36_27B_MTP_ENABLED = False
          QWEN36_27B_THINKING_MODE = "disabled"
        """,
        "tools/ndnsf-di/export_spec175_qwen36_stateful_onnx.py": """
          DECODE_MODE = "single-token-autoregressive"
          MODALITY = "text-only"
          MTP_ENABLED = False
          THINKING_MODE = "disabled"
        """,
        "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py": """
          def full_generation_call():
              application_input = adapter.encode_input(payload, {
                  "useCache": True,
                  "outputMode": "TOKEN_STREAMING",
              })
              handle = coordinator.request_streaming(service_name, application_input)
        """,
        "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py": """
          GenerationExecutionContractV1()
          TOKEN_FEEDBACK = "TOKEN_FEEDBACK"
          streaming_operation_stride = 3
          max_generated_tokens = 64
          generation_contract=generation_contract
        """,
        "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp": """
          generationConfigFromAuthenticatedRequest(request);
          projection->generationContract.enabled;
          authenticatedGeneration.maxEpochs;
          runNativeEpochCoordinator(config);
        """,
        "examples/DI_NativeProviderExecutable.cpp": """
          fields.find("executionPlanDigest");
          capability.planDigest != authenticatedPlanDigest;
        """,
        "NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py": """
          class ProviderRuntimeContext:
              def publish_event(self, event): ...
              def finish_stream(self, response): ...
        """,
        "pythonWrapper/ndnsf/service.py": """
          async def request_streaming(service_name, request): ...
        """,
    }
    for relative, contents in files.items():
        path = tmp_path / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    assert gate.check_source_contract(tmp_path) == []


def test_source_census_rejects_each_known_initial_drift(tmp_path: Path) -> None:
    gate = _load_gate()
    qwen = tmp_path / (
        "NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp"
    )
    qwen.parent.mkdir(parents=True)
    qwen.write_text(
        'require(maxGeneratedTokens <= 32, "bound");\n'
        'const char* role = "/LLM/Stage/0";\n',
        encoding="utf-8",
    )
    user = tmp_path / "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py"
    user.parent.mkdir(parents=True)
    user.write_text(
        "for token_index in range(max_new_tokens):\n"
        "    result = client.distributed_inference(service, payload)\n",
        encoding="utf-8",
    )

    codes = {item["code"] for item in gate.check_source_contract(tmp_path)}

    assert {
        "MISSING_STREAMED_SYMBOL",
        "QWEN_MAX_TOKEN_BOUND_DRIFT",
        "LEGACY_QWEN_ROLE_NAME",
        "PER_TOKEN_DISTRIBUTED_INFERENCE",
        "QWEN_STREAMING_FULL_CONTEXT_OPTIONS",
        "AUTOMATIC_STREAM_EPOCH_PLAN_MISSING",
        "NATIVE_STREAM_EPOCH_RUNTIME_DISABLED",
    } <= codes


def test_minindn_selection_dataflow_preserves_onnx_runtime_identity() -> None:
    """Selection offers must describe the backend that will actually execute."""
    source = (REPO / "Experiments/NDNSF_DI_LlmPipeline_Minindn.py").read_text(
        encoding="utf-8")

    assert '"qwen-onnx", "qwen-onnx-cpu-native"' in source
    assert '"onnxruntime-cuda"' in source
    assert '"onnxruntime-cpu"' in source
    assert "requires a Qwen Transformers or ONNX runtime" in source
    # A Transformers backend must remain limited to the Transformers runtime;
    # otherwise a native ONNX campaign can publish a false capability offer.
    transformers_block = source[source.index('if args.runtime == "qwen-transformers"'):]
    assert 'backend = (\n            "transformers"' in transformers_block
    assert 'elif args.runtime in {"qwen-onnx", "qwen-onnx-cpu-native"}' in transformers_block


def test_requirement_inventory_expands_ranges_and_covers_spec175() -> None:
    gate = _load_gate()

    inventory = gate.validate_requirement_coverage(FEATURE)
    document_codes = {item["code"] for item in gate.check_documents(FEATURE)}

    assert gate._expand_ids("FR-001..FR-003, FR-030a", "FR") == {
        "FR-001", "FR-002", "FR-003", "FR-030a",
    }
    assert inventory["functionalRequirements"] == 73
    assert inventory["successCriteria"] == 16
    assert inventory["missingFromTasks"] == []
    assert inventory["missingFromTraceability"] == []
    assert "LEGACY_ROLE_IN_SPEC" not in document_codes
    assert "QWEN_TEXT_ONLY_BOUNDARY" not in document_codes
    assert "QWEN_SINGLE_TOKEN_DECODE_BOUNDARY" not in document_codes
    assert "QWEN_MTP_DISABLED_BOUNDARY" not in document_codes
    assert "QWEN_THINKING_DISABLED_BOUNDARY" not in document_codes


def test_expected_negative_cli_refuses_an_unexpected_pass(
    tmp_path: Path, monkeypatch,
) -> None:
    gate = _load_gate()
    output = tmp_path / "qualification-manifest-v1.json"
    monkeypatch.setattr(
        gate,
        "build_manifest",
        lambda *args, **kwargs: {"status": "PASS", "blockers": []},
    )

    return_code = gate.main([
        "--project-root", str(REPO),
        "--feature-dir", str(FEATURE),
        "--output", str(output),
        "--expected-negative",
    ])

    assert return_code == 3
    manifest = json.loads(output.read_text(encoding="utf-8"))
    assert manifest == {"status": "PASS", "blockers": []}


def test_dirty_scope_records_but_does_not_block_unrelated_research_paths() -> None:
    gate = _load_gate()
    assert gate._is_in_scope_status(
        " M ndn-service-framework/ServiceUser.cpp")
    assert gate._is_in_scope_status(
        "?? specs/175-ndnsf-di-streamed-invocation/evidence/new.md")
    assert gate._is_in_scope_status(
        "R  old.cpp -> tests/unit-tests/new.cpp")
    assert gate._is_in_scope_status(
        "?? tests/python/test_spec175_future_contract.py")
    assert not gate._is_in_scope_status(
        "?? tests/python/test_context_mode_guard.py")
    assert not gate._is_in_scope_status(
        " M docs/PAPER/named-data-network-service-framework-paper/NDNSF.pdf")
    assert not gate._is_in_scope_status(
        "?? specs/170-reusable-layer-artifacts/evidence/historical.md")
    assert not gate._is_source_subject_path(
        "packaging/ndnsf-di-container/docs/itiger-qwen-models.md")
    assert not gate._is_source_subject_path(
        "specs/175-ndnsf-di-streamed-invocation/tasks.md")


def test_content_bound_source_seal_admits_a_controlled_dirty_subject(
    tmp_path: Path,
) -> None:
    """A seal must bind the exact status and bytes without workspace assumptions."""
    gate = _load_gate()
    project = tmp_path / "project"
    subject = project / "scripts/spec175_contract_gate.py"
    subject.parent.mkdir(parents=True)
    subject.write_text("subject-v1\n", encoding="utf-8")
    seal = tmp_path / "source-seal.json"
    seal.write_text(json.dumps({
        "schemaVersion": "spec175-source-seal-v1",
        "sourceRevision": "controlled-revision",
        "dirtyFiles": {
            "scripts/spec175_contract_gate.py": {
                "status": "??",
                "sha256": gate._sha256(subject),
            },
        },
    }), encoding="utf-8")

    record, issues = gate._verify_source_seal(
        project,
        seal,
        "controlled-revision",
        ["?? scripts/spec175_contract_gate.py"],
    )

    assert issues == []
    assert record["verified"] is True
    assert record["dirtyFileCount"] == 1
