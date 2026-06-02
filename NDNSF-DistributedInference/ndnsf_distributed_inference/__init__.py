"""High-level distributed inference APIs built on NDNSF Python Core."""

from .app import (
    APPClient,
    APPController,
    APPDeployment,
    APPProvider,
    InferencePlanBuilder,
    ModelPart,
)
from .client import DistributedInferenceClient, InferenceResult
from .controller import DistributedInferenceController
from .plan import (
    ArtifactSpec,
    DependencyGraph,
    DistributedInferencePlan,
    DependencyEdge,
    InferenceDependency,
    InferenceRole,
    RoleDependencyView,
    RuntimeSpec,
)
from .provider import DistributedInferenceProvider, ProviderRuntimeContext
from .repo import (
    DistributedRepo,
    LocalDistributedRepo,
    NetworkDistributedRepoClient,
    PlacementPolicy,
    RepoObjectManifest,
    RepoPlacement,
    RepoNodeApp,
    StorageCapability,
    select_replicas,
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
from .policy import (
    ArtifactSecurityPolicy,
    DistributedInferenceDeployment,
    SandboxPolicy,
    load_config,
    load_or_generate_deployment,
    write_policy_bundle,
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
    "DependencyGraph",
    "DependencyEdge",
    "InferenceDependency",
    "InferencePlanBuilder",
    "InferenceResult",
    "InferenceRole",
    "GenericRepoClient",
    "ModelPart",
    "ProviderRuntimeContext",
    "LocalDistributedRepo",
    "NetworkDistributedRepoClient",
    "OnnxChunkSpec",
    "OnnxGraphSummary",
    "OnnxSplitCandidate",
    "PlacementPolicy",
    "ProviderProfile",
    "RepoObjectManifest",
    "RepoNodeApp",
    "RepoPlacement",
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
    "estimate_split_candidates",
    "homogeneous_provider_profiles",
    "load_config",
    "load_or_generate_deployment",
    "recommend_sequential_splits",
    "select_replicas",
    "write_onnx_graph_summary",
    "write_policy_bundle",
]
