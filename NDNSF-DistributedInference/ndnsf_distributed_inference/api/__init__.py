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

try:
    from ndnsf import _ndnsf as _native
except ModuleNotFoundError as exc:
    raise ImportError(
        "ndnsf_distributed_inference.api requires the built ndnsf native extension"
    ) from exc

if not hasattr(_native, "Runtime"):
    raise ImportError(
        "installed ndnsf native extension does not provide the Spec185 Runtime API"
    )

if _native is not None and hasattr(_native, "Runtime"):
    # These are direct pybind views over the C++ owners.  The Python package
    # adds only naming, seconds-unit and asyncio adapters; it does not create
    # a second preparation/request state machine.
    CachePolicy = _native.CachePolicy
    PreparationStatus = _native.PreparationStatus
    PreparationOrigin = _native.PreparationOrigin
    RequestStatus = _native.RequestStatus
    ModelRegistration = _native.ModelRegistration
    RuntimeConfig = _native.RuntimeConfig
    PrepareOptions = _native.PrepareOptions
    ModelCapabilities = _native.ModelCapabilities
    ModelManifest = _native.ModelManifest
    PreparationReceipt = _native.PreparationReceipt
    GenerationOptions = _native.GenerationOptions
    StreamOptions = _native.StreamOptions
    RequestOptions = _native.RequestOptions
    DataRef = _native.DataRef
    Input = _native.Input
    Result = _native.Result
    Event = _native.Event
    RequestDiagnostics = _native.RequestDiagnostics
    ConversationCheckpoint = _native.ConversationCheckpoint
    ConversationOptions = _native.ConversationOptions
    Runtime = _native.Runtime
    User = _native.User
    PreparedModel = _native.PreparedModel
    PreparationHandle = _native.PreparationHandle
    RequestHandle = _native.RequestHandle
    Conversation = _native.Conversation
    EventReader = _native.EventReader
    Subscription = _native.Subscription
    DiError = _native.DiError
    NativeProviderConfig = _native.NativeProviderConfig
    ProviderConfig = _native.NativeProviderConfig
    ServiceDefinition = _native.ServiceDefinition
    ProviderCounters = _native.ProviderCounters
    ProviderRegistration = _native.ProviderRegistration
    Provider = _native.Provider
    from ._async import (NativeCallbackError, drain_result, events_async,
                         next_event, preparation_result, prepare,
                         request_result, runtime_enter, runtime_exit)

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

if _native is not None and hasattr(_native, "Runtime"):
    __all__ += [
        "CachePolicy", "PreparationStatus", "PreparationOrigin", "RequestStatus",
        "ModelRegistration", "RuntimeConfig", "PrepareOptions", "ModelCapabilities",
        "ModelManifest", "PreparationReceipt", "GenerationOptions", "StreamOptions",
        "RequestOptions", "DataRef", "Input", "Result", "Event", "RequestDiagnostics",
        "ConversationCheckpoint", "ConversationOptions", "Runtime", "User", "PreparedModel",
        "PreparationHandle", "RequestHandle", "Conversation", "EventReader", "Subscription",
        "DiError",
        "NativeProviderConfig", "ProviderConfig", "ServiceDefinition", "ProviderCounters",
        "ProviderRegistration", "Provider", "NativeCallbackError", "drain_result",
        "events_async", "next_event", "preparation_result", "prepare", "request_result",
        "runtime_enter", "runtime_exit",
    ]
