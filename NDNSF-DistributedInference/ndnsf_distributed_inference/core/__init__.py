"""Minimal, optional-dependency-free NDNSF-DI Core public surface."""

from .contracts import (
    DATA_DRIVEN_V2, LEGACY_READY_SET_V1, DI_PLACEMENT_V3, placement_wire_schema,
    DIDataDependencyV2, DIRequestEnvelopeV2, DIRoleAssignmentV2,
    DISelectionAssignmentV2,
    DISelectionAcceptanceV2, ExactPrefixKvKeyV1, StateReuseBindingV2,
    ShardResidencyEvidenceV2, ProviderResidencyIdentity,
    AssignmentContext, AuthenticatedProviderReceipt, CanonicalContract, CoreAssignment,
    CoreExecutionEvidence, CoreExecutionPlan, CorePlanDependency,
    DeploymentLifecycleRecord, OrphanCleanupRecord,
    DeploymentIntent, ProviderCapabilityOffer, DeploymentPlan,
    DeploymentInstance, DeploymentInstanceState,
    ProviderReadyMessage, ReadyAcknowledgement, ReadySetMember,
    ExecutionActivateMessage,
    InvocationSummaryV1, LifecycleEventV1,
    LIFECYCLE_EVENT_TYPES, TERMINAL_LIFECYCLE_EVENT_TYPES,
    validate_lifecycle_failure_code,
    ProviderAssignment, ReceiptOperation, RequestCoordinatorBinding,
    ResultRendezvousRecord, canonical_digest, canonical_json,
    exact_receipt_membership,
    EncryptedRequestInput, RecipientEncryptedAssignment, RequestCapabilities,
    ReservationLease, R1FieldContract, SelectionDecision,
    SelectionDecisionReceipt, SelectionDecisionTombstone,
    SelectionInputKeyGrant, SelectionInputKeyOffer, StageAbort,
    StageInputEvidence, validate_r1_capability_combination,
)
from .eligibility import (
    CandidateFacts, EligibilityDecision, EligibilityRequirements,
    eligible_candidates, evaluate_candidate, normalize_candidate,
)
from .execution import (
    CoreExecutor, DependencyDrivenExecution, DIResultEnvelopeV2,
    ExecutionAdapter, ExecutionResult, InputOutputObjectManifest,
    NativeSession, ResultContract, RoleExecutionBinding,
    ReadySetCoordinator, ProviderActivationGate, new_legacy_rollback_plan,
)
from .deployment_control import (
    DISelectionParticipant, GpuMiBAdmissionLedger,
    SelectionPreparationCallbacks, SelectionPreparationContext,
    ShardPreparationCallbacks, ShardPreparationPipeline,
    ModelShardRetentionCache, DerivedStateStore,
    AckWindowDecisionCoordinator, AtomicReservationBook, BoundedExactTargetRetry,
    DeploymentSideEffectCounters, ReservationDecisionAuthority,
    PreparationCallbacks, SelectionGatedProvider,
    DeploymentControlJournal, TentativeReservation,
    ReservationLedgerEvent,
)
from .v3_lifecycle import V3AdmissionController, V3FencingToken, V3LifecycleState, V3QueueRecord
from .device_scheduler import DeviceAdmissionV3, DeviceJobV3, MultiDeviceSchedulerV3
from .accelerator_policy import AcceleratorMode, AcceleratorPolicy
from .hybrid_contracts import (
    NDNSF_DATA_V1, DataSegmentReplayWindow, DataSegmentV1, HybridPlan, LocalTensorGroup,
    RedistributionEdge, TensorDisposition, TensorSlice,
)
from .protected_artifacts import (
    GrantRequestV1, KeyGrantV1, PlaintextLeaseRegistry, ProtectionState,
    RevocationStateV1,
)
from .secure_status import (
    BoundedStatusPoller, EncryptedStatusSnapshot, SecureStatusProvider,
    SecureStatusRequester, StatusEvent, StatusHandleBinding, StatusQuery,
    TERMINAL_STATES,
)
from .native import NativeBindingAdapter
from .ports import *
from .placement import create_assignment_context
from .recovery import (
    AdoptedInputEvidence, AttemptCompensationController,
    AttemptTransition, ControlDispatchResult,
    DICancelAttemptV2, DIReleaseOfferV2, DIStatusQueryV2,
    ContentionRetryController,
    BoundedRecoveryController, OrphanResourceRegistry, RecoveryAction,
    RecoveryAttempt, RecoveryReason, ResultRendezvousStore,
    replan_assignment_context,
)
from .runtime_contracts import (
    ExecutionEvidenceV1, KvCacheTelemetry, MeasuredTelemetrySnapshotV1,
    PlanFeasibilityDecisionV1, PlanFeasibilityRequirementsV1,
    PlanPredicateResultV1, ProviderCapabilityV3, ProviderProfileV1,
    REAL_RUNNER_KINDS, RunnerKind, RuntimeTelemetryV1,
    classify_execution_evidence, evaluate_plan_feasibility,
)
from .state import (
    BoundStateStore, ExecutionAttemptV1, StateBinding, TerminalReasonV1,
)
from .conflict_coordination import (
    CAPABILITY as CONFLICT_ADMISSION_CAPABILITY,
    AdmissionPermit, AuthorityEpoch, CanonicalResourceKey,
    ConflictAdmissionCoordinator, LedgerEvent, RequestAttempt,
    ResourceClaim, ResourceDeclaration, declarations_conflict,
    issue_permit_envelope, verify_permit_envelope,
)

__all__ = [name for name in globals() if not name.startswith("_")]
