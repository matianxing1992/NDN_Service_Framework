from __future__ import annotations

import base64
import hashlib
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "NDNSF-DistributedInference"))

from ndnsf_distributed_inference.adapters import (  # noqa: E402
    ApplicationInput,
    InputTransportMode,
)
from ndnsf_distributed_inference.core.contracts import (  # noqa: E402
    DIRequestEnvelopeV2,
)
from ndnsf_distributed_inference.provider import (  # noqa: E402
    DistributedInferenceProvider,
    ProviderRuntimeContext,
    validate_request_input_boundary,
)
import ndnsf_distributed_inference.provider as provider_module  # noqa: E402
from ndnsf_distributed_inference.repo_reference import (  # noqa: E402
    _publication_manifest_digest,
    bind_published_large_data_reference,
)


def digest(label: str) -> str:
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def input_schema() -> str:
    return digest("input-schema")


def options_schema() -> str:
    return digest("options-schema")


def repo_reference() -> dict:
    return {
        "source": "repo-manifest",
        "dataName": "/repo/input/image-1",
        "objectId": "image-1",
        "manifestDigest": digest("manifest"),
        "plaintextSize": 1024 * 1024,
        "ciphertextDigest": digest("ciphertext"),
        "authorizationScope": "/service/object-detection/input",
        "protectionEpoch": "input-epoch-1",
        "encrypted": True,
    }


def repo_reference_for_payload(payload: bytes) -> dict:
    reference = {
        **repo_reference(),
        "ciphertextDigest": "sha256:" + hashlib.sha256(payload).hexdigest(),
    }
    reference["manifestDigest"] = _publication_manifest_digest(
        data_name=reference["dataName"],
        object_id=reference["objectId"],
        plaintext_size=reference["plaintextSize"],
        content_digest=reference["ciphertextDigest"],
        authorization_scope=reference["authorizationScope"],
        protection_epoch=reference["protectionEpoch"],
    )
    return reference


def test_inline_input_is_bounded_and_mutually_exclusive():
    value = ApplicationInput.from_inline(
        task_name="object-detection",
        input_schema_digest=input_schema(),
        options_schema_digest=options_schema(),
        payload=b"image-bytes",
        options=b"{}",
    )
    assert value.transport_mode is InputTransportMode.INLINE
    assert value.repo_reference is None
    with pytest.raises(ValueError, match="exceeds 4096"):
        ApplicationInput.from_inline(
            task_name="object-detection",
            input_schema_digest=input_schema(),
            options_schema_digest=options_schema(),
            payload=b"x" * 4097,
            options=b"{}",
        )
    with pytest.raises(ValueError, match="cannot carry a repo reference"):
        ApplicationInput(
            task_name="object-detection",
            input_schema_digest=input_schema(),
            options_schema_digest=options_schema(),
            payload=b"x",
            options=b"{}",
            repo_reference=repo_reference(),
        )


def test_repo_reference_is_encrypted_and_has_no_inline_payload():
    value = ApplicationInput.from_repo_ref(
        task_name="object-detection",
        input_schema_digest=input_schema(),
        options_schema_digest=options_schema(),
        reference=repo_reference(),
        options=b"{}",
    )
    assert value.transport_mode is InputTransportMode.REPO_REF
    assert value.payload == b""
    assert value.repo_reference["encrypted"] is True
    with pytest.raises(ValueError, match="must be encrypted"):
        ApplicationInput.from_repo_ref(
            task_name="object-detection",
            input_schema_digest=input_schema(),
            options_schema_digest=options_schema(),
            reference={**repo_reference(), "encrypted": False},
            options=b"{}",
        )
    with pytest.raises(ValueError, match="protection epoch is required"):
        ApplicationInput.from_repo_ref(
            task_name="object-detection",
            input_schema_digest=input_schema(),
            options_schema_digest=options_schema(),
            reference={key: value for key, value in repo_reference().items()
                       if key != "protectionEpoch"},
            options=b"{}",
        )


def test_repo_reference_round_trips_without_plaintext_on_request_wire():
    value = ApplicationInput.from_repo_ref(
        task_name="object-detection",
        input_schema_digest=input_schema(),
        options_schema_digest=options_schema(),
        reference=repo_reference(),
        options=b"{}",
    )
    wire = _request_wire(value)
    envelope = DIRequestEnvelopeV2.from_bytes(wire)
    assert envelope.input_transport == "REPO_REF"
    assert envelope.input_payload_b64 == ""
    assert envelope.input_reference["dataName"] == "/repo/input/image-1"
    assert b"image-bytes" not in wire
    assert b"secret-image-payload" not in wire
    assert validate_request_input_boundary(envelope) == "REPO_REF"


def _request_wire(value: ApplicationInput) -> bytes:
    return DIRequestEnvelopeV2(
        invocation_id="invocation-ownership", request_id="request-ownership", attempt=1,
        service="/ObjectDetection/YOLO26n", model_name="YOLO26n",
        model_identity_hash=digest("model"), task_kind=value.task_name,
        input_manifest_digest=digest("input-manifest"),
        input_payload_b64=base64.b64encode(value.payload).decode("ascii"),
        options_payload_b64=base64.b64encode(value.options).decode("ascii"),
        plan_deadline_ms=9999999999999, security_domain="requester-default",
        input_transport=value.transport_mode.value,
        input_reference=dict(value.repo_reference or {}),
    ).to_bytes()


class _FakeCollaboration:
    session_id = "/request-ownership"

    def __init__(self, payload: bytes = b"repo-image"):
        self.payload = payload
        self.fetches = []
        self.responses = []

    def fetch_large(self, data_name, key_scope, timeout_ms=5000):
        self.fetches.append((data_name, key_scope, timeout_ms))
        return self.payload

    def fetch_large_reference(self, reference, key_scope, timeout_ms=5000):
        assert reference["dataName"] == "/repo/input/image-1"
        assert reference["authorizationScope"] == key_scope
        expected = reference["ciphertextDigest"].split(":", 1)[1]
        if len(self.payload) != int(reference["plaintextSize"]):
            raise ValueError("large reference size mismatch")
        if hashlib.sha256(self.payload).hexdigest() != expected:
            raise ValueError("large reference SHA-256 mismatch")
        return self.fetch_large(reference["dataName"], key_scope, timeout_ms)

    def publish_final_response(self, payload):
        self.responses.append(bytes(payload))


def test_provider_context_fetches_inline_only_for_ingress_owner_and_publishes_terminal_result():
    value = ApplicationInput.from_inline(
        task_name="object-detection", input_schema_digest=input_schema(),
        options_schema_digest=options_schema(), payload=b"image-bytes", options=b"{}")
    fake = _FakeCollaboration()
    context = ProviderRuntimeContext(
        ndnsf=fake, execution=object(), request=_request_wire(value), role="FullModel",
        input_ingress_owner=True, terminal_response_owner=True,
        enforce_dataflow_ownership=True)
    assert context.fetch_application_input() == b"image-bytes"
    context.publish_terminal_result(b"result")
    assert fake.responses == [b"result"]


def test_provider_context_rejects_non_owner_input_and_terminal_publish():
    value = ApplicationInput.from_inline(
        task_name="object-detection", input_schema_digest=input_schema(),
        options_schema_digest=options_schema(), payload=b"image-bytes", options=b"{}")
    fake = _FakeCollaboration()
    context = ProviderRuntimeContext(
        ndnsf=fake, execution=object(), request=_request_wire(value), role="DetectShard0",
        input_ingress_owner=False, terminal_response_owner=False,
        enforce_dataflow_ownership=True)
    with pytest.raises(PermissionError, match="DI_INPUT_FETCH_ROLE_MISMATCH"):
        context.fetch_application_input()
    with pytest.raises(PermissionError, match="DI_TERMINAL_RESPONSE_ROLE_MISMATCH"):
        context.publish_terminal_result(b"result")
    assert fake.fetches == []
    assert fake.responses == []


def test_provider_context_fetches_encrypted_repo_reference_and_checks_plaintext_size():
    payload = b"x" * (1024 * 1024)
    value = ApplicationInput.from_repo_ref(
        task_name="object-detection", input_schema_digest=input_schema(),
        options_schema_digest=options_schema(),
        reference=repo_reference_for_payload(payload),
        options=b"{}")
    fake = _FakeCollaboration(payload=payload)
    context = ProviderRuntimeContext(
        ndnsf=fake, execution=object(), request=_request_wire(value), role="BackboneNeck",
        input_ingress_owner=True, enforce_dataflow_ownership=True)
    assert context.fetch_application_input(timeout_ms=1234) == fake.payload
    assert fake.fetches == [(
        "/repo/input/image-1", "/service/object-detection/input", 1234)]

    fake.payload = b"short"
    with pytest.raises(ValueError, match="DI_INPUT_REPO_SIZE_MISMATCH"):
        context.fetch_application_input()


def test_provider_context_rejects_name_or_size_only_repo_fetch():
    payload = b"x" * (1024 * 1024)
    value = ApplicationInput.from_repo_ref(
        task_name="object-detection", input_schema_digest=input_schema(),
        options_schema_digest=options_schema(),
        reference=repo_reference_for_payload(payload), options=b"{}")

    class NameOnlyCollaboration:
        def fetch_large(self, _data_name, _key_scope, _timeout_ms=5000):
            return payload

    context = ProviderRuntimeContext(
        ndnsf=NameOnlyCollaboration(), execution=object(),
        request=_request_wire(value), role="BackboneNeck",
        input_ingress_owner=True, enforce_dataflow_ownership=True)
    with pytest.raises(RuntimeError, match="DI_INPUT_REPO_DIGEST_UNVERIFIED"):
        context.fetch_application_input()


def test_provider_context_projects_signed_descriptor_to_protected_reader():
    payload = b"image-bytes"
    reference = repo_reference_for_payload(payload)
    reference["authorizationScope"] = "/SERVICE/Inference/ObjectDetection"
    reference["manifestDigest"] = digest("signed-descriptor-payload")
    value = ApplicationInput.from_repo_ref(
        task_name="object-detection", input_schema_digest=input_schema(),
        options_schema_digest=options_schema(), reference=reference, options=b"{}")

    class ProtectedReader:
        def fetch_protected_request_input(self, candidate, timeout_ms):
            assert candidate["manifestDigest"] == reference["manifestDigest"]
            assert candidate["authorizationScope"] == reference["authorizationScope"]
            assert timeout_ms == 1234
            return payload

    context = ProviderRuntimeContext(
        ndnsf=ProtectedReader(), execution=object(), request=_request_wire(value),
        role="BackboneNeck", input_ingress_owner=True, enforce_dataflow_ownership=True)
    assert context.fetch_application_input(timeout_ms=1234) == payload


def test_provider_context_rejects_tampered_repo_content():
    original = b"x" * (1024 * 1024)
    value = ApplicationInput.from_repo_ref(
        task_name="object-detection", input_schema_digest=input_schema(),
        options_schema_digest=options_schema(),
        reference=repo_reference_for_payload(original), options=b"{}")
    fake = _FakeCollaboration(payload=b"y" * len(original))
    context = ProviderRuntimeContext(
        ndnsf=fake, execution=object(), request=_request_wire(value),
        role="BackboneNeck", input_ingress_owner=True,
        enforce_dataflow_ownership=True)
    with pytest.raises(ValueError, match="DI_INPUT_REPO_DIGEST_UNVERIFIED"):
        context.fetch_application_input()


def test_provider_context_rejects_tampered_repo_manifest():
    payload = b"x" * (1024 * 1024)
    reference = repo_reference_for_payload(payload)
    reference["manifestDigest"] = digest("tampered-manifest")
    value = ApplicationInput.from_repo_ref(
        task_name="object-detection", input_schema_digest=input_schema(),
        options_schema_digest=options_schema(), reference=reference, options=b"{}")
    context = ProviderRuntimeContext(
        ndnsf=_FakeCollaboration(payload=payload), execution=object(),
        request=_request_wire(value), role="BackboneNeck",
        input_ingress_owner=True, enforce_dataflow_ownership=True)
    with pytest.raises(ValueError, match="DI_INPUT_REPO_DIGEST_UNVERIFIED"):
        context.fetch_application_input()


def test_legacy_add_role_wrapper_has_no_v3_projection_dependency(monkeypatch):
    """The legacy compatibility wrapper must remain callable after V3 wiring."""

    class FakeServiceProvider:
        def __init__(self):
            self.registration = None

        def add_collaboration_handler(self, service, roles, wrapped, ack):
            self.registration = (service, roles, wrapped, ack)

    class FakePrefetcher:
        def __init__(self, _ctx):
            pass

        def shutdown(self):
            pass

    fake_provider = FakeServiceProvider()
    runtime = DistributedInferenceProvider(fake_provider)
    monkeypatch.setattr(provider_module, "prepare_execution", lambda *a, **k: object())
    monkeypatch.setattr(provider_module, "DependencyPrefetcher", FakePrefetcher)
    runtime._report_preparation = lambda *a, **k: None
    runtime._bind_assignment_metadata = lambda _ctx, execution: execution
    observed = {}
    runtime._run_handler = lambda _handler, context: observed.setdefault("context", context)

    runtime.add_role("/ObjectDetection/YOLO26n", "FullModel", lambda _ctx: None)
    assert fake_provider.registration is not None
    _service, _roles, wrapped, ack = fake_provider.registration
    assert ack(b"request").status is True

    ctx = SimpleNamespace(
        assignment=SimpleNamespace(role="FullModel", assignment_payload=b""),
        is_streamed=False,
        fail=lambda _reason: pytest.fail("legacy role unexpectedly failed"),
    )
    wrapped(ctx, b"request")
    assert observed["context"].role == "FullModel"


def _published_result(payload: bytes):
    from ndn.encoding import Component
    from ndnsf_distributed_inference.naming import (
        canonical_name_uri, make_naming_digest_component, make_semantic_content_prefix)
    object_id = "input"
    content_digest = "sha256:" + hashlib.sha256(payload).hexdigest()
    scope = "/SERVICE/ObjectDetection/YOLO26n"
    epoch = "epoch-1"
    # Mock metadata only; actual Core result parity is exercised by Spec201's
    # C++ oracle fixture, including its real descriptor payload digest.
    manifest_digest = "sha256:" + hashlib.sha256(b"fixture-manifest").hexdigest()
    base = make_semantic_content_prefix("/app", {
        "kind": "input", "label": "image", "input_kind": "application"}, content_digest[7:]) + [
        bytes(Component.from_str("PROTECTED")),
        make_naming_digest_component("1" * 64)]
    data_name = canonical_name_uri(base + [bytes(Component.from_str("DATA"))])
    manifest_name = canonical_name_uri(base + [bytes(Component.from_str("MANIFEST")),
        make_naming_digest_component(manifest_digest[7:])])
    return SimpleNamespace(
        success=True,
        encrypted_data_name=data_name,
        data_prefix=data_name,
        manifest_name=manifest_name,
        object_id=object_id,
        plaintext_size=len(payload),
        content_digest=content_digest,
        manifest_digest=manifest_digest,
        authorization_scope=scope,
        protection_epoch=epoch,
        encrypted=True,
        error="",
    )


def test_native_publication_result_binds_complete_di_reference():
    payload = b"encoded-yolo-input"
    publication = _published_result(payload)
    reference = bind_published_large_data_reference(
        publication,
        service_name="/ObjectDetection/YOLO26n",
        payload=payload,
        object_type="application/x-ndnsf-di-input+npz",
    )
    assert reference.data_name == publication.manifest_name
    assert reference.manifest_digest == publication.manifest_digest
    assert reference.authorization_scope == publication.authorization_scope
    assert reference.protection_epoch == publication.protection_epoch
    assert reference.ciphertext_digest == publication.content_digest


def test_native_publication_result_rejects_missing_trusted_metadata():
    payload = b"encoded-yolo-input"
    publication = _published_result(payload)
    publication.manifest_digest = ""
    with pytest.raises(ValueError, match="manifest_digest"):
        bind_published_large_data_reference(
            publication,
            service_name="/ObjectDetection/YOLO26n",
            payload=payload,
        )
