"""Canonical model/task-first automatic collaboration planning surface."""

from __future__ import annotations

import base64
from dataclasses import dataclass, field, replace
import hashlib
import json
import math
import re
import secrets
import threading
import time
import uuid
from types import MappingProxyType
from typing import Any, Callable, Mapping, Protocol, Tuple
from urllib.parse import quote

from ndnsf import CollaborationDependency, CollaborationRole

from ..adapters import ApplicationInput, InputTransportMode, ModelFamilyAdapter
from .contracts import (
    GenerationConfig,
    GenerationInput,
    PreSplitCatalogSnapshot,
)
from ..core.ports import CandidateBudget
from ..core.contracts import (
    DATA_DRIVEN_V2, DIDataDependencyV2, DIRequestEnvelopeV2, DIRoleAssignmentV2,
    DISelectionAssignmentV2, GenerationRecoveryV1,
)
from ..core.group_capability import (
    GroupMemberV1, GroupOperationV1, seal_group_capability_v1,
)
from ..plan import SealedCollaborationPlan
from ..sdk.placement import (
    ArtifactPreparationMode,
    DIProviderOfferV2,
    DI_PLACEMENT_V3,
    UNBOUND_GRAPH_DIGEST_V3,
    DeviceBinding,
    DeviceBindingMode,
    ExecutionRole,
    ExecutionDisposition,
    GenerationExecutionContractV1,
    GrantBindingV1,
    ModelPlacementStrategy,
    PlacementDecision,
    PlacementProposalV3,
    PlacementRequest,
    PlanSealerV3,
    ProviderAssignment,
    ProviderGrantViewV1,
    ProviderOfferV3,
    ProviderPlanningViewV3,
    ProviderPlanningView,
    ReadinessMode,
    ReadinessPredicate,
    RoleAssemblySpec,
    RoleDataflowContract,
    TensorEndpoint,
    TensorEndpointSource,
    canonical_digest,
    build_provider_planning_view,
    evaluate_placement_strategy,
    is_cpu_backend,
    validate_role_dataflow_contracts,
)
from ..splitter import SplitCandidate, SplitSource, canonical_contract_digest
from ..conversation import (
    DEFAULT_RETENTION_MS,
    ConversationCheckpointV1,
    ConversationContinuation,
    ConversationInputMode,
    ProviderConversationStateReceiptV1,
    ConversationStateReferenceV1,
    ConversationTurnBindingV1,
)


def _require_digest(value: str, name: str) -> None:
    if not isinstance(value, str) or len(value) != 71 or not value.startswith(
            "sha256:"):
        raise ValueError(f"{name} must be a canonical sha256 digest")
    try:
        int(value[7:], 16)
    except ValueError as exc:
        raise ValueError(f"{name} must be a canonical sha256 digest") from exc


def normalize_request_id_component(request_id: str) -> str:
    """Return one canonical V2 request-ID NameComponent URI.

    NDNSF V2 appends exactly one request-ID component after a variable-length
    service name. Human-readable IDs may contain ``/`` separators, so encode
    those separators inside the component instead of silently extending the
    service name on the wire.
    """

    value = str(request_id or "").strip()
    if not value:
        raise ValueError("request_id must not be empty")
    body = value[1:] if value.startswith("/") else value
    if not body:
        raise ValueError("request_id must not be the root name")
    component = quote(body, safe="-._~%")
    if not component or "/" in component:
        raise ValueError("request_id must encode to one NameComponent")
    return "/" + component


def conversation_state_references_for_placement(
    conversation: ConversationContinuation | None,
    *,
    service_name: str,
    providers_by_role: Mapping[str, str],
    execution_roles: Mapping[str, ExecutionRole],
    now_ms: int,
) -> Mapping[str, ConversationStateReferenceV1]:
    """Derive one role-local state commitment from an aggregate checkpoint.

    The input is the finalized one-to-one role map. A changed Provider/role
    map fails instead of silently migrating retained state.
    """

    if (conversation is None
            or conversation.mode is ConversationInputMode.FULL_CONTEXT):
        return MappingProxyType({})
    checkpoint = ConversationCheckpointV1.from_bytes(
        conversation.parent_checkpoint or b"")
    role_map_digest = canonical_digest(tuple(sorted(
        (str(role), str(provider))
        for role, provider in providers_by_role.items())))
    roles = set(str(role) for role in execution_roles)
    if (checkpoint.conversation_id != conversation.conversation_id
            or checkpoint.context_epoch
            != conversation.expected_parent_context_epoch
            or checkpoint.service_name != service_name
            or checkpoint.expires_at_ms <= int(now_ms)
            or checkpoint.plan_role_map_digest != role_map_digest
            or set(checkpoint.role_receipt_digests) != roles
            or set(providers_by_role) != roles):
        raise ValueError(
            "conversation parent checkpoint does not match the exact "
            "Provider-role placement")
    return MappingProxyType({
        role: ConversationStateReferenceV1(
            conversation_id=checkpoint.conversation_id,
            context_epoch=checkpoint.context_epoch,
            service_name=checkpoint.service_name,
            plan_role_map_digest=checkpoint.plan_role_map_digest,
            checkpoint_digest=checkpoint.checkpoint_digest,
            role_name=role,
            role_receipt_digest=checkpoint.role_receipt_digests[role],
            expires_at_ms=checkpoint.expires_at_ms,
        )
        for role in sorted(roles)
    })


def conversation_turn_binding_for_placement(
    conversation: ConversationContinuation | None,
    *,
    service_name: str,
    providers_by_role: Mapping[str, str],
    execution_roles: Mapping[str, ExecutionRole],
    request_contract_digest: str,
    now_ms: int,
) -> ConversationTurnBindingV1 | None:
    """Seal the successor epoch target for every Provider projection."""

    if conversation is None:
        return None
    _require_digest(request_contract_digest, "request_contract_digest")
    roles = set(str(role) for role in execution_roles)
    if set(providers_by_role) != roles:
        raise ValueError("conversation turn role map is incomplete")
    role_map_digest = canonical_digest(tuple(sorted(
        (str(role), str(provider))
        for role, provider in providers_by_role.items())))
    parent_epoch = 0
    parent_checkpoint_digest = ""
    if conversation.mode is ConversationInputMode.APPEND_DELTA:
        checkpoint = ConversationCheckpointV1.from_bytes(
            conversation.parent_checkpoint or b"")
        if (checkpoint.conversation_id != conversation.conversation_id
                or checkpoint.context_epoch
                != conversation.expected_parent_context_epoch
                or checkpoint.service_name != service_name
                or checkpoint.expires_at_ms <= int(now_ms)
                or checkpoint.plan_role_map_digest != role_map_digest
                or set(checkpoint.role_receipt_digests) != roles):
            raise ValueError(
                "conversation turn does not match the retained placement")
        parent_epoch = checkpoint.context_epoch
        parent_checkpoint_digest = checkpoint.checkpoint_digest
    return ConversationTurnBindingV1(
        conversation_id=conversation.conversation_id,
        parent_context_epoch=parent_epoch,
        successor_context_epoch=parent_epoch + 1,
        service_name=service_name,
        plan_role_map_digest=role_map_digest,
        request_contract_digest=request_contract_digest,
        retention_deadline_ms=int(now_ms) + DEFAULT_RETENTION_MS,
        parent_checkpoint_digest=parent_checkpoint_digest,
    )


_DATA_V1_MAX_BYTES = 64 << 20
_DATA_V1_MAX_SEGMENTS = 1 << 20


def _native_group_epoch_key_wrapper(
    recipient_public_key: bytes,
    epoch_key: bytes,
) -> bytes:
    """Wrap one epoch key with the Core RSA-OAEP implementation."""

    try:
        from ndnsf import _ndnsf
        wrapper = _ndnsf.wrap_selection_gated_input_key
    except (ImportError, AttributeError) as exc:
        raise RuntimeError(
            "NDNSF native binding lacks wrap_selection_gated_input_key") from exc
    return bytes(wrapper(bytes(epoch_key), bytes(recipient_public_key)))


@dataclass(frozen=True, init=False)
class ModelRef:
    model_name: str
    content_digest: str
    semantics_digest: str
    source_revision: str | None = None

    def __init__(
        self,
        model_name: str | None = None,
        content_digest: str = "",
        semantics_digest: str = "",
        source_revision: str | None = None,
        *,
        name: str | None = None,
        revision: str | None = None,
        tokenizer_digest: str | None = None,
    ) -> None:
        resolved_name = str(name or model_name or "")
        resolved_revision = revision if revision is not None else source_revision
        resolved_semantics = str(tokenizer_digest or semantics_digest or "")
        if name is not None and model_name is not None and name != model_name:
            raise ValueError("model name aliases disagree")
        if (revision is not None and source_revision is not None
                and revision != source_revision):
            raise ValueError("model revision aliases disagree")
        if (tokenizer_digest is not None and semantics_digest
                and tokenizer_digest != semantics_digest):
            raise ValueError("tokenizer/semantics digest aliases disagree")
        object.__setattr__(self, "model_name", resolved_name)
        object.__setattr__(self, "content_digest", str(content_digest))
        object.__setattr__(self, "semantics_digest", resolved_semantics)
        object.__setattr__(self, "source_revision", resolved_revision)
        self.__post_init__()

    def __post_init__(self) -> None:
        if not self.model_name:
            raise ValueError("model_name is required")
        _require_digest(self.content_digest, "content_digest")
        _require_digest(self.semantics_digest, "semantics_digest")

    @property
    def intent_digest(self) -> str:
        return canonical_digest(self)

    @property
    def name(self) -> str:
        return self.model_name

    @property
    def revision(self) -> str | None:
        return self.source_revision

    @property
    def tokenizer_digest(self) -> str:
        return self.semantics_digest


@dataclass(frozen=True)
class InferenceTaskRef:
    task_name: str
    adapter_name: str
    adapter_descriptor_digest: str
    adapter_composition_digest: str
    task_descriptor_digest: str

    def __post_init__(self) -> None:
        if not self.task_name or not self.adapter_name:
            raise ValueError("inference task reference is incomplete")
        for name in (
                "adapter_descriptor_digest", "adapter_composition_digest",
                "task_descriptor_digest"):
            _require_digest(getattr(self, name), name)

    @classmethod
    def from_adapter(cls, adapter: ModelFamilyAdapter) -> "InferenceTaskRef":
        return cls(
            task_name=adapter.task.descriptor.task_name,
            adapter_name=adapter.descriptor.name,
            adapter_descriptor_digest=adapter.descriptor.descriptor_digest,
            adapter_composition_digest=adapter.composition_digest,
            task_descriptor_digest=canonical_contract_digest(
                adapter.task.descriptor),
        )


@dataclass(frozen=True)
class TaskOptions:
    schema_digest: str
    payload: bytes

    def __post_init__(self) -> None:
        _require_digest(self.schema_digest, "task options schema_digest")
        object.__setattr__(self, "payload", bytes(self.payload))


@dataclass(frozen=True)
class GenerationRequest:
    """One complete model-generation invocation.

    ``input`` contains the complete prompt/prefill payload.  The request is
    submitted once; autoregressive decode is adapter/provider data-plane
    work, and the normative wire result is one complete response.  It must
    not call the planner again for individual output tokens.
    """

    model: ModelRef
    task: InferenceTaskRef
    input: ApplicationInput
    timeout_ms: int
    options: TaskOptions | None = None
    objective: Any = None
    constraints: Mapping[str, Any] = field(default_factory=dict)
    request_id: str = ""
    output_mode: str = "FULL"

    def __post_init__(self) -> None:
        if self.timeout_ms <= 0 or self.output_mode != "FULL":
            raise ValueError("generation requires a positive FULL-output timeout")
        object.__setattr__(self, "constraints", MappingProxyType(
            {str(key): value for key, value in self.constraints.items()}))


# Request-scoped control traffic for an application-owned autoregressive loop.
# It is deliberately not a model dependency edge: the static model graph stays
# acyclic, while the provider handlers may exchange bounded generation control
# records over this separately authorized scope.
GENERATION_CONTROL_SCOPE = "generation-control-v1"
CONVERSATION_STATE_SCOPE = "ndnsf-di-conversation-state-v1"


@dataclass(frozen=True)
class AutomaticInferenceHandle:
    collaboration: Any
    decision: PlacementDecision
    sealed_plan: SealedCollaborationPlan
    adapter: ModelFamilyAdapter
    planning_timings_ms: Mapping[str, float] = None
    invocation_id: str = ""
    # Internal owner wiring for conversation continuation.  These fields are
    # not part of the public handle contract; they let the requester perform
    # the aggregate receipt/CAS transaction after Providers return signed
    # receipts without exposing provider-local state or device handles.
    service_user: Any = None
    conversation_metadata: Mapping[str, Any] | None = None

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "planning_timings_ms",
            MappingProxyType(dict(self.planning_timings_ms or {})),
        )
        object.__setattr__(
            self,
            "conversation_metadata",
            MappingProxyType(dict(self.conversation_metadata or {})),
        )

    def response(self, timeout_ms: int | None = None):
        return self.collaboration.result(timeout_ms)

    def result(self, timeout_ms: int | None = None) -> Any:
        response = self.response(timeout_ms)
        if not response.status:
            raise RuntimeError(response.error or "distributed inference failed")
        return self.adapter.task.decode_result(response.payload)


class AutomaticStreamingHandle:
    """One public generation handle across one or two fenced NDNSF attempts."""

    def __init__(self, base: AutomaticInferenceHandle | None,
                 stream_options: Any, *, logical_request_id: str = "",
                 generation_id: str = "",
                 deadline_ms: int = 0,
                 on_event: Callable[[bytes], None] | None = None,
                 on_complete: Callable[[bytes], None] | None = None,
                 on_error: Callable[[Mapping[str, Any]], None] | None = None,
                 conversation_expected: bool = False) -> None:
        self.stream_options = stream_options
        self._logical_request_id = str(
            logical_request_id or (
                getattr(getattr(base, "collaboration", None), "request_id", "")))
        self._generation_id = str(generation_id)
        self._deadline_ms = int(deadline_ms)
        self._on_event = on_event
        self._on_complete = on_complete
        self._on_error = on_error
        self._condition = threading.Condition(threading.RLock())
        self._attempts: dict[int, AutomaticInferenceHandle] = {}
        self._attempt_request_ids: dict[int, str] = {}
        self._current_attempt = 1
        self._replacement_count = 0
        self._replacement_started = False
        self._stale_event_count = 0
        self._lineage_reject_count = 0
        self._gap_reject_count = 0
        self._duplicate_reject_count = 0
        self._callback_error_count = 0
        self._events: list[bytes] = []
        self._token_ids: list[int] = []
        # Monotonic callback timestamps are intentionally kept separate from
        # token/event payloads.  They are used only for metadata-only TTFT/ITL
        # evidence and never leave this process as prompt, token, or state
        # contents.
        self._started_timestamp_us = time.monotonic_ns() // 1000
        self._event_timestamps_us: list[int] = []
        self._complete_timestamp_us: int | None = None
        self._error_timestamp_us: int | None = None
        self._complete_payload: bytes | None = None
        self._pending_complete_payload: bytes | None = None
        self._error: dict[str, Any] | None = None
        self._terminal = False
        # A failed public handle is terminal before the native stream has
        # necessarily been cancelled.  Keep a separate idempotence bit so
        # cleanup can still post one native cancellation after _fail().
        self._native_cancel_requested = False
        self._conversation_coordinator = None
        self._conversation_turn: Mapping[str, Any] | None = None
        self._conversation_expected = bool(conversation_expected)
        self._conversation_promotion_started = False
        if base is not None:
            self._bind_attempt(1, base)

    @property
    def base(self) -> AutomaticInferenceHandle:
        with self._condition:
            base = self._attempts.get(self._current_attempt)
            if base is None:
                base = self._attempts.get(1)
            if base is None:
                raise RuntimeError("streaming attempt is not bound")
            return base

    @property
    def collaboration(self):
        return self.base.collaboration

    @property
    def request_id(self) -> str:
        return self._logical_request_id

    @property
    def generation_id(self) -> str:
        return self._generation_id

    @property
    def attempt_request_ids(self) -> Mapping[int, str]:
        with self._condition:
            return MappingProxyType(dict(self._attempt_request_ids))

    @property
    def replacement_count(self) -> int:
        with self._condition:
            return self._replacement_count

    @property
    def stream_events(self) -> tuple[bytes, ...]:
        with self._condition:
            return tuple(self._events)

    @property
    def stream_complete(self) -> bytes | None:
        with self._condition:
            return self._complete_payload

    @property
    def timing_summary(self) -> Mapping[str, object]:
        """Return metadata-only TTFT/ITL timing for this stream.

        Timestamps come from the actual verified event/completion callbacks,
        using one monotonic clock.  The summary deliberately contains no token
        IDs, prompt text, response bytes, logits, or decode-state material.
        Missing first/terminal callbacks remain ``None`` instead of being
        synthesized, so an incomplete trace cannot look like a valid span.
        """
        with self._condition:
            timestamps = tuple(self._event_timestamps_us)
            started = int(self._started_timestamp_us)
            completed = self._complete_timestamp_us
            failed = self._error_timestamp_us
            attempts = len(self._attempts)
            replacements = int(self._replacement_count)
            stale_events = int(self._stale_event_count)
            lineage_rejects = int(self._lineage_reject_count)
            gap_rejects = int(self._gap_reject_count)
            duplicate_rejects = int(self._duplicate_reject_count)
            callback_errors = int(self._callback_error_count)
        inter_token_ms = tuple(
            round((right - left) / 1000.0, 3)
            for left, right in zip(timestamps, timestamps[1:])
        )
        return MappingProxyType({
            "startedTimestampUs": started,
            "firstEventTimestampUs": (
                timestamps[0] if timestamps else None),
            "completeTimestampUs": completed,
            "errorTimestampUs": failed,
            "eventCount": len(timestamps),
            "ttftMs": (
                round((timestamps[0] - started) / 1000.0, 3)
                if timestamps else None),
            "interTokenMs": inter_token_ms,
            "totalMs": (
                round(((completed if completed is not None else failed) - started)
                      / 1000.0, 3)
                if (completed is not None or failed is not None) else None),
            "attemptCount": attempts,
            "replacementCount": replacements,
            "staleEventCount": stale_events,
            "lineageRejectCount": lineage_rejects,
            "gapRejectCount": gap_rejects,
            "duplicateRejectCount": duplicate_rejects,
            "callbackErrorCount": callback_errors,
        })

    @property
    def conversation_turn(self) -> Mapping[str, Any] | None:
        """Return the pending turn identity, without exposing cache state."""
        with self._condition:
            if self._conversation_turn is None:
                return None
            return MappingProxyType(dict(self._conversation_turn))

    def _attach_conversation(
        self, coordinator: Any, turn: Mapping[str, Any],
    ) -> None:
        """Bind the user-side transaction created before Request publication."""
        if coordinator is None or not isinstance(turn, Mapping):
            raise TypeError("conversation owner binding is invalid")
        with self._condition:
            if self._conversation_turn is not None:
                raise RuntimeError("conversation owner is already attached")
            self._conversation_coordinator = coordinator
            self._conversation_turn = dict(turn)
            if not self._conversation_promotion_started:
                self._conversation_promotion_started = True
                threading.Thread(
                    target=self._promote_conversation,
                    name="ndnsf-di-conversation-promotion",
                    daemon=True,
                ).start()

    def _promote_conversation(self) -> None:
        """Seal one aggregate conversation turn after all Provider receipts.

        Provider receipts are admitted by the native User only after signature,
        request/name binding, scope-key lookup, and AEAD verification.  The
        Python layer validates the role set and common digests, commits the
        owner checkpoint, then sends one authenticated COMMIT control to each
        staged Provider.  A missing/invalid receipt never becomes a usable
        successor state.
        """
        committed = False
        cleanup_user = None
        cleanup_request_id = ""
        try:
            with self._condition:
                coordinator = self._conversation_coordinator
                turn = dict(self._conversation_turn or {})
                base = self._attempts.get(self._current_attempt)
            if coordinator is None or not turn or base is None:
                return
            service_user = getattr(base, "service_user", None)
            metadata = dict(getattr(base, "conversation_metadata", {}) or {})
            if service_user is None:
                raise RuntimeError("conversation service user is unavailable")
            request_id = str(turn.get("requestId", ""))
            cleanup_user = service_user
            cleanup_request_id = request_id
            conversation_id = str(turn.get("conversationId", ""))
            roles = tuple(sorted(str(item.role) for item in base.sealed_plan.roles))
            providers = {
                str(role): str(provider)
                for role, provider in base.sealed_plan.providers_by_role.items()
            }
            if (not request_id or not conversation_id or not roles
                    or set(providers) != set(roles)
                    or len(set(providers.values())) != len(providers)):
                raise RuntimeError("conversation role/provider map is incomplete")
            plan_digest = str(metadata.get("plan_role_map_digest", ""))
            exact_plan_digest = str(metadata.get("plan_digest", ""))
            model_digest = str(metadata.get("model_contract_digest", ""))
            tokenizer_digest = str(metadata.get("tokenizer_digest", ""))
            template_digest = str(metadata.get("chat_template_digest", ""))
            service_name = str(metadata.get("service_name", ""))
            for name, value in (
                    ("plan_digest", exact_plan_digest),
                    ("plan_role_map_digest", plan_digest),
                    ("model_contract_digest", model_digest),
                    ("tokenizer_digest", tokenizer_digest),
                    ("chat_template_digest", template_digest)):
                _require_digest(value, name)
            if not service_name.startswith("/"):
                raise RuntimeError("conversation service name is unavailable")

            def promotion_timeout_ms(cap_ms: int = 30_000) -> int:
                """Bound every receipt/control wait by the signed request deadline."""
                cap = max(1, int(cap_ms))
                if self._deadline_ms <= 0:
                    return cap
                remaining = self._deadline_ms - int(time.time() * 1000)
                if remaining <= 0:
                    raise TimeoutError(
                        "conversation promotion deadline expired")
                return max(1, min(cap, remaining))

            receipt_records = service_user.wait_for_verified_collaboration_data(
                request_id,
                key_scope=CONVERSATION_STATE_SCOPE,
                topic_prefix="/ndnsf-di/conversation/receipt",
                min_count=len(roles),
                timeout_ms=promotion_timeout_ms(),
                consume=True,
            )
            receipts: list[ProviderConversationStateReceiptV1] = []
            seen_roles: set[str] = set()
            requester_identity = str(
                getattr(service_user, "user", "") or
                getattr(coordinator, "requester_identity", ""))
            now_ms = int(time.time() * 1000)
            parent = turn.get("parentCheckpoint")
            parent_epoch = int(parent.context_epoch) if parent is not None else 0
            for record in receipt_records:
                if str(record.request_id) != request_id:
                    raise RuntimeError("conversation receipt request mismatch")
                receipt = ProviderConversationStateReceiptV1.from_dict(
                    json.loads(bytes(record.payload).decode("utf-8")))
                role = receipt.role_name
                if (role in seen_roles or role not in roles
                        or providers.get(role) != receipt.provider_identity
                        or str(record.producer) != receipt.provider_identity
                        or str(record.producer_role) != role
                        or receipt.conversation_id != conversation_id
                        or receipt.parent_context_epoch != parent_epoch
                        or receipt.successor_context_epoch != parent_epoch + 1
                        or receipt.service_name != service_name
                        or receipt.plan_role_map_digest != plan_digest
                        or receipt.origin_request_id != request_id
                        or (str(turn.get("generationId", ""))
                            and receipt.origin_generation_id
                            != str(turn.get("generationId", "")))
                        or (requester_identity and
                            receipt.requester_identity != requester_identity)
                        or receipt.expires_at_ms <= now_ms):
                    raise RuntimeError("conversation receipt identity mismatch")
                seen_roles.add(role)
                receipts.append(receipt)
            if seen_roles != set(roles):
                raise RuntimeError("conversation receipt set is incomplete")

            # A resumed turn may execute only after every selected Provider
            # has resolved its role-local parent state.  The readiness record
            # is signed/encrypted collaboration Data; it contains commitments
            # and residency metadata, never KV/recurrent/convolution bytes.
            # The initial FULL_CONTEXT turn has no parent state and therefore
            # does not wait for this barrier.
            if parent is not None:
                ready_records = service_user.wait_for_verified_collaboration_data(
                    request_id,
                    key_scope=CONVERSATION_STATE_SCOPE,
                    topic_prefix="/ndnsf-di/conversation/ready",
                    min_count=len(roles),
                    timeout_ms=promotion_timeout_ms(),
                    consume=True,
                )
                ready_keys = {
                    "schema", "requestId", "attemptEpoch", "generationId",
                    "planDigest", "conversationId", "contextEpoch",
                    "serviceName", "planRoleMapDigest", "checkpointDigest",
                    "roleName", "roleReceiptDigest", "providerIdentity",
                    "providerBootId", "cacheEpoch", "ready", "reason",
                    "residency",
                }
                seen_ready: set[str] = set()
                for record in ready_records:
                    try:
                        ready = json.loads(bytes(record.payload).decode("utf-8"))
                    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                        raise RuntimeError(
                            "conversation state-ready record is malformed") from exc
                    if not isinstance(ready, dict) or set(ready) != ready_keys:
                        raise RuntimeError(
                            "conversation state-ready field set mismatch")
                    role = str(ready.get("roleName", ""))
                    provider = str(ready.get("providerIdentity", ""))
                    expected_generation = str(turn.get("generationId", ""))
                    expected_attempt = int(turn.get("attemptEpoch", 1))
                    expected_plan = exact_plan_digest
                    expected_parent_epoch = int(parent.context_epoch)
                    if (role in seen_ready or role not in roles
                            or providers.get(role) != provider
                            or str(record.producer) != provider
                            or str(record.producer_role) != role
                            or str(ready["requestId"]) != request_id
                            or int(ready["attemptEpoch"]) != expected_attempt
                            or str(ready["generationId"]) != expected_generation
                            or str(ready["planDigest"]) != expected_plan
                            or str(ready["conversationId"]) != conversation_id
                            or int(ready["contextEpoch"]) != expected_parent_epoch
                            or str(ready["serviceName"]) != service_name
                            or str(ready["planRoleMapDigest"]) != plan_digest
                            or str(ready["checkpointDigest"])
                            != parent.checkpoint_digest
                            or str(ready["roleReceiptDigest"])
                            != str(parent.role_receipt_digests.get(role, ""))
                            or str(ready["providerBootId"])
                            != next(item.provider_boot_id for item in receipts
                                    if item.role_name == role)
                            or int(ready["cacheEpoch"]) != next(
                                item.cache_epoch for item in receipts
                                if item.role_name == role)
                            or ready["ready"] is not True
                            or str(ready["reason"]) != "state-hit"
                            or str(ready["residency"]) != "GPU_RESIDENT"):
                        raise RuntimeError("conversation state-ready identity mismatch")
                    seen_ready.add(role)
                if seen_ready != set(roles):
                    raise RuntimeError("conversation state-ready set is incomplete")

            with self._condition:
                # Conversation promotion is part of the same request
                # transaction.  A fixed 30-second wait could outlive the
                # caller's authenticated deadline and expose a successor
                # checkpoint after the request had already expired.
                remaining_ms = promotion_timeout_ms()
                deadline = time.monotonic() + max(0, remaining_ms) / 1000.0
                while self._pending_complete_payload is None and not self._terminal:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    self._condition.wait(remaining)
                payload = self._pending_complete_payload
            if payload is None:
                raise RuntimeError("conversation final response was not received")
            commit_kwargs = {
                "result_payload": payload,
                "receipts": tuple(receipts),
                "model_contract_digest": model_digest,
                "plan_role_map_digest": plan_digest,
                "tokenizer_digest": tokenizer_digest,
                "chat_template_digest": template_digest,
                "application_messages": bytes(
                    turn.get("input", metadata.get("application_messages", b""))),
                "canonical_token_ids": tuple(
                    int(item) for item in turn.get("canonicalTokenIds", ())),
            }
            # The real ConversationCoordinator supports a prepare/commit
            # boundary.  Publish Provider controls against the deterministic
            # checkpoint digest first, then persist the User checkpoint.  Test
            # doubles and older coordinators retain the legacy commit-first
            # path so this change is source-compatible without weakening the
            # production ordering.
            prepare_checkpoint = getattr(coordinator, "prepare_checkpoint", None)
            prepared_checkpoint_bytes = None
            commit_now_ms = int(time.time() * 1000)
            if callable(prepare_checkpoint):
                prepared_checkpoint_bytes = prepare_checkpoint(
                    request_id, **commit_kwargs, now_ms=commit_now_ms)
                checkpoint = ConversationCheckpointV1.from_bytes(
                    prepared_checkpoint_bytes)
                checkpoint_digest = checkpoint.checkpoint_digest
            else:
                _, checkpoint_bytes = coordinator.commit_turn(
                    request_id, **commit_kwargs)
                committed = True
                checkpoint = ConversationCheckpointV1.from_bytes(checkpoint_bytes)
                checkpoint_digest = checkpoint.checkpoint_digest
            control_topic = "/ndnsf-di/conversation/control"
            expires_at = min(receipt.expires_at_ms for receipt in receipts)
            published_receipts: list[ProviderConversationStateReceiptV1] = []
            try:
                for receipt in receipts:
                    control = json.dumps({
                        "schema": "ndnsf-di-conversation-promotion-control-v1",
                        "action": "COMMIT",
                        "conversationId": conversation_id,
                        "parentContextEpoch": receipt.parent_context_epoch,
                        "successorContextEpoch": receipt.successor_context_epoch,
                        "serviceName": service_name,
                        "planRoleMapDigest": plan_digest,
                        "roleName": receipt.role_name,
                        "receiptDigest": receipt.receipt_digest,
                        "checkpointDigest": checkpoint_digest,
                        "expiresAtMs": expires_at,
                    }, sort_keys=True, separators=(",", ":")).encode("utf-8")
                    if not service_user.publish_collaboration_data(
                            receipt.provider_identity, request_id,
                            key_scope=CONVERSATION_STATE_SCOPE,
                            topic=control_topic, payload=control):
                        raise RuntimeError("conversation commit control publication failed")
                    published_receipts.append(receipt)
                # Queueing an encrypted control is not an acknowledgement that
                # the Provider accepted it.  Wait for one Provider-authored
                # commit record per role before making the User checkpoint
                # visible; this closes the cross-process atomicity gap.
                commit_ack_wait = getattr(
                    service_user, "wait_for_verified_collaboration_data", None)
                commit_ack_cleanup = getattr(
                    service_user, "clear_verified_collaboration_data", None)
                commit_ack_supported = callable(commit_ack_wait) and callable(commit_ack_cleanup)
                if commit_ack_supported:
                    commit_records = commit_ack_wait(
                        request_id,
                        key_scope=CONVERSATION_STATE_SCOPE,
                        topic_prefix="/ndnsf-di/conversation/commit",
                        min_count=len(roles),
                        timeout_ms=promotion_timeout_ms(),
                        consume=True,
                    )
                elif hasattr(service_user, "_native"):
                    # A production Python ServiceUser always exposes both
                    # methods; fail closed if an old extension is loaded.
                    raise RuntimeError(
                        "conversation commit acknowledgement path is unavailable")
                else:
                    # Retain compatibility with tiny unit-test doubles that
                    # predate the network commit-ack surface.  They do not
                    # qualify any production conversation result.
                    commit_records = ()
                if commit_ack_supported:
                    commit_keys = {
                    "schema", "requestId", "attemptEpoch", "generationId",
                    "planDigest", "conversationId", "parentContextEpoch",
                    "successorContextEpoch", "serviceName",
                    "planRoleMapDigest", "roleName", "receiptDigest",
                    "checkpointDigest", "providerIdentity", "providerBootId",
                    "cacheEpoch", "committed",
                    }
                    seen_commits: set[str] = set()
                    expected_generation = str(turn.get("generationId", ""))
                    expected_attempt = int(turn.get("attemptEpoch", 1))
                    expected_parent_epoch = int(parent.context_epoch) if parent is not None else 0
                    for record in commit_records:
                        try:
                            commit = json.loads(bytes(record.payload).decode("utf-8"))
                        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                            raise RuntimeError(
                                "conversation commit acknowledgement is malformed") from exc
                        role = str(commit.get("roleName", "")) if isinstance(commit, dict) else ""
                        provider = str(commit.get("providerIdentity", "")) if isinstance(commit, dict) else ""
                        expected_receipt = next(
                            (item.receipt_digest for item in receipts if item.role_name == role), "")
                        if (not isinstance(commit, dict) or set(commit) != commit_keys
                                or role in seen_commits or role not in roles
                                or providers.get(role) != provider
                                or str(record.producer) != provider
                                or str(record.producer_role) != role
                                or str(commit["requestId"]) != request_id
                                or int(commit["attemptEpoch"]) != expected_attempt
                                or str(commit["generationId"]) != expected_generation
                                or str(commit["planDigest"]) != exact_plan_digest
                                or str(commit["conversationId"]) != conversation_id
                                or int(commit["parentContextEpoch"]) != expected_parent_epoch
                                or int(commit["successorContextEpoch"]) != expected_parent_epoch + 1
                                or str(commit["serviceName"]) != service_name
                                or str(commit["planRoleMapDigest"]) != plan_digest
                                or str(commit["receiptDigest"]) != expected_receipt
                                or str(commit["checkpointDigest"]) != checkpoint_digest
                                or not str(commit["providerBootId"])
                                or int(commit["cacheEpoch"]) < 0
                                or commit["committed"] is not True):
                            raise RuntimeError(
                                "conversation commit acknowledgement identity mismatch")
                        seen_commits.add(role)
                    if seen_commits != set(roles):
                        raise RuntimeError(
                            "conversation commit acknowledgement set is incomplete")
                if prepared_checkpoint_bytes is not None:
                    _, checkpoint_bytes = coordinator.commit_turn(
                        request_id, **commit_kwargs, now_ms=commit_now_ms)
                    if checkpoint_bytes != prepared_checkpoint_bytes:
                        raise RuntimeError(
                            "conversation checkpoint changed between prepare and commit")
                    committed = True
            except BaseException:
                # Best-effort rollback fences any Provider that accepted the
                # preceding COMMIT controls.  The User checkpoint is still
                # absent until the final commit above succeeds.
                for receipt in published_receipts:
                    rollback = json.dumps({
                        "schema": "ndnsf-di-conversation-promotion-control-v1",
                        "action": "ROLLBACK",
                        "conversationId": conversation_id,
                        "parentContextEpoch": receipt.parent_context_epoch,
                        "successorContextEpoch": receipt.successor_context_epoch,
                        "serviceName": service_name,
                        "planRoleMapDigest": plan_digest,
                        "roleName": receipt.role_name,
                        "receiptDigest": receipt.receipt_digest,
                        "checkpointDigest": checkpoint_digest,
                        "expiresAtMs": expires_at,
                    }, sort_keys=True, separators=(",", ":")).encode("utf-8")
                    try:
                        service_user.publish_collaboration_data(
                            receipt.provider_identity, request_id,
                            key_scope=CONVERSATION_STATE_SCOPE,
                            topic=control_topic, payload=rollback)
                    except Exception:
                        pass
                raise
            with self._condition:
                self._complete_payload = bytes(payload)
                self._complete_timestamp_us = time.monotonic_ns() // 1000
                self._terminal = True
                callback = self._on_complete
                self._condition.notify_all()
            if callback is not None:
                callback(bytes(payload))
        except BaseException as exc:
            if not committed:
                try:
                    coordinator = getattr(self, "_conversation_coordinator", None)
                    request_id = str((self._conversation_turn or {}).get("requestId", ""))
                    if coordinator is not None and request_id:
                        coordinator.abort_turn(request_id)
                except Exception:
                    pass
            # Replacement may have advanced the public handle to attempt 2
            # while this promotion worker was waiting for Provider receipts.
            # Report against the currently active attempt; hard-coding epoch 1
            # would leave a failed replacement handle waiting forever.
            with self._condition:
                failure_attempt = self._current_attempt
            self._fail(failure_attempt, {
                "code": "ConversationPromotionFailed",
                "message": str(exc),
            })
        finally:
            if cleanup_user is not None and cleanup_request_id:
                try:
                    cleanup_user.clear_verified_collaboration_data(
                        cleanup_request_id, key_scope=CONVERSATION_STATE_SCOPE)
                except Exception:
                    # Scope cleanup is best effort after the terminal outcome;
                    # it must not replace the original promotion result.
                    pass

    def conversation_checkpoint(self, timeout_ms: int | None = None) -> bytes:
        """Return the committed opaque checkpoint for this completed turn.

        Checkpoint publication is owned by the coordinator after all Provider
        receipts are accepted.  A streamed result alone never manufactures a
        checkpoint, so callers observe an explicit unavailable error until the
        transaction has committed.
        """
        from ..conversation import ConversationStateUnavailable

        self._wait_terminal(timeout_ms)
        with self._condition:
            if self._error is not None:
                raise RuntimeError(str(self._error.get(
                    "message", "streaming invocation failed")))
            coordinator = self._conversation_coordinator
            turn = dict(self._conversation_turn or {})
        if coordinator is None or not turn:
            raise ConversationStateUnavailable(
                "stream has no conversation continuation")
        checkpoint = coordinator.checkpoint(str(turn["conversationId"]))
        if checkpoint is None:
            raise ConversationStateUnavailable(
                "conversation successor checkpoint is not committed")
        return bytes(checkpoint)

    @property
    def stream_error(self):
        with self._condition:
            return None if self._error is None else dict(self._error)

    def response(self, timeout_ms: int | None = None):
        self._wait_terminal(timeout_ms)
        with self._condition:
            error = None if self._error is None else dict(self._error)
            base = self._attempts.get(self._current_attempt)
        if error is not None:
            raise RuntimeError(str(error.get("message", "streaming invocation failed")))
        if base is None:
            raise RuntimeError("terminal streaming attempt is unavailable")
        return base.response(timeout_ms)

    def result(self, timeout_ms: int | None = None) -> Any:
        self._wait_terminal(timeout_ms)
        with self._condition:
            error = None if self._error is None else dict(self._error)
            base = self._attempts.get(self._current_attempt)
        if error is not None:
            raise RuntimeError(str(error.get("message", "streaming invocation failed")))
        if base is None:
            raise RuntimeError("terminal streaming attempt is unavailable")
        return base.result(timeout_ms)

    def cancel(self) -> None:
        """Cancel the currently bound NDNSF stream without starting recovery."""
        with self._condition:
            if self._native_cancel_requested:
                return
            # A normally completed stream has no native work left to cancel.
            # A failed stream, however, is terminal at the public API while
            # its ServiceUser may still own Interests/callback workers; allow
            # that failure path to reach the native collaboration exactly once.
            if self._terminal and self._error is None:
                return
            base = self._attempts.get(self._current_attempt)
            if base is None:
                raise RuntimeError("streaming attempt is not bound")
            self._native_cancel_requested = True
            if not self._terminal:
                self._terminal = True
                self._condition.notify_all()
        base.collaboration.cancel()

    @property
    def stream_metrics_for_test(self) -> Mapping[str, int]:
        return self.base.collaboration.stream_metrics_for_test

    def _bind_attempt(self, attempt: int, base: AutomaticInferenceHandle) -> None:
        with self._condition:
            if attempt not in (1, 2) or attempt in self._attempts:
                raise RuntimeError("streaming attempt binding is invalid")
            self._attempts[attempt] = base
            self._attempt_request_ids[attempt] = str(base.collaboration.request_id)
            self._condition.notify_all()

    def _wait_attempt(self, attempt: int, deadline_ms: int) -> AutomaticInferenceHandle:
        with self._condition:
            while attempt not in self._attempts:
                remaining = deadline_ms - int(time.time() * 1000)
                if remaining <= 0:
                    raise TimeoutError("streaming attempt binding deadline expired")
                self._condition.wait(remaining / 1000.0)
            return self._attempts[attempt]

    def _begin_replacement(self, attempt: int) -> tuple[int, ...] | None:
        with self._condition:
            if (self._terminal or attempt != 1 or self._current_attempt != 1
                    or self._replacement_started):
                return None
            self._replacement_started = True
            self._replacement_count = 1
            self._current_attempt = 2
            self._condition.notify_all()
            return tuple(self._token_ids)

    def _accept_event(self, attempt: int, payload: bytes) -> None:
        wire = bytes(payload)
        event_request_id = None
        event_generation_id = None
        try:
            event = json.loads(wire.decode("utf-8"))
            schema = event.get("schema", event.get("type"))
            token_id = int(event["tokenId"])
            token_epoch = int(event["tokenEpoch"])
            prefix_digest = str(event["acceptedPrefixDigest"])
            if isinstance(event, Mapping):
                event_request_id = (str(event["requestId"])
                                    if "requestId" in event else None)
                event_generation_id = (str(event["generationId"])
                                       if "generationId" in event else None)
        except (UnicodeDecodeError, json.JSONDecodeError, KeyError,
                TypeError, ValueError) as exc:
            self._fail(attempt, {
                "code": "InvalidStreamEvent",
                "message": "invalid GenerationTokenEventV1 payload",
            })
            return
        callback = None
        callback_error = None
        timestamp_us = time.monotonic_ns() // 1000
        with self._condition:
            if self._terminal or attempt != self._current_attempt:
                self._stale_event_count += 1
                return
            expected_request_id = self._attempt_request_ids.get(attempt)
            mismatch = (
                event_request_id is not None and expected_request_id
                and event_request_id != expected_request_id
            ) or (
                event_generation_id is not None and self._generation_id
                and event_generation_id != self._generation_id
            )
            expected_epoch = len(self._token_ids) + 1
            expected_digest = "sha256:" + hashlib.sha256(
                ",".join(str(value) for value in
                         self._token_ids + [token_id]).encode("ascii")
            ).hexdigest()
            if (mismatch or schema != "GenerationTokenEventV1"
                    or token_id < 0 or token_epoch != expected_epoch
                    or prefix_digest != expected_digest):
                self._lineage_reject_count += 1
                if token_epoch < expected_epoch:
                    self._duplicate_reject_count += 1
                elif token_epoch > expected_epoch:
                    self._gap_reject_count += 1
                value = {
                    "code": "StreamEventLineageMismatch",
                    "message": "GenerationTokenEventV1 lineage mismatch",
                    "attemptEpoch": attempt,
                }
                self._error = value
                self._error_timestamp_us = timestamp_us
                self._terminal = True
                callback = self._on_error
                callback_error = value
            else:
                self._token_ids.append(token_id)
                self._events.append(wire)
                self._event_timestamps_us.append(timestamp_us)
                callback = self._on_event
            self._condition.notify_all()
        if callback_error is not None:
            if callback is not None:
                try:
                    callback(dict(callback_error))
                except BaseException:
                    pass
            return
        if callback is not None:
            try:
                callback(wire)
            except BaseException as exc:
                with self._condition:
                    self._callback_error_count += 1
                self._fail(attempt, {
                    "code": "StreamCallbackFailed",
                    "message": str(exc) or type(exc).__name__,
                })

    def _complete(self, attempt: int, payload: bytes) -> None:
        wire = bytes(payload)
        validation_error = ""
        completion_request_id = None
        completion_generation_id = None
        try:
            decoded = json.loads(wire.decode("utf-8"))
            if isinstance(decoded, Mapping):
                completion_request_id = (str(decoded["requestId"])
                                         if "requestId" in decoded else None)
                completion_generation_id = (str(decoded["generationId"])
                                            if "generationId" in decoded else None)
                final_ids = decoded.get("tokenIds")
                if final_ids is not None and \
                        tuple(int(value) for value in final_ids) != \
                        tuple(self._token_ids):
                    validation_error = "final token transcript mismatch"
        except (UnicodeDecodeError, json.JSONDecodeError):
            # Generic streamed services may return an opaque non-JSON result.
            pass
        except (TypeError, ValueError):
            validation_error = "final token transcript is invalid"
        with self._condition:
            expected_request_id = self._attempt_request_ids.get(attempt)
            if (completion_request_id is not None and expected_request_id
                    and completion_request_id != expected_request_id):
                validation_error = "final response request identity mismatch"
            elif (completion_generation_id is not None and self._generation_id
                  and completion_generation_id != self._generation_id):
                validation_error = "final response generation identity mismatch"
        if validation_error:
            self._fail(attempt, {
                "code": "TranscriptMismatch",
                "message": validation_error,
            })
            return
        callback = None
        timestamp_us = time.monotonic_ns() // 1000
        with self._condition:
            if self._terminal or attempt != self._current_attempt:
                return
            # The planner can complete before APPClient has a chance to call
            # _attach_conversation(). Defer every conversation completion,
            # including that fast path, so the aggregate receipt transaction
            # cannot race with handle attachment and expose a result without
            # its successor checkpoint.
            if self._conversation_expected:
                self._pending_complete_payload = wire
                self._condition.notify_all()
                return
            self._complete_payload = wire
            self._complete_timestamp_us = timestamp_us
            self._terminal = True
            callback = self._on_complete
            self._condition.notify_all()
        if callback is not None:
            try:
                callback(wire)
            except BaseException as exc:
                self._fail(attempt, {
                    "code": "StreamCallbackFailed",
                    "message": str(exc) or type(exc).__name__,
                })

    def _fail(self, attempt: int, error: Mapping[str, Any]) -> None:
        callback = None
        value = dict(error)
        timestamp_us = time.monotonic_ns() // 1000
        with self._condition:
            if self._terminal or attempt != self._current_attempt:
                return
            value.setdefault("attemptEpoch", attempt)
            self._error = value
            self._error_timestamp_us = timestamp_us
            self._terminal = True
            callback = self._on_error
            self._condition.notify_all()
        if callback is not None:
            # Error delivery is best effort.  A user error sink must not
            # escape the transport/delivery thread or leave the already
            # terminal handle in an unobservable state.
            try:
                callback(dict(value))
            except BaseException:
                pass

    def _wait_terminal(self, timeout_ms: int | None) -> None:
        deadline = None if timeout_ms is None else (
            time.monotonic() + max(0, int(timeout_ms)) / 1000.0)
        with self._condition:
            while not self._terminal:
                if deadline is None:
                    self._condition.wait()
                    continue
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("streaming result deadline expired")
                self._condition.wait(remaining)


ProviderViewFactory = Callable[
    [Any, str, int], ProviderPlanningView
]
CatalogSnapshotProvider = Callable[[], Tuple[Any, ...]]
GrantBindingProvider = Callable[[ProviderGrantViewV1, int], GrantBindingV1]


@dataclass(frozen=True)
class AckRoleCoveragePolicy:
    """Application-owned early ACK closure for known required role coverage.

    This policy is intentionally only a coverage predicate.  It validates each
    positive ACK through the configured DI provider-view factory, then reports
    whether the bounded role hint is covered.  Graph inspection, split
    enumeration, provider assignment, artifact publication, and Selection
    remain after the immutable ACK_CLOSED snapshot.
    """

    required_roles: tuple[str, ...]
    provider_view_factory: ProviderViewFactory
    model_intent_digest: str
    deadline_ms: int

    def __post_init__(self) -> None:
        roles = tuple(str(role) for role in self.required_roles)
        if (not roles or any(not role for role in roles)
                or len(set(roles)) != len(roles)):
            raise ValueError("ACK role coverage requires unique non-empty roles")
        if not callable(self.provider_view_factory):
            raise TypeError("provider_view_factory must be callable")
        object.__setattr__(self, "required_roles", roles)

    def __call__(self, candidates: tuple[Any, ...]) -> bool:
        covered: set[str] = set()
        for candidate in candidates:
            if not bool(getattr(candidate, "status", False)):
                continue
            try:
                view = self.provider_view_factory(
                    candidate, self.model_intent_digest, self.deadline_ms)
            except (TypeError, ValueError, RuntimeError):
                # Invalid or stale offers cannot contribute to early closure;
                # the normal ACK timeout still provides the final chance to
                # collect a valid offer.
                continue
            covered.update(str(role) for role in view.accepted_roles)
        return set(self.required_roles).issubset(covered)


@dataclass(frozen=True)
class MaterializedSplit:
    """Trusted local materialization result for one generated split."""

    candidate_digest: str
    artifact_digests_by_role: Mapping[str, str]
    local_references_by_role: Mapping[str, str]

    def __post_init__(self) -> None:
        _require_digest(self.candidate_digest, "materialized candidate_digest")
        digests = dict(self.artifact_digests_by_role)
        references = dict(self.local_references_by_role)
        if not digests or set(digests) != set(references):
            raise ValueError("materialized split role coverage mismatch")
        for role, digest in digests.items():
            if not role or not references[role]:
                raise ValueError("materialized split contains an empty role/reference")
            _require_digest(digest, f"materialized artifact digest for {role}")
        object.__setattr__(
            self, "artifact_digests_by_role", MappingProxyType(digests))
        object.__setattr__(
            self, "local_references_by_role", MappingProxyType(references))


@dataclass(frozen=True)
class PublishedSplit:
    """Content-bound DistributedRepo publication visible to Providers."""

    candidate_digest: str
    artifact_digests_by_role: Mapping[str, str]
    artifact_data_names_by_role: Mapping[str, str]
    # A V3 canonical artifact has a stable identity (the value above) and may
    # use a separately named, encrypted large-data object for transport.  The
    # latter is carried in Selection as ``artifactDataName`` while the former
    # remains the authenticated ``assignedArtifact`` root.
    artifact_fetch_data_names_by_role: Mapping[str, str] = field(
        default_factory=dict)

    def __post_init__(self) -> None:
        _require_digest(self.candidate_digest, "published candidate_digest")
        digests = dict(self.artifact_digests_by_role)
        names = dict(self.artifact_data_names_by_role)
        fetch_names = dict(self.artifact_fetch_data_names_by_role or names)
        if (not digests or set(digests) != set(names)
                or set(fetch_names) != set(digests)):
            raise ValueError("published split role coverage mismatch")
        for role, digest in digests.items():
            if (not role or not names[role].startswith("/")
                    or not fetch_names[role].startswith("/")):
                raise ValueError("published artifact requires an absolute NDN name")
            _require_digest(digest, f"published artifact digest for {role}")
        object.__setattr__(
            self, "artifact_digests_by_role", MappingProxyType(digests))
        object.__setattr__(
            self, "artifact_data_names_by_role", MappingProxyType(names))
        object.__setattr__(
            self, "artifact_fetch_data_names_by_role",
            MappingProxyType(fetch_names))


def _candidate_artifacts_by_execution_key(
    candidate: SplitCandidate,
) -> dict[str, str]:
    """Flatten logical roles into the exact role/rank key space."""

    flattened: dict[str, str] = {}
    for role in candidate.execution_plan.roles:
        degree = int(candidate.tensor_degrees_by_role.get(role, 1))
        values = candidate.rank_artifact_digests_by_role.get(
            role, (candidate.artifacts_by_role[role][0],))
        for rank, digest in enumerate(values):
            key = role if degree == 1 else f"{role}#{rank}"
            flattened[key] = digest
    return flattened


class SplitMaterializer(Protocol):
    """Trusted side-effect port that creates bytes for a generated split."""

    def materialize(
        self, candidate: SplitCandidate, *, deadline_ms: int,
    ) -> MaterializedSplit:
        ...


class DistributedArtifactPublisher(Protocol):
    """Trusted port backed by NDNSF-DistributedRepo."""

    def publish(
        self,
        candidate: SplitCandidate,
        materialized: MaterializedSplit,
        *,
        deadline_ms: int,
    ) -> PublishedSplit:
        ...

    def resolve_existing(
        self, candidate: SplitCandidate, *, deadline_ms: int,
    ) -> PublishedSplit:
        ...


class CanonicalArtifactEnsurer(Protocol):
    """Request-first V3 port for canonical layer publication/lookup.

    Unlike ``DistributedArtifactPublisher`` this port is not a role-split
    compatibility path.  It is invoked only after ACK_CLOSED and graph
    planning, and returns the content-addressed Data names that become part of
    the sealed Selection.  Implementations may resolve an exact existing
    publication or publish missing canonical objects, but must never mutate a
    Provider during ACK collection.
    """

    def ensure(
        self,
        candidate: SplitCandidate,
        role_specs: tuple[RoleAssemblySpec, ...],
        *,
        deadline_ms: int,
    ) -> PublishedSplit:
        ...


class RejectGeneratedSplitMaterializer:
    """Fail closed when an experiment requires an existing pre-split."""

    def materialize(
        self, candidate: SplitCandidate, *, deadline_ms: int,
    ) -> MaterializedSplit:
        del candidate, deadline_ms
        raise RuntimeError("generated split materialization is disabled")


class NetworkCatalogSnapshotResolver:
    """Resolve active pre-split metadata through signed APP Data.

    The resolver deliberately owns only artifact-publication metadata.  It is
    called by the coordinator after ``CollaborationAckClosed`` and therefore
    cannot close capability discovery or select a candidate.  The native
    ``fetch_signed_app_data`` path validates the exact name and expected signer
    before this class parses the canonical, digest-bound snapshot envelope.
    """

    SCHEMA = "ndnsf-di-presplit-catalog-snapshot-v1"

    def __init__(self, fetch_signed_app_data: Callable[..., Any], *,
                 data_name: str, expected_signer: str,
                 timeout_ms: int = 5000) -> None:
        if not callable(fetch_signed_app_data):
            raise TypeError("fetch_signed_app_data must be callable")
        if not str(data_name).startswith("/"):
            raise ValueError("catalogue data name must be an absolute NDN name")
        if not str(expected_signer).startswith("/"):
            raise ValueError("catalogue signer must be an absolute NDN name")
        if int(timeout_ms) <= 0:
            raise ValueError("catalogue fetch timeout must be positive")
        self._fetch = fetch_signed_app_data
        self.data_name = str(data_name)
        self.expected_signer = str(expected_signer)
        self.timeout_ms = int(timeout_ms)

    def __call__(self) -> tuple[PreSplitCatalogSnapshot, ...]:
        result = self._fetch(
            self.data_name,
            self.expected_signer,
            timeout_ms=self.timeout_ms,
        )
        if not bool(getattr(result, "success", False)):
            reason = str(getattr(result, "error", "catalogue fetch failed"))
            raise LookupError("signed pre-split catalogue unavailable: " + reason)
        if str(getattr(result, "data_name", "")) != self.data_name:
            raise ValueError("signed pre-split catalogue exact-name mismatch")
        try:
            envelope = json.loads(bytes(getattr(result, "payload", b"")).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError, TypeError) as exc:
            raise ValueError("signed pre-split catalogue is malformed") from exc
        if (not isinstance(envelope, Mapping)
                or envelope.get("schema") != self.SCHEMA
                or envelope.get("recordName") != self.data_name
                or not isinstance(envelope.get("snapshots"), list)
                or not isinstance(envelope.get("snapshotDigest"), str)):
            raise ValueError("signed pre-split catalogue envelope is invalid")
        snapshots = tuple(
            PreSplitCatalogSnapshot.from_mapping(value)
            for value in envelope["snapshots"]
        )
        ordered = tuple(sorted(
            snapshots, key=lambda item: (item.alias, item.manifest_digest)))
        if canonical_digest([item.to_dict() for item in ordered]) != envelope["snapshotDigest"]:
            raise ValueError("signed pre-split catalogue digest mismatch")
        aliases = [item.alias for item in ordered]
        manifests = [item.manifest_digest for item in ordered]
        candidates = [item.candidate_digest for item in ordered]
        if any(item.status != "ACTIVE" for item in ordered):
            raise ValueError(
                "signed pre-split catalogue requires ACTIVE snapshots")
        if (len(set(aliases)) != len(aliases)
                or len(set(zip(manifests, candidates))) != len(manifests)
                or len(set(candidates)) != len(candidates)):
            raise ValueError("signed pre-split catalogue contains duplicates")
        return ordered


def encode_runtime_catalog_snapshot(
        data_name: str,
        snapshots: tuple[PreSplitCatalogSnapshot, ...] | list[PreSplitCatalogSnapshot],
        *,
        required_candidate_digests: tuple[str, ...] | list[str] = (),
    ) -> bytes:
    """Encode the signed APP payload for the active pre-split catalog.

    The package candidate catalogue and this runtime object catalogue are
    intentionally separate.  This helper only builds the canonical payload;
    the caller must publish it through the controller-owned signed APP path
    and retain that publication receipt.  Requiring the expected candidate
    digests here prevents a runner from publishing a syntactically valid but
    incomplete snapshot before starting User.
    """
    if (not isinstance(data_name, str) or not data_name.startswith("/")
            or any(char.isspace() for char in data_name)):
        raise ValueError("runtime catalogue data name must be absolute")
    values = tuple(snapshots)
    if not values:
        raise ValueError("runtime catalogue requires at least one snapshot")
    if any(not isinstance(item, PreSplitCatalogSnapshot)
           for item in values):
        raise TypeError("runtime catalogue snapshots must be typed records")
    ordered = tuple(sorted(
        values, key=lambda item: (item.alias, item.manifest_digest)))
    aliases = [item.alias for item in ordered]
    manifests = [item.manifest_digest for item in ordered]
    candidates = [item.candidate_digest for item in ordered]
    if (len(set(aliases)) != len(aliases)
            or len(set(zip(manifests, candidates))) != len(manifests)
            or len(set(candidates)) != len(candidates)):
        raise ValueError("runtime catalogue contains duplicate snapshots")
    if any(item.status != "ACTIVE" for item in ordered):
        raise ValueError("runtime catalogue requires ACTIVE snapshots")
    required = {str(item) for item in required_candidate_digests}
    if not required.issubset(set(candidates)):
        raise ValueError("runtime catalogue candidate coverage is incomplete")
    snapshot_values = [item.to_dict() for item in ordered]
    envelope = {
        "schema": NetworkCatalogSnapshotResolver.SCHEMA,
        "recordName": data_name,
        "snapshotDigest": canonical_digest(snapshot_values),
        "snapshots": snapshot_values,
    }
    return json.dumps(
        envelope, sort_keys=True, separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")


class CatalogSnapshotArtifactPublisher:
    """Resolve exact active pre-split snapshots without republishing bytes."""

    def __init__(self, snapshot_provider: CatalogSnapshotProvider) -> None:
        self._snapshot_provider = snapshot_provider

    def publish(
        self,
        candidate: SplitCandidate,
        materialized: MaterializedSplit,
        *,
        deadline_ms: int,
    ) -> PublishedSplit:
        del candidate, materialized, deadline_ms
        raise RuntimeError("pre-split-only publisher cannot publish generated bytes")

    def resolve_existing(
        self, candidate: SplitCandidate, *, deadline_ms: int,
    ) -> PublishedSplit:
        if int(time.time() * 1000) >= deadline_ms:
            raise TimeoutError("pre-split resolution deadline expired")
        matches = [
            value for value in self._snapshot_provider()
            if value.status == "ACTIVE"
            and value.candidate_digest == candidate.candidate_digest
            and value.model_content_digest == candidate.model.content_digest
            and value.semantics_digest == candidate.model.semantics_digest
            and value.graph_digest == candidate.graph_digest
        ]
        if len(matches) != 1:
            raise ValueError("exact active pre-split publication is not unique")
        snapshot = matches[0]
        snapshot_precision = str(getattr(snapshot, "precision", "") or "")
        candidate_precision = str(
            getattr(candidate.model, "precision", "") or "")
        if snapshot_precision and candidate_precision \
                and snapshot_precision != candidate_precision:
            raise ValueError("pre-split publication precision mismatch")
        snapshot_backend = str(getattr(snapshot, "backend", "") or "")
        if any(snapshot_backend not in candidate.requirements_by_role[role].backends
               for role in candidate.execution_plan.roles):
            raise ValueError("pre-split publication backend mismatch")
        expected = _candidate_artifacts_by_execution_key(candidate)
        names: dict[str, str] = {}
        for role in candidate.execution_plan.roles:
            try:
                values = tuple(snapshot.artifact_data_names[role])
            except (KeyError, TypeError):
                raise ValueError(
                    "pre-split publication is missing a candidate role") from None
            degree = int(candidate.tensor_degrees_by_role.get(role, 1))
            if len(values) != degree:
                raise ValueError(
                    "pre-split publication does not cover every role rank")
            for rank, data_name in enumerate(values):
                key = role if degree == 1 else f"{role}#{rank}"
                names[key] = data_name
        return PublishedSplit(
            candidate_digest=candidate.candidate_digest,
            artifact_digests_by_role=expected,
            artifact_data_names_by_role=names,
        )


def v2_provider_view_factory(
    verify_offer_signature: Callable[[DIProviderOfferV2], bool],
) -> ProviderViewFactory:
    """Convert an authenticated generic ACK payload into a sanitized DI view."""
    if not callable(verify_offer_signature):
        raise TypeError("V2 Provider offer verifier must be callable")

    def convert(ack: Any, model_intent_digest: str,
                deadline_ms: int) -> ProviderPlanningView:
        offer = DIProviderOfferV2.from_bytes(bytes(ack.payload))
        return build_provider_planning_view(
            offer,
            ack_status=bool(ack.status),
            at_ms=int(time.time() * 1000),
            request_id=offer.request_id,
            attempt=offer.attempt,
            model_intent_digest=model_intent_digest,
            deadline_ms=deadline_ms,
            verify_signature=verify_offer_signature,
        )

    return convert


def _validate_v3_ack_offer_provenance(
    ack: Any, offer: ProviderOfferV3,
) -> None:
    """Require packet-authenticated identity before V3 planning."""
    signer_identity = str(getattr(ack, "signer_identity", "") or "")
    signer_locator = str(getattr(ack, "signer_key_locator", "") or "")
    wire_digest = str(getattr(ack, "validated_wire_digest", "") or "")
    if (not signer_identity or not signer_locator
            or not re.fullmatch(r"sha256:[0-9a-f]{64}", wire_digest)):
        raise ValueError("V3 ACK authenticated provenance is missing or malformed")
    if signer_identity != offer.provider:
        raise ValueError("V3 ACK signer does not match Provider offer identity")
    if not signer_locator.startswith(signer_identity + "/KEY/"):
        raise ValueError("V3 ACK KeyLocator is outside Provider identity")
    if str(getattr(ack, "service_name", "") or "") != offer.service:
        raise ValueError("V3 ACK service does not match Provider offer")
    if str(getattr(ack, "request_id", "") or "") != offer.request_id:
        raise ValueError("V3 ACK request does not match Provider offer")


def v3_provider_view_factory(
    verify_offer_signature: Callable[[ProviderOfferV3], bool],
    *, require_ack_provenance: bool = True,
) -> Callable[[Any, str, int, str], ProviderPlanningViewV3]:
    """Convert a signed V3 ACK payload into an immutable planning view.

    V3 offers are graph-wildcarded during the first ACK round because the
    dependency graph is intentionally inspected only after ACK_CLOSED.  The
    factory therefore accepts the actual graph digest as a fourth argument and
    performs the final binding before the strategy sees the view.
    """

    if not callable(verify_offer_signature):
        raise TypeError("V3 Provider offer verifier must be callable")

    def convert(ack: Any, model_intent_digest: str,
                deadline_ms: int, graph_digest: str = "") -> ProviderPlanningViewV3:
        offer = ProviderOfferV3.from_bytes(bytes(ack.payload))
        if bool(getattr(ack, "status", False)) != bool(offer.status):
            raise ValueError("V3 ACK status/offer status mismatch")
        if require_ack_provenance:
            _validate_v3_ack_offer_provenance(ack, offer)
        # A production verifier may expose the richer ACK-aware method.  It
        # must run before the offer enters the planning view so the candidate
        # policy can bind the already Trust-Schema-validated packet identity,
        # not a Python-reconstructed Provider name.  Legacy fixture callbacks
        # remain supported for compatibility tests.
        verify_ack = getattr(verify_offer_signature, "verify_ack", None)
        if callable(verify_ack):
            verified = verify_ack(
                offer,
                ack,
                model_digest=str(model_intent_digest or ""),
                graph_digest=str(graph_digest or ""),
                request_id=str(getattr(ack, "request_id", "") or ""),
                deadline_ms=int(deadline_ms),
            )
            if verified is False:
                raise ValueError("V3 Provider offer trust verification failed")
        # The offer binds model content, while the public application intent
        # digest binds model+semantics+revision.  The coordinator checks both
        # at the request envelope boundary; V3 offer verification receives the
        # exact request-bound model digest after descriptor inspection.
        return ProviderPlanningViewV3.from_offer(
            offer,
            request_id=str(getattr(ack, "request_id", "") or offer.request_id),
            model_digest=str(model_intent_digest or ""),
            graph_digest=str(graph_digest or ""),
            now_ms=int(time.time() * 1000),
            deadline_ms=int(deadline_ms),
            verify_signature=verify_offer_signature,
        )

    return convert


def validate_spec175_ordinary_v3_proposal(
    proposal: PlacementProposalV3,
    *,
    required_roles: tuple[str, ...] | list[str],
    tensor_degrees_by_role: Mapping[str, int] | None = None,
    hybrid_plan: Any | None = None,
    placement_profile: str = DI_PLACEMENT_V3,
) -> None:
    """Enforce the narrow Spec175 streamed-invocation V3 boundary.

    Spec174 keeps the richer TensorGroup/hybrid protocol, but the ordinary
    Spec175 stream has one rank-zero pipeline role per selected Provider.  The
    check is deliberately placed after the strategy returns its data-only
    proposal and before canonical publication or Selection, so an incompatible
    proposal cannot reach a Provider or execute as a silent fallback.
    """

    if str(placement_profile) != DI_PLACEMENT_V3:
        raise ValueError(
            "Spec175 streamed invocation requires placement profile "
            f"{DI_PLACEMENT_V3}")
    if not isinstance(proposal, PlacementProposalV3):
        raise TypeError("ordinary Spec175 V3 strategy returned a non-proposal")
    if hybrid_plan is not None:
        raise ValueError(
            "ordinary Spec175 V3 does not permit a hybrid/TensorGroup plan")

    expected = tuple(str(role) for role in required_roles)
    if (not expected or len(set(expected)) != len(expected)
            or any(not role for role in expected)):
        raise ValueError("ordinary Spec175 V3 required role set is invalid")
    specs = tuple(proposal.roles)
    spec_roles = tuple(str(spec.role) for spec in specs)
    if (len(specs) != len(expected)
            or len(set(spec_roles)) != len(spec_roles)
            or set(spec_roles) != set(expected)):
        raise ValueError(
            "ordinary Spec175 V3 role set does not cover every role exactly once")

    degrees = tensor_degrees_by_role or {}
    if any(int(degrees.get(role, 1)) != 1 for role in expected):
        raise ValueError(
            "ordinary Spec175 V3 requires tensor degree one for every role")
    if any(spec.rank != 0 for spec in specs):
        raise ValueError(
            "ordinary Spec175 V3 rejects non-zero tensor rank roles")
    if any(spec.role_kind in {"TENSOR_RANK", "HYBRID_RANK"}
           for spec in specs):
        raise ValueError(
            "ordinary Spec175 V3 rejects tensor or hybrid rank roles")

    assignments = {
        str(role): str(provider)
        for role, provider in proposal.provider_by_role.items()
    }
    if set(assignments) != set(expected):
        raise ValueError(
            "ordinary Spec175 V3 role assignment does not cover every role")
    if (any(not provider for provider in assignments.values())
            or len(set(assignments.values())) != len(assignments)):
        raise ValueError(
            "ordinary Spec175 V3 role/Provider assignment must be one-to-one")


def _v3_candidate_priority_key(candidate: SplitCandidate) -> tuple[int, str]:
    """Order already-enumerated candidates by adapter-signed preference.

    Candidate feasibility is evaluated separately by the strategy.  This key
    is only a deterministic choice among candidates that can be planned; it
    deliberately ignores catalogue/list order, cache hints, and estimated
    runtime cost.
    """
    priority = int(getattr(candidate, "selection_priority", 0))
    if priority < 0:
        raise ValueError("candidate selection priority must be non-negative")
    return (-priority, candidate.candidate_digest)


def _v3_role_kind(role: str) -> str:
    """Map a role name to its conservative assembly kind.

    ``Shard`` is not synonymous with tensor parallelism: the Spec180 YOLO
    DetectShard roles are graph component sets. A shard under an explicit
    stage/tensor/rank namespace remains a tensor-rank role, preserving the
    existing Qwen naming convention without rejecting YOLO component roles.
    """
    parts = tuple(part.lower() for part in str(role).strip("/").split("/")
                  if part)
    has_tensor = any("tensor" in part or "rank" in part for part in parts)
    has_stage = any(
        part in {"pipeline", "stage", "stages"} or part.startswith("stage-")
        for part in parts)
    has_shard = any("shard" in part for part in parts)
    if has_tensor or (has_shard and has_stage):
        return "TENSOR_RANK"
    if has_stage:
        return "PIPELINE_RANGE"
    return "COMPONENT_SET"


class AutomaticPlanningCoordinator:
    """Trusted composition around an untrusted, data-only placement decision."""

    def __init__(
        self,
        *,
        service_user: Any,
        service_name: str,
        adapters: Mapping[str, ModelFamilyAdapter],
        strategy: ModelPlacementStrategy,
        provider_view_factory: ProviderViewFactory,
        split_materializer: SplitMaterializer,
        artifact_publisher: DistributedArtifactPublisher,
        canonical_artifact_ensurer: CanonicalArtifactEnsurer | None = None,
        catalog_snapshot_provider: CatalogSnapshotProvider | None = None,
        budget: CandidateBudget | None = None,
        ack_timeout_ms: int = 300,
        data_v1_no_progress_ms: int = 2000,
        ack_coverage_roles: tuple[str, ...] = (),
        ack_coverage_predicate: Callable[[tuple[Any, ...]], bool] | None = None,
        group_epoch_key_wrapper: Callable[[bytes, bytes], bytes] | None = None,
        grant_binding_provider: GrantBindingProvider | None = None,
        protection_epoch: str = "plaintext-v1",
        lifecycle_observer: Callable[[str, Mapping[str, Any]], None] | None = None,
    ) -> None:
        if not service_name or not adapters:
            raise ValueError("automatic planning coordinator is incomplete")
        if not callable(getattr(split_materializer, "materialize", None)):
            raise TypeError("split_materializer does not implement materialize")
        if (not callable(getattr(artifact_publisher, "publish", None))
                or not callable(
                    getattr(artifact_publisher, "resolve_existing", None))):
            raise TypeError(
                "artifact_publisher does not implement publish/resolve_existing")
        if not protection_epoch:
            raise ValueError("protection epoch must not be empty")
        self.protection_epoch = str(protection_epoch)
        if ack_timeout_ms <= 0:
            raise ValueError("ACK collection timeout must be positive")
        if data_v1_no_progress_ms <= 0:
            raise ValueError("NDNSF_DATA_V1 no-progress bound must be positive")
        self.service_user = service_user
        self.service_name = str(service_name)
        self.adapters = MappingProxyType(dict(adapters))
        self.strategy = strategy
        self.provider_view_factory = provider_view_factory
        self.split_materializer = split_materializer
        self.artifact_publisher = artifact_publisher
        if canonical_artifact_ensurer is not None and not callable(
                getattr(canonical_artifact_ensurer, "ensure", None)):
            raise TypeError(
                "canonical_artifact_ensurer does not implement ensure")
        self.canonical_artifact_ensurer = canonical_artifact_ensurer
        # Remember whether the caller supplied an artifact authority.  V3
        # requests that explicitly use the signed catalog must resolve (or
        # materialize) role artifacts before sealing Selection; the historical
        # direct-coordinator fixtures omit this provider and intentionally keep
        # their synthetic artifact-name behavior.
        self._catalog_snapshot_explicit = catalog_snapshot_provider is not None
        self.catalog_snapshot_provider = (
            catalog_snapshot_provider or (lambda: ()))
        self.budget = budget or CandidateBudget(
            max_candidates=16, max_policy_ms=100)
        self.ack_timeout_ms = int(ack_timeout_ms)
        self.data_v1_no_progress_ms = int(data_v1_no_progress_ms)
        self.ack_coverage_roles = tuple(str(role) for role in ack_coverage_roles)
        if (len(set(self.ack_coverage_roles)) != len(self.ack_coverage_roles)
                or any(not role for role in self.ack_coverage_roles)):
            raise ValueError("ACK coverage roles must be unique non-empty strings")
        if (ack_coverage_predicate is not None
                and not callable(ack_coverage_predicate)):
            raise TypeError("ack_coverage_predicate must be callable")
        self.ack_coverage_predicate = ack_coverage_predicate
        if (group_epoch_key_wrapper is not None
                and not callable(group_epoch_key_wrapper)):
            raise TypeError("group_epoch_key_wrapper must be callable")
        self.group_epoch_key_wrapper = (
            group_epoch_key_wrapper or _native_group_epoch_key_wrapper)
        if (grant_binding_provider is not None
                and not callable(grant_binding_provider)):
            raise TypeError("grant_binding_provider must be callable")
        self.grant_binding_provider = grant_binding_provider

        if (lifecycle_observer is not None
                and not callable(lifecycle_observer)):
            raise TypeError("lifecycle_observer must be callable")
        self.lifecycle_observer = lifecycle_observer

    def _emit_lifecycle(self, milestone: str, *, request_id: str,
                        attempt: int, **fields: Any) -> None:
        """Report a bounded planning transition to an evidence owner.

        The observer is outside the placement decision and receives only
        scalar metadata plus private request/attempt bindings. Exceptions are
        propagated so a required evidence writer cannot silently emit a
        partial trace.
        """
        if self.lifecycle_observer is None:
            return
        payload = dict(fields)
        payload["_requestId"] = str(request_id)
        payload["_attemptId"] = f"attempt-{int(attempt)}"
        self.lifecycle_observer(str(milestone), payload)

    @staticmethod
    def _validated_data_v1_key_offer(
        ack: Any,
        provider_offer: ProviderOfferV3,
    ) -> tuple[bytes, str]:
        fields = dict(
            getattr(ack, "selection_input_key_offer", {}) or {})
        required = {
            "schemaVersion", "recipient", "recipientCertName",
            "recipientPublicKey", "recipientCertDigest",
            "providerBootEpoch", "ndnsfDataV1EndpointPrefix",
        }
        if not required.issubset(fields):
            raise ValueError(
                f"Provider {provider_offer.provider} omitted NDNSF_DATA_V1 key offer")
        mismatched = []
        if fields["schemaVersion"] != "1":
            mismatched.append("schemaVersion")
        if fields["recipient"] != provider_offer.provider:
            mismatched.append("recipient")
        if not fields["recipientCertName"]:
            mismatched.append("recipientCertName")
        # The native key offer binds the epoch to its Provider identity
        # ("/example/provider/X:1234") so a cross-Provider epoch can never be
        # confused; the V3 offer carries the bare epoch.  Accept only these
        # two equivalent forms.
        boot_epoch = str(fields["providerBootEpoch"])
        offer_epoch = str(provider_offer.boot_epoch)
        boot_epoch_ok = (
            boot_epoch == offer_epoch
            or boot_epoch == f"{provider_offer.provider}:{offer_epoch}"
        )
        if not boot_epoch_ok:
            mismatched.append("providerBootEpoch")
        if not fields["ndnsfDataV1EndpointPrefix"]:
            mismatched.append("ndnsfDataV1EndpointPrefix")
        if mismatched:
            diagnostic = ""
            if "providerBootEpoch" in mismatched:
                diagnostic = (
                    f" core={fields['providerBootEpoch']}"
                    f" offer={provider_offer.boot_epoch}")
            raise ValueError(
                f"Provider {provider_offer.provider} returned a mismatched key offer: "
                + ",".join(mismatched) + diagnostic)
        endpoint_prefix = str(fields["ndnsfDataV1EndpointPrefix"]).rstrip("/")
        provider_prefix = str(provider_offer.provider).rstrip("/")
        if (endpoint_prefix != provider_prefix
                and not endpoint_prefix.startswith(provider_prefix + "/")):
            raise ValueError(
                f"Provider {provider_offer.provider} advertised an endpoint "
                "outside its signed identity namespace")
        public_key_hex = str(fields["recipientPublicKey"])
        if (not public_key_hex or len(public_key_hex) % 2 != 0
                or public_key_hex != public_key_hex.lower()):
            raise ValueError(
                f"Provider {provider_offer.provider} returned an invalid key offer")
        try:
            public_key = bytes.fromhex(public_key_hex)
        except ValueError as exc:
            raise ValueError(
                f"Provider {provider_offer.provider} returned an invalid key offer") from exc
        expected_digest = "sha256:" + hashlib.sha256(public_key).hexdigest()
        if str(fields["recipientCertDigest"]).lower() != expected_digest:
            raise ValueError(
                f"Provider {provider_offer.provider} key offer digest mismatch")
        return public_key, endpoint_prefix

    def _seal_v3_group_capabilities(
        self,
        *,
        request_id: str,
        proposal: PlacementProposalV3,
        dependencies: tuple[Mapping[str, Any], ...],
        provider_views: Mapping[str, ProviderPlanningViewV3],
        provider_offers: Mapping[str, ProviderOfferV3],
        provider_acks: Mapping[str, Any],
        plan_digest: str,
        deadline_ms: int,
        generation_contract: GenerationExecutionContractV1 | None = None,
    ) -> tuple[dict[str, str], dict[int, Mapping[str, Any]]]:
        """Seal one least-privilege capability per connected Provider group."""

        role_counts = {
            item.role: sum(other.role == item.role for other in proposal.roles)
            for item in proposal.roles
        }
        providers_by_role: dict[str, set[str]] = {}
        for role in proposal.roles:
            key = (role.role if role_counts[role.role] == 1
                   else f"{role.role}#{role.rank}")
            provider = str(proposal.provider_by_role[key])
            providers_by_role.setdefault(role.role, set()).add(provider)
            providers_by_role.setdefault(key, set()).add(provider)

        parent: dict[str, str] = {}

        def find(item: str) -> str:
            parent.setdefault(item, item)
            while parent[item] != item:
                parent[item] = parent[parent[item]]
                item = parent[item]
            return item

        def union(left: str, right: str) -> None:
            left_root, right_root = find(left), find(right)
            if left_root != right_root:
                parent[max(left_root, right_root)] = min(left_root, right_root)

        cross_dependencies: list[
            tuple[
                int, Mapping[str, Any], tuple[str, ...], tuple[str, ...], str,
                dict[str, Any],
            ]
        ] = []
        for index, dependency in enumerate(dependencies):
            dependency_contract = dict(dependency)
            producer_roles = tuple(
                str(value) for value in dependency_contract.get("producers", ()))
            consumer_roles = tuple(
                str(value) for value in dependency_contract.get("consumers", ()))
            producers = tuple(sorted({
                provider for role in producer_roles
                for provider in providers_by_role.get(role, set())
            }))
            consumers = tuple(sorted({
                provider for role in consumer_roles
                for provider in providers_by_role.get(role, set())
            }))
            if not producers or not consumers:
                raise ValueError("V3 dependency references an unassigned role")
            members = tuple(sorted(set(producers) | set(consumers)))
            if len(members) < 2 or not any(
                    producer != consumer for producer in producers
                    for consumer in consumers):
                continue
            for member in members[1:]:
                union(members[0], member)
            layout_digest = canonical_digest(dependency_contract or {
                "producers": producer_roles,
                "consumers": consumer_roles,
                "tensors": tuple(dependency_contract.get("tensors", ())),
            })
            cross_dependencies.append(
                (index, dependency, producers, consumers, layout_digest,
                 dependency_contract))

        if not cross_dependencies:
            return {}, {}

        components: dict[str, set[str]] = {}
        for provider in parent:
            components.setdefault(find(provider), set()).add(provider)
        capability_by_provider: dict[str, str] = {}
        dependency_metadata: dict[int, Mapping[str, Any]] = {}
        remaining_ms = max(1, deadline_ms - int(time.time() * 1000))
        no_progress_ms = min(self.data_v1_no_progress_ms, remaining_ms)

        for component_index, members_set in enumerate(
                sorted(components.values(), key=lambda values: tuple(sorted(values)))):
            member_names = tuple(sorted(members_set))
            member_rank = {
                provider: rank for rank, provider in enumerate(member_names)}
            public_keys: dict[str, bytes] = {}
            members: list[GroupMemberV1] = []
            for provider in member_names:
                offer = provider_offers.get(provider)
                ack = provider_acks.get(provider)
                view = provider_views.get(provider)
                if offer is None or ack is None or view is None:
                    raise ValueError(
                        f"Provider {provider} is missing its ACK-bound key offer")
                public_key, endpoint_prefix = self._validated_data_v1_key_offer(
                    ack, offer)
                public_keys[provider] = public_key
                members.append(GroupMemberV1(
                    provider=provider, rank=member_rank[provider],
                    offer_digest=view.offer_digest,
                    endpoint_prefix=endpoint_prefix,
                ))

            operations: list[GroupOperationV1] = []
            for (index, dependency, producers, consumers, layout_digest,
                 dependency_contract) in (
                    cross_dependencies):
                involved = set(producers) | set(consumers)
                if not involved.issubset(members_set):
                    continue
                producer_ranks = tuple(str(member_rank[item]) for item in producers)
                consumer_ranks = tuple(str(member_rank[item]) for item in consumers)
                redistributions = tuple(
                    dependency_contract.get("redistributions", ()))
                operation_kind = str(dependency_contract.get(
                    "operationKind",
                    (redistributions[0].get("operation", "")
                     if redistributions else "PIPELINE"),
                ))
                operation_index = int(dependency_contract.get(
                    "collectiveOperationIndex", index))
                source_layout_digest = str(dependency_contract.get(
                    "collectiveSourceLayoutDigest", layout_digest))
                target_layout_digest = str(dependency_contract.get(
                    "collectiveTargetLayoutDigest", layout_digest))
                tensor_digest = str(dependency_contract.get(
                    "collectiveTensorDigest",
                    canonical_digest(tuple(
                        dependency_contract.get("tensors", ())))))
                epoch_count = (
                    generation_contract.max_generated_tokens + 1
                    if generation_contract is not None else 1)
                stride = (
                    generation_contract.streaming_operation_stride
                    if generation_contract is not None else 0)
                for epoch in range(epoch_count):
                    operations.append(GroupOperationV1(
                        operation_index=operation_index + epoch * stride,
                        kind=operation_kind,
                        producer_ranks=producer_ranks,
                        consumer_ranks=consumer_ranks,
                        tensor_layout_digest=canonical_digest({
                            "source": source_layout_digest,
                            "target": target_layout_digest,
                        }),
                        max_bytes=_DATA_V1_MAX_BYTES,
                        max_segments=_DATA_V1_MAX_SEGMENTS,
                    ))
                dependency_metadata[index] = MappingProxyType({
                    "transportProfile": "NDNSF_DATA_V1",
                    "collectiveOperationIndex": operation_index,
                    "collectiveProducerRank": str(dependency_contract.get(
                        "collectiveProducerRank", producer_ranks[0])),
                    "collectiveSourceLayoutDigest": source_layout_digest,
                    "collectiveTargetLayoutDigest": target_layout_digest,
                    "collectiveTensorDigest": tensor_digest,
                })

            group_id = "group-" + canonical_digest({
                "plan": plan_digest,
                "component": component_index,
                "members": member_names,
            })[7:39]
            capability = seal_group_capability_v1(
                request_id=request_id,
                attempt_id=f"attempt-{proposal.attempt}",
                plan_digest=plan_digest,
                group_id=group_id,
                epoch=1,
                ordered_members=members,
                permitted_operations=operations,
                max_inflight_bytes=_DATA_V1_MAX_BYTES,
                no_progress_ms=no_progress_ms,
                hard_deadline_ms=remaining_ms,
                wrap_epoch_key=lambda provider, key: self.group_epoch_key_wrapper(
                    public_keys[provider], key),
            )
            endpoint_prefixes = {
                item.provider: item.endpoint_prefix for item in members
            }
            for index, *_ in cross_dependencies:
                current = dependency_metadata.get(index)
                if current is None:
                    continue
                involved_providers = set()
                dependency = dependencies[index]
                for role in dependency.get("producers", ()):
                    involved_providers.update(
                        providers_by_role.get(str(role), set()))
                for role in dependency.get("consumers", ()):
                    involved_providers.update(
                        providers_by_role.get(str(role), set()))
                if not involved_providers.issubset(members_set):
                    continue
                dependency_metadata[index] = MappingProxyType({
                    **dict(current),
                    "groupId": group_id,
                    "groupEpoch": str(capability.epoch),
                    "noProgressMs": capability.no_progress_ms,
                    "hardDeadlineMs": capability.hard_deadline_ms,
                    "producerEndpointPrefixes": {
                        provider: endpoint_prefixes[provider]
                        for provider in sorted(involved_providers)
                    },
                })
            for provider in member_names:
                capability_by_provider[provider] = (
                    capability.project_for_provider(provider).to_bytes().hex())
        return capability_by_provider, dependency_metadata

    def _resolve_adapter(
        self,
        task: InferenceTaskRef,
        application_input: ApplicationInput,
        options: TaskOptions | None,
    ) -> ModelFamilyAdapter:
        adapter = self.adapters.get(task.adapter_name)
        if adapter is None:
            raise ValueError("model adapter is not allowlisted")
        adapter.validate_pin(
            adapter_descriptor_digest=task.adapter_descriptor_digest,
            composition_digest=task.adapter_composition_digest,
        )
        if (task.task_name != adapter.task.descriptor.task_name
                or task.task_descriptor_digest != canonical_contract_digest(
                    adapter.task.descriptor)):
            raise ValueError("inference task descriptor pin mismatch")
        if (application_input.task_name != task.task_name
                or application_input.input_schema_digest !=
                adapter.descriptor.input_schema_digest
                or application_input.options_schema_digest !=
                adapter.descriptor.options_schema_digest):
            raise ValueError("ApplicationInput was not validated by this adapter")
        if options is not None and options.schema_digest != (
                adapter.descriptor.options_schema_digest):
            raise ValueError("task options schema mismatch")
        return adapter

    def request(
        self,
        *,
        model: ModelRef,
        task: InferenceTaskRef,
        input: ApplicationInput,
        timeout_ms: int,
        options: TaskOptions | None = None,
        objective: Any = None,
        constraints: Mapping[str, Any] | None = None,
        request_id: str = "",
        generation_mode: str = "TOKEN_DIAGNOSTIC",
        strategy: ModelPlacementStrategy | None = None,
        stream_options: Any = None,
        on_stream_event: Callable[[bytes], None] | None = None,
        on_stream_complete: Callable[[bytes], None] | None = None,
        on_stream_error: Callable[[Mapping[str, Any]], None] | None = None,
        conversation: ConversationContinuation | None = None,
        _attempt: int = 1,
        _invocation_id: str = "",
        _deadline_ms: int = 0,
        _excluded_providers: tuple[str, ...] = (),
        _generation_recovery: GenerationRecoveryV1 | None = None,
    ) -> AutomaticInferenceHandle:
        request_started = time.perf_counter()
        timings: dict[str, float] = {}
        if timeout_ms <= self.ack_timeout_ms:
            raise ValueError("request timeout must exceed ACK collection")
        if int(_attempt) not in (1, 2):
            raise ValueError("streaming attempt must be 1 or 2")
        deadline_ms = int(_deadline_ms or (
            int(time.time() * 1000) + timeout_ms))
        if deadline_ms <= int(time.time() * 1000):
            raise TimeoutError("request deadline has expired")
        request_id = normalize_request_id_component(
            request_id or ("ndnsf-di-" + uuid.uuid4().hex))
        invocation_id = str(_invocation_id or (
            "invocation:" + canonical_digest({
                "request_id": request_id, "model": model.intent_digest,
            })[7:39]))
        excluded_providers = frozenset(
            str(value) for value in _excluded_providers)
        if any(not value.startswith("/") for value in excluded_providers):
            raise ValueError("excluded Provider identity must be an absolute name")
        if (_generation_recovery is not None
                and (_attempt != 2
                     or _generation_recovery.recovery_request_id != request_id)):
            raise ValueError("generation recovery is not request/attempt bound")
        # Validate the application/task contract before putting anything on
        # the wire, but deliberately defer model graph inspection and split
        # candidate enumeration.  The generic Request must reach Providers
        # first so their ACK metadata (capacity, cache residency, RTT, and
        # bandwidth) is part of the actual planning input.  This is the
        # request-driven default; PREPLANNED remains the explicit compatibility
        # path in the generic NDNSF API.
        adapter = self._resolve_adapter(task, input, options)
        active_strategy = strategy or self.strategy
        placement_profile = str(
            getattr(active_strategy, "placement_profile", "DI_PLACEMENT_V2"))
        if (str(generation_mode).upper() == "TOKEN_STREAMING"
                and placement_profile != DI_PLACEMENT_V3):
            raise ValueError(
                "Spec175 streamed invocation requires placement profile "
                f"{DI_PLACEMENT_V3}; V2 streamed plans are unsupported")
        if placement_profile == DI_PLACEMENT_V3:
            return self._request_v3(
                model=model, task=task, input=input, timeout_ms=timeout_ms,
                options=options, objective=objective, constraints=constraints,
                request_id=request_id, generation_mode=generation_mode,
                strategy=active_strategy, adapter=adapter,
                stream_options=stream_options,
                on_stream_event=on_stream_event,
                on_stream_complete=on_stream_complete,
                on_stream_error=on_stream_error,
                conversation=conversation,
                _attempt=_attempt,
                _invocation_id=invocation_id,
                _deadline_ms=deadline_ms,
                _excluded_providers=tuple(excluded_providers),
                _generation_recovery=_generation_recovery,
            )
        phase_started = time.perf_counter()
        request_payload = self._encode_request(
            model, task, input, options, deadline_ms, request_id,
            self.service_name, invocation_id, generation_mode,
            placement_profile=placement_profile, attempt=_attempt,
            generation_recovery=_generation_recovery,
            conversation=conversation)
        timings["request_encode_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        ack_close_policy = str((constraints or {}).get("ack_close_policy", "")).upper()
        ack_coverage_predicate = self.ack_coverage_predicate
        if ack_close_policy not in {"DEADLINE", "ACK_TIMEOUT"} \
                and ack_coverage_predicate is None and self.ack_coverage_roles:
            ack_coverage_predicate = AckRoleCoveragePolicy(
                required_roles=self.ack_coverage_roles,
                provider_view_factory=self.provider_view_factory,
                model_intent_digest=model.intent_digest,
                deadline_ms=deadline_ms,
            )
        if ack_coverage_predicate is not None and excluded_providers:
            base_coverage_predicate = ack_coverage_predicate
            ack_coverage_predicate = lambda candidates: base_coverage_predicate(
                tuple(candidate for candidate in candidates
                      if str(getattr(candidate, "provider_name", ""))
                      not in excluded_providers))
        collaboration_kwargs = dict(
            mode="DEFERRED",
            ack_timeout_ms=self.ack_timeout_ms,
            timeout_ms=timeout_ms,
            request_id=request_id,
            fail_fast_terminal_selection=True,
        )
        if ack_coverage_predicate is not None:
            collaboration_kwargs["ack_coverage_predicate"] = ack_coverage_predicate
        if stream_options is not None:
            collaboration_kwargs.update({
                "stream_options": stream_options,
                "on_stream_event": on_stream_event,
                "on_stream_complete": on_stream_complete,
                "on_stream_error": on_stream_error,
            })
        collaboration = self.service_user.begin_collaboration(
            self.service_name,
            request_payload,
            **collaboration_kwargs,
        )
        print(
            "NDNSF_DI_AUTOPLANNING_REQUEST_SENT",
            f"requestId={request_id}",
            "mode=DEFERRED",
            flush=True,
        )
        self._emit_lifecycle(
            "REQUEST_SENT", request_id=request_id, attempt=_attempt,
            requestDigest="sha256:" + hashlib.sha256(
                request_payload).hexdigest())
        timings["request_publish_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        closed = collaboration.acks_closed()
        self._validate_ack_closed_binding(closed, request_id)
        print(
            "NDNSF_DI_AUTOPLANNING_ACK_CLOSED",
            f"requestId={request_id}",
            f"ackCount={len(closed.candidates)}",
            flush=True,
        )
        self._emit_lifecycle(
            "ACK_CLOSED", request_id=request_id, attempt=_attempt,
            ackSnapshotDigest=str(closed.digest),
            ackCount=len(closed.candidates))
        timings["ack_collect_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        providers = tuple(
            self.provider_view_factory(
                ack, model.intent_digest, deadline_ms)
            for ack in closed.candidates
            if ack.status and str(getattr(ack, "provider_name", ""))
            not in excluded_providers
        )
        if not providers:
            raise ValueError("ACK_CLOSED contains no valid DI Provider offer")

        phase_started = time.perf_counter()
        model_descriptor = adapter.describe_model(
            model.model_name,
            model.content_digest,
            model.semantics_digest,
            source_revision=model.source_revision or "",
        )
        graph = adapter.graph.inspect(model_descriptor)
        if (graph is None
                or not str(getattr(graph, "graph_digest", "")).startswith(
                    "sha256:")):
            raise ValueError(
                "post-ACK planning requires an immutable dependency graph snapshot")
        candidates = adapter.splitter.enumerate_candidates(
            model_descriptor, graph)
        timings["adapter_graph_split_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        if not candidates or len(candidates) > self.budget.max_candidates:
            raise ValueError("adapter returned an invalid candidate set")
        print(
            "NDNSF_DI_AUTOPLANNING_GRAPH_READY",
            f"requestId={request_id}",
            f"graphDigest={graph.graph_digest}",
            f"candidateCount={len(candidates)}",
            "after=ACK_CLOSED",
            flush=True,
        )
        self._emit_lifecycle(
            "GRAPH_READY", request_id=request_id, attempt=_attempt,
            graphDigest=str(graph.graph_digest))
        candidate_role_sets = tuple(
            tuple(candidate.execution_plan.roles) for candidate in candidates)
        if any(role_set != candidate_role_sets[0]
               for role_set in candidate_role_sets[1:]):
            raise ValueError(
                "V2 automatic planning requires candidate-local evaluation; "
                "use the V3 placement profile")
        placement = PlacementRequest(
            request_id=collaboration.request_id,
            attempt=_attempt,
            deadline_ms=deadline_ms,
            model_digest=model_descriptor.model_digest,
            graph_digest=graph.graph_digest,
            candidate_ids=tuple(
                candidate.candidate_digest for candidate in candidates),
            providers=providers,
            required_roles=candidate_role_sets[0],
            budget=self.budget,
            objective=objective,
            constraints={
                **dict(constraints or {}),
                **({"generation_recovery_digest":
                    _generation_recovery.digest()}
                   if _generation_recovery is not None else {}),
                **({"excluded_providers": tuple(sorted(excluded_providers))}
                   if excluded_providers else {}),
            },
            catalog_snapshot=tuple(self.catalog_snapshot_provider()),
            task_digest=task.task_descriptor_digest,
            state_contracts=adapter.state.contracts,
            model=model_descriptor,
            graph=graph,
            candidates=candidates,
        )
        print(
            "NDNSF_DI_AUTOPLANNING_PLACEMENT_INPUT",
            f"requestId={request_id}",
            f"catalogCount={len(placement.catalog_snapshot)}",
            f"providerCount={len(placement.providers)}",
            f"candidateCount={len(placement.candidates)}",
            flush=True,
        )
        phase_started = time.perf_counter()
        if active_strategy is None:
            raise ValueError("a placement strategy is required after ACK_CLOSED")
        strategy_identity_digest = canonical_digest({
            "name": active_strategy.name,
            "version": active_strategy.version,
            "state_digest": active_strategy.state_digest,
        })
        decision = evaluate_placement_strategy(active_strategy, placement)
        timings["placement_strategy_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        print(
            "NDNSF_DI_AUTOPLANNING_DECISION",
            f"requestId={request_id}",
            f"preparation={decision.artifact_preparation.value}",
            f"splitId={decision.split_id}",
            f"catalogCount={len(placement.catalog_snapshot)}",
            flush=True,
        )
        selected_for_trace = next(
            (item for item in candidates
             if item.candidate_digest == decision.split_id), None)
        if selected_for_trace is not None:
            self._emit_lifecycle(
                "PLACEMENT_DECISION", request_id=request_id, attempt=_attempt,
                candidateId=str(getattr(selected_for_trace, "candidate_id", "")
                                or decision.split_id),
                candidateDigest=str(decision.split_id),
                candidatePriority=int(getattr(
                    selected_for_trace, "selection_priority", 0)),
                providerCount=len(providers))
        if (decision.artifact_preparation is
                ArtifactPreparationMode.REUSE_CACHED
                and not placement.catalog_snapshot):
            # A Provider ACK may describe bytes retained from an older Repo
            # process, but without an ACTIVE catalog this coordinator cannot
            # resolve the content-addressed Data names required by Selection.
            # Fail closed to the first-request publication path instead of
            # turning an unresolvable cache hint into a request timeout.
            evidence = dict(decision.evidence)
            evidence["cache_resolution"] = {
                "status": "UNRESOLVABLE_WITHOUT_ACTIVE_CATALOG",
                "action": "PUBLISH_SELECTED_CANDIDATE",
            }
            decision = replace(
                decision,
                artifact_preparation=ArtifactPreparationMode.GENERATED,
                evidence=evidence,
                evidence_digest=canonical_digest(evidence),
            )
            print(
                "NDNSF_DI_AUTOPLANNING_CACHE_DOWNGRADE",
                f"requestId={request_id}",
                "from=REUSE_CACHED",
                "to=GENERATED",
                "reason=UNRESOLVABLE_WITHOUT_ACTIVE_CATALOG",
                flush=True,
            )
        print(
            "NDNSF_DI_AUTOPLANNING_CANDIDATE_RESOLVE_BEGIN",
            f"requestId={request_id}",
            f"candidateCount={len(candidates)}",
            f"splitId={decision.split_id}",
            flush=True,
        )
        try:
            candidate = next(
                (item for item in candidates
                 if item.candidate_digest == decision.split_id),
                None,
            )
        except Exception as exc:
            print(
                "NDNSF_DI_AUTOPLANNING_CANDIDATE_RESOLVE_FAILED",
                f"requestId={request_id}",
                f"errorType={type(exc).__name__}",
                f"error={exc}",
                flush=True,
            )
            raise
        if candidate is None:
            raise ValueError("placement selected an unknown candidate")
        print(
            "NDNSF_DI_AUTOPLANNING_CANDIDATE_RESOLVE_DONE",
            f"requestId={request_id}",
            f"candidateDigest={candidate.candidate_digest}",
            flush=True,
        )
        print(
            "NDNSF_DI_AUTOPLANNING_ARTIFACTS_BEGIN",
            f"requestId={request_id}",
            f"preparation={decision.artifact_preparation.value}",
            f"materializer={type(self.split_materializer).__module__}.{type(self.split_materializer).__qualname__}",
            f"publisher={type(self.artifact_publisher).__module__}.{type(self.artifact_publisher).__qualname__}",
            flush=True,
        )
        phase_started = time.perf_counter()
        try:
            published = self._prepare_artifacts(
                candidate, decision.artifact_preparation, deadline_ms)
        except Exception as exc:
            print(
                "NDNSF_DI_AUTOPLANNING_ARTIFACTS_FAILED",
                f"requestId={request_id}",
                f"preparation={decision.artifact_preparation.value}",
                f"errorType={type(exc).__name__}",
                f"error={exc}",
                flush=True,
            )
            raise
        print(
            "NDNSF_DI_AUTOPLANNING_ARTIFACTS_READY",
            f"requestId={request_id}",
            f"candidateDigest={candidate.candidate_digest}",
            f"preparation={decision.artifact_preparation.value}",
            flush=True,
        )
        self._emit_lifecycle(
            "ARTIFACTS_READY", request_id=request_id, attempt=_attempt,
            artifactDigest=canonical_digest(
                dict(published.artifact_digests_by_role)),
            artifactCount=len(published.artifact_digests_by_role))
        timings["artifact_resolve_publish_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        sealed = self._seal(
            closed.digest, placement, decision, candidate, published,
            invocation_id, strategy_identity_digest,
            generation_mode=generation_mode,
            generation_recovery=_generation_recovery)
        timings["plan_seal_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        scope_key_data_names = self._publish_scope_keys(
            sealed.key_scopes, deadline_ms)
        sealed = replace(
            sealed,
            scope_key_data_names=scope_key_data_names,
        )
        self._emit_lifecycle(
            "PLAN_SEALED", request_id=request_id, attempt=_attempt,
            planDigest=str(sealed.plan_digest))
        # Bind conversation metadata to the final post-key-publication plan.
        # The V2 path has no local ``plan_digest`` variable; using the digest
        # computed from this sealed object also prevents authenticating a
        # continuation against a different plan.
        plan_digest = sealed.plan_digest
        timings["scope_key_publish_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        phase_started = time.perf_counter()
        collaboration.commit_plan(
            ack_closed_digest=closed.digest,
            roles=list(sealed.roles),
            key_scopes={
                key: list(value) for key, value in sealed.key_scopes.items()
            },
            dependencies=list(sealed.dependencies),
            artifact_data_names=dict(sealed.artifact_data_names),
            scope_key_data_names=dict(sealed.scope_key_data_names),
            role_scopes={
                key: list(value) for key, value in sealed.role_scopes.items()
            },
            role_provider_assignments=dict(sealed.providers_by_role),
            assignment_payloads_by_role=dict(
                sealed.assignment_payloads_by_role),
        )
        print(
            "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED",
            f"requestId={request_id}",
            f"candidateDigest={candidate.candidate_digest}",
            flush=True,
        )
        self._emit_lifecycle(
            "SELECTION_COMMITTED", request_id=request_id, attempt=_attempt,
            selectionDigest=canonical_digest({
                "plan": sealed.plan_digest,
                "ack": closed.digest,
            }),
            selectedRoleCount=len(sealed.roles))
        timings["selection_commit_ms"] = (
            time.perf_counter() - phase_started) * 1000.0
        timings["pre_response_setup_total_ms"] = (
            time.perf_counter() - request_started) * 1000.0
        return AutomaticInferenceHandle(
            collaboration, decision, sealed, adapter, timings, invocation_id,
            service_user=self.service_user,
            conversation_metadata={
                "model_contract_digest": model.intent_digest,
                "tokenizer_digest": model.semantics_digest,
                "chat_template_digest": str(
                    input.metadata.get("chat_template_digest",
                                      input.metadata.get("chatTemplateDigest",
                                                         model.semantics_digest))),
                "application_messages": bytes(input.payload),
                "service_name": self.service_name,
                "plan_digest": plan_digest,
                "plan_role_map_digest": canonical_digest(tuple(sorted(
                    (str(role), str(provider))
                    for role, provider in sealed.providers_by_role.items()))),
            })

    def request_streaming(
        self,
        *,
        model: ModelRef,
        task: InferenceTaskRef,
        input: ApplicationInput,
        timeout_ms: int,
        options: TaskOptions | None = None,
        stream_options: Any = None,
        on_event: Callable[[bytes], None] | None = None,
        on_complete: Callable[[bytes], None] | None = None,
        on_error: Callable[[Mapping[str, Any]], None] | None = None,
        conversation: ConversationContinuation | None = None,
        objective: Any = None,
        constraints: Mapping[str, Any] | None = None,
        request_id: str = "",
        strategy: ModelPlacementStrategy | None = None,
    ) -> AutomaticStreamingHandle:
        """Expose one logical stream with one optional fresh Normal recovery."""
        active_strategy = strategy or getattr(self, "strategy", None)
        if (active_strategy is not None
                and str(getattr(active_strategy, "placement_profile", ""))
                != DI_PLACEMENT_V3):
            raise ValueError(
                "Spec175 streamed invocation requires placement profile "
                f"{DI_PLACEMENT_V3}; V2 streamed plans are unsupported")
        if not callable(on_event) or not callable(on_complete) or not callable(on_error):
            raise TypeError("streaming requests require on_event/on_complete/on_error")
        if conversation is not None and not isinstance(conversation, ConversationContinuation):
            raise TypeError("conversation must be ConversationContinuation")
        if conversation is not None:
            # Validate continuation metadata before the first Request reaches
            # ACK collection; omission preserves the existing FULL_CONTEXT path.
            input_digest = "sha256:" + hashlib.sha256(bytes(input.payload)).hexdigest()
            if conversation.turn_input_digest and conversation.turn_input_digest != input_digest:
                raise ValueError("conversation turn input digest mismatch")
            expected_contract = conversation.request_contract(input_digest=input_digest)
            if conversation.request_contract_digest and conversation.request_contract_digest != expected_contract:
                raise ValueError("conversation request contract digest mismatch")
        if stream_options is None:
            from ndnsf import StreamedInvocationOptions
            stream_options = StreamedInvocationOptions()
        raw_options = (stream_options.as_dict()
                       if callable(getattr(stream_options, "as_dict", None))
                       else dict(stream_options))
        mode = str(raw_options.get("mode", "Normal"))
        allow_replacement = bool(raw_options.get("allow_replacement", False))
        max_replacements = int(raw_options.get("max_replacements", 0))
        if int(raw_options.get("attempt_epoch", 1)) != 1:
            raise ValueError("application streaming starts at attempt epoch 1")
        if mode.lower() != "normal" and allow_replacement:
            raise ValueError("Targeted streamed replacement is unsupported")
        if allow_replacement != (max_replacements == 1):
            raise ValueError(
                "replacement must be either disabled/0 or explicitly enabled/1")

        logical_request_id = normalize_request_id_component(
            request_id or ("ndnsf-di-" + uuid.uuid4().hex))
        generation_id = str(raw_options.get("generation_id", "")) or secrets.token_hex(16)
        if (len(generation_id) != 32
                or any(ch not in "0123456789abcdef" for ch in generation_id)):
            raise ValueError("stream generation identity must be 16-byte lowercase hex")
        invocation_id = "invocation:" + canonical_digest({
            "request_id": logical_request_id,
            "model": model.intent_digest,
            "generation_id": generation_id,
        })[7:39]
        absolute_deadline_ms = int(time.time() * 1000) + int(timeout_ms)
        handle = AutomaticStreamingHandle(
            None, stream_options,
            logical_request_id=logical_request_id,
            generation_id=generation_id,
            deadline_ms=absolute_deadline_ms,
            on_event=on_event,
            on_complete=on_complete,
            on_error=on_error,
            conversation_expected=conversation is not None,
        )

        def attempt_options(attempt: int):
            values = dict(raw_options)
            values.update({
                "attempt_epoch": attempt,
                "generation_id": generation_id,
                "stream_epoch": attempt,
                # The public coordinator, not Core, owns the only recovery.
                "allow_replacement": allow_replacement and attempt == 1,
                "max_replacements": 1 if allow_replacement and attempt == 1 else 0,
            })
            if callable(getattr(stream_options, "as_dict", None)):
                return replace(stream_options, **{
                    key: value for key, value in values.items()
                    if hasattr(stream_options, key)
                })
            return values

        def submit_attempt(
            attempt: int,
            attempt_request_id: str,
            recovery: GenerationRecoveryV1 | None = None,
            excluded: tuple[str, ...] = (),
        ) -> AutomaticInferenceHandle:
            remaining_ms = absolute_deadline_ms - int(time.time() * 1000)
            if remaining_ms <= self.ack_timeout_ms:
                raise TimeoutError(
                    "insufficient original deadline for a fresh ACK closure")
            return self.request(
                model=model,
                task=task,
                input=input,
                timeout_ms=remaining_ms,
                options=options,
                objective=objective,
                constraints=constraints,
                request_id=attempt_request_id,
                generation_mode="TOKEN_STREAMING",
                strategy=strategy,
                stream_options=attempt_options(attempt),
                conversation=conversation,
                on_stream_event=lambda payload: handle._accept_event(
                    attempt, payload),
                on_stream_complete=lambda payload: handle._complete(
                    attempt, payload),
                on_stream_error=lambda error: handle_attempt_error(
                    attempt, error),
                _attempt=attempt,
                _invocation_id=invocation_id,
                _deadline_ms=absolute_deadline_ms,
                _excluded_providers=excluded,
                _generation_recovery=recovery,
            )

        def run_replacement(
            prefix: tuple[int, ...], original_error: Mapping[str, Any],
        ) -> None:
            try:
                first = handle._wait_attempt(1, absolute_deadline_ms)
                failed_provider = str(
                    original_error.get("providerName", ""))
                if not failed_provider.startswith("/"):
                    raise ValueError(
                        "failed Provider identity is unavailable for replacement")
                recovery_request_id = normalize_request_id_component(
                    logical_request_id + "-recovery-" + uuid.uuid4().hex)
                recovery = GenerationRecoveryV1(
                    logical_generation_id=generation_id,
                    original_request_id=logical_request_id,
                    recovery_request_id=recovery_request_id,
                    attempt=2,
                    original_input_manifest_digest=(
                        self._request_input_manifest_digest(input, options)),
                    prior_plan_digest=first.sealed_plan.plan_digest,
                    failed_provider=failed_provider,
                    committed_token_ids=prefix,
                    committed_token_count=len(prefix),
                    committed_prefix_digest="sha256:" + hashlib.sha256(
                        ",".join(str(value) for value in prefix).encode("ascii")
                    ).hexdigest(),
                )
                second = submit_attempt(
                    2, recovery_request_id, recovery, (failed_provider,))
                handle._bind_attempt(2, second)
            except Exception as exc:
                handle._fail(2, {
                    "code": "ReplacementUnavailable",
                    "message": str(exc),
                    "cause": dict(original_error),
                })

        def handle_attempt_error(
            attempt: int, error: Mapping[str, Any],
        ) -> None:
            value = dict(error)
            recoverable_codes = {6, 7, 17}  # gap timeout, retention, Provider failure
            failed_provider = str(value.get("providerName", ""))
            if (attempt == 1 and allow_replacement
                    and value.get("code") in recoverable_codes
                    and failed_provider.startswith("/")):
                prefix = handle._begin_replacement(1)
                if prefix is not None:
                    threading.Thread(
                        target=run_replacement,
                        args=(prefix, value),
                        name="ndnsf-di-stream-replacement",
                        daemon=True,
                    ).start()
                    return
            handle._fail(attempt, value)

        first = submit_attempt(1, logical_request_id)
        handle._bind_attempt(1, first)
        return handle

    @staticmethod
    def _generation_execution_contract_v1(
        *,
        generation_mode: str,
        application_input: ApplicationInput,
        options: TaskOptions | None,
        generation_id: str,
        role_specs: tuple[RoleAssemblySpec, ...],
        streaming_operation_stride: int,
        generation_recovery: GenerationRecoveryV1 | None,
    ) -> GenerationExecutionContractV1 | None:
        if str(generation_mode).upper() != "TOKEN_STREAMING":
            return None
        options_payload = (
            options.payload if options is not None else application_input.options)
        try:
            values = json.loads(bytes(options_payload).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError(
                "TOKEN_STREAMING options must be canonical JSON") from exc
        if (not isinstance(values, dict)
                or values.get("useCache") is not True
                or str(values.get("outputMode", "")).upper()
                != "TOKEN_STREAMING"):
            raise ValueError(
                "TOKEN_STREAMING requires useCache=true and outputMode="
                "TOKEN_STREAMING")
        generation_id = str(generation_id or values.get("generationId", "")
                            or values.get("generation_id", ""))
        if (len(generation_id) != 32
                or generation_id != generation_id.lower()
                or any(ch not in "0123456789abcdef" for ch in generation_id)):
            raise ValueError(
                "TOKEN_STREAMING requires the request generation identity")
        try:
            max_generated_tokens = int(values["maxNewTokens"])
            eos_token_ids = tuple(int(value) for value in values["eosTokenIds"])
            tokenizer_digest = str(values["tokenizerDigest"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError(
                "TOKEN_STREAMING maxNewTokens/eosTokenIds/tokenizerDigest are incomplete") from exc

        raw_sampling = values.get("sampling", {})
        if raw_sampling is None:
            raw_sampling = {}
        if not isinstance(raw_sampling, dict):
            raise ValueError("TOKEN_STREAMING sampling must be an object")
        mode_value = raw_sampling.get(
            "mode", values.get("samplingMode", ""))
        if not mode_value:
            mode_value = "Greedy" if values.get("greedy", True) else "SeededTopKTopP"
        normalized_mode = str(mode_value).strip().lower().replace("-", "").replace("_", "")
        if normalized_mode == "greedy":
            sampling_mode = "Greedy"
        elif normalized_mode in {"seededtopktopp", "topktopp", "topkp"}:
            sampling_mode = "SeededTopKTopP"
        else:
            sampling_mode = str(mode_value).strip()
        def sampling_value(name: str, default: Any, *aliases: str) -> Any:
            for key in (name, *aliases):
                if key in raw_sampling:
                    return raw_sampling[key]
                if key in values:
                    return values[key]
            return default
        try:
            sampling_temperature = float(sampling_value(
                "temperature", 0.0 if sampling_mode == "Greedy" else 1.0))
            sampling_top_k = int(sampling_value("topK", 1, "top_k"))
            sampling_top_p = float(sampling_value("topP", 1.0, "top_p"))
            sampling_repetition_penalty = float(sampling_value(
                "repetitionPenalty", 1.0, "repetition_penalty"))
            sampling_seed = int(sampling_value("seed", 1_750_001))
        except (TypeError, ValueError) as exc:
            raise ValueError("TOKEN_STREAMING sampling parameters are invalid") from exc
        stop_values = sampling_value("stopStrings", (), "stop_strings")
        if isinstance(stop_values, str) or stop_values is None:
            raise ValueError("TOKEN_STREAMING stopStrings must be an array")
        try:
            stop_strings = tuple(str(value) for value in stop_values)
        except TypeError as exc:
            raise ValueError("TOKEN_STREAMING stopStrings must be an array") from exc

        token_input_name = str(values.get("tokenInputName", "input_ids"))
        state_input_names = tuple(str(value) for value in values.get(
            "stateInputNames", (
                "attention_kv_in",
                "recurrent_state_in",
                "convolution_state_in",
            )))
        state_output_names = tuple(str(value) for value in values.get(
            "stateOutputNames", (
                "attention_kv_out",
                "recurrent_state_out",
                "convolution_state_out",
            )))
        if not role_specs:
            raise ValueError("TOKEN_STREAMING requires at least one role")
        role_input_names = tuple(
            {str(item.get("name", "")) for item in spec.expected_inputs}
            for spec in role_specs)
        role_output_names = tuple(
            {str(item.get("name", "")) for item in spec.expected_outputs}
            for spec in role_specs)
        if token_input_name not in role_input_names[0]:
            raise ValueError(
                "TOKEN_STREAMING first role omits the sealed token input")
        for index, (inputs, outputs) in enumerate(zip(
                role_input_names, role_output_names)):
            if (not set(state_input_names).issubset(inputs)
                    or not set(state_output_names).issubset(outputs)):
                raise ValueError(
                    "TOKEN_STREAMING role state I/O is incomplete at index "
                    f"{index}")
        committed_prefix = (
            tuple(generation_recovery.committed_token_ids)
            if generation_recovery is not None else ())
        return GenerationExecutionContractV1(
            mode="TOKEN_STREAMING",
            max_generated_tokens=max_generated_tokens,
            token_input_name=token_input_name,
            state_input_names=state_input_names,
            state_output_names=state_output_names,
            eos_token_ids=eos_token_ids,
            sampling_digest=canonical_digest({
                "mode": sampling_mode,
                "temperature": sampling_temperature,
                "topK": sampling_top_k,
                "topP": sampling_top_p,
                "repetitionPenalty": sampling_repetition_penalty,
                "seed": sampling_seed,
            }),
            tokenizer_digest=tokenizer_digest,
            sampling_mode=sampling_mode,
            sampling_temperature=sampling_temperature,
            sampling_top_k=sampling_top_k,
            sampling_top_p=sampling_top_p,
            sampling_repetition_penalty=sampling_repetition_penalty,
            sampling_seed=sampling_seed,
            stop_strings=stop_strings,
            generation_id=generation_id,
            committed_prefix_token_ids=committed_prefix,
            streaming_operation_stride=streaming_operation_stride,
        )

    def _request_v3(
        self,
        *,
        model: ModelRef,
        task: InferenceTaskRef,
        input: ApplicationInput,
        timeout_ms: int,
        options: TaskOptions | None,
        objective: Any,
        constraints: Mapping[str, Any] | None,
        request_id: str,
        generation_mode: str,
        strategy: ModelPlacementStrategy,
        adapter: ModelFamilyAdapter,
        stream_options: Any = None,
        on_stream_event: Callable[[bytes], None] | None = None,
        on_stream_complete: Callable[[bytes], None] | None = None,
        on_stream_error: Callable[[Mapping[str, Any]], None] | None = None,
        conversation: ConversationContinuation | None = None,
        _attempt: int = 1,
        _invocation_id: str = "",
        _deadline_ms: int = 0,
        _excluded_providers: tuple[str, ...] = (),
        _generation_recovery: GenerationRecoveryV1 | None = None,
    ) -> AutomaticInferenceHandle:
        """Run the real V3 request/ACK/plan/Selection composition.

        V3 never uses the V2 role-split preparation port.  If the adapter
        supplies a canonical ensurer, it is invoked only after ACK_CLOSED and
        graph planning, before Selection is sealed.  Providers still own local
        fetch/assembly/load after Selection; the requester merely publishes or
        resolves the immutable canonical layer identities.
        """

        request_started = time.perf_counter()
        timings: dict[str, float] = {}
        if timeout_ms <= self.ack_timeout_ms:
            raise ValueError("request timeout must exceed ACK collection")
        deadline_ms = int(_deadline_ms or (
            int(time.time() * 1000) + int(timeout_ms)))
        request_id = normalize_request_id_component(
            request_id or ("ndnsf-di-" + uuid.uuid4().hex))
        invocation_id = str(_invocation_id or (
            "invocation:" + canonical_digest({
                "request_id": request_id, "model": model.intent_digest,
            })[7:39]))
        excluded_providers = frozenset(_excluded_providers)
        request_payload = self._encode_request(
            model, task, input, options, deadline_ms, request_id,
            self.service_name, invocation_id, generation_mode,
            placement_profile=DI_PLACEMENT_V3, attempt=_attempt,
            generation_recovery=_generation_recovery,
            conversation=conversation)
        ack_close_policy = str((constraints or {}).get("ack_close_policy", "")).upper()
        ack_coverage_predicate = self.ack_coverage_predicate
        if ack_close_policy not in {"DEADLINE", "ACK_TIMEOUT"} \
                and ack_coverage_predicate is None and self.ack_coverage_roles:
            ack_coverage_predicate = AckRoleCoveragePolicy(
                required_roles=self.ack_coverage_roles,
                provider_view_factory=self.provider_view_factory,
                model_intent_digest=model.intent_digest,
                deadline_ms=deadline_ms,
            )
        if ack_coverage_predicate is not None and excluded_providers:
            base_coverage_predicate = ack_coverage_predicate
            ack_coverage_predicate = lambda candidates: base_coverage_predicate(
                tuple(candidate for candidate in candidates
                      if str(getattr(candidate, "provider_name", ""))
                      not in excluded_providers))
        collaboration = self.service_user.begin_collaboration(
            self.service_name, request_payload, mode="DEFERRED",
            ack_timeout_ms=self.ack_timeout_ms, timeout_ms=timeout_ms,
            request_id=request_id, fail_fast_terminal_selection=True,
            request_capabilities={"NDNSF_DATA_V1": "required"},
            **({"ack_coverage_predicate": ack_coverage_predicate}
               if ack_coverage_predicate is not None else {}),
            **({"stream_options": stream_options,
                "on_stream_event": on_stream_event,
                "on_stream_complete": on_stream_complete,
                "on_stream_error": on_stream_error}
               if stream_options is not None else {}),
        )
        print(
            "NDNSF_DI_AUTOPLANNING_REQUEST_SENT",
            f"requestId={request_id}", "mode=DEFERRED", "placement=V3",
            flush=True,
        )
        self._emit_lifecycle(
            "REQUEST_SENT", request_id=request_id, attempt=_attempt,
            requestDigest="sha256:" + hashlib.sha256(
                request_payload).hexdigest())
        closed = collaboration.acks_closed()
        self._validate_ack_closed_binding(closed, request_id)
        print(
            "NDNSF_DI_AUTOPLANNING_ACK_CLOSED",
            f"requestId={request_id}", f"ackCount={len(closed.candidates)}",
            "placement=V3", flush=True,
        )
        self._emit_lifecycle(
            "ACK_CLOSED", request_id=request_id, attempt=_attempt,
            ackSnapshotDigest=str(closed.digest),
            ackCount=len(closed.candidates))

        descriptor = adapter.describe_model(
            model.model_name, model.content_digest, model.semantics_digest,
            source_revision=model.source_revision or "",
        )
        graph = adapter.graph.inspect(descriptor)
        if graph is None or not str(getattr(graph, "graph_digest", "")).startswith(
                "sha256:"):
            raise ValueError("V3 planning requires an immutable graph snapshot")
        candidates = tuple(adapter.splitter.enumerate_candidates(descriptor, graph))
        if not candidates or len(candidates) > self.budget.max_candidates:
            raise ValueError("adapter returned an invalid V3 candidate set")
        self._emit_lifecycle(
            "GRAPH_READY", request_id=request_id, attempt=_attempt,
            graphDigest=str(graph.graph_digest))

        providers: list[ProviderPlanningViewV3] = []
        provider_offers: dict[str, ProviderOfferV3] = {}
        provider_acks: dict[str, Any] = {}
        for ack in tuple(closed.candidates):
            if not bool(getattr(ack, "status", False)):
                continue
            try:
                view = self.provider_view_factory(
                    ack, model.intent_digest, deadline_ms, graph.graph_digest)
            except TypeError as exc:
                raise TypeError(
                    "V3 provider view factory must accept graph_digest") from exc
            if not isinstance(view, ProviderPlanningViewV3):
                raise TypeError("V3 ACK did not produce ProviderPlanningViewV3")
            if view.provider in excluded_providers:
                continue
            offer = ProviderOfferV3.from_bytes(bytes(ack.payload))
            if offer.provider != view.provider or view.provider in provider_acks:
                raise ValueError("V3 ACK Provider identity is ambiguous")
            providers.append(view)
            provider_offers[view.provider] = offer
            provider_acks[view.provider] = ack
        if not providers:
            raise ValueError("ACK_CLOSED contains no valid V3 Provider offer")

        # Try graph-derived candidates in the adapter's signed preference
        # order.  Feasibility is still decided by the strategy for each
        # candidate independently; cache residency and estimated cost are not
        # allowed to override the signed candidate priority or make catalogue
        # order authoritative.
        def candidate_order(item: SplitCandidate):
            return _v3_candidate_priority_key(item)

        proposal: PlacementProposalV3 | None = None
        selected_candidate: SplitCandidate | None = None
        candidate_rejections: list[str] = []
        for candidate in sorted(candidates, key=candidate_order):
            role_specs = tuple(self._v3_role_specs(
                candidate, graph, protection_epoch=self.protection_epoch))
            try:
                candidate_proposal = strategy.propose_v3(
                    request_id=collaboration.request_id,
                    attempt=_attempt,
                    model_digest=descriptor.model_digest,
                    graph_digest=graph.graph_digest,
                    roles=role_specs,
                    providers=tuple(providers),
                    ack_closed_digest=closed.digest,
                )
            except (AttributeError, TypeError, ValueError, RuntimeError) as exc:
                # Keep the strategy fail-closed, but do not erase the
                # contract-level reason needed to diagnose a real deployment
                # mismatch.  The message contains only exception type/text;
                # no model bytes or Provider secrets are included.
                candidate_rejections.append(
                    f"{candidate.candidate_digest}:{type(exc).__name__}:{exc}")
                continue
            if not isinstance(candidate_proposal, PlacementProposalV3):
                raise TypeError("V3 strategy returned a non-proposal")
            validate_spec175_ordinary_v3_proposal(
                candidate_proposal,
                required_roles=tuple(candidate.execution_plan.roles),
                tensor_degrees_by_role=candidate.tensor_degrees_by_role,
                hybrid_plan=candidate.hybrid_plan,
                placement_profile=str(getattr(
                    strategy, "placement_profile", "")),
            )
            proposal = replace(
                candidate_proposal,
                candidate_digest=(candidate_proposal.candidate_digest
                                  or candidate.candidate_digest),
            )
            selected_candidate = candidate
            break
        if proposal is None or selected_candidate is None:
            detail = "; ".join(candidate_rejections[-4:])
            raise ValueError(
                "V3 strategy found no feasible graph candidate"
                + (f" ({detail})" if detail else ""))
        self._emit_lifecycle(
            "PLACEMENT_DECISION", request_id=request_id, attempt=_attempt,
            candidateId=str(selected_candidate.candidate_digest),
            candidateDigest=str(selected_candidate.candidate_digest),
            candidatePriority=int(getattr(
                selected_candidate, "selection_priority", 0)),
            providerCount=len(providers))

        # The placement strategy chooses feasible Provider ownership from the
        # ACK set using draft role requirements.  Certify once from the
        # ensurer's current binding before invoking it; this preserves the
        # existing ensurer contract and lets implementations validate the
        # exact recipe they receive.  A request-scoped publisher may refine
        # the model-manifest digest after publication; that binding is
        # re-applied immediately below before Selection is sealed.
        canonical_published: PublishedSplit | None = None
        v3_artifact_preparation: ArtifactPreparationMode | None = None
        catalog_snapshot: tuple[Any, ...] = ()
        catalog_snapshot_loaded = False
        if (self.canonical_artifact_ensurer is not None and
                callable(getattr(self.canonical_artifact_ensurer, "describe", None))):
            binding = self.canonical_artifact_ensurer.describe(selected_candidate)
            proposal = replace(
                proposal,
                roles=self._certify_v3_role_specs(
                    selected_candidate, graph, proposal.roles, binding),
            )

        if self.canonical_artifact_ensurer is not None:
            print(
                "NDNSF_DI_CANONICAL_ENSURE_START",
                f"requestId={request_id}",
                f"candidateDigest={selected_candidate.candidate_digest}",
                flush=True,
            )
            canonical_published = self.canonical_artifact_ensurer.ensure(
                selected_candidate, tuple(proposal.roles), deadline_ms=deadline_ms)
            self._validate_published_split(
                selected_candidate, canonical_published)
            print(
                "NDNSF_DI_CANONICAL_ENSURE_DONE",
                f"requestId={request_id}",
                f"candidateDigest={selected_candidate.candidate_digest}",
                flush=True,
            )
            if callable(getattr(self.canonical_artifact_ensurer, "describe", None)):
                binding = self.canonical_artifact_ensurer.describe(selected_candidate)
                proposal = replace(
                    proposal,
                    roles=self._certify_v3_role_specs(
                        selected_candidate, graph, proposal.roles, binding),
                )

        # The maintained YOLO path supplies a signed APP-data snapshot and
        # the default CatalogSnapshotArtifactPublisher.  Resolve that
        # candidate-bound publication here, after ACK_CLOSED and candidate
        # selection but before the sealed role artifacts are constructed.  The
        # previous V3 path skipped _prepare_artifacts and synthesized names
        # even though sealed roles disabled dynamic provisioning, yielding a
        # Selection that could not be fetched by any Provider.
        if (self.canonical_artifact_ensurer is None
                and self._catalog_snapshot_explicit):
            catalog_snapshot = tuple(self.catalog_snapshot_provider())
            catalog_snapshot_loaded = True
            if self._v3_catalog_snapshot_matches_candidate(
                    selected_candidate, catalog_snapshot):
                v3_artifact_preparation = ArtifactPreparationMode.PRE_SPLIT
            else:
                v3_artifact_preparation = ArtifactPreparationMode.GENERATED
            print(
                "NDNSF_DI_AUTOPLANNING_ARTIFACT_PREPARATION",
                f"requestId={request_id}",
                f"candidateDigest={selected_candidate.candidate_digest}",
                f"preparation={v3_artifact_preparation.value}",
                flush=True,
            )
            canonical_published = self._prepare_artifacts(
                selected_candidate, v3_artifact_preparation, deadline_ms)

        if canonical_published is not None:
            artifact_digests = dict(canonical_published.artifact_digests_by_role)
        else:
            artifact_digests = dict(
                selected_candidate.rank_artifact_digests_by_role or
                selected_candidate.artifacts_by_role)
        self._emit_lifecycle(
            "ARTIFACTS_READY", request_id=request_id, attempt=_attempt,
            artifactDigest=canonical_digest(artifact_digests),
            artifactCount=len(artifact_digests))

        proposal_role_counts = {
            item.role: sum(other.role == item.role for other in proposal.roles)
            for item in proposal.roles
        }

        def proposal_role_key(spec: RoleAssemblySpec) -> str:
            return (spec.role if proposal_role_counts[spec.role] == 1
                    else f"{spec.role}#{spec.rank}")

        role_keys_by_name: dict[str, tuple[str, ...]] = {}
        for logical_role in selected_candidate.execution_plan.roles:
            role_keys_by_name[logical_role] = tuple(
                proposal_role_key(spec) for spec in proposal.roles
                if spec.role == logical_role)
            if not role_keys_by_name[logical_role]:
                raise ValueError(
                    f"V3 proposal omitted role {logical_role}")

        def candidate_role_key(logical_role: str, *, label: str) -> str:
            keys = role_keys_by_name.get(str(logical_role), ())
            if len(keys) != 1:
                raise ValueError(
                    f"V3 candidate {label} role is not uniquely projected")
            return keys[0]

        declared_input_role = ""
        declared_terminal_role = ""
        if str(task.task_name) == "object-detection":
            if (not selected_candidate.input_ingress_role
                    or not selected_candidate.result_egress_role):
                raise ValueError(
                    "V3 object-detection candidate lacks ingress/egress ownership")
            declared_input_role = candidate_role_key(
                selected_candidate.input_ingress_role, label="input-ingress")
            declared_terminal_role = candidate_role_key(
                selected_candidate.result_egress_role, label="result-egress")

        dependency_dicts: list[dict[str, Any]] = []
        redistribution_by_boundary: dict[int, list[dict[str, Any]]] = {}
        if selected_candidate.hybrid_plan is not None:
            rank_stage: dict[int, int] = {}
            rank_cursor = 0
            for stage, degree in enumerate(
                    selected_candidate.hybrid_plan.tensor_degrees):
                for rank in range(rank_cursor, rank_cursor + degree):
                    rank_stage[rank] = stage
                rank_cursor += degree
            for edge in selected_candidate.hybrid_plan.redistributions:
                boundary = rank_stage[edge.producer_ranks[0]]
                redistribution_by_boundary.setdefault(boundary, []).append({
                    "producerRanks": list(edge.producer_ranks),
                    "consumerRanks": list(edge.consumer_ranks),
                    "tensor": edge.tensor,
                    "operation": edge.operation,
                    "epoch": edge.epoch,
                    "integrityDigest": edge.integrity_digest,
                    "sourceLayoutDigest": edge.source_layout_digest,
                    "targetLayoutDigest": edge.target_layout_digest,
                    "temporaryMemoryBytes": edge.temporary_memory_bytes,
                    "completeOutput": edge.complete_output,
                    "axis": edge.axis,
                })
        for index, dependency in enumerate(
                selected_candidate.execution_plan.dependencies):
            scope = f"tensor-{index}-{canonical_digest(dependency)[7:23]}"
            dependency_contract = {
                "producers": list(role_keys_by_name[dependency.producer]),
                "consumers": list(role_keys_by_name[dependency.consumer]),
                "key_scope": scope,
                "topic_prefix": "/activation",
                # Native Providers must derive the same immutable object name
                # without relying on the producer's process-local sequence.
                "object_name_template": (
                    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/"
                    "{keyScope}/{producerRole}"
                ),
                "required": True,
                "tensors": list(dependency.tensor_edges),
            }
            if redistribution_by_boundary.get(index):
                dependency_contract["redistributions"] = (
                    redistribution_by_boundary[index])
                first = redistribution_by_boundary[index][0]
                dependency_contract.update({
                    "transportProfile": "NDNSF_DATA_V1",
                    "collectiveOperationIndex": index,
                    "collectiveProducerRank": str(first["producerRanks"][0]),
                    "collectiveSourceLayoutDigest": first["sourceLayoutDigest"],
                    "collectiveTargetLayoutDigest": first["targetLayoutDigest"],
                    "collectiveTensorDigest": first["integrityDigest"],
                })
            dependency_dicts.append(dependency_contract)
        base_dependency_count = len(dependency_dicts)
        # The terminal response owner is part of every V3 role contract, not
        # only TOKEN_STREAMING.  Derive it from the graph dependencies before
        # entering the optional feedback-edge branch so ordinary unary
        # Presplit requests cannot observe an unbound local.
        planned_role_keys = tuple(
            proposal_role_key(spec) for spec in proposal.roles)
        producers = {
            str(role) for item in dependency_dicts
            for role in item["producers"]
        }
        terminal_roles = ([declared_terminal_role]
                          if declared_terminal_role else
                          sorted(set(planned_role_keys) - producers))
        if len(terminal_roles) != 1:
            raise ValueError(
                "V3 plan requires exactly one terminal response role")
        generation_contract = None
        if str(generation_mode).upper() == "TOKEN_STREAMING":
            consumers = {
                str(role) for item in dependency_dicts
                for role in item["consumers"]
            }
            first_roles = sorted(set(planned_role_keys) - consumers)
            if len(first_roles) != 1 or len(terminal_roles) != 1:
                raise ValueError(
                    "TOKEN_STREAMING requires one pipeline source and one terminal role")
            feedback_scope = (
                "token-feedback-" + canonical_digest({
                    "request": collaboration.request_id,
                    "attempt": _attempt,
                    "producer": terminal_roles[0],
                    "consumer": first_roles[0],
                })[7:23]
            )
            feedback_layout_digest = canonical_digest({
                "tensor": "input_ids",
                "layout": "int64[1,1]",
                "operation": "TOKEN_FEEDBACK",
            })
            dependency_dicts.append({
                "producers": [terminal_roles[0]],
                "consumers": [first_roles[0]],
                "key_scope": feedback_scope,
                "topic_prefix": "/token-feedback",
                "object_name_template": (
                    "{producerProvider}/NDNSF/DI/DATA/{sessionId}/"
                    "{keyScope}/{producerRole}/{sequence}"
                ),
                "required": True,
                "tensors": ["input_ids"],
                "operationKind": "TOKEN_FEEDBACK",
                "transportProfile": "NDNSF_DATA_V1",
                "collectiveOperationIndex": base_dependency_count,
                "collectiveProducerRank": str(
                    planned_role_keys.index(terminal_roles[0])),
                "collectiveSourceLayoutDigest": feedback_layout_digest,
                "collectiveTargetLayoutDigest": feedback_layout_digest,
                "collectiveTensorDigest": canonical_digest(("input_ids",)),
            })
            generation_contract = self._generation_execution_contract_v1(
                generation_mode=generation_mode,
                application_input=input,
                options=options,
                generation_id=(
                    str(getattr(stream_options, "generation_id", "") or "")
                    if callable(getattr(stream_options, "as_dict", None))
                    else str((stream_options or {}).get("generation_id", "")
                             if isinstance(stream_options, Mapping) else "")),
                role_specs=tuple(proposal.roles),
                streaming_operation_stride=len(dependency_dicts),
                generation_recovery=_generation_recovery,
            )
        # Dependencies are part of the proposal/core digest.  Adding them only
        # to Provider projections after sealing would let a rank edge change
        # without changing the selected plan identity.
        proposal = replace(proposal, dependencies=tuple(dependency_dicts))

        # Use the strategy's sealed role specs (including rank/device choices)
        # as the plan's authoritative role input.  Canonical publication, when
        # enabled, has already been completed and re-certified above; the
        # candidate-derived tuple is only the strategy's initial input.
        role_specs = tuple(proposal.roles)

        placement_input = PlacementRequest(
            request_id=collaboration.request_id, attempt=_attempt,
            deadline_ms=deadline_ms, model_digest=descriptor.model_digest,
            graph_digest=graph.graph_digest,
            candidate_ids=tuple(item.candidate_digest for item in candidates),
            providers=tuple(providers),
            required_roles=selected_candidate.execution_plan.roles,
            budget=self.budget, objective=objective,
            constraints={
                **dict(constraints or {}),
                **({"generation_recovery_digest":
                    _generation_recovery.digest()}
                   if _generation_recovery is not None else {}),
                **({"excluded_providers": tuple(sorted(excluded_providers))}
                   if excluded_providers else {}),
            },
            catalog_snapshot=(catalog_snapshot if catalog_snapshot_loaded
                              else tuple(self.catalog_snapshot_provider())),
            task_digest=task.task_descriptor_digest, state_contracts=adapter.state.contracts,
            model=descriptor, graph=graph, candidates=candidates,
        )
        strategy_identity_digest = canonical_digest({
            "name": strategy.name, "version": strategy.version,
            "state_digest": strategy.state_digest,
        })
        core = PlanSealerV3.seal_core(
            {
                "request_id": collaboration.request_id,
                "attempt": _attempt,
                "now_ms": int(time.time() * 1000),
                "deadline_ms": deadline_ms,
                "ack_closed_digest": closed.digest,
                "candidate_digest": selected_candidate.candidate_digest,
                "request_contract_digest": "sha256:" + hashlib.sha256(
                    request_payload).hexdigest(),
                "generation_contract": generation_contract,
            }, proposal, {view.provider: view for view in providers})
        security_policy_digest = canonical_digest({
            "policy": "ndnsf-di-default-v3",
            "request_id": collaboration.request_id,
            "attempt": _attempt,
        })
        provider_views = {item.provider: item for item in providers}
        role_names = tuple(item.role for item in core.roles)
        protected_providers = {
            core.provider_by_role[
                role.role if role_names.count(role.role) == 1
                else f"{role.role}#{role.rank}"]
            for role in core.roles
            if role.protection_epoch != "plaintext-v1"
        }
        if protected_providers and self.grant_binding_provider is None:
            raise RuntimeError(
                "protected placement requires an ArtifactPolicyAuthority grant provider")
        grant_bindings_by_provider: dict[str, GrantBindingV1] = {}
        for provider in sorted(protected_providers):
            grant_view = PlanSealerV3.grant_view(
                core, provider, provider_views[provider], security_policy_digest)
            binding = self.grant_binding_provider(grant_view, deadline_ms)
            if not isinstance(binding, GrantBindingV1):
                raise TypeError("grant provider did not return GrantBindingV1")
            grant_bindings_by_provider[provider] = binding
        grants = tuple(grant_bindings_by_provider.values())
        plan_digest = PlanSealerV3.finalize_security(
            core, grants, security_policy_digest)
        self._emit_lifecycle(
            "PLAN_SEALED", request_id=request_id, attempt=_attempt,
            planDigest=str(plan_digest))

        dependencies: list[CollaborationDependency] = []
        committed_dependencies: list[DIDataDependencyV2] = []
        key_scopes: dict[str, tuple[str, ...]] = {}
        all_role_keys = tuple(
            proposal_role_key(spec) for spec in proposal.roles)
        role_scopes: dict[str, list[str]] = {
            role: [] for role in all_role_keys
        }
        input_scopes: dict[str, list[str]] = {
            role: [] for role in all_role_keys
        }
        for dependency_dict, dependency in zip(
                dependency_dicts,
                selected_candidate.execution_plan.dependencies):
            scope = str(dependency_dict["key_scope"])
            producer_roles = tuple(str(item)
                                   for item in dependency_dict["producers"])
            consumer_roles = tuple(str(item)
                                   for item in dependency_dict["consumers"])
            dependencies.append(CollaborationDependency(
                producers=list(producer_roles), consumers=list(consumer_roles),
                key_scope=scope, topic_prefix="/activation", required=True,
            ))
            committed_dependencies.append(DIDataDependencyV2(
                producers=producer_roles, consumers=consumer_roles,
                key_scope=scope, topic_prefix="/activation", required=True,
                tensors=tuple(dependency.tensor_edges),
            ))
            key_scopes[scope] = producer_roles + consumer_roles
            for role in producer_roles + consumer_roles:
                role_scopes[role].append(scope)
            for role in consumer_roles:
                input_scopes[role].append(scope)
        if str(generation_mode).upper() == "FULL":
            key_scopes[GENERATION_CONTROL_SCOPE] = all_role_keys
            for role in all_role_keys:
                role_scopes[role].append(GENERATION_CONTROL_SCOPE)
        if conversation is not None:
            # The requester owns this one request-scoped key. Providers use it
            # only to publish their signed role receipts; no model-state bytes
            # or reusable cache handle crosses the network.
            key_scopes[CONVERSATION_STATE_SCOPE] = all_role_keys
            for role in all_role_keys:
                role_scopes[role].append(CONVERSATION_STATE_SCOPE)

        artifact_names: dict[str, str] = {}
        artifact_fetch_names: dict[str, str] = {}
        roles: list[CollaborationRole] = []
        for spec in proposal.roles:
            role = proposal_role_key(spec)
            artifact_name = (
                canonical_published.artifact_data_names_by_role.get(
                    role,
                    canonical_published.artifact_data_names_by_role.get(
                        spec.role, ""))
                if canonical_published is not None
                else self._v3_artifact_name(model, graph, spec, adapter))
            if not artifact_name:
                raise ValueError(
                    f"published split omitted rank artifact {role}")
            artifact_names[role] = artifact_name
            fetch_name = (
                canonical_published.artifact_fetch_data_names_by_role.get(
                    role,
                    canonical_published.artifact_fetch_data_names_by_role.get(
                        spec.role, artifact_name),
                )
                if canonical_published is not None else artifact_name)
            if not fetch_name:
                raise ValueError(
                    f"published split omitted rank fetch reference {role}")
            # A Provider whose accepted V3 offer declared preparation from
            # local material (or exact residency) prepares the role without
            # any external fetch.  Its sealed-plan fetch reference stays
            # empty while the canonical artifact identity remains bound in
            # ``artifact_names`` and the V3 projection digest.  A Provider
            # that advertises provisioning (can_provision=True) still needs
            # a real external fetch reference.
            selected_provider = str(proposal.provider_by_role.get(role, ""))
            selected_offer = provider_offers.get(selected_provider)
            if selected_offer is not None and not bool(
                    selected_offer.can_provision) and (
                    bool(selected_offer.preparation_accepted)
                    or (selected_offer.execution_disposition
                        is ExecutionDisposition.ACCEPT_IF_EXACT_REUSE)):
                fetch_name = ""
            artifact_fetch_names[role] = fetch_name
            roles.append(CollaborationRole(
                role=role, service=self.service_name, artifact=artifact_name,
                allow_dynamic_provisioning=False,
                terminal_response_owner=(role == terminal_roles[0]),
            ))

        assignment_payloads: dict[str, bytes] = {}
        providers_by_role = dict(proposal.provider_by_role)
        group_capabilities, dependency_metadata = (
            self._seal_v3_group_capabilities(
                request_id=collaboration.request_id,
                proposal=proposal,
                dependencies=tuple(dependency_dicts),
                provider_views=provider_views,
                provider_offers=provider_offers,
                provider_acks=provider_acks,
                plan_digest=plan_digest,
                deadline_ms=deadline_ms,
                generation_contract=generation_contract,
            ))
        for index, metadata in dependency_metadata.items():
            dependency_dicts[index].update(dict(metadata))

        specs_by_role = {
            proposal_role_key(spec): spec for spec in proposal.roles
        }
        execution_roles = {
            role: ExecutionRole(
                role_id=role,
                stage_id=spec.role,
                rank=spec.rank,
                layer_begin=spec.layer_begin,
                layer_end=spec.layer_end,
                backend=spec.backend,
                adapter_id=spec.adapter_id,
                adapter_version=spec.adapter_version,
            )
            for role, spec in specs_by_role.items()
        }
        may_publish: dict[str, list[TensorEndpoint]] = {
            role: [] for role in specs_by_role
        }
        must_fetch: dict[str, list[TensorEndpoint]] = {
            role: [] for role in specs_by_role
        }
        outgoing_roles: set[str] = set()
        if declared_input_role:
            input_layout_digest = canonical_digest({
                "task": task.task_name,
                "inputSchemaDigest": input.input_schema_digest,
                "transport": str(getattr(
                    input.transport_mode, "value", input.transport_mode)),
            })
            input_endpoint = TensorEndpoint(
                producer_namespace=self.service_name,
                requester=(collaboration.request_id
                           if collaboration.request_id.startswith("/")
                           else "/" + collaboration.request_id),
                request_id=collaboration.request_id,
                attempt=core.attempt,
                plan_digest=plan_digest,
                group_id="application-input",
                group_epoch=f"attempt-{core.attempt}",
                operation="APPLICATION_INPUT",
                round=0,
                source_kind=TensorEndpointSource.APPLICATION_INPUT,
                producer_role="",
                producer_rank=0,
                consumer_role=declared_input_role,
                tensor_id="application-input",
                tensor_digest=input.logical_input_digest,
                layout_digest=input_layout_digest,
                microbatch=0,
                segment_count=1,
                manifest_digest=input.logical_input_digest,
                security_profile="NDNSF_DATA_V1",
                no_progress_deadline_ms=self.data_v1_no_progress_ms,
                hard_deadline_ms=max(
                    1, deadline_ms - int(time.time() * 1000)),
            )
            must_fetch[declared_input_role].append(input_endpoint)
        for index, dependency in enumerate(dependency_dicts):
            if str(dependency.get("operationKind", "")) == "TOKEN_FEEDBACK":
                # The feedback edge closes the per-epoch runtime loop but is
                # deliberately excluded from the acyclic one-epoch V3
                # readiness projection. NativeEpochCoordinator consumes it
                # from the separately sealed execution-plan dependency list.
                continue
            redistributions = tuple(dependency.get("redistributions", ()))
            redistribution = (
                dict(redistributions[0]) if redistributions else {})
            operation = str(
                redistribution.get("operation", "PIPELINE"))
            dependency_tensors = tuple(
                str(item) for item in dependency.get("tensors", ())
                if str(item))
            if redistributions:
                tensor_specs = ((str(redistribution.get(
                    "tensor", dependency_tensors[0]
                    if dependency_tensors else dependency["key_scope"])),),)
            else:
                if not dependency_tensors:
                    raise ValueError(
                        "V3 pipeline dependency has no tensor identity")
                # A normal pipeline dependency is a transport scope that may
                # carry several adapter tensors.  Each sealed DATA_V1
                # endpoint is tensor-specific so the native runner can map
                # its named ONNX outputs to the corresponding publication.
                tensor_specs = tuple((tensor,) for tensor in dependency_tensors)
            tensor_digest = str(
                redistribution.get(
                    "integrityDigest",
                    canonical_digest(dependency.get("tensors", ())),
                ))
            layout_digest = str(
                redistribution.get(
                    "sourceLayoutDigest",
                    canonical_digest({
                        "scope": dependency["key_scope"],
                        "layout": "adapter-certified-opaque",
                    }),
                ))
            target_layout_digest = str(
                redistribution.get("targetLayoutDigest", layout_digest))
            group_id = str(dependency.get(
                "groupId", dependency["key_scope"]))
            group_epoch = str(dependency.get(
                "groupEpoch",
                redistribution.get("epoch", f"attempt-{core.attempt}")))
            round_id = int(dependency.get(
                "collectiveOperationIndex", index))
            for producer_role in dependency["producers"]:
                producer = str(producer_role)
                outgoing_roles.add(producer)
                if producer not in specs_by_role:
                    raise ValueError(
                        "V3 dependency references an unknown producer role")
                producer_spec = specs_by_role[producer]
                producer_provider = providers_by_role.get(producer)
                if producer_provider is None:
                    raise ValueError("V3 dependency producer has no Provider")
                endpoint_prefixes = dict(
                    dependency.get("producerEndpointPrefixes", {}))
                producer_namespace = str(
                    endpoint_prefixes.get(producer_provider, producer_provider))
                dependency_consumers = tuple(
                    str(role) for role in dependency["consumers"])
                producer_endpoints: list[TensorEndpoint] = []
                for (tensor_id,) in tensor_specs:
                    for consumer_role in dependency_consumers:
                        consumer = str(consumer_role)
                        if consumer not in specs_by_role:
                            raise ValueError(
                                "V3 dependency references an unknown consumer role")
                        manifest_digest = canonical_digest({
                            "requestId": collaboration.request_id,
                            "attempt": core.attempt,
                            "planDigest": plan_digest,
                            "group": group_id,
                            "epoch": group_epoch,
                            "operation": operation,
                            "round": round_id,
                            "producer": producer,
                            "consumers": dependency_consumers,
                            "tensor": tensor_id,
                            "tensorDigest": tensor_digest,
                        })
                        endpoint = TensorEndpoint(
                            producer_namespace=producer_namespace,
                            requester=(collaboration.request_id
                                       if collaboration.request_id.startswith("/")
                                       else "/" + collaboration.request_id),
                            request_id=collaboration.request_id,
                            attempt=core.attempt,
                            plan_digest=plan_digest,
                            group_id=group_id,
                            group_epoch=group_epoch,
                            operation=operation,
                            round=round_id,
                            source_kind=TensorEndpointSource.ROLE,
                            producer_role=producer,
                            producer_rank=producer_spec.rank,
                            consumer_role=consumer,
                            tensor_id=tensor_id,
                            tensor_digest=tensor_digest,
                            layout_digest=layout_digest,
                            microbatch=0,
                            # Plan-time upper bound. The signed runtime manifest
                            # supplies the concrete segment count after execution.
                            segment_count=_DATA_V1_MAX_SEGMENTS,
                            manifest_digest=manifest_digest,
                            security_profile="NDNSF_DATA_V1",
                            no_progress_deadline_ms=int(dependency.get(
                                "noProgressMs", self.data_v1_no_progress_ms)),
                            hard_deadline_ms=int(dependency.get(
                                "hardDeadlineMs",
                                max(1, deadline_ms - int(time.time() * 1000)))),
                            consumer_roles=dependency_consumers,
                            target_layout_digest=target_layout_digest,
                        )
                        if consumer == dependency_consumers[0]:
                            producer_endpoints.append(endpoint)
                        must_fetch[consumer].append(endpoint)
                may_publish[producer].extend(producer_endpoints)

        terminal_roles = ([declared_terminal_role]
                          if declared_terminal_role else
                          sorted(set(specs_by_role) - outgoing_roles))
        if len(terminal_roles) != 1:
            raise ValueError(
                "V3 plan must have exactly one terminal Response owner")
        dataflow_contracts = {}
        for role in sorted(specs_by_role):
            fetches = tuple(must_fetch[role])
            wait_for = (() if not fetches else (
                ReadinessPredicate(
                    ReadinessMode.ALL,
                    tuple(item.endpoint_digest for item in fetches),
                ),
            ))
            dataflow_contracts[role] = RoleDataflowContract(
                request_id=collaboration.request_id,
                attempt=core.attempt,
                plan_digest=plan_digest,
                role=role,
                may_publish=tuple(may_publish[role]),
                must_fetch=fetches,
                wait_for=wait_for,
                terminal_response_owner=(role == terminal_roles[0]),
            )
        validate_role_dataflow_contracts(
            tuple(execution_roles.values()),
            tuple(dataflow_contracts.values()),
        )
        print(
            "NDNSF_DI_V3_DATAFLOW_TERMINAL_CHECK",
            "roles=" + ",".join(sorted(dataflow_contracts)),
            "terminals=" + ",".join(
                sorted(role for role, contract in dataflow_contracts.items()
                       if contract.terminal_response_owner)),
            "publishers=" + ",".join(sorted(outgoing_roles)),
            flush=True,
        )

        device_bindings = {}
        for role, spec in specs_by_role.items():
            provider = providers_by_role[role]
            view = provider_views[provider]
            cpu = is_cpu_backend(spec.backend)
            if not cpu and len(spec.device_set) != 1:
                raise ValueError(
                    "V3 accelerator role requires one selected device")
            resource_sequence = max(
                (item.resource_sequence for item in view.resources),
                default=1,
            )
            device_bindings[role] = DeviceBinding(
                mode=(DeviceBindingMode.CPU if cpu
                      else DeviceBindingMode.SINGLE_DEVICE),
                provider=provider,
                role=role,
                offer_digest=view.offer_digest,
                topology_profile_digest=view.topology.digest(),
                resource_snapshot_digest=canonical_digest(view.resources),
                resource_sequence=resource_sequence,
                offer_scoped_device_handle=("" if cpu else spec.device_set[0]),
            )
        conversation_now_ms = int(time.time() * 1000)
        conversation_state_references = (
            conversation_state_references_for_placement(
                conversation,
                service_name=self.service_name,
                providers_by_role=providers_by_role,
                execution_roles=execution_roles,
                now_ms=conversation_now_ms,
            ))
        # The turn binding authenticates the conversation continuation itself,
        # while ``core.request_contract_digest`` binds the complete encoded
        # DI request envelope.  These are deliberately different domains:
        # using the envelope digest here makes every Provider reject an
        # otherwise valid continuation because the two digests cannot match.
        conversation_contract_digest = None
        if conversation is not None:
            conversation_contract_digest = conversation.request_contract(
                input_digest="sha256:" + hashlib.sha256(
                    bytes(input.payload)).hexdigest())
        conversation_turn_binding = conversation_turn_binding_for_placement(
            conversation,
            service_name=self.service_name,
            providers_by_role=providers_by_role,
            execution_roles=execution_roles,
            request_contract_digest=(conversation_contract_digest
                                     if conversation_contract_digest is not None
                                     else core.request_contract_digest),
            now_ms=conversation_now_ms,
        )
        for provider in sorted(set(providers_by_role.values())):
            provider_roles = tuple(
                spec for spec in proposal.roles
                if providers_by_role[
                    spec.role if sum(item.role == spec.role for item in proposal.roles) == 1
                    else f"{spec.role}#{spec.rank}"] == provider
            )
            local_role = proposal_role_key(provider_roles[0])
            state_reference = conversation_state_references.get(local_role)
            projection = PlanSealerV3.project(
                core,
                plan_digest=plan_digest,
                provider=provider,
                offer=provider_views[provider],
                security_policy_snapshot_digest=security_policy_digest,
                execution_role=execution_roles[proposal_role_key(provider_roles[0])],
                assembly=provider_roles[0],
                dataflow=dataflow_contracts[proposal_role_key(provider_roles[0])],
                device_binding=device_bindings[proposal_role_key(provider_roles[0])],
                dependencies=tuple(dependency_dicts), deadline_ms=deadline_ms,
                group_capability_v1=group_capabilities.get(provider, ""),
                grant_binding=grant_bindings_by_provider.get(provider),
                conversation_state_reference=state_reference,
                conversation_turn_binding=conversation_turn_binding,
            )
            payload = projection.to_bytes()
            print(
                "NDNSF_DI_V3_PROJECTION_TERMINAL",
                f"provider={provider}",
                f"role={proposal_role_key(provider_roles[0])}",
                f"terminal={dataflow_contracts[proposal_role_key(provider_roles[0])].terminal_response_owner}",
                f"wireTerminal={json.loads(payload.decode('utf-8')).get('dataflow', {}).get('terminal_response_owner')}",
                f"conversationTurnBinding={conversation_turn_binding is not None}",
                f"conversationStateReference={state_reference is not None}",
                flush=True,
            )
            for spec in provider_roles:
                # ``provider_by_role`` uses a stable role#rank key whenever
                # one logical role has multiple ranks.  Keep the assignment
                # payload map in that same key space; using ``spec.role``
                # here silently overwrote rank 0 with rank 1 and left the
                # sealed plan with an incomplete per-role projection.
                key = proposal_role_key(spec)
                assignment_payloads[key] = payload

        scope_key_data_names = self._publish_scope_keys(key_scopes, deadline_ms)
        sealed = SealedCollaborationPlan(
            ack_closed_digest=closed.digest,
            placement_input_digest=placement_input.digest(),
            placement_decision_digest=proposal.digest(),
            strategy_identity_digest=strategy_identity_digest,
            execution_policy=DATA_DRIVEN_V2, roles=tuple(roles),
            dependencies=tuple(dependencies), key_scopes=key_scopes,
            role_scopes={key: tuple(value) for key, value in role_scopes.items()},
            providers_by_role=providers_by_role,
            artifact_data_names=artifact_names,
            artifact_fetch_data_names=artifact_fetch_names,
            scope_key_data_names=scope_key_data_names,
            assignment_payloads_by_role=assignment_payloads,
        )

        assignments = []
        exact_all = True
        for spec in proposal.roles:
            key = proposal_role_key(spec)
            provider = providers_by_role[key]
            view = provider_views[provider]
            requirement = selected_candidate.requirements_by_role[spec.role]
            required_mib = int(
                math.ceil((requirement.estimated_peak_gpu_memory_bytes or 0) / (1024 * 1024)))
            device = spec.device_set[0] if len(spec.device_set) == 1 else (
                "cpu" if is_cpu_backend(spec.backend) else "")
            assignments.append(ProviderAssignment(
                role=key, provider=provider,
                required_gpu_memory_mb=required_mib,
                backend=spec.backend, device=device,
            ))
            exact = any(
                proof.role == spec.role and proof.rank == spec.rank
                and proof.artifact_digest == spec.artifact_digest
                for proof in view.residency)
            exact_all = exact_all and exact
        decision = PlacementDecision(
            split_id=selected_candidate.candidate_digest,
            split_digest=selected_candidate.candidate_digest,
            assignments=tuple(assignments), fallback_order={},
            input_digest=placement_input.digest(),
            evidence_digest=canonical_digest({
                "placement": placement_input.digest(),
                "proposal": proposal.digest(), "core": core.digest(),
                "plan": plan_digest,
            }),
            artifact_preparation=(v3_artifact_preparation
                                  if v3_artifact_preparation is not None else
                                  ArtifactPreparationMode.REUSE_CACHED
                                  if exact_all else ArtifactPreparationMode.GENERATED),
            evidence={"placementProfile": DI_PLACEMENT_V3,
                      "planCoreDigest": core.plan_core_digest or core.digest(),
                      "planDigest": plan_digest},
        )
        self._publish_scope_keys  # keep the trusted side-effect boundary explicit
        committed = collaboration.commit_plan(
            ack_closed_digest=closed.digest,
            roles=list(sealed.roles), key_scopes={
                key: list(value) for key, value in sealed.key_scopes.items()},
            dependencies=list(sealed.dependencies),
            artifact_data_names=dict(sealed.artifact_fetch_data_names),
            scope_key_data_names=dict(sealed.scope_key_data_names),
            role_scopes={key: list(value) for key, value in sealed.role_scopes.items()},
            role_provider_assignments=dict(sealed.providers_by_role),
            assignment_payloads_by_role=dict(sealed.assignment_payloads_by_role),
        )
        if not committed:
            raise RuntimeError(
                "V3 collaboration plan commit produced no Selection")
        print(
            "NDNSF_DI_AUTOPLANNING_SELECTION_COMMITTED",
            f"requestId={request_id}",
            f"candidateDigest={selected_candidate.candidate_digest}",
            "placement=V3", f"planDigest={plan_digest}", flush=True,
        )
        self._emit_lifecycle(
            "SELECTION_COMMITTED", request_id=request_id, attempt=_attempt,
            selectionDigest=canonical_digest({
                "plan": plan_digest,
                "ack": closed.digest,
            }),
            selectedRoleCount=len(sealed.roles))
        timings["pre_response_setup_total_ms"] = (
            time.perf_counter() - request_started) * 1000.0
        return AutomaticInferenceHandle(
            collaboration, decision, sealed, adapter, timings, invocation_id,
            service_user=self.service_user,
            conversation_metadata={
                "model_contract_digest": model.intent_digest,
                "tokenizer_digest": model.semantics_digest,
                "chat_template_digest": str(
                    input.metadata.get("chat_template_digest",
                                      input.metadata.get("chatTemplateDigest",
                                                         model.semantics_digest))),
                "application_messages": bytes(input.payload),
                "service_name": self.service_name,
                "plan_digest": plan_digest,
                "plan_role_map_digest": canonical_digest(tuple(sorted(
                    (str(role), str(provider))
                    for role, provider in sealed.providers_by_role.items()))),
            })

    @staticmethod
    def _v3_role_specs(
        candidate: SplitCandidate, graph: Any | None = None,
        *, protection_epoch: str = "plaintext-v1",
    ) -> tuple[RoleAssemblySpec, ...]:
        # Keep the historical candidate-only helper source-compatible.  The
        # production request path passes the post-ACK graph explicitly and
        # certification still requires that graph.  A legacy caller has no
        # graph object to provide, so it receives role/rank/artifact specs
        # without guessed model I/O contracts; this avoids fabricating graph
        # metadata while preserving the candidate's exact rank coverage.
        def role_kind(role: str) -> str:
            """Map a Collaboration role to its adapter-defined identity kind.

            The role name is an execution namespace (and may contain many
            slash components); it is never safe to place it in one canonical
            NDN component.  The semantic kind is stable across stage labels
            and therefore preserves canonical-layer reuse.
            """
            parts = tuple(part.lower() for part in str(role).strip("/").split("/")
                          if part)
            if any("tensor" in part or "shard" in part or "rank" in part
                   for part in parts):
                return "TENSOR_RANK"
            if any(part in {"pipeline", "stage", "stages"}
                       or part.startswith("stage-")
                   for part in parts):
                return "PIPELINE_RANGE"
            # A non-layer adapter still has a deterministic component-set
            # identity; it must not fall back to the raw role path.
            return "COMPONENT_SET"

        edge_by_name = ({edge.edge_id: edge for edge in graph.edges}
                        if graph is not None else {})
        model_inputs = ({item.name: item for item in graph.model_inputs}
                        if graph is not None else {})
        model_outputs = ({item.name: item for item in graph.model_outputs}
                         if graph is not None else {})

        def contract(item: Any) -> dict[str, Any]:
            return {
                "name": str(getattr(item, "name", getattr(item, "edge_id", ""))),
                "dtype": str(item.dtype),
                "shape": list(item.shape),
            }

        specs = []
        for role in candidate.execution_plan.roles:
            requirement = candidate.requirements_by_role[role]
            kind = _v3_role_kind(role)
            begin, end = AutomaticPlanningCoordinator._role_layer_range(candidate, role)
            owned = [
                node for node, owner in candidate.execution_plan.node_roles.items()
                if owner == role
            ]
            if kind == "COMPONENT_SET":
                # Component roles are identified by their canonical node set;
                # a fabricated layer interval would change their identity.
                begin, end = 0, 0
            elif begin is None or end is None:
                begin = 0
                end = max(1, len(owned))
            degree = int(candidate.tensor_degrees_by_role.get(role, 1))
            rank_artifacts = candidate.rank_artifact_digests_by_role.get(
                role, (candidate.artifacts_by_role[role][0],))
            incoming = tuple(
                edge for dependency in candidate.execution_plan.dependencies
                if dependency.consumer == role
                for edge in dependency.tensor_edges)
            outgoing = tuple(
                edge for dependency in candidate.execution_plan.dependencies
                if dependency.producer == role
                for edge in dependency.tensor_edges)
            input_contracts = {
                name: contract(edge_by_name[name]) for name in incoming
                if name in edge_by_name
            }
            output_contracts = {
                name: contract(edge_by_name[name]) for name in outgoing
                if name in edge_by_name
            }
            if not incoming:
                input_contracts.update(
                    {name: contract(item) for name, item in model_inputs.items()})
            if not outgoing:
                output_contracts.update(
                    {name: contract(item) for name, item in model_outputs.items()})
            input_contracts.update({
                item.name: contract(item)
                for item in candidate.role_state_inputs_by_role.get(role, ())
            })
            output_contracts.update({
                item.name: contract(item)
                for item in candidate.role_state_outputs_by_role.get(role, ())
            })
            for rank in range(degree):
                recipe_digest = canonical_digest({
                    "candidate": candidate.candidate_digest,
                    "role": role, "rank": rank, "tensorDegree": degree,
                    "begin": begin, "end": end,
                    "backends": requirement.backends,
                })
                specs.append(RoleAssemblySpec(
                    role=role, rank=rank, layer_begin=int(begin),
                    layer_end=int(end), recipe_digest=recipe_digest,
                    artifact_digest=rank_artifacts[rank],
                    backend=str(requirement.backends[0]),
                    protection_epoch=protection_epoch,
                    required_device_memory_mb=int(math.ceil(
                        (requirement.estimated_peak_gpu_memory_bytes or 0)
                        / (1024 * 1024))),
                    adapter_id=str(candidate.model.adapter.name),
                    adapter_version=str(candidate.model.adapter.version),
                    role_kind=("HYBRID_RANK" if degree > 1 else kind),
                    node_indices=(tuple(
                        index for index, node in enumerate(
                            candidate.execution_plan.node_roles)
                        if node in owned)
                        if kind == "COMPONENT_SET" else ()),
                    expected_inputs=tuple(
                        input_contracts[name]
                        for name in sorted(input_contracts)),
                    expected_outputs=tuple(
                        output_contracts[name]
                        for name in sorted(output_contracts)),
                    merge_kind=(candidate.merge_kind
                                if role == candidate.result_egress_role else ""),
                    postprocess_identity=(str(candidate.postprocessing.get("identity", ""))
                                          if role == candidate.result_egress_role else ""),
                    postprocess_output_name=(str(candidate.postprocessing.get("outputName", ""))
                                             if role == candidate.result_egress_role else ""),
                    postprocess_confidence_threshold=(float(
                        candidate.postprocessing.get("confidenceThreshold", 0.0))
                        if role == candidate.result_egress_role else 0.0),
                    postprocess_sort=(str(candidate.postprocessing.get("sort", ""))
                                      if role == candidate.result_egress_role else ""),
                ))
        return tuple(specs)

    @staticmethod
    def _certify_v3_role_specs(
        candidate: SplitCandidate,
        graph: Any,
        role_specs: tuple[RoleAssemblySpec, ...],
        binding: Any,
    ) -> tuple[RoleAssemblySpec, ...]:
        """Seal exact Provider-local ONNX recipes after ACK-driven placement."""

        from ..adapters.onnx.executor import CertifiedOnnxAssemblyRecipe
        from .canonical_artifacts import CanonicalArtifactBinding

        if not isinstance(binding, CanonicalArtifactBinding):
            raise TypeError("canonical ensurer returned an invalid binding")
        if (binding.graph_digest != graph.graph_digest
                or binding.adapter_descriptor_digest
                != graph.adapter.descriptor_digest):
            raise ValueError("canonical root does not match the selected adapter graph")

        edge_by_name = {edge.edge_id: edge for edge in graph.edges}
        model_inputs = {item.name: item for item in graph.model_inputs}
        model_outputs = {item.name: item for item in graph.model_outputs}
        order = {node: index for index, node in enumerate(graph.topological_order)}

        def contract(item: Any) -> dict[str, Any]:
            return {
                "name": str(getattr(item, "name", getattr(item, "edge_id", ""))),
                "dtype": str(item.dtype),
                "shape": list(item.shape),
            }

        certified: list[RoleAssemblySpec] = []
        for spec in role_specs:
            if (candidate.merge_kind == "NATIVE_POSTPROCESS" and
                    spec.role == candidate.result_egress_role):
                # A native Merge consumes the declared dependency tensors and
                # owns only deterministic postprocessing. It has no ONNX
                # model-layer artifact and must not enter the assembler, but
                # a protected-epoch grant still binds the same model-manifest
                # digest as the component roles (spec181 T008 Y-B).
                certified.append(replace(
                    spec,
                    model_manifest_digest=binding.model_manifest_digest,
                    artifact_profile_digest=binding.artifact_profile_digest,
                    graph_digest=binding.graph_digest,
                    canonical_initializer_digest=(
                        binding.canonical_initializer_digest),
                    adapter_descriptor_digest=(
                        binding.adapter_descriptor_digest),
                    assembler_descriptor_digest=(
                        binding.assembler_descriptor_digest),
                ))
                continue
            owned = tuple(sorted(
                order[node] for node, owner
                in candidate.execution_plan.node_roles.items()
                if owner == spec.role))
            if not owned:
                raise ValueError(f"role {spec.role} owns no canonical graph nodes")
            incoming = tuple(
                edge for dependency in candidate.execution_plan.dependencies
                if dependency.consumer == spec.role
                for edge in dependency.tensor_edges)
            outgoing = tuple(
                edge for dependency in candidate.execution_plan.dependencies
                if dependency.producer == spec.role
                for edge in dependency.tensor_edges)
            input_contracts = {
                name: contract(edge_by_name[name]) for name in incoming
                if name in edge_by_name
            }
            output_contracts = {
                name: contract(edge_by_name[name]) for name in outgoing
                if name in edge_by_name
            }
            if not incoming:
                input_contracts.update(
                    {name: contract(item) for name, item in model_inputs.items()})
            if not outgoing:
                output_contracts.update(
                    {name: contract(item) for name, item in model_outputs.items()})
            input_contracts.update({
                item.name: contract(item)
                for item in candidate.role_state_inputs_by_role.get(spec.role, ())
            })
            output_contracts.update({
                item.name: contract(item)
                for item in candidate.role_state_outputs_by_role.get(spec.role, ())
            })
            if not input_contracts or not output_contracts:
                raise ValueError(
                    f"role {spec.role} has an incomplete ONNX I/O boundary")

            max_source = int(binding.canonical_source_bytes)
            # Provider-local extraction may inline a separately addressed
            # initializer and re-encode the graph protobuf.  The source limit
            # applies independently to each fetched object; it is not a
            # valid upper bound for the assembled model.  Bind a conservative
            # finite output limit to the same immutable source facts instead
            # of silently allowing the executor's multi-gigabyte default.
            initializer_bytes = int(binding.canonical_initializer_bytes or 0)
            max_assembled = max_source + initializer_bytes + max_source
            # The assembler verifies the canonical ONNX identity digest,
            # which may differ from the planning-space graph-port digest.
            recipe_graph_digest = (
                binding.canonical_graph_digest or binding.graph_digest)
            recipe = CertifiedOnnxAssemblyRecipe(
                model_manifest_digest=binding.model_manifest_digest,
                artifact_profile_digest=binding.artifact_profile_digest,
                graph_digest=recipe_graph_digest,
                canonical_initializer_digest=binding.canonical_initializer_digest,
                adapter_descriptor_digest=binding.adapter_descriptor_digest,
                assembler_descriptor_digest=binding.assembler_descriptor_digest,
                backend_abi=binding.backend_abi,
                role_kind=spec.role_kind,
                layer_begin=spec.layer_begin,
                layer_end=spec.layer_end,
                node_indices=owned,
                input_names=tuple(sorted(input_contracts)),
                output_names=tuple(sorted(output_contracts)),
                expected_inputs=tuple(
                    input_contracts[name] for name in sorted(input_contracts)),
                expected_outputs=tuple(
                    output_contracts[name] for name in sorted(output_contracts)),
                precision=str(candidate.model.precision),
                max_source_bytes=max_source,
                max_assembled_bytes=max_assembled,
                max_nodes=len(graph.nodes),
            )
            certified.append(replace(
                spec,
                recipe_digest=recipe.digest,
                model_manifest_digest=binding.model_manifest_digest,
                artifact_profile_digest=binding.artifact_profile_digest,
                graph_digest=recipe_graph_digest,
                canonical_initializer_digest=binding.canonical_initializer_digest,
                adapter_descriptor_digest=binding.adapter_descriptor_digest,
                assembler_descriptor_digest=binding.assembler_descriptor_digest,
                backend_abi=binding.backend_abi,
                node_indices=recipe.node_indices,
                expected_inputs=recipe.expected_inputs,
                expected_outputs=recipe.expected_outputs,
                precision=recipe.precision,
                quantization=recipe.quantization,
                layout=recipe.layout,
                padding=recipe.padding,
                resource_envelope={
                    "maxSourceBytes": recipe.max_source_bytes,
                    "maxAssembledBytes": recipe.max_assembled_bytes,
                    "maxNodes": recipe.max_nodes,
                },
            ))
        return tuple(certified)

    @staticmethod
    def _v3_artifact_name(
        model: ModelRef, graph: Any, spec: RoleAssemblySpec,
        adapter: ModelFamilyAdapter,
    ) -> str:
        """Build the stable Repo identity carried in generic Role.artifact."""

        from .canonical_artifacts import canonical_layer_name

        return canonical_layer_name(
            publisher="/ndnsf-di", model_name=model.model_name,
            model_digest=model.content_digest, profile=adapter.descriptor.name,
            graph_digest=str(graph.graph_digest), role_kind=spec.role_kind,
            layer_begin=spec.layer_begin, layer_end=spec.layer_end,
            rank=spec.rank, recipe_digest=spec.recipe_digest,
            object_digest=spec.artifact_digest,
        )

    def request_application(
        self,
        *,
        model: ModelRef,
        input: GenerationInput,
        generation: GenerationConfig,
        strategy: ModelPlacementStrategy | None = None,
        request_id: str = "",
    ) -> AutomaticInferenceHandle:
        """Encode a public generation call without accepting deployment data."""
        if not isinstance(model, ModelRef):
            raise TypeError("model must be ModelRef")
        if not model.source_revision:
            raise ValueError(
                "public model requests require an immutable model revision")
        if not isinstance(input, GenerationInput):
            raise TypeError("input must be GenerationInput")
        if not isinstance(generation, GenerationConfig):
            raise TypeError("generation must be GenerationConfig")
        if generation.adapter_name:
            adapter = self.adapters.get(generation.adapter_name)
            if adapter is None:
                raise ValueError("requested model adapter is not allowlisted")
        elif len(self.adapters) == 1:
            adapter = next(iter(self.adapters.values()))
        else:
            raise ValueError(
                "generation.adapter_name is required when multiple adapters exist")
        task = InferenceTaskRef.from_adapter(adapter)
        encoded = adapter.task.encode_input(
            input.to_task_value(), generation.to_task_options())
        return self.request(
            model=model,
            task=task,
            input=encoded,
            timeout_ms=generation.timeout_ms,
            objective=None,
            constraints={},
            request_id=request_id,
            generation_mode="FULL",
            strategy=strategy,
        )

    @staticmethod
    def _validate_ack_closed_binding(closed: Any, request_id: str) -> None:
        """Reject an ACK snapshot that is not bound to this invocation.

        Generic NDNSF authenticates and freezes the ACK_CLOSED snapshot.  This
        NDNSF-DI boundary additionally verifies the inference request binding
        before graph inspection or an external placement strategy can run.
        A late ACK cannot alter the frozen tuple; a foreign-request ACK must
        never become placement input.
        """
        if str(getattr(closed, "request_id", "")) != request_id:
            raise ValueError("ACK_CLOSED request binding mismatch")
        for candidate in tuple(getattr(closed, "candidates", ())):
            if str(getattr(candidate, "request_id", "")) != request_id:
                raise ValueError("ACK candidate request binding mismatch")

    def generate(self, request: GenerationRequest) -> AutomaticInferenceHandle:
        """Submit one full-generation request through one durable invocation."""
        if not isinstance(request, GenerationRequest):
            raise TypeError("generate requires a GenerationRequest")
        return self.request(
            model=request.model,
            task=request.task,
            input=request.input,
            timeout_ms=request.timeout_ms,
            options=request.options,
            objective=request.objective,
            constraints=request.constraints,
            request_id=request.request_id,
            generation_mode=request.output_mode,
        )

    def _publish_scope_keys(
        self,
        key_scopes: Mapping[str, tuple[str, ...]],
        deadline_ms: int,
    ) -> dict[str, str]:
        """Publish one request-scoped encryption key for every data edge.

        The generic collaboration wire contract carries both the symbolic
        ``key_scopes`` and the encrypted Data names that let each Provider
        retrieve its key.  Automatic planning used to populate only the
        former, which made a correctly sealed dependency graph fail at the
        first inter-stage fetch.  Keep publication in the trusted coordinator
        after ACK/placement and before Selection commit so the committed plan
        is self-contained and providers never have to guess or share keys.
        """
        data_names: dict[str, str] = {}
        for scope in key_scopes:
            remaining_ms = int(deadline_ms - int(time.time() * 1000))
            if remaining_ms <= 0:
                raise TimeoutError("scope-key publication deadline expired")
            result = self.service_user.publish_encrypted_large_data(
                self.service_name,
                secrets.token_bytes(32),
                object_label=f"inference-scope-key-{scope}",
                freshness_ms=max(60000, remaining_ms),
            )
            if not result.success:
                raise RuntimeError(
                    f"scope key publish failed for {scope}: {result.error}")
            data_name = str(result.encrypted_data_name)
            if not data_name.startswith("/"):
                raise ValueError(
                    f"scope key publication returned a non-absolute Data name: {scope}")
            data_names[str(scope)] = data_name
        return data_names

    @staticmethod
    def _v3_catalog_snapshot_matches_candidate(
            candidate: SplitCandidate,
            snapshots: tuple[Any, ...],
    ) -> bool:
        """Return whether a signed active snapshot can satisfy every rank.

        This is only a mode probe.  ``_prepare_artifacts`` and
        ``CatalogSnapshotArtifactPublisher.resolve_existing`` remain the final
        digest/name authority; the probe prevents a known-missing publication
        from being treated as a usable PRE_SPLIT input.
        """
        expected_roles = tuple(candidate.execution_plan.roles)
        for snapshot in snapshots:
            if (getattr(snapshot, "status", "") != "ACTIVE"
                    or getattr(snapshot, "candidate_digest", "")
                    != candidate.candidate_digest
                    or getattr(snapshot, "model_content_digest", "")
                    != candidate.model.content_digest
                    or getattr(snapshot, "semantics_digest", "")
                    != candidate.model.semantics_digest
                    or getattr(snapshot, "graph_digest", "")
                    != candidate.graph_digest
                    or (getattr(snapshot, "precision", "")
                        and getattr(snapshot, "precision", "")
                        != getattr(candidate.model, "precision", ""))):
                continue
            backend = str(getattr(snapshot, "backend", ""))
            if any(backend not in candidate.requirements_by_role[role].backends
                   for role in expected_roles):
                continue
            names_by_role = getattr(snapshot, "artifact_data_names", {})
            if set(names_by_role) != set(expected_roles):
                continue
            if any(len(tuple(names_by_role[role]))
                   != int(candidate.tensor_degrees_by_role.get(role, 1))
                   for role in expected_roles):
                continue
            return True
        return False

    @staticmethod
    def _validate_published_split(
        candidate: SplitCandidate, published: PublishedSplit,
    ) -> None:
        expected = _candidate_artifacts_by_execution_key(candidate)
        if (published.candidate_digest != candidate.candidate_digest
                or dict(published.artifact_digests_by_role) != expected
                or set(published.artifact_data_names_by_role) != set(expected)):
            raise ValueError(
                "published split does not match the selected candidate")

    def _prepare_artifacts(
        self,
        candidate: SplitCandidate,
        preparation: ArtifactPreparationMode,
        deadline_ms: int,
    ) -> PublishedSplit:
        if int(time.time() * 1000) >= deadline_ms:
            raise TimeoutError("artifact preparation deadline expired")
        if preparation in {
                ArtifactPreparationMode.PRE_SPLIT,
                ArtifactPreparationMode.REUSE_CACHED,
        }:
            # REUSE_CACHED is deliberately resolve-only: ACK cache evidence
            # says the selected Providers already hold these content-addressed
            # shards, so a second invocation must not materialize or publish
            # the model again.
            published = self.artifact_publisher.resolve_existing(
                candidate, deadline_ms=deadline_ms)
        else:
            print(
                "NDNSF_DI_AUTOPLANNING_MATERIALIZE_START",
                f"candidateDigest={candidate.candidate_digest}",
                f"materializer={type(self.split_materializer).__module__}.{type(self.split_materializer).__qualname__}",
                flush=True,
            )
            materialized = self.split_materializer.materialize(
                candidate, deadline_ms=deadline_ms)
            print(
                "NDNSF_DI_AUTOPLANNING_MATERIALIZE_DONE",
                f"candidateDigest={candidate.candidate_digest}",
                flush=True,
            )
            expected = _candidate_artifacts_by_execution_key(candidate)
            if (materialized.candidate_digest != candidate.candidate_digest
                    or dict(materialized.artifact_digests_by_role) != expected):
                raise ValueError(
                    "materialized split does not match the selected candidate")
            print(
                "NDNSF_DI_AUTOPLANNING_PUBLISH_START",
                f"candidateDigest={candidate.candidate_digest}",
                f"publisher={type(self.artifact_publisher).__module__}.{type(self.artifact_publisher).__qualname__}",
                flush=True,
            )
            published = self.artifact_publisher.publish(
                candidate, materialized, deadline_ms=deadline_ms)
        self._validate_published_split(candidate, published)
        if int(time.time() * 1000) >= deadline_ms:
            raise TimeoutError("artifact publication completed after deadline")
        return published

    @staticmethod
    def _request_input_manifest_digest(
        application_input: ApplicationInput,
        options: TaskOptions | None,
    ) -> str:
        options_payload = (
            options.payload if options is not None
            else application_input.options)
        return canonical_digest({
            "input_schema_digest": application_input.input_schema_digest,
            "options_schema_digest": application_input.options_schema_digest,
            "input_transport": getattr(
                application_input.transport_mode, "value",
                str(application_input.transport_mode)),
            "input_digest": application_input.logical_input_digest,
            "input_reference": application_input.repo_reference or {},
            "options_digest": hashlib.sha256(options_payload).hexdigest(),
        })

    @staticmethod
    def _encode_request(
        model: ModelRef,
        task: InferenceTaskRef,
        application_input: ApplicationInput,
        options: TaskOptions | None,
        deadline_ms: int,
        request_id: str,
        service_name: str,
        invocation_id: str,
        generation_mode: str = "TOKEN_DIAGNOSTIC",
        placement_profile: str = "DI_PLACEMENT_V2",
        attempt: int = 1,
        generation_recovery: GenerationRecoveryV1 | None = None,
        conversation: ConversationContinuation | None = None,
    ) -> bytes:
        options_payload = (
            options.payload if options is not None
            else application_input.options)
        input_manifest_digest = (
            AutomaticPlanningCoordinator._request_input_manifest_digest(
                application_input, options))
        task_contract = {
            "name": task.task_name,
            "adapter": task.adapter_name,
            "adapter_descriptor_digest":
                task.adapter_descriptor_digest,
            "adapter_composition_digest":
                task.adapter_composition_digest,
            "task_descriptor_digest": task.task_descriptor_digest,
            "generation_mode": str(generation_mode),
            "placement_profile": str(placement_profile),
        }
        if generation_recovery is not None:
            task_contract["generation_recovery"] = (
                generation_recovery.to_dict())
        if conversation is not None:
            if not isinstance(conversation, ConversationContinuation):
                raise TypeError("conversation must be ConversationContinuation")
            input_digest = application_input.logical_input_digest
            if conversation.turn_input_digest \
                    and conversation.turn_input_digest != input_digest:
                raise ValueError("conversation turn input digest mismatch")
            contract_digest = conversation.request_contract(
                input_digest=input_digest)
            if (conversation.request_contract_digest
                    and conversation.request_contract_digest != contract_digest):
                raise ValueError("conversation request contract digest mismatch")
            # The checkpoint is an opaque authenticated capability.  It is
            # carried inside the request contract, never interpreted by the
            # planner and never copied into logs, manifests, or provider
            # selection input.
            task_contract["conversation"] = {
                **conversation.to_dict(),
                "requestContractDigest": contract_digest,
            }
        transport_mode = getattr(
            application_input.transport_mode, "value",
            str(application_input.transport_mode))
        if transport_mode == InputTransportMode.INLINE.value:
            encoded_input = base64.b64encode(
                application_input.payload).decode("ascii")
            input_reference = {}
        else:
            encoded_input = ""
            input_reference = dict(application_input.repo_reference or {})
        return DIRequestEnvelopeV2(
            invocation_id=invocation_id,
            request_id=request_id,
            attempt=int(attempt),
            service=service_name,
            model_name=model.model_name,
            model_identity_hash=model.intent_digest,
            task_kind=task.task_name,
            input_manifest_digest=input_manifest_digest,
            input_payload_b64=encoded_input,
            options_payload_b64=base64.b64encode(
                options_payload).decode("ascii"),
            plan_deadline_ms=deadline_ms,
            security_domain="requester-default",
            input_transport=transport_mode,
            input_reference=input_reference,
            model={
                "name": model.model_name,
                "identity_hash": model.intent_digest,
                "content_digest": model.content_digest,
                "semantics_digest": model.semantics_digest,
                "source_revision": model.source_revision,
            },
            task=task_contract,
        ).to_bytes()

    def _seal(
        self,
        ack_closed_digest: str,
        placement: PlacementRequest,
        decision: PlacementDecision,
        candidate: SplitCandidate,
        published: PublishedSplit,
        invocation_id: str,
        strategy_identity_digest: str,
        generation_mode: str = "TOKEN_DIAGNOSTIC",
        generation_recovery: GenerationRecoveryV1 | None = None,
    ) -> SealedCollaborationPlan:
        assignments = {item.role: item for item in decision.assignments}
        roles = []
        artifact_names = {}
        assignment_payloads = {}
        provider_views = {item.provider: item for item in placement.providers}
        for role in candidate.execution_plan.roles:
            artifact_name = published.artifact_data_names_by_role[role]
            artifact_names[role] = artifact_name
            roles.append(CollaborationRole(
                role=role,
                service=self.service_name,
                artifact=artifact_name,
                allow_dynamic_provisioning=False,
            ))
        dependencies = []
        committed_dependencies: list[DIDataDependencyV2] = []
        key_scopes: dict[str, tuple[str, ...]] = {}
        role_scopes: dict[str, list[str]] = {
            role: [] for role in candidate.execution_plan.roles
        }
        input_scopes_by_role: dict[str, list[str]] = {
            role: [] for role in candidate.execution_plan.roles
        }
        for index, dependency in enumerate(
                candidate.execution_plan.dependencies):
            scope = f"tensor-{index}-{canonical_digest(dependency)[7:23]}"
            dependencies.append(CollaborationDependency(
                producers=[dependency.producer],
                consumers=[dependency.consumer],
                key_scope=scope,
                topic_prefix="/activation",
                required=True,
            ))
            committed_dependencies.append(DIDataDependencyV2(
                producers=(dependency.producer,),
                consumers=(dependency.consumer,),
                key_scope=scope,
                topic_prefix="/activation",
                required=True,
                tensors=tuple(dependency.tensor_edges),
            ))
            key_scopes[scope] = (
                dependency.producer, dependency.consumer)
            role_scopes[dependency.producer].append(scope)
            role_scopes[dependency.consumer].append(scope)
            input_scopes_by_role[dependency.consumer].append(scope)
        if str(generation_mode).upper() == "FULL":
            # Full generation uses a bounded provider-to-provider token
            # control loop.  This scope is not a graph edge and therefore does
            # not change model splitting or topological graph identity.  All
            # selected roles receive it explicitly in the sealed plan so the
            # loop remains encrypted and request-scoped.
            key_scopes[GENERATION_CONTROL_SCOPE] = tuple(
                candidate.execution_plan.roles)
            for role in candidate.execution_plan.roles:
                role_scopes[role].append(GENERATION_CONTROL_SCOPE)
        plan_digest = canonical_digest({
            "ack_closed_digest": ack_closed_digest,
            "placement_input_digest": placement.digest(),
            "placement_decision_digest": decision.digest(),
            "strategy_identity_digest": strategy_identity_digest,
            "execution_policy": DATA_DRIVEN_V2,
            "candidate_digest": candidate.candidate_digest,
        })
        for provider in sorted(set(
                item.provider for item in assignments.values())):
            provider_roles = tuple(
                role for role in candidate.execution_plan.roles
                if assignments[role].provider == provider)
            view = provider_views[provider]
            layer_ranges = {
                role: self._role_layer_range(candidate, role)
                for role in provider_roles
            }
            dependencies_by_role = {
                role: tuple(
                    dependency for dependency in committed_dependencies
                    if role in dependency.producers + dependency.consumers)
                for role in provider_roles
            }
            role_tuple = tuple(
                DIRoleAssignmentV2(
                    role=role,
                    graph_node_id=",".join(sorted(
                        node for node, owner in
                        candidate.execution_plan.node_roles.items()
                        if owner == role)),
                    layer_start=layer_ranges[role][0],
                    layer_end=layer_ranges[role][1],
                    artifact_digest=candidate.artifacts_by_role[role][0],
                    dependency_digest=canonical_digest(
                        dependencies_by_role[role]),
                    adapter_id=candidate.model.adapter.name,
                    adapter_version=candidate.model.adapter.version,
                    dependencies=dependencies_by_role[role],
                    required_gpu_mib=assignments[
                        role].required_gpu_memory_mb,
                    backend=assignments[role].backend,
                    device=self._resolve_execution_device(
                        assignments[role], view),
                    input_grant_digests=(canonical_digest({
                        "role": role,
                        "request_id": placement.request_id,
                        "attempt": placement.attempt,
                    }),),
                    required_input_scopes=tuple(
                        input_scopes_by_role[role]),
                )
                for role in provider_roles
            )
            provider_assignment = DISelectionAssignmentV2(
                invocation_id=invocation_id,
                request_id=placement.request_id,
                attempt=placement.attempt,
                plan_digest=plan_digest,
                provider=provider,
                provider_boot_epoch=view.boot_epoch,
                offer_digest=view.offer_digest,
                resource_sequence=view.resource_sequence,
                roles=role_tuple,
                artifact_set_digest=canonical_digest({
                    role: candidate.artifacts_by_role[role]
                    for role in provider_roles
                }),
                dependency_graph_digest=canonical_contract_digest(
                    candidate.execution_plan),
                deadline_ms=placement.deadline_ms,
                generation=1,
                generation_recovery=generation_recovery,
                execution_policy=DATA_DRIVEN_V2,
            ).to_bytes()
            for role in provider_roles:
                assignment_payloads[role] = provider_assignment
        return SealedCollaborationPlan(
            ack_closed_digest=ack_closed_digest,
            placement_input_digest=placement.digest(),
            placement_decision_digest=decision.digest(),
            strategy_identity_digest=strategy_identity_digest,
            execution_policy=DATA_DRIVEN_V2,
            roles=tuple(roles),
            dependencies=tuple(dependencies),
            key_scopes=key_scopes,
            role_scopes={
                key: tuple(value) for key, value in role_scopes.items()
            },
            providers_by_role={
                role: assignment.provider
                for role, assignment in assignments.items()
            },
            artifact_data_names=artifact_names,
            scope_key_data_names={},
            assignment_payloads_by_role=assignment_payloads,
        )

    @staticmethod
    def _resolve_execution_device(
        assignment: ProviderAssignment,
        view: ProviderPlanningView,
    ) -> str:
        """Bind an external strategy decision to one signed ACK device."""

        if assignment.backend not in view.backends:
            raise ValueError(
                f"Provider {view.provider} did not offer backend "
                f"{assignment.backend}")
        device = assignment.device
        if not device:
            if is_cpu_backend(assignment.backend):
                device = "cpu"
            elif len(view.devices) == 1:
                device = view.devices[0]
            else:
                raise ValueError(
                    f"Provider {view.provider} assignment requires one exact "
                    "device from the signed ACK offer")
        if device == "cpu":
            if not is_cpu_backend(assignment.backend):
                raise ValueError("non-CPU backend cannot be assigned to CPU")
        elif device not in view.devices:
            raise ValueError(
                f"Provider {view.provider} did not offer device {device}")
        return device

    @staticmethod
    def _role_layer_range(
        candidate: SplitCandidate, role: str,
    ) -> tuple[int | None, int | None]:
        """Derive an exact half-open layer range from adapter graph node IDs."""
        layers = []
        for node, owner in candidate.execution_plan.node_roles.items():
            if owner != role:
                continue
            match = re.fullmatch(r"layer-(\d+)", node)
            if match is not None:
                layers.append(int(match.group(1)))
        if not layers:
            return None, None
        ordered = sorted(layers)
        if ordered != list(range(ordered[0], ordered[-1] + 1)):
            raise ValueError("role layer nodes do not form a contiguous range")
        return ordered[0], ordered[-1] + 1


def replan_placement_request(
    placement: PlacementRequest, *, at_ms: int,
    candidate_ids: tuple[str, ...],
    providers: tuple[ProviderPlanningView, ...],
) -> PlacementRequest:
    """Create a fresh-attempt immutable strategy input without extending time."""

    if at_ms >= placement.deadline_ms:
        raise TimeoutError("placement replan deadline expired")
    if not candidate_ids:
        raise ValueError("placement replan requires candidate coverage")
    return replace(
        placement,
        attempt=placement.attempt + 1,
        candidate_ids=tuple(candidate_ids),
        providers=tuple(providers),
    )


__all__ = [
    "AutomaticInferenceHandle",
    "AutomaticPlanningCoordinator",
    "CanonicalArtifactEnsurer",
    "encode_runtime_catalog_snapshot",
    "InferenceTaskRef",
    "ModelRef",
    "TaskOptions",
    "validate_spec175_ordinary_v3_proposal",
    "replan_placement_request",
]
