"""Minimal, optional-dependency-free NDNSF-DI Core public surface."""

from .contracts import (
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
    CoreExecutor, DependencyDrivenExecution, ExecutionAdapter, ExecutionResult, NativeSession,
    ReadySetCoordinator, ProviderActivationGate,
)
from .deployment_control import (
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
