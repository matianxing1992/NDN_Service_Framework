"""Reviewed NDNSF-DI user API.

This namespace contains re-exports only.  Implementations remain in their
single APP role owners so importing the API cannot create an alternate runtime
or load optional model backends.
"""

from ..app_sdk.application import InferenceApplication
from ..app_sdk.client import InferenceClient, InferenceRequestHandle
from ..app_sdk.contracts import (
    ArtifactReference,
    DeploymentActivationRecord,
    DeploymentConstraints,
    DeploymentDefinition,
    DeploymentDefinitionRef,
    DeploymentHandleRef,
    DeploymentProgress,
    DeploymentRef,
    DeploymentStatus,
    DeploymentSummary,
    GenerationConfig,
    GenerationInput,
    InferenceOptions,
    ModelIntent,
    OptimizationObjective,
    ProviderDeploymentOffer,
    ProviderDeploymentOffers,
    RequestContract,
    RequestRef,
    RequestableDeployment,
)
from ..app_sdk.placement import ModelRef
from ..app_sdk.deployment import DeploymentHandle
from ..app_sdk.provider import InferenceProvider
from ..client import InferenceResult

__all__ = [
    "ArtifactReference",
    "DeploymentActivationRecord",
    "DeploymentConstraints",
    "DeploymentDefinition",
    "DeploymentDefinitionRef",
    "DeploymentHandle",
    "DeploymentHandleRef",
    "DeploymentProgress",
    "DeploymentRef",
    "DeploymentStatus",
    "DeploymentSummary",
    "GenerationConfig",
    "GenerationInput",
    "InferenceApplication",
    "InferenceClient",
    "InferenceOptions",
    "InferenceProvider",
    "InferenceRequestHandle",
    "InferenceResult",
    "ModelIntent",
    "ModelRef",
    "OptimizationObjective",
    "ProviderDeploymentOffer",
    "ProviderDeploymentOffers",
    "RequestContract",
    "RequestRef",
    "RequestableDeployment",
]
