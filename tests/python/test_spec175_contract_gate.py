from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
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
              !config.runnerPreparationFactory;
              !config.allowPreassembledV3Compatibility;
              DI_PROVIDER_ASSEMBLY_FACTORY_MISSING;
              coordinatorConfig.samplingMode;
              coordinatorConfig.samplingTemperature;
              coordinatorConfig.samplingTopK;
              coordinatorConfig.samplingTopP;
              coordinatorConfig.samplingRepetitionPenalty;
              coordinatorConfig.samplingSeed;
              requireGenerationTextOutput;
              generationTextDecoderFactory;
            """,
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.cpp": """
              "sampling_mode" "sampling_temperature" "sampling_top_k" "sampling_top_p"
              "sampling_repetition_penalty" "sampling_seed" "sampling_digest"
            """,
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp": """
              candidateText = textDelta = candidateText.substr requireTextOutput
              candidateText.substr(generatedText.size())
              textDelta
            """,
            "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp": """
              deviceStateBySession stateHandleBySession boundDeviceInputs
              streamingStateExecution stateDeviceToHostBytes
              complete CUDA allocation must not cross supportsOpaqueStateHandles
              stateHandleSnapshot PROVIDER_OPAQUE_STATE_HANDLE_MISSING
              PROVIDER_OPAQUE_STATE_HANDLE_IDENTITY_MISMATCH
            """,
            "NDNSF-DistributedInference/ndnsf_distributed_inference/conversation.py": """
              def pause_to_host( def prefetch_to_gpu( copy_state transfer_bytes
              ResidencyTier.HOST_RESIDENT ResidencyTier.GPU_RESIDENT
            """,
            "tests/integration-tests/ndnsf-di-core-flow.t.cpp": """
              ProductionIngressRunsNativePostSelectionAssignmentFetch
              ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentRequestResponse
              Spec175NativeTinyOnnxI01OneProvider
              Spec175NativeTinyOnnxI02TwoProviderEpochCoordinator
              Spec175NativeTinyOnnxI03FourProviderEpochCoordinator
            """,
            "tests/python/test_spec175_native_oracle.py": """
              def _run_native_case(test_name): ...
              Spec175NativeTinyOnnxI01OneProvider
              Spec175NativeTinyOnnxI03FourProviderEpochCoordinator
              Spec175NativeTinyOnnxI16SeededUnicodeAndSplitStop
              \"textDelta\":\"🙂\"
              \"finishReason\":\"stop_sequence\"
            """,
            "Experiments/NDNSF_DI_StreamedGeneration_Minindn.py": """
              "M01" "M02" "M03" "M04" "M05" "M06" "M07"
              "M08" "M09" "M10" "M11" "M12" "M13" "M14"
            """,
            "examples/DI_NativeProviderExecutable.cpp": """
              fields.find("executionPlanDigest");
              capability.planDigest != authenticatedPlanDigest;
              config.runnerPreparationFactory;
              config.allowPreassembledV3Compatibility = false;
              prepareNativeCanonicalOnnxRole;
              if (!options.artifactReferencesPath.empty());
              serving rejects preassembled --artifact-references; use canonical;
              serving rejects ready-made role artifact for;
            """,
        "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp": """
          canonicalSourceDataName;
          DI_CANONICAL_SOURCE_METADATA_MISSING;
          DI_CANONICAL_SOURCE_SIZE_MISMATCH;
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


def test_native_provider_serving_path_is_selection_gated_and_source_bound() -> None:
    """The formal serving path must not silently fall back to ready files."""
    source = (REPO / "examples/DI_NativeProviderExecutable.cpp").read_text(
        encoding="utf-8")
    install = source[source.index("auto installTask =") :]
    assert "waiting for authenticated post-Selection role assembly" in install
    assert "config.runnerPreparationFactory" in install
    assert "prepareNativeCanonicalOnnxRole" in install
    assert "config.allowPreassembledV3Compatibility = false" in install
    assert "serving rejects preassembled --artifact-references" in source
    assert "serving rejects ready-made role artifact for" in source
    # Startup may keep metadata-only role declarations, but may not invoke the
    # legacy repository materializer in the serving installation block.
    serving_block = install[:install.index("provider.fetchPermissionsFromController")]
    assert "materializeManifestSpecs(" not in serving_block

    assembler = (REPO /
        "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp"
    ).read_text(encoding="utf-8")
    assert "canonicalSourceDataName" in assembler
    assert "DI_CANONICAL_SOURCE_METADATA_MISSING" in assembler
    assert "DI_CANONICAL_SOURCE_SIZE_MISMATCH" in assembler


def test_spec175_production_path_mutations_fail_closed(tmp_path: Path) -> None:
    """T042 mutations must be caught before a broad test can look green."""
    gate = _load_gate()
    relative_paths = (
        "ndn-service-framework/InvocationStream.hpp",
        "ndn-service-framework/ServiceUser.hpp",
        "ndn-service-framework/ServiceProvider.hpp",
        "NDNSF-DistributedInference/cpp/adapters/qwen/QwenGenerationSession.cpp",
        "NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/qwen/placement.py",
        "tools/ndnsf-di/export_spec175_qwen36_stateful_onnx.py",
        "examples/python/NDNSF-DistributedInference/llm_pipeline/user.py",
        "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py",
        "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp",
        "examples/DI_NativeProviderExecutable.cpp",
        "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.cpp",
        "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.cpp",
        "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp",
        "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp",
        "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.cpp",
        "NDNSF-DistributedInference/ndnsf_distributed_inference/conversation.py",
        "NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py",
        "pythonWrapper/ndnsf/service.py",
        "tests/integration-tests/ndnsf-di-core-flow.t.cpp",
        "Experiments/NDNSF_DI_StreamedGeneration_Minindn.py",
        "tests/python/test_spec175_native_oracle.py",
    )

    def make_subject(name: str) -> Path:
        subject = tmp_path / name
        for relative in relative_paths:
            source = REPO / relative
            destination = subject / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        assert gate.check_source_contract(subject) == []
        return subject

    mutations = (
        (
            "compatibility",
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp",
            "!config.allowPreassembledV3Compatibility",
            "config.allowPreassembledV3Compatibility",
            "NATIVE_V3_COMPATIBILITY_GUARD_MISSING",
        ),
        (
            "launcher-preassembled-rejection",
            "examples/DI_NativeProviderExecutable.cpp",
            "serving rejects ready-made role artifact for",
            "serving accepts ready-made role artifact for",
            "NATIVE_FORMAL_PREASSEMBLED_INPUT_REJECTION_MISSING",
        ),
        (
            "sampling",
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.cpp",
            '"sampling_temperature"',
            '"sampling_temperature_removed"',
            "NATIVE_SAMPLING_AUTHORITY_MISSING",
        ),
        (
            "sampling-runtime",
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp",
            "coordinatorConfig.samplingTemperature",
            "coordinatorConfig.samplingTemperature_removed",
            "NATIVE_SAMPLING_AUTHORITY_MISSING",
        ),
        (
            "sampling-digest-only",
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.cpp",
            "coordinatorConfig.samplingMode",
            "coordinatorConfig.samplingMode_removed",
            "NATIVE_SAMPLING_AUTHORITY_MISSING",
        ),
        (
            "text",
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp",
            "textDelta = candidateText.substr",
            "textDelta = candidateText",
            "NATIVE_TEXT_DELTA_CONTRACT_MISSING",
        ),
        (
            "text-prefix",
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp",
            "candidateText.substr(generatedText.size())",
            "candidateText.substr(generatedText.capacity())",
            "NATIVE_TEXT_DELTA_SEMANTICS_MISSING",
        ),
        (
            "empty-delta",
            "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.cpp",
            "textDelta = candidateText.substr",
            "textDelta = \"\"",
            "NATIVE_TEXT_DELTA_CONTRACT_MISSING",
        ),
        (
            "device-state",
            "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.cpp",
            "complete CUDA allocation must not cross",
            "complete CUDA allocation crosses",
            "NATIVE_DEVICE_STATE_RESIDENCY_MISSING",
        ),
        (
            "conversation-tier",
            "NDNSF-DistributedInference/ndnsf_distributed_inference/conversation.py",
            "def pause_to_host(",
            "def paused_to_host(",
            "CONVERSATION_TIER_TRANSFER_MISSING",
        ),
        (
            "case-coverage",
            "tests/integration-tests/ndnsf-di-core-flow.t.cpp",
            "Spec175NativeTinyOnnxI03FourProviderEpochCoordinator",
            "Spec175NativeTinyTinyFourProviderCaseRemoved",
            "SPEC175_PRODUCTION_CASE_COVERAGE_MISSING",
        ),
    )
    for name, relative, old, new, code in mutations:
        subject = make_subject(name)
        path = subject / relative
        path.write_text(path.read_text(encoding="utf-8").replace(old, new),
                        encoding="utf-8")
        assert code in {item["code"] for item in gate.check_source_contract(subject)}


def test_design_code_convergence_is_a_hard_gate_before_formal_tests() -> None:
    """Formal tests must not be counted while design and production code diverge."""
    convergence = (REPO / ".specify/memory/design-code-convergence.md").read_text(
        encoding="utf-8")
    spec = (FEATURE / "spec.md").read_text(encoding="utf-8")
    tasks = (FEATURE / "tasks.md").read_text(encoding="utf-8")
    audit = (FEATURE / "audit.md").read_text(encoding="utf-8")

    assert "pre-qualification checklist" in convergence.lower()
    assert "formal validation is unlocked only by a fresh `pass`" in convergence.lower()
    assert "design-to-code convergence" in spec.lower()
    assert "T020" in tasks
    assert "corrected candidate" in audit.lower()
    assert "complete suites and minindn" in audit.lower()
    assert "separate downstream gates" in audit.lower()


def test_current_svs_abi_surface_is_used_by_data_v1_fetcher() -> None:
    """Require the Experimental SVS catch-up API used by the production path."""
    provider = (REPO / "ndn-service-framework/ServiceProvider.cpp").read_text(
        encoding="utf-8")
    user = (REPO / "ndn-service-framework/ServiceUser.cpp").read_text(
        encoding="utf-8")
    assert "m_svsps->subscribeToProducerWithCatchUp" in provider
    assert "getMappingFetchStats" not in provider
    assert "getPublicationFetchStats" not in provider
    assert "getPiggybackStats" not in provider
    assert "getMappingFetchStats" not in user
    assert "getPublicationFetchStats" not in user
    assert "getPiggybackStats" not in user
    assert "subscribeToProducerWithCatchUp(" in provider


def test_source_seal_scope_includes_host_runner_and_repository_probe() -> None:
    gate = _load_gate()
    assert gate._is_source_subject_path("Experiments/NDNSF_DI_LlmPipeline_Minindn.py")
    assert gate._is_source_subject_path("Experiments/spec175_repo_bootstrap.py")
    for path in (
            "scripts/analyze_spec175_conversation_residency.py",
            "scripts/analyze_spec175_performance.py",
            "scripts/build_spec175_functional_bundle.py",
            "scripts/build_spec175_workload.py",
            "scripts/collect_spec175_resources.py",
            "scripts/run_spec175_integration_gate.py",
            "scripts/run_spec175_python_gate.py",
            "scripts/seal_spec175_gate_prerequisite.py",
            "scripts/spec175_contract_gate.py",
            "scripts/spec175_g3_manifest.py",
            "scripts/spec175_source_seal.py"):
        assert gate._is_source_subject_path(path), path


def test_requirement_inventory_expands_ranges_and_covers_spec175() -> None:
    gate = _load_gate()

    inventory = gate.validate_requirement_coverage(FEATURE)
    document_codes = {item["code"] for item in gate.check_documents(FEATURE)}

    assert gate._expand_ids("FR-001..FR-003, FR-030a", "FR") == {
        "FR-001", "FR-002", "FR-003", "FR-030a",
    }
    assert inventory["functionalRequirements"] == 10
    assert inventory["successCriteria"] == 5
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


def test_source_seal_survives_only_descendant_document_commits(
    tmp_path: Path,
) -> None:
    gate = _load_gate()
    project = tmp_path / "project"
    source = project / "scripts/spec175_contract_gate.py"
    progress = project / "specs/175-ndnsf-di-streamed-invocation/tasks.md"
    source.parent.mkdir(parents=True)
    progress.parent.mkdir(parents=True)
    source.write_text("source-v1\n", encoding="utf-8")
    progress.write_text("- [ ] task\n", encoding="utf-8")
    subprocess.run(["git", "init", "-q"], cwd=project, check=True)
    subprocess.run(["git", "add", "."], cwd=project, check=True)
    subprocess.run([
        "git", "-c", "user.name=Spec175 Test",
        "-c", "user.email=spec175@example.invalid",
        "commit", "-q", "-m", "source",
    ], cwd=project, check=True)
    sealed_revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=project, text=True).strip()
    seal = project / "source-seal.json"
    seal.write_text(json.dumps({
        "schemaVersion": "spec175-source-seal-v1",
        "sourceRevision": sealed_revision,
        "dirtyFiles": {},
    }), encoding="utf-8")

    progress.write_text("- [x] task\n", encoding="utf-8")
    subprocess.run(["git", "add", str(progress)], cwd=project, check=True)
    subprocess.run([
        "git", "-c", "user.name=Spec175 Test",
        "-c", "user.email=spec175@example.invalid",
        "commit", "-q", "-m", "progress",
    ], cwd=project, check=True)
    document_revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=project, text=True).strip()

    record, issues = gate._verify_source_seal(
        project, seal, document_revision, [])
    assert issues == []
    assert record["revisionAdvancedWithoutSourceChange"] is True

    source.write_text("source-v2\n", encoding="utf-8")
    subprocess.run(["git", "add", str(source)], cwd=project, check=True)
    subprocess.run([
        "git", "-c", "user.name=Spec175 Test",
        "-c", "user.email=spec175@example.invalid",
        "commit", "-q", "-m", "source change",
    ], cwd=project, check=True)
    source_revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=project, text=True).strip()

    _, issues = gate._verify_source_seal(project, seal, source_revision, [])
    assert {item["code"] for item in issues} == {
        "SOURCE_SEAL_REVISION_SOURCE_CHANGE"
    }
