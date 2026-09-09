#!/usr/bin/env python3
"""User for the real YOLO layout distributed inference example."""

from __future__ import annotations

import hashlib
import json
from dataclasses import replace
from ndnsf_distributed_inference.app_sdk import APPClient, ProviderOfferTrustVerifier
from pathlib import Path
import os
import re
import sys
import time

from ndnsf_distributed_inference.adapters import ApplicationInput
from ndnsf_distributed_inference.adapters.yolo import (
    YoloCanonicalArtifactBinding,
    build_yolo26n_adapter,
)
from ndnsf_distributed_inference.adapters.yolo.reference import (
    compare_reference, load_reference,
)
from ndnsf_distributed_inference.app_sdk.placement import (
    InferenceTaskRef,
    ModelRef,
    NetworkCatalogSnapshotResolver,
    TaskOptions,
)
from ndnsf_distributed_inference.planner.presplit_first import PreSplitFirstStrategy

from yolo_2x2_lib import (
    DEFAULT_MODEL,
    DEFAULT_INPUT_SIZE,
    YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
    YOLO_PARALLEL_DETECT_REPLICATED_BACKBONE_SEMANTICS,
    YOLO_PARALLEL_OUTPUT_SEMANTICS,
    compare_yolo_outputs,
    decode_yolo_output,
    decode_image,
    encode_image_for_yolo,
    encode_native_tensor_bundle,
    full_forward,
    make_input,
    optional_local_nfd,
    parse_args_with_common,
    run_local_onnx_pipeline,
    run_local_parallel_detect_scale_pipeline,
    run_local_parallel_output_pipeline,
    runtime_spec,
    yolo_inference_service,
)


_YN_NEGATIVE_BOUNDARIES = {
    "Y-N-C": ("PLACEMENT_DECISION", "NO_FEASIBLE_CANDIDATE"),
    "Y-N-P": ("ACK_CLOSED", "ACK_PROVENANCE_REJECTED"),
    "Y-N-R": ("PLAN_SEALED", "ROLE_KIND_REJECTED"),
    "Y-N-I": ("PROVIDER_EXECUTION_STARTED", "NON_INGRESS_INPUT_REJECTED"),
    # spec181 T006: the three real grant mutations must be verifier-rejected
    # before this subcase may record the registered PASS.
    "Y-N-E": ("PROVIDER_GRANT_VERIFICATION", "DI_PROTECTED_GRANT_REJECTED"),
    "Y-N-L": ("EVIDENCE_ACCEPTANCE", "REDACTION_REJECTED"),
}
# Child exit code, kept identical to the runner's Y-N constant: 91 = a
# registered negative PASS (every subcase, including the T006 Y-N-E).


def _spec180_y_n_mutation(args) -> str:
    mutation = os.environ.get("SPEC180_YN_MUTATION", "").strip()
    if not mutation:
        return ""
    if mutation not in _YN_NEGATIVE_BOUNDARIES:
        raise RuntimeError("SPEC180_YN_MUTATION_INVALID")
    if str(args.lifecycle_case) != mutation:
        raise RuntimeError("SPEC180_YN_MUTATION_CASE_MISMATCH")
    return mutation


def _spec180_negative_matches(mutation: str, error: Exception, journal) -> bool:
    """Accept only the named production rejection at its observed phase.

    Unknown errors retain their failure status. In particular an absent grant
    implementation, transport timeout or internal strategy error is not a
    successful safety control.
    """
    phase = journal.last_milestone
    if mutation == "Y-N-C":
        # The coordinator includes per-candidate rejections. Every rejection
        # must be the planner's actual lack of a distinct feasible Provider;
        # the coordinator also wraps internal strategy errors in ValueError.
        detail = r"sha256:[0-9a-f]{64}:ValueError:no distinct feasible Provider for V3 role [A-Za-z0-9_/#.-]+"
        return (type(error) is ValueError and phase == "GRAPH_READY" and
                re.fullmatch(r"V3 strategy found no feasible graph candidate \("
                             + detail + r"(?:; " + detail + r")*\)", str(error)) is not None)
    expected = {
        "Y-N-P": ("GRAPH_READY", ValueError,
                  "Provider ACK failed Trust Schema verification"),
        "Y-N-R": ("PLACEMENT_DECISION", ValueError,
                  "range/rank role requires a non-empty layer interval"),
        "Y-N-L": ("INPUT_REFERENCE_PUBLISHED", RunnerError,
                  "LIFECYCLE_FIELD_FORBIDDEN:payload"),
    }.get(mutation)
    if mutation == "Y-N-E":
        # Only the selected Provider's actual verifier record can prove this.
        return False
    return (expected is not None and phase == expected[0]
            and type(error) is expected[1] and str(error) == expected[2])


def _emit_spec180_y_n_negative(args, mutation: str, *, journal) -> None:
    if mutation == "Y-N-E":
        raise RuntimeError("Y_N_E_REQUIRES_PROVIDER_VERIFIER_RECORD")
    if (journal.request_id != args.request_id
            or args.lifecycle_case != mutation or not journal.attempt_id):
        raise RuntimeError("SPEC180_NEGATIVE_IDENTITY_MISMATCH")
    boundary, reason = _YN_NEGATIVE_BOUNDARIES[mutation]
    print(
        "SPEC180_YN_NEGATIVE_RESULT status=PASS"
        f" subcase={mutation} boundary={boundary} reason={reason}"
        f" requestId={journal.request_id} attemptId={journal.attempt_id}"
        f" observedPhase={journal.last_milestone}",
        flush=True,
    )


class _Spec180MutationBinding:
    """Keep Y-N mutation at the maintained post-ACK binding seam."""

    def __init__(self, binding, mutation: str) -> None:
        self._binding = binding
        self._mutation = mutation

    def describe(self, candidate):
        return self._binding.describe(candidate)

    def ensure(self, candidate, role_specs, *, deadline_ms: int):
        if self._mutation == "Y-N-R":
            specs = tuple(role_specs)
            if not specs:
                raise RuntimeError("DI_ROLE_KIND_REJECTED")
            # Constructing this invalid range through the real RoleAssemblySpec
            # validator exercises the production role contract before the
            # canonical artifact publisher is allowed to fetch/compute.
            replace(specs[0], role_kind="PIPELINE_RANGE",
                    layer_begin=0, layer_end=0)
        return self._binding.ensure(
            candidate, role_specs, deadline_ms=deadline_ms)


# The case-owned journal implementation lives with the Spec180 runner so the
# evidence schema and redaction rules have one source of truth.  The runner is
# import-safe (MiniNDN is loaded only when its runtime adapter is invoked), and
# the explicit path keeps the maintained example usable from the repository
# checkout and from the sealed image.
_REPO_ROOT = Path(__file__).resolve().parents[4]
_EXPERIMENTS_ROOT = _REPO_ROOT / "Experiments"
if str(_EXPERIMENTS_ROOT) not in sys.path:
    sys.path.insert(0, str(_EXPERIMENTS_ROOT))
from NDNSF_DI_YoloAckDriven_Minindn import (  # noqa: E402
    LifecycleJournal,
    MILESTONES,
    RunnerError,
)


def _prepare_yolo_input(args, package):
    reference = load_reference(package, _REPO_ROOT, args.input_size)
    if not args.native_tensor_input:
        raise ValueError("ACK_DRIVEN_REQUIRES_NATIVE_TENSOR_INPUT")
    payload = encode_native_tensor_bundle({"images": reference.input_tensor})
    if args.input_payload_file:
        if Path(args.input_payload_file).expanduser().read_bytes() != payload:
            raise ValueError("REGISTERED_FIXTURE_PAYLOAD_MISMATCH")
    return reference, payload


def _record_yolo_numerical_result(args, reference, payload, plan_digest, attempt_id):
    """Check the received bytes, emitting a fresh, payload-free component record."""
    record = {
        "schemaVersion": "spec180-yolo-numerical-v1",
        "case": args.lifecycle_case, "requestId": args.request_id,
        "attemptId": attempt_id, "planDigest": plan_digest,
        "manifestDigest": reference.manifest_digest,
        "oracleDigest": reference.oracle_digest,
        "fixtureDigest": reference.fixture_digest,
        "inputTensorDigest": "sha256:" + hashlib.sha256(reference.input_tensor.tobytes()).hexdigest(),
        "responseDigest": "sha256:" + hashlib.sha256(payload).hexdigest(),
    }
    # The terminal collector must be able to bind this component oracle to the
    # immutable Tiger candidate.  These values come from the sealed workload's
    # environment; they are intentionally absent from standalone local probes.
    candidate_id = os.environ.get("SPEC180_CANDIDATE_ID", "")
    candidate_digest = os.environ.get("SPEC180_CANDIDATE_DIGEST", "")
    if candidate_id:
        record["candidateId"] = candidate_id
    if candidate_digest:
        record["candidateDigest"] = candidate_digest
    try:
        _, actual = decode_yolo_output(payload, native_predictions_only=True)
        record.update(compare_reference(reference, actual))
    except (ValueError, KeyError, UnicodeError, OverflowError):
        record.update(matched=False, reason="INVALID_NUMERICAL_RESPONSE")
    path = Path(args.lifecycle_output_dir) / "yolo-numerical.json"
    with path.open("x", encoding="utf-8") as stream:
        json.dump(record, stream, sort_keys=True, allow_nan=False)
        stream.write("\n")
    return record["matched"]


def _digest_json(value) -> str:
    return "sha256:" + hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")).hexdigest()


def _make_lifecycle_observer(journal: LifecycleJournal):
    """Bridge coordinator transitions into the bound case journal."""
    def observe(milestone: str, fields: dict[str, object]) -> None:
        values = dict(fields)
        request_id = str(values.pop("_requestId", ""))
        attempt_id = str(values.pop("_attemptId", ""))
        if request_id != journal.request_id or attempt_id != journal.attempt_id:
            raise RunnerError("LIFECYCLE_PROTOCOL_IDENTITY_MISMATCH")
        journal.append(milestone, **values)
    return observe


def _build_grant_seam(client, *, registry_path, model_manifest_digest,
                      model_family="YOLO26n"):
    """Build the in-process authority grant seam for a protected epoch.

    SPEC181_PROTECTION_EPOCH is empty or ``plaintext-v1`` by default and the
    seam stays absent.  A protected epoch (spec181 T001/T002 wiring) loads
    the operator authority key from the Spec180 config registry, derives the
    requester signing key from the request-envelope seed, maps every
    Provider identity to an Ed25519 or EC P-256 recipient public key, owns
    one content key per model-manifest digest,
    and publishes grant Data through the client's signed-APP-Data path.
    """
    epoch = os.environ.get("SPEC181_PROTECTION_EPOCH", "").strip()
    mutation = ""
    if os.environ.get("SPEC180_YN_MUTATION") == "Y-N-E":
        from ndnsf_distributed_inference.security.grant_mutations import (
            GRANT_MUTATION_VARIANTS)
        mutation = os.environ.get("SPEC181_GRANT_MUTATION", "").strip()
        if mutation not in GRANT_MUTATION_VARIANTS:
            raise ValueError("GRANT_MUTATION_INVALID")
        if not epoch or epoch == "plaintext-v1":
            raise ValueError("GRANT_MUTATION_REQUIRES_PROTECTED_EPOCH")
    if not epoch or epoch == "plaintext-v1":
        return None, "plaintext-v1"
    from ndnsf_distributed_inference.security.registry_keys import (
        load_artifact_policy_authority_private_key,
        load_artifact_policy_authority_registry, load_ed25519_private_key,
        load_grant_recipient_private_key)
    from ndnsf_distributed_inference.security.requester_grant_pipeline import (
        build_in_process_grant_provider)
    policy = load_artifact_policy_authority_registry(
        registry_path, model_family=model_family, protection_epoch=epoch)
    authority_key = load_artifact_policy_authority_private_key(
        expected_public_key=policy.public_key)
    def current_manifest():
        value = model_manifest_digest() if callable(model_manifest_digest) else model_manifest_digest
        if (not isinstance(value, str) or not value.startswith("sha256:")
                or len(value) != 71
                or any(c not in "0123456789abcdef" for c in value[7:])):
            raise ValueError("grant policy requires a canonical model manifest digest")
        return value

    if not callable(model_manifest_digest):
        current_manifest()
    requester_seed_path = Path(os.environ["SPEC181_REQUESTER_PRIVATE_KEY"])
    requester_key = load_ed25519_private_key(requester_seed_path, raw_seed=True)
    recipient_map_path = Path(os.environ["SPEC181_PROVIDER_RECIPIENT_KEY_MAP"])
    recipient_entries = json.loads(recipient_map_path.read_text(
        encoding="utf-8"))
    recipient_public_keys = {}
    for provider, pem_path in recipient_entries.items():
        key = load_grant_recipient_private_key(pem_path)
        recipient_public_keys[provider] = key.public_key()
    content_keys: dict[tuple[str, str], bytes] = {}

    def content_key_owner(model_manifest_digest: str, protection_epoch: str):
        if (model_manifest_digest != current_manifest() or protection_epoch != epoch):
            raise ValueError("content key request is outside the configured model/epoch")
        identity = (model_manifest_digest, protection_epoch)
        if identity not in content_keys:
            content_keys[identity] = os.urandom(32)
        return content_keys[identity]

    service_user = getattr(client._network_client, "service_user", None)
    if service_user is None:
        raise RuntimeError("grant publisher requires the ServiceUser owner")
    requester_identity = str(client.deployment.user)
    if not requester_identity.startswith("/") or requester_identity.endswith("/"):
        raise ValueError("grant requester identity must be a canonical absolute name")

    def publisher(data_name: str, payload: bytes) -> None:
        result = service_user.publish_signed_app_data(
            data_name, payload, freshness_ms=600000)
        if getattr(result, "error", "") or not getattr(result, "success", False):
            raise RuntimeError(
                f"grant Data publish failed: {getattr(result, 'error', '')}")

    mutated = False

    def provider(grant_view, deadline_ms):
        nonlocal mutated
        pending = []
        mutate_this_grant = bool(mutation and not mutated)
        # Canonical publication replaces the package digest with the published
        # root digest before grant acquisition. Read that trusted final binding,
        # never the candidate grant view, to form the one-model allowlist.
        bound_provider = build_in_process_grant_provider(
            requester_identity=requester_identity,
            requester_private_key=requester_key,
            authority_identity=policy.authority_id,
            publication_identity=requester_identity,
            authority_key_id=policy.key_id,
            authority_private_key=authority_key,
            protection_epoch=epoch,
            allowed_model_manifests=frozenset({current_manifest()}),
            recipient_public_keys=recipient_public_keys.get,
            content_key_owner=content_key_owner,
            publisher=(lambda name, wire: pending.append((name, wire)))
            if mutate_this_grant else publisher,
        )
        binding = bound_provider(grant_view, deadline_ms)
        if mutate_this_grant:
            from ndnsf_distributed_inference.core.protected_artifacts import (
                grant_from_wire, grant_to_wire)
            from ndnsf_distributed_inference.security.grant_mutations import (
                mutate_for_provider_publication)
            from ndnsf_distributed_inference.security.grant_provider import (
                canonical_grant_name)
            if len(pending) != 1 or pending[0][0] != binding.grant_name:
                raise RuntimeError("GRANT_MUTATION_PUBLICATION_MISMATCH")
            grant = mutate_for_provider_publication(
                grant_from_wire(pending[0][1]), variant=mutation,
                authority_private_key=authority_key,
                content_key=content_key_owner(current_manifest(), epoch),
                now_ms=int(time.time() * 1000))
            name = canonical_grant_name(
                authority=requester_identity, provider_identity=binding.provider,
                request_id=binding.request_id, attempt=binding.attempt,
                plan_core_digest=binding.plan_core_digest,
                model_manifest_digest=grant.model_manifest_digest,
                protection_epoch=epoch, grant_digest=grant.grant_digest)
            binding = replace(binding, grant_name=name, grant_digest=grant.grant_digest)
            publisher(name, grant_to_wire(grant))
            mutated = True
            print("SPEC181_GRANT_MUTATION_PUBLISHED " + json.dumps({
                "variant": mutation, "provider": binding.provider,
                "requestId": binding.request_id,
                "attemptId": f"attempt-{binding.attempt}",
                "planCoreDigest": binding.plan_core_digest,
                "grantDigest": binding.grant_digest}, sort_keys=True), flush=True)
        return binding
    return provider, epoch


def _load_yolo_ack_driven(client, args) -> int:
    """Run the maintained model-first YOLO path.

    This path deliberately accepts only a canonical package, an authenticated
    input reference, a registered ACK-offer verifier, and an exact-name signed
    catalogue APP Data record.  It never accepts a Provider list, a split ID,
    a role map, or the legacy ``auto_parallel_detect_plan`` result as planning
    authority.  The historical service-policy invocation remains available
    only through ``--offline-oracle``.
    """
    required = {
        "--canonical-package": args.canonical_package,
        "--catalogue-registry": args.catalogue_registry,
        "--offer-trust-root": args.offer_trust_root,
        "--offer-public-key-map": args.offer_public_key_map,
        "--catalog-data-name": args.catalog_data_name,
        "--catalog-signer": args.catalog_signer,
    }
    missing = [name for name, value in required.items() if not value]
    if missing:
        raise RuntimeError(
            "ACK-driven YOLO mode requires " + ", ".join(missing) +
            "; use --offline-oracle only for the legacy offline path")
    if int(args.ack_timeout_ms) != 1500:
        raise RuntimeError(
            "Spec180 YOLO qualification requires ack-timeout-ms=1500")
    if args.input_reference_file:
        raise RuntimeError(
            "ACK-driven YOLO does not accept a bare input-reference-file; "
            "publish the source payload through the canonical APPClient")

    if (not args.lifecycle_output_dir or not args.lifecycle_case
            or not args.request_id):
        raise RuntimeError(
            "ACK-driven YOLO requires explicit lifecycle output, case, and request ID")
    if not str(args.request_id).startswith("/"):
        raise RuntimeError("ACK-driven YOLO request ID must be an absolute NDN name")
    mutation = _spec180_y_n_mutation(args)
    journal = LifecycleJournal(
        Path(args.lifecycle_output_dir).expanduser().resolve(),
        str(args.lifecycle_case),
    )
    try:
        journal.bind_protocol_identity(
            request_id=str(args.request_id), attempt_id="attempt-1")
    except RunnerError as exc:
        raise RuntimeError(str(exc)) from exc

    service = yolo_inference_service(client.deployment)
    package = Path(args.canonical_package).expanduser().resolve()
    registry = Path(args.catalogue_registry).expanduser().resolve()
    adapter = build_yolo26n_adapter(package, registry_path=registry)
    manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    source = manifest.get("source")
    graph = manifest.get("graph")
    if not isinstance(source, dict) or not isinstance(graph, dict):
        raise RuntimeError("YOLO canonical manifest lacks source/graph identity")
    model_digest = str(source.get("checkpointSha256", ""))
    if not model_digest.startswith("sha256:"):
        model_digest = "sha256:" + model_digest
    semantics_digest = "sha256:" + hashlib.sha256(json.dumps(
        {
            "preprocessing": manifest.get("preprocessing", {}),
            "postprocessing": manifest.get("postprocessing", {}),
        }, sort_keys=True, separators=(",", ":"),
    ).encode("utf-8")).hexdigest()
    model = ModelRef(
        model_name="YOLO26n",
        content_digest=model_digest,
        semantics_digest=semantics_digest,
        source_revision=str(manifest.get("graphRevision", "")),
    )
    task = InferenceTaskRef.from_adapter(adapter)
    numerical_reference, input_payload = _prepare_yolo_input(args, package)
    reference = client.publish_application_input_reference(
        service,
        input_payload,
        object_label="inference-input-image",
        object_type="application/x-ndnsf-di-input+native-tensor",
        freshness_ms=120000,
    )
    reference_digest = "sha256:" + hashlib.sha256(json.dumps(
        reference, sort_keys=True, separators=(",", ":"),
    ).encode("utf-8")).hexdigest()
    app_input = ApplicationInput.from_repo_ref(
        task_name=task.task_name,
        input_schema_digest=adapter.descriptor.input_schema_digest,
        options_schema_digest=adapter.descriptor.options_schema_digest,
        reference=reference,
        options=b"{}",
        metadata={"publicationDigest": reference_digest},
    )
    journal.append(
        "INPUT_REFERENCE_PUBLISHED", referenceDigest=reference_digest)
    if mutation == "Y-N-L":
        try:
            # Deliberately route a plaintext-bearing field through the real
            # evidence writer.  LifecycleJournal must reject it before any
            # terminal response can be accepted.
            journal.append("REQUEST_SENT", payload="redacted-test")
        except RunnerError as exc:
            if not _spec180_negative_matches(mutation, exc, journal):
                raise
            _emit_spec180_y_n_negative(args, mutation, journal=journal)
            return 91
        raise RuntimeError("DI_REDACTION_ACCEPTED")

    service_user = client._network_client._client.user
    snapshots = NetworkCatalogSnapshotResolver(
        service_user.fetch_signed_app_data,
        data_name=args.catalog_data_name,
        expected_signer=args.catalog_signer,
        timeout_ms=args.catalog_timeout_ms,
    )
    if args.selection_offer_key_map:
        raise RuntimeError(
            "--selection-offer-key-map is a fixture-only HMAC input and "
            "cannot be used by the maintained ACK-driven path")
    key_paths = json.loads(
        Path(args.offer_public_key_map).read_text(encoding="utf-8"))
    if not isinstance(key_paths, dict) or not key_paths:
        raise RuntimeError("offer public-key map must be a non-empty object")
    offer_keys = {
        str(key_id): Path(path).expanduser().read_bytes()
        for key_id, path in key_paths.items()
    }

    def trust_schema_was_validated(ack) -> bool:
        # The C++ ServiceUser sets this only after the configured Trust Schema
        # accepted the received ACK Data.  A Python-side name/key guess is not
        # an authentication substitute.
        if mutation == "Y-N-P":
            return False
        return bool(getattr(ack, "trust_schema_validated", False))

    offer_verifier = ProviderOfferTrustVerifier.from_json(
        args.offer_trust_root,
        offer_keys,
        trust_schema_verifier=trust_schema_was_validated,
    )

    # The canonical binding certifies Provider-local assembly recipes: each
    # role carries its certified node cover and interface contracts in the
    # sealed Selection, and every Provider assembles its own subgraph from
    # the validated canonical root after Selection.
    canonical_model_descriptor = adapter.describe_model(
        model.model_name, model.content_digest, model.semantics_digest,
        source_revision=model.source_revision)
    yolo_graph = adapter.graph.inspect(canonical_model_descriptor)
    canonical_binding = YoloCanonicalArtifactBinding(
        package_dir=package,
        adapter=adapter,
        model=canonical_model_descriptor,
        graph=yolo_graph,
        artifact_root=str(args.catalog_signer).rstrip("/") + "/NDNSF/DI/ARTIFACT",
        publish_encrypted_artifact=(
            lambda payload, *, object_label, object_type:
            client.publish_application_input_reference(
                service,
                payload,
                object_label=object_label,
                object_type=object_type,
                freshness_ms=120000,
            )
        ),
    )
    grant_model_binding = canonical_binding
    if mutation == "Y-N-R":
        canonical_binding = _Spec180MutationBinding(canonical_binding, mutation)

    grant_binding_provider, protection_epoch = _build_grant_seam(
        client, registry_path=registry, model_family=model.model_name,
        model_manifest_digest=lambda: grant_model_binding.model_manifest_digest)
    client.configure_automatic_planning(
        service_name=service,
        adapters=(adapter,),
        strategy=PreSplitFirstStrategy(at_ms=int(time.time() * 1000)),
        catalog_snapshot_provider=snapshots,
        canonical_artifact_ensurer=canonical_binding,
        verify_offer_signature=offer_verifier,
        ack_timeout_ms=args.ack_timeout_ms,
        # A first tensor may require cold protected assembly, ORT loading,
        # encrypted input fetch and upstream compute. Use the caller's request
        # budget instead of an independent shorter timeout; DATA_V1 still
        # clamps every fetch to its hard deadline and observes cancellation.
        data_v1_no_progress_ms=int(args.timeout_ms),
        ack_coverage_roles=(),
        grant_binding_provider=grant_binding_provider,
        protection_epoch=protection_epoch,
        lifecycle_observer=_make_lifecycle_observer(journal),
    )
    try:
        handle = client.request_task(
            model=model,
            task=task,
            input=app_input,
            timeout_ms=args.timeout_ms,
            options=TaskOptions(adapter.descriptor.options_schema_digest, b"{}"),
            request_id=args.request_id,
        )
    except Exception as exc:
        if _spec180_negative_matches(mutation, exc, journal):
            _emit_spec180_y_n_negative(args, mutation, journal=journal)
            return 91
        raise
    role_map = {
        str(role): str(provider)
        for role, provider in handle.sealed_plan.providers_by_role.items()
    }
    journal.append(
        "PROVIDER_EXECUTION_STARTED",
        roleDigest=_digest_json(role_map),
        providerCount=len(set(role_map.values())),
    )
    # Keep this terminal branch independently executable for the focused
    # production-tail regression, which intentionally starts at `response`.
    if locals().get("mutation") == "Y-N-E":
        try:
            response = handle.response(min(int(args.timeout_ms), 3000))
        except Exception:
            # Exit alone proves nothing: the runner requires the independent
            # selected-Provider verifier record and complete child cleanup.
            return 91
        if response.status:
            raise RuntimeError("GRANT_MUTATION_WAS_ACCEPTED")
        return 91
    if locals().get("mutation") == "Y-N-I":
        try:
            # The bound must stay under the runner's 5 s terminal-cleanup
            # window: the failure Response does not traverse the DATA_V1
            # result channel on this branch, so this wait always times out
            # and the User must reach its own 91 exit first.
            response = handle.response(min(int(args.timeout_ms), 3000))
        except Exception:
            # The native Provider's owner-side rejection is the registered
            # evidence (the runner validates the Provider's SPEC180 negative
            # marker independently); the failure Response does not traverse
            # the DATA_V1 result channel on this branch, so the User's own
            # wait cannot observe it.  Exiting 91 here lets the runner's
            # marker gate decide: no Provider marker means the subcase
            # fails as Y_N_NEGATIVE_MARKER_MISSING.
            return 91
        if response.status:
            raise RuntimeError("DI_INPUT_FETCH_ROLE_MISMATCH_ACCEPTED")
        # Only the native Provider's owner-side rejection may prove this case.
        # A generic failed Response does not identify the input-fetch boundary.
        if getattr(response, "error", "") != "DI_INPUT_FETCH_ROLE_MISMATCH":
            raise RuntimeError("SPEC180_Y_N_I_REQUIRES_PROVIDER_EVIDENCE")
        return 91
    response = handle.response(args.timeout_ms)
    journal.append(
        "TERMINAL_RESPONSE",
        resultDigest="sha256:" + hashlib.sha256(
            bytes(response.payload)).hexdigest(),
        requestCount=1,
        status=bool(response.status),
    )
    journal.validate_complete()
    if not response.status:
        print("YOLO_ACK_DRIVEN_RESULT status=false reason=REMOTE_RESPONSE_FAILED")
        return 3
    if not _record_yolo_numerical_result(
            args, numerical_reference, bytes(response.payload),
            handle.sealed_plan.plan_digest, journal.attempt_id):
        print("YOLO_ACK_DRIVEN_RESULT status=false reason=NUMERICAL_ORACLE_FAILED", flush=True)
        return 4
    print(
        "YOLO_ACK_DRIVEN_RESULT status=true "
        f"payload_bytes={len(response.payload)} "
        f"plan_digest={handle.sealed_plan.plan_digest}",
        flush=True,
    )
    return 0


def _load_yolo_native_payload(client, args) -> int:
    """Submit the maintained YOLO payload through the native requester.

    The native configuration is the only composition authority in this branch:
    catalog, grant/admission, preparation and split/placement owners are
    constructed by ``APPClient``.  The tensor bundle is published once as an
    encrypted repository object and the bound reference is submitted through
    the native requester; the selected Provider owns fetch/decrypt.  The
    historical ACK-driven Python planner remains a separate explicitly
    selected path when this option is absent.
    """
    if not args.native_requester_config:
        raise RuntimeError("native YOLO route requires --native-requester-config")
    if not args.canonical_package or not args.catalogue_registry:
        raise RuntimeError(
            "native YOLO route requires --canonical-package and --catalogue-registry")
    if not args.native_tensor_input:
        raise RuntimeError(
            "native YOLO route requires --native-tensor-input")
    if args.lifecycle_output_dir or args.lifecycle_case or args.request_id:
        raise RuntimeError(
            "native YOLO route does not yet support Spec180 lifecycle journaling")

    package = Path(args.canonical_package).expanduser().resolve()
    registry = Path(args.catalogue_registry).expanduser().resolve()
    adapter = build_yolo26n_adapter(package, registry_path=registry)
    numerical_reference, payload = _prepare_yolo_input(args, package)

    # This call is intentionally before any request construction.  It binds
    # the native catalog/runtime/grant owner and fails closed on configuration
    # drift; no Python planner is consulted by the native branch.
    client.configure_native_requester_from_config(args.native_requester_config)
    from ndnsf import _ndnsf

    native_model = getattr(client, "_native_model", None)
    manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
    source = manifest.get("source")
    expected_digest = (
        "sha256:" + str(source.get("checkpointSha256", ""))
        if isinstance(source, dict) else "")
    if (native_model is None or native_model.model_name != "YOLO26n" or
            native_model.adapter_id != adapter.descriptor.name or
            native_model.content_digest != expected_digest):
        raise RuntimeError("native YOLO requester model identity does not match package")

    options = _ndnsf.NativeRequestOptions()
    options.timeout_ms = int(args.timeout_ms)
    options.ack_timeout_ms = int(args.ack_timeout_ms)
    options.output_mode = "FULL"
    runtime = getattr(client, "_native_runtime", None)
    contract = getattr(runtime, "contract", None)
    task_name = str(getattr(contract, "task_name", ""))
    if not task_name:
        raise RuntimeError("native YOLO runtime has no task identity")

    service = yolo_inference_service(client.deployment)
    reference = client.publish_application_input_reference(
        service,
        payload,
        object_label="inference-input-image",
        object_type="application/x-ndnsf-di-input+native-tensor",
        freshness_ms=120000,
    )
    handle = client.request_native_reference(
        reference,
        options=options,
        task_name=task_name,
        application_options=b"{}",
    )
    try:
        result = handle.result(int(args.timeout_ms))
    except Exception as exc:
        print(
            "YOLO_NATIVE_REQUEST_RESULT status=false "
            f"request={handle.request_id} error={exc}",
            flush=True,
        )
        return 3
    response_payload = bytes(result.payload)
    matched = False
    try:
        _, actual = decode_yolo_output(response_payload, native_predictions_only=True)
        oracle = compare_reference(numerical_reference, actual)
        matched = bool(oracle["matched"])
        max_diff = oracle["maxAbsError"]
        mean_diff = None
    except (ValueError, KeyError, UnicodeError, OverflowError):
        max_diff = float("inf")
        mean_diff = float("inf")
    print(
        "YOLO_NATIVE_REQUEST_RESULT "
        f"status={str(bool(result.payload) and matched).lower()} "
        f"request={handle.request_id} payload_bytes={len(response_payload)} "
        f"max_diff={max_diff} mean_diff={mean_diff}",
        flush=True,
    )
    return 0 if matched else 4


def main() -> int:
    parser = parse_args_with_common("Run YOLO 2x2 user")
    parser.add_argument("--ack-timeout-ms", type=int, default=1500)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument(
        "--request-id", default="",
        help=("optional request identity supplied by the case driver; the "
              "coordinator returns and authenticates this same identity"),
    )
    parser.add_argument(
        "--lifecycle-output-dir", default="",
        help="case evidence directory for the bound lifecycle journal",
    )
    parser.add_argument(
        "--lifecycle-case", default="",
        help="registered Spec180 case identifier for lifecycle evidence",
    )
    parser.add_argument("--permission-wait-ms", type=int, default=2500)
    parser.add_argument("--async-requests", type=int, default=1)
    parser.add_argument("--dynamic-provisioning", action="store_true",
                        help="kept for older commands; service invocation now provisions dynamically by default")
    parser.add_argument("--deployed-models", action="store_true",
                        help="use providers that already have local model shards")
    parser.add_argument(
        "--repo-manifest-file",
        default="",
        help="artifact reference manifest produced by the repo-backed deployer",
    )
    parser.add_argument("--sequential-requests", type=int, default=0)
    parser.add_argument("--sequential-duration-s", type=float, default=0.0,
                        help="Run sequential requests for this many seconds; 0 disables duration mode")
    parser.add_argument("--sequential-interval-ms", type=int, default=0,
                        help="Minimum interval between sequential request starts")
    parser.add_argument("--preflight-requests", type=int, default=0,
                        help="Warm the deployed plan/session before measured requests")
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--input-size", type=int, default=DEFAULT_INPUT_SIZE)
    parser.add_argument("--native-tensor-input", action="store_true",
                        help="publish request input as an NDNSF-DI native tensor bundle")
    parser.add_argument(
        "--native-requester-config", default="",
        help=(
            "operator-pinned ndnsf-di-native-requester-v1 configuration; "
            "when set, submit inline YOLO tensor bytes through the native "
            "requester without Python planner fallback"),
    )
    parser.add_argument(
        "--offline-oracle", action="store_true",
        help="run the historical service-policy path as an offline oracle only",
    )
    parser.add_argument("--canonical-package", default="",
                        help="signed Spec180 YOLO canonical package directory")
    parser.add_argument("--catalogue-registry", default="",
                        help="Spec180 catalogue trust-root registry JSON")
    parser.add_argument("--input-reference-file", default="",
                        help="offline-only legacy reference JSON; rejected by ACK-driven mode")
    parser.add_argument("--input-payload-file", default="",
                        help="encoded YOLO input bytes to publish; omitted uses the deterministic fixture")
    parser.add_argument("--envelope-key-file", required=True,
                        help="owner-only raw 32-byte key protecting persistent request state")
    parser.add_argument(
        "--offer-trust-root",
        default=os.environ.get("SPEC180_YOLO_OFFER_TRUST_ROOT", ""),
        help="candidate-bound Provider-offer Trust Schema policy JSON")
    parser.add_argument(
        "--offer-public-key-map",
        default=os.environ.get("SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP", ""),
        help="JSON map of Provider signer key IDs to PEM public-key paths")
    parser.add_argument(
        "--selection-offer-key-map", default="",
        help="deprecated fixture-only HMAC map; rejected for ACK-driven mode")
    parser.add_argument("--catalog-data-name", default="",
                        help="exact signed APP Data name for active catalogue metadata")
    parser.add_argument("--catalog-signer", default="",
                        help="expected NDN signer identity for catalogue APP Data")
    parser.add_argument("--catalog-timeout-ms", type=int, default=5000,
                        help="signed catalogue APP Data fetch timeout")
    parser.add_argument("--catalog-snapshot-file", default="",
                        help="offline-oracle fixture only; never used by ACK-driven mode")
    args = parser.parse_args()
    if args.native_requester_config and args.offline_oracle:
        raise SystemExit(
            "--native-requester-config and --offline-oracle are mutually exclusive")
    if args.dry_run:
        print("Run YOLO 2x2 user")
        print("config:", args.config)
        return 0

    with optional_local_nfd(args.start_local_nfd):
        trace_init = os.environ.get("NDNSF_DI_INIT_TRACE") == "1"
        client = APPClient.from_config(
            args.config,
            envelope_key_file=args.envelope_key_file,
            generated_policy_dir=args.generated_policy_dir,
            group=args.group,
            permission_wait_ms=args.permission_wait_ms,
            adaptive_admission=False,
            async_workers=max(1, args.async_requests),
        )
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_client", flush=True)
        if args.native_requester_config:
            try:
                return _load_yolo_native_payload(client, args)
            finally:
                client.shutdown()
        if not args.offline_oracle:
            try:
                return _load_yolo_ack_driven(client, args)
            finally:
                client.shutdown()
        service = yolo_inference_service(client.deployment)
        service_policy = client.deployment.service_policy(service)
        metadata = service_policy.metadata or {}
        layout = str(metadata.get("layout", "2x2"))
        layout_semantics = str(metadata.get("layout_semantics", ""))
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_before_make_input", flush=True)
        image = make_input(args.input_size)
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_make_input", flush=True)
        reference_image_payload = client.encode_input(service, image)
        if trace_init:
            print(
                "NDNSF_DI_INIT_TRACE "
                f"stage=user_after_encode_input bytes={len(reference_image_payload)}",
                flush=True,
            )
        image_payload = (
            encode_native_tensor_bundle({"images": image})
            if args.native_tensor_input else
            reference_image_payload
        )
        if trace_init:
            print(
                "NDNSF_DI_INIT_TRACE "
                f"stage=user_before_publish_input bytes={len(image_payload)}",
                flush=True,
            )
        payload = client.publish_large_payload_reference(
            service,
            image_payload,
            object_label="inference-input-image",
            object_type="application/x-ndnsf-di-input+npz",
            freshness_ms=120000,
        )
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_publish_input", flush=True)
        inference_image = decode_image(reference_image_payload)
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_decode_input", flush=True)
        artifact_paths = {
            artifact.role: artifact.path
            for artifact in service_policy.artifacts
            if getattr(artifact, "path", "")
        }
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_before_expected", flush=True)
        if artifact_paths and all(Path(path).exists() for path in artifact_paths.values()):
            if layout_semantics in {
                    YOLO_PARALLEL_DETECT_SCALE_SEMANTICS,
                    YOLO_PARALLEL_DETECT_REPLICATED_BACKBONE_SEMANTICS,
            }:
                expected = run_local_parallel_detect_scale_pipeline(
                    artifact_paths,
                    inference_image,
                    layout,
                )
            elif layout_semantics == YOLO_PARALLEL_OUTPUT_SEMANTICS:
                expected = run_local_parallel_output_pipeline(
                    artifact_paths,
                    inference_image,
                    layout,
                )
            else:
                expected = run_local_onnx_pipeline(
                    artifact_paths,
                    inference_image,
                    service_policy.roles,
                )
        else:
            expected = full_forward(args.model, inference_image)
        if trace_init:
            print("NDNSF_DI_INIT_TRACE stage=user_after_expected", flush=True)
        duration_s = max(0.0, float(args.sequential_duration_s or 0.0))
        interval_s = max(0.0, float(args.sequential_interval_ms or 0) / 1000.0)
        request_count = args.sequential_requests or args.async_requests
        if duration_s > 0:
            request_count = max(1, int(duration_s / max(interval_s, 0.001)))
        dynamic_provisioning = None
        if args.deployed_models:
            dynamic_provisioning = False
        elif args.dynamic_provisioning or args.repo_manifest_file:
            dynamic_provisioning = True
        plan_session = None
        if dynamic_provisioning:
            plan = client.service_plan(
                service,
                runtime=runtime_spec(),
                artifact_references=args.repo_manifest_file or None,
            )
            plan_session = client.deploy_plan(plan, freshness_ms=120000)

        def invoke_once():
            if plan_session is not None:
                return client.invoke_plan(
                    plan_session,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                )
            return client.distributed_inference(
                service,
                payload,
                ack_timeout_ms=args.ack_timeout_ms,
                timeout_ms=args.timeout_ms,
                dynamic_provisioning=False,
                runtime=runtime_spec(),
                artifact_references=args.repo_manifest_file or None,
            )

        preflight_requests = max(0, int(args.preflight_requests or 0))
        for index in range(preflight_requests):
            started = time.perf_counter()
            epoch_started = time.time()
            if plan_session is not None:
                preflight = client.preflight_plan(
                    plan_session,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                )
            else:
                preflight = client.distributed_inference(
                    service,
                    payload,
                    ack_timeout_ms=args.ack_timeout_ms,
                    timeout_ms=args.timeout_ms,
                    dynamic_provisioning=False,
                    runtime=runtime_spec(),
                    artifact_references=args.repo_manifest_file or None,
                )
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            epoch_finished = time.time()
            print(
                "YOLO_LAYOUT_PREFLIGHT "
                f"layout={layout} index={index} "
                f"epoch_start_s={epoch_started:.6f} "
                f"epoch_end_s={epoch_finished:.6f} "
                f"status={str(preflight.status).lower()} "
                f"elapsed_ms={elapsed_ms:.2f} "
                f"error={preflight.error}"
            )
            if not preflight.status:
                client.shutdown()
                return 4

        if args.sequential_requests or duration_s > 0:
            futures = []
            run_deadline = time.perf_counter() + duration_s if duration_s > 0 else None
            index = 0
            while True:
                if run_deadline is not None and time.perf_counter() >= run_deadline:
                    break
                if run_deadline is None and index >= request_count:
                    break
                started = time.perf_counter()
                epoch_started = time.time()
                result = invoke_once()
                futures.append(_TimedFuture(_ImmediateResult(result), started, time.perf_counter(),
                                            epoch_started, time.time()))
                index += 1
                if interval_s > 0:
                    next_start = started + interval_s
                    delay = next_start - time.perf_counter()
                    if delay > 0:
                        if run_deadline is not None:
                            delay = min(delay, max(0.0, run_deadline - time.perf_counter()))
                        time.sleep(delay)
        else:
            futures = []
            for _ in range(request_count):
                started = time.perf_counter()
                epoch_started = time.time()
                if plan_session is not None:
                    future = client.invoke_plan_async(
                        plan_session,
                        payload,
                        ack_timeout_ms=args.ack_timeout_ms,
                        timeout_ms=args.timeout_ms,
                    )
                else:
                    future = client.async_distributed_inference(
                        service,
                        payload,
                        ack_timeout_ms=args.ack_timeout_ms,
                        timeout_ms=args.timeout_ms,
                        dynamic_provisioning=False,
                        runtime=runtime_spec(),
                        artifact_references=args.repo_manifest_file or None,
                    )
                futures.append(_TimedFuture(future, started, None, epoch_started, None))
        ok = True
        for index, timed in enumerate(futures):
            result = timed.future.result(timeout=args.timeout_ms / 1000 + 10)
            finished = timed.finished if timed.finished is not None else time.perf_counter()
            epoch_finished = timed.epoch_finished if timed.epoch_finished is not None else time.time()
            elapsed_ms = (finished - timed.started) * 1000.0
            if not result.status:
                print(
                    f"YOLO_LAYOUT_RESULT layout={layout} index={index} "
                    f"epoch_start_s={timed.epoch_started:.6f} "
                    f"epoch_end_s={epoch_finished:.6f} "
                    f"status=false inference_elapsed_ms={elapsed_ms:.2f} error={result.error}"
                )
                if layout == "2x2":
                    print(
                        f"YOLO_2X2_RESULT index={index} status=false "
                        f"epoch_start_s={timed.epoch_started:.6f} "
                        f"epoch_end_s={epoch_finished:.6f} "
                        f"inference_elapsed_ms={elapsed_ms:.2f} error={result.error}"
                    )
                ok = False
                continue
            _, actual = decode_yolo_output(result.payload)
            atol = 1e-3
            rtol = 1e-4
            item_ok, max_diff, mean_diff = compare_yolo_outputs(
                actual,
                expected,
                atol=atol,
                rtol=rtol,
            )
            ok = ok and item_ok
            print(
                "YOLO_LAYOUT_RESULT "
                f"layout={layout} "
                f"index={index} "
                f"epoch_start_s={timed.epoch_started:.6f} "
                f"epoch_end_s={epoch_finished:.6f} "
                f"status=true shape={actual.shape} "
                f"max_abs_diff={max_diff:.8f} mean_abs_diff={mean_diff:.8f} "
                f"atol={atol:.1e} rtol={rtol:.1e} "
                f"inference_elapsed_ms={elapsed_ms:.2f} "
                f"ok={str(item_ok).lower()}"
            )
            if layout == "2x2":
                print(
                    "YOLO_2X2_RESULT "
                    f"index={index} "
                    f"epoch_start_s={timed.epoch_started:.6f} "
                    f"epoch_end_s={epoch_finished:.6f} "
                    f"status=true shape={actual.shape} "
                    f"max_abs_diff={max_diff:.8f} mean_abs_diff={mean_diff:.8f} "
                    f"atol={atol:.1e} rtol={rtol:.1e} "
                    f"inference_elapsed_ms={elapsed_ms:.2f} "
                    f"ok={str(item_ok).lower()}"
                )
        client.shutdown()
        return 0 if ok else 3


class _ImmediateResult:
    def __init__(self, value):
        self._value = value

    def result(self, timeout=None):
        return self._value


class _TimedFuture:
    def __init__(self, future, started: float, finished: float | None,
                 epoch_started: float, epoch_finished: float | None):
        self.future = future
        self.started = started
        self.finished = finished
        self.epoch_started = epoch_started
        self.epoch_finished = epoch_finished


if __name__ == "__main__":
    raise SystemExit(main())
