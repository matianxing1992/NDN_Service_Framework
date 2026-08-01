"""Minimal, optional-dependency-free NDNSF-DI Core public surface."""

from .contracts import (
    DIRequestEnvelopeV2, DIRoleAssignmentV2, DISelectionAssignmentV2,
    DISelectionAcceptanceV2, ExactPrefixKvKeyV1, StateReuseBindingV2,
    ShardResidencyEvidenceV2,
    AssignmentContext, AuthenticatedProviderReceipt, CanonicalContract, CoreAssignment,
    CoreExecutionEvidence, CoreExecutionPlan, CorePlanDependency,
    DeploymentLifecycleRecord, OrphanCleanupRecord,
    DeploymentIntent, ProviderCapabilityOffer, DeploymentPlan,
    DeploymentInstance, DeploymentInstanceState,
    ProviderReadyMessage, ReadyAcknowledgement, ReadySetMember,
    ExecutionActivateMessage,
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
    ReadySetCoordinator, ProviderActivationGate,
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
