"""Journal-backed durable APP request handles."""

from __future__ import annotations

import hashlib
import base64
import json
import os
import time
import uuid
import asyncio
import warnings
from concurrent.futures import Future
from dataclasses import replace
from datetime import timedelta
from pathlib import Path

from ..client import (
    InferenceResult, SelectionAcceptanceTracker,
)
from .contracts import (
    DeploymentProgress,
    DeploymentStatus,
    ApplicationRuntimeConfig,
    GenerationConfig,
    GenerationInput,
    InferenceRequestHandle as RequestRecord,
    InferenceOptions,
    RequestTiming,
    RequestableDeployment,
    RequestEnvelopeReference,
    RequestHandle,
    ResultRendezvousRecord,
)
from ..core.execution_intent import ExecutionIntentCoordinator
from ..core.contracts import ExecutionActivateMessage
from .execution_control import ExistingServiceExecutionControlTransport
from .runtime_journal import (
    FileRequestEnvelopeKeyProvider,
    RequestEnvelopeKeyProvider,
    RuntimeJournal,
    RuntimeJournalError,
    RuntimeJournalKeyError,
    RuntimeJournalUnsafeRootError,
)
from .status import RequestEvent, RequestState


class RequestRecoveryError(RuntimeError):
    """Typed fail-closed request recovery error."""


class APPClient:
    def __init__(self, journal: RuntimeJournal, *, executor=None, engine=None,
                 observers=None, intent_coordinator=None, network_client=None,
                 requester_identity: str = "", execution_control_transport=None,
                 automatic_planner=None):
        if not journal.has_envelope_key:
            raise RuntimeJournalKeyError(
                "APPClient requires an owner-injected request-envelope key provider")
        self.journal = journal
        self.executor = executor
        self.engine = engine
        self.observers = observers
        self._network_client = network_client
        self._execution_control_transport = execution_control_transport
        self._automatic_planner = automatic_planner
        self._network_futures = {}
        self._result_cache: dict[str, bytes] = {}
        self._selection_acceptance_trackers: dict[
            tuple[str, int], SelectionAcceptanceTracker] = {}
        self.requester_identity = requester_identity or journal.root.name
        self.intent_coordinator = intent_coordinator or ExecutionIntentCoordinator()
        self._request_handles: dict[str, RequestHandle] = {}
        self._request_events_by_id: dict[str, list[dict]] = {}
        self._result_rendezvous_by_id: dict[str, dict] = {}
        for record in self.journal.records():
            payload = record["payload"]
            if record["kind"] in {"request-handle", "request-handle-update"}:
                handle = RequestRecord.from_record(payload)
                self._request_handles[handle.request_id] = handle
            elif record["kind"] == "request-event":
                self._request_events_by_id.setdefault(
                    str(payload["requestId"]), []).append(payload)
            elif record["kind"] == "result-rendezvous":
                self._result_rendezvous_by_id[str(payload["request_id"])] = payload

    @classmethod
    def from_application_runtime(
        cls,
        runtime: ApplicationRuntimeConfig,
        *,
        state_root: str | Path,
        envelope_key_provider: RequestEnvelopeKeyProvider | None = None,
        envelope_key_file: str | Path | None = None,
        optimization_suite=None,
        **network_options,
    ) -> "APPClient":
        """Connect from identity/routing config without generating a plan."""
        from ..client import DistributedInferenceClient
        from ..policy import DistributedInferenceDeployment
        from .facades import APPClient as NetworkAPPClient

        if envelope_key_provider is not None and envelope_key_file is not None:
            raise RuntimeJournalKeyError(
                "configure either an envelope key provider or key file, not both")
        if envelope_key_file is not None:
            envelope_key_provider = FileRequestEnvelopeKeyProvider(
                envelope_key_file)
        requester = runtime.identity
        namespace = "requester-" + hashlib.sha256(
            requester.encode("utf-8")).hexdigest()[:24]
        journal = RuntimeJournal(
            Path(state_root), namespace,
            envelope_key_provider=envelope_key_provider)
        deployment = DistributedInferenceDeployment(
            application=runtime.identity,
            controller=runtime.controller,
            group=runtime.group,
            user=runtime.identity,
            provider_prefix=runtime.service.rstrip("/") + "/provider",
            services=(),
            trust_schema=runtime.trust_schema,
            policy_file="",
        )
        native = DistributedInferenceClient.connect(
            group=runtime.group,
            controller=runtime.controller,
            user=runtime.identity,
            trust_schema=runtime.trust_schema,
            permission_wait_ms=int(network_options.pop(
                "permission_wait_ms", 2500)),
            async_workers=int(network_options.pop("async_workers", 4)),
            adaptive_admission=bool(network_options.pop(
                "adaptive_admission", False)),
        )
        if network_options:
            raise TypeError(
                "unknown application runtime options: "
                + ",".join(sorted(network_options)))
        facade = NetworkAPPClient(
            deployment, native, optimization_suite=optimization_suite)
        return cls(
            journal,
            engine=facade.optimization_engine,
            network_client=facade,
            requester_identity=requester,
        )

    @classmethod
    def from_config(
        cls,
        config: str | Path,
        *,
        state_root: str | Path | None = None,
        identity: str = "",
        envelope_key_provider: RequestEnvelopeKeyProvider | None = None,
        envelope_key_file: str | Path | None = None,
        test_only_allow_ephemeral_state_root: bool = False,
        automatic_planner=None,
        **network_options,
    ) -> "APPClient":
        """Construct the canonical client over the existing NDNSF runtime.

        ``facades.APPClient`` remains an internal network adapter.  Returning a
        canonical instance here prevents applications and MiniNDN roles from
        selecting a second public APPClient implementation.
        """
        from .facades import APPClient as NetworkAPPClient

        network_client = NetworkAPPClient.from_config(config, **network_options)
        requester = identity or str(network_client.deployment.user)
        namespace = "requester-" + hashlib.sha256(
            requester.encode("utf-8")).hexdigest()[:24]
        state_root = state_root or os.environ.get("NDNSF_DI_STATE_ROOT")
        if state_root is None:
            raise RuntimeJournalUnsafeRootError(
                "APPClient requires an explicit operator persistent state root")
        root = Path(state_root)
        envelope_key_file = (
            envelope_key_file or os.environ.get("NDNSF_DI_ENVELOPE_KEY_FILE"))
        if envelope_key_provider is not None and envelope_key_file is not None:
            raise RuntimeJournalKeyError(
                "configure either an envelope key provider or key file, not both")
        if envelope_key_file is not None:
            envelope_key_provider = FileRequestEnvelopeKeyProvider(
                envelope_key_file)
        if test_only_allow_ephemeral_state_root:
            if envelope_key_provider is None:
                journal = RuntimeJournal.for_test(root, namespace)
            else:
                journal = RuntimeJournal(
                    root,
                    namespace,
                    envelope_key_provider=envelope_key_provider,
                    test_only_allow_ephemeral_state_root=True,
                )
        else:
            journal = RuntimeJournal(
                root,
                namespace,
                envelope_key_provider=envelope_key_provider,
            )
        return cls(
            journal,
            engine=network_client.optimization_engine,
            network_client=network_client,
            requester_identity=requester,
            automatic_planner=automatic_planner,
        )

    @classmethod
    def for_deployment(
        cls,
        deployment,
        *,
        client=None,
        state_root: str | Path | None = None,
        identity: str = "offline-app-client",
        optimization_suite=None,
        envelope_key_provider: RequestEnvelopeKeyProvider | None = None,
        envelope_key_file: str | Path | None = None,
        test_only_allow_ephemeral_state_root: bool = False,
    ) -> "APPClient":
        """Build the canonical APP owner over an injected/offline network adapter."""
        from .facades import APPClient as NetworkAPPClient

        network_client = NetworkAPPClient(
            deployment, client, optimization_suite=optimization_suite)
        state_root = state_root or os.environ.get("NDNSF_DI_STATE_ROOT")
        if state_root is None:
            raise RuntimeJournalUnsafeRootError(
                "APPClient requires an explicit operator persistent state root")
        envelope_key_file = (
            envelope_key_file or os.environ.get("NDNSF_DI_ENVELOPE_KEY_FILE"))
        if envelope_key_provider is not None and envelope_key_file is not None:
            raise RuntimeJournalKeyError(
                "configure either an envelope key provider or key file, not both")
        if envelope_key_file is not None:
            envelope_key_provider = FileRequestEnvelopeKeyProvider(
                envelope_key_file)
        journal = (
            RuntimeJournal.for_test(state_root, identity)
            if test_only_allow_ephemeral_state_root and
            envelope_key_provider is None
            else RuntimeJournal(
                state_root,
                identity,
                envelope_key_provider=envelope_key_provider,
                test_only_allow_ephemeral_state_root=(
                    test_only_allow_ephemeral_state_root),
            )
        )
        return cls(
            journal,
            engine=network_client.optimization_engine,
            network_client=network_client,
            requester_identity=identity,
        )

    @property
    def deployment(self):
        if self._network_client is None:
            raise AttributeError("deployment")
        return self._network_client.deployment

    @property
    def optimization_engine(self):
        return self.engine

    def __getattr__(self, name):
        network_client = self.__dict__.get("_network_client")
        if network_client is not None:
            return getattr(network_client, name)
        raise AttributeError(name)

    def decide(self, requests):
        if self.engine is None:
            raise RuntimeError("APPClient requires an explicit optimization engine")
        return self.engine.run_decision_graph(requests)

    def request(
        self,
        *,
        model,
        task,
        input,
        timeout_ms: int,
        options=None,
        objective=None,
        constraints=None,
        request_id: str = "",
        strategy=None,
    ):
        """Run the canonical model/task-first deferred-planning lifecycle."""
        if self._automatic_planner is None:
            raise RuntimeError(
                "APPClient requires an AutomaticPlanningCoordinator")
        request_args = dict(
            model=model,
            task=task,
            input=input,
            timeout_ms=timeout_ms,
            options=options,
            objective=objective,
            constraints=constraints,
            request_id=request_id,
        )
        if strategy is not None:
            request_args["strategy"] = strategy
        return self._automatic_planner.request(**request_args)

    def generate(self, request):
        """Submit one complete-generation request through one invocation.

        The adapter/provider owns the internal autoregressive loop; this
        method never expands it into per-token NDNSF Requests.
        """
        if self._automatic_planner is None:
            raise RuntimeError(
                "APPClient requires an AutomaticPlanningCoordinator")
        return self._automatic_planner.generate(request)

    def configure_automatic_planning(
        self,
        *,
        service_name: str,
        adapters,
        strategy,
        catalog_snapshot_provider,
        verify_offer_signature,
        split_materializer=None,
        artifact_publisher=None,
        canonical_artifact_ensurer=None,
        budget=None,
        ack_timeout_ms: int = 300,
        ack_coverage_roles=(),
        ack_coverage_predicate=None,
    ):
        """Attach the canonical deferred planner after the network client exists."""
        if self._network_client is None:
            raise RuntimeError("automatic planning requires a network client")
        from .placement import (
            AutomaticPlanningCoordinator,
            CatalogSnapshotArtifactPublisher,
            RejectGeneratedSplitMaterializer,
            v2_provider_view_factory,
            v3_provider_view_factory,
        )
        snapshot_provider = catalog_snapshot_provider or (lambda: ())
        placement_profile = str(
            getattr(strategy, "placement_profile", "DI_PLACEMENT_V2"))
        if placement_profile == "DI_PLACEMENT_V3":
            provider_factory = v3_provider_view_factory(
                verify_offer_signature)
        else:
            provider_factory = v2_provider_view_factory(
                verify_offer_signature)
        coordinator = AutomaticPlanningCoordinator(
            service_user=self._network_client.service_user,
            service_name=service_name,
            adapters={
                adapter.descriptor.name: adapter for adapter in adapters
            },
            strategy=strategy,
            provider_view_factory=provider_factory,
            split_materializer=(
                split_materializer or RejectGeneratedSplitMaterializer()),
            artifact_publisher=(
                artifact_publisher
                or CatalogSnapshotArtifactPublisher(snapshot_provider)),
            canonical_artifact_ensurer=canonical_artifact_ensurer,
            catalog_snapshot_provider=snapshot_provider,
            budget=budget,
            ack_timeout_ms=ack_timeout_ms,
            ack_coverage_roles=tuple(ack_coverage_roles),
            ack_coverage_predicate=ack_coverage_predicate,
        )
        self._automatic_planner = coordinator
        return coordinator

    def track_selection_acceptance(
        self, *, request_id: str, attempt: int, deadline_ms: int,
        encryption_key: bytes,
    ) -> SelectionAcceptanceTracker:
        """Create/reuse the encrypted V2 final-Selection control journal."""
        key = (request_id, int(attempt))
        current = self._selection_acceptance_trackers.get(key)
        if current is not None:
            if current.deadline_ms != int(deadline_ms):
                raise ValueError(
                    "Selection acceptance tracker deadline is immutable")
            return current
        tracker = SelectionAcceptanceTracker(
            request_id=request_id, attempt=attempt,
            deadline_ms=deadline_ms, encryption_key=encryption_key)
        self._selection_acceptance_trackers[key] = tracker
        return tracker

    def prepare_intent(self, intent, validator=lambda value: None):
        prepared = self.intent_coordinator.prepare(intent)
        self.intent_coordinator.revalidate(prepared.intent_id, validator)
        return self.intent_coordinator.commit(prepared.intent_id)

    def emit_outcome(self, outcome, *, idempotency_key: str, timeout_ms: int = 100):
        if self.observers is None:
            return {}
        return self.observers.deliver(outcome, idempotency_key, timeout_ms)

    def distributed_inference(
        self,
        service,
        value,
        *,
        deployment_revision: str,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30_000,
        freshness_ms: int = 60_000,
        dynamic_provisioning=None,
        runtime=None,
        artifact_references=None,
        role_app_requirements=None,
        request_id: str = "",
    ) -> InferenceResult:
        """Synchronous compatibility view over the durable submit identity."""
        return self.async_distributed_inference(
            service,
            value,
            deployment_revision=deployment_revision,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            freshness_ms=freshness_ms,
            dynamic_provisioning=dynamic_provisioning,
            runtime=runtime,
            artifact_references=artifact_references,
            role_app_requirements=role_app_requirements,
            request_id=request_id,
        ).result(timeout=max(1.0, (timeout_ms + 1_000) / 1000.0))

    def async_distributed_inference(
        self,
        service,
        value,
        *,
        deployment_revision: str,
        ack_timeout_ms: int = 500,
        timeout_ms: int = 30_000,
        freshness_ms: int = 60_000,
        dynamic_provisioning=None,
        runtime=None,
        artifact_references=None,
        role_app_requirements=None,
        on_result=None,
        on_error=None,
        request_id: str = "",
    ) -> Future:
        """Process-local Future adapter over one durable request handle."""
        options = {
            "ack_timeout_ms": ack_timeout_ms,
            "timeout_ms": timeout_ms,
            "freshness_ms": freshness_ms,
            "dynamic_provisioning": dynamic_provisioning,
            "runtime": runtime,
            "artifact_references": artifact_references,
            "role_app_requirements": role_app_requirements,
            "deployment_revision": deployment_revision,
        }
        options = {key: item for key, item in options.items()
                   if item is not None}
        public_future = Future()
        try:
            handle = self.submit(
                service=service,
                input=value,
                deployment_revision=deployment_revision,
                deadline=(timeout_ms + 1_000) / 1000.0,
                inference_options=options,
                request_id=request_id,
            )
            native_future = self._network_futures.get(handle.request_id)

            def finish(_native=None):
                if public_future.cancelled():
                    self.cancel(handle)
                    return
                state = self.status(handle)
                if state == RequestState.COMPLETED:
                    result = self._completed_inference_result(handle)
                else:
                    events = self._events(handle.request_id)
                    reason = (events[-1].get("reasonCode", state.value)
                              if events else state.value)
                    result = InferenceResult(
                        False, b"", reason, handle.request_id)
                public_future.set_result(result)
                if on_result is not None:
                    on_result(result)

            if native_future is None:
                finish()
            else:
                native_future.add_done_callback(finish)
            public_future.add_done_callback(
                lambda item: self.cancel(handle) if item.cancelled() else None)
        except Exception as exc:
            public_future.set_exception(exc)
            if on_error is not None:
                on_error(exc)
        return public_future

    def infer(
        self, plan, payload, *, deployment_revision: str,
        ack_timeout_ms: int = 500, timeout_ms: int = 30_000,
        freshness_ms: int = 60_000,
    ) -> InferenceResult:
        """Plan-level synchronous adapter over one durable request identity."""
        return self.infer_async(
            plan, payload, deployment_revision=deployment_revision,
            ack_timeout_ms=ack_timeout_ms, timeout_ms=timeout_ms,
            freshness_ms=freshness_ms,
        ).result(timeout=max(1.0, (timeout_ms + 1_000) / 1000.0))

    def infer_async(
        self, plan, payload, *, deployment_revision: str,
        ack_timeout_ms: int = 500, timeout_ms: int = 30_000,
        freshness_ms: int = 60_000, on_result=None, on_error=None,
    ) -> Future:
        """Plan-level Future adapter over one durable request identity."""
        if self._network_client is None:
            raise RuntimeError("network infer requires APPClient.from_config")
        service = str(getattr(plan, "service", ""))
        if not service:
            raise ValueError("inference plan requires a service identity")
        public_future = Future()
        try:
            handle = self.submit(
                service=service,
                input=payload,
                deployment_revision=deployment_revision,
                deadline=(timeout_ms + 1_000) / 1000.0,
                inference_options={
                    "ack_timeout_ms": ack_timeout_ms,
                    "timeout_ms": timeout_ms,
                    "freshness_ms": freshness_ms,
                },
                _network_submitter=lambda encoded, **options:
                    self._network_client.infer_async(plan, encoded, **options),
            )
            native_future = self._network_futures.get(handle.request_id)

            def finish(_native=None):
                if public_future.cancelled():
                    self.cancel(handle)
                    return
                state = self.status(handle)
                if state == RequestState.COMPLETED:
                    result = self._completed_inference_result(handle)
                else:
                    events = self._events(handle.request_id)
                    reason = (events[-1].get("reasonCode", state.value)
                              if events else state.value)
                    result = InferenceResult(False, b"", reason, handle.request_id)
                public_future.set_result(result)
                if on_result is not None:
                    on_result(result)

            if native_future is None:
                finish()
            else:
                native_future.add_done_callback(finish)
            public_future.add_done_callback(
                lambda item: self.cancel(handle) if item.cancelled() else None)
        except Exception as exc:
            public_future.set_exception(exc)
            if on_error is not None:
                on_error(exc)
        return public_future

    def submit(
        self,
        deployment_id: str = "",
        revision: str = "",
        payload: bytes | None = None,
        *,
        service: str = "",
        input=None,
        deployment_revision: str = "",
        objective=None,
        deadline=None,
        ttl_ms: int = 60_000,
        inference_options: dict | None = None,
        request_id: str = "",
        _network_submitter=None,
        _encoded_input: bytes | None = None,
    ) -> RequestHandle:
        """Durably bind one APP request to one NDNSF wire request identity.

        The positional form remains the bounded local compatibility adapter.
        The canonical keyword form requires a configured network client and
        injects the durable request ID into the existing NDNSF request path.
        """
        now = int(time.time() * 1000)
        request_id = str(request_id or uuid.uuid4().hex)
        if (
            len(request_id) > 256
            or not request_id
            or any(
                not (character.isalnum() or character in "._:-")
                for character in request_id
            )
        ):
            raise ValueError(
                "request_id must be 1-256 characters using letters, digits, "
                "dot, underscore, colon, or hyphen"
            )
        if request_id in self._request_handles or self._events(request_id):
            raise ValueError(f"request_id already exists: {request_id}")
        if deadline is None:
            expires_at_ms = now + int(ttl_ms)
        elif hasattr(deadline, "timestamp"):
            expires_at_ms = int(deadline.timestamp() * 1000)
        else:
            numeric_deadline = float(deadline)
            if numeric_deadline >= 1_000_000_000_000:
                expires_at_ms = int(numeric_deadline)
            elif numeric_deadline >= 1_000_000_000:
                expires_at_ms = int(numeric_deadline * 1000)
            else:
                expires_at_ms = now + int(numeric_deadline * 1000)
        if expires_at_ms <= now:
            raise ValueError("request deadline must be in the future")

        network_submission = bool(service)
        if network_submission:
            if self._network_client is None:
                raise RuntimeError("network submit requires APPClient.from_config")
            revision = deployment_revision or revision
            if not revision:
                raise ValueError("network submit requires deployment_revision")
            payload_bytes = (
                bytes(_encoded_input) if _encoded_input is not None else
                self._network_client.encode_input(service, input))
            deployment_id = deployment_id or service
        else:
            if payload is None:
                raise ValueError("local submit requires payload")
            payload_bytes = bytes(payload)

        prepared_envelope = self.journal.prepare_envelope(
            request_id, payload_bytes, expires_at_ms=expires_at_ms)
        digest = prepared_envelope.wire_digest
        reference = RequestEnvelopeReference(
            requester_identity=self.requester_identity,
            request_id=request_id,
            locator=f"journal:protected-envelope/{request_id}",
            wire_digest=digest,
            security_context="AES-256-GCM:ndnsf-di-protected-request-v3",
            expires_at_ms=expires_at_ms,
            retention_owner=self.requester_identity,
        )
        context = {
            "requestId": request_id,
            "service": service,
            "objectiveDigest": (
                "sha256:" + hashlib.sha256(
                    json.dumps(objective, sort_keys=True, default=str).encode()
                ).hexdigest() if objective is not None else ""
            ),
        }
        initial_states = [RequestState.CREATED, RequestState.PLANNING]
        if network_submission:
            initial_states.append(RequestState.PREPARING)
        handle = RequestRecord(
            request_id=request_id,
            deployment_id=deployment_id,
            revision=revision,
            created_at_ms=now,
            expires_at_ms=expires_at_ms,
            envelope_digest=digest,
            requester_identity=self.requester_identity,
            attempt_epoch=1,
            envelope_reference=reference,
            event_cursor=len(initial_states),
            service_name=service,
        )
        self._append_events(
            handle, [(state, "", "") for state in initial_states],
            extra_entries=(
                ("request-handle", handle.to_record()),
                ("request-context", context),
            ),
            prepared_envelope=prepared_envelope,
        )
        self._request_handles[request_id] = handle
        if network_submission:
            options = dict(inference_options or {})
            options.setdefault("timeout_ms", max(1, expires_at_ms - now))
            options.update({
                "request_id": request_id,
                "on_result": lambda result: self._network_result(handle, result),
                "on_error": lambda error: self._event(
                    handle, RequestState.FAILED, reason=type(error).__name__),
            })
            try:
                if _network_submitter is None:
                    future = self._network_client.async_distributed_inference(
                        service, payload_bytes, **options)
                else:
                    future = _network_submitter(payload_bytes, **options)
                self._network_futures[request_id] = future
            except Exception as exc:
                self._event(handle, RequestState.FAILED, reason=type(exc).__name__)
        elif self.executor is not None:
            try:
                self._event(handle, RequestState.EXECUTING)
                result = bytes(self.executor(payload))
                prepared_result = self.journal.prepare_envelope(
                    request_id + "-result", result,
                    expires_at_ms=now+ttl_ms)
                self._complete_request(
                    handle, result, prepared_result.wire_digest,
                    wire_request_id=request_id,
                    transitions=(RequestState.COMPLETED,),
                    network_result=None,
                    prepared_envelope=prepared_result)
                self._result_cache[request_id] = result
            except Exception as exc:
                self._event(handle, RequestState.FAILED, reason=type(exc).__name__)
        return handle

    def bind_execution_activation(
        self, handle: RequestHandle, activation: ExecutionActivateMessage,
    ) -> RequestHandle:
        """Durably bind USER-authorized exact ReadySet activation."""
        current = self._request_handles.get(handle.request_id, handle)
        if (activation.requester_identity != current.requester_identity
                or activation.request_id != current.request_id
                or activation.attempt != current.attempt_epoch
                or not activation.members):
            raise RequestRecoveryError("EXECUTION_ACTIVATION_BINDING_MISMATCH")
        wire = activation.to_bytes()
        digest = activation.digest()
        if current.activation_digest:
            if current.activation_digest != digest:
                raise RequestRecoveryError("EXECUTION_ACTIVATION_CONFLICT")
            return current
        updated = replace(
            current,
            activation_digest=digest,
            execution_activation_wire=base64.b64encode(wire).decode("ascii"),
            event_cursor=len(self._events(current.request_id)) + 1,
        )
        self._append_events(
            updated, [(RequestState.CERTIFIED, "", "")],
            extra_entries=(("request-handle-update", updated.to_record()),))
        self._request_handles[current.request_id] = updated
        return updated

    def _execution_activation(
        self, handle: RequestHandle,
    ) -> ExecutionActivateMessage:
        if not handle.activation_digest or not handle.execution_activation_wire:
            raise RequestRecoveryError("EXECUTION_ACTIVATION_MISSING")
        try:
            activation = ExecutionActivateMessage.from_bytes(
                base64.b64decode(handle.execution_activation_wire, validate=True))
        except Exception as exc:
            raise RequestRecoveryError("EXECUTION_ACTIVATION_INVALID") from exc
        if activation.digest() != handle.activation_digest:
            raise RequestRecoveryError("EXECUTION_ACTIVATION_DIGEST_MISMATCH")
        if (activation.requester_identity != handle.requester_identity
                or activation.request_id != handle.request_id
                or activation.attempt != handle.attempt_epoch):
            raise RequestRecoveryError("EXECUTION_ACTIVATION_BINDING_MISMATCH")
        return activation

    def _network_result(self, handle: RequestHandle, result) -> None:
        # The callback closes over the initially submitted handle.  Certificate
        # binding may advance the durable handle while the request is in flight,
        # so terminal processing must use the latest journal-backed identity.
        handle = self._request_handles.get(handle.request_id, handle)
        wire_request_id = str(getattr(result, "request_id", "")).lstrip("/")
        if wire_request_id != handle.request_id:
            self._event(
                handle, RequestState.FAILED,
                reason="WIRE_REQUEST_ID_MISMATCH")
            return
        binding = {
            "requestId": handle.request_id,
            "wireRequestId": wire_request_id,
        }
        if not bool(getattr(result, "status", False)):
            self._append_events(
                handle,
                [(RequestState.FAILED, "", str(getattr(
                    result, "error", "NETWORK_FAILURE"))[:160])],
                extra_entries=(("request-wire-binding", binding),),
            )
            return
        payload = bytes(getattr(result, "payload", b""))
        try:
            prepared_result = self.journal.prepare_envelope(
                handle.request_id + "-result", payload,
                expires_at_ms=handle.expires_at_ms)
        except Exception:
            self._event(
                handle, RequestState.FAILED,
                reason="RESULT_PERSISTENCE_FAILED")
            return
        if handle.activation_digest:
            self._execution_activation(handle)
            current_state = self.status(handle)
            if current_state == RequestState.CERTIFIED:
                transitions = (RequestState.EXECUTING, RequestState.COMPLETED)
            elif current_state == RequestState.EXECUTING:
                transitions = (RequestState.COMPLETED,)
            else:
                self._event(
                    handle, RequestState.FAILED,
                    reason="EXECUTION_ACTIVATION_STATE_MISMATCH")
                return
        else:
            transitions = (
                RequestState.CERTIFIED,
                RequestState.EXECUTING,
                RequestState.COMPLETED,
            )
        try:
            self._complete_request(
                handle, payload, prepared_result.wire_digest,
                wire_request_id=wire_request_id,
                transitions=transitions,
                extra_entries=(("request-wire-binding", binding),),
                network_result=result,
                prepared_envelope=prepared_result,
            )
        except (RuntimeJournalError, OSError):
            self._event(
                handle, RequestState.FAILED,
                reason="RESULT_PERSISTENCE_FAILED")
            return
        self._result_cache[handle.request_id] = payload

    def _complete_request(
        self, handle, payload, protected_digest, *, wire_request_id,
        transitions, extra_entries=(), network_result=None,
        prepared_envelope=None,
    ):
        terminal_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
        rendezvous = ResultRendezvousRecord(
            requester_identity=handle.requester_identity,
            request_id=handle.request_id,
            attempt_epoch=handle.attempt_epoch,
            activation_digest=handle.activation_digest,
            output_epoch=1,
            terminal_digest=terminal_digest,
            protected_wire_digest=protected_digest,
            locator=(
                f"journal:protected-envelope/{handle.request_id}-result"),
            terminal_state=RequestState.COMPLETED.value,
            expires_at_ms=handle.expires_at_ms,
            provider_result_data_name=str(
                getattr(network_result, "data_name", "")),
            signer_certificate=str(
                getattr(network_result, "signer_certificate", "")),
            network_wire_digest=str(
                getattr(network_result, "wire_digest", "")),
        )
        rendezvous_payload = rendezvous.__dict__
        rendezvous_digest = "sha256:" + hashlib.sha256(
            json.dumps(
                rendezvous_payload, sort_keys=True,
                separators=(",", ":"),
            ).encode()
        ).hexdigest()
        updated = replace(
            handle,
            result_rendezvous_digest=rendezvous_digest,
            event_cursor=len(self._events(handle.request_id)) + len(transitions),
            terminal_evidence_digest=terminal_digest,
        )
        transition_rows = [
            (state, terminal_digest if state == RequestState.COMPLETED else "", "")
            for state in transitions
        ]
        self._append_events(
            updated,
            transition_rows,
            extra_entries=(
                *extra_entries,
                ("result-rendezvous", rendezvous_payload),
                ("request-handle-update", updated.to_record()),
            ),
            prepared_envelope=prepared_envelope,
        )
        self._request_handles[handle.request_id] = updated
        self._result_rendezvous_by_id[handle.request_id] = rendezvous_payload

    def open_request(
        self, request_id: str, attempt_epoch: int = None,
    ) -> RequestHandle:
        try:
            handle = self._request_handles[request_id]
        except KeyError as exc:
            raise KeyError("request handle missing") from exc
        if (attempt_epoch is not None and
                int(attempt_epoch) != handle.attempt_epoch):
            raise RequestRecoveryError("ATTEMPT_EPOCH_MISMATCH")
        if handle.requester_identity != self.requester_identity:
            raise RequestRecoveryError("REQUESTER_IDENTITY_MISMATCH")
        if self.status(handle) not in {
                RequestState.COMPLETED, RequestState.FAILED,
                RequestState.CANCELLED, RequestState.EXPIRED}:
            try:
                reference = handle.envelope_reference
                if reference is None:
                    raise ValueError("request envelope reference missing")
                self.journal.read_envelope(handle.request_id)
                if self.journal.envelope_digest(
                        handle.request_id) != reference.wire_digest:
                    raise ValueError("request envelope digest mismatch")
            except Exception as exc:
                self._event(
                    handle, RequestState.FAILED,
                    reason="REQUEST_ENVELOPE_UNAVAILABLE")
                raise RequestRecoveryError(
                    "REQUEST_ENVELOPE_UNAVAILABLE") from exc
        return self._request_handles[request_id]

    def status(self, handle: RequestHandle) -> RequestState:
        events = self._events(handle.request_id)
        return RequestState(events[-1]["state"]) if events else RequestState.CREATED

    def wait(self, handle: RequestHandle, timeout_ms: int = 1000) -> RequestState:
        deadline = time.monotonic()+timeout_ms/1000
        terminal = {RequestState.COMPLETED, RequestState.FAILED,
                    RequestState.CANCELLED, RequestState.EXPIRED}
        while time.monotonic()<deadline:
            state=self.status(handle)
            if state in terminal: return state
            time.sleep(.01)
        return self.status(handle)

    def result(self, handle: RequestHandle) -> bytes:
        if self.status(handle) != RequestState.COMPLETED:
            raise RuntimeError("request has no successful result")
        cached = self._result_cache.get(handle.request_id)
        if cached is not None:
            return cached
        current = self._request_handles.get(handle.request_id, handle)
        rendezvous = self._result_rendezvous_by_id.get(handle.request_id)
        if not current.result_rendezvous_digest or rendezvous is None:
            raise RequestRecoveryError("RESULT_RENDEZVOUS_MISSING")
        digest = "sha256:" + hashlib.sha256(json.dumps(
            rendezvous, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        if digest != current.result_rendezvous_digest:
            raise RequestRecoveryError("RESULT_RENDEZVOUS_MISMATCH")
        payload = self.journal.read_envelope(handle.request_id+"-result")
        if ("sha256:" + hashlib.sha256(payload).hexdigest() !=
                str(rendezvous["terminal_digest"])):
            raise RequestRecoveryError("RESULT_DIGEST_MISMATCH")
        return payload

    def _completed_inference_result(self, handle: RequestHandle) -> InferenceResult:
        rendezvous = self._result_rendezvous_by_id.get(handle.request_id, {})
        return InferenceResult(
            True,
            self.result(handle),
            "",
            handle.request_id,
            str(rendezvous.get("provider_result_data_name", "")),
            str(rendezvous.get("signer_certificate", "")),
            str(rendezvous.get("network_wire_digest", "")),
        )

    def cancel(
        self, handle: RequestHandle, reason: str = "",
        attempt_epoch: int = None,
    ) -> None:
        current = self._request_handles.get(handle.request_id, handle)
        if (attempt_epoch is not None and
                int(attempt_epoch) != current.attempt_epoch):
            raise RequestRecoveryError("ATTEMPT_EPOCH_MISMATCH")
        if current.requester_identity != self.requester_identity:
            raise RequestRecoveryError("REQUESTER_IDENTITY_MISMATCH")
        state = self.status(current)
        if state in {
                RequestState.COMPLETED, RequestState.FAILED,
                RequestState.CANCELLED, RequestState.EXPIRED}:
            return
        cancellation_id = "sha256:" + hashlib.sha256(
            (f"{handle.request_id}:{current.attempt_epoch}:"
             f"{current.activation_digest}:{reason}").encode()
        ).hexdigest()
        updated = replace(
            current,
            cancellation_id=cancellation_id,
            cancellation_reason=str(reason)[:160],
            event_cursor=len(self._events(handle.request_id)) + 1,
        )
        if current.activation_digest:
            activation = self._execution_activation(current)
            if not current.service_name:
                raise RequestRecoveryError("EXECUTION_CONTROL_SERVICE_MISSING")
            transport = self._execution_control_transport
            if transport is None and self._network_client is not None:
                transport = ExistingServiceExecutionControlTransport(
                    self._network_client)
            if transport is None:
                raise RequestRecoveryError("EXECUTION_CONTROL_TRANSPORT_MISSING")
            request_record = {
                "schema": "ndnsf-di-execution-control-v2",
                "operation": "CANCEL",
                "requestId": current.request_id,
                "attemptEpoch": current.attempt_epoch,
                "requesterIdentity": current.requester_identity,
                "activationDigest": current.activation_digest,
                "cancellationId": cancellation_id,
                "reason": str(reason)[:160],
            }
            self.journal.append("request-cancellation-request", request_record)
            evidence_records = []
            seen_providers = set()
            for member in activation.members:
                if member.provider in seen_providers:
                    continue
                seen_providers.add(member.provider)
                payload = {
                    **request_record,
                    "providerName": member.provider,
                    "providerRole": member.role,
                }
                try:
                    evidence = transport.cancel(
                        member.provider,
                        current.service_name,
                        payload,
                        timeout_ms=max(1, min(5000, current.expires_at_ms - int(time.time() * 1000))),
                    )
                except Exception as exc:
                    self.journal.append("request-cancellation-failure", {
                        **request_record,
                        "provider": member.provider,
                        "reason": type(exc).__name__,
                    })
                    raise RequestRecoveryError("CANCELLATION_INCOMPLETE") from exc
                if (
                    not evidence.accepted
                    or evidence.operation != "CANCEL"
                    or evidence.provider != member.provider
                    or evidence.service_name != current.service_name
                    or evidence.requester_identity != current.requester_identity
                    or evidence.request_id != current.request_id
                    or evidence.attempt_epoch != current.attempt_epoch
                    or evidence.activation_digest != current.activation_digest
                    or evidence.cancellation_id != cancellation_id
                ):
                    self.journal.append("request-cancellation-failure", {
                        **request_record,
                        "provider": member.provider,
                        "reason": "EVIDENCE_BINDING_MISMATCH",
                    })
                    raise RequestRecoveryError("CANCELLATION_INCOMPLETE")
                evidence_records.append((
                    "execution-control-evidence", evidence.to_record()))
            self._append_events(
                updated,
                [(RequestState.CANCELLED, "", str(reason)[:160])],
                extra_entries=(
                    *evidence_records,
                    ("request-handle-update", updated.to_record()),
                ),
            )
        else:
            self._append_events(
                updated,
                [(RequestState.CANCELLED, "", str(reason)[:160])],
                extra_entries=((
                    "request-handle-update", updated.to_record()),),
            )
        self._request_handles[handle.request_id] = updated
        future = self._network_futures.get(handle.request_id)
        if future is not None:
            future.cancel()

    def stream(self, handle: RequestHandle):
        for event in self._events(handle.request_id): yield RequestEvent(
            handle.request_id, event["sequence"], RequestState(event["state"]),
            event["timestampMs"], event.get("resultDigest", ""), event.get("reasonCode", ""))

    def prepare_session(self, plan, *args, **kwargs):
        if self._network_client is not None:
            return self._network_client.deploy_plan(plan, *args, **kwargs)
        if self.executor is None or not hasattr(self.executor, "deploy_plan"):
            return plan
        return self.executor.deploy_plan(plan, *args, **kwargs)

    def deploy_plan(self, plan, *args, **kwargs):
        """Bounded compatibility alias for :meth:`prepare_session`."""
        return self.prepare_session(plan, *args, **kwargs)

    def collaboration_status(self, request_id: str, *, timeout_ms: int = 500):
        if self._network_client is None:
            return ()
        return self._network_client.collaboration_status(
            request_id, timeout_ms=timeout_ms)

    def _events(self, request_id):
        return list(self._request_events_by_id.get(request_id, ()))

    def _event(self, handle, state, result_digest="", reason=""):
        self._append_events(
            handle, [(RequestState(state), result_digest, reason)])

    def _append_events(
        self, handle, transitions, *, extra_entries=(),
        prepared_envelope=None,
    ):
        events = self._events(handle.request_id)
        current = RequestState(events[-1]["state"]) if events else None
        terminal = {
            RequestState.COMPLETED, RequestState.FAILED,
            RequestState.CANCELLED, RequestState.EXPIRED,
        }
        order = {
            RequestState.CREATED: 0,
            RequestState.PLANNING: 1,
            RequestState.PREPARING: 2,
            RequestState.CERTIFIED: 3,
            RequestState.EXECUTING: 4,
            RequestState.COMPLETED: 5,
        }
        payloads = []
        for state, result_digest, reason in transitions:
            state = RequestState(state)
            if current in terminal:
                if current == state and len(transitions) == 1:
                    return ()
                raise RuntimeError("durable request state is already terminal")
            if (current is not None and state not in terminal and
                    order.get(state, -1) <= order.get(current, -1)):
                raise ValueError("durable request state must advance monotonically")
            payloads.append({
                "requestId": handle.request_id,
                "sequence": len(events) + len(payloads) + 1,
                "state": state.value,
                "timestampMs": int(time.time() * 1000),
                "resultDigest": result_digest,
                "reasonCode": reason,
            })
            current = state
        entries = (
            *extra_entries,
            *(("request-event", payload) for payload in payloads),
        )
        if prepared_envelope is None:
            self.journal.append_many(entries)
        else:
            self.journal.commit_prepared_envelope(
                prepared_envelope, entries)
        self._request_events_by_id.setdefault(
            handle.request_id, []).extend(payloads)
        return tuple(payloads)


class RequestCatalog:
    """Rebind durable request identities to the existing APP journal."""

    def __init__(self, client: "InferenceClient"):
        self._client = client

    def get(self, ref: RequestRecord) -> "InferenceRequestHandle":
        if not isinstance(ref, RequestRecord):
            raise TypeError("request reference must be RequestRef")
        record = self._client._core.open_request(
            ref.request_id, attempt_epoch=ref.attempt_epoch)
        if record != ref:
            # The saved reference may precede later monotonic journal updates,
            # but immutable identity/fence fields must remain identical.
            immutable = (
                "request_id", "deployment_id", "revision",
                "requester_identity", "attempt_epoch", "envelope_digest",
            )
            if any(getattr(record, name) != getattr(ref, name)
                   for name in immutable):
                raise RequestRecoveryError("REQUEST_REFERENCE_BINDING_MISMATCH")
        deployment = self._client._deployment_for_record(record)
        return InferenceRequestHandle(self._client, record, deployment)


class InferenceRequestHandle:
    """Bound user view over one durable APPClient request record."""

    def __init__(self, client: "InferenceClient", record: RequestRecord,
                 deployment):
        self._client = client
        self._record = record
        self._deployment = deployment

    @property
    def ref(self) -> RequestRecord:
        return self._client._core._request_handles.get(
            self._record.request_id, self._record)

    def status(self) -> RequestState:
        return self._client._core.status(self.ref)

    def deployment_status(self):
        self._client._refresh_deployment_status(self.ref, self._deployment)
        return self._deployment.status()

    def events(self, *, after=None):
        cursor = int(after or 0)
        for event in self._client._core.stream(self.ref):
            if event.sequence > cursor:
                yield event

    @staticmethod
    def _wait_ms(wait_timeout, record: RequestRecord) -> int:
        if wait_timeout is None:
            return max(1, record.expires_at_ms - int(time.time() * 1000))
        if not isinstance(wait_timeout, timedelta):
            raise TypeError("wait_timeout must be datetime.timedelta")
        if wait_timeout.total_seconds() <= 0:
            raise ValueError("wait_timeout must be positive")
        return max(1, int(wait_timeout.total_seconds() * 1000))

    def wait(self, *, wait_timeout: timedelta | None = None) -> RequestState:
        return self._client._core.wait(
            self.ref, timeout_ms=self._wait_ms(wait_timeout, self.ref))

    def result(self, *, wait_timeout: timedelta | None = None) -> InferenceResult:
        state = self.wait(wait_timeout=wait_timeout)
        if state != RequestState.COMPLETED:
            raise RuntimeError(f"inference request terminated as {state.value}")
        return self._client._core._completed_inference_result(self.ref)

    async def result_async(
        self, *, wait_timeout: timedelta | None = None,
    ) -> InferenceResult:
        return await asyncio.to_thread(self.result, wait_timeout=wait_timeout)

    def cancel(self, reason: str = "user-requested") -> RequestState:
        self._client._core.cancel(self.ref, reason=reason)
        return self.status()


class InferenceClient:
    """Requester-only canonical NDNSF-DI API.

    This class is composition, not a second client implementation: request
    persistence, wire publication, cancellation, and result recovery remain in
    the existing ``APPClient`` owner.
    """

    def __init__(self, core: APPClient, *, deployments=None,
                 request_submitter=None):
        from .deployment import DeploymentCatalog

        self._core = core
        self._deployments = deployments or DeploymentCatalog(
            owner_identity=core.requester_identity, journal=core.journal)
        self._request_submitter = request_submitter
        self._request_deployments = {}
        self._request_definitions = {}
        self._requests = RequestCatalog(self)

    @classmethod
    def from_application_config(
        cls, runtime: ApplicationRuntimeConfig, *, state_root,
        envelope_key_file=None, envelope_key_provider=None,
        optimization=None, **network_options,
    ):
        core = APPClient.from_application_runtime(
            runtime,
            state_root=state_root,
            envelope_key_file=envelope_key_file,
            envelope_key_provider=envelope_key_provider,
            optimization_suite=optimization,
            **network_options,
        )
        return cls(core)

    @classmethod
    def from_config(cls, config, *, state_root,
                    envelope_key_file=None, envelope_key_provider=None,
                    optimization=None):
        core = APPClient.from_config(
            config, state_root=state_root,
            envelope_key_file=envelope_key_file,
            envelope_key_provider=envelope_key_provider,
            optimization_suite=optimization)
        from .deployment import (
            DeploymentCatalog, NetworkDeploymentCatalogTransport,
        )

        service_user = core._network_client._client.user
        transport = NetworkDeploymentCatalogTransport(service_user)
        catalog = DeploymentCatalog(
            owner_identity=core.requester_identity,
            journal=core.journal,
            definition_publisher=transport.publish_definition,
            definition_fetcher=transport.fetch_definition,
            activation_publisher=transport.publish_activation,
            activation_fetcher=transport.fetch_activation,
            discovery_hints=transport.discovery_hints,
        )
        client = cls(core, deployments=catalog)
        client._deployment_transport = transport
        return client

    @property
    def deployments(self):
        return self._deployments

    @property
    def requests(self):
        return self._requests

    def deploy(self, definition):
        return self._deployments.ensure(definition)

    @staticmethod
    def _encode_remote_input(value) -> bytes:
        if isinstance(value, bytes):
            return value
        if isinstance(value, (bytearray, memoryview)):
            return bytes(value)
        if isinstance(value, str):
            return value.encode("utf-8")
        try:
            return json.dumps(
                value, sort_keys=True, separators=(",", ":")).encode("utf-8")
        except (TypeError, ValueError) as exc:
            raise TypeError(
                "remote coordinator input must be bytes, text, or JSON-compatible") from exc

    def _remote_coordinator_submitter(
        self, definition_ref, deadline_utc, effective_options,
    ):
        from .coordinator import submit_via_coordinator

        service_user = self._core._network_client._client.user

        def submit(payload: bytes, **network_options):
            on_result = network_options.get("on_result")
            on_error = network_options.get("on_error")
            future = submit_via_coordinator(
                service_user,
                definition_ref.coordinator_service,
                definition_ref,
                payload,
                deadline=deadline_utc,
                options=effective_options,
                outer_request_id=str(network_options.get("request_id", "")),
            )

            def finish(item):
                try:
                    result = item.result()
                    if on_result is not None:
                        on_result(result)
                except Exception as exc:
                    if on_error is not None:
                        on_error(exc)

            future.add_done_callback(finish)
            return future

        return submit

    def request_model(
        self, *, model, input: GenerationInput,
        generation: GenerationConfig, strategy=None, request_id: str = "",
    ):
        if self._core._automatic_planner is None:
            raise RuntimeError(
                "model-first request requires an AutomaticPlanningCoordinator")
        return self._core._automatic_planner.request_application(
            model=model, input=input, generation=generation,
            strategy=strategy, request_id=request_id)

    def request_preplanned(
        self, deployment: RequestableDeployment, *, input,
        timeout=None, deadline=None, options: InferenceOptions | None = None,
    ) -> InferenceRequestHandle:
        return self._request_impl(
            deployment, input=input, timeout=timeout, deadline=deadline,
            options=options, authorized_coordinator="")

    def request(
        self, deployment: RequestableDeployment, *, input,
        timeout=None, deadline=None, options: InferenceOptions | None = None,
    ) -> InferenceRequestHandle:
        warnings.warn(
            "InferenceClient.request(deployment, ...) is preplanned compatibility; "
            "use request_preplanned() or InferenceApplication.request(model=...)",
            DeprecationWarning, stacklevel=2)
        return self.request_preplanned(
            deployment, input=input, timeout=timeout, deadline=deadline,
            options=options)

    def _request_as_coordinator(
        self, deployment: RequestableDeployment, *, input,
        deadline, options: InferenceOptions, coordinator_service: str,
    ) -> InferenceRequestHandle:
        return self._request_impl(
            deployment, input=input, deadline=deadline, options=options,
            authorized_coordinator=coordinator_service)

    def _request_impl(
        self, deployment: RequestableDeployment, *, input,
        timeout=None, deadline=None, options: InferenceOptions | None = None,
        authorized_coordinator: str = "",
    ) -> InferenceRequestHandle:
        timing = RequestTiming(timeout=timeout, deadline=deadline)
        definition, revision = self._deployments.resolve_definition(deployment)
        if (authorized_coordinator and
                definition.coordinator_service != authorized_coordinator):
            raise PermissionError("definition is bound to another coordinator")
        deployment_handle = self._deployments.ensure(deployment)
        effective_options = options or InferenceOptions()
        if not isinstance(effective_options, InferenceOptions):
            raise TypeError("options must be InferenceOptions")
        deadline_utc = timing.deadline_utc()
        if self._request_submitter is None:
            remote = (
                definition.application_identity != self._core.requester_identity and
                definition.coordinator_service != authorized_coordinator)
            definition_ref = self._deployments.definition_ref(definition)
            record = self._core.submit(
                service=definition.service,
                input=input,
                deployment_id=definition.deployment_id,
                deployment_revision=revision,
                deadline=deadline_utc,
                objective=definition.objective,
                inference_options={
                    "metadata": dict(effective_options.metadata),
                    "output_encoding": effective_options.output_encoding,
                },
                _encoded_input=(
                    self._encode_remote_input(input) if remote else None),
                _network_submitter=(
                    self._remote_coordinator_submitter(
                        definition_ref, deadline_utc, effective_options)
                    if remote else None),
            )
        else:
            record = self._request_submitter(
                self._core, definition, revision, input, deadline_utc,
                effective_options)
        if not isinstance(record, RequestRecord):
            raise TypeError("request path did not return the durable request record")
        self._request_deployments[record.request_id] = deployment_handle.handle_ref
        self._request_definitions[record.request_id] = definition
        return InferenceRequestHandle(self, record, deployment_handle)

    def _refresh_deployment_status(self, record: RequestRecord, handle) -> None:
        current = handle.status()
        if current.state in {"ACTIVE", "READY", "FAILED", "CANCELLED", "EXPIRED"}:
            return
        snapshots = self._core.collaboration_status(
            record.request_id, timeout_ms=500)
        if not snapshots:
            return
        definition = self._request_definitions.get(record.request_id)
        if definition is None:
            try:
                definition, _ = self._deployments.resolve_definition(
                    handle.handle_ref)
            except (LookupError, PermissionError, ValueError):
                return
        progress = []
        certificate_rows = []
        terminal_state = ""
        terminal_reason = ""
        coordinator_epoch = 1
        for snapshot in snapshots:
            for member in snapshot.member_statuses:
                if member.details_schema not in {
                        "ndnsf-di-preparation-progress-v1",
                        "ndnsf-di-coordinator-progress-v1"}:
                    continue
                try:
                    details = json.loads(member.details_payload.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    continue
                if details.get("deploymentRevision") != record.revision:
                    continue
                if member.details_schema == "ndnsf-di-coordinator-progress-v1":
                    if (snapshot.provider_name != definition.deployment_owner or
                            snapshot.service_name != definition.coordinator_service or
                            details.get("deploymentOwner") != definition.deployment_owner or
                            details.get("applicationIdentity") != definition.application_identity or
                            details.get("coordinatorService") != definition.coordinator_service or
                            details.get("definitionDigest") != definition.digest()):
                        continue
                    try:
                        coordinator_epoch = int(details.get("coordinatorEpoch", 1))
                        projected = [DeploymentProgress.from_dict(item)
                                     for item in details.get("roles", ())]
                    except (TypeError, ValueError):
                        continue
                    if any(item.request_id not in {record.request_id, "ensure"} or
                           item.revision != record.revision for item in projected):
                        continue
                    progress.extend(projected)
                    state = str(details.get("state", "PREPARING"))
                    if state in {"FAILED", "CANCELLED", "EXPIRED"}:
                        terminal_state = state
                        terminal_reason = member.reason_code
                    readiness_digest = str(
                        details.get("readinessCertificateDigest", ""))
                    if state in {"READY", "ACTIVE"} and readiness_digest.startswith(
                            "sha256:"):
                        certificate_rows.append({
                            "provider": snapshot.provider_name,
                            "role": "coordinator",
                            "selectionDigest": snapshot.selection_digest,
                            "attempt": member.attempt,
                            "epoch": member.epoch,
                            "sequence": member.sequence,
                            "readinessCertificateDigest": readiness_digest,
                            "details": details,
                        })
                    continue
                phase = str(details.get("phase", ""))
                if (phase not in {
                        "ACCEPTED", "FETCHING", "VERIFYING", "LOADING",
                        "WARMING", "READY", "FAILED", "CANCELLED", "EXPIRED"} or
                        (phase == "READY" and not details.get("adapter"))):
                    continue
                item = DeploymentProgress(
                    request_id=record.request_id,
                    attempt=member.attempt,
                    revision=record.revision,
                    role=member.role,
                    provider=member.provider_name,
                    operation_id=member.operation_id,
                    phase=phase,
                    sequence=member.sequence,
                    progress=member.progress if member.progress_known else 0.0,
                    reason=member.reason_code,
                )
                progress.append(item)
                if phase == "READY":
                    certificate_rows.append({
                        "provider": member.provider_name,
                        "role": member.role,
                        "selectionDigest": snapshot.selection_digest,
                        "attempt": member.attempt,
                        "epoch": member.epoch,
                        "sequence": member.sequence,
                        "details": details,
                    })
                elif phase in {"FAILED", "CANCELLED", "EXPIRED"}:
                    terminal_state = phase
                    terminal_reason = member.reason_code
        deduplicated = {}
        for item in progress:
            key = (item.role, item.provider)
            previous = deduplicated.get(key)
            if previous is None or (item.attempt, item.sequence) > (
                    previous.attempt, previous.sequence):
                deduplicated[key] = item
        progress = list(deduplicated.values())
        if terminal_state:
            self._deployments.record_status(
                handle.handle_ref,
                DeploymentStatus(
                    terminal_state, record.revision,
                    tuple(sorted(progress, key=lambda item: (
                        item.role, item.provider))),
                    coordinator_epoch=coordinator_epoch,
                    reason=terminal_reason))
            return
        required = int(getattr(definition.constraints, "minimum_providers", 1))
        ready = [item for item in progress if item.phase.value == "READY"]
        unique = {(item.role, item.provider) for item in ready}
        expected_roles = set(definition.roles)
        observed_roles = {item.role for item in ready}
        if (len(unique) < required or
                (expected_roles and not expected_roles.issubset(observed_roles)) or
                not certificate_rows):
            if progress:
                self._deployments.record_status(
                    handle.handle_ref,
                    DeploymentStatus(
                        "PREPARING", record.revision,
                        tuple(sorted(progress, key=lambda item: (
                            item.role, item.provider))),
                        coordinator_epoch=coordinator_epoch))
            return
        digest = "sha256:" + hashlib.sha256(json.dumps(
            sorted(certificate_rows, key=lambda item: (
                item["role"], item["provider"])),
            sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        self._deployments.record_status(
            handle.handle_ref,
            DeploymentStatus(
                "READY", record.revision,
                tuple(sorted(progress, key=lambda item: (
                    item.role, item.provider))),
                digest, coordinator_epoch=coordinator_epoch))

    def run(self, deployment: RequestableDeployment, *, input,
            timeout=None, deadline=None,
            options: InferenceOptions | None = None) -> InferenceResult:
        return self.request(
            deployment, input=input, timeout=timeout, deadline=deadline,
            options=options).result()

    async def run_async(self, deployment: RequestableDeployment, *, input,
                        timeout=None, deadline=None,
                        options: InferenceOptions | None = None) -> InferenceResult:
        return await self.request(
            deployment, input=input, timeout=timeout, deadline=deadline,
            options=options).result_async()

    def _deployment_for_record(self, record: RequestRecord):
        ref = self._request_deployments.get(record.request_id)
        if ref is None:
            from .contracts import DeploymentHandleRef
            locator = f"journal:deployment/{record.deployment_id}/{record.revision}"
            ref = DeploymentHandleRef(
                deployment_id=record.deployment_id,
                revision=record.revision,
                lifecycle_epoch=1,
                owner_identity=self._core.requester_identity,
                journal_locator=locator,
                journal_digest="sha256:" + hashlib.sha256(locator.encode()).hexdigest(),
            )
        return self._deployments.get(ref)


__all__ = [
    "APPClient", "InferenceClient", "InferenceRequestHandle",
    "RequestCatalog", "RequestRecoveryError",
]
