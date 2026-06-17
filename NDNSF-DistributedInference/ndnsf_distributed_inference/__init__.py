"""High-level distributed inference APIs built on NDNSF Python Core."""

from .app import (
    APPClient,
    APPController,
    APPDeployment,
    APPProvider,
    InferencePlanBuilder,
    ModelPart,
)
from .client import DeploymentSession, DistributedInferenceClient, InferenceResult
from .controller import DistributedInferenceController
from .plan import (
    ArtifactSpec,
    DependencyGraph,
    DistributedInferencePlan,
    DependencyEdge,
    InferenceDependency,
    InferenceRole,
    ModelFamily,
    ModelFormat,
    PlannerDescriptor,
    PlannerKind,
    RoleDependencyView,
    RuntimeSpec,
    nxm_stage_frontier_dependencies,
    nxm_stage_roles,
    stage_shard_role,
)
from .provider import DistributedInferenceProvider, ProviderRuntimeContext
from .repo import (
    DistributedRepo,
    LocalDistributedRepo,
    NetworkDistributedRepoClient,
    PlacementPolicy,
    RepoObjectManifest,
    RepoRepairAction,
    RepoPlacement,
    RepoNodeApp,
    StorageCapability,
    large_data_reference_from_repo_manifest,
    repo_artifact_reference,
    repo_manifest_from_artifact_reference,
    repo_manifest_from_large_data_reference,
    select_replicas,
)
from .runtime_compatibility import (
    RUNTIME_COMPATIBILITY,
    default_runtime_backend,
    supported_runtime_backends,
    validate_runtime_compatibility,
)
from .artifact_deployment import (
    ArtifactProvisioningState,
    MaterializedArtifact,
    artifact_references_need_repo_client,
    load_artifact_references,
    materialize_role_artifacts,
    materialized_path,
    role_artifact_entries,
)
from .llm_stub_planner import (
    LLM_RUNTIME_COMPATIBILITY,
    default_llm_runtime_backend,
    llm_planner_registry,
    llm_planner_request,
    llm_splitter_output_from_result,
    llm_supported_runtime_backends,
    llm_stub_plan_from_request,
    validate_llm_runtime_compatibility,
)
from .llm_runtime import (
    ManagedLlamaServerRuntime,
    OpenAIChatRequest,
    OpenAICompatibleChatRuntime,
    call_openai_chat_runtime,
    decode_openai_chat_response,
    encode_openai_chat_request,
    materialize_llm_runtime_artifacts,
)
from .onnx_graph import (
    OnnxChunkSpec,
    OnnxGraphSummary,
    OnnxSplitCandidate,
    analyze_onnx_graph,
    build_chunk_dependencies,
    build_sequential_chunk_dependencies,
    estimate_split_candidates,
    write_onnx_graph_summary,
)
try:
    from .onnx_executor import (
        OnnxExecutionResult,
        decode_tensor_bundle,
        encode_tensor_bundle,
        execute_onnx_dependency_chunk,
        load_npz_payload,
        npz_payload,
        prefetch_dependency_inputs,
        role_topic_token,
        select_tensor_payload,
        verify_tensor_payload,
    )
except ImportError as exc:  # pragma: no cover - optional ONNX runtime path
    _onnx_executor_import_error = exc

    class OnnxExecutionResult:  # type: ignore[no-redef]
        pass

    def _missing_onnx_executor(*args, **kwargs):
        raise ImportError(
            "ONNX executor support requires optional ONNX runtime dependencies"
        ) from _onnx_executor_import_error

    decode_tensor_bundle = _missing_onnx_executor
    encode_tensor_bundle = _missing_onnx_executor
    execute_onnx_dependency_chunk = _missing_onnx_executor
    load_npz_payload = _missing_onnx_executor
    npz_payload = _missing_onnx_executor
    prefetch_dependency_inputs = _missing_onnx_executor
    role_topic_token = _missing_onnx_executor
    select_tensor_payload = _missing_onnx_executor
    verify_tensor_payload = _missing_onnx_executor
from .policy import (
    ArtifactSecurityPolicy,
    DistributedInferenceDeployment,
    SandboxPolicy,
    load_config,
    load_or_generate_deployment,
    native_execution_plan_spec,
    write_policy_bundle,
)
from .planner_registry import (
    PlannerBackend,
    PlannerBackendRegistry,
    PlannerRequest,
    PlannerResult,
    default_planner_registry,
)
from .split_planner import (
    ProviderProfile,
    SequentialSplitCandidate,
    SplitPlanRecommendation,
    SplitPlannerWeights,
    homogeneous_provider_profiles,
    recommend_sequential_splits,
)
from .splitter import SplitArtifact, SplitServiceSpec, SplitterOutput
try:
    from py_repoclient import RepoClient as GenericRepoClient
except ImportError:  # pragma: no cover - optional when repo binding is not installed
    GenericRepoClient = None

__all__ = [
    "ArtifactProvisioningState",
    "ArtifactSpec",
    "ArtifactSecurityPolicy",
    "APPClient",
    "APPController",
    "APPDeployment",
    "APPProvider",
    "DistributedInferenceClient",
    "DistributedInferenceController",
    "DistributedInferencePlan",
    "DistributedInferenceProvider",
    "DistributedRepo",
    "DistributedInferenceDeployment",
    "DeploymentSession",
    "DependencyGraph",
    "DependencyEdge",
    "InferenceDependency",
    "InferencePlanBuilder",
    "InferenceResult",
    "InferenceRole",
    "GenericRepoClient",
    "MaterializedArtifact",
    "ManagedLlamaServerRuntime",
    "ModelPart",
    "ModelFamily",
    "ModelFormat",
    "PlannerDescriptor",
    "PlannerBackend",
    "PlannerBackendRegistry",
    "PlannerKind",
    "PlannerRequest",
    "PlannerResult",
    "ProviderRuntimeContext",
    "LocalDistributedRepo",
    "OpenAIChatRequest",
    "OpenAICompatibleChatRuntime",
    "RUNTIME_COMPATIBILITY",
    "default_runtime_backend",
    "supported_runtime_backends",
    "validate_runtime_compatibility",
    "LLM_RUNTIME_COMPATIBILITY",
    "default_llm_runtime_backend",
    "llm_planner_registry",
    "llm_planner_request",
    "llm_splitter_output_from_result",
    "llm_supported_runtime_backends",
    "llm_stub_plan_from_request",
    "NetworkDistributedRepoClient",
    "OnnxChunkSpec",
    "OnnxGraphSummary",
    "OnnxExecutionResult",
    "OnnxSplitCandidate",
    "PlacementPolicy",
    "ProviderProfile",
    "RepoObjectManifest",
    "RepoRepairAction",
    "RepoNodeApp",
    "RepoPlacement",
    "large_data_reference_from_repo_manifest",
    "artifact_references_need_repo_client",
    "call_openai_chat_runtime",
    "decode_openai_chat_response",
    "encode_openai_chat_request",
    "load_artifact_references",
    "materialize_llm_runtime_artifacts",
    "materialize_role_artifacts",
    "materialized_path",
    "repo_artifact_reference",
    "repo_manifest_from_artifact_reference",
    "repo_manifest_from_large_data_reference",
    "role_artifact_entries",
    "RoleDependencyView",
    "RuntimeSpec",
    "SandboxPolicy",
    "SequentialSplitCandidate",
    "SplitArtifact",
    "SplitServiceSpec",
    "SplitPlanRecommendation",
    "SplitPlannerWeights",
    "SplitterOutput",
    "StorageCapability",
    "analyze_onnx_graph",
    "build_chunk_dependencies",
    "build_sequential_chunk_dependencies",
    "decode_tensor_bundle",
    "encode_tensor_bundle",
    "execute_onnx_dependency_chunk",
    "estimate_split_candidates",
    "default_planner_registry",
    "homogeneous_provider_profiles",
    "load_npz_payload",
    "load_config",
    "load_or_generate_deployment",
    "native_execution_plan_spec",
    "npz_payload",
    "nxm_stage_frontier_dependencies",
    "nxm_stage_roles",
    "prefetch_dependency_inputs",
    "recommend_sequential_splits",
    "role_topic_token",
    "select_replicas",
    "select_tensor_payload",
    "stage_shard_role",
    "validate_llm_runtime_compatibility",
    "verify_tensor_payload",
    "write_onnx_graph_summary",
    "write_policy_bundle",
]
